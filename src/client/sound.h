#pragma once

// 8-channel positional WAV mixer + music player, and the soundclasses TDF map.
// Extracted verbatim from client/main.cpp: kept at global scope and header-only
// (every method stays inline, exactly as in the original) so its unqualified use
// sites there are unchanged. The audio-callback mixer and the procedural-DSP
// track builders are delicate and cannot be verified in a headless build, so this
// was moved verbatim rather than split into a translation unit of its own.

#include <SDL.h>

#include "client/options.h"   // ta::detectOutputChannels / ta::openAudioDevice
#include "hpi/hpi.h"          // ta::hpi::Vfs
#include "tdf/tdf.h"          // ta::tdf::parseText (SoundClasses::load)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Minimal 8-channel WAV mixer over an SDL audio device (the game's WAVs are
// 11025 Hz 8-bit mono). Failing to open audio is non-fatal: play() no-ops.
class SoundBank {
public:
    const ta::hpi::Vfs* vfs_ = nullptr;   // runtime read-path (owned by main)

    SoundBank() = default;
    // Owns an SDL audio device + the buffers its callback reads; never copy it.
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;
    ~SoundBank() {
        // Stop SDL's audio callback thread BEFORE our buffers (music_/voices_) are
        // destroyed. SDL_CloseAudioDevice blocks until the callback returns and
        // won't call it again, so mixThunk can't fire on freed state -- this is the
        // return-to-menu teardown crash (GameView, and thus SoundBank, is freed).
        if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
    }

    void init(const ta::hpi::Vfs& vfs) {
        vfs_ = &vfs;
        // Index the sounds/ namespace by stem (user overrides already win via the
        // VFS). A missing sounds dir must NOT skip audio init (music shares the
        // device); just index whatever's there.
        for (const std::string& path : vfs.list("sounds")) {
            std::filesystem::path fp(path);
            std::string ext = fp.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".wav") continue;
            std::string stem = fp.stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            index_[stem] = path;
        }

        openOutput();
    }

    // Open (or re-open) the output device on the current ta::g_audioDevice. Safe to call
    // again to switch devices live (Options): it closes the old device first, which stops
    // and joins the mix thread, so there's no concurrent access to chan_/spec_ while we
    // reconfigure. The detected channel layout follows the newly-chosen device.
    void openOutput() {
        if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        // The process-wide detected layout (see ta::detectOutputChannels) -- shared
        // with the Options per-speaker sliders so they always match what we mix into,
        // and it reveals surround even when the default sink advertises stereo.
        int chans = ta::detectOutputChannels();
        SDL_AudioSpec want{};
        want.freq = 11025;
        want.format = AUDIO_S16SYS;
        want.channels = Uint8(chans);
        // 256 samples @ 11025 Hz = ~23ms of device buffer. The old 1024 put
        // ~93ms between a command click and its acknowledgment tone -- a very
        // feelable lag on the mouse. The mix callback is light (a handful of
        // 11kHz channels), so short periods are cheap; the OS adds its own
        // safety on top.
        want.samples = 256;
        want.callback = &SoundBank::mixThunk;
        want.userdata = this;
        dev_ = ta::openAudioDevice(0, &want, &spec_, SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
        chan_ = dev_ ? (spec_.channels ? spec_.channels : 2) : 1;
        std::fprintf(stderr, "audio: %d output channels%s%s\n", chan_,
                     chan_ >= 4 ? " (surround: front/rear enabled)" : "",
                     (chan_ == 6 || chan_ == 8) ? ", LFE subwoofer driven" : "");
        if (dev_) SDL_PauseAudioDevice(dev_, 0);
    }

    // Live output-device switch from Options: re-open on the now-current device.
    void reopenDevice() { openOutput(); }

    // (User sound overrides -- e.g. overrides/click.hpi replacing the faction
    // order tones -- now arrive through the VFS's overrides layer, which wins the
    // sounds/ namespace automatically, so no separate override loader is needed.)

    bool has(const std::string& name) const {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        return index_.count(n) != 0;
    }

    void setVerbose(bool v) { verbose_ = v; }

    // The listener (camera) frame in world coords, so positional sounds pan by
    // where the source sits on screen. halfW/halfH are half the visible extent.
    void setListener(float cx, float cz, float halfW, float halfH) {
        listenX_ = cx; listenZ_ = cz;
        listenHW_ = std::max(halfW, 1.0f); listenHH_ = std::max(halfH, 1.0f);
        // Re-pan every still-playing positional sound so it tracks its world source as
        // the camera pans/zooms (matters for anything longer than a blip).
        if (dev_) {
            SDL_LockAudioDevice(dev_);
            for (auto& c : channels_)
                if (c.positional && c.data && c.pos < c.data->size()) repan(c);
            SDL_UnlockAudioDevice(dev_);
        }
    }

    // Non-positional (UI, music-adjacent) — centred across all speakers.
    void play(const std::string& name, float gain = 1.0f, float rate = 1.0f) {
        playAt(name, 0.0f, 0.0f, false, 0, 0, gain, rate);
    }
    // Peak amplitude (0..1) of a loaded sample, for normalizing UI feedback:
    // computed once per sound and cached.
    float peakOf(const std::string& name) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        auto pi = peaks_.find(n);
        if (pi != peaks_.end()) return pi->second;
        auto it = index_.find(n);
        if (it == index_.end()) return 0.0f;
        const auto* samples = load(n, it->second);
        int peak = 0;
        if (samples)
            for (int16_t v : *samples) peak = std::max(peak, std::abs(int(v)));
        float pf = float(peak) / 32767.0f;
        peaks_[n] = pf;
        return pf;
    }

    // Positional: pan by the source's world position relative to the listener.
    // Left/right from x; front(up)/rear(down) from z on surround setups. The sound is
    // tagged positional, so setListener re-pans it every frame as the camera moves --
    // it tracks its world source for its whole duration, not just at trigger time.
    void playWorld(const std::string& name, float x, float z) {
        float pan = std::clamp((x - listenX_) / listenHW_, -1.0f, 1.0f);
        float depth = std::clamp((z - listenZ_) / listenHH_, -1.0f, 1.0f);
        playAt(name, pan, depth, true, x, z);
    }

    // Play the synthesised 10-second disco loop as a positional SFX from (x,z) -- the
    // Shift+D dance-floor track. Generated in code (no shipped asset) at the mixer's
    // 11025 Hz mono and cached under "disco", so it pans/fades like any unit sound.
    void discoAt(float x, float z) {
        if (!cache_.count("disco")) buildDisco();
        playWorld("disco", x, z);
    }

    // Likewise for the Shift+H headbang: a synthesised 10s heavy-metal track (distorted
    // power-chord chugs, double-bass kick, snare + crash), played positionally.
    void metalAt(float x, float z) {
        if (!cache_.count("metal")) buildMetal();
        playWorld("metal", x, z);
    }

    // Move the world source of any currently-playing copies of `name` (e.g. keep the
    // disco pinned to a monarch that walks off). setListener re-pans from the new point.
    void repositionWorld(const std::string& name, float x, float z) {
        if (!dev_) return;
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        auto it = cache_.find(n);
        if (it == cache_.end()) return;
        const std::vector<int16_t>* data = &it->second;
        SDL_LockAudioDevice(dev_);
        for (auto& c : channels_)
            if (c.data == data) { c.wx = x; c.wz = z; repan(c); }
        SDL_UnlockAudioDevice(dev_);
    }

  private:
    void buildDisco() {
        constexpr float PI = 3.14159265358979f;
        const int SR = 11025;
        const float BPM = 120.0f, beat = 60.0f / BPM, eighth = beat * 0.5f;
        const int N = int(SR * 10.0f);
        std::vector<float> buf(size_t(N) + size_t(SR), 0.0f);
        std::mt19937 rng(1234567u);
        auto rnd = [&] { return float(rng()) / float(std::mt19937::max()) * 2.0f - 1.0f; };
        auto place = [&](const std::vector<float>& s, float start) {
            size_t i = size_t(start * SR);
            for (size_t k = 0; k < s.size() && i + k < buf.size(); ++k) buf[i + k] += s[k];
        };
        auto sawv = [](float f, float t) { float p = f * t; return 2.0f * (p - std::floor(0.5f + p)); };
        auto n2f = [](float semi) { return 110.0f * std::pow(2.0f, semi / 12.0f); };
        auto kick = [&] {
            int n = int(SR * 0.20f); std::vector<float> s(size_t(n), 0.0f); double ph = 0;
            for (int i = 0; i < n; ++i) { float t = i / float(SR);
                ph += 2.0 * PI * (110.0f * std::exp(-t * 42.0f) + 46.0f) / SR;
                s[size_t(i)] = float(std::sin(ph)) * std::exp(-t * 16.0f); } return s; };
        auto clap = [&] {
            int n = int(SR * 0.22f); std::vector<float> s(size_t(n), 0.0f); float mx = 1e-6f;
            for (int i = 0; i < n; ++i) { float t = i / float(SR); float amp = 0.6f * std::exp(-t * 22.0f);
                for (float off : {0.0f, 0.010f, 0.020f}) { int so = int(off * SR);
                    if (i >= so) amp += std::exp(-((i - so) / float(SR)) * 90.0f); }
                s[size_t(i)] = rnd() * amp; mx = std::max(mx, std::fabs(s[size_t(i)])); }
            for (auto& v : s) v *= 0.5f / mx;
            return s; };
        auto hat = [&](bool open) {
            int n = int(SR * (open ? 0.28f : 0.045f)); std::vector<float> s(size_t(n), 0.0f), nz(size_t(n), 0.0f);
            for (auto& v : nz) v = rnd();
            for (int i = 0; i < n; ++i) { float t = i / float(SR); float sm = 0; int c = 0;
                for (int k = -3; k <= 2; ++k) { int j = i + k; if (j >= 0 && j < n) { sm += nz[size_t(j)]; ++c; } }
                sm /= std::max(1, c);
                s[size_t(i)] = (open ? 0.22f : 0.28f) * (nz[size_t(i)] - sm) * std::exp(-t * (open ? 11.0f : 70.0f)); }
            return s; };
        auto bass = [&](float f, float L) {
            int n = int(SR * L); std::vector<float> s(size_t(n), 0.0f); float acc = 0;
            for (int i = 0; i < n; ++i) { float t = i / float(SR); float x = sawv(f, t) + 0.5f * sawv(f * 0.5f, t);
                acc += (0.06f + 0.25f * std::exp(-t * 12.0f)) * (x - acc);
                s[size_t(i)] = 0.55f * acc * std::min(1.0f, t * 200.0f) * std::exp(-t * 3.0f); } return s; };
        auto stab = [&](const std::vector<float>& fr, float L) {
            int n = int(SR * L); std::vector<float> s(size_t(n), 0.0f), o(size_t(n), 0.0f);
            for (int i = 0; i < n; ++i) { float t = i / float(SR); float v = 0;
                for (float f : fr) v += sawv(f - 0.6f, t) + sawv(f + 0.6f, t);
                s[size_t(i)] = v / float(fr.size() * 2); }
            for (int i = 0; i < n; ++i) { float t = i / float(SR); float sm = 0; int c = 0;
                for (int k = -1; k <= 1; ++k) { int j = i + k; if (j >= 0 && j < n) { sm += s[size_t(j)]; ++c; } }
                sm /= std::max(1, c);
                o[size_t(i)] = (0.7f * s[size_t(i)] + 0.3f * (s[size_t(i)] - sm)) * 0.5f * std::exp(-t * 9.0f); }
            return o; };
        auto lead = [&](float f, float L) {
            int n = int(SR * L); std::vector<float> s(size_t(n), 0.0f); double ph = 0;
            for (int i = 0; i < n; ++i) { float t = i / float(SR);
                ph += 2.0 * PI * f * (1.0f + 0.006f * std::sin(2 * PI * 6 * t)) / SR;
                s[size_t(i)] = 0.28f * (float(std::sin(ph)) + 0.3f * float(std::sin(2 * ph))) *
                               std::min(1.0f, t * 40.0f) * std::min(1.0f, (L - t) * 30.0f); } return s; };

        struct Bar { const char* name; float root; std::vector<float> tones, melo; };
        auto n = [&](float s) { return n2f(s + 12); };
        std::vector<Bar> prog = {
            {"Am", 0, {n(0), n(3), n(7)},   {n(0), n(3), n(7)}},
            {"F", -4, {n(-4), n(0), n(3)},  {n(-4), n(0), n(3)}},
            {"C",  3, {n(3), n(7), n(10)},  {n(3), n(7), n(10)}},
            {"G", -2, {n(-2), n(2), n(5)},  {n(-2), n(2), n(5)}},
            {"Am", 0, {n(0), n(3), n(7)},   {n(0), n(3), n(7)}},
        };
        for (size_t b = 0; b < prog.size(); ++b) {
            float b0 = float(b) * 4 * beat;
            for (int k = 0; k < 4; ++k) place(kick(), b0 + k * beat);
            place(clap(), b0 + beat); place(clap(), b0 + 3 * beat);
            for (int k = 0; k < 8; ++k) place(hat(k == 7), b0 + k * eighth);
            float r = n2f(prog[b].root);
            for (int k = 0; k < 8; ++k) place(bass((k % 2) ? r * 2 : r, eighth * 0.95f), b0 + k * eighth);
            for (int k : {1, 3, 5, 7}) place(stab(prog[b].tones, eighth * 1.2f), b0 + k * eighth);
            for (size_t j = 0; j < prog[b].melo.size(); ++j)
                place(lead(prog[b].melo[j], beat * 0.9f), b0 + (2 + float(j) * 0.66f) * beat);
        }
        float mx = 1e-6f;
        for (int i = 0; i < N; ++i) mx = std::max(mx, std::fabs(buf[size_t(i)]));
        float g = 1.4f / mx;
        std::vector<int16_t> pcm(size_t(N), 0);
        for (int i = 0; i < N; ++i)
            pcm[size_t(i)] = int16_t(std::clamp(std::tanh(buf[size_t(i)] * g) * 0.9f, -1.0f, 1.0f) * 32767);
        cache_["disco"] = std::move(pcm);
        index_["disco"] = "disco";
    }

    void buildMetal() {
        constexpr float PI = 3.14159265358979f;
        const int SR = 11025;
        const float BPM = 152.0f, beat = 60.0f / BPM, six = beat / 4;
        const int N = int(SR * 10.0f);
        std::vector<float> buf(size_t(N) + size_t(SR), 0.0f);
        std::mt19937 rng(99887766u);
        auto rnd = [&] { return float(rng()) / float(std::mt19937::max()) * 2.0f - 1.0f; };
        auto place = [&](const std::vector<float>& s, float start) {
            size_t i = size_t(start * SR);
            for (size_t k = 0; k < s.size() && i + k < buf.size(); ++k) buf[i + k] += s[k];
        };
        auto sawv = [](float f, float t) { float p = f * t; return 2.0f * (p - std::floor(0.5f + p)); };
        auto n2f = [](float semi) { return 110.0f * std::pow(2.0f, semi / 12.0f); };
        // Distorted power chord (root + fifth + octave), palm-muted or ringing.
        auto chug = [&](float root, float L, bool mute) {
            int n = int(SR * L); std::vector<float> s(size_t(n), 0.0f);
            float f5 = root * std::pow(2.0f, 7.0f / 12.0f), f8 = root * 2.0f, lp = 0.0f;
            for (int i = 0; i < n; ++i) { float t = i / float(SR);
                // Detuned double-tracked rhythm guitars = a fatter wall of chug.
                float x = sawv(root, t) + sawv(root + 0.35f, t)
                        + sawv(f5, t) + sawv(f5 + 0.35f, t) + 0.7f * sawv(f8, t);
                x = std::tanh(x * 5.5f);                              // heavy distortion
                lp += 0.5f * (x - lp);                                // tame the fizz
                float amp = mute ? std::exp(-t * 26.0f)
                                 : std::min(1.0f, t * 400.0f) * std::exp(-t * 2.5f);
                s[size_t(i)] = 0.42f * lp * amp; }
            return s; };
        auto kick = [&] {
            int n = int(SR * 0.12f); std::vector<float> s(size_t(n), 0.0f); double ph = 0;
            for (int i = 0; i < n; ++i) { float t = i / float(SR);
                ph += 2.0 * PI * (150.0f * std::exp(-t * 55.0f) + 50.0f) / SR;
                float click = t < 0.004f ? (1.0f - t / 0.004f) : 0.0f;
                s[size_t(i)] = float(std::sin(ph)) * std::exp(-t * 22.0f) + 0.5f * click; }
            return s; };
        auto snare = [&] {
            int n = int(SR * 0.18f); std::vector<float> s(size_t(n), 0.0f); double ph = 0;
            for (int i = 0; i < n; ++i) { float t = i / float(SR);
                ph += 2.0 * PI * 180.0f / SR;
                s[size_t(i)] = (0.7f * rnd() + 0.4f * float(std::sin(ph))) * std::exp(-t * 26.0f); }
            return s; };
        auto crash = [&] {
            int n = int(SR * 0.7f); std::vector<float> s(size_t(n), 0.0f);
            for (int i = 0; i < n; ++i) { float t = i / float(SR);
                s[size_t(i)] = 0.3f * (rnd() - 0.5f * rnd()) * std::exp(-t * 4.0f); }
            return s; };
        // Distorted twin-lead guitar (detuned, with vibrato) -- the soaring melody line.
        auto lead = [&](float freq, float L) {
            int n = int(SR * L); std::vector<float> s(size_t(n), 0.0f);
            for (int i = 0; i < n; ++i) { float t = i / float(SR);
                float vib = 1.0f + 0.012f * std::sin(2 * PI * 5.5f * t);
                float x = sawv(freq * vib, t) + sawv(freq * vib * 1.006f, t);
                x = std::tanh(x * 4.0f);
                float amp = std::min(1.0f, t * 90.0f) * std::min(1.0f, (L - t) * 45.0f);
                s[size_t(i)] = 0.20f * x * amp; }
            return s; };
        // E natural minor scale (E F# G A B C D), semitones from A2 (E2 = -5); deg 0 = E.
        auto scale = [&](int deg) {
            static const int st[7] = {-5, -3, -2, 0, 2, 3, 5};
            int o = 0; while (deg < 0) { deg += 7; --o; } while (deg >= 7) { deg -= 7; ++o; }
            return n2f(float(st[deg]) + 12.0f * o);
        };
        auto twin = [&](int deg, float start, float L) {   // melody + a diatonic 3rd above
            place(lead(scale(deg), L), start);
            place(lead(scale(deg + 2), L), start);
        };
        // Riff: root per bar (E-heavy with movement), galloping chugs + gallop kick.
        float roots[6] = {n2f(-5), n2f(-5), n2f(-2), n2f(0), n2f(-5), n2f(3)};   // E E G A E C
        // Twin-lead melody (upper octave, deg 7 = E4), two half-notes per bar.
        int melody[6][2] = {{7, 9}, {11, 10}, {13, 11}, {14, 12}, {11, 9}, {7, 9}};
        for (int b = 0; b < 6; ++b) {
            float b0 = float(b) * 4 * beat;
            place(crash(), b0);
            for (int bt = 0; bt < 4; ++bt) {
                float t0 = b0 + bt * beat;
                for (float off : {0.0f, 2 * six, 3 * six}) {   // dum da-da gallop
                    place(chug(roots[b], (off == 0.0f ? beat * 0.5f : six * 0.9f), true), t0 + off);
                    place(kick(), t0 + off);
                }
            }
            place(snare(), b0 + beat);
            place(snare(), b0 + 3 * beat);
            twin(melody[b][0], b0, beat * 2 * 0.92f);              // soaring lead over the riff
            twin(melody[b][1], b0 + 2 * beat, beat * 2 * 0.92f);
        }
        float mx = 1e-6f;
        for (int i = 0; i < N; ++i) mx = std::max(mx, std::fabs(buf[size_t(i)]));
        float g = 1.5f / mx;
        std::vector<int16_t> pcm(size_t(N), 0);
        for (int i = 0; i < N; ++i)
            pcm[size_t(i)] = int16_t(std::clamp(std::tanh(buf[size_t(i)] * g) * 0.92f, -1.0f, 1.0f) * 32767);
        cache_["metal"] = std::move(pcm);
        index_["metal"] = "metal";
    }

  public:

    void playAt(const std::string& name, float pan, float depth,
                bool positional = false, float wx = 0, float wz = 0,
                float gain = 1.0f, float rate = 1.0f) {
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        auto it = index_.find(n);
        if (it == index_.end()) return;
        if (verbose_) std::printf("SND %s\n", n.c_str());
        const auto* samples = load(n, it->second);
        if (!samples || !dev_) return;
        SDL_LockAudioDevice(dev_);
        for (auto& c : channels_)
            if (c.pos >= (c.data ? c.data->size() : 0)) {
                c.data = samples;
                c.pos = 0;
                c.pan = pan;
                c.depth = depth;
                c.positional = positional;
                c.wx = wx;
                c.wz = wz;
                c.gain = gain;
                c.fpos = 0;
                c.step = uint32_t(std::clamp(rate, 0.25f, 4.0f) * 65536.0f);
                break;
            }
        SDL_UnlockAudioDevice(dev_);
    }

private:
    struct Channel {
        const std::vector<int16_t>* data = nullptr;
        size_t pos = 0;
        float pan = 0, depth = 0;   // -1..+1 : left..right, front..rear
        float wx = 0, wz = 0;       // world emission point (for positional re-panning)
        bool positional = false;    // true = re-pan every frame from (wx,wz)
        float gain = 1.0f;          // per-sound boost (UI clicks undo the /2 headroom)
        uint32_t step = 65536;      // 16.16 playback rate (pitch); 65536 = native
        uint64_t fpos = 0;          // 16.16 fractional read position
    };
    // Recompute a channel's pan/depth from its world point and the current listener.
    void repan(Channel& c) {
        c.pan = std::clamp((c.wx - listenX_) / listenHW_, -1.0f, 1.0f);
        c.depth = std::clamp((c.wz - listenZ_) / listenHH_, -1.0f, 1.0f);
    }

    // Per-output-channel gains for a source at (pan, depth). Equal-power pan
    // left/right; on surround layouts also crossfade front/rear by depth.
    void channelGains(float pan, float depth, float* g) const {
        float lg = std::sqrt(std::clamp((1 - pan) * 0.5f, 0.0f, 1.0f));
        float rg = std::sqrt(std::clamp((1 + pan) * 0.5f, 0.0f, 1.0f));
        float fg = std::sqrt(std::clamp((1 - depth) * 0.5f, 0.0f, 1.0f));
        float bg = std::sqrt(std::clamp((1 + depth) * 0.5f, 0.0f, 1.0f));
        for (int i = 0; i < chan_; ++i) g[i] = 0;
        float cg = (1.0f - std::fabs(pan)) * 0.7f;   // centre channel content
        switch (chan_) {
            case 1: g[0] = 1.0f; break;
            case 2: g[0] = lg; g[1] = rg; break;                       // FL FR
            case 4: g[0]=lg*fg; g[1]=rg*fg; g[2]=lg*bg; g[3]=rg*bg; break;  // FL FR BL BR
            case 6:  // FL FR FC LFE BL BR
                g[0]=lg*fg; g[1]=rg*fg; g[2]=cg*fg; g[3]=0; g[4]=lg*bg; g[5]=rg*bg; break;
            case 8:  // FL FR FC LFE BL BR SL SR
                g[0]=lg*fg; g[1]=rg*fg; g[2]=cg*fg; g[3]=0;
                g[4]=lg*bg; g[5]=rg*bg; g[6]=lg*0.7f; g[7]=rg*0.7f; break;
            default: g[0]=lg; if (chan_>1) g[1]=rg; break;
        }
    }
    float listenX_ = 0, listenZ_ = 0, listenHW_ = 1, listenHH_ = 1;

    // Decode a WAV (from memory) to the mixer's 11025 Hz mono S16 format.
    std::optional<std::vector<int16_t>> decodeWav(const uint8_t* data, size_t size) {
        SDL_AudioSpec spec{};
        Uint8* buf = nullptr;
        Uint32 len = 0;
        SDL_RWops* rw = SDL_RWFromConstMem(data, int(size));
        if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) return std::nullopt;
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS,
                              1, 11025) < 0) {
            SDL_FreeWAV(buf);
            return std::nullopt;
        }
        std::vector<uint8_t> work(size_t(len) * size_t(std::max(cvt.len_mult, 1)));
        std::memcpy(work.data(), buf, len);
        SDL_FreeWAV(buf);
        cvt.buf = work.data();
        cvt.len = int(len);
        if (cvt.needed && SDL_ConvertAudio(&cvt) != 0) return std::nullopt;
        size_t outBytes = cvt.needed ? size_t(cvt.len_cvt) : len;
        std::vector<int16_t> out(outBytes / 2);
        std::memcpy(out.data(), work.data(), out.size() * 2);
        return out;
    }

    const std::vector<int16_t>* load(const std::string& key, const std::string& path) {
        auto it = cache_.find(key);
        if (it != cache_.end()) return &it->second;
        if (!vfs_) return nullptr;
        std::vector<uint8_t> bytes;
        try { bytes = vfs_->read(path); } catch (const std::exception&) { return nullptr; }
        auto pcm = decodeWav(bytes.data(), bytes.size());
        if (!pcm) return nullptr;
        return &cache_.emplace(key, std::move(*pcm)).first->second;
    }

    static void mixThunk(void* ud, Uint8* stream, int len) {
        static_cast<SoundBank*>(ud)->mix(reinterpret_cast<int16_t*>(stream), len / 2);
    }

    void mix(int16_t* out, int n) {
        std::memset(out, 0, size_t(n) * 2);
        int ch = std::max(chan_, 1);
        int frames = n / ch;
        int lfe = (ch == 6 || ch == 8) ? 3 : -1;   // 5.1/7.1 LFE (subwoofer) channel
        if (lfe >= 0) lfeMono_.assign(size_t(frames), 0);   // full-range mono for the sub
        auto add = [&](int f, int ci, int v) {
            int idx = f * ch + ci;
            out[idx] = int16_t(std::clamp(out[idx] + v, -32768, 32767));
        };
        // Background music bed (quieter than SFX), spread across the main speakers --
        // NOT the LFE, which gets the low-passed sub feed instead.
        for (int f = 0; f < frames; ++f) {
            if (musicPos_ >= music_.size()) { musicDone_ = true; break; }
            int m = (music_[musicPos_++] * musicVol_) / 256;
            int mc = m / (ch > 2 ? 2 : 1);   // don't get louder with more speakers
            for (int ci = 0; ci < ch; ++ci)
                if (ci != lfe) add(f, ci, mc);
            if (lfe >= 0) lfeMono_[size_t(f)] += m;
        }
        // Positional SFX: pan each into the speaker layout.
        float g[8];
        for (auto& c : channels_) {
            if (!c.data) continue;
            channelGains(c.pan, c.depth, g);
            for (int f = 0; f < frames; ++f) {
                c.pos = size_t(c.fpos >> 16);
                if (c.pos >= c.data->size()) break;
                int s = int(float((*c.data)[c.pos]) * c.gain) / 2 * sfxVol_ / 256;
                c.fpos += c.step;
                for (int ci = 0; ci < ch; ++ci)
                    if (g[ci] != 0.0f) add(f, ci, int(s * g[ci]));
                if (lfe >= 0) lfeMono_[size_t(f)] += s;
            }
        }
        // Subwoofer: one-pole low-pass of the mono mix (~110 Hz cutoff at 11025 Hz),
        // fed into the LFE channel so a 5.1/7.1 rig drives the sub directly.
        if (lfe >= 0) {
            constexpr float kAlpha = 0.06f;   // 1/(1 + fs/(2*pi*fc)), fc ~= 110 Hz
            constexpr float kGain = 0.7f;     // sub level (headroom for stacked bass)
            for (int f = 0; f < frames; ++f) {
                lpfState_ += kAlpha * (float(lfeMono_[size_t(f)]) - lpfState_);
                add(f, lfe, int(lpfState_ * kGain));
            }
        }
        // Final trim: master volume, then each output channel's own gain (LFE
        // included) -- applied after everything is summed so one control scales the
        // whole mix. Both default to unity (masterVol_=256, chanGain_=1), so this is
        // an exact no-op at default settings.
        for (int f = 0; f < frames; ++f)
            for (int ci = 0; ci < ch; ++ci) {
                int idx = f * ch + ci;
                int v = int(out[idx] * masterVol_ / 256 * chanGain_[ci]);
                out[idx] = int16_t(std::clamp(v, -32768, 32767));
            }
    }

public:
    // Begin playing a shuffled playlist of the given track numbers (a
    // faction's tracks, per sidedata.tdf). Empty = all 20. `dataRoot` is
    // the extracted data dir; music may live there or in the game install.
    void startMusic(const ta::hpi::Vfs& vfs, const std::vector<int>& tracks) {
        vfs_ = &vfs;
        std::vector<int> want = tracks;
        if (want.empty())
            for (int i = 1; i <= 20; ++i) want.push_back(i);
        for (int n : want) {
            std::string path = "music/track" + std::to_string(n) + ".wav";
            if (vfs.has(path)) playlist_.push_back(path);
        }
        std::fprintf(stderr, "music: %zu faction tracks, audio=%s\n", playlist_.size(),
                     dev_ ? "yes" : "NO DEVICE");
        if (!playlist_.empty() && dev_) {
            std::shuffle(playlist_.begin(), playlist_.end(),
                         std::mt19937{std::random_device{}()});
            loadTrack(0);
        }
    }

    // Advance to the next track when the current one finishes (call per frame).
    void pollMusic() {
        if (musicDone_ && !playlist_.empty()) {
            musicDone_ = false;
            loadTrack((musicTrack_ + 1) % playlist_.size());
        }
    }

    void setMusicVolume(int v) { musicVol_ = std::clamp(v, 0, 256); }
    void setMasterVolume(int v) { masterVol_ = std::clamp(v, 0, 256); }
    void setSfxVolume(int v) { sfxVol_ = std::clamp(v, 0, 256); }
    void setChannelGain(int i, float g) { if (i >= 0 && i < 8) chanGain_[i] = std::clamp(g, 0.0f, 1.0f); }

    // For the Options screen: how many output channels the device gave us, and a
    // human label for each (matching channelGains()'s per-count speaker layout).
    int channelCount() const { return std::clamp(chan_, 1, 8); }
    const char* channelRole(int i) const {
        switch (chan_) {
            case 2: { static const char* r[] = {"LEFT", "RIGHT"}; return i < 2 ? r[i] : ""; }
            case 4: { static const char* r[] = {"FRONT L", "FRONT R", "REAR L", "REAR R"}; return i < 4 ? r[i] : ""; }
            case 6: { static const char* r[] = {"FRONT L", "FRONT R", "CENTER", "SUB", "REAR L", "REAR R"}; return i < 6 ? r[i] : ""; }
            case 8: { static const char* r[] = {"FRONT L", "FRONT R", "CENTER", "SUB", "REAR L", "REAR R", "SIDE L", "SIDE R"}; return i < 8 ? r[i] : ""; }
            default: return "MONO";
        }
    }

private:
    void loadTrack(size_t idx) {
        SDL_AudioSpec spec{};
        Uint8* buf = nullptr;
        Uint32 len = 0;
        std::vector<uint8_t> raw;
        if (vfs_) { try { raw = vfs_->read(playlist_[idx]); } catch (const std::exception&) {} }
        SDL_RWops* rw = raw.empty() ? nullptr : SDL_RWFromConstMem(raw.data(), int(raw.size()));
        if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) {
            std::fprintf(stderr, "music: LoadWAV failed: %s\n", SDL_GetError());
            return;
        }
        SDL_AudioCVT cvt;
        if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS,
                              1, 11025) < 0) {
            std::fprintf(stderr, "music: BuildAudioCVT failed: %s\n", SDL_GetError());
            SDL_FreeWAV(buf);
            return;
        }
        std::vector<uint8_t> work(size_t(len) * size_t(std::max(cvt.len_mult, 1)));
        std::memcpy(work.data(), buf, len);
        SDL_FreeWAV(buf);
        cvt.buf = work.data();
        cvt.len = int(len);
        if (cvt.needed && SDL_ConvertAudio(&cvt) != 0) {
            std::fprintf(stderr, "music: ConvertAudio failed: %s\n", SDL_GetError());
            return;
        }
        size_t outBytes = cvt.needed ? size_t(cvt.len_cvt) : len;
        std::vector<int16_t> pcm(outBytes / 2);
        std::memcpy(pcm.data(), work.data(), pcm.size() * 2);
        SDL_LockAudioDevice(dev_);
        music_ = std::move(pcm);
        musicPos_ = 0;
        musicTrack_ = idx;
        musicDone_ = false;
        SDL_UnlockAudioDevice(dev_);
        std::fprintf(stderr, "music: now playing %s\n", playlist_[idx].c_str());
    }

    std::map<std::string, std::string> index_;
    std::map<std::string, std::vector<int16_t>> cache_;
    std::map<std::string, float> peaks_;   // per-sound peak (UI normalization)
    Channel channels_[8];
    std::vector<std::string> playlist_;
    std::vector<int16_t> music_;
    size_t musicPos_ = 0, musicTrack_ = 0;
    int musicVol_ = 128;  // out of 256 (BGM); 50% default
    int masterVol_ = 256; // out of 256, global gain over the whole mix
    int sfxVol_ = 256;    // out of 256, sound effects
    float chanGain_[8] = {1, 1, 1, 1, 1, 1, 1, 1};   // per-output-channel trim 0..1
    bool musicDone_ = false;
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec spec_{};
    int chan_ = 1;              // output channel count (2=stereo, 4/6/8=surround)
    bool verbose_ = false;
    // Subwoofer (LFE) feed: a one-pole low-pass of the full mono mix, driven into the
    // 5.1/7.1 LFE channel. lpfState_ persists across callbacks (the filter's memory).
    float lpfState_ = 0;
    std::vector<int> lfeMono_;   // per-callback mono accumulator (reused, no hot alloc)
};

// Sound classes: gamedata/soundclasses/*.tdf map a class to event ->
// candidate WAV names.
class SoundClasses {
public:
    void load(const ta::hpi::Vfs& vfs) {
        try {
            for (const std::string& path : vfs.list("gamedata/soundclasses")) {
                if (std::filesystem::path(path).extension() != ".tdf") continue;
                try {
                    auto sb = vfs.read(path);
                    auto root = ta::tdf::parseText(std::string(sb.begin(), sb.end()), path);
                    for (const auto& clsName : root.childOrder) {
                        auto& cls = classes_[clsName];
                        const auto& node = root.children.at(clsName);
                        for (const auto& evName : node.childOrder) {
                            auto& list = cls[evName];
                            // Entries are WEIGHTED: the "_NN-note" click tone
                            // carries 100.0 vs 1.0 per voice line, so retail
                            // mostly bongs and only occasionally speaks.
                            for (const auto& [wav, weight] : node.children.at(evName).values) {
                                float w = float(std::atof(weight.c_str()));
                                list.push_back({wav, w > 0 ? w : 1.0f});
                            }
                        }
                    }
                } catch (const std::exception&) {}
            }
        } catch (const std::exception&) {}
    }

    const std::string* pick(const std::string& cls, const std::string& event,
                            uint32_t salt) const {
        auto ci = classes_.find(cls);
        if (ci == classes_.end()) return nullptr;
        auto ei = ci->second.find(event);
        if (ei == ci->second.end() || ei->second.empty()) return nullptr;
        // Weighted pick (retail: note tone at 100.0 dwarfs 1.0 voice lines).
        // The salt is a counter, so whiten it into a uniform draw first.
        float total = 0;
        for (const auto& [w, wt] : ei->second) total += wt;
        uint32_t hsh = salt * 2654435761u;
        float r = float(hsh & 0xFFFFF) / 1048576.0f * total;
        for (const auto& [w, wt] : ei->second) {
            r -= wt;
            if (r <= 0) return &w;
        }
        return &ei->second.back().first;
    }

private:
    std::map<std::string,
             std::map<std::string, std::vector<std::pair<std::string, float>>>>
        classes_;
};

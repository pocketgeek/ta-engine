#pragma once
#include "util/virtualpath.h"
#include "client/soundlevel.h"

// Positional WAV mixer (retail's 32 voices by default, SOUND VOICES in Options) + music
// player, and the soundclasses TDF map.
// Script sound classes control voice admission; output-channel panning remains
// independent of the number of simultaneous voices. PCM mixing is also exercised
// with a paused dummy device by death_sound_test.

#include <SDL.h>

#include "util/audiolayout.h"
#include "client/retailsound.h"
#include "client/options.h"   // tak::detectOutputChannels / tak::openAudioDevice
#include "hpi/hpi.h"          // tak::hpi::Vfs
#include "client/musicfile.h" // tak::music::loadTrack (CUSTOM MUSIC, MP3/FLAC/Ogg)
#include "tdf/tdf.h"          // tak::tdf::parseText (SoundClasses::load)

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

// Minimal WAV mixer over an SDL audio device (32 voices as retail, up to kMaxVoices). It mixes at 44100 Hz (retail's
// Miles ran at 22050/16/stereo) so a mod may ship sounds and music up to 44.1 kHz and
// keep that quality. Every WAV is cached at its own rate and channel count (S16; only
// a rate above 44100 is converted down): the 11025 Hz 8-bit mono unit sounds, the
// 22050 Hz stereo music tracks and a mod's 44.1 kHz or 5.1 files alike. Each voice
// steps through its source at srcRate/kMixRate through a windowed-sinc interpolator
// (tak::sinc below), and its channels meet the output layout at mix time, so a device
// switch needs no reload. Failing to open audio is non-fatal: play() no-ops.

// Polyphase Kaiser-windowed sinc for the mixer's rate stepping. A source played slower
// than the mix rate (11025/22050 Hz at 44100) is low-passed at its own Nyquist, so the
// spectral images linear interpolation left above it (-26 dB on a unit sound) are gone;
// a source stepped faster than 1 frame per output frame (pitch up) stretches the kernel
// by the step, moving the cutoff to the output's Nyquist so it does not alias.
//   32 taps, Kaiser beta 8, -6 dB at 0.42 x source rate (-1 dB at 0.386, -3 dB at
//   0.405; >= 81 dB down from 0.5 = the source Nyquist up), 256 phases + linear blend.
// 16 taps cannot both pass 4 kHz of an 11025 Hz sound and stop 5.5 kHz; 32 voices of
// 32 taps cost well under a millisecond per callback. Built once at static init.
namespace tak::sinc {
inline constexpr int kTaps = 32;         // source frames under the kernel at step <= 1
inline constexpr int kPhaseBits = 8;     // 256 phases per source frame
inline constexpr int kPhases = 1 << kPhaseBits;
inline constexpr int kMaxStep = 4;       // playAt rate <= 4 and source rate <= mix rate
inline constexpr int kMaxTaps = kTaps * kMaxStep + 2;
inline constexpr double kBeta = 8.0, kCutoff = 0.42;   // cutoff as a fraction of source rate
// row[j][m] = h(m - (kTaps/2 - 1) - j/kPhases), each row normalised to unit DC gain:
// the weights of the frames n-15..n+16 for a read position n + j/kPhases.
struct Table { std::array<std::array<float, kTaps>, kPhases + 1> row{}; };
inline Table build() {
    constexpr double pi = 3.14159265358979323846;
    const auto i0 = [](double x) {   // modified Bessel I0 (series)
        double sum = 1, term = 1;
        for (int k = 1; k < 64; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
        return sum;
    };
    Table t;
    for (int j = 0; j <= kPhases; ++j) {
        double h[kTaps], sum = 0;
        for (int m = 0; m < kTaps; ++m) {
            const double x = double(m - (kTaps / 2 - 1)) - double(j) / kPhases, r = 2 * x / kTaps;
            const double window = std::fabs(r) >= 1 ? 0 : i0(kBeta * std::sqrt(1 - r * r)) / i0(kBeta);
            const double a = 2 * kCutoff * x;
            h[m] = 2 * kCutoff * (a == 0 ? 1 : std::sin(pi * a) / (pi * a)) * window;
            sum += h[m];
        }
        for (int m = 0; m < kTaps; ++m) t.row[size_t(j)][size_t(m)] = float(h[m] / sum);
    }
    return t;
}
inline const Table kTable = build();
// The kernel at an arbitrary offset u in source frames (|u| < kTaps/2), from the table.
inline float kernel(double u) {
    const double v = u + kTaps / 2;
    if (v <= 0 || v >= kTaps) return 0;
    const int m = std::min(int(v), kTaps - 1);
    const double pos = (1 - (v - m)) * kPhases;
    const int j = std::min(int(pos), kPhases - 1);
    const float a = float(pos - j), lo = kTable.row[size_t(j)][size_t(m)];
    return lo + a * (kTable.row[size_t(j) + 1][size_t(m)] - lo);
}
}  // namespace tak::sinc

class SoundBank {
    friend struct SoundBankTestAccess;
public:
    static constexpr int kMixRate = tak::audio::kMixRate;   // device rate
    static constexpr int kSfxRate = 11025;   // the shipped unit WAVs (and the synth tracks)
    // Cached PCM: interleaved S16 at its own rate, 1..8 channels in SDL's order.
    struct Pcm {
        std::vector<int16_t> samples;
        int rate = kMixRate;
        int channels = 1;
        float evenGain = 1.0f;   // EVEN OUT SOUND LEVELS' boost (client/soundlevel.h), set at load
        size_t frames() const { return samples.size() / size_t(std::max(channels, 1)); }
    };
    const tak::hpi::Vfs* vfs_ = nullptr;   // runtime read-path (owned by main)

    SoundBank() = default;
    // Owns an SDL audio device + the buffers its callback reads; never copy it.
    SoundBank(const SoundBank&) = delete;
    SoundBank& operator=(const SoundBank&) = delete;
    ~SoundBank() {
        // Stop SDL's audio callback thread BEFORE our buffers (music_/voices_) are
        // destroyed. SDL_CloseAudioDevice blocks until the callback returns and
        // won't call it again, so mixThunk can't fire on freed state -- this is the
        // return-to-menu teardown crash (GameView, and thus SoundBank, is freed).
        releaseOutput();
    }

    // Retain indexed/decoded resources while a read-only replay temporarily
    // owns the output. Closing joins SDL's callback before another bank opens.
    void releaseOutput() {
        if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
    }

    void setAudioTap(void* context, void (*tap)(void*, const int16_t*, int, int)) {
        if (dev_) SDL_LockAudioDevice(dev_);
        tapContext_ = context; tap_ = tap;
        if (dev_) SDL_UnlockAudioDevice(dev_);
    }

    void reload(const tak::hpi::Vfs& vfs) {
        releaseOutput();
        for(auto& channel:channels_)channel=Channel{};
        index_.clear();cache_.clear();peaks_.clear();playlist_.clear();music_=Pcm{};
        musicPos_=musicTrack_=0;musicDone_=false;
        init(vfs);
    }
    void init(const tak::hpi::Vfs& vfs) {
        vfs_ = &vfs;
        // Index the sounds/ namespace by stem (user mods already win via the
        // VFS). A missing sounds dir must NOT skip audio init (music shares the
        // device); just index whatever's there.
        for (const std::string& path : vfs.list("sounds")) {
            std::string ext = tak::vpath::extension(path);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".wav") continue;
            std::string stem = tak::vpath::stem(path);
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            index_[stem] = path;
        }

        openOutput();
    }

    // Open (or re-open) the output device on the current tak::g_audioDevice. Safe to call
    // again to switch devices live (Options): it closes the old device first, which stops
    // and joins the mix thread, so there's no concurrent access to chan_/spec_ while we
    // reconfigure. The detected channel layout follows the newly-chosen device.
    void openOutput() {
        if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        // The process-wide detected layout (see tak::detectOutputChannels) -- shared
        // with the Options per-speaker sliders so they always match what we mix into,
        // and it reveals surround even when the default sink advertises stereo.
        int chans = tak::detectOutputChannels();
        SDL_AudioSpec want{};
        want.freq = kMixRate;
        want.format = AUDIO_S16SYS;
        want.channels = Uint8(chans);
        // 1024 samples @ 44100 Hz = ~23ms of device buffer. The old 1024 @ 11025
        // put ~93ms between a command click and its acknowledgment tone -- a very
        // feelable lag on the mouse. The mix callback is light (a handful of
        // channels), so short periods are cheap; the OS adds its own safety on top.
        want.samples = 1024;
        want.callback = &SoundBank::mixThunk;
        want.userdata = this;
        dev_ = tak::openAudioDevice(0, &want, &spec_, SDL_AUDIO_ALLOW_CHANNELS_CHANGE);
        chan_ = dev_ ? (spec_.channels ? spec_.channels : 2) : 1;
        std::fprintf(stderr, "audio: %d output channels%s%s\n", chan_,
                     chan_ >= 4 ? " (surround: front/rear enabled)" : "",
                     tak::audio::hasLfe(chan_) ? ", LFE subwoofer driven" : "");
        if (dev_) SDL_PauseAudioDevice(dev_, 0);
    }

    // Live output-device switch from Options: re-open on the now-current device.
    void reopenDevice() { openOutput(); }

    // (User sound mods -- e.g. the always-enabled mods/cosmetic/always_enabled/click/ replacing the
    // faction order tones -- arrive through the VFS's mod layer, which wins the
    // sounds/ namespace automatically, so no separate sound loader is needed.)

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
                if (c.positional && c.data && (c.looping || c.pos < c.data->frames())) repan(c);
            SDL_UnlockAudioDevice(dev_);
        }
    }

    // Non-positional (UI, music-adjacent) — centred across all speakers.
    void play(const std::string& name, float gain = 1.0f, float rate = 1.0f, int priority = 7, bool looping = false) {
        if (effectsMuted_) return;
        playAt(name, 0.0f, 0.0f, false, 0, 0, gain, rate, priority, looping);
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
            for (int16_t v : samples->samples) peak = std::max(peak, std::abs(int(v)));
        float pf = float(peak) / 32767.0f;
        peaks_[n] = pf;
        return pf;
    }

    // Positional: pan by the source's world position relative to the listener.
    // Left/right from x; front(up)/rear(down) from z on surround setups. The sound is
    // tagged positional, so setListener re-pans it every frame as the camera moves --
    // it tracks its world source for its whole duration, not just at trigger time.
    void playWorld(const std::string& name, float x, float z, int priority = 3) {
        if (effectsMuted_) return;
        float pan = std::clamp((x - listenX_) / listenHW_, -1.0f, 1.0f);
        float depth = std::clamp((z - listenZ_) / listenHH_, -1.0f, 1.0f);
        playAt(name, pan, depth, true, x, z, 1.f, 1.f, priority);
    }

    // Play the synthesised 10-second disco loop as a positional SFX from (x,z) -- the
    // Shift+D dance-floor track. Generated in code (no shipped asset), mono, and
    // cached under "disco", so it pans/fades like any unit sound.
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
        const Pcm* data = &it->second;
        SDL_LockAudioDevice(dev_);
        for (auto& c : channels_)
            if (c.data == data) { c.wx = x; c.wz = z; repan(c); }
        SDL_UnlockAudioDevice(dev_);
    }

  private:
    void buildDisco() {
        constexpr float PI = 3.14159265358979f;
        const int SR = kSfxRate;
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
        // The filters and envelopes are tuned per kSfxRate sample: cached at that rate,
        // the mixer steps it up like any 11025 Hz unit sound.
        cache_["disco"] = Pcm{std::move(pcm), kSfxRate, 1};
        index_["disco"] = "disco";
    }

    void buildMetal() {
        constexpr float PI = 3.14159265358979f;
        const int SR = kSfxRate;
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
        cache_["metal"] = Pcm{std::move(pcm), kSfxRate, 1};
        index_["metal"] = "metal";
    }

  public:

#ifndef NDEBUG
    // Dev harness probe: every playAt request and what became of it.
    struct RequestTrace {
        std::string name;
        bool positional=false;
        float x=0,z=0,gain=0,peak=0;
        int priority=0,slot=-1;
        bool indexed=false,loaded=false;
    };
    std::function<void(const RequestTrace&)> requestHook;
    // A harness compressing minutes of game time into seconds ends every voice
    // itself, as real-time playback would have.
    void debugSilence() {
        if (dev_) SDL_LockAudioDevice(dev_);
        for (auto& channel:channels_) channel=Channel{};
        if (dev_) SDL_UnlockAudioDevice(dev_);
    }
#endif

    void playAt(const std::string& name, float pan, float depth,
                bool positional = false, float wx = 0, float wz = 0,
                float gain = 1.0f, float rate = 1.0f, int priority = 3, bool looping = false) {
#ifndef NDEBUG
        // Reported on every exit, with the voice slot the mixer granted (-1 none).
        struct Report {
            SoundBank& bank;RequestTrace trace;
            ~Report() { if (bank.requestHook) bank.requestHook(trace); }
        } report{*this,{}};
        if (requestHook) {
            auto& r=report.trace;
            r.name=name;r.positional=positional;r.x=wx;r.z=wz;r.priority=priority;
            r.gain=positional ? worldSoundGain(wx,wz) : 1.f;
            std::string key=name;
            std::transform(key.begin(),key.end(),key.begin(),::tolower);
            r.indexed=index_.count(key)!=0;
            r.peak=r.indexed ? peakOf(key) : 0.f;
            r.loaded=r.indexed && cache_.count(key) && !cache_.at(key).samples.empty();
        }
#endif
        // Inaudible one-shots must not consume/evict an audible voice or load
        // their sample. Loops retain their clock so camera movement can reveal them.
        if (positional && !looping && worldSoundGain(wx,wz) <= 0) return;
        std::string n = name;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        auto it = index_.find(n);
        if (it == index_.end()) return;
        if (verbose_) std::printf("SND %s\n", n.c_str());
        const auto* samples = load(n, it->second);
        if (!samples || samples->frames() == 0 || !dev_) return;
        SDL_LockAudioDevice(dev_);
        std::array<tak::SoundVoice,kMaxVoices> voices;
        bool free=false;
        const size_t limit=size_t(voiceLimit_);
        for (size_t i=0;i<limit;++i) {
            const auto& c=channels_[i];
            const bool active=c.data && (c.looping || c.pos<c.data->frames());
            voices[i]={active,c.looping,c.priority,c.started};
            free|=!active;
        }
        // Retail class 0/1 chatter requires a free voice, even if it could
        // otherwise preempt a lower class. Classes 2..7 use priority eviction.
        const int slot=(priority<=1 && !free) ? -1 : tak::retailSoundVoice(std::span(voices).first(limit),priority);
        if (slot>=0) {
            auto& c=channels_[size_t(slot)];
            c.data=samples;c.pos=0;c.pan=pan;c.depth=depth;
            c.positional=positional;c.wx=wx;c.wz=wz;c.gain=gain*(evenOut_ ? samples->evenGain : 1.f);
            c.distanceGain=positional ? worldSoundGain(wx,wz) : 1.f;
            c.fpos=0;c.step=stepFor(std::clamp(rate,0.25f,4.f)*double(samples->rate)/kMixRate);
            c.wrapped=false;
            c.priority=priority;c.started=++soundSequence_;c.looping=looping;
#ifndef NDEBUG
            report.trace.slot=slot;
#endif
        }
        SDL_UnlockAudioDevice(dev_);
    }

private:
    struct Channel {
        const Pcm* data = nullptr;
        size_t pos = 0;             // source frame
        float pan = 0, depth = 0;   // -1..+1 : left..right, front..rear
        float wx = 0, wz = 0;       // world emission point (for positional re-panning)
        bool positional = false;    // true = re-pan every frame from (wx,wz)
        float gain = 1.0f;          // per-sound boost (UI clicks undo the /2 headroom)
        float distanceGain = 1.f;   // independent of the authored/UI gain
        uint64_t step = kUnitStep;  // 32.32 source frames per output frame (pitch x rate)
        int priority=0;
        uint64_t started=0;
        bool looping=false;
        bool wrapped=false;         // a loop has come round: frames before 0 are its end
        uint64_t fpos = 0;          // 32.32 fractional read position in source frames
    };
    static constexpr uint64_t kUnitStep = uint64_t(1) << 32;
    static uint64_t stepFor(double ratio) { return uint64_t(ratio * double(kUnitStep) + 0.5); }
    // Source frame `i` (its first sample), or null for silence: before frame 0 and past
    // the end of a one-shot. A loop wraps both ways once it has come round, but its
    // first pass still starts from silence.
    static const int16_t* frameAt(const Pcm& p, int64_t i, bool loop, bool wrapped) {
        const int64_t n = int64_t(p.frames());
        if (i < 0 || i >= n) {
            if (!loop || n == 0 || (i < 0 && !wrapped)) return nullptr;
            i %= n;
            if (i < 0) i += n;
        }
        return &p.samples[size_t(i) * size_t(p.channels)];
    }
    // Every channel of the source at the 32.32 position `fpos` read `step` per output
    // frame, into out[0..channels). Exact 1:1 (step == kUnitStep) copies the frame,
    // bit for bit. Slower than 1:1 the polyphase kernel sits on frames n-15..n+16 at the
    // blended phase; faster, it is stretched by the step (cutoff / step) and its taps
    // are renormalised. Allocation- and lock-free: it runs on the audio thread.
    static void framesAt(const Pcm& p, uint64_t fpos, uint64_t step, bool loop, bool wrapped, int* out) {
        namespace s = tak::sinc;
        const int c = std::clamp(p.channels, 1, 8);
        const int64_t n = int64_t(fpos >> 32), total = int64_t(p.frames());
        if (step == kUnitStep) {
            const int16_t* x = frameAt(p, n, loop, wrapped);
            for (int k = 0; k < c; ++k) out[k] = x ? x[k] : 0;
            return;
        }
        float w[s::kMaxTaps];
        const float* wp = w;   // the weights: w, or a table row as it is
        int64_t first = n - (s::kTaps / 2 - 1);
        int taps = s::kTaps;
        const uint32_t frac = uint32_t(fpos);
        if (step < kUnitStep) {
            constexpr int shift = 32 - s::kPhaseBits;
            constexpr uint32_t below = (uint32_t(1) << shift) - 1;   // the fraction between phases
            const uint32_t j = frac >> shift;
            const float a = float(frac & below) * (1.0f / float(below + 1));
            const auto& r0 = s::kTable.row[j];
            const auto& r1 = s::kTable.row[j + 1];
            if (a == 0) wp = r0.data();   // on a phase (11025/22050 at 44100 always are)
            else for (int m = 0; m < taps; ++m) w[m] = r0[size_t(m)] + a * (r1[size_t(m)] - r0[size_t(m)]);
        } else {
            const double st = std::min(double(step) / double(kUnitStep), double(s::kMaxStep));
            const double f = double(frac) / double(kUnitStep), half = s::kTaps / 2 * st;
            const int64_t lo = int64_t(std::floor(f - half)) + 1, hi = int64_t(std::ceil(f + half)) - 1;
            first = n + lo;
            taps = int(std::min<int64_t>(hi - lo + 1, s::kMaxTaps));
            float sum = 0;
            for (int m = 0; m < taps; ++m) sum += w[m] = s::kernel((double(lo + m) - f) / st);
            const float norm = sum != 0 ? 1.0f / sum : 0.0f;
            for (int m = 0; m < taps; ++m) w[m] *= norm;
        }
        float acc[8] = {};
        if (first >= 0 && first + taps <= total) {
            const int16_t* x = &p.samples[size_t(first) * size_t(c)];
            if (c == 1) {
                // Eight running sums (one per lane) so the compiler can vectorise the
                // dot product without reassociating floats.
                float a8[8] = {};
                int m = 0;
                for (; m + 8 <= taps; m += 8)
                    for (int i = 0; i < 8; ++i) a8[i] += wp[m + i] * float(x[m + i]);
                for (; m < taps; ++m) a8[0] += wp[m] * float(x[m]);
                acc[0] = ((a8[0] + a8[4]) + (a8[1] + a8[5])) + ((a8[2] + a8[6]) + (a8[3] + a8[7]));
            } else if (c == 2) {
                float a8[8] = {};   // lanes L R L R ..: four frames per step
                int m = 0;
                for (; m + 4 <= taps; m += 4)
                    for (int i = 0; i < 8; ++i) a8[i] += wp[m + i / 2] * float(x[2 * m + i]);
                for (; m < taps; ++m) {
                    a8[0] += wp[m] * float(x[2 * m]);
                    a8[1] += wp[m] * float(x[2 * m + 1]);
                }
                acc[0] = (a8[0] + a8[4]) + (a8[2] + a8[6]);
                acc[1] = (a8[1] + a8[5]) + (a8[3] + a8[7]);
            } else {
                for (int m = 0; m < taps; ++m)
                    for (int k = 0; k < c; ++k) acc[k] += wp[m] * float(x[m * c + k]);
            }
        } else {
            for (int m = 0; m < taps; ++m)
                if (const int16_t* x = frameAt(p, first + m, loop, wrapped))
                    for (int k = 0; k < c; ++k) acc[k] += wp[m] * float(x[k]);
        }
        for (int k = 0; k < c; ++k) out[k] = int(std::lrint(acc[k]));
    }
    // Full gain throughout the camera rectangle. Outside, measure the shortest
    // world-space distance to its edge (including corners), then fade to zero
    // over one shorter viewport dimension. This intentional retail deviation
    // keeps distant battles from remaining audible across the entire map.
    float worldSoundGain(float x,float z) const {
        const float dx=std::max(0.f,std::abs(x-listenX_)-listenHW_);
        const float dz=std::max(0.f,std::abs(z-listenZ_)-listenHH_);
        const float distance=std::sqrt(dx*dx+dz*dz);
        const float remaining=std::clamp(1.f-distance/(2.f*std::min(listenHW_,listenHH_)),0.f,1.f);
        return remaining*remaining;
    }
    // Recompute a channel's pan/depth from its world point and the current listener.
    void repan(Channel& c) {
        c.pan = std::clamp((c.wx - listenX_) / listenHW_, -1.0f, 1.0f);
        c.depth = std::clamp((c.wz - listenZ_) / listenHH_, -1.0f, 1.0f);
        c.distanceGain = worldSoundGain(c.wx,c.wz);
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

    // Decode a WAV (from memory) to S16 at its own rate and channel count -- only a
    // rate above the mix rate is converted down, and never up (memory): the mixer
    // steps through it at srcRate/kMixRate. Any format SDL loads works, so a mod WAV
    // need not match the shipped ones. SDL_GetError() says why on failure.
    static std::optional<Pcm> convertWav(const uint8_t* data, size_t size) {
        SDL_AudioSpec spec{};
        Uint8* buf = nullptr;
        Uint32 len = 0;
        SDL_RWops* rw = size ? SDL_RWFromConstMem(data, int(size)) : nullptr;
        if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) return std::nullopt;
        const int channels = spec.channels, rate = std::min(spec.freq, kMixRate);
        SDL_AudioCVT cvt;
        if (channels < 1 || channels > 8 || spec.freq <= 0 ||
            SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_S16SYS,
                              spec.channels, rate) < 0) {
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
        Pcm out{std::vector<int16_t>(outBytes / 2 / size_t(channels) * size_t(channels)), rate, channels};
        std::memcpy(out.samples.data(), work.data(), out.samples.size() * 2);
        return out;
    }
    std::optional<Pcm> decodeWav(const uint8_t* data, size_t size) { return convertWav(data, size); }

    const Pcm* load(const std::string& key, const std::string& path) {
        auto it = cache_.find(key);
        if (it != cache_.end()) return &it->second;
        if (!vfs_) return nullptr;
        std::vector<uint8_t> bytes;
        try { bytes = vfs_->read(path); } catch (const std::exception&) { return nullptr; }
        auto pcm = decodeWav(bytes.data(), bytes.size());
        if (!pcm) return nullptr;
        pcm->evenGain = tak::evenOutGain(pcm->samples.data(), pcm->samples.size(), pcm->rate, pcm->channels);
        return &cache_.emplace(key, std::move(*pcm)).first->second;
    }

    void* tapContext_ = nullptr;
    void (*tap_)(void*, const int16_t*, int, int) = nullptr;
    static void mixThunk(void* ud, Uint8* stream, int len) {
        auto* bank = static_cast<SoundBank*>(ud);
        auto* pcm = reinterpret_cast<int16_t*>(stream);
        bank->mix(pcm, len / 2);
        // The stream capture takes the device mix as it is (video::kAudioRate =
        // kMixRate, any layout: Stream::audio folds it to stereo).
        if (bank->tap_) bank->tap_(bank->tapContext_, pcm, len / 2 / std::max(bank->chan_, 1), bank->chan_);
    }

    void mix(int16_t* out, int n) {
        std::memset(out, 0, size_t(n) * 2);
        int ch = std::max(chan_, 1);
        int frames = n / ch;
        int lfe = tak::audio::speakerIndex(ch, tak::audio::Speaker::LFE);   // the sub, if any
        if (lfe >= 0) lfeMono_.assign(size_t(frames), 0);   // full-range mono for the sub
        auto add = [&](int f, int ci, int v) {
            int idx = f * ch + ci;
            out[idx] = int16_t(std::clamp(out[idx] + v, -32768, 32767));
        };
        // The sub feed of a source without its own LFE: the mono sum of its stereo fold.
        auto subFeed = [](int sc, float* w) {
            const auto two = tak::audio::downmixMatrix(sc, 2);
            const bool own = tak::audio::hasLfe(sc);
            for (int k = 0; k < sc; ++k)
                w[k] = sc == 1 ? 1.f : own ? 0.f : 0.5f * (two[0][size_t(k)] + two[1][size_t(k)]);
        };
        int src[8];
        // Background music bed (quieter than SFX), at the track's own rate and
        // channels: musicMatrix (util/audiolayout.h) lays a stereo track on the
        // speakers as the game always has and a multichannel one channel for channel.
        // The LFE gets the low-passed sub feed of the mains' mono sum, or a 5.1/7.1
        // track's own LFE channel.
        if (!music_.samples.empty()) {
            const int mc = music_.channels;
            const auto mm = tak::audio::musicMatrix(mc, ch);
            float feed[8];
            subFeed(mc, feed);
            const size_t total = music_.frames();
            for (int f = 0; f < frames; ++f) {
                const size_t frame = size_t(musicPos_ >> 32);
                if (frame >= total) { musicDone_ = true; break; }
                framesAt(music_, musicPos_, musicStep_, false, false, src);
                for (int k = 0; k < mc; ++k) src[k] = src[k] * musicVol_ / 256;
                musicPos_ += musicStep_;
                for (int o = 0; o < ch; ++o) {
                    float v = 0;
                    for (int k = 0; k < mc; ++k) v += mm[size_t(o)][size_t(k)] * float(src[k]);
                    if (v != 0.0f) add(f, o, int(v));
                }
                if (lfe >= 0) {
                    float v = 0;
                    for (int k = 0; k < mc; ++k) v += feed[k] * float(src[k]);
                    lfeMono_[size_t(f)] += int(v);
                }
            }
        } else {
            musicDone_ = true;
        }
        // Effects. A mono one is panned into the layout by channelGains. A stereo one
        // keeps its image and still pans and fades with position: the left source
        // feeds the left-side speakers (FL/BL/SL) and the right the right-side ones,
        // each at that speaker's channelGains, while the centres and the sub take the
        // mono sum. Its width narrows to mono as it pans off to one side, so a sound
        // at the screen edge does not lose its far channel. A positional multichannel
        // sound is first folded to stereo at the source (a point in the world has no
        // surround image of its own); a non-positional (UI) one maps channel for
        // channel onto the output and down-mixes only what the output lacks.
        float g[8];
        for (auto& c : channels_) {
            if (!c.data || (!c.looping && c.pos >= c.data->frames())) continue;
            const Pcm& p = *c.data;
            const int sc = std::clamp(p.channels, 1, 8);
            const size_t total = p.frames();
            channelGains(c.pan, c.depth, g);
            const bool multi = sc > 2 && !c.positional;
            const auto fold = sc > 2 ? tak::audio::downmixMatrix(sc, 2) : tak::audio::Matrix{};
            const auto mm = multi ? tak::audio::downmixMatrix(sc, ch) : tak::audio::Matrix{};
            const auto roles = tak::audio::speakers(ch);
            float feed[8];
            if (multi) subFeed(sc, feed);
            const float width = c.positional ? 1.0f - std::fabs(c.pan) : 1.0f;
            const float level = c.gain * c.distanceGain;
            auto scale = [&](float v) { return int(v * level) / 2 * sfxVol_ / 256; };
            for (int f = 0; f < frames; ++f) {
                c.pos = size_t(c.fpos >> 32);
                if (c.pos >= total) {
                    if (!c.looping || total == 0) break;
                    c.fpos %= uint64_t(total) << 32;
                    c.pos = size_t(c.fpos >> 32);
                    c.wrapped = true;
                }
                framesAt(p, c.fpos, c.step, c.looping, c.wrapped, src);
                c.fpos += c.step;
                if (sc == 1) {
                    int s = scale(float(src[0]));
                    for (int ci = 0; ci < ch; ++ci)
                        if (g[ci] != 0.0f) add(f, ci, int(s * g[ci]));
                    if (lfe >= 0) lfeMono_[size_t(f)] += s;
                    continue;
                }
                if (multi) {
                    for (int o = 0; o < ch; ++o) {
                        float v = 0;
                        for (int k = 0; k < sc; ++k) v += mm[size_t(o)][size_t(k)] * float(src[k]);
                        if (v != 0.0f) add(f, o, scale(v));
                    }
                    if (lfe >= 0) {
                        float v = 0;
                        for (int k = 0; k < sc; ++k) v += feed[k] * float(src[k]);
                        lfeMono_[size_t(f)] += scale(v);
                    }
                    continue;
                }
                float l = float(src[0]), r = float(src[1]);
                if (sc > 2) {
                    l = r = 0;
                    for (int k = 0; k < sc; ++k) {
                        l += fold[0][size_t(k)] * float(src[k]);
                        r += fold[1][size_t(k)] * float(src[k]);
                    }
                }
                const float mid = 0.5f * (l + r), side = 0.5f * (l - r) * width;
                const int ls = scale(mid + side), rs = scale(mid - side), ms = scale(mid);
                for (int ci = 0; ci < ch; ++ci) {
                    if (g[ci] == 0.0f) continue;
                    using S = tak::audio::Speaker;
                    const S role = roles[size_t(ci)];
                    const int v = (role == S::FL || role == S::BL || role == S::SL) ? ls
                                : (role == S::FR || role == S::BR || role == S::SR) ? rs : ms;
                    add(f, ci, int(v * g[ci]));
                }
                if (lfe >= 0) lfeMono_[size_t(f)] += ms;
            }
        }
        // Subwoofer: one-pole low-pass of the mono mix (~110 Hz cutoff at the mix
        // rate), fed into the LFE channel so a 2.1..7.1 rig drives the sub directly.
        if (lfe >= 0) {
            // 1/(1 + fs/(2*pi*fc)), fc = 110 Hz: ~0.0154 at 44100 Hz.
            constexpr float kAlpha = 1.0f / (1.0f + float(kMixRate) / (2.0f * 3.14159265f * 110.0f));
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
    // `custom` false plays the originals only (the lobby: CUSTOM MUSIC is for games).
    void startMusic(const tak::hpi::Vfs& vfs, const std::vector<int>& tracks, bool custom = true) {
        vfs_ = &vfs;
        musicCustom_ = custom;
        std::vector<int> want = tracks;
        if (want.empty())
            for (int i = 1; i <= 20; ++i) want.push_back(i);
        for (int n : want)   // an original the install has, or a custom file for it
            if (vfs.has("music/track" + std::to_string(n) + ".wav") || (custom && !tak::music::customFile(n).empty()))
                playlist_.push_back(n);
        std::fprintf(stderr, "music: %zu faction tracks, audio=%s\n", playlist_.size(),
                     dev_ ? "yes" : "NO DEVICE");
        if (!playlist_.empty() && dev_) {
            std::shuffle(playlist_.begin(), playlist_.end(),
                         std::mt19937{std::random_device{}()});
            loadTrack(0);
        }
    }

    // Advance to the next track when the current one finishes (call per frame). The next
    // one was decoded in the background meanwhile (a custom MP3 or FLAC takes a moment);
    // if it is not ready yet, the music waits a frame or two rather than the game.
    void pollMusic() {
        if (!musicDone_ || playlist_.empty()) return;
        if (playlist_.size() == 1 && !music_.samples.empty()) {   // one track: play it again
            SDL_LockAudioDevice(dev_);
            musicPos_ = 0;
            musicDone_ = false;
            SDL_UnlockAudioDevice(dev_);
            return;
        }
        if (nextMusic_.valid() && nextMusic_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        musicDone_ = false;
        loadTrack((musicTrack_ + 1) % playlist_.size());
    }

    // The volumes and channel gains are read by mix() on the audio thread: set them
    // under the device lock, as every other mixer state is.
    void setMusicVolume(int v) { underMixLock([&] { musicVol_ = std::clamp(v, 0, 256); }); }
    void setMasterVolume(int v) { underMixLock([&] { masterVol_ = std::clamp(v, 0, 256); }); }
    void setSfxVolume(int v) { underMixLock([&] { sfxVol_ = std::clamp(v, 0, 256); }); }
    // How many sound effects play at once (Options: SOUND VOICES). Retail's voice
    // manager (56ed40) holds 32; past the pool a new sound replaces a strictly lower
    // class or is not played. Lowering it stops the voices past the new limit.
    void setVoiceLimit(int n) {
        n = std::clamp(n, kMinVoices, kMaxVoices);
        if (n == voiceLimit_) return;
        if (dev_) SDL_LockAudioDevice(dev_);
        for (int i = n; i < kMaxVoices; ++i) channels_[i] = Channel{};
        voiceLimit_ = n;
        if (dev_) SDL_UnlockAudioDevice(dev_);
    }
    int voiceLimit() const { return voiceLimit_; }
    // EVEN OUT SOUND LEVELS (Options; ours): quiet effects play raised by their
    // evenGain (client/soundlevel.h). Off is retail. Applies to sounds started after.
    void setEvenOut(bool on) { evenOut_ = on; }
    static constexpr int kMinVoices = 8, kMaxVoices = 256, kRetailVoices = 32;
    // New effects stop starting (a replay running too fast to follow); music plays on.
    void setEffectsMuted(bool muted) { effectsMuted_ = muted; }
    void setChannelGain(int i, float g) {
        if (i >= 0 && i < 8) underMixLock([&] { chanGain_[i] = std::clamp(g, 0.0f, 1.0f); });
    }

    // For the Options screen: how many output channels the device gave us.
    int channelCount() const { return std::clamp(chan_, 1, 8); }

private:
    template <class F> void underMixLock(F&& set) {
        if (dev_) SDL_LockAudioDevice(dev_);
        set();
        if (dev_) SDL_UnlockAudioDevice(dev_);
    }

    void loadTrack(size_t idx) {
        // At the track's own rate and channels: the retail tracks are 22050 Hz stereo
        // (decoded from IMA ADPCM, kept at 22050: half the memory of 44100), a mod's
        // replacement keeps its 44.1 kHz or its 5.1; mix() steps and lays it out.
        // CUSTOM MUSIC: the player's own file for this track, if set (tak::music).
        std::optional<tak::music::Pcm16> pcm;
        if (nextMusic_.valid() && nextMusicIdx_ == idx) pcm = nextMusic_.get();
        else if (vfs_) pcm = tak::music::loadTrack(*vfs_, playlist_[idx], kMixRate, musicCustom_);
        if (nextMusic_.valid()) nextMusic_.get();   // a stale prefetch (the list changed)
        if (!pcm) return;   // loadTrack said why
        // Decode the one after while this one plays.
        if (vfs_ && playlist_.size() > 1) {
            nextMusicIdx_ = (idx + 1) % playlist_.size();
            nextMusic_ = std::async(std::launch::async, [vfs = vfs_, track = playlist_[nextMusicIdx_], custom = musicCustom_] {
                return tak::music::loadTrack(*vfs, track, kMixRate, custom);
            });
        }
        SDL_LockAudioDevice(dev_);
        music_.samples = std::move(pcm->samples);
        music_.rate = pcm->rate;
        music_.channels = pcm->channels;
        musicStep_ = stepFor(double(music_.rate) / kMixRate);
        musicPos_ = 0;
        musicTrack_ = idx;
        musicDone_ = false;
        SDL_UnlockAudioDevice(dev_);
        std::fprintf(stderr, "music: now playing track%d\n", playlist_[idx]);   // (loadTrack named a custom file)
    }

    std::map<std::string, std::string> index_;
    std::map<std::string, Pcm> cache_;
    std::map<std::string, float> peaks_;   // per-sound peak (UI normalization)
    Channel channels_[kMaxVoices];
    int voiceLimit_ = kRetailVoices;   // channels_[0, voiceLimit_) take new sounds
    bool evenOut_ = false;             // EVEN OUT SOUND LEVELS
    uint64_t soundSequence_=0;
    std::vector<int> playlist_;    // original track numbers (music/track<n>.wav)
    std::future<std::optional<tak::music::Pcm16>> nextMusic_;   // the next track, decoding
    size_t nextMusicIdx_ = 0;
    Pcm music_;                    // the current track, at its own rate and channels
    uint64_t musicPos_ = 0;        // 32.32 position in music_'s frames
    uint64_t musicStep_ = kUnitStep;   // music_.rate / kMixRate, 32.32
    size_t musicTrack_ = 0;
    bool musicCustom_ = true;   // the playlist may play CUSTOM MUSIC files (not in the lobby)
    int musicVol_ = 128;  // out of 256 (BGM); 50% default
    int masterVol_ = 256; // out of 256, global gain over the whole mix
    int sfxVol_ = 256;    // out of 256, sound effects
    float chanGain_[8] = {1, 1, 1, 1, 1, 1, 1, 1};   // per-output-channel trim 0..1
    bool musicDone_ = false;
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec spec_{};
    int chan_ = 1;              // output channel count (2=stereo, 4/6/8=surround)
    bool verbose_ = false;
    bool effectsMuted_ = false;
    // Subwoofer (LFE) feed: a one-pole low-pass of the full mono mix, driven into the
    // 5.1/7.1 LFE channel. lpfState_ persists across callbacks (the filter's memory).
    float lpfState_ = 0;
    std::vector<int> lfeMono_;   // per-callback mono accumulator (reused, no hot alloc)
};

// Sound classes: gamedata/soundclasses/*.tdf map a class to event ->
// candidate WAV names.
class SoundClasses {
public:
    void load(const tak::hpi::Vfs& vfs) {
        classes_.clear();
        try {
            for (const std::string& path : vfs.list("gamedata/soundclasses")) {
                if (tak::vpath::extension(path) != ".tdf") continue;
                try {
                    auto sb = vfs.read(path);
                    auto root = tak::tdf::parseText(std::string(sb.begin(), sb.end()), path);
                    for (const auto& clsName : root.childOrder) {
                        auto& cls = classes_[clsName];
                        const auto& node = root.children.at(clsName);
                        for (const auto& evName : node.childOrder) {
                            auto& list = cls[evName];
                            // Two shipped layouts. Unit voice classes are WEIGHTED
                            // "NAME = weight" entries: the "_NN-note" click tone
                            // carries 100.0 vs 1.0 per voice line, so retail mostly
                            // bongs and only occasionally speaks. The weapon impact
                            // classes in soundclasses.tdf (sword, arrow, rock, ...)
                            // list "soundN = FILE.wav" instead: the WAV is the VALUE,
                            // and every variant is equally likely. Reading those as
                            // "name = weight" picked a sound called "sound0" that
                            // does not exist, so every soundhitclass impact was silent.
                            for (const auto& [key, value] : node.children.at(evName).values) {
                                if (isVariantKey(key)) {
                                    list.push_back({wavStem(value), 1.0f});
                                    continue;
                                }
                                float w = float(std::atof(value.c_str()));
                                list.push_back({key, w > 0 ? w : 1.0f});
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

    // "sound0".."soundNN" (any case): the soundclasses.tdf variant layout.
    static bool isVariantKey(const std::string& key) {
        if (key.size() < 6) return false;
        std::string head = key.substr(0, 5);
        std::transform(head.begin(), head.end(), head.begin(), ::tolower);
        if (head != "sound") return false;
        return std::all_of(key.begin() + 5, key.end(), [](unsigned char c) { return std::isdigit(c); });
    }
    // "RHITFLS1.wav" -> "rhitfls1": the SoundBank indexes sounds/ by lowercase stem.
    static std::string wavStem(std::string v) {
        std::transform(v.begin(), v.end(), v.begin(), ::tolower);
        if (v.size() > 4 && v.compare(v.size() - 4, 4, ".wav") == 0) v.resize(v.size() - 4);
        return v;
    }

private:
    std::map<std::string,
             std::map<std::string, std::vector<std::pair<std::string, float>>>>
        classes_;
};

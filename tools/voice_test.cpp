// voice_test -- voice chat, client side (client/voice.h), without a microphone, speakers
// or a network:
//
//  - the Opus codec round trip on a synthetic tone (level kept, packets small);
//  - the jitter buffer: buffering, in-order play, a dropped frame rebuilt from FEC, a late
//    frame waited out, END, 16-bit sequence wrap, a burst trimmed;
//  - the push-to-talk rules: never while typing, never leaking a held key across a focus
//    change, and the Space default with its modifier rules;
//  - the settings: voice chat OFF by default, a round trip through the file;
//  - the engine on SDL's DISK audio driver: a synthetic tone in the capture file, captured
//    only while talking, sent as START..END frames, played back (looped in as a remote
//    talker) at the output volume and at a per-player VOLUME, mic volume applied, a
//    missing device falling back;
//  - the output mix: each talker at its diplomacy VOLUME (synthetic PCM, two talkers, one
//    at 50%), then the output volume and the soft limiter;
//  - the VOICE CHAT page: every row but the switch greyed and inert while off.
#include "client/hotkeys.h"
#include "client/options.h"
#include "client/settings.h"
#include "client/voice.h"
#include "client/voicecodec.h"
#include "client/voicejitter.h"
#include "client/voiceptt.h"
#include "net/protocol.h"
#include "util/png.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tak;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_fail; std::printf("  FAIL %s\n", what.c_str()); }
}

static std::vector<float> tone(int samples, float amp, float hz = 440.0f, int offset = 0) {
    std::vector<float> v(static_cast<size_t>(samples));
    for (int i = 0; i < samples; ++i)
        v[size_t(i)] = amp * std::sin(2.0f * 3.14159265f * hz * float(i + offset) / float(voice::kRate));
    return v;
}
static float rms(const float* p, size_t n) {
    double s = 0;
    for (size_t i = 0; i < n; ++i) s += double(p[i]) * p[i];
    return n ? float(std::sqrt(s / double(n))) : 0.0f;
}

static void testCodec() {
    std::printf("codec\n");
    voice::Encoder enc;
    voice::Decoder dec;
    check(enc.ok() && dec.ok(), "opus encoder/decoder create");
    const auto pcm = tone(voice::kFrame * 50, 0.5f);
    std::vector<float> out;
    size_t bytes = 0, biggest = 0;
    std::vector<std::vector<uint8_t>> packets;
    for (int f = 0; f < 50; ++f) {
        auto pkt = enc.encode(pcm.data() + size_t(f) * voice::kFrame);
        check(!pkt.empty() && pkt.size() <= net::kVoiceMaxOpus, "packet within the wire cap");
        bytes += pkt.size(); biggest = std::max(biggest, pkt.size());
        packets.push_back(pkt);
        float frame[voice::kFrame];
        check(dec.decode(pkt.data(), pkt.size(), frame), "decode");
        out.insert(out.end(), frame, frame + voice::kFrame);
    }
    // Skip the codec's start-up; the tone's level comes through within a few dB.
    const float in = rms(pcm.data() + 10 * voice::kFrame, 40 * voice::kFrame);
    const float got = rms(out.data() + 10 * voice::kFrame, 40 * voice::kFrame);
    check(got > in * 0.7f && got < in * 1.3f, "decoded level " + std::to_string(got) + " vs " + std::to_string(in));
    const double kbps = double(bytes) * 8 / 1.0 / 1000.0;   // 50 frames = 1 s
    check(kbps > 8 && kbps < 40, "bitrate near 24 kbit/s: " + std::to_string(kbps));
    // FEC and concealment produce a full frame.
    float frame[voice::kFrame];
    check(dec.decodeFec(packets[20].data(), packets[20].size(), frame), "FEC decode");
    check(dec.conceal(frame), "concealment");
    check(!dec.decode(nullptr, 0, frame), "an empty packet is refused");
    std::printf("  %zu bytes/s, largest packet %zu\n", bytes, biggest);
}

static void testJitter() {
    std::printf("jitter buffer\n");
    using voice::JitterBuffer; using voice::Playout;
    auto pkt = [](uint8_t b) { return std::vector<uint8_t>{b}; };
    {
        JitterBuffer jb;
        uint64_t t = 1000;
        jb.push(100, voice::kFlagStart, pkt(1), t);
        check(jb.next().kind == Playout::Idle, "buffers before the target depth");
        jb.push(101, 0, pkt(2), t += 20);
        Playout p = jb.next();
        check(p.kind == Playout::Decode && (*p.data)[0] == 1, "plays the first frame at the target");
        p = jb.next();
        check(p.kind == Playout::Decode && (*p.data)[0] == 2, "then the second, in order");
        // 102 never arrives (the server dropped it); 103 does: rebuilt from 103's FEC.
        jb.push(103, 0, pkt(4), t += 40);
        jb.push(104, 0, pkt(5), t += 20);
        p = jb.next();
        check(p.kind == Playout::Fec && (*p.data)[0] == 4, "a dropped frame comes from the next packet's FEC");
        p = jb.next();
        check(p.kind == Playout::Decode && (*p.data)[0] == 4, "and the next packet still plays");
        p = jb.next();
        check(p.kind == Playout::Decode && (*p.data)[0] == 5, "...followed by its successor");
        // Nothing queued: late, not lost -- conceal without skipping it.
        p = jb.next();
        check(p.kind == Playout::Conceal, "an empty queue conceals");
        jb.push(105, 0, pkt(6), t += 60);
        p = jb.next();
        check(p.kind == Playout::Decode && (*p.data)[0] == 6, "the late frame still plays when it arrives");
        jb.push(106, voice::kFlagEnd, {}, t += 20);
        p = jb.next();
        check(p.kind == Playout::Idle && jb.idle(), "END stops the talker cleanly");
        check(jb.stats().fec == 1 && jb.stats().concealed == 1, "stats count FEC and concealment");
    }
    {   // 16-bit wrap.
        JitterBuffer jb;
        uint64_t t = 0;
        uint16_t s = 65533;
        jb.push(s, voice::kFlagStart, pkt(0), t);
        for (int i = 1; i < 6; ++i) jb.push(uint16_t(s + i), 0, pkt(uint8_t(i)), t += 20);
        bool order = true;
        for (int i = 0; i < 6; ++i) { auto p = jb.next(); order = order && p.kind == Playout::Decode && (*p.data)[0] == i; }
        check(order, "sequence numbers wrap at 65535");
    }
    {   // A burst far deeper than the buffer keeps only the newest.
        JitterBuffer jb;
        jb.push(0, voice::kFlagStart, pkt(0), 0);
        for (int i = 1; i < 40; ++i) jb.push(uint16_t(i), 0, pkt(uint8_t(i)), 0);
        auto p = jb.next();
        check(p.kind == Playout::Decode && (*p.data)[0] >= 40 - JitterBuffer::kMaxTarget && jb.stats().trimmed > 0,
              "a burst is trimmed to the newest frames");
    }
    {   // A late frame from before a new START is ignored.
        JitterBuffer jb;
        jb.push(500, voice::kFlagStart, pkt(1), 0);
        jb.push(499, 0, pkt(9), 0);
        jb.push(501, 0, pkt(2), 0);
        auto p = jb.next();
        check(p.kind == Playout::Decode && (*p.data)[0] == 1 && jb.stats().late == 1, "pre-START frames are dropped");
    }
    {   // A long stall holds a frame more from then on -- but calm playing gives it back.
        JitterBuffer jb;
        uint64_t t = 0;
        uint16_t seq = 0;
        jb.push(seq++, voice::kFlagStart, pkt(0), t);
        jb.push(seq++, 0, pkt(0), t += 20);
        for (int i = 0; i < 2 + JitterBuffer::kMaxWait + 1; ++i) jb.next();   // play 2, then stall out
        check(jb.bump() == 1, "a stall past kMaxWait holds one frame more");
        // Steady 20 ms arrivals, one frame played per frame pushed, for longer than kCalmFrames.
        for (int i = 0; i < JitterBuffer::kCalmFrames + 50; ++i) {
            jb.push(seq++, 0, pkt(1), t += 20);
            jb.next();
        }
        check(jb.bump() == 0 && jb.target() == JitterBuffer::kMinTarget,
              "the extra depth is given back after calm playing: target " + std::to_string(jb.target()));
    }
}

static void testPtt() {
    std::printf("push to talk\n");
    voice::PushToTalk p;
    check(!p.press(false, true, true) && !p.talking(), "a press while typing is text, not voice (not consumed)");
    check(!p.press(false, false, false) && !p.talking(), "no talking where voice is not allowed");
    check(p.press(false, true, false) && p.talking(), "a press starts talking");
    check(p.press(true, true, false) && p.talking(), "key repeat is consumed while talking");
    p.frame(true, true, true, true);
    check(!p.talking(), "a text field taking focus mid-press ends the transmission");
    check(!p.press(true, true, false) && !p.talking(), "and a repeat does not restart it");
    check(!p.release(), "its release then ends nothing");
    p.press(false, true, false); p.frame(true, false, false, true);
    check(!p.talking(), "losing window focus ends it");
    p.press(false, true, false); p.frame(true, false, true, false);
    check(!p.talking(), "a release we never saw ends it");
    p.press(false, true, false); p.frame(false, false, true, true);
    check(!p.talking(), "leaving the room / switching voice off ends it");
    p.press(false, true, false);
    check(p.release() && !p.talking(), "the release ends it");

    // Voice activation: a loud frame opens the gate, which holds through short pauses.
    voice::VoiceGate g;
    check(!g.frame(0.05f, 0.15f) && !g.open(), "quiet frames stay closed");
    check(g.frame(0.3f, 0.15f), "a frame at the level opens it");
    int held = 0;
    while (g.frame(0.0f, 0.15f)) ++held;
    check(held == voice::VoiceGate::kHoldFrames - 1, "it holds through a pause: " + std::to_string(held) + " frames");
    check(!g.open(), "and closes after the hold");
    check(g.frame(0.15f, 0.15f), "exactly the level counts");
    g.reset();
    check(!g.open() && g.frame(0.0f, 0.0f), "reset closes it; level 0 sends everything");

    // The default binding and its matching rules.
    Hotkeys hk; hk.load({});
    const KeyChord c = hk.chord(Act::PushToTalk);
    check(c.key == SDLK_SPACE && c.mod == 0, "Space is the default push-to-talk key");
    check(hk.pushToTalkPress(SDLK_SPACE, 0) && hk.pushToTalkPress(SDLK_SPACE, KMOD_LSHIFT),
          "pressed with extra modifiers it still talks");
    check(!hk.pushToTalkPress(SDLK_a, 0) && hk.pushToTalkRelease(SDLK_SPACE), "other keys do not; the release matches");
    std::map<std::string, std::string> o;
    setChordOverride(o, Act::Stop, KeyChord{SDLK_SPACE, HK_SHIFT});
    hk.load(o);
    check(hk.pushToTalkPress(SDLK_SPACE, 0) && !hk.pushToTalkPress(SDLK_SPACE, KMOD_LSHIFT),
          "a chord bound exactly to another action is that action");
    o.clear();
    setChordOverride(o, Act::PushToTalk, KeyChord{SDLK_v, HK_CTRL});
    check(o.count("voice.ptt") && o["voice.ptt"] == chordName(KeyChord{SDLK_v, HK_CTRL}), "rebinding persists as voice.ptt");
    hk.load(o);
    check(!hk.pushToTalkPress(SDLK_SPACE, 0) && !hk.pushToTalkPress(SDLK_v, 0) && hk.pushToTalkPress(SDLK_v, KMOD_LCTRL),
          "the rebound key replaces Space and needs its modifier");
    setChordOverride(o, Act::PushToTalk, KeyChord{SDLK_SPACE, 0});
    check(o.empty(), "back to Space stores nothing");
    // CONTROLS refuses the keys the game reads itself for push to talk.
    check(pushToTalkReserved(SDLK_RETURN) && pushToTalkReserved(SDLK_KP_ENTER) && pushToTalkReserved(SDLK_0) &&
          pushToTalkReserved(SDLK_9) && pushToTalkReserved(SDLK_LEFT) && pushToTalkReserved(SDLK_DOWN),
          "Enter, digits and arrows are not push-to-talk keys");
    check(!pushToTalkReserved(SDLK_SPACE) && !pushToTalkReserved(SDLK_v) && !pushToTalkReserved(SDLK_CAPSLOCK),
          "Space and ordinary keys are");
}

// The output mix, as VoiceEngine::update() makes it: each talker's decoded frame scaled by
// that player's diplomacy VOLUME and summed, then OUTPUT VOLUME and the soft limiter.
static void testMix() {
    std::printf("per-player volume mix\n");
    constexpr int n = voice::kFrame;
    VoiceEngine v;
    bool full = true;
    for (int p = 0; p < 8; ++p) full = full && v.playerVolume(p) == 100;
    check(full && v.playerVolume(-1) == 100 && v.playerVolume(0xFF) == 100,
          "every player (and a spectator, who has no slot) starts at 100");
    v.setPlayerVolume(3, 150); v.setPlayerVolume(4, -5); v.setPlayerVolume(8, 10); v.setPlayerVolume(-1, 10);
    check(v.playerVolume(3) == 100 && v.playerVolume(4) == 0 && v.playerVolume(8) == 100 && v.playerVolume(-1) == 100,
          "VOLUME clamps to 0..100 and only player slots have one");

    const auto a = tone(n, 0.4f, 440.0f), b = tone(n, 0.3f, 700.0f, 17);
    float mix[n] = {}, plain[n] = {};
    VoiceEngine::mixTalker(mix, a.data(), n, 100);
    VoiceEngine::mixTalker(mix, b.data(), n, 50);
    for (int i = 0; i < n; ++i) plain[i] = a[size_t(i)] + b[size_t(i)];
    float err = 0;
    for (int i = 0; i < n; ++i) err = std::max(err, std::fabs(mix[i] - (a[size_t(i)] + 0.5f * b[size_t(i)])));
    check(err < 1e-6f, "two talkers, one at 50%: the mix is a + b/2 (max error " + std::to_string(err) + ")");
    float bOnly[n] = {};
    VoiceEngine::mixTalker(bOnly, b.data(), n, 50);
    check(std::fabs(rms(bOnly, n) - 0.5f * rms(b.data(), n)) < 1e-4f, "the 50% talker plays at half level");
    // Below the limiter's knee the finished frame is the sum at OUTPUT VOLUME.
    float out[n];
    std::copy(mix, mix + n, out);
    VoiceEngine::finishMix(out, n, 100);
    err = 0;
    for (int i = 0; i < n; ++i) err = std::max(err, std::fabs(out[i] - mix[i]));
    check(err < 1e-6f, "OUTPUT VOLUME 100 leaves a quiet mix as it is");
    // Both at 100 is the plain sum: the default changes nothing.
    float same[n] = {};
    VoiceEngine::mixTalker(same, a.data(), n, 100);
    VoiceEngine::mixTalker(same, b.data(), n, 100);
    err = 0;
    for (int i = 0; i < n; ++i) err = std::max(err, std::fabs(same[i] - plain[i]));
    check(err == 0, "at the default 100 the mix is the plain sum, bit for bit");
    float muted[n] = {};
    VoiceEngine::mixTalker(muted, a.data(), n, 100);
    VoiceEngine::mixTalker(muted, b.data(), n, 0);
    err = 0;
    for (int i = 0; i < n; ++i) err = std::max(err, std::fabs(muted[i] - a[size_t(i)]));
    check(err == 0, "a talker at 0 adds nothing");
    // The gain comes before OUTPUT VOLUME and the limiter: two loud talkers stay inside
    // full scale, and halving one lowers the limited peak.
    const auto loud1 = tone(n, 0.8f, 440.0f), loud2 = tone(n, 0.8f, 440.0f);
    float hot[n] = {}, eased[n] = {};
    VoiceEngine::mixTalker(hot, loud1.data(), n, 100); VoiceEngine::mixTalker(hot, loud2.data(), n, 100);
    VoiceEngine::mixTalker(eased, loud1.data(), n, 100); VoiceEngine::mixTalker(eased, loud2.data(), n, 50);
    VoiceEngine::finishMix(hot, n, 100); VoiceEngine::finishMix(eased, n, 100);
    float peakHot = 0, peakEased = 0;
    for (int i = 0; i < n; ++i) { peakHot = std::max(peakHot, std::fabs(hot[i])); peakEased = std::max(peakEased, std::fabs(eased[i])); }
    check(peakHot <= 1.0f && peakEased <= 1.0f && peakEased < peakHot && peakEased > 0.9f,
          "the limiter follows the per-player gain: " + std::to_string(peakHot) + " / " + std::to_string(peakEased));
    float half[n];
    std::copy(mix, mix + n, half);
    VoiceEngine::finishMix(half, n, 50);
    err = 0;
    for (int i = 0; i < n; ++i) err = std::max(err, std::fabs(half[i] - 0.5f * mix[i]));
    check(err < 1e-6f, "then OUTPUT VOLUME 50 halves the whole mix");
}

static void testSettings(const fs::path& dir) {
    std::printf("settings\n");
    const Settings d;
    check(!d.voiceEnabled && d.voiceOutputSameAsGame && d.voiceMicVolume == 100 && d.voiceOutputVolume == 100 &&
          d.voiceMicDevice.empty() && d.voiceOutputDevice.empty(), "voice chat is OFF by default");
    check(!d.voiceActivation && d.voiceActivationLevel == 15, "TALK MODE defaults to push to talk");
    check(d.voiceMicEnabled, "the MICROPHONE is on by default");
    const fs::path file = dir / "settings.ini";
    setSettingsPath(file.string());
    {   // a file with no voice rows: voice off.
        std::ofstream f(file); f << "masterVol = 200\n";
    }
    Settings old = loadSettings();
    check(!old.voiceEnabled && !old.voiceActivation && old.masterVol == 200,
          "a settings file without voice rows loads with voice off, push to talk");
    Settings s;
    s.voiceEnabled = true; s.voiceMicDevice = "USB Mic"; s.voiceOutputSameAsGame = false;
    s.voiceOutputDevice = "Headset"; s.voiceMicVolume = 35; s.voiceOutputVolume = 70;
    s.voiceActivation = true; s.voiceActivationLevel = 42;
    s.voiceMicEnabled = false;
    s.hotkeys["voice.ptt"] = "Ctrl+V";
    check(saveSettings(s), "save");
    const Settings back = loadSettings();
    check(back == s, "voice settings round-trip through the file");
    check(serializeSettings(Settings{}).find("voiceEnabled = 0") != std::string::npos, "the file says voiceEnabled = 0");
    { std::ofstream f(file); f << "voiceMicVolume = 900\nvoiceOutputVolume = -4\nvoiceActivationLevel = 300\n"; }
    const Settings clamp = loadSettings();
    check(clamp.voiceMicVolume == 400 && clamp.voiceOutputVolume == 0 && clamp.voiceActivationLevel == 100,
          "MIC GAIN clamps to 0..400 %, the output volume and the activation level to 0..100");
    { std::ofstream f(file); f << "voiceMicVolume = 250\n"; }
    check(loadSettings().voiceMicVolume == 250, "a MIC GAIN boost (250 %) is kept");
    Settings va = s;
    va.voiceActivation = true;
    check(!voiceDefaults(va).voiceActivation && voiceDefaults(va).voiceActivationLevel == 15,
          "the page's DEFAULTS go back to push to talk");
    setSettingsPath("");
}

// The disk driver reads the "microphone" from SDL_DISKAUDIOFILEIN and writes the output
// to SDL_DISKAUDIOFILE, in real time: a synthetic tone in, the played voice out.
static void testEngine(const fs::path& dir) {
    std::printf("engine (SDL disk audio)\n");
    const fs::path in = dir / "mic.raw", out = dir / "speaker.raw";
    {
        const auto pcm = tone(voice::kRate * 12, 0.5f);
        std::ofstream f(in, std::ios::binary);
        f.write(reinterpret_cast<const char*>(pcm.data()), std::streamsize(pcm.size() * sizeof(float)));
    }
    SDL_setenv("SDL_AUDIODRIVER", "disk", 1);
    SDL_setenv("SDL_DISKAUDIOFILEIN", in.string().c_str(), 1);
    SDL_setenv("SDL_DISKAUDIOFILE", out.string().c_str(), 1);
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { check(false, std::string("disk audio: ") + SDL_GetError()); return; }
    check(std::string(SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "") == "disk", "disk driver");

    VoiceEngine v;
    Settings s;
    std::vector<VoiceEngine::Frame> sent;
    auto send = [&](const VoiceEngine::Frame& f) { sent.push_back(f); };
    auto pump = [&](int ms, bool room, bool talk) {
        const uint64_t until = SDL_GetTicks64() + uint64_t(ms);
        while (SDL_GetTicks64() < until) { v.configure(s, ""); v.update(room, talk, send); SDL_Delay(5); }
    };

    // Off (the default): nothing opens, nothing is sent, whatever the key does.
    pump(100, true, true);
    check(sent.empty() && v.micState() == VoiceEngine::Device::Off && v.outState() == VoiceEngine::Device::Off,
          "voice off opens no device and sends nothing");

    s.voiceEnabled = true;
    pump(150, true, false);
    check(v.micState() == VoiceEngine::Device::Ok && v.outState() == VoiceEngine::Device::Ok, "on, in a room: both open");
    check(sent.empty(), "nothing is sent while the key is up");

    pump(1000, true, true);
    const size_t talked = sent.size();
    check(talked >= 30 && talked <= 70, "about 50 frames in a second of talking: " + std::to_string(talked));
    check(!sent.empty() && (sent.front().flags & net::kVoiceStart) && !(sent.back().flags & net::kVoiceEnd),
          "the first frame is a START");
    bool seqOk = true;
    for (size_t i = 1; i < sent.size(); ++i) seqOk = seqOk && sent[i].seq == uint16_t(sent[i - 1].seq + 1);
    check(seqOk, "sequence numbers count up");
    check(v.transmitting() && v.micLevel() > 0.4f && v.micLevel() < 0.6f,
          "mic level follows the tone: " + std::to_string(v.micLevel()));
    pump(60, true, false);
    check(sent.size() == talked + 1 && (sent.back().flags & net::kVoiceEnd) && sent.back().opus.empty() && !v.transmitting(),
          "releasing sends one empty END frame");

    // Mic volume 50 halves the level.
    s.voiceMicVolume = 50;
    sent.clear();
    pump(400, true, true);
    check(v.micLevel() > 0.2f && v.micLevel() < 0.3f, "mic volume 50 halves the level: " + std::to_string(v.micLevel()));
    pump(60, true, false);
    s.voiceMicVolume = 100;

    // Loop the frames back as a remote talker and measure what the "speaker" played.
    auto playback = [&](int volume) {
        s.voiceOutputVolume = volume;
        sent.clear();
        pump(800, true, true);
        pump(60, true, false);
        std::vector<VoiceEngine::Frame> frames = sent;
        // The disk driver truncates its file when the output opens and (on Windows) keeps
        // it unreadable until it closes: leave the room and come back for a fresh file,
        // play, then leave again before reading it.
        pump(30, false, false);
        pump(60, true, false);
        // Delivered at the talker's own pace, one frame every 20 ms, as the server relays it.
        for (size_t k = 0; k < frames.size(); ++k) {
            const auto& f = frames[k];
            VoiceEngine::Incoming i; i.talker = 7; i.slot = 2; i.seq = f.seq; i.flags = f.flags; i.opus = f.opus;
            if (k == 0) i.name = "Remote";
            v.push(i);
            if (k == 3) check(!v.speaking().empty() && v.speaking()[0].name == "Remote" && v.speaking()[0].slot == 2,
                              "the remote talker shows as speaking");
            pump(20, true, false);
        }
        pump(500, true, false);
        pump(30, false, false);   // closes the output, and with it the file
        std::vector<float> got;
        {
            std::ifstream f(out, std::ios::binary);
            float x;
            while (f.read(reinterpret_cast<char*>(&x), sizeof x)) got.push_back(x);
        }
        float peak = 0;
        for (float x : got) peak = std::max(peak, std::fabs(x));
        return peak;
    };
    const float full = playback(100);
    const float half = playback(50);
    check(full > 0.35f && full < 0.75f, "the remote talker plays at full volume: peak " + std::to_string(full));
    check(half > full * 0.35f && half < full * 0.65f, "output volume 50 halves it: " + std::to_string(half));
    // The talker's diplomacy VOLUME (slot 2) at 50 halves them too; another slot's does not.
    v.setPlayerVolume(5, 0);
    const float other = playback(100);
    v.setPlayerVolume(2, 50);
    const float player = playback(100);
    v.setPlayerVolume(2, 100); v.setPlayerVolume(5, 100);
    check(other > full * 0.8f && other < full * 1.25f, "another player's VOLUME leaves this talker alone: " + std::to_string(other));
    check(player > full * 0.35f && player < full * 0.65f, "this player's VOLUME 50 halves them: " + std::to_string(player));
    check(v.stats().decoded > 20, "frames were decoded: " + std::to_string(v.stats().decoded));

    // Out of the room: no devices, and no talkers.
    pump(60, false, true);
    check(v.micState() == VoiceEngine::Device::Off && v.outState() == VoiceEngine::Device::Off && v.speaking().empty(),
          "leaving the room closes both devices");
    // The VOICE CHAT screen's meter: the mic runs, nothing is sent even with the key down.
    sent.clear();
    v.setMonitoring(true);
    pump(300, false, true);
    check(v.micState() == VoiceEngine::Device::Ok && v.micLevel() > 0.4f && sent.empty(),
          "monitoring meters the mic without sending");
    v.setMonitoring(false);
    pump(30, false, false);
    check(v.micState() == VoiceEngine::Device::Off, "and closes it again");

    // Device loss: both devices vanish (closed behind the engine's back, as an unplug stops
    // them) while the key is held. The re-opened microphone usually has the SAME device id,
    // and it starts paused: the engine must start it again without a fresh key press.
    auto unplug = [] { for (SDL_AudioDeviceID id = 1; id < 64; ++id) SDL_CloseAudioDevice(id); };
    sent.clear();
    pump(200, true, true);
    check(!sent.empty(), "talking before the unplug");
    unplug();
    pump(2300, true, true);   // the re-open waits out kRetryMs
    const size_t beforeTail = sent.size();
    pump(500, true, true);
    check(v.micState() == VoiceEngine::Device::Lost && v.outState() == VoiceEngine::Device::Lost,
          "a lost device re-opens and says DEVICE LOST");
    check(sent.size() - beforeTail >= 15, "the re-opened mic sends with the key still held: " +
                                              std::to_string(sent.size() - beforeTail) + " frames in 0.5 s");
    pump(60, true, false);
    pump(30, false, false);
    // The same on the VOICE CHAT page: the meter comes back without reopening the page.
    v.setMonitoring(true);
    pump(200, false, false);
    unplug();
    pump(2400, false, false);
    s.voiceMicVolume = 50;    // a level that moves proves frames arrive (a stuck meter keeps 0.5)
    pump(300, false, false);
    check(v.micState() == VoiceEngine::Device::Lost && v.micLevel() > 0.2f && v.micLevel() < 0.3f,
          "the page's meter runs again after a lost mic re-opens: " + std::to_string(v.micLevel()));
    s.voiceMicVolume = 100;
    v.setMonitoring(false);
    pump(30, false, false);

    // A chosen device that is not there falls back to the system default, and says so.
    s.voiceMicDevice = "No Such Microphone";
    s.voiceOutputSameAsGame = false; s.voiceOutputDevice = "No Such Speaker";
    pump(100, true, false);
    check(v.micState() == VoiceEngine::Device::NotFound && v.outState() == VoiceEngine::Device::NotFound,
          "missing devices fall back to the default");
    check(VoiceEngine::note(v.micState()) == "NOT FOUND - USING DEFAULT", "...and the screen says so");
    sent.clear();
    pump(300, true, true);
    check(!sent.empty(), "still talking on the fallback device");
    pump(60, true, false);

    // Voice activation: no key, the tone (peak 0.5) opens the gate at level 15...
    s.voiceActivation = true; s.voiceActivationLevel = 15;
    sent.clear();
    pump(600, true, true);
    check(sent.size() >= 15 && (sent.front().flags & net::kVoiceStart) && v.transmitting(),
          "voice activation sends the tone by itself: " + std::to_string(sent.size()) + " frames");
    // ...and not at 60, where it only meters.
    s.voiceActivationLevel = 60;
    pump(500, true, true);   // the gate's 400 ms hold runs out and the transmission ENDs
    sent.clear();
    pump(600, true, true);
    check(sent.empty() && !v.transmitting() && v.micLevel() > 0.4f,
          "below the activation level nothing is sent: " + std::to_string(sent.size()) + " frames");
    // Not allowed (out of the room, voice off): never.
    s.voiceActivationLevel = 0;
    pump(600, true, false);
    check(!v.transmitting(), "voice activation sends nothing where talking is not allowed");
    s.voiceActivation = false; s.voiceActivationLevel = 15;

    // MICROPHONE off: the mic closes and nothing is sent, key held or voice activation;
    // the output stays open for the other players.
    s.voiceMicEnabled = false;
    sent.clear();
    pump(300, true, true);
    check(v.micState() == VoiceEngine::Device::Off && v.outState() != VoiceEngine::Device::Off && sent.empty() &&
          !v.transmitting(), "MICROPHONE off closes the mic, sends nothing, keeps the output");
    s.voiceActivation = true; s.voiceActivationLevel = 0;
    pump(300, true, true);
    check(sent.empty(), "...voice activation included");
    s.voiceActivation = false; s.voiceActivationLevel = 15;
    s.voiceMicEnabled = true;
    pump(300, true, true);
    check(!sent.empty(), "MICROPHONE on again: talking resumes");
    pump(60, true, false);
    // MIC GAIN above 100 % boosts: the 0.5 tone reads ~1.0 at 200 %.
    s.voiceMicVolume = 200;
    v.setMonitoring(true);
    pump(300, false, false);
    check(v.micLevel() > 0.9f, "MIC GAIN 200 % doubles the level: " + std::to_string(v.micLevel()));
    v.setMonitoring(false);
    s.voiceMicVolume = 100;
    pump(30, false, false);

    // Switched off mid-game: everything closes at once.
    s.voiceEnabled = false;
    sent.clear();
    pump(60, true, true);
    check(v.micState() == VoiceEngine::Device::Off && v.outState() == VoiceEngine::Device::Off && sent.empty(),
          "switching voice off closes everything");
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

// The VOICE CHAT page on a software renderer.
static void testPage() {
    std::printf("VOICE CHAT page\n");
    SDL_Window* win = SDL_CreateWindow("voice", 0, 0, 1280, 720, SDL_WINDOW_HIDDEN);
    SDL_Renderer* ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE) : nullptr;
    if (!ren) { check(false, std::string("renderer: ") + SDL_GetError()); return; }
    Settings s;
    int changes = 0, saves = 0, hotkeys = 0, monitorOn = 0, monitorOff = 0;
    VoicePageHooks hooks;
    hooks.micLevel = [] { return 0.5f; };
    hooks.micNote = [] { return std::string(); };
    hooks.outNote = [] { return std::string(); };
    hooks.monitor = [&](bool on) { (on ? monitorOn : monitorOff)++; };
    const int W = 1280, H = 720;
    {
        OptionsScreen page(ren, s, [&] { ++changes; }, [&] { ++saves; }, hooks, [&] { ++hotkeys; });
        check(page.page() == OptionsPage::Voice && monitorOn == 1, "the page opens and starts the meter");
        page.render(W, H);
        // TAK_VOICE_SHOT_DIR=<dir>: also write the page as PNGs, to look at.
        auto shot = [&](const char* name) {
            const char* dir = std::getenv("TAK_VOICE_SHOT_DIR");
            if (!dir) return;
            SDL_SetRenderDrawColor(ren, 40, 60, 40, 255); SDL_RenderClear(ren);
            page.render(W, H);
            std::vector<uint8_t> px(size_t(W) * H * 4);
            if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32, px.data(), W * 4) == 0)
                png::write((fs::path(dir) / name).string(), W, H, px);
        };
        auto click = [&](const char* label, float frac) {
            const SDL_FRect* r = page.rowRect(label);
            check(r != nullptr, std::string("row ") + label);
            if (!r) return;
            SDL_Event e{}; e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT;
            e.button.x = int(r->x + r->w * frac); e.button.y = int(r->y + r->h / 2);
            page.input(e, W, H);
            SDL_Event u{}; u.type = SDL_MOUSEBUTTONUP; u.button.button = SDL_BUTTON_LEFT;
            u.button.x = e.button.x; u.button.y = e.button.y;
            page.input(u, W, H);
            page.render(W, H);
        };
        const char* rows[] = {"TALK MODE", "MICROPHONE", "MIC DEVICE", "MIC GAIN", "MIC LEVEL", "OUTPUT DEVICE", "OUTPUT VOLUME", "PUSH TO TALK"};
        shot("voice-off.png");
        check(page.rowEnabled("VOICE CHAT"), "the switch is live while off");
        for (const char* r : rows) check(!page.rowEnabled(r), std::string(r) + " is greyed while voice chat is off");
        // Greyed rows ignore input.
        click("MIC GAIN", 0.25f);
        click("OUTPUT VOLUME", 0.25f);
        click("PUSH TO TALK", 0.98f);
        click("OUTPUT DEVICE", 0.9f);
        check(s.voiceMicVolume == 100 && s.voiceOutputVolume == 100 && hotkeys == 0 && changes == 0 && saves == 0,
              "greyed rows ignore clicks");
        // Switch on: every row comes alive.
        click("VOICE CHAT", 0.95f);
        check(s.voiceEnabled && changes == 1 && saves == 1, "the switch turns voice chat on, saved at once");
        for (const char* r : rows) check(page.rowEnabled(r), std::string(r) + " is live once on");
        click("MIC GAIN", 0.5f);
        check(s.voiceMicVolume >= 195 && s.voiceMicVolume <= 205, "MIC GAIN 0..400 %: " + std::to_string(s.voiceMicVolume));
        check(saves == 2, "a slider click saves once, on release: " + std::to_string(saves));
        click("OUTPUT VOLUME", 0.75f);
        check(s.voiceOutputVolume >= 73 && s.voiceOutputVolume <= 77, "OUTPUT VOLUME 0..100");
        click("PUSH TO TALK", 0.98f);
        check(hotkeys == 1, "PUSH TO TALK opens CONTROLS");
        check(!s.voiceActivation && !page.rowEnabled("ACTIVATION LEVEL"),
              "push to talk is the TALK MODE once on; ACTIVATION LEVEL is greyed");
        shot("voice-on.png");
        // MICROPHONE off greys the microphone's rows (not the output's); on again revives them.
        const int savesBefore = saves;
        click("MICROPHONE", 0.95f);
        check(!s.voiceMicEnabled && saves == savesBefore + 1, "the MICROPHONE switch turns the mic off, saved at once");
        for (const char* r : {"MIC DEVICE", "MIC GAIN", "MIC LEVEL"})
            check(!page.rowEnabled(r), std::string(r) + " is greyed while the microphone is off");
        check(page.rowEnabled("OUTPUT VOLUME") && page.rowEnabled("MICROPHONE"), "...the output and the switch stay live");
        click("MICROPHONE", 0.95f);
        check(s.voiceMicEnabled && page.rowEnabled("MIC GAIN"), "MICROPHONE on again");
        saves -= 2;   // the counts below are the rest of the page's own
        // TALK MODE: pick Voice Activation (index 1) from its list.
        click("TALK MODE", 0.9f);
        {
            const SDL_FRect item = page.dropItemRect(1);
            SDL_Event e{}; e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT;
            e.button.x = int(item.x + item.w / 2); e.button.y = int(item.y + item.h / 2);
            page.input(e, W, H);
            page.render(W, H);
        }
        check(s.voiceActivation && page.rowEnabled("ACTIVATION LEVEL") && !page.rowEnabled("PUSH TO TALK"),
              "Voice Activation: ACTIVATION LEVEL comes alive, the key row greys");
        check(saves == 4, "a TALK MODE pick is saved at once: " + std::to_string(saves));
        click("ACTIVATION LEVEL", 0.4f);
        check(s.voiceActivationLevel >= 38 && s.voiceActivationLevel <= 42,
              "ACTIVATION LEVEL 0..100: " + std::to_string(s.voiceActivationLevel));
        shot("voice-activation.png");
        s.voiceActivation = false;
        // VOICE OUTPUT: open the list, pick System Default (index 1), not same as game.
        const SDL_FRect* out = page.rowRect("OUTPUT DEVICE");
        float micChipX = -1;
        click("MIC DEVICE", 0.9f);
        micChipX = page.dropItemRect(0).x;
        click("MIC DEVICE", 0.9f);   // its own chip closes the list
        click("OUTPUT DEVICE", 0.9f);
        {
            const SDL_FRect item = page.dropItemRect(1);   // [0] SAME AS GAME, [1] System Default
            check(micChipX > 0 && std::fabs(item.x - micChipX) < 0.5f,
                  "the two device lists start on one column: " + std::to_string(micChipX) + " vs " + std::to_string(item.x));
            check(item.w > 0 && out != nullptr, "the OUTPUT DEVICE list opens");
            SDL_Event e{}; e.type = SDL_MOUSEBUTTONDOWN; e.button.button = SDL_BUTTON_LEFT;
            e.button.x = int(item.x + item.w / 2); e.button.y = int(item.y + item.h / 2);
            page.input(e, W, H);
            page.render(W, H);
        }
        check(!s.voiceOutputSameAsGame && s.voiceOutputDevice.empty(),
              "OUTPUT DEVICE can leave SAME AS GAME for the system default");
        check(s.masterVol == 256, "the page touches nothing but voice");
    }
    check(monitorOff == 1, "closing the page stops the meter");
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
}

int main() {
    SDL_SetMainReady();
    const fs::path dir = fs::temp_directory_path() / ("tak-voice-test-" + std::to_string(SDL_GetPerformanceCounter()));
    fs::create_directories(dir);
    testCodec();
    testJitter();
    testPtt();
    testMix();
    testSettings(dir);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { std::printf("SDL_Init: %s\n", SDL_GetError()); return 1; }
    testEngine(dir);
    testPage();
    SDL_Quit();
    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("voice_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

#include "client/voice.h"

#include "client/voicecodec.h"
#include "client/voicejitter.h"
#include "client/voiceptt.h"
#include "net/protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <map>

namespace tak {

static_assert(voice::kMaxPacket == net::kVoiceMaxOpus, "codec packet cap matches the wire cap");
static_assert(voice::kFlagStart == net::kVoiceStart && voice::kFlagEnd == net::kVoiceEnd, "voice flags match the wire");

namespace {

constexpr int kRetryMs = 2000;                 // how often a fallen-back device looks for the chosen one
constexpr int kMaxBacklog = 10;                // captured frames kept (200 ms); older audio is dropped
constexpr Uint32 kOutTargetBytes = 3 * voice::kFrame * sizeof(float);   // 60 ms queued ahead
constexpr int kSpeakingMs = 300;               // a talker counts as speaking this long after a frame
constexpr int kForgetMs = 10000;               // a silent talker's decoder is freed after this

bool devicePresent(const std::string& name, int iscapture) {
    const int n = SDL_GetNumAudioDevices(iscapture);
    for (int i = 0; i < n; ++i)
        if (const char* d = SDL_GetAudioDeviceName(i, iscapture); d && name == d) return true;
    return false;
}

// Soft limiter: unity below the knee, a smooth approach to full scale above it, so
// several talkers at once never clip hard.
float limit(float x) {
    constexpr float knee = 0.9f, room = 1.0f - knee;
    const float a = std::fabs(x);
    if (a <= knee) return x;
    const float y = knee + room * std::tanh((a - knee) / room);
    return x < 0 ? -y : y;
}

// One audio device (capture or output) and how it came to be open.
struct Endpoint {
    explicit Endpoint(int capture) : iscapture(capture) {}
    int iscapture = 0;
    SDL_AudioDeviceID dev = 0;
    std::string wanted;            // the chosen device ("" = system default)
    VoiceEngine::Device state = VoiceEngine::Device::Off;
    uint64_t retryAt = 0;
    // Counts open() calls. A re-open usually gets the SAME device id back (SDL hands out the
    // lowest free one, and open() closes the old device first), so this, not the id, says the
    // device is new -- a new capture device starts paused and has to be started again.
    unsigned opens = 0;

    void close() {
        if (dev) SDL_CloseAudioDevice(dev);
        dev = 0;
        state = VoiceEngine::Device::Off;
    }
    // Open `wanted`, or the system default when it is absent or will not open. `lost`
    // marks a re-open after the device went away (shown as DEVICE LOST).
    void open(bool lost, uint64_t now) {
        close();
        ++opens;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { state = VoiceEngine::Device::Failed; return; }
        SDL_AudioSpec want{}, got{};
        want.freq = voice::kRate;
        want.format = AUDIO_F32SYS;
        want.channels = 1;              // mono either way: SDL converts to the device's layout,
        want.samples = voice::kFrame;   // so ONE output volume covers stereo or surround alike
        want.callback = nullptr;        // queue API: no audio thread of ours
        const bool chosen = !wanted.empty();
        if (chosen && devicePresent(wanted, iscapture)) {
            dev = SDL_OpenAudioDevice(wanted.c_str(), iscapture, &want, &got, 0);
            if (dev) state = VoiceEngine::Device::Ok;
        }
        if (!dev) {
            // SDL_GetNumAudioDevices can be 0 or -1 on backends that only offer a default.
            dev = SDL_OpenAudioDevice(nullptr, iscapture, &want, &got, 0);
            if (!dev) state = VoiceEngine::Device::Failed;
            else if (!chosen) state = lost ? VoiceEngine::Device::Lost : VoiceEngine::Device::Ok;
            else state = lost ? VoiceEngine::Device::Lost : VoiceEngine::Device::NotFound;
        }
        if (!dev)
            std::fprintf(stderr, "voice: cannot open the %s: %s\n", iscapture ? "microphone" : "voice output",
                         SDL_GetError());
        else if (state != VoiceEngine::Device::Ok)
            std::fprintf(stderr, "voice: %s '%s' %s -- using the system default\n",
                         iscapture ? "microphone" : "voice output", wanted.c_str(),
                         lost ? "was lost" : "is not present");
        retryAt = now + kRetryMs;
        if (dev && !iscapture) SDL_PauseAudioDevice(dev, 0);   // output plays (silence) from the start
    }
    // Device lost (unplugged) or a fallback that can now return to the chosen device.
    void maintain(uint64_t now) {
        if (!dev) {
            if (state == VoiceEngine::Device::Failed && now >= retryAt) open(false, now);
            return;
        }
        // Disconnected. At most one re-open per kRetryMs, so a backend whose default is
        // gone as well cannot turn this into a re-open (and a log line) every frame.
        if (SDL_GetAudioDeviceStatus(dev) == SDL_AUDIO_STOPPED && now >= retryAt) {
            std::fprintf(stderr, "voice: %s disconnected\n", iscapture ? "microphone" : "voice output");
            open(true, now);
            return;
        }
        if ((state == VoiceEngine::Device::NotFound || state == VoiceEngine::Device::Lost) &&
            !wanted.empty() && now >= retryAt) {
            retryAt = now + kRetryMs;
            if (devicePresent(wanted, iscapture)) open(false, now);   // it came back
        }
    }
};

}  // namespace

struct VoiceEngine::Impl {
    struct Config {
        bool enabled = false;
        std::string mic, out;          // effective names ("" = system default)
        int micVol = 100, outVol = 100;   // MIC GAIN 0..400 %, OUTPUT VOLUME 0..100
        bool micOn = true;                // MICROPHONE: off = no capture, nothing sent
        bool activation = false;       // TALK MODE voice activation (else push to talk)
        int activationLevel = 15;      // 0..100 of the level meter
        bool operator==(const Config&) const = default;
    } cfg;
    bool configured = false;
    bool monitoring = false;
    bool inRoom = false;

    Endpoint cap{1}, out{0};
    bool capRunning = false;
    std::vector<float> pending;        // captured samples not yet framed
    voice::Encoder encoder;
    uint16_t seq = 0;
    bool sending = false;              // a transmission is open (START sent, END not yet)
    voice::VoiceGate gate;             // voice activation
    std::vector<std::array<float, voice::kFrame>> preroll;   // the frames just before the gate opened
    float level = 0;

    struct Talker {
        voice::JitterBuffer jb;
        std::unique_ptr<voice::Decoder> dec;
        std::string name;
        uint8_t slot = 0xFF;
        uint64_t lastMs = 0;
    };
    std::map<uint32_t, Talker> talkers;
    std::array<uint8_t, 8> playerVol{100, 100, 100, 100, 100, 100, 100, 100};   // diplomacy VOLUME, by slot
    Stats stats;

    uint64_t now() const { return SDL_GetTicks64(); }

    void closeAll() {
        cap.close(); out.close();
        capRunning = false; pending.clear(); sending = false; level = 0;
        gate.reset(); preroll.clear();
        talkers.clear();
    }
};

VoiceEngine::VoiceEngine() : p_(std::make_unique<Impl>()) {}
VoiceEngine::~VoiceEngine() { p_->closeAll(); }

void VoiceEngine::configure(const Settings& s, const std::string& gameOutput) {
    Impl::Config c;
    c.enabled = s.voiceEnabled;
    c.mic = s.voiceMicDevice;
    c.out = s.voiceOutputSameAsGame ? gameOutput : s.voiceOutputDevice;
    c.micVol = std::clamp(s.voiceMicVolume, 0, 400);
    c.micOn = s.voiceMicEnabled;
    c.outVol = std::clamp(s.voiceOutputVolume, 0, 100);
    c.activation = s.voiceActivation;
    c.activationLevel = std::clamp(s.voiceActivationLevel, 0, 100);
    if (p_->configured && c == p_->cfg) return;
    const bool first = !p_->configured;
    const bool micChanged = first || c.mic != p_->cfg.mic, outChanged = first || c.out != p_->cfg.out;
    const bool switched = first || c.enabled != p_->cfg.enabled;
    p_->cfg = c;
    p_->configured = true;
    p_->cap.wanted = c.mic;
    p_->out.wanted = c.out;
    if (!c.enabled) { if (switched) p_->closeAll(); return; }
    // A different device: re-open just that endpoint, if it is open (update() opens it
    // when it is next needed otherwise). A volume alone changes nothing here.
    const uint64_t now = p_->now();
    if (micChanged && p_->cap.state != Device::Off) { p_->cap.open(false, now); p_->capRunning = false; p_->pending.clear(); }
    if (outChanged && p_->out.state != Device::Off) p_->out.open(false, now);
}

void VoiceEngine::setMonitoring(bool on) { p_->monitoring = on; }

void VoiceEngine::update(bool inRoom, bool transmit, const Sender& send) {
    Impl& p = *p_;
    const uint64_t now = p.now();
    if (inRoom != p.inRoom) { p.inRoom = inRoom; if (!inRoom) p.talkers.clear(); }
    const bool on = p.cfg.enabled;
    // MICROPHONE off: the capture device is closed (the system's mic-in-use light goes
    // out) and nothing is sent; others are still heard.
    const bool wantCap = on && p.cfg.micOn && (inRoom || p.monitoring);
    const bool wantOut = on && inRoom;
    transmit = transmit && on && p.cfg.micOn && inRoom;

    // ---- devices: open what is needed, close what is not, follow loss and return ----
    if (wantCap && !p.cap.dev && p.cap.state == Device::Off) { p.cap.open(false, now); p.capRunning = false; }
    if (!wantCap && p.cap.state != Device::Off) { p.cap.close(); p.capRunning = false; p.pending.clear(); p.level = 0; }
    if (wantOut && !p.out.dev && p.out.state == Device::Off) p.out.open(false, now);
    if (!wantOut && p.out.state != Device::Off) p.out.close();
    if (wantCap) { const unsigned was = p.cap.opens; p.cap.maintain(now); if (p.cap.opens != was) p.capRunning = false; }
    if (wantOut) p.out.maintain(now);

    // ---- microphone: record only while talking or metering ----
    // In voice activation `transmit` means talking is allowed: the microphone then runs
    // for the whole time and the gate decides which frames go out.
    const bool activation = p.cfg.activation;
    const bool record = p.cap.dev && (transmit || p.monitoring);
    if (p.cap.dev && record != p.capRunning) {
        if (record) { SDL_ClearQueuedAudio(p.cap.dev); p.pending.clear(); }
        SDL_PauseAudioDevice(p.cap.dev, record ? 0 : 1);
        p.capRunning = record;
    }
    if (p.capRunning) {
        float buf[4096];
        Uint32 got;
        while ((got = SDL_DequeueAudio(p.cap.dev, buf, sizeof buf)) > 0)
            p.pending.insert(p.pending.end(), buf, buf + got / sizeof(float));
    }
    // Keep latency bounded after a stalled frame: the newest 200 ms, whole frames.
    if (p.pending.size() > size_t(kMaxBacklog * voice::kFrame)) {
        const size_t drop = (p.pending.size() / voice::kFrame - kMaxBacklog) * voice::kFrame;
        p.pending.erase(p.pending.begin(), p.pending.begin() + long(drop));
        p.stats.dropped += drop / voice::kFrame;
    }
    if (!p.capRunning) p.level *= 0.85f;
    if (!transmit || !activation) { p.gate.reset(); p.preroll.clear(); }
    auto sendEnd = [&] {
        Frame end;
        end.seq = p.seq++;
        end.flags = net::kVoiceEnd;
        if (send) send(end);
        p.sending = false;
    };
    auto sendFrame = [&](const float* frame) {
        if (!p.sending) p.encoder.reset();
        Frame f;
        f.opus = p.encoder.encode(frame);
        if (f.opus.empty()) return;
        f.seq = p.seq++;
        f.flags = p.sending ? 0 : net::kVoiceStart;
        p.sending = true;
        send(f);
        ++p.stats.sent;
    };
    const float gain = float(p.cfg.micVol) / 100.0f;
    const float threshold = float(p.cfg.activationLevel) / 100.0f;
    while (p.pending.size() >= size_t(voice::kFrame)) {
        std::array<float, voice::kFrame> frame;
        float peak = 0;
        for (int i = 0; i < voice::kFrame; ++i) {
            frame[size_t(i)] = std::clamp(p.pending[size_t(i)] * gain, -1.0f, 1.0f);
            peak = std::max(peak, std::fabs(frame[size_t(i)]));
        }
        p.pending.erase(p.pending.begin(), p.pending.begin() + voice::kFrame);
        p.level = std::max(peak, p.level * 0.85f);
        if (!transmit || !send) continue;
        if (activation && !p.gate.frame(peak, threshold)) {
            if (p.sending) sendEnd();   // quiet for the hold time: the transmission ends
            p.preroll.push_back(frame);
            if (p.preroll.size() > size_t(voice::VoiceGate::kPreFrames)) p.preroll.erase(p.preroll.begin());
            continue;
        }
        for (const auto& pre : p.preroll) sendFrame(pre.data());
        p.preroll.clear();
        sendFrame(frame.data());
    }
    if (p.sending && !transmit) sendEnd();   // released (or no longer allowed): close the transmission

    // ---- output: mix every talker, keep ~60 ms queued ahead of the device ----
    if (p.out.dev) {
        for (int guard = 0; guard < 8 && SDL_GetQueuedAudioSize(p.out.dev) < kOutTargetBytes; ++guard) {
            float mix[voice::kFrame] = {};
            float pcm[voice::kFrame];
            bool any = false;
            for (auto& [id, t] : p.talkers) {
                (void)id;
                const voice::Playout play = t.jb.next();
                if (play.kind == voice::Playout::Idle) continue;
                if (!t.dec) t.dec = std::make_unique<voice::Decoder>();
                bool ok = false;
                switch (play.kind) {
                    case voice::Playout::Decode: ok = t.dec->decode(play.data->data(), play.data->size(), pcm); ++p.stats.decoded; break;
                    case voice::Playout::Fec: ok = t.dec->decodeFec(play.data->data(), play.data->size(), pcm); ++p.stats.fec; break;
                    case voice::Playout::Conceal: ok = t.dec->conceal(pcm); ++p.stats.concealed; break;
                    case voice::Playout::Idle: break;
                }
                if (!ok) continue;
                mixTalker(mix, pcm, voice::kFrame, t.slot < 8 ? p.playerVol[t.slot] : 100);
                any = true;
            }
            if (!any) break;   // nobody talking: let the device drain to silence
            finishMix(mix, voice::kFrame, p.cfg.outVol);
            SDL_QueueAudio(p.out.dev, mix, sizeof mix);
        }
    }
    for (auto it = p.talkers.begin(); it != p.talkers.end();)
        if (it->second.jb.idle() && now - it->second.lastMs > uint64_t(kForgetMs)) it = p.talkers.erase(it);
        else ++it;
}

void VoiceEngine::push(Incoming in) {
    Impl& p = *p_;
    if (!p.cfg.enabled || !p.inRoom) return;
    if (p.talkers.size() >= 32 && !p.talkers.count(in.talker)) return;   // more than any room holds
    auto& t = p.talkers[in.talker];
    if (!in.name.empty()) t.name = in.name;
    t.slot = in.slot;
    t.lastMs = p.now();
    if ((in.flags & net::kVoiceStart) && t.dec) t.dec->reset();
    t.jb.push(in.seq, in.flags, std::move(in.opus), t.lastMs);
    ++p.stats.received;
}

void VoiceEngine::clearTalkers() { p_->talkers.clear(); }

void VoiceEngine::setPlayerVolume(int slot, int percent) {
    if (slot >= 0 && slot < 8) p_->playerVol[size_t(slot)] = uint8_t(std::clamp(percent, 0, 100));
}

int VoiceEngine::playerVolume(int slot) const { return slot >= 0 && slot < 8 ? p_->playerVol[size_t(slot)] : 100; }

void VoiceEngine::mixTalker(float* mix, const float* pcm, int n, int percent) {
    const float g = float(std::clamp(percent, 0, 100)) / 100.0f;
    for (int i = 0; i < n; ++i) mix[i] += pcm[i] * g;
}

void VoiceEngine::finishMix(float* mix, int n, int outPercent) {
    const float vol = float(std::clamp(outPercent, 0, 100)) / 100.0f;
    for (int i = 0; i < n; ++i) mix[i] = limit(mix[i] * vol);
}

bool VoiceEngine::enabled() const { return p_->cfg.enabled; }
bool VoiceEngine::transmitting() const { return p_->sending; }

std::vector<VoiceEngine::Talker> VoiceEngine::speaking() const {
    std::vector<Talker> out;
    const uint64_t now = p_->now();
    for (const auto& [id, t] : p_->talkers)
        if (t.jb.playing() || now - t.lastMs < uint64_t(kSpeakingMs)) out.push_back({id, t.slot, t.name});
    return out;
}

float VoiceEngine::micLevel() const { return p_->level; }
VoiceEngine::Device VoiceEngine::micState() const { return p_->cap.state; }
VoiceEngine::Device VoiceEngine::outState() const { return p_->out.state; }
const VoiceEngine::Stats& VoiceEngine::stats() const { return p_->stats; }

std::string VoiceEngine::note(Device d) {
    switch (d) {
        case Device::NotFound: return "NOT FOUND - USING DEFAULT";
        case Device::Lost: return "DEVICE LOST - USING DEFAULT";
        case Device::Failed: return "UNAVAILABLE";
        default: return "";
    }
}

}  // namespace tak

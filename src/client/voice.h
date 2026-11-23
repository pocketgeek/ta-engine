#pragma once

// Voice chat, client side: the microphone, Opus, one jitter buffer per talker and the
// voice output device (docs/multiplayer-design.md, "Voice chat").
//
// PRESENTATION / IO ONLY. Nothing here reaches the simulation, World::stateHash, the
// lockstep command stream or a replay; the frames travel on their own message types
// (net::Msg::VoiceUp / VoiceDown), relayed by the server. It owns no MpClient either:
// outbound frames leave through the Sender the host passes to update(), inbound ones
// arrive through push(). Every method runs on the main thread, and the audio devices
// use SDL's queue API (no audio callback), so there is nothing to lock.
//
// Devices open only while needed: the microphone while in a voice room or while the
// VOICE CHAT screen shows its level meter (and it records only while talking or
// metering, or all the time in a voice room under voice activation), the output while
// in a voice room. With voice chat off nothing opens at all, and the headless harnesses
// never construct an engine.

#include <SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "client/settings.h"

namespace tak {

class VoiceEngine {
public:
    // One outbound frame (seq and flags as on the wire; opus empty for an END marker).
    struct Frame { uint16_t seq = 0; uint8_t flags = 0; std::vector<uint8_t> opus; };
    // One frame from another player, as MpClient::takeVoice() delivers it.
    struct Incoming {
        uint32_t talker = 0;      // the talker's connection id on the server
        uint8_t slot = 0xFF;      // their player slot (0xFF = spectator)
        uint16_t seq = 0;
        uint8_t flags = 0;
        std::string name;         // present on some frames only (empty = keep the last)
        std::vector<uint8_t> opus;
    };
    using Sender = std::function<void(const Frame&)>;
    // A device's state, for the VOICE CHAT screen.
    enum class Device {
        Off,        // not open (voice off, or not needed right now)
        Ok,         // the chosen device (or the system default when that was chosen)
        NotFound,   // the chosen device is not present: using the system default
        Lost,       // the chosen device went away while open: using the system default
        Failed,     // nothing could be opened
    };
    struct Talker { uint32_t id = 0; uint8_t slot = 0xFF; std::string name; };
    struct Stats { uint64_t sent = 0, received = 0, decoded = 0, fec = 0, concealed = 0, dropped = 0; };

    VoiceEngine();
    ~VoiceEngine();
    VoiceEngine(const VoiceEngine&) = delete;
    VoiceEngine& operator=(const VoiceEngine&) = delete;

    // Apply the settings (call every frame; cheap when nothing changed). gameOutput is
    // the device the game's sound uses (tak::currentAudioDevice(), "" = system default),
    // followed when voice output is SAME AS GAME.
    void configure(const Settings& s, const std::string& gameOutput);
    // The VOICE CHAT screen is open: keep the microphone running for its level meter.
    void setMonitoring(bool on);
    // Once per frame. inRoom: connected to a room that carries voice. transmit: push to
    // talk is held (already gated by the host); in voice activation (TALK MODE), that
    // talking is allowed at all -- the level then opens and closes the transmission.
    // Captured frames go out through `send`.
    void update(bool inRoom, bool transmit, const Sender& send);
    // A frame from another player.
    void push(Incoming in);
    // Forget every talker (leaving a room).
    void clearTalkers();
    // One player's VOLUME from the diplomacy screen, by player slot: 0..100, 100 (the
    // default) plays them as received. Applied to that talker's decoded voice before the
    // mix, so it holds whether or not they are talking now. Local only: it changes what
    // this machine plays and nothing else. A spectator (no slot) always plays at 100.
    void setPlayerVolume(int slot, int percent);
    int playerVolume(int slot) const;

    // The output mix exactly as update() makes it, for the tests: add one talker's frame
    // at its VOLUME, then finish the summed frame with OUTPUT VOLUME and the soft limiter.
    static void mixTalker(float* mix, const float* pcm, int n, int percent);
    static void finishMix(float* mix, int n, int outPercent);

    bool enabled() const;
    bool transmitting() const;               // sending right now
    std::vector<Talker> speaking() const;    // others heard in the last moment
    float micLevel() const;                  // 0..1 peak, after MIC GAIN, decaying
    Device micState() const;
    Device outState() const;
    // The note the VOICE CHAT screen shows beside a device ("" when there is nothing to say).
    static std::string note(Device d);
    const Stats& stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace tak

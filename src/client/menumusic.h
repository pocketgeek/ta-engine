#pragma once

// A tiny looping background-music player for the front-end. main() keeps ONE of
// these alive across the menu -> lobby transition so the track doesn't restart at
// the handoff, and streams it at the game's music level so it matches once in game.
// Both the menu (MainMenu) and main()'s game loop poll it; GameView suppresses its
// own lobby music while this is playing (see externalLobbyMusic_).

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "hpi/hpi.h"
#include "client/options.h"   // ta::openAudioDevice (routes to the chosen device)

namespace ta {

class MenuMusic {
public:
    ~MenuMusic() { stop(); }

    // Play music/track<n>.wav on a loop. Idempotent: if this track is already
    // playing, does nothing (so it does NOT restart across menu -> lobby).
    void start(const hpi::Vfs& vfs, int track) {
        if (dev_ && track_ == track) return;
        stop();
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        std::vector<uint8_t> raw;
        try { raw = vfs.read("music/track" + std::to_string(track) + ".wav"); } catch (...) { return; }
        SDL_RWops* rw = raw.empty() ? nullptr : SDL_RWFromConstMem(raw.data(), int(raw.size()));
        SDL_AudioSpec wav{}; Uint8* buf = nullptr; Uint32 len = 0;
        if (!rw || !SDL_LoadWAV_RW(rw, 1, &wav, &buf, &len)) return;
        // Keep the unscaled source so the BGM/master volume can be re-applied live.
        src_.assign(buf, buf + len);
        fmt_ = wav.format;
        SDL_FreeWAV(buf);
        // Callback-driven, NOT queue-driven: the audio thread pulls chunks itself, so a
        // long synchronous load on the main thread (map + texture atlases when a
        // game starts) can't starve the queue and stutter the BGM the way a poll()
        // did. Matches SoundBank's model.
        SDL_AudioSpec want = wav, have{};
        want.samples = 2048;             // ~46ms device buffer at 44.1kHz
        want.callback = &MenuMusic::mixThunk;
        want.userdata = this;
        pos_ = 0;                        // set before the device unpauses (callback reads it)
        openWant_ = want;                // kept so reopen() can re-open on a new device
        dev_ = ta::openAudioDevice(0, &want, &have, 0);   // flags 0 => have == want
        if (!dev_) { src_.clear(); return; }
        silence_ = have.silence;
        track_ = track;
        SDL_PauseAudioDevice(dev_, 0);   // the callback starts pulling
    }

    // Re-open the current track on the now-current output device (Options device switch),
    // continuing from the same play position so the music doesn't restart. No-op if nothing
    // is playing -- the next start() then simply opens on the new device.
    void reopen() {
        if (!dev_ || src_.empty()) return;
        SDL_CloseAudioDevice(dev_);      // stops + joins the callback thread; pos_ is preserved
        dev_ = 0;
        SDL_AudioSpec have{};
        dev_ = ta::openAudioDevice(0, &openWant_, &have, 0);
        if (!dev_) { src_.clear(); track_ = -1; return; }
        silence_ = have.silence;
        SDL_PauseAudioDevice(dev_, 0);
    }

    // BGM + master volume on the SoundBank 0..256 scale. Applies live WITHOUT
    // restarting: the callback simply reads the new gain, so dragging a volume slider
    // doesn't jump the track back to its start.
    void setVolume(int master, int bgm) {
        master_.store(master, std::memory_order_relaxed);
        bgm_.store(bgm, std::memory_order_relaxed);
    }

    // Callback-driven now, so there is nothing to pump each frame. Kept so the existing
    // per-frame call sites stay valid.
    void poll() {}

    void stop() {
        // CloseAudioDevice stops + joins the callback thread, so it's safe to clear the
        // source it reads only after this returns.
        if (dev_) { SDL_CloseAudioDevice(dev_); dev_ = 0; }
        src_.clear();
        pos_ = 0;
        track_ = -1;
    }

    bool playing() const { return dev_ != 0; }

private:
    static void SDLCALL mixThunk(void* userdata, Uint8* stream, int len) {
        static_cast<MenuMusic*>(userdata)->fill(stream, len);
    }

    // Fill one device buffer by mixing the volume-scaled source at pos_, wrapping at the
    // end for a seamless loop. Runs on the AUDIO thread. Offsets stay frame-aligned
    // because the source length and every mix step are.
    void fill(Uint8* stream, int len) {
        if (src_.empty()) { SDL_memset(stream, silence_, size_t(len)); return; }
        int vol = 128 * bgm_.load(std::memory_order_relaxed) / 256
                      * master_.load(std::memory_order_relaxed) / 256;   // SDL_MIX_MAXVOLUME=128
        SDL_memset(stream, silence_, size_t(len));   // silence -> MixAudioFormat scales onto it
        size_t need = size_t(len);
        Uint8* out = stream;
        while (need > 0) {
            size_t avail = src_.size() - pos_;
            size_t take = std::min(need, avail);
            SDL_MixAudioFormat(out, src_.data() + pos_, fmt_, Uint32(take), vol);
            pos_ += take;
            if (pos_ >= src_.size()) pos_ = 0;       // loop
            out += take;
            need -= take;
        }
    }

    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec openWant_{};    // spec used to open dev_, kept so reopen() can switch devices
    std::vector<uint8_t> src_;    // unscaled source, kept so volume can re-apply live
    SDL_AudioFormat fmt_ = 0;
    Uint8 silence_ = 0;          // device silence byte (have.silence)
    size_t pos_ = 0;             // play cursor into src_ (bytes, frame-aligned); audio thread only
    std::atomic<int> master_{256}, bgm_{128};   // read by the audio thread, set from the main thread
    int track_ = -1;
};

}  // namespace ta

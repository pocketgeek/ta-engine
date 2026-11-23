#pragma once

// A tiny looping background-music player for the front-end. main() keeps ONE of
// these alive across the menu -> lobby transition so the track doesn't restart at
// the handoff, and streams it at the game's music level so it matches once in game.
// Both the menu (MainMenu) and main()'s game loop poll it; GameView suppresses its
// own lobby music while this is playing (see externalLobbyMusic_).

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hpi/hpi.h"
#include "client/musicfile.h"   // tak::music::loadTrack (CUSTOM MUSIC, MP3/FLAC/Ogg)
#include "util/audiolayout.h"
#include "client/options.h"   // tak::openAudioDevice / tak::detectOutputChannels

namespace tak {

class MenuMusic {
public:
    ~MenuMusic() { stop(); }
    MenuMusic() = default;
    MenuMusic(const MenuMusic&) = delete;
    MenuMusic& operator=(const MenuMusic&) = delete;

    // The stream mixer accepts 44100 Hz signed PCM in any layout (it folds it to
    // stereo). Tap the volume-scaled device buffer, resampled from the track's rate.
    void setAudioTap(void* context, void (*tap)(void*, const int16_t*, int, int)) {
        if (dev_) SDL_LockAudioDevice(dev_);
        tapContext_ = context; tap_ = tap;
        resetTapConverter();
        if (dev_) SDL_UnlockAudioDevice(dev_);
    }

    // Play original track n on a loop. The front-end always plays the original: a
    // CUSTOM MUSIC file for the track plays only in games (tak::music::kMenuTrack).
    // Idempotent: if this track is already playing, does nothing (so it does NOT restart
    // across menu -> lobby).
    void start(const hpi::Vfs& vfs, int track) {
        const std::string file;
        if (dev_ && track_ == track && file_ == file) return;
        stop();
        vfs_ = &vfs;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        auto pcm = music::loadTrack(vfs, track, audio::kMixRate, false);
        if (!pcm) return;
        src_ = std::move(pcm->samples);
        rate_ = pcm->rate;
        srcChannels_ = pcm->channels;
        file_ = file;
        pos_ = 0;                        // set before the device unpauses (callback reads it)
        if (!openDevice()) { src_.clear(); return; }
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
        playing_.store(false);
        if (!openDevice()) { src_.clear(); track_ = -1; return; }
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
        playing_.store(false);
        if (tapConverter_) { SDL_FreeAudioStream(tapConverter_); tapConverter_ = nullptr; }
        src_.clear();
        pos_ = 0;
        track_ = -1;
    }

    bool playing() const { return playing_.load(); }

    // The AUDIO page changed CUSTOM MUSIC: nothing to do, the front-end plays originals.
    void refresh() {}

private:
    friend struct MenuMusicTestAccess;
    // The track as S16 at its own rate (only a rate above 44100 is converted down)
    // and its own channel count, so a mod's 44.1 kHz stereo or 5.1 replacement keeps
    // its quality. fill() lays it onto the device's speakers.
    bool decode(const std::vector<uint8_t>& raw) {
        auto pcm = music::decode(raw, audio::kMixRate);
        if (!pcm) return false;
        src_ = std::move(pcm->samples);
        rate_ = pcm->rate; srcChannels_ = pcm->channels;
        return !src_.empty();
    }
    // Open on the current device at the track's rate, in the device's own speaker
    // layout (tak::detectOutputChannels, as SoundBank uses): S16, flags 0, so the
    // device takes exactly what fill() writes and SDL converts nothing more.
    bool openDevice() {
        SDL_AudioSpec want{}, have{};
        want.freq = rate_;
        want.format = AUDIO_S16SYS;
        want.channels = Uint8(std::clamp(tak::detectOutputChannels(), 1, 8));
        // Callback-driven, NOT queue-driven: the audio thread pulls chunks itself, so a
        // long synchronous load on the main thread (map + texture atlases when a
        // game starts) can't starve the queue and stutter the BGM the way a poll()
        // did. Matches SoundBank's model.
        want.samples = 2048;             // ~46ms device buffer at 44.1kHz
        want.callback = &MenuMusic::mixThunk;
        want.userdata = this;
        openWant_ = want;
        dev_ = tak::openAudioDevice(0, &want, &have, 0);   // flags 0 => have == want
        if (!dev_) return false;
        setLayout(want.channels);
        resetTapConverter();
        playing_.store(true);
        return true;
    }
    void setLayout(int channels) {
        channels_ = channels;
        matrix_ = audio::musicMatrix(srcChannels_, channels_);
    }
    void resetTapConverter() {
        if (tapConverter_) SDL_FreeAudioStream(tapConverter_);
        tapConverter_ = tap_ && !src_.empty()
            ? SDL_NewAudioStream(AUDIO_S16SYS, Uint8(channels_), rate_,
                                 AUDIO_S16SYS, Uint8(channels_), audio::kMixRate) : nullptr;
    }
    static void SDLCALL mixThunk(void* userdata, Uint8* stream, int len) {
        static_cast<MenuMusic*>(userdata)->fill(stream, len);
    }

    // Fill one device buffer from the volume-scaled source at pos_, laid onto the
    // device's speakers (audio::musicMatrix: the game's stereo music bed, or a
    // multichannel track channel for channel), wrapping at the end for a seamless
    // loop. Runs on the AUDIO thread.
    void fill(Uint8* stream, int len) {
        SDL_memset(stream, 0, size_t(len));
        const size_t sc = size_t(srcChannels_), frames = size_t(len) / 2 / size_t(channels_);
        if (src_.empty() || frames == 0) return;
        const float vol = float(bgm_.load(std::memory_order_relaxed)) / 256.f
                        * float(master_.load(std::memory_order_relaxed)) / 256.f;
        auto* out = reinterpret_cast<int16_t*>(stream);
        const size_t total = src_.size() / sc;
        for (size_t f = 0; f < frames; ++f) {
            const int16_t* in = src_.data() + pos_ * sc;
            for (int o = 0; o < channels_; ++o) {
                float v = 0;
                for (size_t k = 0; k < sc; ++k) v += matrix_[size_t(o)][k] * float(in[k]);
                out[f * size_t(channels_) + size_t(o)] = int16_t(std::clamp(v * vol, -32768.f, 32767.f));
            }
            if (++pos_ >= total) pos_ = 0;   // loop
        }
        if (tap_ && tapConverter_ && SDL_AudioStreamPut(tapConverter_, stream, len) == 0) {
            std::array<int16_t, 4096> pcm;
            const int frameBytes = channels_ * int(sizeof(int16_t));   // whole frames only (5.1: 12)
            const int want = int(sizeof pcm) / frameBytes * frameBytes;
            int bytes;
            while ((bytes = SDL_AudioStreamGet(tapConverter_, pcm.data(), want)) > 0)
                tap_(tapContext_, pcm.data(), bytes / (channels_ * int(sizeof(int16_t))), channels_);
        }
    }

    void* tapContext_ = nullptr;
    void (*tap_)(void*, const int16_t*, int, int) = nullptr;
    SDL_AudioStream* tapConverter_ = nullptr;
    std::atomic<bool> playing_{false}; // also read by the game's streaming audio callback
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioSpec openWant_{};    // spec dev_ was opened with
    std::vector<int16_t> src_;    // unscaled source, kept so volume can re-apply live
    int rate_ = audio::kMixRate, srcChannels_ = 2;   // the track's own rate and channels
    int channels_ = 2;            // the device's speaker count
    audio::Matrix matrix_ = audio::musicMatrix(2, 2);   // src_ -> device speakers
    size_t pos_ = 0;             // play cursor into src_ (frames); audio thread only
    std::atomic<int> master_{256}, bgm_{128};   // read by the audio thread, set from the main thread
    int track_ = -1;
    const hpi::Vfs* vfs_ = nullptr;   // where track_ came from (refresh())
    std::string file_;                // the custom file playing for track_, "" = the original
};

}  // namespace tak

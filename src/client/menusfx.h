#pragma once

// Plays the front-end's button clicks (client/menuclick.h): one queue-driven device,
// separate from the shared MenuMusic device so a click mixes over the music. The
// title menu, the campaign picker it hosts and the result screen each own one.

#include <SDL.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "client/menuclick.h"
#include "client/options.h"    // tak::openAudioDevice (routes to the chosen device)
#include "client/settings.h"
#include "hpi/hpi.h"

namespace tak {

class MenuSfx {
public:
    explicit MenuSfx(const hpi::Vfs& vfs) : vfs_(vfs) {}
    ~MenuSfx() { if (dev_) SDL_CloseAudioDevice(dev_); }
    MenuSfx(const MenuSfx&) = delete;
    MenuSfx& operator=(const MenuSfx&) = delete;

    // The MASTER / SOUND EFFECTS levels the clicks honour (null: full).
    const Settings* settings = nullptr;

    // Read sounds/<name> and keep it as device PCM; opens the device on first use.
    void load(const std::string& name) {
        if (name.empty() || pcm_.count(name)) return;
        auto& pcm = pcm_[name];   // a missing WAV stays empty: not retried, plays nothing
        std::vector<uint8_t> raw;
        try { raw = vfs_.read("sounds/" + name); } catch (...) { return; }
        if (raw.empty()) return;
        SDL_RWops* rw = SDL_RWFromConstMem(raw.data(), int(raw.size()));
        SDL_AudioSpec spec{}; Uint8* buf = nullptr; Uint32 len = 0;
        if (!rw || !SDL_LoadWAV_RW(rw, 1, &spec, &buf, &len)) return;
        // The shipped clicks are already u8/11025/mono; a replacement in another
        // format is converted rather than played as noise.
        SDL_AudioCVT cvt;
        const int need = SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                                           AUDIO_U8, 1, menuclick::kRate);
        if (need < 0) { SDL_FreeWAV(buf); return; }
        if (need == 0) {
            pcm.assign(buf, buf + len);
        } else {
            std::vector<uint8_t> work(size_t(len) * size_t(cvt.len_mult));
            std::copy(buf, buf + len, work.begin());
            cvt.buf = work.data();
            cvt.len = int(len);
            if (SDL_ConvertAudio(&cvt) == 0) pcm.assign(work.begin(), work.begin() + cvt.len_cvt);
        }
        SDL_FreeWAV(buf);
        if (!dev_ && !pcm.empty()) {
            SDL_InitSubSystem(SDL_INIT_AUDIO);
            SDL_AudioSpec want{}, have{};
            want.freq = menuclick::kRate;
            want.format = AUDIO_U8;
            want.channels = 1;
            // Small buffer so a queued click plays with minimal latency. The whole
            // click is queued at once (never a partial fill), so a short device
            // period can't underrun it. ~128/11025 ~= 12ms vs ~46ms at 512.
            want.samples = 128;
            dev_ = tak::openAudioDevice(0, &want, &have, 0);   // flags 0 => have == want
            if (dev_) SDL_PauseAudioDevice(dev_, 0);
        }
    }

    // Play a click now (loading it first if need be), cutting off any still playing.
    void play(const std::string& name) {
        if (name.empty()) return;
        load(name);
        auto it = pcm_.find(name);
        if (!dev_ || it == pcm_.end() || it->second.empty()) return;
        const auto out = menuclick::level(it->second, settings ? settings->masterVol : 256,
                                          settings ? settings->sfxVol : 256);
        SDL_ClearQueuedAudio(dev_);   // retrigger cleanly on rapid clicks
        SDL_QueueAudio(dev_, out.data(), Uint32(out.size()));
    }

    // Still sounding. Closing the device (destroying this) cuts the click off, so a
    // screen that a click leaves keeps drawing until this clears (bounded, ~140ms).
    bool busy() const { return dev_ && SDL_GetQueuedAudioSize(dev_) > 0; }

private:
    const hpi::Vfs& vfs_;
    SDL_AudioDeviceID dev_ = 0;
    std::unordered_map<std::string, std::vector<uint8_t>> pcm_;
};

}  // namespace tak

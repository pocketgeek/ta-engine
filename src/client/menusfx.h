#pragma once

// Plays the front-end's button clicks (client/menuclick.h): one queue-driven device,
// separate from the shared MenuMusic device so a click mixes over the music. The
// title menu, the campaign picker it hosts, TA's front end (tafrontend.cpp) and the
// result screen each own a MenuSfx, but they all queue on one process-wide device that
// stays open, so a click that leaves a screen plays to its end on the next (BIGBUTTON's
// butmain1.wav runs 1.14 s, far past any wait a screen change could afford). A request is a WAV name or a gamedata/allsound.tdf
// entry (client/guisound.h), as TotalA.exe's 0x47f1a0 takes.

#include <SDL.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "client/guisound.h"
#include "client/menuclick.h"
#include "client/options.h"    // ta::openAudioDevice (routes to the chosen device)
#include "client/settings.h"
#include "hpi/hpi.h"

namespace ta {

class MenuSfx {
public:
    explicit MenuSfx(const hpi::Vfs& vfs) : vfs_(vfs) {}
    ~MenuSfx() = default;   // the shared device outlives every screen (sharedDevice)
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
        if (!dev_ && !pcm.empty()) dev_ = sharedDevice();
    }

    // The one click device, opened on first use and kept open for the process. It is
    // reopened when Options switches the output device.
    static SDL_AudioDeviceID sharedDevice() {
        static SDL_AudioDeviceID dev = 0;
        static std::string on;
        if (dev && on != ta::currentAudioDevice()) { SDL_CloseAudioDevice(dev); dev = 0; }
        if (!dev) {
            SDL_InitSubSystem(SDL_INIT_AUDIO);
            SDL_AudioSpec want{}, have{};
            want.freq = menuclick::kRate;
            want.format = AUDIO_U8;
            want.channels = 1;
            // Small buffer so a queued click plays with minimal latency. The whole
            // click is queued at once (never a partial fill), so a short device
            // period can't underrun it. ~128/11025 ~= 12ms vs ~46ms at 512.
            want.samples = 128;
            dev = ta::openAudioDevice(0, &want, &have, 0);   // flags 0 => have == want
            if (dev) SDL_PauseAudioDevice(dev, 0);
            on = ta::currentAudioDevice();
        }
        return dev;
    }

    // The WAV a request plays: a WAV name as is when sounds/ has it; an allsound.tdf
    // entry (no extension, e.g. "BIGBUTTON") through that table, as 0x47f1a0 looks it
    // up; and the Kingdoms click names (menuclick::kOk ...), which TA does not ship,
    // as their retail counterparts (guisound::engineClick).
    std::string resolve(const std::string& name) {
        if (name.empty()) return {};
        std::string low = name;
        for (char& c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
        const bool isWav = low.size() > 4 && low.compare(low.size() - 4, 4, ".wav") == 0;
        if (isWav && vfs_.has("sounds/" + name)) return name;
        if (!table_.loaded()) table_.load(vfs_);
        if (!isWav) return table_.wav(name);
        const std::string entry = guisound::engineClick(name);
        const std::string wav = entry.empty() ? std::string() : table_.wav(entry);
        return wav.empty() ? name : wav;
    }

    // Play a click now (loading it first if need be), cutting off any still playing.
    // True when it was queued on the device. `where` / `gadget` name the press for
    // TA_GUISOUNDLOG (guisound::log).
    bool play(const std::string& name, const char* where = "menu", const std::string& gadget = {}) {
        const std::string wav = resolve(name);
        last = wav;
        lastDecoded = false;
        bool queued = false;
        if (!wav.empty()) {
            load(wav);
            auto it = pcm_.find(wav);
            lastDecoded = it != pcm_.end() && !it->second.empty();
            if (dev_) dev_ = sharedDevice();   // follow an Options device switch
            if (dev_ && lastDecoded) {
                const auto out = menuclick::level(it->second, settings ? settings->masterVol : 256,
                                                  settings ? settings->sfxVol : 256);
                SDL_ClearQueuedAudio(dev_);   // retrigger cleanly on rapid clicks
                queued = SDL_QueueAudio(dev_, out.data(), Uint32(out.size())) == 0;
            }
        }
        if (!name.empty()) guisound::log(where, gadget, name, wav, lastDecoded, queued);
        return queued;
    }
    // The WAV the last play() resolved to ("" none), for logs and checks.
    std::string last;
    bool lastDecoded = false;   // ... and it decoded

    // Still sounding (for logs and checks; a screen change no longer has to wait).
    bool busy() const { return dev_ && SDL_GetQueuedAudioSize(dev_) > 0; }

private:
    const hpi::Vfs& vfs_;
    SDL_AudioDeviceID dev_ = 0;
    std::unordered_map<std::string, std::vector<uint8_t>> pcm_;
    guisound::Table table_;   // gamedata/allsound.tdf, read on the first entry request
};

}  // namespace ta

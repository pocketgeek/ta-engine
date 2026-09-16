#include "client/options.h"
#include "client/briefingscreen.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "campaign/campaign.h"
#include "util/png.h"
#include "client/blockfont.h"
#include "client/cursors.h"
#include "client/menumusic.h"
#include "client/settings.h"
#include "client/dev.h"
#include "client/appquit.h"

namespace ta {

namespace {

// Greedy word-wrap `s` to at most `maxChars` per line (block font is fixed-width).
std::vector<std::string> wrap(const std::string& s, int maxChars) {
    std::vector<std::string> lines;
    std::string line, word;
    auto push = [&] {
        if (word.empty()) return;
        if (line.empty()) line = word;
        else if (int(line.size() + 1 + word.size()) <= maxChars) line += " " + word;
        else { lines.push_back(line); line = word; }
        word.clear();
    };
    for (char c : s) { if (c == ' ') push(); else word += c; }
    push();
    if (!line.empty()) lines.push_back(line);
    if (lines.empty()) lines.push_back("");
    return lines;
}

bool inRect(const SDL_FRect& r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

}  // namespace

// Briefing voice-over (Sounds/<stem>.wav): loaded, played, and torn down via RAII so
// the screen's several early-return paths all release the audio device.
struct BriefingVo {
    SDL_AudioDeviceID dev = 0;
    Uint8* buf = nullptr;
    ~BriefingVo() {
        if (dev) SDL_CloseAudioDevice(dev);
        if (buf) SDL_FreeWAV(buf);
    }
};

bool BriefingScreen::run(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& stem,
                         const std::string& title, Settings* settings, MenuMusic* music) {
    std::vector<std::string> objectives = ta::loadObjectives(vfs, stem);

    // Play the mission's briefing VO if the install ships one (only some missions do).
    BriefingVo vo;
    if (std::string wavPath = "sounds/" + stem + ".wav"; vfs.has(wavPath)) {
        std::vector<uint8_t> wav = vfs.read(wavPath);
        SDL_AudioSpec spec{};
        Uint32 len = 0;
        if (SDL_LoadWAV_RW(SDL_RWFromConstMem(wav.data(), int(wav.size())), 1, &spec, &vo.buf, &len)) {
            vo.dev = ta::openAudioDevice(0, &spec, nullptr, 0);
            int gain = settings ? std::clamp(settings->masterVol * settings->sfxVol * 128 / (256 * 256), 0, 128) : 128;
            if (vo.dev && gain > 0) {
                if (gain < 128) {   // scale to the user's volume
                    std::vector<Uint8> scaled(len, 0);
                    SDL_MixAudioFormat(scaled.data(), vo.buf, spec.format, len, gain);
                    SDL_QueueAudio(vo.dev, scaled.data(), len);
                } else {
                    SDL_QueueAudio(vo.dev, vo.buf, len);
                }
                SDL_PauseAudioDevice(vo.dev, 0);
            }
        }
    }

    CursorSet cursors;
    cursors.load(ren, vfs, settings);
    // Cursor visibility is decided per frame below (hardware -> shown, software ->
    // hidden and drawn), so only the no-art case is settled here.
    if (!cursors.ok()) SDL_ShowCursor(SDL_ENABLE);

    // Drop any click queued by the screen we came from (movie / picker fires on DOWN).
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);

    for (;;) {
        if (ta::termRequested()) return false;   // SIGTERM/SIGINT -> abort to shutdown
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(ren, &w, &h);
        float u = std::clamp(std::min(w / 1280.0f, h / 720.0f), 1.0f, 3.0f);

        float panelW = std::min(900 * u, w * 0.86f);
        float panelH = std::min(620 * u, h * 0.9f);
        SDL_FRect panel{(w - panelW) / 2, (h - panelH) / 2, panelW, panelH};
        float bw = 210 * u, bh = 38 * u, gap = 30 * u;
        float by = panel.y + panel.h - 62 * u;
        SDL_FRect beginRect{panel.x + panel.w / 2 + gap / 2, by, bw, bh};
        SDL_FRect backRect{panel.x + panel.w / 2 - gap / 2 - bw, by, bw, bh};

        // ---- events ----
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) continue;   // ignore the WM close button
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) return false;
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE) return true;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                float mx = float(e.button.x), my = float(e.button.y);
                if (inRect(beginRect, mx, my)) return true;
                if (inRect(backRect, mx, my)) return false;
            }
        }
        if (music) music->poll();

        // ---- draw ----
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 14, 15, 20, 255);
        SDL_RenderClear(ren);
        SDL_SetRenderDrawColor(ren, 26, 28, 36, 255);
        SDL_RenderFillRectF(ren, &panel);
        SDL_SetRenderDrawColor(ren, 96, 106, 138, 255);
        SDL_RenderDrawRectF(ren, &panel);

        float tpx = 3.2f * u;
        drawBlockText(ren, title, panel.x + (panel.w - blockTextWidth(title, tpx)) / 2,
                      panel.y + 26 * u, tpx, {236, 226, 190, 255});
        // divider
        SDL_FRect rule{panel.x + 40 * u, panel.y + 26 * u + 7 * tpx + 16 * u, panel.w - 80 * u, 2 * u};
        SDL_SetRenderDrawColor(ren, 120, 100, 60, 255);
        SDL_RenderFillRectF(ren, &rule);

        float opx = 1.6f * u;
        const char* head = "OBJECTIVES";
        float y = rule.y + 22 * u;
        drawBlockText(ren, head, panel.x + 44 * u, y, opx, {200, 170, 110, 255});
        y += 7 * opx + 20 * u;

        float bpx = 1.7f * u;
        int maxChars = int((panel.w - 110 * u) / (6 * bpx));
        if (objectives.empty()) {
            drawBlockText(ren, "(No briefing text.)", panel.x + 60 * u, y, bpx, {150, 154, 168, 255});
        }
        for (const std::string& obj : objectives) {
            // bullet
            SDL_FRect dot{panel.x + 52 * u, y + 2 * u, 5 * u, 5 * u};
            SDL_SetRenderDrawColor(ren, 210, 180, 90, 255);
            SDL_RenderFillRectF(ren, &dot);
            for (const std::string& ln : wrap(obj, maxChars)) {
                drawBlockText(ren, ln, panel.x + 70 * u, y, bpx, {214, 220, 235, 255});
                y += 7 * bpx + 8 * u;
            }
            y += 6 * u;
        }

        // buttons
        auto button = [&](const SDL_FRect& r, const char* label, bool accent) {
            SDL_SetRenderDrawColor(ren, accent ? 70 : 52, accent ? 104 : 56, accent ? 70 : 70, 255);
            SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, accent ? 130 : 116, accent ? 180 : 124, accent ? 130 : 150, 255);
            SDL_RenderDrawRectF(ren, &r);
            float px = 1.8f * u;
            drawBlockText(ren, label, r.x + (r.w - blockTextWidth(label, px)) / 2,
                          r.y + (r.h - 7 * px) / 2, px, {224, 230, 242, 255});
        };
        button(backRect, "BACK", false);
        button(beginRect, "BEGIN MISSION", true);

        // Honour the hardware-cursor option here too. This screen used to always draw
        // the software cursor, which ignored the setting -- and once load() began
        // preparing hardware frames it also meant reconstructing frames nothing read.
        if (cursors.ok()) {
            const int sc = settings ? settings->cursorScale : 1;
            if (settings && settings->hardwareCursor &&
                cursors.applyHardware(CursorId::Normal, sc)) {
                SDL_ShowCursor(SDL_ENABLE);
            } else {
                SDL_ShowCursor(SDL_DISABLE);
                int mx = 0, my = 0; SDL_GetMouseState(&mx, &my);
                cursors.draw(ren, CursorId::Normal, mx, my, sc);
            }
        }
        // Debug: TA_SHOT_BRIEFING captures one frame for tests, then begins.
        if (const char* sp = ta::devEnv("TA_SHOT_BRIEFING")) {
            std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
            if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4) == 0)
                png::write(sp, w, h, px);
            return true;
        }
        // Headless (dummy video): no input to click BEGIN, so proceed to the mission.
        if (const char* drv = SDL_GetCurrentVideoDriver(); drv && !std::strcmp(drv, "dummy"))
            return true;
        SDL_RenderPresent(ren);
        SDL_Delay(8);
    }
}

}  // namespace ta

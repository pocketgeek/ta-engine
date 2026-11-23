#pragma once

// A two-button question box over a settings panel (Options, Voice Chat, Controls): the
// data folder's RESTART NOW / LATER and DEFAULTS' RESET TO DEFAULTS? YES / NO. It dims
// the screen, centres a box on the panel and takes every event while it is open.
// Enter is the first button, Esc the second.

#include <SDL.h>

#include <algorithm>
#include <string>
#include <vector>

#include "client/uitext.h"

namespace tak {

struct PromptBox {
    bool open = false;
    SDL_FRect first{}, second{};   // the buttons as last drawn (empty while closed)

    // u: the panel's layout unit. lines are centred under the title; line `highlight`
    // (-1 = none) is drawn in the value colour.
    void render(SDL_Renderer* ren, const SDL_FRect& panel, float u, int winW, int winH,
                const std::string& title, const std::vector<std::string>& lines,
                const char* firstLabel, const char* secondLabel, int highlight = -1) {
        if (!open) { first = second = {}; return; }
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
        SDL_FRect dim{0, 0, float(winW), float(winH)}; SDL_RenderFillRectF(ren, &dim);
        const float w = std::min(panel.w - 40 * u, 560 * u), lpx = 1.6f * u, tpx = 2.4f * u;
        const float h = 40 * u + float(lines.size()) * 14 * u + 64 * u;
        SDL_FRect box{panel.x + (panel.w - w) / 2, panel.y + (panel.h - h) / 2, w, h};
        SDL_SetRenderDrawColor(ren, 30, 33, 44, 252); SDL_RenderFillRectF(ren, &box);
        SDL_SetRenderDrawColor(ren, 170, 180, 210, 255); SDL_RenderDrawRectF(ren, &box);
        uiText(ren, title, box.x + (box.w - uiTextWidth(title, tpx)) / 2, box.y + 14 * u, tpx,
               {235, 225, 180, 255});
        for (size_t i = 0; i < lines.size(); ++i)
            uiText(ren, lines[i], box.x + (box.w - uiTextWidth(lines[i], lpx)) / 2,
                   box.y + 40 * u + float(i) * 14 * u, lpx,
                   int(i) == highlight ? SDL_Color{150, 200, 235, 255} : SDL_Color{205, 210, 222, 255});
        const float bw = 170 * u, bh = 32 * u, gap = 18 * u, by = box.y + box.h - bh - 14 * u;
        first = {box.x + (box.w - 2 * bw - gap) / 2, by, bw, bh};
        second = {first.x + bw + gap, by, bw, bh};
        for (int i = 0; i < 2; ++i) {
            const SDL_FRect& r = i ? second : first;
            SDL_SetRenderDrawColor(ren, i ? 60 : 62, i ? 66 : 92, i ? 86 : 80, 255); SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, 130, 140, 170, 255); SDL_RenderDrawRectF(ren, &r);
            const char* label = i ? secondLabel : firstLabel;
            const float bpx = 2.2f * u;
            uiText(ren, label, r.x + (r.w - uiTextWidth(label, bpx)) / 2, r.y + (r.h - 7 * bpx) / 2, bpx,
                   {228, 232, 242, 255});
        }
    }

    // One event while open: 1 = the first button (or Enter), 2 = the second (or Esc),
    // 0 = neither. Either button closes the box.
    int input(const SDL_Event& e) {
        auto in = [](const SDL_FRect& r, float x, float y) { return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h; };
        int pick = 0;
        if (e.type == SDL_KEYDOWN) {
            if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) pick = 1;
            else if (e.key.keysym.sym == SDLK_ESCAPE) pick = 2;
        } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            const float x = float(e.button.x), y = float(e.button.y);
            pick = in(first, x, y) ? 1 : in(second, x, y) ? 2 : 0;
        }
        if (pick) { open = false; first = second = {}; }
        return pick;
    }
};

}  // namespace tak

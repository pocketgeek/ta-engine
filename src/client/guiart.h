#pragma once

// Shared helpers for drawing a retail `.gui` screen: GAF art into an SDL texture, and
// the mapping from the 640x480 space every .gui is authored in onto the real window.
// The front-end menu grew its own copies of these first; the loading and end-of-game
// screens use these so all three agree on palette lookup and letterboxing.

#include <SDL.h>

#include <string>

#include "gaf/gaf.h"

namespace ta::hpi { class Vfs; }

namespace ta {

// Palette for `anims/<gaf>.gaf`: its sibling .pcx if present, else the shared GUI
// palette. Returns an empty palette if neither loads (art then simply doesn't draw).
gaf::Palette guiPalette(const hpi::Vfs& vfs, const std::string& gafName);

// Sequence `seq` frame `frame` of `anims/<gaf>.gaf` as a texture, or nullptr.
// The caller owns the texture and must free it with gpuvram::destroy.
//
// `keyBlack` punches pure black out to fully transparent. Retail's DirectDraw blits
// colour-keyed, so some plates (the loading arch) paint their cut-out in black and
// rely on whatever was drawn underneath showing through.
SDL_Texture* gafTexture(SDL_Renderer* ren, const hpi::Vfs& vfs, const std::string& gafName,
                        const std::string& seq, int frame = 0, bool keyBlack = false);

// Maps .gui coordinates onto the window: uniform scale, centred, never cropped, so a
// 640x480 layout keeps its proportions on a 32:9 display instead of stretching.
struct GuiLayout {
    float scale = 1;
    float ox = 0, oy = 0;

    GuiLayout() = default;
    GuiLayout(int winW, int winH, float baseW = 640, float baseH = 480) {
        scale = std::min(float(winW) / baseW, float(winH) / baseH);
        ox = (float(winW) - baseW * scale) / 2;
        oy = (float(winH) - baseH * scale) / 2;
    }
    SDL_FRect rect(float x, float y, float w, float h) const {
        return {ox + x * scale, oy + y * scale, w * scale, h * scale};
    }
    float px(float x) const { return ox + x * scale; }
    float py(float y) const { return oy + y * scale; }
    bool hit(float x, float y, float w, float h, float mx, float my) const {
        SDL_FRect r = rect(x, y, w, h);
        return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
    }
};

}  // namespace ta

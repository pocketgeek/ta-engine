#pragma once

// Bitmap font. Two sources, because the two games store fonts differently:
//
//   * a GAF sequence, one frame per glyph, with per-glyph vertical offsets
//     (Kingdoms);
//   * a TA `.FNT` -- a packed 1-bit-per-pixel format (see src/fnt/fnt.h).
//
// A .FNT glyph is a coverage MASK with no colour of its own, so it is built as
// white-with-alpha and takes the tint draw() already applies.
//
// Extracted from client/main.cpp; kept at global scope so its existing
// unqualified use sites there are unchanged.

#include <SDL.h>

#include <string>

namespace ta::hpi { class Vfs; }

class Font {
public:
    Font() = default;
    Font(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, const std::string& gafPath);
    // Load a TA .FNT (e.g. "fonts/ARMBUTT.FNT"). Returns an unusable Font rather
    // than throwing if the file is missing or will not parse, so a caller can
    // simply test ok() and fall back.
    static Font fromFnt(SDL_Renderer* ren, const ta::hpi::Vfs& vfs,
                        const std::string& fntPath);

    bool ok() const { return ok_; }

    int width(const std::string& text, float scale = 1) const;

    // Tallest glyph cell, for sizing a backing panel behind a line of text.
    int height(float scale = 1) const;

    // Where `text` actually renders vertically, relative to the `y` passed to draw():
    // its pixels occupy [y + topOff, y + topOff + h]. draw() lifts each glyph by its
    // yoff, so topOff is usually NEGATIVE (the text sits ABOVE y). Used to draw a
    // backing box that truly wraps the text instead of sitting below it.
    void vbounds(const std::string& text, float scale, float& topOff, float& h) const;

    // Free the glyph textures (gpuvram-accounted). Called at session teardown so
    // the VRAM budget doesn't leak across menu->game->menu loops; the font is
    // unusable afterwards until reconstructed. (Not a destructor: Font objects
    // are copy-assigned when the GUI loads, so an owning dtor would double-free.)
    void destroyGlyphs();

    void draw(SDL_Renderer* ren, const std::string& text, float x, float y,
              float scale = 1, SDL_Color tint = {255, 255, 255, 255}) const;

private:
    struct Glyph {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0, yoff = 0;
    };
    static float advance(const Glyph& g);
    Glyph glyphs_[256] = {};
    bool ok_ = false;
};

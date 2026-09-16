#pragma once

// Bitmap font loaded from a GAF sequence (one frame per glyph), drawn with
// per-glyph vertical offsets. Extracted from client/main.cpp; kept at global
// scope so its existing unqualified use sites there are unchanged.

#include <SDL.h>

#include <string>

namespace ta::hpi { class Vfs; }

class Font {
public:
    Font() = default;
    Font(SDL_Renderer* ren, const ta::hpi::Vfs& vfs, const std::string& gafPath);

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

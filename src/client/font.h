#pragma once

// Bitmap font loaded from a GAF sequence (one frame per glyph), drawn with
// per-glyph vertical offsets. Extracted from client/main.cpp; kept at global
// scope so its existing unqualified use sites there are unchanged.
//
// Each load names the face's ROLE. FontRole::Art (the default) is retail lettering --
// font48, Lombardic, decorativesm, bodfontdecor -- and always draws its glyph art.
// FontRole::Text is plain reading text -- bodfontbody and the Times faces -- and
// follows UI FONT (client/uitext.h): under MODERN every method below measures and
// draws Noto Sans Bold instead, at the GAF's own cap height ('H') and baseline, so a
// layout keeps its lines; a tinted draw() takes the art's typical colour times the
// tint, as the shaded glyphs would.

#include <SDL.h>

#include <string>

namespace tak::hpi { class Vfs; }

enum class FontRole { Art, Text };

class Font {
public:
    Font() = default;
    Font(SDL_Renderer* ren, const tak::hpi::Vfs& vfs, const std::string& gafPath,
         FontRole role = FontRole::Art);
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;
    Font(Font&&) noexcept;
    Font& operator=(Font&&) noexcept;

    bool ok() const { return ok_; }
    void setLetterSpacing(float pixels) { letterSpacing_ = pixels; }

    int width(const std::string& text, float scale = 1) const;

    // Tallest glyph cell, for sizing a backing panel behind a line of text.
    int height(float scale = 1) const;
    // The GAF's tallest glyph cell in EITHER style: a size reference that does not
    // move with UI FONT (MODERN draws at the GAF's own cap height, so a scale set
    // from this gives the same caps in both; height() above is MODERN's taller line).
    int glyphHeight(float scale = 1) const;

    // Where `text` actually renders vertically, relative to the `y` passed to draw():
    // its pixels occupy [y + topOff, y + topOff + h]. draw() lifts each glyph by its
    // yoff, so topOff is usually NEGATIVE (the text sits ABOVE y). Used to draw a
    // backing box that truly wraps the text instead of sitting below it.
    void vbounds(const std::string& text, float scale, float& topOff, float& h) const;

    // Free the glyph textures (gpuvram-accounted). Called at session teardown so
    // the VRAM budget doesn't leak across menu->game->menu loops; the font is
    // unusable afterwards until reconstructed. Also called by the destructor;
    // explicit cleanup is safe and must precede destroying the SDL renderer.
    void destroyGlyphs();

    void draw(SDL_Renderer* ren, const std::string& text, float x, float y,
              float scale = 1, SDL_Color tint = {255, 255, 255, 255}) const;

    // draw() in one flat colour: every opaque pixel of the glyph becomes exactly
    // `color`. The retail glyph art is shaded tan, so a tint through draw() comes out
    // a mottled, dark-edged version of the colour that is hard to read over terrain
    // (the under-unit squad marker); this keeps the same shapes, size and placement.
    void drawSolid(SDL_Renderer* ren, const std::string& text, float x, float y,
                   float scale, SDL_Color color) const;

private:
    struct Glyph {
        SDL_Texture* tex = nullptr;
        SDL_Texture* mask = nullptr;   // the glyph's coverage in white, for drawSolid()
        int w = 0, h = 0, yoff = 0;
    };
    float advance(const Glyph& g) const;
    void drawWith(SDL_Renderer* ren, const std::string& text, float x, float y,
                  float scale, SDL_Color tint, bool solid) const;
    float letterSpacing_ = 2;
    Glyph glyphs_[256] = {};
    bool ok_ = false;
    // FontRole::Text: the face's cap height and the baseline below the draw() y (both
    // at scale 1, from glyph 'H'), and the art's typical colour.
    FontRole role_ = FontRole::Art;
    float capH_ = 0, baseOff_ = 0;
    SDL_Color base_{255, 255, 255, 255};
    bool modern() const;   // a text face, drawn in the MODERN face right now
};

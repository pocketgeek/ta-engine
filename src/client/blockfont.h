#pragma once

// The CLASSIC interface text: a tiny 5x7 vector block font drawn as filled rects --
// crisp at any scale, no GAF needed. The one copy of it: everything draws and measures
// text through client/uitext.h (uiText / uiTextWidth), which comes here when UI FONT is
// CLASSIC. A glyph advances 6 * px (5 + a 1 px gap); a space, or a character the font
// lacks, advances 4 * px and draws nothing. Letters draw as capitals.

#include <SDL.h>

#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace tak {

inline const uint8_t* blockGlyph(char c) {
    static const std::unordered_map<char, std::array<uint8_t, 5>> F = {
        {'0',{0x3E,0x51,0x49,0x45,0x3E}},{'1',{0x00,0x42,0x7F,0x40,0x00}},
        {'2',{0x42,0x61,0x51,0x49,0x46}},{'3',{0x21,0x41,0x45,0x4B,0x31}},
        {'4',{0x18,0x14,0x12,0x7F,0x10}},{'5',{0x27,0x45,0x45,0x45,0x39}},
        {'6',{0x3C,0x4A,0x49,0x49,0x30}},{'7',{0x01,0x71,0x09,0x05,0x03}},
        {'8',{0x36,0x49,0x49,0x49,0x36}},{'9',{0x06,0x49,0x49,0x29,0x1E}},
        {'A',{0x7E,0x11,0x11,0x11,0x7E}},{'B',{0x7F,0x49,0x49,0x49,0x36}},
        {'C',{0x3E,0x41,0x41,0x41,0x22}},{'D',{0x7F,0x41,0x41,0x22,0x1C}},
        {'E',{0x7F,0x49,0x49,0x49,0x41}},{'F',{0x7F,0x09,0x09,0x09,0x01}},
        {'G',{0x3E,0x41,0x49,0x49,0x7A}},{'H',{0x7F,0x08,0x08,0x08,0x7F}},
        {'I',{0x00,0x41,0x7F,0x41,0x00}},{'J',{0x20,0x40,0x41,0x3F,0x01}},
        {'K',{0x7F,0x08,0x14,0x22,0x41}},{'L',{0x7F,0x40,0x40,0x40,0x40}},
        {'M',{0x7F,0x02,0x0C,0x02,0x7F}},{'N',{0x7F,0x04,0x08,0x10,0x7F}},
        {'O',{0x3E,0x41,0x41,0x41,0x3E}},{'P',{0x7F,0x09,0x09,0x09,0x06}},
        {'Q',{0x3E,0x41,0x51,0x21,0x5E}},{'R',{0x7F,0x09,0x19,0x29,0x46}},
        {'S',{0x46,0x49,0x49,0x49,0x31}},{'T',{0x01,0x01,0x7F,0x01,0x01}},
        {'U',{0x3F,0x40,0x40,0x40,0x3F}},{'V',{0x1F,0x20,0x40,0x20,0x1F}},
        {'W',{0x7F,0x20,0x18,0x20,0x7F}},{'X',{0x63,0x14,0x08,0x14,0x63}},
        {'Y',{0x07,0x08,0x70,0x08,0x07}},{'Z',{0x61,0x51,0x49,0x45,0x43}},
        {'.',{0x00,0x60,0x60,0x00,0x00}},{':',{0x00,0x36,0x36,0x00,0x00}},
        {'-',{0x08,0x08,0x08,0x08,0x08}},{'+',{0x08,0x08,0x3E,0x08,0x08}},
        {'%',{0x63,0x13,0x08,0x64,0x63}},{'/',{0x20,0x10,0x08,0x04,0x02}},
        {'(',{0x00,0x1C,0x22,0x41,0x00}},{')',{0x00,0x41,0x22,0x1C,0x00}},
        // An unknown glyph renders blank but still advances, so anything missing is
        // silently swallowed. '_' is legal in an account name (auth::validUsername).
        {'_',{0x40,0x40,0x40,0x40,0x40}},{',',{0x00,0x50,0x30,0x00,0x00}},
        {'!',{0x00,0x00,0x5F,0x00,0x00}},{'\'',{0x00,0x00,0x03,0x00,0x00}},
        {'?',{0x02,0x01,0x51,0x09,0x06}},{'*',{0x14,0x08,0x3E,0x08,0x14}},
        // Windows folder paths (Options' GAME DATA FOLDER): '/' mirrored.
        {'\\',{0x02,0x04,0x08,0x10,0x20}},
    };
    auto it = F.find(c);
    return it == F.end() ? nullptr : it->second.data();
}

inline float classicBlockAdvance(char c, float px) {
    return blockGlyph(char(std::toupper((unsigned char)c))) ? 6 * px : 4 * px;
}
inline float classicBlockWidth(const std::string& s, float px) {
    float w = 0;
    for (char c : s) w += classicBlockAdvance(c, px);
    return w;
}

// One block-glyph pixel, snapped to whole DEVICE pixels (at the renderer's current
// scale). OpenGL fills the pixels whose centres fall inside a fractional rect, but
// SDL's software renderer truncates the rect's position and size separately, so at a
// fractional size (the 1.7px stats panel, a UI-scaled Options row) every glyph dot
// shrank to 1px with a gap after it and the text read as faint dotted outlines.
// Rounding both edges here gives the pixels OpenGL already drew, on every renderer.
// The +0.01 keeps the software path's truncation from landing one pixel short.
inline SDL_FRect blockPixel(float x, float y, float px, float sx, float sy) {
    float x0 = std::floor(x * sx + 0.5f), x1 = std::floor((x + px) * sx + 0.5f);
    float y0 = std::floor(y * sy + 0.5f), y1 = std::floor((y + px) * sy + 0.5f);
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    return {(x0 + 0.01f) / sx, (y0 + 0.01f) / sy, (x1 - x0 + 0.01f) / sx, (y1 - y0 + 0.01f) / sy};
}
inline void blockScale(SDL_Renderer* r, float& sx, float& sy) {
    SDL_RenderGetScale(r, &sx, &sy);
    if (!(sx > 0)) sx = 1;
    if (!(sy > 0)) sy = 1;
}

inline void drawClassicBlock(SDL_Renderer* r, const std::string& s, float x, float y, float px, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    float sx, sy;
    blockScale(r, sx, sy);
    float cx = x;
    for (char ch : s) {
        const uint8_t* cols = blockGlyph(char(std::toupper((unsigned char)ch)));
        if (!cols) { cx += 4 * px; continue; }   // space / unknown
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (cols[col] & (1 << row)) {
                    SDL_FRect q = blockPixel(cx + col * px, y + row * px, px, sx, sy);
                    SDL_RenderFillRectF(r, &q);
                }
        cx += 6 * px;
    }
}

}  // namespace tak

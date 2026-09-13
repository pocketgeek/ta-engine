#include "client/font.h"

#include "client/gpuvram.h"
#include "client/artscale.h"
#include "gaf/gaf.h"
#include "hpi/hpi.h"

#include <algorithm>
#include <filesystem>

Font::Font(SDL_Renderer* ren, const tak::hpi::Vfs& vfs, const std::string& gafPath) {
    std::filesystem::path pcx = gafPath;
    pcx.replace_extension(".pcx");
    auto pal = tak::gaf::Palette::fromBytes(vfs.read(pcx.generic_string()),
                                            pcx.generic_string());
    auto seqs = tak::gaf::load(vfs.read(gafPath), pal, -1, gafPath);
    if (seqs.empty()) return;
    auto& frames = seqs[0].frames;
    for (size_t i = 0; i < frames.size() && i < 256; ++i) {
        auto& f = frames[i];
        Glyph g;
        g.w = f.width;
        g.h = f.height;
        g.yoff = f.yoff;
        if (f.width > 0 && f.height > 0) {
            // g.w/g.h stay the 1x LOGICAL size -- draw(), advance(), width() and
            // vbounds() are all in those units, so a 2x texture is invisible to layout.
            // Glyphs are the smallest art in the game (a few px tall) and the HUD
            // magnifies them, so they stair-step as badly as anything.
            g.tex = tak::art::makeTexture(ren, f.rgba, f.width, f.height);
        }
        glyphs_[i] = g;
    }
    ok_ = true;
}

int Font::width(const std::string& text, float scale) const {
    float x = 0;
    for (unsigned char c : text) x += advance(glyphs_[c]) * scale;
    return int(x);
}

int Font::height(float scale) const {
    int h = 0;
    for (const Glyph& g : glyphs_) if (g.h > h) h = g.h;
    return int(h * scale);
}

void Font::vbounds(const std::string& text, float scale, float& topOff, float& h) const {
    bool any = false; float top = 0, bot = 0;
    for (unsigned char c : text) {
        const Glyph& g = glyphs_[c];
        if (!g.tex || c == ' ') continue;
        float gt = -float(g.yoff) * scale, gb = float(g.h - g.yoff) * scale;
        if (!any) { top = gt; bot = gb; any = true; }
        else { top = std::min(top, gt); bot = std::max(bot, gb); }
    }
    if (!any) { topOff = 0; h = float(height(scale)); return; }
    topOff = top; h = bot - top;
}

void Font::draw(SDL_Renderer* ren, const std::string& text, float x, float y,
                float scale, SDL_Color tint) const {
    for (unsigned char c : text) {
        const Glyph& g = glyphs_[c];
        // Some fonts have a visible space glyph (a dot) — never draw it.
        if (g.tex && c != ' ') {
            SDL_SetTextureColorMod(g.tex, tint.r, tint.g, tint.b);
            SDL_FRect dst{x, y - g.yoff * scale, g.w * scale, g.h * scale};
            SDL_RenderCopyF(ren, g.tex, nullptr, &dst);
        }
        x += advance(g) * scale;
    }
}

float Font::advance(const Glyph& g) { return g.w > 0 ? float(g.w + 2) : 4.0f; }

void Font::destroyGlyphs() {
    for (auto& g : glyphs_)
        if (g.tex) { gpuvram::destroy(g.tex); g.tex = nullptr; }
    ok_ = false;
}

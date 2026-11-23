#include "client/font.h"

#include "client/gpuvram.h"
#include "client/artscale.h"
#include "client/uifont.h"
#include "client/uitext.h"
#include "gaf/gaf.h"
#include "hpi/hpi.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

Font::~Font() { destroyGlyphs(); }
Font::Font(Font&& other) noexcept { *this = std::move(other); }
Font& Font::operator=(Font&& other) noexcept {
    if (this == &other) return *this;
    destroyGlyphs();
    for (size_t i=0;i<256;++i) glyphs_[i]=std::exchange(other.glyphs_[i],{});
    ok_=std::exchange(other.ok_,false);
    letterSpacing_=other.letterSpacing_;
    role_=other.role_; capH_=other.capH_; baseOff_=other.baseOff_; base_=other.base_;
    return *this;
}

Font::Font(SDL_Renderer* ren, const tak::hpi::Vfs& vfs, const std::string& gafPath, FontRole role) {
    std::filesystem::path pcx = gafPath;
    pcx.replace_extension(".pcx");
    auto pal = tak::gaf::Palette::fromBytes(vfs.read(pcx.generic_string()),
                                            pcx.generic_string());
    auto seqs = tak::gaf::load(vfs.read(gafPath), pal, -1, gafPath);
    if (seqs.empty()) return;
    // A failed allocation partway through construction must release earlier
    // glyphs too; the destructor of a partially constructed Font is not called.
    Font loaded;
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
            std::vector<uint8_t> mask = f.rgba;
            for (size_t p = 0; p + 3 < mask.size(); p += 4) mask[p] = mask[p + 1] = mask[p + 2] = 255;
            g.mask = tak::art::makeTexture(ren, mask, f.width, f.height);
        }
        loaded.glyphs_[i] = g;
    }
    loaded.ok_ = true;
    if (role == FontRole::Text) {
        // The size and line the MODERN face takes: the cap height and baseline of the
        // first of these the face has.
        for (unsigned char c : {'H', 'E', 'A', '0'})
            if (c < frames.size() && frames[c].height > 0) {
                loaded.capH_ = float(frames[c].height);
                loaded.baseOff_ = float(frames[c].height - frames[c].yoff);
                break;
            }
        // The typical colour: the mean of the brighter half of the glyphs' opaque
        // pixels (the art is shaded, often with a dark rim).
        double sum[3] = {}, n = 0;
        int maxL = 0;
        auto lum = [](const std::vector<uint8_t>& p, size_t i) { return p[i] * 3 + p[i + 1] * 6 + p[i + 2]; };
        for (const auto& f : frames)
            for (size_t p = 0; p + 3 < f.rgba.size(); p += 4)
                if (f.rgba[p + 3] > 128) maxL = std::max(maxL, lum(f.rgba, p));
        for (const auto& f : frames)
            for (size_t p = 0; p + 3 < f.rgba.size(); p += 4)
                if (f.rgba[p + 3] > 128 && lum(f.rgba, p) * 2 >= maxL) {
                    for (size_t k = 0; k < 3; ++k) sum[k] += f.rgba[p + k];
                    n += 1;
                }
        if (n > 0) loaded.base_ = {Uint8(sum[0] / n), Uint8(sum[1] / n), Uint8(sum[2] / n), 255};
        if (loaded.capH_ > 0) loaded.role_ = FontRole::Text;
    }
    *this = std::move(loaded);
}

bool Font::modern() const { return role_ == FontRole::Text && tak::modernUiText() && tak::uiFont().ok(); }

int Font::width(const std::string& text, float scale) const {
    if (modern()) return int(std::ceil(tak::uiFont().width(text, tak::uiFontScaleForCap(capH_ * scale))));
    float x = 0;
    for (unsigned char c : text) x += advance(glyphs_[c]) * scale;
    return int(x);
}

int Font::height(float scale) const {
    if (modern()) return int(tak::uiFont().pixelSize(tak::uiFontScaleForCap(capH_ * scale)) + 0.5f);
    return glyphHeight(scale);
}

int Font::glyphHeight(float scale) const {
    int h = 0;
    for (const Glyph& g : glyphs_) if (g.h > h) h = g.h;
    return int(h * scale);
}

void Font::vbounds(const std::string& text, float scale, float& topOff, float& h) const {
    if (modern()) {
        // UiFont's bounds are below its line top, which drawWith puts ascent above the
        // GAF baseline (y + baseOff_ * scale).
        const tak::UiFont& f = tak::uiFont();
        const float sc = tak::uiFontScaleForCap(capH_ * scale);
        f.vbounds(text, sc, topOff, h);
        topOff += baseOff_ * scale - f.ascentFrac() * f.pixelSize(sc);
        return;
    }
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
    drawWith(ren, text, x, y, scale, tint, false);
}

void Font::drawSolid(SDL_Renderer* ren, const std::string& text, float x, float y,
                     float scale, SDL_Color color) const {
    drawWith(ren, text, x, y, scale, color, true);
}

void Font::drawWith(SDL_Renderer* ren, const std::string& text, float x, float y,
                    float scale, SDL_Color tint, bool solid) const {
    if (modern()) {
        SDL_Color c = tint;
        if (!solid)
            c = {Uint8(base_.r * tint.r / 255), Uint8(base_.g * tint.g / 255), Uint8(base_.b * tint.b / 255), tint.a};
        const tak::UiFont& f = tak::uiFont();
        const float sc = tak::uiFontScaleForCap(capH_ * scale);
        f.drawSolid(ren, text, x, y + baseOff_ * scale - f.ascentFrac() * f.pixelSize(sc), sc, c);
        return;
    }
    for (unsigned char c : text) {
        const Glyph& g = glyphs_[c];
        // Some fonts have a visible space glyph (a dot) — never draw it.
        if (SDL_Texture* t = solid ? g.mask : g.tex; t && c != ' ') {
            SDL_SetTextureColorMod(t, tint.r, tint.g, tint.b);
            if (solid) SDL_SetTextureAlphaMod(t, tint.a);
            SDL_FRect dst{x, y - g.yoff * scale, g.w * scale, g.h * scale};
            SDL_RenderCopyF(ren, t, nullptr, &dst);
        }
        x += advance(g) * scale;
    }
}

float Font::advance(const Glyph& g) const { return g.w > 0 ? g.w + letterSpacing_ : 4.0f; }

void Font::destroyGlyphs() {
    for (auto& g : glyphs_) {
        if (g.tex) { gpuvram::destroy(g.tex); g.tex = nullptr; }
        if (g.mask) { gpuvram::destroy(g.mask); g.mask = nullptr; }
    }
    ok_ = false;
}

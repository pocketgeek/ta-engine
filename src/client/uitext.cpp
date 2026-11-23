#include "client/uitext.h"

#include "client/blockfont.h"
#include "client/chatbox.h"
#include "client/uifont.h"
#include "client/utf8.h"

#include <algorithm>
#include <cmath>

namespace tak {

namespace {
UiFontStyle g_style = UiFontStyle::Modern;

// MODERN: the UiFont scale for a block cell, and the line top that puts its baseline
// on the cell's (y + 7 * px).
float cellScale(float px) { return uiFontScaleForCap(7 * px); }
float lineTop(float y, float px, float scale) {
    const UiFont& f = uiFont();
    return y + 7 * px - f.ascentFrac() * f.pixelSize(scale);
}
}  // namespace

UiFontStyle uiFontStyle() { return g_style; }
void setUiFontStyle(UiFontStyle style) { g_style = style; }

float uiFontScaleForCap(float capPx) {
    const UiFont& f = uiFont();
    return std::max(0.05f, capPx / (f.capFrac() * f.basePx()));
}

float uiTextWidth(const std::string& s, float px) {
    if (modernUiText() && uiFont().ok()) return uiFont().width(s, cellScale(px));
    return classicBlockWidth(s, px);
}

void uiText(SDL_Renderer* r, const std::string& s, float x, float y, float px, SDL_Color c) {
    if (modernUiText() && uiFont().ok()) {
        const float scale = cellScale(px);
        uiFont().drawSolid(r, s, x, lineTop(y, px, scale), scale, c);
        return;
    }
    drawClassicBlock(r, s, x, y, px, c);
}

void uiTextShadow(SDL_Renderer* r, const std::string& s, float x, float y, float px, SDL_Color c) {
    const float o = std::max(1.0f, px * 0.4f);
    uiText(r, s, x + o, y + o, px, {0, 0, 0, 170});
    uiText(r, s, x, y, px, c);
}

float uiTextDescent(float px) {
    if (!modernUiText() || !uiFont().ok()) return 0;
    const UiFont& f = uiFont();
    return f.descentFrac() * f.pixelSize(cellScale(px));
}

float uiTextAscent(float px) {
    if (!modernUiText() || !uiFont().ok()) return 0;
    const UiFont& f = uiFont();
    return std::max(0.0f, f.ascentFrac() * f.pixelSize(cellScale(px)) - 7 * px);
}

float uiTextInkDescent(const std::string& s, float px) {
    if (!modernUiText() || !uiFont().ok()) return 0;
    const UiFont& f = uiFont();
    const float scale = cellScale(px);
    float top = 0, h = 0;
    f.vbounds(s, scale, top, h);
    // vbounds measures from the line top; the baseline sits ascent below it. Round
    // letters (S, O, C) overshoot the baseline by ~1.5% of the cap height: not a descent.
    const float d = top + h - f.ascentFrac() * f.pixelSize(scale);
    return d > 0.05f * 7 * px ? d : 0.0f;
}

float uiTextFitPx(const std::string& s, float px, float maxW, float minPx) {
    const float w = uiTextWidth(s, px);
    return w <= maxW || w <= 0 ? px : std::max(minPx, px * maxW / w);
}

std::string uiTextFit(const std::string& s, float px, float maxW, bool ellipsis) {
    if (uiTextWidth(s, px) <= maxW) return s;
    const std::string tail = ellipsis ? "..." : "";
    const float room = maxW - uiTextWidth(tail, px);
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const size_t n = utf8::next(s, i);
        if (uiTextWidth(out + s.substr(i, n), px) > room) break;
        out += s.substr(i, n);
        i += n;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out + tail;
}

std::string uiTextFitStart(const std::string& s, float px, float maxW) {
    if (uiTextWidth(s, px) <= maxW) return s;
    const float room = maxW - uiTextWidth("...", px);
    // Character starts, so the kept tail begins on a whole character.
    std::vector<size_t> starts;
    for (size_t i = 0; i < s.size(); i += utf8::next(s, i)) starts.push_back(i);
    for (size_t k = 0; k < starts.size(); ++k)
        if (uiTextWidth(s.substr(starts[k]), px) <= room) return "..." + s.substr(starts[k]);
    return "...";
}

std::vector<std::string> uiTextWrap(const std::string& s, float px, float maxW) {
    return chatbox::wrap(s, maxW, maxW, [px](const std::string& t) { return uiTextWidth(t, px); });
}

void releaseUiTextGlyphs() { uiFont().destroyGlyphs(); }

}  // namespace tak

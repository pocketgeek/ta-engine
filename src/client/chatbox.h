#pragma once

// In-game chat box: the pure layout rules (word wrap, size clamp, placement), kept
// apart from GameView so they can be tested without a renderer. GameView's
// drawGameChat() measures text with the chat font (client/uifont.h) and draws what
// these return.
// Display only: nothing here touches the simulation or the network.

#include "client/buildrow.h"
#include "client/utf8.h"

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace tak::chatbox {

// Options: CHAT SCALE (on top of UI SCALE) and CHAT POSITION.
inline constexpr float kMinScale = 0.5f, kMaxScale = 3.0f;
enum Align : int { Left = 0, Centered = 1 };

// How long a received line stays up while not typing, then how long it fades.
inline constexpr float kHoldSeconds = 12.0f, kFadeSeconds = 2.0f;

// Opacity of a line `age` seconds old (1 = fully shown, 0 = gone).
inline float lineAlpha(float age) {
    if (age <= kHoldSeconds) return 1.0f;
    return std::clamp(1.0f - (age - kHoldSeconds) / kFadeSeconds, 0.0f, 1.0f);
}

// Word-wrap `text` to `maxW` pixels. The first row has `firstW` pixels instead (the
// sender's name sits in front of it). A word longer than a whole row is broken
// between characters (client/utf8.h: a well-formed UTF-8 sequence, or else one byte --
// exactly the glyphs the chat font draws), never inside one. Always returns
// at least one row (possibly empty).
inline std::vector<std::string> wrap(const std::string& text, float firstW, float maxW,
                                     const std::function<float(const std::string&)>& width) {
    std::vector<std::string> rows(1);
    auto limit = [&] { return rows.size() == 1 ? firstW : maxW; };
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == ' ') {                        // a space: keep it unless the row is empty
            if (!rows.back().empty()) rows.back() += ' ';
            ++i;
            continue;
        }
        size_t e = text.find(' ', i);
        if (e == std::string::npos) e = text.size();
        std::string word = text.substr(i, e - i);
        i = e;
        if (width(rows.back() + word) <= limit()) { rows.back() += word; continue; }
        // Does not fit on this row: start a new one (dropping the trailing space)...
        if (!rows.back().empty()) {
            while (rows.back().back() == ' ') rows.back().pop_back();   // never only spaces
            rows.emplace_back();
        }
        if (width(word) <= limit()) { rows.back() = word; continue; }
        // An empty first row is narrower (the name sits there): a word that fits a whole
        // continuation row goes there whole rather than being broken after the name.
        if (rows.size() == 1 && width(word) <= maxW) { rows.emplace_back(word); continue; }
        // ...and break a word that is wider than a whole row.
        for (size_t c = 0; c < word.size();) {
            const size_t n = utf8::next(word, c);
            std::string ch = word.substr(c, n);
            if (!rows.back().empty() && width(rows.back() + ch) > limit()) rows.emplace_back();
            rows.back() += ch;
            c += n;
        }
    }
    while (!rows.back().empty() && rows.back().back() == ' ') rows.back().pop_back();
    return rows;
}

// The top of the conjure (build) row GameView::drawPanel draws above the information
// bar for a selected builder, as one row (client/buildrow.h): icons (barH - 10) x BUILD
// MENU SCALE tall (shrunk to fit over the bar), 5 px above it, the TRAINING line drawn
// at 24 x scale over the icons and a hovered icon's tooltip at 28 x scale, the highest.
// `barTop` is the bar's top, `barH` its height. The row may sit left, centre or right,
// so the band is the whole width. A long menu that wraps into more rows is higher; that
// is the drawn row's own top (`rowTop` below), not this estimate.
inline float buildRowTop(float barTop, float barH, float buildScale) {
    return buildrow::layout(1, barTop, barH, buildScale, 1e6f, 1e6f, buildrow::Centered).bandTop;
}

// The smallest box worth drawing: two lines and the composer at the 10 px floor text
// size (three 13.75 px rows, the pads and the rule), with `margin` above and below.
inline float minRoom(float margin) { return 52.0f + 2 * margin; }

// The lowest y the box may reach: above the information bar and the HUD drawn over it
// -- the build row with its TRAINING line and tooltip (above), and the weapon-switch
// row (26 px buttons 8 px over the bar). `rowTop` is the top of the build row drawn
// this frame (0: none is up); with none up, the room a row of BUILD MENU SCALE
// `buildScale` would take is kept anyway (buildRowTop), so the box never covers the row
// when it appears. The two agree for a menu that fits one row; a long menu wrapped
// into more rows is higher (the drawn top wins). A row that is not drawn never takes
// the box's last `minRoom(margin)` of height, though (a big build scale on a short
// window would otherwise reserve the whole screen and hide chat with nothing there).
// `hudRow` is false for a spectator, who gets no bottom HUD.
inline float hudFloor(float barTop, float barH, float buildScale, float rowTop, bool hudRow,
                      float margin) {
    if (!hudRow) return barTop;
    const float row = rowTop > 0 ? rowTop
                                 : std::max(buildRowTop(barTop, barH, buildScale), minRoom(margin));
    return std::min(barTop - 34.0f, row);
}

// Where the box goes. `areaW` is the width of the play area (left of the command
// panel), `bottom` the y the box must stay above (hudFloor), `top` the highest it may
// reach; the box keeps `margin` clear of the play area's sides and of `bottom`.
struct Rect { float x = 0, y = 0, w = 0, h = 0; };
inline Rect place(int align, float boxW, float boxH, float areaW, float bottom, float top,
                  float margin) {
    Rect r;
    r.w = std::min(boxW, std::max(0.0f, areaW - 2 * margin));
    r.h = boxH;
    r.x = align == Centered ? (areaW - r.w) / 2 : margin;
    r.x = std::max(r.x, margin);
    r.y = std::max(top, bottom - margin - r.h);
    return r;
}

}  // namespace tak::chatbox

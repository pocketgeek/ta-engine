#pragma once

// Layout arithmetic for the minimap-strip stats readout (GameView::drawStatsPanel),
// kept OUT of the draw call so it can be tested without a renderer, a font or a window.
//
// It lives here because the interesting behaviour is entirely geometric, and none of it
// is observable from a screenshot on a headless build: SDL's dummy and offscreen video
// drivers ignore SDL_SetWindowSize, so a "resize" test against them silently captures the
// same size every time. Testing the arithmetic directly is both possible and
// reproducible; see tools/statsfit_test.cpp.
//
// THE RULES, deliberately separated:
//   * TYPE SIZE is FIXED. It follows the UI SCALE option and nothing else -- resizing the
//     window never changes how big the readout is, only how much of it there is room for.
//   * ROW COUNT follows the gap's HEIGHT, and nothing else.
//   * WIDTH is a visibility guard only: if the reserved columns do not fit the strip, the
//     panel is hidden rather than spilling over the map. It never resizes the type and it
//     never changes the row count.
//
// The width budget is a FIXED character count rather than the text actually on screen.
// Measuring the live strings would make the panel jump whenever a value gained a digit --
// ping crossing 100ms, unit count crossing 1000 -- so the readout would twitch as the
// game ran.

#include <algorithm>

namespace ta::hud {

struct StatsFit {
    bool visible = false;   // false = draw nothing (columns do not fit, or no room)
    float rowH = 0;         // pitch between rows
    int rows = 0;           // how many rows fit the HEIGHT, capped at what we have
};

// `scale` is the caller's fixed glyph scale, `colBudget` the width reserved in characters
// (label + separator + value), `glyphW`/`glyphH` the font's cell size at scale 1.
inline StatsFit fitStats(int candidates, float availW, float availH, int colBudget,
                         float glyphW, float glyphH, float scale, float rowPad) {
    StatsFit f;
    if (candidates <= 0 || availW <= 0 || availH <= 0 || scale <= 0) return f;

    // Width: a guard, not an input to the layout. The strip never gets narrower than the
    // minimap it sits under (cmdPanelW's miniSize floor), and that floor tracks UI SCALE
    // exactly as the type does, so a correctly chosen scale always clears it -- this
    // catches the cases that floor does not cover rather than quietly reflowing.
    if (float(colBudget) * glyphW * scale > availW) return f;

    // HEIGHT sets the row count -- the ONLY thing that does. Rows are dropped from the
    // end of the caller's list, so that list is a priority order.
    f.rowH = glyphH * scale + rowPad;
    if (f.rowH <= 0) return f;
    int fit = int(availH / f.rowH);
    if (fit <= 0) return f;

    f.visible = true;
    f.rows = std::min(fit, candidates);
    return f;
}

}  // namespace ta::hud

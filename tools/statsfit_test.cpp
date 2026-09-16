// Layout test for the minimap-strip stats readout (src/client/statsfit.h).
//
// This exists because the behaviour it covers CANNOT be checked from a screenshot on a
// headless build: SDL's dummy and offscreen video drivers ignore SDL_SetWindowSize, so a
// harness that "resizes" the window and captures each size silently writes the same size
// five times and looks like it passed. (That happened while this panel was being built.)
// The fit is pure arithmetic, so it is tested as arithmetic.
//
// The properties under test are the rules the panel is built on: the type size is FIXED
// (the window never resizes it), the row count follows the HEIGHT alone, and width is
// only ever a visibility guard.

#include "client/statsfit.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(const std::string& what, bool ok) {
    std::printf("  %-58s %s\n", what.c_str(), ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// The block font the mana readout uses: 5x7 cells advancing 6 per character.
constexpr float kGlyphW = 6.0f, kGlyphH = 7.0f;
constexpr int kBudget = 13;    // 5 label + 1 gap + 7 value

// What the engine uses: a fixed scale off UI SCALE, never off the window.
float pxFor(float uiScale) { return 2.2f * uiScale; }
// The narrowest the strip ever gets, from cmdPanelW's miniSize floor, minus padding.
float narrowestStrip(float uiScale) {
    return 180.0f * uiScale + 12.0f - 2.0f * std::max(4.0f, 6.0f * uiScale);
}

ta::hud::StatsFit fit(float w, float h, int rows = 9, float uiScale = 0.75f) {
    return ta::hud::fitStats(rows, w, h, kBudget, kGlyphW, kGlyphH, pxFor(uiScale),
                              /*rowPad=*/6.0f * uiScale);
}

}  // namespace

int main() {
    std::printf("statsfit_test\n");

    // THE TYPE SIZE IS FIXED. The same UI SCALE gives the same row pitch whatever the
    // window is doing -- this is the rule the panel exists to honour, so it is asserted
    // directly rather than inferred.
    {
        auto a = fit(147, 600), b = fit(295, 600), c = fit(1000, 600);
        check("row pitch identical across strip widths",
              a.visible && b.visible && c.visible && a.rowH == b.rowH && b.rowH == c.rowH);
        check("row count identical across strip widths",
              a.rows == b.rows && b.rows == c.rows);
    }

    // THE FIXED SCALE CLEARS THE NARROWEST STRIP, at every UI SCALE -- not just the
    // default. Both the strip's floor and the type track uiScale, so if this holds at one
    // setting it should hold at all of them; it is checked across the slider's range
    // because "it looked right at 100%" is exactly how the other setting breaks.
    for (float ui : {0.75f, 1.0f, 1.25f, 1.5f, 2.0f}) {
        float need = float(kBudget) * kGlyphW * pxFor(ui);
        check("budget clears the narrowest strip at uiScale " + std::to_string(ui).substr(0, 4),
              need <= narrowestStrip(ui));
    }

    // ROW COUNT FOLLOWS HEIGHT ONLY.
    {
        // Heights chosen to actually STRADDLE the row pitch. The fixed type is small
        // enough that 150px already fits all nine rows, so comparing 600 against 150
        // compares two capped values and proves nothing -- it passed as a tautology until
        // the numbers were checked against the real pitch.
        auto tall = fit(147, 600), mid = fit(147, 60), low = fit(147, 20);
        check("shorter gap -> fewer rows", tall.rows > mid.rows && mid.rows > low.rows);
        check("rows never exceed the gap",
              tall.rows * tall.rowH <= 600.0f && mid.rows * mid.rowH <= 60.0f);
        check("no height -> hidden", !fit(147, 4).visible);
        check("never more rows than we have", fit(147, 10000).rows == 9);
    }

    // WIDTH IS A GUARD, NOT A LAYOUT INPUT: too narrow hides the panel outright instead
    // of reflowing it or letting it spill over the map.
    {
        check("too narrow -> hidden", !fit(40, 600).visible);
        auto f = fit(147, 600);
        check("visible panel never exceeds its width",
              f.visible && float(kBudget) * kGlyphW * pxFor(0.75f) <= 147.0f);
    }

    // A CONTINUOUS RESIZE: walking the window smaller must shed rows monotonically and
    // never leave a row overflowing. A layout that cached geometry would hold a stale row
    // count while the space it draws into kept shrinking.
    {
        bool monotone = true, contained = true;
        int prev = 99;
        for (int h = 600; h >= 0; h -= 7) {
            auto f = fit(147, float(h));
            int n = f.visible ? f.rows : 0;
            if (n > prev) monotone = false;
            if (f.visible && f.rows * f.rowH > float(h)) contained = false;
            prev = n;
        }
        check("live resize: row count never grows as the gap shrinks", monotone);
        check("live resize: rows always fit the gap they were given", contained);
    }

    std::printf(failures ? "statsfit_test: %d FAILED\n" : "statsfit_test: all passed\n", failures);
    return failures ? 1 : 0;
}

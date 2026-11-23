#pragma once

// The interface's plain text, in the UI FONT the player chose (Options > Interface):
//
//   MODERN  (the default) -- Noto Sans Bold (client/uifont.h), the chat box's face;
//   CLASSIC -- the fonts the client always had: the 5x7 block font (client/blockfont.h)
//              and the retail text faces (bodfontbody, the Times GAFs).
//
// Fonts are routed by ROLE, never by file: every caller of uiText()/uiTextWidth() (the
// block-font role: lobby, rooms, menus, Options, dialogs, HUD readouts) and every Font
// loaded with FontRole::Text (client/font.h) follows the style; a Font loaded as
// FontRole::Art (font48, Lombardic, decorativesm, bodfontdecor) is retail lettering and
// never changes. The style is read at draw time, so switching it is live: the next
// frame lays out and draws in the other face.
//
// Sizes. `px` is the size of one block-font cell pixel, as it always was: the caps are
// 7 * px tall, from `y` down to the baseline at y + 7 * px, in both styles, so a layout
// keeps its line positions. MODERN text is proportional and has descenders, which reach
// uiTextDescent(px) below the baseline (CLASSIC: 0), and is drawn in the case given --
// names, maps and mod packs keep their own capitals -- where CLASSIC capitalizes.
// Widths always come from uiTextWidth() (never chars * 6 * px), and what it returns is
// exactly what uiText() advances.
//
// Display only: nothing here touches the simulation.

#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tak {

enum class UiFontStyle : uint8_t { Modern = 0, Classic = 1 };
UiFontStyle uiFontStyle();
void setUiFontStyle(UiFontStyle style);
inline bool modernUiText() { return uiFontStyle() == UiFontStyle::Modern; }

// Width of `s` at cell size `px` in the current style (CLASSIC: 6 * px a glyph, 4 * px
// a space or a character the block font lacks).
float uiTextWidth(const std::string& s, float px);
void uiText(SDL_Renderer* r, const std::string& s, float x, float y, float px, SDL_Color c);
// The same, with a dark drop shadow (menus over art).
void uiTextShadow(SDL_Renderer* r, const std::string& s, float x, float y, float px, SDL_Color c);

// Vertical extent around the cap band [y, y + 7 * px]: how far the ink of the current
// face can reach below the baseline (descenders) and above the cap line (accents).
float uiTextDescent(float px);
float uiTextAscent(float px);
// How far `s`'s own ink reaches below the baseline: 0 for text without descenders (and
// always 0 in CLASSIC), so a box can keep caps-only labels where they always sat and
// make room only when a 'g' or 'y' is there to need it.
float uiTextInkDescent(const std::string& s, float px);

// The cell size, at most `px` and at least `minPx`, at which `s` fits `maxW`.
float uiTextFitPx(const std::string& s, float px, float maxW, float minPx = 1);
// `s` cut to fit `maxW`: whole characters only, ending in "..." when cut (and the "..."
// counts against maxW). fitStart keeps the END instead ("...tail", for folder paths).
std::string uiTextFit(const std::string& s, float px, float maxW, bool ellipsis = true);
std::string uiTextFitStart(const std::string& s, float px, float maxW);
// Word-wrap `s` to rows no wider than `maxW` (a word wider than a row is split between
// characters). Always at least one row.
std::vector<std::string> uiTextWrap(const std::string& s, float px, float maxW);

// MODERN's TrueType size for a cap height in pixels: the UiFont scale whose 'H' is
// capPx tall (quarter-pixel steps, as the face sets them). Shared with Font's text role.
float uiFontScaleForCap(float capPx);

// Free the TrueType glyph sheets (renderer reset / session teardown / exit).
void releaseUiTextGlyphs();

}  // namespace tak

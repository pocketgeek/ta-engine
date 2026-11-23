// uifont_test -- the interface's TrueType face (client/uifont.h) and the plain-text API
// on top of it (client/uitext.h), with its UI FONT setting.
//
// The face loads with sane metrics; widths add up (and kern); text wraps to a width
// with it (client/chatbox.h); UTF-8 decodes strictly, invalid bytes stand for
// themselves as Latin-1, and characters the face lacks draw as '?' one per code
// point; glyph sheets are built once per pixel size -- not per draw, not per colour --
// kept for every size in use up to a bound, rebuilt after destroyGlyphs(), and put ink
// on the target, from one sheet per device size under a render scale.
//
// The text API, in both styles: what uiTextWidth() measures is what uiText() advances
// (ink starts at x and ends within it), the caps sit in the cell band [y, y + 7 px],
// fit and wrap stay within their widths on whole characters, and switching the style
// switches both measuring and drawing. UI FONT: MODERN by default and for a settings
// file without the key, round-trips CLASSIC, is visible to the DEFAULTS button and is
// reset by it, and applyRuntimeSettings() applies it.

#include "client/chatbox.h"
#include "client/runtimesettings.h"
#include "client/settings.h"
#include "client/uifont.h"
#include "client/uitext.h"

#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

static int g_fail = 0;
static void check(bool ok, const char* what) {
    std::printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

static bool same(const std::vector<uint32_t>& a, std::initializer_list<uint32_t> b) {
    return a == std::vector<uint32_t>(b);
}

int main() {
    std::printf("uifont_test\n");
    tak::UiFont f;
    check(f.ok(), "the embedded Noto Sans Bold loads");
    if (!f.ok()) return 1;

    // ---- metrics ----
    float top = 0, h = 0;
    f.vbounds("Hdgjpqy", 1, top, h);
    check(h > 10.5f && h < 13 && top > 2 && top < 5, "ink of Hdgjpqy in a 16 px line: ~11.8 tall, ~3.6 down");
    float capTop = 0, capH = 0;
    f.vbounds("H", 1, capTop, capH);
    check(capH > 7.5f && capH < 9.5f && capTop > top, "cap height ~8.4 px, below the ascender line");
    const float wH = f.width("H"), wi = f.width("i"), wSp = f.width(" ");
    check(wH > wi && wi > 0 && wSp > 0, "proportional: H wider than i, space has width");
    check(f.width("") == 0, "the empty string is 0 wide");

    // ---- widths ----
    const std::string hello = "Hello, world!";
    const float w1 = f.width(hello);
    check(w1 > 65 && w1 < 82, "\"Hello, world!\" in a 16 px line is ~74 px wide");
    check(std::fabs(f.width(hello, 2) - 2 * w1) < 2, "twice the scale, twice the width");
    check(std::fabs(f.width("AV") - (f.width("A") + f.width("V"))) > 0.1f, "kerning: AV is not A + V");
    float sum = 0;
    for (char c : std::string("xxxx")) sum += f.width(std::string(1, c));
    check(std::fabs(f.width("xxxx") - sum) < 0.01f, "no kerning pair: widths add up");
    check(f.pixelSize(1) == 16 && f.pixelSize(1.3f) == 20.75f && f.pixelSize(0.01f) == 1,
          "pixel size: 16 px at 1, quarter-pixel steps, never below 1");

    // ---- wrapping with the face ----
    auto width = [&](const std::string& t) { return f.width(t); };
    const std::string para = "the quick brown fox jumps over the lazy dog again and again";
    const float wrapW = 150;
    const auto rows = tak::chatbox::wrap(para, wrapW, wrapW, width);
    bool fits = rows.size() >= 3;
    std::string joined;
    for (const auto& r : rows) {
        fits = fits && f.width(r) <= wrapW && !r.empty();
        joined += (joined.empty() ? "" : " ") + r;
    }
    check(fits, "a sentence wraps to rows that each fit 150 px");
    check(joined == para, "and the rows rejoin to the sentence");
    const auto broken = tak::chatbox::wrap(std::string(60, 'W'), 100, 100, width);
    bool bfits = broken.size() > 1;
    for (const auto& r : broken) bfits = bfits && f.width(r) <= 100;
    check(bfits, "an unbroken word is split between characters to fit");
    const auto u = tak::chatbox::wrap("\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", 20, 20, width);
    bool whole = u.size() > 1;
    for (const auto& r : u) whole = whole && r.size() % 2 == 0;
    check(whole, "a split never cuts a UTF-8 character");
    // Stray bytes (a run of continuation bytes, or one after a valid character) are each
    // a glyph of their own, so wrap may break between them like any other characters.
    for (const std::string& stray : {std::string(150, '\x80'), "\xE2\x80\x8B" + std::string(150, '\x80')}) {
        const auto sr = tak::chatbox::wrap(stray, 200, 300, width);
        bool sfits = sr.size() > 1;
        std::string sj;
        for (size_t k = 0; k < sr.size(); ++k) {
            sfits = sfits && f.width(sr[k]) <= (k == 0 ? 200 : 300);
            sj += sr[k];
        }
        check(sfits && sj == stray, "a run of stray UTF-8 bytes wraps to rows that fit");
    }

    // ---- UTF-8 ----
    using tak::decodeUtf8Lenient;
    check(same(decodeUtf8Lenient("A\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"), {'A', 0xE9, 0x20AC, 0x1F600}),
          "1-, 2-, 3- and 4-byte sequences decode");
    check(same(decodeUtf8Lenient("caf\xE9"), {'c', 'a', 'f', 0xE9}), "a Latin-1 byte stands for itself");
    check(same(decodeUtf8Lenient("\xC3"), {0xC3}) && same(decodeUtf8Lenient("\xE2\x82"), {0xE2, 0x82}),
          "a truncated sequence: its bytes, one each");
    check(same(decodeUtf8Lenient("\xC0\xAF"), {0xC0, 0xAF}) && same(decodeUtf8Lenient("\xE0\x81\x81"), {0xE0, 0x81, 0x81}),
          "overlong forms are not decoded");
    check(same(decodeUtf8Lenient("\xED\xA0\x80"), {0xED, 0xA0, 0x80}) &&
              same(decodeUtf8Lenient("\xF4\x90\x80\x80"), {0xF4, 0x90, 0x80, 0x80}),
          "surrogates and values past U+10FFFF are not decoded");
    check(same(decodeUtf8Lenient("\xC3x"), {0xC3, 'x'}), "a lead byte before a non-continuation: both kept");
    check(tak::UiFont::mapped(0x1F600) == '?' && tak::UiFont::mapped(0x4E2D) == '?' &&
              tak::UiFont::mapped(7) == '?' && tak::UiFont::mapped(0x85) == '?',
          "emoji, CJK and control codes map to '?'");
    check(tak::UiFont::mapped(0xE9) == 0xE9 && tak::UiFont::mapped(0x20AC) == 0x20AC &&
              tak::UiFont::mapped(0x2019) == 0x2019,
          "Latin-1 and the typographic extras are carried");
    check(std::fabs(f.width("\xE4\xB8\xAD\xF0\x9F\x98\x80") - f.width("??")) < 0.01f,
          "two missing characters measure as two '?'");
    check(std::fabs(f.width("a\xFF\xFE") - f.width("a\xC3\xBF\xC3\xBE")) < 0.01f,
          "invalid bytes measure as their Latin-1 letters");

    // ---- the glyph sheet ----
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, 400, 120, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* ren = surf ? SDL_CreateSoftwareRenderer(surf) : nullptr;
    check(ren != nullptr, "a software renderer for the sheet");
    if (ren) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        check(f.sheetBuilds() == 0, "no sheet before the first draw");
        for (int i = 0; i < 5; ++i)
            f.drawSolid(ren, hello, 10, 10, 1, {255, 255, 255, 255});
        f.drawSolid(ren, hello, 11, 12, 1, {0, 0, 0, 128});
        f.width(hello, 3);   // measuring at another size builds nothing
        check(f.sheetBuilds() == 1, "one sheet for many draws, colours and positions");
        check(f.sheetW() >= 256 && f.sheetH() > 0 && f.sheetH() <= f.sheetW() * 2, "the sheet is roughly square");
        f.drawSolid(ren, hello, 10, 40, 1.5f, {255, 255, 255, 255});
        f.drawSolid(ren, hello, 10, 40, 1.5f, {255, 255, 255, 255});
        check(f.sheetBuilds() == 2 && f.sheetCount() == 2, "a new pixel size builds its own sheet once");
        f.drawSolid(ren, hello, 10, 40, 1.5f + 0.001f, {255, 255, 255, 255});
        check(f.sheetBuilds() == 2, "a size within the same quarter pixel does not");
        f.drawSolid(ren, hello, 10, 10, 1, {255, 255, 255, 255});
        check(f.sheetBuilds() == 2, "going back to the first size builds nothing (both kept)");
        {
            // A live resize: every frame a slightly different size (16 -> 17.6 px over a
            // hundred frames) draws from whole-pixel sheets, not one sheet per frame.
            const int b0 = f.sheetBuilds();
            for (int i = 0; i <= 100; ++i) f.drawSolid(ren, hello, 10, 70, 1.0f + 0.001f * float(i), {255, 255, 255, 255});
            check(f.sheetBuilds() - b0 <= 2, "a size that creeps each frame rebuilds only at whole pixels");
        }
        for (int i = 0; i < 40; ++i) f.drawSolid(ren, hello, 10, 10, 0.5f + 0.25f * float(i), {255, 255, 255, 255});
        check(f.sheetCount() <= tak::UiFont::kMaxSheets, "forty sizes: the cache stays within its bound");
        const int before = f.sheetBuilds();
        f.drawSolid(ren, hello, 10, 10, 0.5f + 0.25f * 39, {255, 255, 255, 255});
        check(f.sheetBuilds() == before, "the most recent size is still held");
        f.destroyGlyphs();
        f.drawSolid(ren, hello, 10, 40, 1.5f, {255, 255, 255, 255});
        check(f.sheetBuilds() == before + 1 && f.sheetCount() == 1,
              "destroyGlyphs (renderer reset): all freed, the next draw rebuilds");
        f.drawSolid(ren, "\xE4\xB8\xAD\xFF\x01", 10, 80, 1, {255, 255, 255, 255});
        check(f.sheetBuilds() == before + 2, "odd text draws without trouble");
        f.drawSolid(ren, hello, 10, 10, 6, {255, 255, 255, 255});   // 300% x UI 200%
        check(f.sheetW() <= 4096 && f.sheetH() <= 4096, "the 96 px sheet stays within 4096 x 4096");
        f.destroyGlyphs();

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        f.drawSolid(ren, "Hello", 10, 10, 1, {255, 255, 255, 255});
        SDL_RenderPresent(ren);
        int inkMinX = 999, inkMaxX = -1, inkMinY = 999, inkMaxY = -1;
        SDL_LockSurface(surf);
        for (int y = 0; y < surf->h; ++y)
            for (int x = 0; x < surf->w; ++x) {
                const uint8_t* p = static_cast<const uint8_t*>(surf->pixels) + y * surf->pitch + x * 4;
                if (p[0] > 128) {
                    inkMinX = std::min(inkMinX, x); inkMaxX = std::max(inkMaxX, x);
                    inkMinY = std::min(inkMinY, y); inkMaxY = std::max(inkMaxY, y);
                }
            }
        SDL_UnlockSurface(surf);
        float hTop = 0, hH = 0;
        f.vbounds("Hello", 1, hTop, hH);
        check(inkMaxX > inkMinX && inkMinX >= 10 && inkMinX <= 12 && float(inkMaxX - 10) <= f.width("Hello") + 1,
              "ink starts at x and ends within the measured width");
        check(std::fabs(float(inkMinY) - (10 + hTop)) <= 1 && std::fabs(float(inkMaxY - inkMinY + 1) - hH) <= 1.5f,
              "ink rows match vbounds");
        f.destroyGlyphs();

        // ---- device pixels: under a render scale the sheet is set at the scaled size ----
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderSetScale(ren, 2, 2);
        f.drawSolid(ren, "Hello", 5, 5, 1, {255, 255, 255, 255});
        SDL_RenderSetScale(ren, 1, 1);
        check(f.sheetCount() == 1 && f.sheetBuilds() > 0, "a render scale of 2 draws from one sheet");
        f.drawSolid(ren, "Hello", 10, 10, 2, {255, 255, 255, 255});
        check(f.sheetCount() == 1, "...the same sheet as scale 2 at a render scale of 1");
        f.destroyGlyphs();
        SDL_DestroyRenderer(ren);
    }
    if (surf) SDL_FreeSurface(surf);

    // ---- the text API (client/uitext.h), both styles ----
    SDL_Surface* ts = SDL_CreateRGBSurfaceWithFormat(0, 900, 160, 32, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer* tr = ts ? SDL_CreateSoftwareRenderer(ts) : nullptr;
    check(tr != nullptr, "a software renderer for the text API");
    struct Ink { int x0 = 9999, x1 = -1, y0 = 9999, y1 = -1; };
    auto inkOf = [&](const std::string& t, float x, float y, float px) {
        SDL_SetRenderDrawColor(tr, 0, 0, 0, 255);
        SDL_RenderClear(tr);
        tak::uiText(tr, t, x, y, px, {255, 255, 255, 255});
        SDL_RenderPresent(tr);
        Ink k;
        SDL_LockSurface(ts);
        for (int yy = 0; yy < ts->h; ++yy)
            for (int xx = 0; xx < ts->w; ++xx)
                if (static_cast<const uint8_t*>(ts->pixels)[yy * ts->pitch + xx * 4] > 100) {
                    k.x0 = std::min(k.x0, xx); k.x1 = std::max(k.x1, xx);
                    k.y0 = std::min(k.y0, yy); k.y1 = std::max(k.y1, yy);
                }
        SDL_UnlockSurface(ts);
        return k;
    };
    check(tak::uiFontStyle() == tak::UiFontStyle::Modern, "MODERN is the style before any setting");
    for (const auto style : {tak::UiFontStyle::Modern, tak::UiFontStyle::Classic}) {
        tak::setUiFontStyle(style);
        const bool modern = style == tak::UiFontStyle::Modern;
        const char* tag = modern ? "MODERN" : "CLASSIC";
        char what[96];
        if (!tr) break;
        const float px = 2;
        const std::string line = "SETTINGS HOST READY";
        const Ink k = inkOf(line, 20, 40, px);
        const float w = tak::uiTextWidth(line, px);
        std::snprintf(what, sizeof what, "%s: ink starts at x and ends within the measured width", tag);
        check(k.x1 > k.x0 && k.x0 >= 20 && k.x0 <= 23 && float(k.x1 + 1) <= 20 + w + 1, what);
        std::snprintf(what, sizeof what, "%s: ...and fills it (no hidden slack)", tag);
        check(float(k.x1 + 1) >= 20 + w - 2.5f * px, what);
        std::snprintf(what, sizeof what, "%s: the caps fill the cell band [y, y + 7 px]", tag);
        check(std::abs(k.y0 - 40) <= 1 && std::abs(k.y1 + 1 - (40 + 14)) <= 1, what);
        std::snprintf(what, sizeof what, "%s: widths add up across a split", tag);
        check(std::fabs(tak::uiTextWidth("SETTINGS", px) + tak::uiTextWidth(" HOST READY", px) - w) < (modern ? 1.0f : 0.01f), what);
        std::snprintf(what, sizeof what, "%s: descenders %s", tag, modern ? "reach below the baseline" : "do not exist");
        const Ink g = inkOf("gjpqy", 20, 40, px);
        check(modern ? (g.y1 + 1 > 54 && float(g.y1 + 1) <= 54 + tak::uiTextDescent(px) + 1)
                     : tak::uiTextDescent(px) == 0 && g.y1 + 1 <= 54, what);
        std::snprintf(what, sizeof what, "%s: a text's own descent: none for caps, the ink's for 'gjpqy'", tag);
        const float gd = tak::uiTextInkDescent("gjpqy", px);
        check(tak::uiTextInkDescent(line, px) == 0 &&
                  (modern ? gd > 0 && gd <= tak::uiTextDescent(px) && std::fabs(54 + gd - float(g.y1 + 1)) <= 1.5f
                          : gd == 0), what);
        // Fit: whole characters, within the width, "..." when cut.
        const std::string longName = "Lord Kirenna's Great Northern Expanse (8 players)";
        const std::string cut = tak::uiTextFit(longName, px, 200);
        std::snprintf(what, sizeof what, "%s: fit cuts to the width and ends in ...", tag);
        check(tak::uiTextWidth(cut, px) <= 200 && cut.size() > 3 && cut.substr(cut.size() - 3) == "...", what);
        std::snprintf(what, sizeof what, "%s: text that fits is untouched", tag);
        check(tak::uiTextFit("OK", px, 200) == "OK", what);
        const std::string path = "/home/someone/games/Total Annihilation Kingdoms/data";
        const std::string tail = tak::uiTextFitStart(path, px, 180);
        std::snprintf(what, sizeof what, "%s: fitStart keeps the end of a path", tag);
        check(tak::uiTextWidth(tail, px) <= 180 && tail.substr(0, 3) == "..." &&
                  path.substr(path.size() - (tail.size() - 3)) == tail.substr(3), what);
        const std::string accents = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9";
        const std::string ucut = tak::uiTextFit(accents, px, 40, false);
        std::snprintf(what, sizeof what, "%s: fit never cuts a UTF-8 character", tag);
        check(ucut.size() % 2 == 0 && tak::uiTextWidth(ucut, px) <= 40, what);
        // Wrap.
        const auto rows = tak::uiTextWrap("THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG AGAIN AND AGAIN", px, 160);
        bool fits = rows.size() >= 3;
        for (const auto& r : rows) fits = fits && tak::uiTextWidth(r, px) <= 160 && !r.empty();
        std::snprintf(what, sizeof what, "%s: wrap gives rows that each fit", tag);
        check(fits, what);
    }
    tak::setUiFontStyle(tak::UiFontStyle::Classic);
    const float classicW = tak::uiTextWidth("Hello world", 2);
    check(classicW == (10 * 6 + 4) * 2.0f, "CLASSIC: 6 px a glyph, 4 px a space");
    tak::setUiFontStyle(tak::UiFontStyle::Modern);
    check(std::fabs(tak::uiTextWidth("Hello world", 2) - classicW) > 1 &&
              tak::uiTextWidth("Hello world", 2) < tak::uiTextWidth("HELLO WORLD", 2),
          "MODERN: proportional, in the case given (lower case is narrower)");
    // Measuring is free; drawing a dozen sizes keeps a dozen sheets, and releasing frees them.
    tak::releaseUiTextGlyphs();
    if (tr) {
        for (float px : {1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.8f, 2.0f, 2.2f, 2.5f, 3.0f, 3.4f})
            tak::uiText(tr, "Abc", 10, 10, px, {255, 255, 255, 255});
        const int builds = tak::uiFont().sheetBuilds();
        for (int frame = 0; frame < 3; ++frame)
            for (float px : {1.1f, 1.2f, 1.3f, 1.4f, 1.5f, 1.6f, 1.8f, 2.0f, 2.2f, 2.5f, 3.0f, 3.4f})
                tak::uiText(tr, "Abc", 10, 10, px, {255, 255, 255, 255});
        check(tak::uiFont().sheetBuilds() == builds && tak::uiFont().sheetCount() == 12,
              "twelve sizes a frame: twelve sheets, none rebuilt on later frames");
        tak::releaseUiTextGlyphs();
        check(tak::uiFont().sheetCount() == 0, "releaseUiTextGlyphs frees every sheet");
        SDL_DestroyRenderer(tr);
    }
    if (ts) SDL_FreeSurface(ts);

    // ---- the UI FONT setting ----
    {
        namespace fs = std::filesystem;
        std::mt19937_64 rng(std::random_device{}());
        const fs::path dir = fs::temp_directory_path() / ("tak-uifont-" + std::to_string(rng()));
        fs::create_directories(dir);
        const fs::path file = dir / "settings.ini";
        tak::setSettingsPath(file.string());
        check(tak::Settings{}.uiFont == 0, "default: MODERN");
        std::ofstream(file) << "uiScale = 1.5\n";
        check(tak::loadSettings().uiFont == 0, "a settings file without the key: MODERN");
        tak::Settings s;
        s.uiFont = 1;
        check(tak::saveSettings(s), "save");
        check(tak::loadSettings().uiFont == 1, "CLASSIC round-trips");
        {
            std::ifstream in(file);
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            check(text.find("uiFont = classic\n") != std::string::npos, "written as uiFont = classic");
        }
        s.uiFont = 0;
        tak::saveSettings(s);
        check(tak::loadSettings().uiFont == 0, "MODERN round-trips");
        std::ofstream(file) << "uiFont = 7\n";
        check(tak::loadSettings().uiFont == 0, "an unknown value: MODERN");
        tak::Settings a, b;
        b.uiFont = 1;
        check(!(a == b), "visible to operator== (the DEFAULTS button)");
        check(tak::preferenceDefaults(b).uiFont == 0, "DEFAULTS resets it to MODERN");
        tak::applyRuntimeSettings(b);
        check(tak::uiFontStyle() == tak::UiFontStyle::Classic, "applyRuntimeSettings applies CLASSIC");
        tak::applyRuntimeSettings(a);
        check(tak::uiFontStyle() == tak::UiFontStyle::Modern, "...and MODERN");
        tak::setSettingsPath("");
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    std::printf(g_fail ? "uifont_test: %d FAILED\n" : "uifont_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

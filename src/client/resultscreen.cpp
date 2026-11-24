#include "client/resultscreen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>
#include <vector>

#include "util/png.h"
#include "client/appquit.h"
#include "client/uitext.h"
#include "client/hidpi.h"
#include "client/cursors.h"
#include "client/dev.h"
#include "client/gpuvram.h"
#include "client/guiart.h"
#include "client/artscale.h"
#include "sim/sides.h"
#include "client/font.h"
#include "client/menumusic.h"
#include "client/menuclick.h"
#include "client/menusfx.h"
#include "client/hotkeys.h"
#include "client/settings.h"
#include "gui/gui.h"

namespace ta {

namespace {

bool inRect(const SDL_FRect& r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// The result plate. A data set may ship victory<side>.gui / defeat.gui plates (a
// mod's); retail TA has none: its skirmish and multiplayer end is the statistics
// backdrop bitmaps/outcome0.pcx (ENDMSN's non-campaign backdrop, 0x41f0a0,
// docs/retail-campaign.md section 4.3; ENDMULTI.GUI's OK at 479,414), whose column
// headers (Name, Kills, Losses, Energy/Metal Produced, Excess Energy/Metal, Score)
// are painted in the art. These defaults are that layout, measured off the bitmap;
// a .gui that parses overrides them.
struct Geom {
    Rect menuBtn{359, 414, 100, 40};
    Rect okBtn{479, 414, 115, 40};
    Rect title{20, 360, 322, 50};
    Rect help{20, 412, 322, 22};
    // Column rects come from the FIRST row's gadgets; later rows just step by `pitch`.
    // A zero-width column has no place on the plate and is not drawn.
    Rect logo{16, 96, 16, 16};
    Rect name{34, 94, 72, 20};
    Rect units{0, 94, 0, 20};
    Rect kills{110, 94, 70, 20};
    Rect losses{185, 94, 69, 20};
    Rect time{0, 94, 0, 20};
    Rect score{556, 94, 70, 20};
    float headY = 64, pitch = 22;
    int maxRows = 9;
    bool headersInArt = true;   // outcome0.pcx paints its own column headers
    std::string bgPcx = "bitmaps/outcome0.pcx";
    std::string bgGaf, bgSeq, btnGaf;
    // Each button's click: a plate .gui binds its own in its states. TA's plate has no
    // .gui; retail plays BIGBUTTON for its big buttons (gamedata/allsound.tdf:
    // butmain1; docs/retail-gui.md section 1.6).
    std::string menuSound = "butmain1.wav", proceedSound = "butmain1.wav";
};

Geom loadGeom(const hpi::Vfs& vfs, bool victory, int faction) {
    Geom g;
    const std::string path = "guis/" +
                             (victory && faction >= 0 ? "victory" + sides::lowerPrefix(faction) : std::string("defeat")) +
                             ".gui";
    if (!vfs.has(path)) return g;
    try {
        gui::Gui ui = gui::parse(vfs.read(path), path);
        auto take = [&](const char* n, Rect& r) {
            if (const gui::Gadget* w = ui.find(n))
                r = {float(w->x), float(w->y), float(w->w), float(w->h)};
        };
        take("MainMenu", g.menuBtn);
        take("Proceed", g.okBtn);
        take("Static0", g.title);
        take("HelpText", g.help);
        take("Logo", g.logo);
        take("PlayerName", g.name);
        take("UnitsBuilt", g.units);
        take("Kills", g.kills);
        take("Losses", g.losses);
        take("Time", g.time);
        take("Score", g.score);
        if (const gui::Gadget* hdr = ui.find("Static1")) g.headY = float(hdr->y);
        // Row pitch: the gap between the first two PlayerName gadgets. They all share
        // the name, so walk the gadget list instead of find()ing it twice.
        std::vector<int> ys;
        for (const auto& w : ui.gadgets)
            if (w.name == "PlayerName") ys.push_back(w.y);
        if (ys.size() >= 2) g.pitch = float(ys[1] - ys[0]);
        if (!ys.empty()) g.maxRows = int(ys.size());
        if (!ui.gadgets.empty() && !ui.gadgets[0].imgs.empty()) {
            const gui::ImgRef& im = ui.gadgets[0].imgs[0];
            g.bgGaf = im.gaf;
            g.bgSeq = im.seq;
            g.bgPcx.clear();
            g.headersInArt = false;
        }
        if (const gui::Gadget* b = ui.find("Proceed"); b && !b->imgs.empty())
            g.btnGaf = b->imgs[0].gaf;
        if (const gui::Gadget* b = ui.find("MainMenu")) g.menuSound = menuclick::soundOf(*b);
        if (const gui::Gadget* b = ui.find("Proceed")) g.proceedSound = menuclick::soundOf(*b);
    } catch (...) {}
    if (g.btnGaf.empty()) g.btnGaf = g.bgGaf;
    return g;
}

// Retail's team-colour ramp, indexed by the lobby colour slot. Only the row swatch
// uses it here, so a representative mid-tone of each is enough.
SDL_Color slotColor(int slot) {
    static const SDL_Color kC[10] = {
        {70, 110, 230, 255},  {220, 70, 60, 255},   {250, 250, 250, 255}, {70, 190, 90, 255},
        {40, 40, 46, 255},    {230, 160, 50, 255},  {150, 90, 200, 255},  {80, 205, 210, 255},
        {200, 110, 170, 255}, {150, 140, 110, 255}};
    return kC[size_t(std::clamp(slot, 0, 9))];
}

// TotalA.exe's PlayerColor%d art (0x41e5b1..0x41e5dc): sequence "32xlogos" of
// textures\logos.gaf (loaded into game +0x148d7 / +0x148db at 0x4318e4..0x431911),
// frame = the player's colour, drawn in the game palette (palettes\palette.pal;
// outcome0.pcx carries the same entries but for 7-9 and 246), not guipal.pal.
SDL_Texture* playerColorTile(SDL_Renderer* ren, const hpi::Vfs& vfs, int frame) {
    try {
        const std::string pal = "palettes/palette.pal", gp = "textures/logos.gaf";
        for (auto& sq : gaf::load(vfs.read(gp), gaf::Palette::fromBytes(vfs.read(pal), pal), -1, gp)) {
            if (sq.name != "32xlogos" || sq.frames.empty()) continue;
            if (frame < 0 || size_t(frame) >= sq.frames.size()) frame = 0;
            const auto& f = sq.frames[size_t(frame)];
            if (f.width == 0 || f.height == 0) return nullptr;
            return art::makeTexture(ren, f.rgba, f.width, f.height);
        }
    } catch (...) {}
    return nullptr;
}

std::string mmss(int sec) {
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    return b;
}

}  // namespace

ResultChoice ResultScreen::run(SDL_Renderer* ren, const hpi::Vfs& vfs, bool victory,
                               const std::string& title, bool hasNext,
                               Settings* settings, MenuMusic* music,
                               const ResultStats* stats, ResultChoice skirmishNext,
                               const std::string& menuTip, const std::string& proceedTipIn) {
    CursorSet cursors;
    cursors.load(ren, vfs, settings);
    // Decided per frame below (hardware -> shown, software -> hidden and drawn).
    if (!cursors.ok()) SDL_ShowCursor(SDL_ENABLE);

    SDL_PumpEvents();
    SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);

    int faction = stats ? stats->faction : 0;
    Geom g = loadGeom(vfs, victory, faction);
    SDL_Texture* bg = nullptr;
    if (!g.bgPcx.empty()) {
        try {
            const auto img = gaf::loadPcx(vfs.read(g.bgPcx), g.bgPcx);
            bg = art::makeTexture(ren, img.rgba, img.width, img.height);
        } catch (...) {}
    } else {
        bg = gafTexture(ren, vfs, g.bgGaf, g.bgSeq, 0);
    }
    SDL_Texture* okTex[3] = {};
    SDL_Texture* cancelTex[3] = {};
    for (int i = 0; i < 3; ++i) {
        okTex[i] = gafTexture(ren, vfs, g.btnGaf, "OKButton", i);
        cancelTex[i] = gafTexture(ren, vfs, g.btnGaf, "CancelButton", i);
    }
    std::vector<SDL_Texture*> logos, swatches;
    if (stats) for (const auto& row : stats->rows) {
        logos.push_back(gafTexture(ren, vfs, kFactionLogoGaf, factionLogoSequence(row.side), row.colorSlot));
        swatches.push_back(g.headersInArt ? playerColorTile(ren, vfs, row.colorSlot) : nullptr);
    }
    Font head, body, deco;
    try { head = Font(ren, vfs, "fonts/lombardic (cd).gaf", FontRole::Art); } catch (...) {}
    try { deco = Font(ren, vfs, "fonts/decorativesm.gaf", FontRole::Art); } catch (...) {}
    try { body = Font(ren, vfs, "fonts/b_times new roman (100).gaf", FontRole::Text); } catch (...) {}
    if (!body.ok()) try { body = Font(ren, vfs, "fonts/times new roman (100).gaf", FontRole::Text); } catch (...) {}
    const Font& heading = head.ok() ? head : body;
    auto freeAll = [&] {
        if (bg) gpuvram::destroy(bg);
        for (auto* logo : logos) if (logo) gpuvram::destroy(logo);
        for (auto* swatch : swatches) if (swatch) gpuvram::destroy(swatch);
        for (int i = 0; i < 3; ++i) {
            if (okTex[i]) gpuvram::destroy(okTex[i]);
            if (cancelTex[i]) gpuvram::destroy(cancelTex[i]);
        }
        head.destroyGlyphs();
        if (body.ok()) body.destroyGlyphs();
        if (deco.ok()) deco.destroyGlyphs();
    };

    // Retail's two buttons. "Proceed" means the natural next step: the next mission
    // when there is one, another try after a defeat, otherwise back to the menu.
    ResultChoice proceed = ResultChoice::Menu;
    if (victory && hasNext) proceed = ResultChoice::Next;
    else if (!victory) proceed = ResultChoice::Retry;
    if (skirmishNext != ResultChoice::Menu) proceed = skirmishNext;
    const std::string proceedTip = !proceedTipIn.empty()                ? proceedTipIn
                                 : proceed == ResultChoice::Next       ? "Next Mission"
                                 : proceed == ResultChoice::NewGame    ? "New Game"
                                 : proceed == ResultChoice::GameSelect ? "Game Select"
                                                                       : "Retry";
    const std::string cancelTip = menuTip.empty() ? "Title Screen" : menuTip;
    // A campaign's last win, or a replay's result, has no next step: the right door is inert.
    const bool canProceed = hasNext || skirmishNext != ResultChoice::Menu ||
                            (!victory && !title.empty());

    // A choice plays its button's click on the press, as the title menu's doors do,
    // then the plate stays up until the click has sounded: leaving closes the device.
    MenuSfx sfx(vfs);
    sfx.settings = settings;
    sfx.load(g.menuSound);
    sfx.load(g.proceedSound);
    std::optional<ResultChoice> chosen;
    int leaving = 0;   // frames drawn since the choice; bounded well above the ~140ms clicks
    // Debug: TA_RESULT_PRESS=menu|proceed presses that button on the first frame (as
    // a real left-button press event) and reports the click it played; with
    // TA_SHOT_RESULT and dummy video the screen then waits out the click before it
    // returns (the result_click test).
    const char* press = devEnv("TA_RESULT_PRESS");
    if (press && !*press) press = nullptr;
    bool pressPending = press != nullptr;
    auto choose = [&](ResultChoice c, const std::string& sound) {
        if (chosen) return;
        chosen = c;
        sfx.play(sound);
        if (press)
            std::fprintf(stderr, "result click: choice=%d sound=%s queued=%d\n", int(c),
                         sound.c_str(), int(sfx.busy()));
    };

    // TA's table (TotalA.exe 0x41e420): per row a bar cell for each column, counting up
    // from 0 (0x4a4890: every game tick, +trunc(max(1, value/15)), so ~half a second),
    // its length the value over the column's largest (0x41dc20: at least 10 for Kills
    // and Losses, 100 for the rest). A screenshot (TA_SHOT_RESULT) shows the totals.
    const uint64_t openedMs = SDL_GetTicks64();
    const bool countUp = !devEnv("TA_SHOT_RESULT");
    constexpr int kTaCols = 7;
    int32_t colMax[kTaCols] = {10, 10, 100, 100, 100, 100, 100};
    auto taValue = [](const ResultRow& r, int c) -> int32_t {
        switch (c) {
            case 0: return r.kills;
            case 1: return r.losses;
            case 2: return r.energyProduced;
            case 3: return r.metalProduced;
            case 4: return r.energyWasted;
            case 5: return r.metalWasted;
            default: return r.score;
        }
    };
    if (stats)
        for (const auto& r : stats->rows)
            for (int c = 0; c < kTaCols; ++c) colMax[c] = std::max(colMax[c], taValue(r, c));

    for (;;) {
        if (termRequested()) { freeAll(); return ResultChoice::Menu; }
        if (chosen && (!sfx.busy() || ++leaving > 30)) { freeAll(); return *chosen; }
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(ren, &w, &h);
        GuiLayout lay(w, h);
        SDL_FRect okR = lay.rect(g.okBtn.x, g.okBtn.y, g.okBtn.w, g.okBtn.h);
        SDL_FRect cancelR = lay.rect(g.menuBtn.x, g.menuBtn.y, g.menuBtn.w, g.menuBtn.h);

        if (pressPending) {
            pressPending = false;
            const SDL_FRect& r = std::strcmp(press, "menu") == 0 ? cancelR : okR;
            const float perPoint = float(hidpi::toPixels(ren, 1000)) / 1000.0f;   // event coords are points
            SDL_Event ev{};
            ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = SDL_BUTTON_LEFT;
            ev.button.state = SDL_PRESSED;
            ev.button.x = int((r.x + r.w / 2) / perPoint);
            ev.button.y = int((r.y + r.h / 2) / perPoint);
            SDL_PushEvent(&ev);
        }

        int mx = 0, my = 0;
        hidpi::mousePixels(ren, mx, my);   // layout is in output pixels
        bool okHover = canProceed && inRect(okR, float(mx), float(my));
        bool cancelHover = inRect(cancelR, float(mx), float(my));

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || chosen) continue;
            if (settings && toggleFullscreenKey(e, settings->hotkeys, settings->fullscreen)) {
                saveSettings(*settings);
                continue;
            }
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) choose(ResultChoice::Menu, g.menuSound);
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) choose(proceed, g.proceedSound);
                // Replaying a mission you just won isn't on retail's plate; the key is
                // here because the campaign screen is the only other way back in.
                if (k == SDLK_r) choose(ResultChoice::Retry, g.proceedSound);
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                float bx = float(hidpi::toPixels(ren, e.button.x)), by = float(hidpi::toPixels(ren, e.button.y));
                if (canProceed && inRect(okR, bx, by)) choose(proceed, g.proceedSound);
                else if (inRect(cancelR, bx, by)) choose(ResultChoice::Menu, g.menuSound);
            }
        }
        if (music) music->poll();

        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (bg) {
            SDL_FRect r = lay.rect(0, 0, 640, 480);
            SDL_RenderCopyF(ren, bg, nullptr, &r);
        } else {
            // No plate art: keep the old dark panel so the screen still reads.
            SDL_SetRenderDrawColor(ren, 12, 13, 18, 255);
            SDL_RenderClear(ren);
        }

        const SDL_Color gold{236, 214, 160, 255};
        const SDL_Color dim{196, 178, 138, 255};
        auto text = [&](const Font& f, const std::string& s, const Rect& r, SDL_Color c,
                        bool rightAlign = false, bool overArt = false, bool centred = false) {
            if (s.empty() || r.w <= 0) return;
            float sc = lay.scale * (r.h / 20.0f);
            if (!f.ok()) sc = lay.scale * 1.6f;
            float tw = f.ok() ? float(f.width(s, sc)) : uiTextWidth(s, sc);
            const float available = std::max(1.0f, (r.w - 2) * lay.scale);
            if (tw > available) {
                sc *= available / tw;
                tw = f.ok() ? float(f.width(s, sc)) : uiTextWidth(s, sc);
            }
            float x = lay.px(r.x) + (centred ? (r.w * lay.scale - tw) / 2 : rightAlign ? r.w * lay.scale - tw : 0);
            // draw() takes a baseline, not the gadget's top. Use common font
            // bounds so names, digits and times share one baseline in each row.
            float top = 0, height = 7 * sc;
            if (f.ok()) f.vbounds("Ag0123456789", sc, top, height);
            float y = lay.py(r.y) + (r.h * lay.scale - height) * 0.5f - top;
            // Over the mural, MODERN's flat face needs the dark rim the Times art carries
            // in its glyphs, or tan text on tan paint all but vanishes.
            if (overArt && f.ok() && ta::modernUiText()) {
                const float o = std::max(1.0f, lay.scale);
                for (const auto& [dx, dy] : {std::pair{-o, 0.0f}, {o, 0.0f}, {0.0f, -o}, {0.0f, o}, {o, o}})
                    f.drawSolid(ren, s, x + dx, y + dy, sc, {24, 16, 8, 220});
            }
            if (f.ok()) f.draw(ren, s, x, y, sc, c);
            else uiText(ren, s, x, y, sc, c);
        };

        // Column headers, then one row per player.
        auto col = [&](Rect base, float y) { Rect r = base; r.y = y; return r; };
        if (stats && !stats->rows.empty()) {
            if (!g.headersInArt) {
            text(heading, "Player", col(g.name, g.headY), gold);
            text(heading, "Units", col(g.units, g.headY), gold);
            text(heading, "Kills", col(g.kills, g.headY), gold);
            text(heading, "Losses", col(g.losses, g.headY), gold);
            text(heading, "Time", col(g.time, g.headY), gold);
            text(heading, "Score", col(g.score, g.headY), gold);
            }

            if (g.headersInArt) {
                // TotalA.exe 0x41e420, the outcome0.pcx plate: up to 10 rows from y 93,
                // 20 apart. PlayerColor (16, y, 91x21) with the name over it at x 16;
                // then Kills, Losses, EProduced, MProduced, EWasted, MWasted and Score
                // cells 67x18 at x 112 + 74k (0x4a4660 draws each).
                //
                // The colours are the GUI context's map (+0x8b2, game +0xdcb) that
                // 0x4ac7d0 builds from palettes\guipal.pal -- its 16 EGA colours --
                // each matched to the nearest entry (summed |dR|+|dG|+|dB|, the first
                // on a tie) of the game palette (game +0x143a7, palettes\palette.pal;
                // outcome0.pcx's own palette has the same entries): guipal 20
                // (183,183,183) -> 84 (187,187,187) fills the cell and 0 / 17 -> 0
                // (0,0,0) / 82 (219,219,219) draw its 2 px bevel, top-left / bottom-
                // right (0x4b04b0 -> 0x4b0090); then the 2 px inset well in game
                // +0xdd3 = guipal 8 (85,85,85) -> 90 (91,91,91) and the bar from its
                // left in game +0xdcf = guipal 4 (170,0,0) -> 213 (171,23,0), trunc(
                // shown / column max x (67 - 4)) wide (0x4a4738..0x4a476a); the number
                // centred on it (0x4a4871).
                const int frames = countUp ? int((SDL_GetTicks64() - openedMs) * 30 / 1000) : 1 << 20;
                const int n = std::min<int>(int(stats->rows.size()), 10);
                auto fill = [&](float fx, float fy, float fw, float fh, SDL_Color c) {
                    if (fw <= 0 || fh <= 0) return;
                    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
                    const SDL_FRect r = lay.rect(fx, fy, fw, fh);
                    SDL_RenderFillRectF(ren, &r);
                };
                constexpr SDL_Color kCellFill{187, 187, 187, 255}, kBevelDark{0, 0, 0, 255},
                                    kBevelLight{219, 219, 219, 255}, kWell{91, 91, 91, 255},
                                    kBar{171, 23, 0, 255};
                for (int i = 0; i < n; ++i) {
                    const ResultRow& row = stats->rows[size_t(i)];
                    const float y = 93.0f + 20.0f * float(i);
                    // 0x4a4a1a: an uncompressed frame is texture-mapped from (1,1) to
                    // (w-1,h-1) onto the gadget's corners (0x4c7580), so the 32x32
                    // tile is stretched over 91x21 without its outer row and column.
                    const SDL_FRect sw = lay.rect(16, y, 91, 21);
                    if (swatches[size_t(i)]) {
                        const SDL_Rect src{1, 1, 31, 31};
                        SDL_RenderCopyF(ren, swatches[size_t(i)], &src, &sw);
                    } else {
                        const SDL_Color pc = slotColor(row.colorSlot);
                        SDL_SetRenderDrawColor(ren, pc.r, pc.g, pc.b, 255);
                        SDL_RenderFillRectF(ren, &sw);
                    }
                    const SDL_Color pc = slotColor(row.colorSlot);
                    const SDL_Color ink = (pc.r + pc.g + pc.b) > 500 ? SDL_Color{24, 20, 12, 255} : SDL_Color{250, 246, 230, 255};
                    text(body, row.name, {16, y, 89, 20}, ink);
                    for (int c = 0; c < kTaCols; ++c) {
                        const int32_t value = std::max<int32_t>(taValue(row, c), 0);
                        const int32_t step = std::max<int32_t>(1, int32_t(float(value) / 15.0f));
                        const int32_t shown = int32_t(std::min<int64_t>(value, int64_t(step) * frames));
                        const float x = 112.0f + 74.0f * float(c);
                        fill(x, y, 67, 18, kCellFill);
                        fill(x, y, 67, 2, kBevelDark);           // top, two rows
                        fill(x, y, 2, 18, kBevelDark);           // left, two columns
                        fill(x, y + 16, 67, 2, kBevelLight);     // bottom
                        fill(x + 65, y, 2, 18, kBevelLight);     // right
                        fill(x + 2, y + 2, 63, 14, kWell);
                        const double frac = double(shown) / double(std::max(colMax[c], 1));
                        const int barW = int(std::min(frac, 1.0) * 63.0);
                        fill(x + 2, y + 2, float(barW), 14, kBar);
                        text(body, std::to_string(shown), {x + 2, y + 1, 63, 16}, gold, false, false, true);
                    }
                }
            }
            int n = g.headersInArt ? 0 : std::min<int>(int(stats->rows.size()), g.maxRows);
            for (int i = 0; i < n; ++i) {
                const ResultRow& row = stats->rows[size_t(i)];
                float y = g.name.y + g.pitch * float(i);
                SDL_FRect sw = lay.rect(g.logo.x, g.logo.y + g.pitch * float(i), g.logo.w, g.logo.h);
                if (logos[size_t(i)]) {
                    const float size = std::min(sw.w, sw.h);
                    SDL_FRect emblem{sw.x + (sw.w-size)*0.5f, sw.y + (sw.h-size)*0.5f, size, size};
                    SDL_RenderCopyF(ren, logos[size_t(i)], nullptr, &emblem);
                } else {
                    SDL_Color c = slotColor(row.colorSlot);
                    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
                    SDL_RenderFillRectF(ren, &sw);
                }
                // The local player's row is the one you came here to read.
                SDL_Color rc = row.isLocal ? gold : dim;
                text(body, row.name, col(g.name, y), rc);
                text(body, std::to_string(row.built), col(g.units, y), rc, true);
                text(body, std::to_string(row.kills), col(g.kills, y), rc, true);
                text(body, std::to_string(row.losses), col(g.losses, y), rc, true);
                text(body, mmss(row.timeSec), col(g.time, y), rc, true);
                text(body, std::to_string(row.score), col(g.score, y), rc, true);
            }
        } else {
            // No table (a replay, or a game that ended before anyone was set up):
            // just name the outcome where the table would have been.
            Rect where = g.name;
            where.y = g.headY;
            text(heading, title, where, gold);
        }

        // Title band ("Victory"/"Defeat") in the decorative face the .gui names for it,
        // sized to fill the band. The mission name (campaign only) sits just above it.
        {
            const char* big = stats && !stats->bandWord.empty() ? stats->bandWord.c_str()
                              : victory ? "Victory" : "Defeat";
            // decorativesm's cell is 25x29 but its glyphs hang below the baseline, so
            // scale to a little under the band height or the word laps the help line.
            float sc = deco.ok() ? lay.scale * (g.title.h / 40.0f) : lay.scale * 1.6f;
            const Font& tf = deco.ok() ? deco : head;
            float tw = tf.ok() ? float(tf.width(big, sc)) : uiTextWidth(big, sc * 2);
            if (tw > g.title.w * lay.scale && tw > 0) {
                sc *= (g.title.w * lay.scale) / tw;
                tw = tf.ok() ? float(tf.width(big, sc)) : tw;
            }
            float x = lay.px(g.title.x) + (g.title.w * lay.scale - tw) / 2;
            float top = 0, th = 0;
            if (tf.ok()) tf.vbounds(big, sc, top, th);
            float y = lay.py(g.title.y) + (g.title.h * lay.scale - th) / 2 - top;
            if (tf.ok()) tf.draw(ren, big, x, y, sc, gold);
            else uiText(ren, big, x, lay.py(g.title.y), sc * 2, gold);
            // ...or, for a watcher, who won.
            const std::string& above = stats && !stats->watchLine.empty() ? stats->watchLine : title;
            if (!above.empty() && stats) {
                float tsc = lay.scale;
                float ttw = body.ok() ? float(body.width(above, tsc)) : uiTextWidth(above, tsc);
                text(body, above, {(640 - ttw / lay.scale) / 2, g.title.y - 26, ttw / lay.scale, 20}, dim, false, true);
            }
        }
        text(body, cancelHover ? cancelTip : okHover ? proceedTip : std::string(), g.help, dim, false, true, true);

        auto button = [&](SDL_Texture** tex, const SDL_FRect& r, bool hover, const char* fallback) {
            SDL_Texture* t = tex[hover && tex[1] ? 1 : 0];
            if (t) { SDL_RenderCopyF(ren, t, nullptr, &r); return; }
            SDL_SetRenderDrawColor(ren, hover ? 74 : 52, hover ? 66 : 48, hover ? 42 : 32, 235);
            SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, 150, 132, 84, 255);
            SDL_RenderDrawRectF(ren, &r);
            float px = std::max(1.0f, lay.scale * 1.2f);
            uiText(ren, fallback, r.x + (r.w - uiTextWidth(fallback, px)) / 2,
                          r.y + (r.h - 7 * px) / 2, px, gold);
        };
        button(cancelTex, cancelR, cancelHover, "TITLE");
        button(okTex, okR, okHover, "GO");

        // Honour the hardware-cursor option here too -- see BriefingScreen.
        if (cursors.ok()) {
            const int sc = settings ? settings->cursorScale : 1;
            if (settings && settings->hardwareCursor &&
                cursors.applyHardware(CursorId::Normal, sc)) {
                SDL_ShowCursor(SDL_ENABLE);
            } else {
                SDL_ShowCursor(SDL_DISABLE);
                cursors.draw(ren, CursorId::Normal, mx, my, sc);
            }
        }
        if (const char* sp = devEnv("TA_SHOT_RESULT"); sp && !chosen) {
            std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
            if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4) == 0)
                png::write(sp, w, h, px);
            freeAll();
            return ResultChoice::Menu;
        }
        // Headless (dummy video): no input, so end at the result rather than spin.
        if (const char* drv = SDL_GetCurrentVideoDriver(); drv && !std::strcmp(drv, "dummy") && !chosen) {
            freeAll();
            return ResultChoice::Menu;
        }
        SDL_RenderPresent(ren);
        SDL_Delay(8);
    }
}

}  // namespace ta

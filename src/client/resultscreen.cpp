#include "client/resultscreen.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "util/png.h"
#include "client/appquit.h"
#include "client/blockfont.h"
#include "client/cursors.h"
#include "client/dev.h"
#include "client/gpuvram.h"
#include "client/guiart.h"
#include "client/font.h"
#include "client/menumusic.h"
#include "client/settings.h"
#include "gui/gui.h"

namespace tak {

namespace {

bool inRect(const SDL_FRect& r, float x, float y) {
    return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
}

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// The victory/defeat plates are one layout with a different background per faction.
// These are the shipped rects (victoryara.gui / defeat.gui); the .gui is still parsed
// at runtime so an override that moves a column moves ours too.
struct Geom {
    Rect menuBtn{69, 407, 39, 51};
    Rect okBtn{534, 407, 39, 51};
    Rect title{149, 398, 342, 56};
    Rect help{209, 454, 221, 25};
    // Column rects come from the FIRST row's gadgets; later rows just step by `pitch`.
    Rect logo{81, 72, 17, 17};
    Rect name{106, 70, 131, 20};
    Rect units{239, 70, 73, 20};
    Rect kills{314, 70, 60, 20};
    Rect losses{376, 70, 60, 20};
    Rect time{438, 70, 60, 20};
    Rect score{500, 70, 60, 20};
    float headY = 40, pitch = 22;
    int maxRows = 8;
    std::string bgGaf, bgSeq, btnGaf;
};

// The per-faction victory plate, then the shared defeat plate.
const char* kVictoryGui[5] = {"victoryara", "victorytar", "victoryver",
                              "victoryzon", "victorycre"};

Geom loadGeom(const hpi::Vfs& vfs, bool victory, int faction) {
    Geom g;
    std::string path = "guis/" +
                       std::string(victory ? kVictoryGui[std::clamp(faction, 0, 4)] : "defeat") +
                       ".gui";
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
        }
        if (const gui::Gadget* b = ui.find("Proceed"); b && !b->imgs.empty())
            g.btnGaf = b->imgs[0].gaf;
    } catch (...) {}
    if (g.bgGaf.empty()) {
        static const char* kPlate[5] = {"TAKVAramonScreen", "TAKVTarosScreen", "TAKVVerunaScreen",
                                        "TAKVZhonScreen", "TAKVCreonScreen"};
        g.bgGaf = victory ? kPlate[std::clamp(faction, 0, 4)] : "takdefeatscreen";
        g.bgSeq = victory ? "VictoryBG" : "DefeatBG";
    }
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

std::string mmss(int sec) {
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", sec / 60, sec % 60);
    return b;
}

// Score. Retail's exact formula isn't recoverable from the shipped data, so this is
// ours: it rewards fielding an army and killing with it, and charges for what you
// threw away. Floored at zero so a wipe reads as 0 rather than a negative.
int scoreOf(const ResultRow& r) {
    return std::max(0, r.built * 2 + r.kills * 10 - r.losses * 3);
}

}  // namespace

ResultChoice ResultScreen::run(SDL_Renderer* ren, const hpi::Vfs& vfs, bool victory,
                               const std::string& title, bool hasNext,
                               Settings* settings, MenuMusic* music,
                               const ResultStats* stats) {
    CursorSet cursors;
    cursors.load(ren, vfs, settings);
    SDL_ShowCursor(cursors.ok() ? SDL_DISABLE : SDL_ENABLE);

    SDL_PumpEvents();
    SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);

    int faction = stats ? stats->faction : 0;
    Geom g = loadGeom(vfs, victory, faction);
    SDL_Texture* bg = gafTexture(ren, vfs, g.bgGaf, g.bgSeq, 0);
    SDL_Texture* okTex[3] = {};
    SDL_Texture* cancelTex[3] = {};
    for (int i = 0; i < 3; ++i) {
        okTex[i] = gafTexture(ren, vfs, g.btnGaf, "OKButton", i);
        cancelTex[i] = gafTexture(ren, vfs, g.btnGaf, "CancelButton", i);
    }
    Font head, body, deco;
    try { head = Font(ren, vfs, "fonts/lombardic (cd).gaf"); } catch (...) {}
    try { deco = Font(ren, vfs, "fonts/decorativesm.gaf"); } catch (...) {}
    try { body = Font(ren, vfs, "fonts/b_times new roman (100).gaf"); } catch (...) {}
    if (!body.ok()) try { body = Font(ren, vfs, "fonts/times new roman (100).gaf"); } catch (...) {}
    if (!head.ok()) head = body;
    auto freeAll = [&] {
        if (bg) gpuvram::destroy(bg);
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
    const char* proceedHint = proceed == ResultChoice::Next   ? "ENTER: NEXT MISSION"
                              : proceed == ResultChoice::Retry ? "ENTER: TRY AGAIN"
                                                               : "ENTER: CONTINUE";

    for (;;) {
        if (termRequested()) { freeAll(); return ResultChoice::Menu; }
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(ren, &w, &h);
        GuiLayout lay(w, h);
        SDL_FRect okR = lay.rect(g.okBtn.x, g.okBtn.y, g.okBtn.w, g.okBtn.h);
        SDL_FRect cancelR = lay.rect(g.menuBtn.x, g.menuBtn.y, g.menuBtn.w, g.menuBtn.h);

        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        bool okHover = inRect(okR, float(mx), float(my));
        bool cancelHover = inRect(cancelR, float(mx), float(my));

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) continue;
            if (e.type == SDL_KEYDOWN) {
                SDL_Keycode k = e.key.keysym.sym;
                if (k == SDLK_ESCAPE) { freeAll(); return ResultChoice::Menu; }
                if (k == SDLK_RETURN || k == SDLK_KP_ENTER) { freeAll(); return proceed; }
                // Replaying a mission you just won isn't on retail's plate; the key is
                // here because the campaign screen is the only other way back in.
                if (k == SDLK_r) { freeAll(); return ResultChoice::Retry; }
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                float bx = float(e.button.x), by = float(e.button.y);
                if (inRect(okR, bx, by)) { freeAll(); return proceed; }
                if (inRect(cancelR, bx, by)) { freeAll(); return ResultChoice::Menu; }
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
                        bool rightAlign = false) {
            if (s.empty()) return;
            float sc = lay.scale * (r.h / 20.0f);
            float tw = f.ok() ? float(f.width(s, sc)) : blockTextWidth(s, lay.scale * 1.6f);
            float x = lay.px(r.x) + (rightAlign ? (r.w * lay.scale - tw) : 0);
            float y = lay.py(r.y + r.h * 0.15f);
            if (f.ok()) f.draw(ren, s, x, y, sc, c);
            else drawBlockText(ren, s, x, y, lay.scale * 1.6f, c);
        };

        // Column headers, then one row per player.
        auto col = [&](Rect base, float y) { Rect r = base; r.y = y; return r; };
        if (stats && !stats->rows.empty()) {
            text(head, "Player", col(g.name, g.headY), gold);
            text(head, "Units", col(g.units, g.headY), gold);
            text(head, "Kills", col(g.kills, g.headY), gold);
            text(head, "Losses", col(g.losses, g.headY), gold);
            text(head, "Time", col(g.time, g.headY), gold);
            text(head, "Score", col(g.score, g.headY), gold);

            int n = std::min<int>(int(stats->rows.size()), g.maxRows);
            for (int i = 0; i < n; ++i) {
                const ResultRow& row = stats->rows[size_t(i)];
                float y = g.name.y + g.pitch * float(i);
                SDL_FRect sw = lay.rect(g.logo.x, g.logo.y + g.pitch * float(i), g.logo.w, g.logo.h);
                SDL_Color c = slotColor(row.colorSlot);
                SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, 255);
                SDL_RenderFillRectF(ren, &sw);
                SDL_SetRenderDrawColor(ren, 20, 18, 14, 255);
                SDL_RenderDrawRectF(ren, &sw);
                // The local player's row is the one you came here to read.
                SDL_Color rc = row.isLocal ? gold : dim;
                text(body, row.name, col(g.name, y), rc);
                text(body, std::to_string(row.built), col(g.units, y), rc, true);
                text(body, std::to_string(row.kills), col(g.kills, y), rc, true);
                text(body, std::to_string(row.losses), col(g.losses, y), rc, true);
                text(body, mmss(row.timeSec), col(g.time, y), rc, true);
                text(body, std::to_string(scoreOf(row)), col(g.score, y), rc, true);
            }
        } else {
            // No table (a replay, or a game that ended before anyone was set up):
            // just name the outcome where the table would have been.
            Rect where = g.name;
            where.y = g.headY;
            text(head, title, where, gold);
        }

        // Title band ("Victory"/"Defeat") in the decorative face the .gui names for it,
        // sized to fill the band. The mission name (campaign only) sits just above it.
        {
            const char* big = victory ? "Victory" : "Defeat";
            // decorativesm's cell is 25x29 but its glyphs hang below the baseline, so
            // scale to a little under the band height or the word laps the help line.
            float sc = deco.ok() ? lay.scale * (g.title.h / 40.0f) : lay.scale * 1.6f;
            const Font& tf = deco.ok() ? deco : head;
            float tw = tf.ok() ? float(tf.width(big, sc)) : blockTextWidth(big, sc * 2);
            if (tw > g.title.w * lay.scale && tw > 0) {
                sc *= (g.title.w * lay.scale) / tw;
                tw = tf.ok() ? float(tf.width(big, sc)) : tw;
            }
            float x = lay.px(g.title.x) + (g.title.w * lay.scale - tw) / 2;
            float top = 0, th = 0;
            if (tf.ok()) tf.vbounds(big, sc, top, th);
            float y = lay.py(g.title.y) + (g.title.h * lay.scale - th) / 2 - top;
            if (tf.ok()) tf.draw(ren, big, x, y, sc, gold);
            else drawBlockText(ren, big, x, lay.py(g.title.y), sc * 2, gold);
            if (!title.empty() && stats) {
                float tsc = lay.scale;
                float ttw = body.ok() ? float(body.width(title, tsc)) : blockTextWidth(title, tsc);
                text(body, title, {(640 - ttw / lay.scale) / 2, g.title.y - 26, ttw / lay.scale, 20}, dim);
            }
        }
        text(body, proceedHint, g.help, dim);

        auto button = [&](SDL_Texture** tex, const SDL_FRect& r, bool hover, const char* fallback) {
            SDL_Texture* t = tex[hover && tex[1] ? 1 : 0];
            if (t) { SDL_RenderCopyF(ren, t, nullptr, &r); return; }
            SDL_SetRenderDrawColor(ren, hover ? 74 : 52, hover ? 66 : 48, hover ? 42 : 32, 235);
            SDL_RenderFillRectF(ren, &r);
            SDL_SetRenderDrawColor(ren, 150, 132, 84, 255);
            SDL_RenderDrawRectF(ren, &r);
            float px = std::max(1.0f, lay.scale * 1.2f);
            drawBlockText(ren, fallback, r.x + (r.w - blockTextWidth(fallback, px)) / 2,
                          r.y + (r.h - 7 * px) / 2, px, gold);
        };
        button(cancelTex, cancelR, cancelHover, "MENU");
        button(okTex, okR, okHover, "GO");

        if (cursors.ok())
            cursors.draw(ren, CursorId::Normal, mx, my, settings ? settings->cursorScale : 1);
        if (const char* sp = devEnv("TAK_SHOT_RESULT")) {
            std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
            if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ABGR8888, px.data(), w * 4) == 0)
                png::write(sp, w, h, px);
            freeAll();
            return ResultChoice::Menu;
        }
        // Headless (dummy video): no input, so end at the result rather than spin.
        if (const char* drv = SDL_GetCurrentVideoDriver(); drv && !std::strcmp(drv, "dummy")) {
            freeAll();
            return ResultChoice::Menu;
        }
        SDL_RenderPresent(ren);
        SDL_Delay(8);
    }
}

}  // namespace tak

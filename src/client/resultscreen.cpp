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
    // Each button's click, bound in its gui states like every retail menu button's.
    std::string menuSound = "cancel.wav", proceedSound = "ok.wav";
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
        if (const gui::Gadget* b = ui.find("MainMenu")) g.menuSound = menuclick::soundOf(*b);
        if (const gui::Gadget* b = ui.find("Proceed")) g.proceedSound = menuclick::soundOf(*b);
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
    SDL_Texture* bg = gafTexture(ren, vfs, g.bgGaf, g.bgSeq, 0);
    SDL_Texture* okTex[3] = {};
    SDL_Texture* cancelTex[3] = {};
    for (int i = 0; i < 3; ++i) {
        okTex[i] = gafTexture(ren, vfs, g.btnGaf, "OKButton", i);
        cancelTex[i] = gafTexture(ren, vfs, g.btnGaf, "CancelButton", i);
    }
    std::vector<SDL_Texture*> logos;
    if (stats) for (const auto& row : stats->rows)
        logos.push_back(gafTexture(ren, vfs, "colorlogos2", factionLogoSequence(row.side), row.colorSlot));
    Font head, body, deco;
    try { head = Font(ren, vfs, "fonts/lombardic (cd).gaf", FontRole::Art); } catch (...) {}
    try { deco = Font(ren, vfs, "fonts/decorativesm.gaf", FontRole::Art); } catch (...) {}
    try { body = Font(ren, vfs, "fonts/b_times new roman (100).gaf", FontRole::Text); } catch (...) {}
    if (!body.ok()) try { body = Font(ren, vfs, "fonts/times new roman (100).gaf", FontRole::Text); } catch (...) {}
    const Font& heading = head.ok() ? head : body;
    auto freeAll = [&] {
        if (bg) gpuvram::destroy(bg);
        for (auto* logo : logos) if (logo) gpuvram::destroy(logo);
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
            if (s.empty()) return;
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
            text(heading, "Player", col(g.name, g.headY), gold);
            text(heading, "Units", col(g.units, g.headY), gold);
            text(heading, "Kills", col(g.kills, g.headY), gold);
            text(heading, "Losses", col(g.losses, g.headY), gold);
            text(heading, "Time", col(g.time, g.headY), gold);
            text(heading, "Score", col(g.score, g.headY), gold);

            int n = std::min<int>(int(stats->rows.size()), g.maxRows);
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

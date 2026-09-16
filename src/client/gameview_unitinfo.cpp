#include "client/gameview.h"

// Retail's Unit Info dialog (`guis/unitinfo.gui`). A small modal plate over the game
// showing one unit's portrait and its three mobility stats -- max velocity,
// acceleration and turn rate -- in the same units and with the same formatting the
// retail engine used. Buildings print "N\A" for all three, as retail does.
//
// Retail ships the command UNBOUND (it isn't in gamedata/keys.tdf, only in
// translate/customkeys.tdf as a rebindable "Function"); we give it F1 by default.
// Like retail's, this dialog does NOT pause -- the game runs behind it.

#include <cstdio>

#include "client/guiart.h"
#include "gui/gui.h"

namespace {

struct Rect { float x = 0, y = 0, w = 0, h = 0; };

// The gadget rects, in the .gui's 640x480 space. Parsed from guis/unitinfo.gui;
// these shipped values stand in if the file can't be read.
struct Geom {
    Rect dialog{88, 133, 486, 210};
    Rect ok{494, 280, 39, 51};
    Rect image{410, 171, 126, 91};
    Rect title{119, 170, 121, 20};
    Rect lblVel{117, 195, 121, 18}, lblAcc{121, 221, 121, 18}, lblTurn{119, 247, 121, 20};
    Rect valVel{248, 195, 100, 20}, valAcc{248, 221, 100, 20}, valTurn{248, 247, 100, 20};
    std::string bgGaf = "genericdialogue", bgSeq = "GenericBG";
};

const Geom& geom(const ta::hpi::Vfs& vfs) {
    static Geom g = [&] {
        Geom r;
        try {
            ta::gui::Gui ui = ta::gui::parse(vfs.read("guis/unitinfo.gui"), "guis/unitinfo.gui");
            auto take = [&](const char* n, Rect& out) {
                if (const ta::gui::Gadget* w = ui.find(n))
                    out = {float(w->x), float(w->y), float(w->w), float(w->h)};
            };
            take("UnitInfo", r.dialog);   // the root gadget shares the title's name
            take("Ok", r.ok);
            take("UnitImage", r.image);
            take("Static0", r.lblVel);
            take("Static1", r.lblAcc);
            take("Static2", r.lblTurn);
            take("MaxVelocity", r.valVel);
            take("Acceleration", r.valAcc);
            take("TurnRate", r.valTurn);
            // Root first, then the "UnitInfo" label: find() returns the root, so read
            // the title rect from the LAST gadget carrying the name instead.
            for (const auto& w : ui.gadgets)
                if (w.name == "UnitInfo" && w.type != 1)
                    r.title = {float(w.x), float(w.y), float(w.w), float(w.h)};
            if (!ui.gadgets.empty() && !ui.gadgets[0].imgs.empty()) {
                r.dialog = {float(ui.gadgets[0].x), float(ui.gadgets[0].y),
                            float(ui.gadgets[0].w), float(ui.gadgets[0].h)};
                std::string base = ui.gadgets[0].imgs[0].gaf;
                if (base.size() >= 4 && base.substr(base.size() - 4) == ".gaf")
                    base = base.substr(0, base.size() - 4);
                r.bgGaf = base;
                r.bgSeq = ui.gadgets[0].imgs[0].seq;
            }
        } catch (...) {}
        return r;
    }();
    return g;
}

}  // namespace

// Pick the unit whose info to show, retail's way: the conjure/build icon under the
// cursor wins, else the first selected unit. Returns nullptr if neither yields one.
const ta::sim::UnitType* GameView::unitInfoSubject() const {
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    for (const auto& [r, bt] : iconRects_)
        if (bt && float(mx) >= r.x && float(mx) <= r.x + r.w &&
            float(my) >= r.y && float(my) <= r.y + r.h)
            return bt;
    for (int id : selection_)
        if (const UnitR* u = frameUnitP(id); u && u->alive() && u->type) return u->type;
    return nullptr;
}

void GameView::toggleUnitInfo() {
    if (unitInfoType_) { unitInfoType_ = nullptr; return; }
    unitInfoType_ = unitInfoSubject();   // stays closed if nothing is selected/hovered
}

void GameView::drawUnitInfo(int winW, int winH) {
    if (!unitInfoType_) return;
    const ta::sim::UnitType* t = unitInfoType_;
    const Geom& g = geom(vfs_);
    ta::GuiLayout lay(winW, winH);

    if (!unitInfoBg_) unitInfoBg_ = ta::gafTexture(ren_, vfs_, g.bgGaf, g.bgSeq, 0);
    SDL_FRect dlg = lay.rect(g.dialog.x, g.dialog.y, g.dialog.w, g.dialog.h);
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    if (unitInfoBg_) {
        SDL_RenderCopyF(ren_, unitInfoBg_, nullptr, &dlg);
    } else {
        SDL_SetRenderDrawColor(ren_, 26, 22, 16, 240);
        SDL_RenderFillRectF(ren_, &dlg);
        SDL_SetRenderDrawColor(ren_, 150, 132, 84, 255);
        SDL_RenderDrawRectF(ren_, &dlg);
    }

    const SDL_Color gold{236, 214, 160, 255};
    auto text = [&](const std::string& s, const Rect& r, bool rightAlign) {
        if (s.empty() || !statFont_.ok()) return;
        float sc = lay.scale * (r.h / 20.0f);
        float top = 0, th = 0;
        statFont_.vbounds(s, sc, top, th);
        float tw = float(statFont_.width(s, sc));
        float x = lay.px(r.x) + (rightAlign ? (r.w * lay.scale - tw) : 0);
        statFont_.draw(ren_, s, x, lay.py(r.y) + (r.h * lay.scale - th) / 2 - top, sc, gold);
    };

    // The portrait comes from anims/buildbuttons.gaf, whose sequences are named by the
    // FBI UnitName (uppercase) -- our type id is the lowercase stem, so ask for both.
    if (!unitInfoIcon_ || unitInfoIconFor_ != t->id) {
        if (unitInfoIcon_) { gpuvram::destroy(unitInfoIcon_); unitInfoIcon_ = nullptr; }
        std::string up = t->id;
        for (char& c : up) c = char(std::toupper((unsigned char)c));
        unitInfoIcon_ = ta::gafTexture(ren_, vfs_, "buildbuttons", up, 0);
        if (!unitInfoIcon_) unitInfoIcon_ = ta::gafTexture(ren_, vfs_, "buildbuttons", t->id, 0);
        unitInfoIconFor_ = t->id;
    }
    if (unitInfoIcon_) {
        SDL_FRect r = lay.rect(g.image.x, g.image.y, g.image.w, g.image.h);
        SDL_RenderCopyF(ren_, unitInfoIcon_, nullptr, &r);
    }

    // Retail prints only the three mobility stats -- no name, no HP, no cost. The unit
    // name isn't on the plate either, but without it the dialog is unreadable when it's
    // opened from a build icon, so it goes where retail put the "Unit Info" title.
    text(t->name.empty() ? t->id : t->name, g.title, false);
    text("Max Velocity", g.lblVel, true);
    text("Acceleration", g.lblAcc, true);
    text("Turn Rate", g.lblTurn, true);

    // The retail formulas, quirks included: a world unit is 0.4 "metres", velocity and
    // acceleration are 16.16 fixed point scaled by the tick rate (acceleration by the
    // SAME single factor, not squared), and turn rate is a COB angle per tick.
    char buf[48];
    if (t->isStructure()) {
        text("N\\A", g.valVel, false);
        text("N\\A", g.valAcc, false);
        text("N\\A", g.valTurn, false);
    } else {
        std::snprintf(buf, sizeof buf, "%.1f m/s", t->maxVel * 0.4f);
        text(buf, g.valVel, false);
        std::snprintf(buf, sizeof buf, "%.2f m/s/s", t->accel * 0.4f / 30.0f);
        text(buf, g.valAcc, false);
        std::snprintf(buf, sizeof buf, "%.0f deg/s", t->turnRate * 57.2957795f);
        text(buf, g.valTurn, false);
    }

    // The OK button: retail art if it's there, a plain plate otherwise.
    SDL_FRect ok = lay.rect(g.ok.x, g.ok.y, g.ok.w, g.ok.h);
    unitInfoOkRect_ = ok;
    if (!unitInfoOk_) unitInfoOk_ = ta::gafTexture(ren_, vfs_, g.bgGaf, "OkButtons", 0);
    if (unitInfoOk_) {
        SDL_RenderCopyF(ren_, unitInfoOk_, nullptr, &ok);
    } else {
        SDL_SetRenderDrawColor(ren_, 52, 48, 32, 235);
        SDL_RenderFillRectF(ren_, &ok);
        SDL_SetRenderDrawColor(ren_, 150, 132, 84, 255);
        SDL_RenderDrawRectF(ren_, &ok);
        blockText("OK", ok.x + (ok.w - blockWidth("OK", 2.0f)) / 2,
                  ok.y + (ok.h - 14) / 2, 2.0f, gold);
    }
}

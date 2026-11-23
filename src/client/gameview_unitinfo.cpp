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
#include "client/hidpi.h"
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

const Geom& geom(const tak::hpi::Vfs& vfs) {
    static Geom g = [&] {
        Geom r;
        try {
            tak::gui::Gui ui = tak::gui::parse(vfs.read("guis/unitinfo.gui"), "guis/unitinfo.gui");
            auto take = [&](const char* n, Rect& out) {
                if (const tak::gui::Gadget* w = ui.find(n))
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
const tak::sim::UnitType* GameView::unitInfoSubject() const {
    int mx = 0, my = 0;
    tak::hidpi::mousePixels(ren_, mx, my);   // iconRects_ are output pixels
    if (selectedBuilder())
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

bool GameView::inputUnitInfo(const SDL_Event& e, int winW, int winH) {
    if (!unitInfoType_) return false;
    if (e.type == SDL_MOUSEMOTION) {
        mouseX_ = float(e.motion.x); mouseY_ = float(e.motion.y);
    }
    bool close = false;
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        const auto& g = geom(vfs_);
        close = tak::GuiLayout(winW, winH).hit(g.ok.x, g.ok.y, g.ok.w, g.ok.h,
                                             float(e.button.x), float(e.button.y));
    } else if (e.type == SDL_KEYDOWN) {
        close = e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER ||
                e.key.keysym.sym == SDLK_ESCAPE ||
                hotkeys_.match(e.key.keysym.sym, e.key.keysym.mod) == tak::Act::UnitInfo;
    }
    if (close) { sounds_.play("ok.wav"); unitInfoType_ = nullptr; }
    // A modal dialog owns input while the simulation continues behind it.
    return true;
}

void GameView::drawUnitInfo(int winW, int winH) {
    if (!unitInfoType_) return;
    const tak::sim::UnitType* t = unitInfoType_;
    const Geom& g = geom(vfs_);
    tak::GuiLayout lay(winW, winH);

    if (!unitInfoBg_) unitInfoBg_ = tak::gafTexture(ren_, vfs_, g.bgGaf, g.bgSeq, 0);
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

    if (!unitInfoLabelFont_.ok()) {
        try { unitInfoLabelFont_ = Font(ren_, vfs_, "fonts/lombardic (cd).gaf", FontRole::Art); }
        catch (...) {}
        unitInfoLabelFont_.setLetterSpacing(0);
    }
    if (!unitInfoValueFont_.ok()) {
        try { unitInfoValueFont_ = Font(ren_, vfs_, "fonts/b_times new roman (100).gaf", FontRole::Text); }
        catch (...) {}
        unitInfoValueFont_.setLetterSpacing(0);
    }
    const SDL_Color gold{255, 255, 255, 255};
    auto text = [&](const std::string& s, const Rect& r, bool rightAlign, bool label = false) {
        const Font& font = label ? unitInfoLabelFont_ : unitInfoValueFont_;
        if (s.empty() || !font.ok()) return;
        float sc = lay.scale;
        float top = 0, th = 0;
        font.vbounds(s, sc, top, th);
        // Keep the native glyph proportions; only shrink text that cannot fit.
        sc *= std::min({1.0f, r.w * lay.scale / std::max(1.0f, float(font.width(s, sc))),
                       r.h * lay.scale / std::max(1.0f, th)});
        font.vbounds(s, sc, top, th);
        float tw = float(font.width(s, sc));
        float x = lay.px(r.x) + (rightAlign ? (r.w * lay.scale - tw) : 0);
        font.draw(ren_, s, x, lay.py(r.y) + (r.h * lay.scale - th) / 2 - top, sc,
                  label ? gold : SDL_Color{48, 30, 12, 255});
    };

    // The portrait comes from anims/buildbuttons.gaf, whose sequences are named by the
    // FBI UnitName (uppercase) -- our type id is the lowercase stem, so ask for both.
    if (!unitInfoIcon_ || unitInfoIconFor_ != t->id) {
        if (unitInfoIcon_) { gpuvram::destroy(unitInfoIcon_); unitInfoIcon_ = nullptr; }
        std::string up = t->id;
        for (char& c : up) c = char(std::toupper((unsigned char)c));
        unitInfoIcon_ = tak::gafTexture(ren_, vfs_, "buildbuttons", up, 0);
        if (!unitInfoIcon_) unitInfoIcon_ = tak::gafTexture(ren_, vfs_, "buildbuttons", t->id, 0);
        unitInfoIconFor_ = t->id;
    }
    if (unitInfoIcon_) {
        SDL_FRect r = lay.rect(g.image.x, g.image.y, g.image.w, g.image.h);
        SDL_RenderCopyF(ren_, unitInfoIcon_, nullptr, &r);
    }

    text("Unit Info", g.title, false, true);
    text("Max Velocity", g.lblVel, true, true);
    text("Acceleration", g.lblAcc, true, true);
    text("Turn Rate", g.lblTurn, true, true);

    // The retail formulas, quirks included: a world unit is 0.4 "metres", velocity and
    // acceleration are 16.16 fixed point scaled by the tick rate (acceleration by the
    // SAME single factor, not squared), and turn rate is a COB angle per tick.
    char buf[48];
    if (t->isStructure()) {
        text("N\\A", g.valVel, false);
        text("N\\A", g.valAcc, false);
        text("N\\A", g.valTurn, false);
    } else {
        std::snprintf(buf, sizeof buf, "%.1f m/s", t->maxVel.toFloat() * 30.0f * 0.4f);
        text(buf, g.valVel, false);
        std::snprintf(buf, sizeof buf, "%.2f m/s/s", t->accel.toFloat() * 900.0f * 0.4f / 30.0f);
        text(buf, g.valAcc, false);
        // turnRate is BAM per TICK: degrees/second = v * (360/65536) * 30.
        std::snprintf(buf, sizeof buf, "%.0f deg/s",
                      float(t->turnRate) * (360.0f / 65536.0f) * 30.0f);
        text(buf, g.valTurn, false);
    }

    // The OK button: retail art if it's there, a plain plate otherwise.
    SDL_FRect ok = lay.rect(g.ok.x, g.ok.y, g.ok.w, g.ok.h);
    unitInfoOkRect_ = ok;
    for (int i = 0; i < 2; ++i)
        if (!unitInfoOk_[i]) unitInfoOk_[i] = tak::gafTexture(ren_, vfs_, g.bgGaf, "OkButtons", i);
    bool hover = lay.hit(g.ok.x, g.ok.y, g.ok.w, g.ok.h, mouseX_, mouseY_);
    SDL_Texture* button = unitInfoOk_[hover && unitInfoOk_[1] ? 1 : 0];
    if (button) {
        SDL_RenderCopyF(ren_, button, nullptr, &ok);
    } else {
        SDL_SetRenderDrawColor(ren_, 52, 48, 32, 235);
        SDL_RenderFillRectF(ren_, &ok);
        SDL_SetRenderDrawColor(ren_, 150, 132, 84, 255);
        SDL_RenderDrawRectF(ren_, &ok);
        blockText("OK", ok.x + (ok.w - blockWidth("OK", 2.0f)) / 2,
                  ok.y + (ok.h - 14) / 2, 2.0f, gold);
    }
}

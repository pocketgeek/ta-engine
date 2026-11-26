// Total Annihilation's in-game interface (docs/retail-gui.md section 3), laid out as
// TotalA.exe lays it out at any resolution: the side panel on the left (PANELSIDE,
// the minimap in its 128x128 corner and the sidebar page under it), the top bar
// (PANELTOP with the energy and metal readouts) and the bottom bar (PANELBOT with
// the selected unit's readouts), the game view taking the rest ((128, 32)-(W-1,
// H-33), 0x4981c9). The sidebar page is <SIDE>MAIN2 with nothing selected, the
// builder's guis/<UNIT><n>.GUI build pages (with download/*.tdf add-ons and IGPATCH
// fillers), and the <SIDE>GEN orders page, switched by the ORDERS/BUILD tabs and
// paged with PREV/NEXT.

#include "client/gameview.h"
#include "client/artscale.h"
#include "client/guiart.h"
#include "client/taguiview.h"
#include "gaf/gaf.h"
#include "gui/tagui.h"
#include "sim/sides.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace {
std::string upperStr(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

    ta::TaGuiView* GameView::taPage(const std::string& name) {
        auto it = taPages_.find(name);
        if (it != taPages_.end()) return it->second.get();
        std::unique_ptr<ta::TaGuiView> view;
        try {
            if (ta::tagui::exists(vfs_, name))
                view = std::make_unique<ta::TaGuiView>(ren_, vfs_, ta::tagui::load(vfs_, name), false);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ta hud: %s: %s\n", name.c_str(), e.what());
        }
        auto* p = view.get();
        taPages_[name] = std::move(view);
        return p;
    }

    // The sidebar page for the current selection (section 3 table).
    std::string GameView::taSidebarPage() {
        const int side = std::max(0, ta::sides::find(side_));
        const std::string prefix = ta::sides::at(side).prefix;
        const UnitR* u = selection_.empty() ? nullptr : frameUnitP(selection_.front());
        if (!u || !u->type) return prefix + "MAIN2";
        const auto& pages = registry_.buildPages(u->type->id);
        if (taBuildTab_ && !pages.empty()) {
            int maxPage = 1;
            for (const auto& s : pages) maxPage = std::max(maxPage, s.page);
            taBuildPage_ = std::clamp(taBuildPage_, 1, maxPage);
            const std::string gui = u->type->id + std::to_string(taBuildPage_);
            return ta::tagui::exists(vfs_, gui) ? gui : prefix + "DL";
        }
        return prefix + "GEN";
    }

    // sidedata.tdf's interface rectangles for the shown side, in 640x480 space (the
    // footer's in `baseheight` space, 0x429efc), read the way TotalA.exe's 0x431950
    // reads them (x1, y1, x2, y2), and the side's console font.
    void GameView::loadTaFrame() {
        const int side = std::max(0, ta::sides::find(side_));
        const auto& s = ta::sides::at(side);
        if (taTopBar_ && taFrameSide_ == side) return;
        taFrameSide_ = side;
        ta::tagui::Screen sc;
        sc.name = upperStr(s.intGaf);
        sc.gadgets.push_back(ta::tagui::Gadget{});
        taTopBar_ = std::make_unique<ta::TaGuiView>(ren_, vfs_, sc, false);   // palette.pal
        taBarRects_.clear();
        taBaseHeight_ = 480;
        try {
            auto b = vread("gamedata/sidedata.tdf");
            auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), "sidedata.tdf");
            if (const auto* n = root.child("SIDE" + std::to_string(side))) {
                for (const char* key : {"LOGO", "METALBAR", "ENERGYBAR", "METALNUM", "ENERGYNUM", "METALMAX",
                                        "ENERGYMAX", "METAL0", "ENERGY0", "METALPRODUCED", "METALCONSUMED",
                                        "ENERGYPRODUCED", "ENERGYCONSUMED", "LOGO2", "UNITNAME", "DAMAGEBAR",
                                        "UNITMETALMAKE", "UNITMETALUSE", "UNITENERGYMAKE", "UNITENERGYUSE",
                                        "UNITNAME2", "DAMAGEBAR2", "NAME", "DESCRIPTION"})
                    if (const auto* r = n->child(key))
                        taBarRects_[key] = SDL_Rect{int(r->numberOr("x1", 0)), int(r->numberOr("y1", 0)),
                                                    int(r->numberOr("x2", 0)), int(r->numberOr("y2", 0))};
                // 0x429efc: `baseheight` (default 480) -- the footer regions sit
                // H - baseheight lower on a taller screen (0x46a86f..0x46a87e).
                taBaseHeight_ = int(n->numberOr("baseheight", 480));
            }
        } catch (...) {}
        taConsole_ = std::make_unique<ta::TaFont>();
        try { taConsole_->loadFnt(ren_, vread("fonts/" + (s.font.empty() ? std::string("console") : s.font) + ".fnt"),
                                  SDL_Color{255, 255, 255, 255}); } catch (...) {}
    }

    // The player's colour emblem: frame <colour> of textures\logos.gaf's 32xlogos
    // (game +0x148db), drawn into a sidedata LOGO rectangle by 0x467c00.
    SDL_Texture* GameView::taLogo(int colorSlot) {
        auto it = taLogos_.find(colorSlot);
        if (it != taLogos_.end()) return it->second;
        SDL_Texture* tex = nullptr;
        try {
            const std::string pal = "palettes/palette.pal", gp = "textures/logos.gaf";
            for (auto& sq : ta::gaf::load(vfs_.read(gp), ta::gaf::Palette::fromBytes(vfs_.read(pal), pal), -1, gp)) {
                if (sq.name != "32xlogos" || sq.frames.empty()) continue;
                const auto& f = sq.frames[size_t(std::clamp(colorSlot, 0, int(sq.frames.size()) - 1))];
                if (f.width && f.height) tex = ta::art::makeTexture(ren_, f.rgba, f.width, f.height);
                break;
            }
        } catch (...) {}
        taLogos_[colorSlot] = tex;
        return tex;
    }

    // TA's frame (docs/retail-gui.md section 3), in window coordinates at UI SCALE.
    // TotalA.exe clears its offscreen surface to colour 0 and draws the side's INT
    // gaf (0x467d70): PANELSIDE at (0, 0), nothing below it (the 480 rows it has);
    // the top bar is PANELTOP at x 129 with PANELBOT copies after it to the screen's
    // width (0x469050..0x46909d); the bottom bar is PANELBOT copies from x 129 at
    // H - 32 (0x46a8b5..0x46a929). Frame offsets cancel (0x4b8051..0x4b805a).
    void GameView::renderTaFrame(int winW, int winH) {
        loadTaFrame();
        const auto& s = ta::sides::at(taFrameSide_);
        const float sc = uiScale_;
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
        const SDL_FRect side{0, 0, float(taSideW()), float(winH)};
        const SDL_FRect top{float(taSideW()), 0, float(winW - taSideW()), float(taTopH())};
        const SDL_FRect bottom{float(taSideW()), float(winH - barH()), float(winW - taSideW()), float(barH())};
        for (const auto* r : {&side, &top, &bottom}) SDL_RenderFillRectF(ren_, r);
        ta::GuiLayout L;
        L.scale = sc;
        taTopBar_->drawSequence(s.intGaf, "PANELSIDE", 0, 0, 0, L);
        const SDL_Point topArt = taTopBar_->sequenceSize(s.intGaf, "PANELTOP");
        const SDL_Point botArt = taTopBar_->sequenceSize(s.intGaf, "PANELBOT");
        {   // the top bar: 32 rows (the view starts at y 32, 0x4981d9)
            const SDL_Rect clip{0, 0, winW, taTopH()};
            SDL_RenderSetClipRect(ren_, &clip);
            float x = 129;
            taTopBar_->drawSequence(s.intGaf, "PANELTOP", 0, x, 0, L);
            x += float(std::max(1, topArt.x));
            while (botArt.x > 0 && x * sc < float(winW)) {
                taTopBar_->drawSequence(s.intGaf, "PANELBOT", 0, x, 0, L);
                x += float(botArt.x);
            }
        }
        {   // the bottom bar
            const SDL_Rect clip{0, winH - barH(), winW, barH()};
            SDL_RenderSetClipRect(ren_, &clip);
            ta::GuiLayout F = L;
            F.oy = float(winH) - 480.0f * sc;
            for (float x = 129; botArt.x > 0 && x * sc < float(winW); x += float(botArt.x))
                taTopBar_->drawSequence(s.intGaf, "PANELBOT", 0, x, 448, F);
        }
        SDL_RenderSetClipRect(ren_, nullptr);
    }

    void GameView::renderTaHud(int winW, int winH) {
        const float scale = uiScale_;
        const std::string pageName = taSidebarPage();
        ta::TaGuiView* page = taPage(pageName);
        taHudPage_ = pageName;
        if (page) {
            // The page is authored at (0, 128) in 640x480 space, under the minimap's
            // 128x128 corner (docs/retail-gui.md section 3), and TA draws it there at
            // every resolution: the side panel keeps its size.
            ta::GuiLayout L;
            L.scale = scale;
            L.ox = 0;
            L.oy = 0;
            taHudLayout_ = L;
            // Build pages: slots the registry does not place on this page are fillers;
            // download add-ons take their slot (gadget BUTTON + 4).
            const UnitR* u = selection_.empty() ? nullptr : frameUnitP(selection_.front());
            if (u && u->type && taBuildTab_) {
                for (const auto& g : page->screen().gadgets) {
                    if (g.type != ta::tagui::Type::Button) continue;
                    const std::string n = upperStr(g.name);
                    if (const auto* t = registry_.find(g.name)) {
                        const bool allowed = worldTables().buildAllowed(t);
                        page->setGrayed(g.name, !allowed);
                    }
                    // The stockpile button's caption (0x4199b0, gadget flag 8):
                    // the slot-0 stock ("%d", when there is any) then the stock
                    // still queued (" +%d", 0x439d80).
                    if (n.size() >= 8 && (n.compare(n.size() - 8, 8, "MAKENUKE") == 0 ||
                                          n.compare(n.size() - 8, 8, "MAKEANTI") == 0)) {
                        std::string caption;
                        if (u->stock[0]) caption = std::to_string(int(u->stock[0]));
                        if (u->stockQueued) caption += " +" + std::to_string(u->stockQueued);
                        page->setText(g.name, caption);
                    }
                }
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "BUILD", true);
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "ORDERS", false);
            } else if (u && u->type) {
                // The <SIDE>GEN page shows only the orders the unit has (BLAST is the
                // Commander's D-gun, sharing LOAD's slot).
                const auto& t = *u->type;
                const std::string pre = ta::sides::at(std::max(0, ta::sides::find(side_))).prefix;
                const bool armed = !t.weapons.empty();
                const std::pair<const char*, bool> shown[] = {
                    {"MOVE", t.canMove}, {"PATROL", t.canPatrol || t.canMove}, {"ATTACK", armed},
                    {"DEFEND", t.canGuard || t.canMove}, {"RECLAIM", t.canReclaim}, {"REPAIR", t.isBuilder},
                    {"LOAD", t.canTransport}, {"UNLOAD", t.canTransport}, {"BLAST", t.canDgun},
                    {"CLOAK", t.canCloak}, {"ONOFF", t.onOffable}, {"FIREORD", armed && t.canSetStance},
                    {"MOVEORD", t.canMove && t.canSetStance}, {"CAPTURE", false}};
                for (const auto& [name, on] : shown) page->setVisible(pre + name, on);
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "ORDERS", true);
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "BUILD", false);
            }
            page->draw(L);
        }
        renderTaTopBar(winW);
        renderTaBottomBar(winW, winH);
    }

    // The top bar's readouts (TotalA.exe 0x4690ab..0x46960b), at sidedata's
    // coordinates in 640x480 space, which TA keeps at every width:
    //   - the LOGO: the shown player's emblem (0x4690ab -> 0x467c00);
    //   - each bar filled x1 .. x1 + (x2 - x1) * stored / storage over y1..y2, in the
    //     side's energycolor / metalcolor (palette.pal; 0x4690f3..0x46912e);
    //   - <RES>NUM the amount stored, "%d" at (x1, y1); <RES>0 a "0" at the bar's
    //     left end; <RES>MAX the storage, "%d" right-aligned to x1 (0x469247);
    //   - ENERGYPRODUCED / CONSUMED "%d" ("%dK" past 99999, 0x4692a5 / 0x469336),
    //     METALPRODUCED / CONSUMED "%.1f" (0x469578 / 0x4695cf).
    void GameView::renderTaTopBar(int winW) {
        (void)winW;
        loadTaFrame();
        const auto& s = ta::sides::at(taFrameSide_);
        ta::GuiLayout L;
        L.scale = uiScale_;
        const int shown = std::clamp(hudPlayer(), 0, std::max(0, frameNumPlayers() - 1));
        const PlayerR& p = framePlayer(shown);
        if (auto it = taBarRects_.find("LOGO"); it != taBarRects_.end())
            if (SDL_Texture* logo = taLogo(playerColorSlot(shown))) {
                const SDL_Rect r = it->second;
                const SDL_FRect d = L.rect(float(r.x), float(r.y), float(r.w - r.x + 1), float(r.h - r.y + 1));
                SDL_RenderCopyF(ren_, logo, nullptr, &d);
            }
        auto bar = [&](const char* key, float value, float cap, SDL_Color c) {
            auto it = taBarRects_.find(key);
            if (it == taBarRects_.end() || cap <= 0) return;
            const SDL_Rect r = it->second;
            const float frac = std::clamp(value / cap, 0.0f, 1.0f);
            const SDL_FRect f = L.rect(float(r.x), float(r.y), float(r.w - r.x) * frac, float(std::max(1, r.h - r.y + 1)));
            SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
            SDL_RenderFillRectF(ren_, &f);
        };
        auto text = [&](const char* key, const std::string& t, SDL_Color c, bool right) {
            auto it = taBarRects_.find(key);
            if (it == taBarRects_.end() || !taConsole_) return;
            float x = float(it->second.x);
            if (right) x -= float(taConsole_->width(t));
            taConsole_->draw(ren_, L, x, float(it->second.y), t, c);
        };
        auto whole = [](float v) { return std::to_string(int(v)); };
        auto big = [](float v) {   // 0x46927a / 0x469308: past +-99999 the count goes in thousands
            const int n = int(std::fabs(v));
            return std::fabs(v) > 99999.0f ? std::to_string(n / 1000) + "K" : std::to_string(n);
        };
        auto tenth = [](float v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", double(std::fabs(v))); return std::string(b); };
        const SDL_Color white{255, 255, 255, 255};
        // The produced / consumed colours are entries of TA's interface colour table
        // (game +0xdcb, +0xa / +0xc); which palette slots those are is not traced.
        const SDL_Color made{0, 255, 0, 255}, spent{255, 64, 64, 255};
        bar("ENERGYBAR", p.energy, p.energyCap, taTopBar_->paletteColor(s.energyColor ? s.energyColor : 208));
        bar("METALBAR", p.metal, p.metalCap, taTopBar_->paletteColor(s.metalColor ? s.metalColor : 224));
        text("ENERGYNUM", whole(p.energy), white, false);
        text("ENERGY0", "0", white, false);
        text("ENERGYMAX", whole(p.energyCap), white, true);
        text("ENERGYPRODUCED", big(p.energyIncome), made, false);
        text("ENERGYCONSUMED", big(p.energyExpense), spent, false);
        text("METALNUM", whole(p.metal), white, false);
        text("METAL0", "0", white, false);
        text("METALMAX", whole(p.metalCap), white, true);
        text("METALPRODUCED", tenth(p.metalIncome), made, false);
        text("METALCONSUMED", tenth(p.metalExpense), spent, false);
    }

    // The bottom bar's readouts (TotalA.exe 0x46a860..0x46b789), sidedata's footer
    // rectangles moved down by H - baseheight (0x46a86f..0x46a87e):
    //   - the selected unit: its owner's emblem in LOGO2 (0x46b105), its name centred
    //     on UNITNAME.x1 (0x46afcf..0x46b00e, palette colour 0x53), its health in
    //     DAMAGEBAR (0x46b04c..0x46b0f0: x1 .. x1 + (x2 - x1) * hp / maxdamage, the
    //     rest of the bar in the empty colour) and its kills under the bar ("%d
    //     kill(s)", " - Veteran" past 4, 0x46b2b8..0x46b3ae); for one of the shown
    //     player's units its make / use, UNITMETALMAKE "+%.1f", UNITENERGYMAKE
    //     "+%.0f", UNITMETALUSE "-%.1f", UNITENERGYUSE "-%.0f" (0x46b16d..0x46b2b3);
    //   - the unit under the pointer in UNITNAME2 / DAMAGEBAR2 (0x46b492..0x46b55f).
    void GameView::renderTaBottomBar(int winW, int winH) {
        (void)winW;
        if (taBarRects_.empty() || !taConsole_) return;
        ta::GuiLayout F;
        F.scale = uiScale_;
        F.oy = float(winH) - float(taBaseHeight_) * uiScale_;
        auto rectOf = [&](const char* key) -> const SDL_Rect* {
            auto it = taBarRects_.find(key);
            return it == taBarRects_.end() ? nullptr : &it->second;
        };
        const SDL_Color nameC = taTopBar_->paletteColor(0x53);
        const SDL_Color white{255, 255, 255, 255}, made{0, 255, 0, 255}, spent{255, 64, 64, 255};
        auto centred = [&](const char* key, const std::string& t, SDL_Color c) {
            if (const SDL_Rect* r = rectOf(key))
                taConsole_->draw(ren_, F, float(r->x) - float(taConsole_->width(t) / 2), float(r->y), t, c);
        };
        auto at = [&](const char* key, const std::string& t, SDL_Color c) {
            if (const SDL_Rect* r = rectOf(key)) taConsole_->draw(ren_, F, float(r->x), float(r->y), t, c);
        };
        auto health = [&](const char* key, const UnitR& u) {
            const SDL_Rect* r = rectOf(key);
            if (!r || !u.type || u.type->maxHp <= 0) return;
            const float frac = std::clamp(u.hp / float(u.type->maxHp), 0.0f, 1.0f);
            const float w = float(r->w - r->x), h = float(r->h - r->y + 1);
            const float filled = std::floor(w * frac);
            const SDL_FRect full = F.rect(float(r->x), float(r->y), filled, h);
            const SDL_FRect rest = F.rect(float(r->x) + filled, float(r->y), w - filled, h);
            SDL_SetRenderDrawColor(ren_, 0, 220, 0, 255);
            SDL_RenderFillRectF(ren_, &full);
            SDL_SetRenderDrawColor(ren_, 200, 0, 0, 255);
            SDL_RenderFillRectF(ren_, &rest);
        };
        const UnitR* u = selection_.empty() ? nullptr : frameUnitP(selection_.front());
        if (u && u->alive() && u->type) {
            if (const SDL_Rect* r = rectOf("LOGO2"))
                if (SDL_Texture* logo = taLogo(playerColorSlot(u->player))) {
                    const SDL_FRect d = F.rect(float(r->x), float(r->y), float(r->w - r->x + 1), float(r->h - r->y + 1));
                    SDL_RenderCopyF(ren_, logo, nullptr, &d);
                }
            centred("UNITNAME", u->displayName(), nameC);
            health("DAMAGEBAR", *u);
            if (u->kills > 0)
                if (const SDL_Rect* r = rectOf("DAMAGEBAR")) {
                    std::string k = std::to_string(u->kills) + (u->kills == 1 ? " kill" : " kills");
                    if (u->kills > 4) k += " - Veteran";
                    taConsole_->draw(ren_, F, float(r->x), float(r->h + 2), k, white);
                }
            if (u->player == hudPlayer()) {
                char b[32];
                std::snprintf(b, sizeof b, "+%.1f", double(std::max(0.0f, u->make[1]))); at("UNITMETALMAKE", b, made);
                std::snprintf(b, sizeof b, "+%.0f", double(std::max(0.0f, u->make[0]))); at("UNITENERGYMAKE", b, made);
                std::snprintf(b, sizeof b, "-%.1f", double(std::max(0.0f, u->use[1]))); at("UNITMETALUSE", b, spent);
                std::snprintf(b, sizeof b, "-%.0f", double(std::max(0.0f, u->use[0]))); at("UNITENERGYUSE", b, spent);
            }
        }
        // The unit under the pointer, when it is in the world view.
        if (inView(screenMouseX_, screenMouseY_))
            if (const UnitR* h = hoveredUnit(ta::client::PointerArea::World); h && h->alive() && h->type) {
                centred("UNITNAME2", h->displayName(), nameC);
                health("DAMAGEBAR2", *h);
            }
    }

    // Clicks on the sidebar page. True when the click belonged to it.
    bool GameView::taHudEvent(const SDL_Event& e) {
        if (taHudPage_.empty()) return false;
        ta::TaGuiView* page = taPage(taHudPage_);
        if (!page) return false;
        const bool mouse = e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP;
        if (mouse && !page->at(float(e.button.x), float(e.button.y), taHudLayout_)) return false;
        if (watchOnly()) return mouse;
        // A build page picture (TotalA.exe's build-menu handler 0x41aa00). The
        // gadget's name is the unit (0x41aa39 -> 0x488b10). A unit that is placed
        // on the map -- its type's bmcode byte (+0x22f, stored at 0x42c422) is 0 --
        // arms the placement (0x41ab7f..0x41ab9c; either button). Anything else, a
        // mobile unit or the stockpile button (MAKENUKE / MAKEANTI, no unit), is
        // queued in the unit whose menu this is (0x41abd9..0x41ac3c -> 0x419b00:
        // MOBILEBUILD / BUILDINGBUILD, or BUILDWEAPON): a left click +1, a right
        // click -1, Shift five.
        if (e.type == SDL_MOUSEBUTTONDOWN && taBuildTab_ && !selection_.empty() &&
            (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)) {
            if (const auto* g = page->at(float(e.button.x), float(e.button.y), taHudLayout_);
                g && g->type == ta::tagui::Type::Button && !g->grayedOut) {
                const std::string name = upperStr(g->name);
                const size_t at = name.size() >= 8 ? name.size() - 8 : 0;
                const std::string tail = name.substr(at);
                const bool stock = tail == "MAKENUKE" || tail == "MAKEANTI";
                const ta::sim::UnitType* t = stock ? nullptr : registry_.find(g->name);
                if (t && t->buildMovementCode == 0) {
                    playClickTone();
                    placing_ = t;
                    shiftBuildPlaced_ = false;
                    return true;
                }
                if (stock || t) {
                    playClickTone();
                    ta::net::Command c;
                    c.kind = e.button.button == SDL_BUTTON_LEFT ? ta::net::Cmd::Train : ta::net::Cmd::Unqueue;
                    c.unitId = selection_.front();
                    c.targetId = (SDL_GetModState() & KMOD_SHIFT) ? 5 : 1;
                    std::snprintf(c.type, sizeof c.type, "%s", stock ? tail.c_str() : t->id.c_str());
                    issue(c);
                    return true;
                }
            }
        }
        if (mouse && e.button.button != SDL_BUTTON_LEFT) return true;   // any other press on the page
        const std::string hit = page->handle(e, taHudLayout_);
        if (hit.empty()) return mouse;
        playClickTone();
        const int side = std::max(0, ta::sides::find(side_));
        const std::string prefix = upperStr(ta::sides::at(side).prefix);
        std::string n = upperStr(hit);
        if (n.rfind(prefix, 0) == 0) n = n.substr(prefix.size());
        if (n == "BUILD") { taBuildTab_ = true; return true; }
        if (n == "ORDERS") { taBuildTab_ = false; return true; }
        if (n == "NEXT") { ++taBuildPage_; return true; }
        if (n == "PREV") { taBuildPage_ = std::max(1, taBuildPage_ - 1); return true; }
        if (registry_.find(hit) && taBuildTab_) return true;   // a build picture: taken on the press
        auto stopAll = [&] {
            for (int id : selection_) {
                ta::net::Command c;
                c.kind = ta::net::Cmd::Stop;
                c.unitId = id;
                issue(c);
            }
            pendingCmd_ = 0;
        };
        if (n == "MOVE") pendingCmd_ = 'm';
        else if (n == "ATTACK") pendingCmd_ = 'a';
        else if (n == "PATROL") pendingCmd_ = 'p';
        else if (n == "DEFEND") pendingCmd_ = 'g';
        else if (n == "REPAIR") pendingCmd_ = 'r';
        else if (n == "LOAD") pendingCmd_ = 'l';
        else if (n == "BLAST") pendingCmd_ = 'd';   // the Commander's D-gun
        else if (n == "UNLOAD") pendingCmd_ = 'u';
        else if (n == "RECLAIM") pendingCmd_ = 'e';
        else if (n == "STOP") stopAll();
        else if (n == "FIREORD") issuePerUnit(ta::net::Cmd::Stance, std::clamp(page->stage(hit), 0, 2));
        else if (n == "ONOFF") issuePerUnit(ta::net::Cmd::SetActive, page->stage(hit) ? 1 : 0);
        else if (n == "CLOAK") issuePerUnit(ta::net::Cmd::Cloak, page->stage(hit) ? 1 : 0);
        return true;
    }



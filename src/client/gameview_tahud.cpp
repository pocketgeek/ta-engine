// Total Annihilation's in-game interface (docs/retail-gui.md section 3), drawn from
// the retail GUIs:
//   - the sidebar page: <SIDE>MAIN2 with nothing selected, the builder's
//     guis/<UNIT><n>.GUI build pages (with download/*.tdf add-ons and IGPATCH
//     fillers), and the <SIDE>GEN orders page, switched by the ORDERS/BUILD tabs
//     and paged with PREV/NEXT;
//   - the top bar: the side's INT gaf PANELTOP with the metal and energy bars and
//     numbers at sidedata.tdf's coordinates.
// Layout difference from retail: the sidebar sits in the engine's right-hand
// command strip under the minimap (retail: left edge, minimap above it), so the
// world view keeps its screen origin.

#include "client/gameview.h"
#include "client/taguiview.h"
#include "gui/tagui.h"
#include "sim/sides.h"
#include "tdf/tdf.h"

#include <algorithm>
#include <cctype>

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

    void GameView::renderTaHud(int winW, int winH) {
        const SDL_FRect mini = minimapRect(winW, winH);
        const float panelX = float(mapViewW(winW));
        const float scale = std::max(0.5f, (float(cmdPanelW()) - 8.0f) / 128.0f);
        const std::string pageName = taSidebarPage();
        ta::TaGuiView* page = taPage(pageName);
        taHudPage_ = pageName;
        if (page) {
            // The page's container is authored at (0,128) in 640x480 space; map that
            // corner just under the minimap in the command strip.
            ta::GuiLayout L;
            L.scale = scale;
            L.ox = panelX + 4.0f;
            L.oy = (mini.y + mini.h + 6.0f) - 128.0f * scale;
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
                    (void)n;
                }
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "BUILD", true);
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "ORDERS", false);
            } else if (u && u->type) {
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "ORDERS", true);
                page->setPressed(ta::sides::at(std::max(0, ta::sides::find(side_))).prefix + "BUILD", false);
            }
            page->draw(L);
        }
        renderTaTopBar(winW);
    }

    void GameView::renderTaTopBar(int winW) {
        const int side = std::max(0, ta::sides::find(side_));
        const auto& s = ta::sides::at(side);
        if (!taTopBar_) {
            ta::tagui::Screen sc;
            sc.name = upperStr(s.intGaf);
            sc.gadgets.push_back(ta::tagui::Gadget{});
            taTopBar_ = std::make_unique<ta::TaGuiView>(ren_, vfs_, sc, false);
            try {   // sidedata rectangles, 640x480 space with PANELTOP at x=129
                auto b = vread("gamedata/sidedata.tdf");
                auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), "sidedata.tdf");
                if (const auto* n = root.child("SIDE" + std::to_string(side)))
                    for (const char* key : {"METALBAR", "ENERGYBAR", "METALNUM", "ENERGYNUM", "METALMAX",
                                            "ENERGYMAX", "METALPRODUCED", "METALCONSUMED",
                                            "ENERGYPRODUCED", "ENERGYCONSUMED"})
                        if (const auto* r = n->child(key))
                            taBarRects_[key] = SDL_Rect{int(r->numberOr("x1", 0)), int(r->numberOr("y1", 0)),
                                                        int(r->numberOr("x2", 0)), int(r->numberOr("y2", 0))};
            } catch (...) {}
            taConsole_ = std::make_unique<ta::TaFont>();
            try { taConsole_->loadFnt(ren_, vread("fonts/" + (s.font.empty() ? std::string("console") : s.font) + ".fnt"),
                                      SDL_Color{255, 255, 255, 255}); } catch (...) {}
        }
        (void)winW;
        const float scale = uiScale_;
        ta::GuiLayout L;
        L.scale = scale;
        L.ox = -129.0f * scale;   // PANELTOP's authored x is 129; our bar starts at the view's left edge
        L.oy = 0;
        taTopBar_->drawSequence(s.intGaf, "PANELTOP", 0, 129, 0, L);
        const PlayerR& p = framePlayer(std::clamp(hudPlayer(), 0, std::max(0, frameNumPlayers() - 1)));
        auto bar = [&](const char* key, float value, float cap, SDL_Color c) {
            auto it = taBarRects_.find(key);
            if (it == taBarRects_.end()) return;
            const SDL_Rect r = it->second;
            const float frac = cap > 0 ? std::clamp(value / cap, 0.0f, 1.0f) : 0.0f;
            SDL_FRect f = L.rect(float(r.x), float(r.y), float(r.w - r.x) * frac, float(std::max(1, r.h - r.y + 1)));
            SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
            SDL_RenderFillRectF(ren_, &f);
        };
        auto num = [&](const char* key, float v, SDL_Color c, bool right) {
            auto it = taBarRects_.find(key);
            if (it == taBarRects_.end() || !taConsole_) return;
            const std::string t = std::to_string(int(v + (v >= 0 ? 0.5f : -0.5f)));
            float x = float(it->second.x);
            if (right) x -= float(taConsole_->width(t));
            taConsole_->draw(ren_, L, x, float(it->second.y), t, c);
        };
        const SDL_Color metalC{200, 200, 200, 255}, energyC{255, 255, 0, 255};
        bar("METALBAR", p.metal, p.metalCap, metalC);
        bar("ENERGYBAR", p.energy, p.energyCap, energyC);
        num("METALNUM", p.metal, metalC, false);
        num("ENERGYNUM", p.energy, energyC, false);
        num("METALMAX", p.metalCap, metalC, true);
        num("ENERGYMAX", p.energyCap, energyC, true);
        num("METALPRODUCED", p.metalIncome, SDL_Color{0, 255, 0, 255}, false);
        num("METALCONSUMED", p.metalExpense, SDL_Color{255, 64, 64, 255}, false);
        num("ENERGYPRODUCED", p.energyIncome, SDL_Color{0, 255, 0, 255}, false);
        num("ENERGYCONSUMED", p.energyExpense, SDL_Color{255, 64, 64, 255}, false);
    }

    // Clicks on the sidebar page. True when the click belonged to it.
    bool GameView::taHudEvent(const SDL_Event& e) {
        if (taHudPage_.empty()) return false;
        ta::TaGuiView* page = taPage(taHudPage_);
        if (!page) return false;
        const bool mouse = e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP;
        if (mouse && !page->at(float(e.button.x), float(e.button.y), taHudLayout_)) return false;
        if (watchOnly()) return mouse;
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
        if (const auto* t = registry_.find(hit); t && taBuildTab_) {   // a build picture
            placing_ = t;
            shiftBuildPlaced_ = false;
            return true;
        }
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
        else if (n == "UNLOAD") pendingCmd_ = 'u';
        else if (n == "RECLAIM") pendingCmd_ = 'e';
        else if (n == "STOP") stopAll();
        else if (n == "FIREORD") issuePerUnit(ta::net::Cmd::Stance, std::clamp(page->stage(hit), 0, 2));
        else if (n == "ONOFF") issuePerUnit(ta::net::Cmd::SetActive, page->stage(hit) ? 1 : 0);
        else if (n == "CLOAK") issuePerUnit(ta::net::Cmd::Cloak, page->stage(hit) ? 1 : 0);
        return true;
    }



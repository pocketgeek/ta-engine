// Total Annihilation's in-game interface, laid out as the TAK engine lays out its
// HUD rather than in retail's 640x480 frame (docs/retail-gui.md section 3 has that
// frame): the world view runs from the window's left edge to a column on the right,
// the whole height of the window. The column holds, top down, the minimap, the
// selected unit's readout with the STATS PANEL under it, and the <SIDE>GEN order
// buttons (retail's art and rules, client/orderpage.h). The selected builder's menu
// is a row of pictures along the bottom of the view (GameView::drawPanel,
// client/buildrow.h) and the metal / energy bar (PANELTOP with sidedata's readouts)
// floats over the top of the view. Each piece has its own scale in Options, and the
// build row and the resource bar their own left / center / right placement.

#include "client/gameview.h"
#include "client/artscale.h"
#include "client/guiart.h"
#include "client/orderpage.h"
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

    // The orders page for the current selection: TotalA.exe's 0x41b2e0 counts the
    // shown player's selected units, and any selection shows <SIDE>GEN
    // (0x41b853..0x41b883 -> 0x41b0f0). The build pages are not used: the build row
    // (drawPanel) shows the builder's menu. Nothing selected shows nothing.
    std::string GameView::taSidebarPage() {
        if (taOrderUnits().empty()) return {};
        const int side = std::max(0, ta::sides::find(side_));
        return ta::sides::at(side).prefix + "GEN";
    }

    std::vector<const UnitR*> GameView::taOrderUnits() const {
        std::vector<const UnitR*> out;
        const int owner = hudPlayer();
        for (int id : selection_)
            if (const UnitR* u = frameUnitP(id); u && u->type && u->alive() && u->player == owner) out.push_back(u);
        return out;
    }

    // The <SIDE>GEN page for the selection (client/orderpage.h: TotalA.exe 0x41b2e0
    // folds the selection, 0x41a120 greys, hides and stages the gadgets). The
    // ORDERS / BUILD tabs are hidden: the build row is always on show.
    void GameView::applyTaOrderPage(ta::TaGuiView& page, const std::string& prefix) {
        const auto units = taOrderUnits();
        std::vector<ta::client::OrderPageUnit> sel;
        for (const UnitR* u : units)
            sel.push_back({u->type->orderFlags, u->moveState, u->fireState, u->active, u->cloakOn});
        for (const auto& b : ta::client::orderPage(ta::client::orderSummary(sel), false)) {
            const std::string name = prefix + std::string(b.name);
            page.setVisible(name, b.shown);
            page.setGrayed(name, b.grayed);
            page.setStage(name, std::max(0, b.stage));
        }
        page.setVisible(prefix + "ORDERS", false);
        page.setVisible(prefix + "BUILD", false);
    }

    // sidedata.tdf's interface rectangles for the shown side, in 640x480 space,
    // read the way TotalA.exe's 0x431950 reads them (x1, y1, x2, y2), and the side's
    // console font.
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
        try {
            auto b = vread("gamedata/sidedata.tdf");
            auto root = ta::tdf::parseText(std::string(b.begin(), b.end()), "sidedata.tdf");
            if (const auto* n = root.child("SIDE" + std::to_string(side))) {
                for (const char* key : {"LOGO", "METALBAR", "ENERGYBAR", "METALNUM", "ENERGYNUM", "METALMAX",
                                        "ENERGYMAX", "METAL0", "ENERGY0", "METALPRODUCED", "METALCONSUMED",
                                        "ENERGYPRODUCED", "ENERGYCONSUMED"})
                    if (const auto* r = n->child(key))
                        taBarRects_[key] = SDL_Rect{int(r->numberOr("x1", 0)), int(r->numberOr("y1", 0)),
                                                    int(r->numberOr("x2", 0)), int(r->numberOr("y2", 0))};
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

    // The stockpile button on a type's first build page: guis\\<UNIT>1.GUI's
    // MAKENUKE / MAKEANTI gadget (0x4199b0, gadget flag 8), "" when it has none.
    // Stockpilers (the silos, the anti-nukes) are not builders, so the registry has
    // no build pages for them; the button is the whole of their menu.
    const std::string& GameView::stockButtonOf(const std::string& typeId) const {
        auto it = stockButtons_.find(typeId);
        if (it != stockButtons_.end()) return it->second;
        std::string found;
        try {
            const std::string gui = typeId + "1";
            if (ta::tagui::exists(vfs_, gui))
                for (const auto& g : ta::tagui::load(vfs_, gui).gadgets) {
                    const std::string n = upperStr(g.name);
                    if (g.type == ta::tagui::Type::Button && n.size() >= 8 &&
                        (n.compare(n.size() - 8, 8, "MAKENUKE") == 0 || n.compare(n.size() - 8, 8, "MAKEANTI") == 0)) {
                        found = n.substr(n.size() - 8);
                        break;
                    }
                }
        } catch (...) {}
        return stockButtons_[typeId] = found;
    }

    // The right-hand column's backing, behind the minimap and everything under it.
    void GameView::renderTaFrame(int winW, int winH) {
        loadTaFrame();
        const float x = float(winW - hudColW());
        const SDL_FRect col{x, 0, float(hudColW()), float(winH)};
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren_, 16, 17, 20, 255);
        SDL_RenderFillRectF(ren_, &col);
        SDL_SetRenderDrawColor(ren_, 70, 74, 82, 255);
        SDL_RenderDrawLineF(ren_, x, 0, x, float(winH));
        {   // the minimap's well
            const float pad = hudColPad(), box = miniBoxSize();
            const SDL_FRect well{x + (float(hudColW()) - box) / 2 - 1, pad - 1, box + 2, box + 2};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
            SDL_RenderFillRectF(ren_, &well);
            SDL_SetRenderDrawColor(ren_, 70, 74, 82, 255);
            SDL_RenderDrawRectF(ren_, &well);
        }
    }

    // Everything of the HUD but the minimap, in window coordinates: down the column
    // the selected unit's readout, the STATS PANEL and the order buttons; then the
    // metal / energy bar and the build row over the view.
    void GameView::renderTaHud(int winW, int winH) {
        loadTaFrame();
        taHudPage_.clear();
        taOrdersRect_ = {};
        statsBottom_ = 0;
        const float pad = hudColPad();
        const float colX = float(winW - hudColW());
        float y = pad + miniBoxSize() + pad;
        if (hudShown()) y = renderTaUnitInfo(winW, y);
        {   // the STATS PANEL drops rows rather than push the order buttons off the window
            const float floor = float(winH) - pad - (hudShown() ? taOrdersHeight() + pad : 0.0f);
            const float bottom = drawStatsPanel(colX + pad, float(winW) - pad, y + pad / 2, floor);
            if (bottom > y) y = bottom + pad;
        }
        if (hudShown()) {
            renderTaOrders(winW, y);
            renderTaResourceBar(winW, winH);
            drawPanel(winW, winH);
        }
    }

    // The <SIDE>GEN order buttons at `top` in the column, at UI SCALE x ORDERS SCALE,
    // the page's tab strip (ORDERS / BUILD, rows 0..30) cut off.
    // The orders page's size in its own units, less the tab strip: its width, its
    // height and the rows cut off its top (4 above the highest button not a tab).
    GameView::TaOrdersCrop GameView::taOrdersCrop(ta::TaGuiView& page) const {
        const std::string prefix = ta::sides::at(std::max(0, ta::sides::find(side_))).prefix;
        const auto& gads = page.screen().gadgets;
        TaOrdersCrop c;
        c.w = gads.empty() ? 128.0f : float(std::max(1, gads.front().w));
        c.h = gads.empty() ? 352.0f : float(std::max(1, gads.front().h));
        c.top = c.h;
        for (const auto& g : gads) {
            if (g.index == 0 || g.type != ta::tagui::Type::Button) continue;
            const std::string n = upperStr(g.name);
            if (n == prefix + "ORDERS" || n == prefix + "BUILD") continue;
            c.top = std::min(c.top, float(g.y) - 4.0f);
        }
        c.top = std::clamp(c.top, 0.0f, c.h - 1);
        return c;
    }

    float GameView::taOrdersHeight() {
        const std::string pageName = taSidebarPage();
        ta::TaGuiView* page = pageName.empty() ? nullptr : taPage(pageName);
        if (!page) return 0;
        const TaOrdersCrop c = taOrdersCrop(*page);
        return (c.h - c.top) * ordersScaleEff();
    }

    float GameView::renderTaOrders(int winW, float top) {
        const std::string pageName = taSidebarPage();
        if (pageName.empty()) return top;
        ta::TaGuiView* page = taPage(pageName);
        if (!page) return top;
        taHudPage_ = pageName;
        const std::string prefix = ta::sides::at(std::max(0, ta::sides::find(side_))).prefix;
        applyTaOrderPage(*page, prefix);
        const TaOrdersCrop crop = taOrdersCrop(*page);
        const float pageW = crop.w, pageH = crop.h, cropTop = crop.top;
        const float s = ordersScaleEff();
        const float colX = float(winW - hudColW());
        ta::GuiLayout L;
        L.scale = s;
        L.ox = std::round(colX + (float(hudColW()) - pageW * s) / 2) - float(page->originX()) * s;
        L.oy = std::round(top) - (float(page->originY()) + cropTop) * s;
        taHudLayout_ = L;
        taOrdersRect_ = {L.px(float(page->originX())), std::round(top), pageW * s, (pageH - cropTop) * s};
        const SDL_Rect clip{int(taOrdersRect_.x), int(taOrdersRect_.y), int(std::ceil(taOrdersRect_.w)),
                            int(std::ceil(taOrdersRect_.h))};
        SDL_RenderSetClipRect(ren_, &clip);
        page->draw(L);
        SDL_RenderSetClipRect(ren_, nullptr);
        return taOrdersRect_.y + taOrdersRect_.h;
    }

    // The selected unit's readout at `top` in the column (retail's footer, 0x46a860..
    // 0x46b789, as a block): its owner's emblem and its name (palette colour 0x53),
    // its health (hp / maxdamage, the rest of the bar in the empty colour), its kills
    // ("%d kill(s)", " - Veteran" past 4); for one of the shown player's own units
    // its metal make / use "+%.1f" / "-%.1f" and energy "+%.0f" / "-%.0f"
    // (0x46b16d..0x46b2b3); then the unit under the pointer, its name and health
    // (0x46b492..0x46b55f). At UI SCALE x STATS SCALE.
    float GameView::renderTaUnitInfo(int winW, float top) {
        if (!taConsole_ || !taTopBar_) return top;
        const UnitR* u = selection_.empty() ? nullptr : frameUnitP(selection_.front());
        if (u && (!u->alive() || !u->type)) u = nullptr;
        // The unit under the pointer, in the view or on the minimap (0x48cd80).
        const UnitR* h = nullptr;
        if (const UnitR* hv = pointerUnit(); hv && hv->alive() && hv->type && hv != u) h = hv;
        if (!u && !h) return top;
        const float k = statsScaleEff(), pad = hudColPad();
        ta::GuiLayout F;
        F.scale = k;
        F.ox = float(winW - hudColW()) + pad;
        F.oy = top;
        const float w = (float(hudColW()) - 2 * pad) / k;   // the block's width in its own units
        const float fh = float(std::max(8, taConsole_->height()));
        const SDL_Color nameC = taTopBar_->paletteColor(0x53);
        const SDL_Color white{255, 255, 255, 255}, made{0, 255, 0, 255}, spent{255, 64, 64, 255}, dim{170, 175, 185, 255};
        auto health = [&](float y, const UnitR& v) {
            if (v.type->maxHp <= 0) return;
            const float frac = std::clamp(v.hp / float(v.type->maxHp), 0.0f, 1.0f);
            const float filled = std::floor(w * frac);
            const SDL_FRect full = F.rect(0, y, filled, 3), rest = F.rect(filled, y, w - filled, 3);
            SDL_SetRenderDrawColor(ren_, 0, 220, 0, 255);
            SDL_RenderFillRectF(ren_, &full);
            SDL_SetRenderDrawColor(ren_, 200, 0, 0, 255);
            SDL_RenderFillRectF(ren_, &rest);
        };
        auto heading = [&](float y, const UnitR& v) {   // emblem + name; returns the next row
            const float logo = std::max(fh, 16.0f);
            if (SDL_Texture* t = taLogo(playerColorSlot(v.player))) {
                const SDL_FRect d = F.rect(0, y, logo, logo);
                SDL_RenderCopyF(ren_, t, nullptr, &d);
            }
            taConsole_->draw(ren_, F, logo + 4, y + (logo - fh) / 2, v.displayName(), nameC);
            return y + logo + 3;
        };
        float y = 0;
        if (u) {
            y = heading(y, *u);
            health(y, *u);
            y += 6;
            if (u->kills > 0) {
                std::string kl = std::to_string(u->kills) + (u->kills == 1 ? " kill" : " kills");
                if (u->kills > 4) kl += " - Veteran";
                taConsole_->draw(ren_, F, 0, y, kl, white);
                y += fh + 2;
            }
            if (u->player == hudPlayer()) {
                char b[32];
                auto row = [&](const char* label, const char* mfmt, float mk, const char* ufmt, float us) {
                    taConsole_->draw(ren_, F, 0, y, label, dim);
                    std::snprintf(b, sizeof b, mfmt, double(std::max(0.0f, mk)));
                    taConsole_->draw(ren_, F, w * 0.45f, y, b, made);
                    std::snprintf(b, sizeof b, ufmt, double(std::max(0.0f, us)));
                    taConsole_->draw(ren_, F, w * 0.73f, y, b, spent);
                    y += fh + 2;
                };
                row("Metal", "+%.1f", u->make[1], "-%.1f", u->use[1]);
                row("Energy", "+%.0f", u->make[0], "-%.0f", u->use[0]);
            }
        }
        if (h) {
            if (u) y += 4;
            if (!mainViewVisible(*h)) {
                // 0x46af3d -> 0x46b6f3: a unit the viewed player does not see -- a radar
                // or sonar dot on the minimap -- is only "S: " (a sonar contact) or "R: "
                // and "Unidentified object", its name and health withheld.
                taConsole_->draw(ren_, F, 0, y, std::string(h->sonarSeen ? "S: " : "R: ") + "Unidentified object", nameC);
                y += fh + 2;
            } else {
                y = heading(y, *h);
                health(y, *h);
                y += 6;
            }
        }
        return F.py(y) + pad / 2;
    }

    // The metal / energy bar (TotalA.exe 0x4690ab..0x46960b): the side's PANELTOP art
    // (x 129..640 of retail's top bar) and sidedata's readouts on it, floating over the
    // top of the view at RESOURCE BAR (left / center / right) and UI SCALE x RESOURCE
    // BAR SCALE, shrunk only when the view is too narrow for it:
    //   - the LOGO: the shown player's emblem (0x4690ab -> 0x467c00);
    //   - each bar filled x1 .. x1 + (x2 - x1) * stored / storage over y1..y2, in the
    //     side's energycolor / metalcolor (palette.pal; 0x4690f3..0x46912e);
    //   - <RES>NUM the amount stored, "%d" at (x1, y1); <RES>0 a "0" at the bar's
    //     left end; <RES>MAX the storage, "%d" right-aligned to x1 (0x469247);
    //   - ENERGYPRODUCED / CONSUMED "%d" ("%dK" past 99999, 0x4692a5 / 0x469336),
    //     METALPRODUCED / CONSUMED "%.1f" (0x469578 / 0x4695cf).
    void GameView::renderTaResourceBar(int winW, int winH) {
        (void)winH;
        const auto& s = ta::sides::at(taFrameSide_);
        constexpr float kArtX = 129, kArtW = 640 - kArtX, kArtH = 32;
        const float areaW = float(mapViewW(winW));
        const float sc = std::max(0.1f, std::min(uiScale_ * resourceBarScale_, areaW / kArtW));
        const float barW = kArtW * sc, barH = kArtH * sc;
        const float x = resourceBarAlign_ == 0 ? 0.0f
                      : resourceBarAlign_ == 2 ? areaW - barW
                                               : std::round((areaW - barW) / 2);
        const float y = replayMode_ && replayBar_.box.h > 0 ? replayBar_.box.y + replayBar_.box.h + 4 : 0.0f;
        // The overlays in the view's corners stack under it.
        if (resourceBarAlign_ != 2) topLeft_.place(y, barH);
        if (resourceBarAlign_ != 0) statsBottom_ = y + barH;
        taResourceRect_ = {x, y, barW, barH};
        ta::GuiLayout L;
        L.scale = sc;
        L.ox = x - kArtX * sc;
        L.oy = y;
        {
            const SDL_Rect clip{int(x), int(y), int(std::ceil(barW)), int(std::ceil(barH))};
            SDL_RenderSetClipRect(ren_, &clip);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
            const SDL_FRect back{x, y, barW, barH};
            SDL_RenderFillRectF(ren_, &back);
            taTopBar_->drawSequence(s.intGaf, "PANELTOP", 0, kArtX, 0, L);
        }
        // +View: the viewed player's books (TotalA.exe reads +0x2a43 for the bar).
        const int shown = std::clamp(watchOnly() ? hudPlayer() : seenAs(), 0, std::max(0, frameNumPlayers() - 1));
        const PlayerR& p = framePlayer(shown);
        // The stored amounts as TotalA.exe shows them (0x468e7b..0x468f6c): a display
        // copy (game +0x37e3f, saved back at 0x468fec) steps toward the stored whole
        // number by a truncated eighth of the gap, at least one, then is held to the
        // storage cap -- energy then metal. It moves once per main-loop pass (the bar
        // is drawn at the end of 0x496790, which runs at most one sim tick), so the
        // readout rolls between the once-a-second settles and counts up from 0 at the
        // start. Stepped here 30 times a second of wall time, at any frame rate. The
        // bars fill from the same shown values (0x4690f3, 0x469184).
        {
            const uint64_t now = SDL_GetTicks64();
            if (!taShownAt_) taShownAt_ = now;
            int steps = int(std::min<uint64_t>((now - taShownAt_) * 30 / 1000, 30));
            taShownAt_ += uint64_t(steps) * 1000 / 30;
            if (steps == 30) taShownAt_ = now;   // a long stall: no catching up
            auto step = [](float shownV, float real) {
                const int a = int(shownV), b = int(real);   // _ftol (0x4e43a0) truncates
                int d = b - a;
                d = d / 8 != 0 ? d / 8 : (d > 0 ? 1 : d < 0 ? -1 : 0);
                return float(a + d);
            };
            while (steps-- > 0) {
                taShownE_ = step(taShownE_, p.energy);
                taShownM_ = step(taShownM_, p.metal);
                if (taShownE_ > p.energyCap) taShownE_ = p.energyCap;
                if (taShownM_ > p.metalCap) taShownM_ = p.metalCap;
            }
        }
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
            float tx = float(it->second.x);
            if (right) tx -= float(taConsole_->width(t));
            taConsole_->draw(ren_, L, tx, float(it->second.y), t, c);
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
        bar("ENERGYBAR", taShownE_, p.energyCap, taTopBar_->paletteColor(s.energyColor ? s.energyColor : 208));
        bar("METALBAR", taShownM_, p.metalCap, taTopBar_->paletteColor(s.metalColor ? s.metalColor : 224));
        text("ENERGYNUM", whole(taShownE_), white, false);
        text("ENERGY0", "0", white, false);
        text("ENERGYMAX", whole(p.energyCap), white, true);
        text("ENERGYPRODUCED", big(p.energyIncome), made, false);
        text("ENERGYCONSUMED", big(p.energyExpense), spent, false);
        text("METALNUM", whole(taShownM_), white, false);
        text("METAL0", "0", white, false);
        text("METALMAX", whole(p.metalCap), white, true);
        text("METALPRODUCED", tenth(p.metalIncome), made, false);
        text("METALCONSUMED", tenth(p.metalExpense), spent, false);
        SDL_RenderSetClipRect(ren_, nullptr);
    }

    // Clicks on the order buttons. True when the click belonged to them.
    bool GameView::taHudEvent(const SDL_Event& e) {
        if (taHudPage_.empty()) return false;
        ta::TaGuiView* page = taPage(taHudPage_);
        if (!page) return false;
        const bool mouse = e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP;
        if (mouse) {
            const float mx = float(e.button.x), my = float(e.button.y);
            const SDL_FRect& r = taOrdersRect_;
            if (mx < r.x || my < r.y || mx >= r.x + r.w || my >= r.y + r.h) return false;   // the crop
            if (!page->at(mx, my, taHudLayout_)) return false;
        }
        if (watchOnly()) return mouse;
        if (mouse && e.button.button != SDL_BUTTON_LEFT) return true;   // any other press on the page
        const std::string hit = page->handle(e, taHudLayout_);
        if (hit.empty()) return mouse;
        const int side = std::max(0, ta::sides::find(side_));
        const std::string prefix = upperStr(ta::sides::at(side).prefix);
        std::string n = upperStr(hit);
        if (n.rfind(prefix, 0) == 0) n = n.substr(prefix.size());
        // The press's retail sound (client/guisound.h: 0x419be0 orders).
        playGuiSound(ta::guisound::sidebar(n), "sidebar", n);
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
        else if (n == "FIREORD" || n == "MOVEORD" || n == "ONOFF" || n == "CLOAK") {
            // A state button sets the whole selection to the summary's next state
            // (handler 0x41a490, client/orderpage.h); the order goes to every
            // selected unit (0x48cf30), the sim keeping it to the types that
            // take it (Standing_*Order: 0x48d0d7 / 0x48d104).
            std::vector<ta::client::OrderPageUnit> sel;
            for (const UnitR* u : taOrderUnits())
                sel.push_back({u->type->orderFlags, u->moveState, u->fireState, u->active, u->cloakOn});
            const auto s = ta::client::orderSummary(sel);
            if (n == "FIREORD" && s.fire != 4)          // STANDING_FIREORDER (0x41a5d9..0x41a6ad)
                issuePerUnit(ta::net::Cmd::StandingOrder, ta::client::nextStandingState(s.fire), 1);
            else if (n == "MOVEORD" && s.move != 4)     // STANDING_MOVEORDER (0x41a4d5..0x41a5aa)
                issuePerUnit(ta::net::Cmd::StandingOrder, ta::client::nextStandingState(s.move), 0);
            else if (n == "ONOFF" && s.onOff != 3)      // ACTIVATE / DEACTIVATE (0x41a7db..0x41a88f)
                issuePerUnit(ta::net::Cmd::SetActive, ta::client::nextOnOff(s.onOff) ? 1 : 0);
            else if (n == "CLOAK" && s.cloak != 3)      // CLOAK_ON / CLOAK_OFF (0x41a743..0x41a7a5)
                issuePerUnit(ta::net::Cmd::Cloak, ta::client::nextCloak(s.cloak) ? 1 : 0);
        }
        return true;
    }



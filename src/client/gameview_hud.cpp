#include <ctime>
#include <iterator>
#include <type_traits>
#include "client/guiart.h"
#include "sim/taweapons.h"
#include "client/cliprect.h"
#include "client/gameview.h"
#include "client/gamespeed.h"
#include "client/artscale.h"
#include "client/cursorpath.h"
#include "client/cursorrange.h"
#include "client/unitstatus.h"
#include "client/cursorrules.h"
#include "sim/retailflyerspread.h"
#include "client/reclaimpick.h"
#include "client/gpuvram.h"
#include "client/renderdriver.h"
#include "client/uitext.h"
#include "client/chatbox.h"
#include "client/buildrow.h"
#include "client/statsfit.h"
#include "net/protocol.h"
#include "sim/retailtransport.h"
#include "sim/retailheight.h"
#include "sim/footprint.h"
#include "util/procmetrics.h"

// Out-of-line GameView method definitions (hud concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

namespace {
    // An OPEN gate's doorway at world point (wx,wz): the cell is one of the gate's
    // own 'c'/'C' yard cells (the cells retail's yard stamp 0x5062d0 flags 0x20) and
    // the yard is open. Yardmaps are indexed unrotated, as the sim does
    // (World::gatePassageAt): structures never turn their footprint.
    bool openGateDoorwayAt(const UnitR& u,float wx,float wz) {
        if (!u.yardOpen || !u.type || !u.type->gate || u.type->yardMap.empty()) return false;
        const auto& type=*u.type;
        const int dx=int(std::floor(wx/16))-ta::sim::footprintOrigin(u.x,type.footX);
        const int dz=int(std::floor(wz/16))-ta::sim::footprintOrigin(u.z,type.footZ);
        if (dx<0 || dz<0 || dx>=type.footX || dz>=type.footZ) return false;
        const char yard=type.yardMap[size_t(dz)*type.footX+dx];
        return yard=='c' || yard=='C';
    }
}

    // The reclaim target under a world point, read from the render snapshot: the cursor
    // asks every frame, and the two click sites (right-click, armed CLEAR) issue what it
    // returns. No simMutex_ anywhere -- the snapshot can be a frame stale, and the sim
    // validates the target when it applies the Reclaim (a gone feature is dropped).
    std::optional<int> GameView::reclaimTargetAt(float wx,float wz) const {
        ta::client::ReclaimPick pick{wx,wz};
        for (const auto& f:features_) {
            if (!f.hasSim || !f.aliveVis || !f.reclaimable || !canPickPoint(f.x,f.z)) continue;
            pick.offer(f.simId,f.x,f.z,f.fx,f.fz);
        }
        // Corpses / statues / rubble (negative id), sized by their FEATURE footprint.
        const auto& live=front().live;
        for (uint32_t index:bodyCandidates(wx,wz)) {
            const UnitR* u=live[index];
            if (u->alive() || !u->corpsePhase || !u->corpseReclaimable || !canPickPoint(u->x,u->z)) continue;
            pick.offer(-u->id,u->x,u->z,u->corpseFootX,u->corpseFootZ);
        }
        return pick.id;
    }

    // Mirrors World::canHelpBuild (retail HelpBuild, 0x519e60), which the sim
    // enforces again: no repeat/rally/busy test -- a conjurer on a repeat queue
    // helps too.
    bool GameView::canAssistSite(const UnitR& b,const UnitR& site) const {
        if (b.id==site.id || !b.alive() || b.embarked() || b.underConstruction ||
            !b.type || !b.type->isBuilder || b.type->isStructure() ||
            !site.alive() || !site.type || !site.underConstruction ||
            b.player!=site.player) return false;
        const auto& menu=registry_.buildable(b.type->id);
        return !b.type->builderLimited || std::find(menu.begin(),menu.end(),site.type->id)!=menu.end();
    }

    bool GameView::canLoadPassenger(const UnitR& u,const UnitR& t) const {
        if(u.id==t.id || !u.alive() || !t.alive() || u.embarked() || t.embarked() ||
           !u.type || !t.type || u.underConstruction || t.underConstruction ||
           u.paralyzedFor>0 || t.paralyzedFor>0) return false;
        if(u.type->isStructure() || u.type->canFly || u.type->cantBeTransported ||
           !t.type->canTransport || u.player!=t.player ||
           (!t.type->canFly && !u.type->transportLandEligible)) return false;
        if(int64_t(u.worldPosition[1])+u.type->modelTop<=
           int64_t(uint8_t(mapView_.map().seaLevel))*65536) return false;
        uint32_t count=0,used=0;
        for(int id:t.cargo) {
            const auto* c=frameUnitP(id);
            if(c && c->alive() && c->inTransport==t.id && c->type) {
                ++count;used+=uint16_t(c->type->transportSize);
            }
        }
        return ta::sim::retailTransportCapacity(uint16_t(u.type->transportSize),
            uint16_t(t.type->maxTransportSize),count,uint16_t(t.type->transportCap),
            used,uint16_t(t.type->transportSizeCap));
    }

    bool GameView::hardwareCursorInUse() const {
        return settings_ && ta::useHardwareCursor(settings_->hardwareCursor, hwCursorFailed_);
    }

    void GameView::drawCursorOverlay(bool uiOverlay) {
        if (!cursorsInit_) { cursorsInit_ = true; cursors_.load(ren_, vfs_, settings_); }
        // A benchmark RUN is hands-off: hide the cursor entirely (OS + software). It
        // returns for the stats screen (benchStatsShown_) so DONE is clickable.
        if (benchmarkMode_ && !benchStatsShown_ && !uiOverlay) {
            if (cursorMode_ != 0) { cursors_.releaseHardware(); SDL_ShowCursor(SDL_DISABLE); cursorMode_ = 0; }
            return;
        }
        if (!cursors_.ok()) {   // no cursor art -> just keep the OS arrow
            if (cursorMode_ != 1) { SDL_ShowCursor(SDL_ENABLE); cursorMode_ = 1; }
            return;
        }
        bool fightTint = false;
#ifndef NDEBUG
        const auto cursorStart = std::chrono::steady_clock::now();
        debugPickTests_ = 0;
#endif
        ta::CursorId c = ta::CursorId::Normal;
        if (!uiOverlay) {
            ta::render::ScopedPhase phase(renderStats_, ta::render::Phase::Cursor);
            renderStats_.add(ta::render::Counter::CursorEvals);
            c = desiredCursor(fightTint);
        }
#ifndef NDEBUG
        if (cursorStudyOn_ && !uiOverlay)
            cursorStudy_ = {uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - cursorStart).count()),
                            debugPickTests_, int(c), fightTint, mouseX_, mouseY_};
        static const bool pickVerify = ta::devFlag("TA_PICK_INDEX_VERIFY");
        if (pickVerify && !uiOverlay) {
            // The same decision from a scan over every unit (pickCandidates' reference).
            bool fullTint = false;
            pickForceFull_ = true;
            const ta::CursorId full = desiredCursor(fullTint);
            pickForceFull_ = false;
            ++pickVerify_.decisions;
            if (full != c || fullTint != fightTint)
                throw std::runtime_error("pick index verify: the cursor differs from a full scan");
        }
#endif
        int sc = settings_ ? settings_->cursorScale : 1;
        SDL_Color tint = fightTint ? kFightMoveTint : SDL_Color{255, 255, 255, 255};

        // HARDWARE cursor: hand the sprite to the OS, which tracks the pointer position
        // itself -- so it stays smooth even when a heavy frame stalls our render loop.
        // Streaming no longer forces the software path: the stream worker stamps this
        // cursor into the broadcast (client/cursorpath.h, video/streamcursor.h).
        if (hardwareCursorInUse()) {
            if (cursorMode_ != 1) { SDL_ShowCursor(SDL_ENABLE); cursorMode_ = 1; }
            if (cursors_.applyHardware(c, sc, tint)) return;
            hwCursorFailed_ = true;   // platform rejected it (size cap?) -> software from here on
                                      // (CursorSet logged the size and SDL's reason)
        }

        // SOFTWARE cursor: hide the OS arrow and draw our own into the frame.
        if (cursorMode_ != 0) { cursors_.releaseHardware(); SDL_ShowCursor(SDL_DISABLE); cursorMode_ = 0; }
        // Pointer position in renderer-output pixels (the space mouse events are mapped
        // into). Before the first motion, sample the OS position and map it the same way.
        int mx, my;
        if (!uiOverlay && screenMouseX_ >= 0) { mx = int(screenMouseX_); my = int(screenMouseY_); }
        else {
            int wx, wy; SDL_GetMouseState(&wx, &wy);
            float lx, ly; SDL_RenderWindowToLogical(ren_, wx, wy, &lx, &ly);
            mx = int(lx); my = int(ly);
        }
        cursors_.draw(ren_, c, mx, my, sc, tint);
    }

    ta::CursorId GameView::desiredCursor(bool& fightTint) {
        // Retail draws fight-move with the plain MOVE cursor (the selector never
        // reads the fight flag), so nothing is tinted any more.
        fightTint = false;
        using namespace ta::client;
        const PointerArea area = pointerArea();
        CursorFrame frame;
        // The Benchmark's results screen lies over the frozen game: the pointer there is on
        // the results (SAVE AS JPG, DONE), never on a unit under them.
        frame.modalUi = inLobbyPhase() || exitMenu_ || giveUnitsMenu_ || options_ || benchStatsShown_;
        frame.area = area;
        frame.watcher = watchOnly();
        if (frame.watcher && !frame.modalUi && area == PointerArea::World && !dragging_) {
            // A watcher has no orders to promise: Select over what a click selects.
            const auto& live = front().live;
            for (uint32_t index : pickCandidates(mouseX_, mouseY_)) {
                const UnitR& u = *live[index];
                if (canPickUnit(u) && u.alive() && u.type && !u.underConstruction &&
                    unitUnderCursor(u, mouseX_, mouseY_)) { frame.watcherOverUnit = true; break; }
            }
        }
        frame.placing = placing_ != nullptr;
        if (frame.placing && !frame.watcher && !frame.modalUi && area == PointerArea::Minimap) {
            float wx = 0, wz = 0;
            frame.minimapPlaceOk = minimapToWorld(screenMouseX_, screenMouseY_, winW_, winH_, wx, wz) && minimapPlaceOk(wx, wz);
        }
        // A box past its area threshold (the right-drag, an armed A/L/C left-drag)
        // shows what its release does; before that, the cursor of the click.
        const auto boxMode = rightDrag_ ? std::optional<BoxMode>(BoxMode::Right)
                           : armedPress_ ? armedBoxMode(pendingCmd_) : std::nullopt;
        const auto& boxAnchor = rightDrag_ ? rdAnchor_ : dragAnchor_;
        const bool areaBox = (rightDrag_ || armedPress_) && !frame.placing && !frame.modalUi && !frame.watcher &&
                             areaBoxAt(boxAnchor, mouseX_, mouseY_);
        if (areaBox && boxMode) {
            const auto a = dragStartOnScreen(boxAnchor);
            frame.areaBoxDrag = true;
            frame.areaBoxCursor = planBox(*boxMode, std::min(a.x, mouseX_), std::min(a.y, mouseY_),
                                          std::max(a.x, mouseX_), std::max(a.y, mouseY_)).cursor;
        }
        // An armed press of another command past the threshold is a box select.
        frame.boxDrag = armedPress_ ? areaBox && !boxMode : dragging_;
        frame.armed = pendingCmd_ != 0;
        ta::CursorId fixed = ta::CursorId::Normal;
        const CursorStage stage = cursorStage(frame, fixed);
        if (stage == CursorStage::Fixed) return fixed;

        // Ours: with nothing armed the minimap keeps Normal. Retail's hover cursor
        // describes the LEFT click in the right-click interface, and on the minimap
        // that is the camera.
        if (stage == CursorStage::Hover && area == PointerArea::Minimap) return ta::CursorId::Normal;
        float wx = 0, wz = 0;
        if (area == PointerArea::Minimap) minimapToWorld(screenMouseX_, screenMouseY_, winW_, winH_, wx, wz);
        else pickWorld(mouseX_, mouseY_, wx, wz);
        const UnitR* hovered = hoveredUnit(area);
        if (stage == CursorStage::Hover) return hoverCursor(wx, wz, hovered);
        // Armed: the per-unit decisions the click itself takes (clickrules.h), the
        // lowest cursor slot over the selected units but the hovered one (0x521cd0).
        return planClick(pendingCmd_, wx, wz, hovered, keyQueue(), false,
                         area == PointerArea::World).cursor;
    }

    // Where the pointer is, as retail's 0x528430 sorts it: the main view, the
    // minimap, or interface (the side panel, the top and bottom bars, the replay
    // bar), where every cursor is Normal. The minimap and the frame are tested in
    // window coordinates, the replay bar (in the view) in view coordinates.
    ta::client::PointerArea GameView::pointerArea() const {
        using ta::client::PointerArea;
        if (screenMouseX_ < 0 || screenMouseY_ < 0) return PointerArea::Unknown;
        if (replayMode_ && replayBar_.box.hit(mouseX_, mouseY_)) return PointerArea::Interface;
        const SDL_FRect mini = minimapRect(winW_, winH_);
        const float sx = screenMouseX_, sy = screenMouseY_;
        if (sx >= mini.x && sy >= mini.y && sx <= mini.x + mini.w && sy <= mini.y + mini.h)
            return PointerArea::Minimap;
        if (!inView(sx, sy)) return PointerArea::Interface;
        return PointerArea::World;
    }

    // Retail 0x521260: the ONE unit under the pointer, which the cursor and every
    // click use alike (P1). In the main view, among the units whose drawn region
    // contains the pointer (unitUnderCursor: the projected model bounds + lift +
    // flyer altitude; an open gate's doorway is not the gate), the SMALLEST wins --
    // retail sorts by (height/2 + depth) x width of the type's model box
    // (0x52133e..0x521381 S), the screen area of that box, which is what our hit
    // region measures -- a tie to the nearer centre, then the earlier entry. A
    // carried unit is not drawn and never picked. On the minimap, the nearest dot
    // within 2 px, from the units the minimap draws (0x5213ae S).
    const UnitR* GameView::hoveredUnitAt(ta::client::PointerArea area, float mx, float my) {
        const UnitR* best = nullptr;
        float bestD = 1e30f, bestSize = 1e30f;
        if (area == ta::client::PointerArea::Minimap) {
            const SDL_FRect r = minimapRect(winW_, winH_);
            const float mapW = float(mapView_.map().blocksX) * 32, mapH = float(mapView_.map().blocksY) * 32;
            if (mapW <= 0 || mapH <= 0) return nullptr;
            for (const UnitR* up : front().live) {
                const UnitR& u = *up;
                if (radarCode_ ? !u.alive() || u.embarked() || !u.type : !radarVisible(u)) continue;
                if (!u.alive() || !u.type) continue;
                // The dot's own point: the projected row, z - (y >> 1) (0x48cee1).
                const float pz = u.z - float((u.worldPosition[1] >> 16) >> 1);
                const float dx = r.x + u.x / mapW * r.w - mx, dy = r.y + pz / mapH * r.h - my;
                const float d = dx * dx + dy * dy;
                if (d <= 4.0f && d < bestD) { bestD = d; best = &u; }
            }
            return best;
        }
        if (area != ta::client::PointerArea::World) return nullptr;
        const auto& live = front().live;
        for (uint32_t index : pickCandidates(mx, my)) {
            const UnitR& u = *live[index];
            if (!canPickUnit(u) || !u.alive() || u.embarked() || !u.type) continue;
            float d = 0, size = 0;
            if (!unitUnderCursor(u, mx, my, &d, &size)) continue;
            if (size < bestSize || (size == bestSize && d < bestD)) { bestSize = size; bestD = d; best = &u; }
        }
        return best;
    }

    // The hovered unit as the selector sees it from the local player's side:
    // Select needs our own FINISHED unit (0x4de03b's test); anything else of
    // ours or an ally's is friendly, the rest enemy. One a mission script runs
    // (+0x130 & 0x20 clear) is not selectable.
    ta::client::HoverKind GameView::cursorHoverKind(const UnitR* u) const {
        using ta::client::HoverKind;
        if (!u) return HoverKind::None;
        if (u->player == localPlayer_ && !u->underConstruction && !u->embarked() && u->selectable)
            return HoverKind::OwnSelectable;
        return worldTables().allied(u->player, localPlayer_) ? HoverKind::Friendly : HoverKind::Enemy;
    }

    // A selected unit that takes part in the armed-attack cursor: retail's
    // `canattack` (WEAPON1 block present) on a live, finished, unembarked unit.
    // A factory's build repeat suppresses it, as a structure's does not.
    bool GameView::isCursorAttacker(const UnitR& u) const {
        return u.alive() && !u.embarked() && !u.underConstruction && u.type &&
               u.type->hasPrimaryWeaponBlock && !(u.repeatType && !u.type->isStructure());
    }

    // Nothing armed: retail's right-click interface (UseLeftClickIface=0, the
    // shipped setting, and the interface our right-click orders follow) shows only
    // Select, Red, Green and Normal on hover, whatever the selection can do -- an
    // enemy is Red, an ally's unit or one of ours under construction (the site the
    // selected builder is working on included) is Green, and a body or feature a
    // selected unit could raise or reclaim is Green. Attack, Repair, Load, Unload,
    // Revive and Reclaim appear only with the command armed. cursorrules.h.
    ta::CursorId GameView::hoverCursor(float wx, float wz, const UnitR* hovered) {
        ta::client::HoverFacts f;
        f.target = cursorHoverKind(hovered);
        f.othersSelected = std::any_of(selection_.begin(), selection_.end(), [&](int id) {
            const auto* u = frameUnitP(id);
            return u && u->alive() && u->player == localPlayer_ && (!hovered || u->id != hovered->id);
        });
        if (f.othersSelected && f.target == ta::client::HoverKind::None) {
            f.raisableAtPoint = raiseTargetAt(wx, wz) != 0;
            f.reclaimableAtPoint = !f.raisableAtPoint && haveReclaimer() && reclaimTargetAt(wx, wz);
        }
        return ta::client::cursorForHover(f);
    }

    void GameView::postNotice(std::string msg, float t) {
        if (std::this_thread::get_id() == mainThreadId_) { notice_ = std::move(msg); noticeTimer_ = t; return; }
        std::lock_guard<std::mutex> lk(noticeMutex_);
        pendingNotice_ = std::move(msg); pendingNoticeTimer_ = t; pendingNoticeSet_ = true;
    }

    void GameView::drainPendingNotice() {   // main thread only
        std::lock_guard<std::mutex> lk(noticeMutex_);
        if (pendingNoticeSet_) { notice_ = std::move(pendingNotice_); noticeTimer_ = pendingNoticeTimer_; pendingNoticeSet_ = false; }
    }

    void GameView::resetMinimap() {
        if (miniThread_.joinable()) miniThread_.join();
        miniBuilding_ = miniReady_ = false;
        miniPix_.clear();
        if (miniTex_) { gpuvram::destroy(miniTex_); miniTex_ = nullptr; }
    }

    int GameView::cmdPanelW() const { return hudColW(); }

    // Window coordinates. The minimap is the map fitted into a box at the top of the
    // right-hand column (126 x UI SCALE x MINIMAP SCALE, as TotalA.exe's 0x466780
    // fits it into 126x126): the longer side fills the box, the shorter one its share,
    // centred on that axis (0x4667c6 / 0x4667eb).
    SDL_FRect GameView::minimapRect(int winW, int winH) const {
        const float bx = float(mapView_.map().blocksX), by = float(mapView_.map().blocksY);
        float aspect = by / std::max(1.0f, bx);
        if (fsRadar_) {
            // The full-screen radar fills the world view, the map's aspect fitted
            // inside and centred rather than stretched (a 2:1 map on a 16:9 view
            // would distort every distance on it).
            float vw = float(mapViewW(winW)), vh = float(mapViewH(winH));
            float w = vw, h = vw * aspect;
            if (h > vh) { h = vh; w = vh / aspect; }
            return {float(viewX0()) + (vw - w) * 0.5f, float(viewY0()) + (vh - h) * 0.5f, w, h};
        }
        const float box = miniBoxSize();
        float w = box, h = box;
        if (bx >= by) h = std::floor(box * by / std::max(1.0f, bx));
        else w = std::floor(box * bx / std::max(1.0f, by));
        const float colX = float(winW - hudColW());
        return {std::floor(colX + (float(hudColW()) - w) / 2), std::floor(hudColPad() + (box - h) / 2), w, h};
    }

    void GameView::buildMinimap() {
        if (miniBuilding_) {
            std::lock_guard<std::mutex> lk(miniMu_);
            if (!miniReady_) return;    // still crunching
            int bw = mapView_.map().blocksX, bh = mapView_.map().blocksY;
            miniTex_ = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, bw, bh);
            SDL_UpdateTexture(miniTex_, nullptr, miniPix_.data(), bw * 4);
            SDL_SetTextureScaleMode(miniTex_, SDL_ScaleModeLinear);
            miniRgb_ = std::move(miniPix_);
            miniGreySet_ = false;
            miniPix_.clear();
            miniBuilding_ = false;
            if (miniThread_.joinable()) miniThread_.join();
            return;
        }
        miniBuilding_ = true;
        miniReady_ = false;
        if (miniThread_.joinable()) miniThread_.join();   // stale thread from a reload
        miniThread_ = std::thread([this] {
            int bw = mapView_.map().blocksX, bh = mapView_.map().blocksY;
            std::vector<uint8_t> pix(size_t(bw) * bh * 4);
            std::vector<uint8_t> block(32 * 32 * 4);
            for (int bz = 0; bz < bh; ++bz)
                for (int bx = 0; bx < bw; ++bx) {
                    mapView_.compositor().renderBlock(mapView_.map(), bx, bz, block, 32, 0, 0);
                    uint32_t r = 0, g = 0, b = 0;
                    for (size_t i = 0; i < block.size(); i += 4) {
                        r += block[i]; g += block[i + 1]; b += block[i + 2];
                    }
                    size_t n = block.size() / 4;
                    uint8_t* p = &pix[(size_t(bz) * bw + bx) * 4];
                    p[0] = uint8_t(r / n); p[1] = uint8_t(g / n); p[2] = uint8_t(b / n);
                    p[3] = 255;
                }
            std::lock_guard<std::mutex> lk(miniMu_);
            miniPix_ = std::move(pix);
            miniReady_ = true;
        });
    }

    float GameView::drawStatsPanel(float x0, float x1, float top, float bot) {
        // The rows draw in the UI face (blockText -> ta::uiText), not hudFont_: that is
        // fonts/bodfontbody.gaf, a Kingdoms font TA does not ship, and gating on it hid
        // the panel (and a benchmark run's PATHFINDER row) on every TA install.
        if (!statsPanel_) return top;

        // WHERE IT GOES: the right-hand column, under the minimap and the selected
        // unit's readout (window coordinates, x0..x1 from `top` down to `bot`), at
        // UI SCALE x STATS SCALE.
        const float pad = std::max(4.0f, 6.0f * statsScaleEff());
        float availW = x1 - x0, availH = bot - top;
        if (availW <= 8 || availH <= 4) return top;

        // WHAT TO SHOW, most useful first. Rows are dropped from the END of this list,
        // so the ordering is the priority ordering: frame rate before link quality before
        // counts before process stats.
        //
        // Labels are kept to the width budget below (<=10 chars) and values to <=7, which
        // is what lets the type size stay put instead of resizing when a value gains a
        // digit.
        struct Row { const char* label; std::string value; };
        std::vector<Row> rows;
        char b[48];

        std::snprintf(b, sizeof b, "%.0f", fps_ + 0.5f);
        rows.push_back({"FPS", b});

        if (mp_) {
            std::snprintf(b, sizeof b, "%.0f MS", netRttMs());
            rows.push_back({"PING", b});
        }
        if (actualSpeed_ > 0) {
            // The measured speed as retail's level (NORMAL, +5, -3), not a multiplier.
            rows.push_back({"SIM", ta::speedLevelText(ta::speedLevelOf(actualSpeed_))});
        }
        // A benchmark run's identity, so two runs on screen can be told apart.
        if (benchmarkMode_) rows.push_back({"PATHFINDER", ta::benchmarkPathLabel(worldTables().pathfindingMode())});

        // Living units owned by the player on show, including embarked units. With
        // nobody on show (a watcher, nothing selected) the match-wide living count.
        int units = 0;
        const int shown = hudPlayer();
        for (const UnitR* up : front().live)
            if (up->alive() && up->type && (shown < 0 || up->player == shown)) ++units;
        std::snprintf(b, sizeof b, "%d", units);
        rows.push_back({"UNITS", b});

        const std::time_t clockNow = std::time(nullptr);
        std::tm localClock{};
#ifdef _WIN32
        const bool haveClock = localtime_s(&localClock, &clockNow) == 0;
#else
        const bool haveClock = localtime_r(&clockNow, &localClock) != nullptr;
#endif
        if (haveClock) std::strftime(b, sizeof b, "%H:%M", &localClock);
        else std::snprintf(b, sizeof b, "--:--");
        rows.push_back({"CLOCK", b});

        if (shown >= 0) {
            std::snprintf(b, sizeof b, "%d", framePlayer(shown).kills);
            rows.push_back({"KILLS", b});
            rows.push_back({"SCORE", std::to_string(framePlayer(shown).score)});
        }
        const uint64_t wallNow = SDL_GetTicks64();
        const uint64_t elapsed = gameStartMs_ && wallNow >= gameStartMs_
            ? (wallNow - gameStartMs_) / 1000 : 0;
        std::snprintf(b, sizeof b, "%llu:%02llu",
                      (unsigned long long)(elapsed / 60), (unsigned long long)(elapsed % 60));
        rows.push_back({"REAL TIME", b});
        const uint32_t gameSecs = front().gameTick / uint32_t(ta::net::kServerHz);
        std::snprintf(b, sizeof b, "%u:%02u", gameSecs / 60, gameSecs % 60);
        rows.push_back({"GAME TIME", b});

        const uint64_t now = SDL_GetTicks64();
        if (!statsSampleAt_ || now - statsSampleAt_ >= 1000) {
            const auto client = ta::proc::sample(0);
            statsCpuPct_ = ta::proc::processCpuPercent(statsProcess_, client,
                statsSampleAt_ ? double(now-statsSampleAt_)/1000.0 : 0.0, ta::proc::numCpus());
            statsProcess_ = client;
            if(localServerPid_>0) {
                const auto server=ta::proc::sample(localServerPid_);
                statsServerCpuPct_=ta::proc::processCpuPercent(statsServer_,server,
                    statsSampleAt_ ? double(now-statsSampleAt_)/1000.0 : 0.0,ta::proc::numCpus());
                statsServer_=server;
            }
            statsSampleAt_ = now;
        }
        // GPU driver queries can take time. Never wait for them in a
        // frame: keep one background request in flight and reuse the last result.
        if (statsGpuPending_.valid() &&
            statsGpuPending_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            statsGpu_ = statsGpuPending_.get();
        if (!statsGpuPending_.valid() && (!statsGpuAt_ || now - statsGpuAt_ >= 2000)) {
            statsGpuAt_ = now;
            statsGpuPending_ = std::async(std::launch::async, [] { return ta::proc::gpuSample(); });
        }
        if (statsCpuPct_ >= 0) std::snprintf(b, sizeof b, "%.0f%%", statsCpuPct_);
        else std::snprintf(b, sizeof b, "N/A");
        rows.push_back({"CLIENT CPU", b});   // client share of total machine CPU capacity, 0–100%
        if(localServerPid_>0) {
            if(statsServerCpuPct_>=0)std::snprintf(b,sizeof b,"%.0f%%",statsServerCpuPct_);
            else std::snprintf(b,sizeof b,"N/A");
            rows.push_back({"SERVER CPU",b});
        }
        const double gpuPct=ta::proc::systemGpuPercent(statsGpu_);
        if (gpuPct >= 0)
            std::snprintf(b, sizeof b, "%.0f%%", gpuPct);
        else std::snprintf(b, sizeof b, "N/A");
        rows.push_back({"GPU", b});

        if (statsProcess_.ok && statsProcess_.rssBytes) {
            std::snprintf(b, sizeof b, "%zu MB", statsProcess_.rssBytes >> 20);
            rows.push_back({"MEM", b});
        }
        std::snprintf(b, sizeof b, "%zu MB", gpuvram::bytes() >> 20);
        rows.push_back({"VID", b});
        rows.push_back({"RENDERER", ta::renderdriver::shortLabel(ta::renderdriver::active().created)});

        // The HUD's UI face (client/uitext.h), so the panel matches the HUD
        // it sits in rather than introducing a second face.
        //
        // Fixed size follows UI scale. Reserve ten label characters (SERVER CPU),
        // one separator and seven value characters without overlapping columns: the
        // per-character budget is the block cell (6), or the UI font's average over
        // such a row when that is wider, so the columns never meet in either face.
        constexpr int kColBudget = 18;
        const float px = 1.7f * statsScaleEff();
        const float glyphW = std::max(6.0f, blockWidth("SERVER CPU 9999 MB", 1.0f) / float(kColBudget));
        const ta::hud::StatsFit fit =
            ta::hud::fitStats(int(rows.size()), availW, availH, kColBudget,
                               glyphW, /*glyphH=*/7.0f, px,
                               // Breathing room between rows. The gap under the minimap
                               // runs to hundreds of pixels while nine rows need barely a
                               // hundred, so there is room to spare -- and this only ever
                               // feeds the HEIGHT fit, never the type size.
                               /*rowPad=*/6.0f * statsScaleEff());
        if (!fit.visible) return top;
        rows.resize(size_t(fit.rows));

        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        {   // a dark plate, so the column reads over the terrain
            const SDL_FRect plate{x0 - pad / 2, top - pad / 2, availW + pad, fit.rowH * float(fit.rows) + pad / 2};
            SDL_SetRenderDrawColor(ren_, 10, 10, 14, 170);
            SDL_RenderFillRectF(ren_, &plate);
        }
        const SDL_Color mc{200, 215, 255, 255};   // the readout's colour
        float y = top;
        for (const auto& r : rows) {
            blockText(r.label, x0, y, px, mc);
            // Values right-align against the strip edge so the digits form a column and a
            // changing number does not shuffle the ones above it sideways.
            blockText(r.value, x1 - blockWidth(r.value, px), y, px, mc);
            y += fit.rowH;
        }
#ifndef NDEBUG
        // TA_BENCH_REPORT: the rows as drawn during a run, for benchmark_pathfinding.
        static bool reported = false;
        if (!reported && worldTables().benchmarkMode() && ta::devFlag("TA_BENCH_REPORT")) {
            reported = true;
            for (const auto& r : rows) std::printf("BENCHSTATS %s %s\n", r.label, r.value.c_str());
            std::fflush(stdout);
        }
#endif
        return y;
    }

    void GameView::drawMinimap(int winW, int winH) {
        (void)winW;
        if (!miniTex_) buildMinimap();
        SDL_FRect r = minimapRect(winW, winH);
        // Full-screen: black out the WHOLE world viewport first. Retail's radar
        // rect simply is the viewport, so no world shows around it; ours fits the
        // map's aspect inside, which would leave the terrain peeking through the
        // letterbox margins and read as a window rather than a map. In the corner
        // the side panel's art (PANELSIDE) shows round a map that is not square.
        if (fsRadar_) {
            const SDL_FRect frame{float(viewX0()), float(viewY0()), float(mapViewW(winW)), float(mapViewH(winH))};
            SDL_SetRenderDrawColor(ren_, 30, 30, 40, 255);
            SDL_RenderFillRectF(ren_, &frame);
        }
        if (miniTex_) SDL_RenderCopyF(ren_, miniTex_, nullptr, &r);
        // TotalA.exe 0x466c20: the minimap shows the local plane cell by cell,
        // unblended; out of view each cell's pixel through the grey table.
        if (!miniGreySet_ && !front().fog.empty()) {
            if (!fogArtLoaded_) loadFogArt();
            const int cw = front().fogW, ch = front().fogH;
            if (fogGreyLut_.size() == 256 * 3 && miniRgb_.size() == size_t(cw) * size_t(ch) * 4) {
                std::vector<uint8_t> grey(size_t(cw) * size_t(ch) * 3);
                for (size_t i = 0; i < size_t(cw) * size_t(ch); ++i) {
                    const int level = (int(miniRgb_[i * 4]) + miniRgb_[i * 4 + 1] + miniRgb_[i * 4 + 2]) / 3;
                    for (int c = 0; c < 3; ++c) grey[i * 3 + size_t(c)] = fogGreyLut_[size_t(level) * 3 + size_t(c)];
                }
                miniFogUpload_.setMinimapColours(std::move(grey));
                miniFogTexGen_ = ~0u;
                miniGreySet_ = true;
            }
        }
        if (!noFog_ && !front().fog.empty() && syncFogTexture(miniFogTex_, miniFogUpload_, miniFogTexGen_)) {
            SDL_SetTextureScaleMode(miniFogTex_, SDL_ScaleModeNearest);
            SDL_RenderCopyF(ren_, miniFogTex_, nullptr, &r);
        }

        float mapW = float(mapView_.map().blocksX) * 32;
        float mapH = float(mapView_.map().blocksY) * 32;
        auto toMini = [&](float wx, float wz) {
            return SDL_FPoint{r.x + wx / mapW * r.w, r.y + wz / mapH * r.h};
        };
        drawMinimapMarkers(r, mapW, mapH);
        // The fallback dots (no fx.gaf art), batched into one draw call.
        overlayBatch_.clear();
        if (!radLogo_[0]) for (const UnitR* _up : front().live) { const UnitR& u = *_up;
            // A spectator (noFog_) sees every unit on the radar; a player sees allied
            // units, and enemies in view or within its radar (radarVisible). The
            // +Radar power code shows them all here, on the minimap only (retail's
            // one reader of the flag is the minimap dot pass, 0x4f9561).
            if (radarCode_ ? !u.alive() || u.embarked() || !u.type : !radarVisible(u)) continue;
            // 0x4f95c5: the dot sits at the unit's projected row, z - (y >> 1), on
            // the same tile art the minimap shows.
            SDL_FPoint p = toMini(u.x, u.z - float((u.worldPosition[1] >> 16) >> 1));
            SDL_Color tc = playerColor(u.player);
            pushQuad(overlayBatch_, p.x - 1.5f, p.y - 1.5f, 3, 3, tc);
        }
        if (!overlayBatch_.empty())
            SDL_RenderGeometry(ren_, nullptr, overlayBatch_.data(),
                               int(overlayBatch_.size()), nullptr, 0);
        // Camera view rectangle.
        float zm = mapView_.zoom();
        SDL_FPoint a = toMini(mapView_.offX(), mapView_.offY());
        SDL_FRect view{a.x, a.y, mapViewW(winW) / zm / mapW * r.w,
                       mapViewH(winH) / zm / mapH * r.h};
        SDL_SetRenderDrawColor(ren_, 240, 240, 240, 200);
        SDL_RenderDrawRectF(ren_, &view);
    }

    // TotalA.exe's minimap markers (0x466dc0 / 0x4671a0, docs/retail-fog.md 5.2):
    //  - each shown unit is its owner colour's radlogo frame at its projected point;
    //  - a selected unit draws its radar and sonar range in green and its jammer
    //    ranges in red (guipal 0xa / 0xc, 0x4c0070) -- an onoffable one only while
    //    switched on -- and an antiweapons unit its weapons' coverage;
    //  - every shot in view, or the viewed player's own, is a yellow pixel (guipal
    //    0xe), a targetable or interceptor weapon's (bits 29-30) a nuclogo in its
    //    owner's colour; a noradar weapon's never shows.
    // Sizes are retail's pixels times the minimap's scale over retail's 128 px box.
    const UnitR* GameView::pointerUnit() {
        if (inView(screenMouseX_, screenMouseY_)) return hoveredUnit(ta::client::PointerArea::World);
        return hoveredUnitAt(ta::client::PointerArea::Minimap, screenMouseX_, screenMouseY_);
    }

    void GameView::drawMinimapMarkers(const SDL_FRect& r, float mapW, float mapH) {
        if (!radLogosLoaded_) {
            radLogosLoaded_ = true;
            // In the game palette (palettes/palette.pal), as the minimap draws them: the
            // GUI palette gafTexture uses maps the player-colour entries alike, which
            // made every dot one colour. Point-sampled: 4 x 4 art scaled up stays crisp.
            try {
                const std::string palPath = "palettes/palette.pal", gafPath = "anims/fx.gaf";
                const auto pal = ta::gaf::Palette::fromBytes(vfs_.read(palPath), palPath);
                for (auto& sq : ta::gaf::load(vfs_.read(gafPath), pal, -1, gafPath)) {
                    auto make = [&](size_t k) -> SDL_Texture* {
                        if (k >= sq.frames.size() || !sq.frames[k].width) return nullptr;
                        SDL_Texture* t = ta::art::makeTexture(ren_, sq.frames[k].rgba, sq.frames[k].width, sq.frames[k].height);
                        if (t) SDL_SetTextureScaleMode(t, SDL_ScaleModeNearest);
                        return t;
                    };
                    if (sq.name == "radlogo") for (size_t i = 0; i < 10; ++i) radLogo_[i] = make(i);
                    else if (sq.name == "nuclogo") for (size_t i = 0; i < 10; ++i) nucLogo_[i] = make(i);
                    else if (sq.name == "radlogohigh") radLogoHigh_ = make(0);
                }
            } catch (const std::exception& e) { std::fprintf(stderr, "minimap logos: %s\n", e.what()); }
        }
        if (!radLogo_[0]) return;
        const float px = std::max(1.0f, std::round(std::min(r.w, r.h) / 128.0f));
        auto toMini = [&](float wx, float wz) { return SDL_FPoint{r.x + wx / mapW * r.w, r.y + wz / mapH * r.h}; };
        auto logo = [&](SDL_Texture* t, SDL_FPoint p, float size) {
            if (!t) return;
            const SDL_FRect d{std::floor(p.x - (size / 2) * px), std::floor(p.y - (size / 2) * px), size * px, size * px};
            SDL_RenderCopyF(ren_, t, nullptr, &d);
        };
        auto ring = [&](SDL_FPoint c, float radiusPx, SDL_Color col) {
            if (radiusPx <= 0) return;
            const float rx = radiusPx / mapW * r.w, rz = radiusPx / mapH * r.h;
            SDL_SetRenderDrawColor(ren_, col.r, col.g, col.b, 255);
            constexpr int kSegs = 48;
            SDL_FPoint pts[kSegs + 1];
            for (int i = 0; i <= kSegs; ++i) {
                const float a = float(i) * 6.2831853f / kSegs;
                pts[i] = {c.x + rx * std::cos(a), c.y + rz * std::sin(a)};
            }
            SDL_RenderDrawLinesF(ren_, pts, kSegs + 1);
        };
        constexpr SDL_Color kGreen{85, 255, 85, 255}, kRed{255, 85, 85, 255}, kYellow{255, 255, 85, 255};
        const auto& f = front();
        const UnitR* pointed = pointerUnit();
        // 0x466eb9..0x466eca: a blinking dot shows only in the phase where game
        // +0x142f1 bit 0 is set; 0x466580 flips it every 8 game ticks.
        const uint32_t now = f.gameTick;
        const bool blinkOn = (now >> 3) & 1;
        std::erase_if(miniBlink_, [&](const auto& e) { return e.second <= now; });
        for (const UnitR* up : f.live) {
            const UnitR& u = *up;
            if (radarCode_ ? !u.alive() || u.embarked() || !u.type : !radarVisible(u)) continue;
            const SDL_FPoint p = toMini(u.x, u.z - float((u.worldPosition[1] >> 16) >> 1));
            if (!blinkOn && miniBlink_.count(u.id)) continue;
            logo(radLogo_[size_t(std::clamp(playerColorSlot(u.player), 0, 9))], p, 4);
            // 0x466f18: the unit under the pointer (game +0x2cba) also gets radlogohigh.
            if (&u == pointed) logo(radLogoHigh_, p, 6);
        }
        for (int id : selection_) {
            const UnitR* u = frameUnitP(id);
            if (!u || !u->alive() || !u->type) continue;
            if (u->type->onOffable && !u->active) continue;
            const SDL_FPoint p = toMini(u->x, u->z - float((u->worldPosition[1] >> 16) >> 1));
            const float ypx = float(u->worldPosition[1] >> 16);
            // The ranges detection uses: DOUBLE SIGHT/RADAR doubles them (World::radarDistance).
            if (u->type->radar) ring(p, float(worldTables().radarDistance(*u->type)) + 2 * std::max(0.0f, ypx), kGreen);
            if (u->type->sonar) ring(p, float(worldTables().sonarDistance(*u->type)), kGreen);
            if (u->type->radarJam) ring(p, float(u->type->radarJam), kRed);
            if (u->type->sonarJam) ring(p, float(u->type->sonarJam), kRed);
            if (u->type->antiWeapons)
                for (const auto& w : u->type->weapons) if (w.coverage > 0) ring(p, float(w.coverage), kGreen);
        }
        overlayBatch_.clear();
        for (const auto& shot : f.projectiles) {
            if (shot.spent || shot.life <= 0 || !shot.wsrc) continue;
            const auto& w = *shot.wsrc;
            if (w.hasTaFlag(ta::sim::kWNoRadar)) continue;
            const bool native3d = shot.ballistic3d || shot.guided3d || shot.taLineOfSight;
            const float sx = native3d ? float(shot.position[0] >> 16) : shot.x.toFloat();
            const float sy = native3d ? float(shot.position[1] >> 16) : 0.0f;
            const float sz = native3d ? float(shot.position[2] >> 16) : shot.z.toFloat();
            if (!noFog_ && shot.fromPlayer != seenAs() &&
                projectedFogCellR(sx, sz - sy / 2) != ta::retailfog::kInView) continue;
            const SDL_FPoint p = toMini(sx, sz - sy / 2);
            if (w.hasTaFlag(ta::sim::kWTargetable) || w.hasTaFlag(ta::sim::kWInterceptor))
                logo(nucLogo_[size_t(std::clamp(playerColorSlot(shot.fromPlayer), 0, 9))], p, 7);
            else pushQuad(overlayBatch_, std::floor(p.x), std::floor(p.y), px, px, kYellow);
        }
        if (!overlayBatch_.empty())
            SDL_RenderGeometry(ren_, nullptr, overlayBatch_.data(), int(overlayBatch_.size()), nullptr, 0);
    }

    bool GameView::minimapToWorld(float mx, float my, int winW, int winH, float& wx, float& wz) {
        SDL_FRect r = minimapRect(winW, winH);
        if (mx < r.x || my < r.y || mx > r.x + r.w || my > r.y + r.h) return false;
        wx = (mx - r.x) / r.w * float(mapView_.map().blocksX) * 32;
        wz = (my - r.y) / r.h * float(mapView_.map().blocksY) * 32;
        return true;
    }

    bool GameView::minimapClick(float mx, float my, int winW, int winH) {
        float wx, wz;
        if (!minimapToWorld(mx, my, winW, winH, wx, wz)) return false;
        float zm = mapView_.zoom();   // centre the clicked point in the map viewport
        mapView_.setOffset(wx - mapViewW(winW) / zm / 2,
                           wz - mapViewH(winH) / zm / 2);
        return true;
    }

    void GameView::armedOrderIssued(bool queue) {
        // Shift queues the order AND keeps its command armed for the next
        // click; without Shift the order disarms at once.
        shiftOrderArmed_ = queue;
        if (!queue) pendingCmd_ = 0;
    }

    void GameView::loadInterfaceFonts() {
        // By role (client/font.h): bodfontbody and the Times faces are reading text and
        // follow UI FONT; font48 is retail lettering and always draws its art.
        auto load = [&](Font& font, const char* path, FontRole role) {
            font.destroyGlyphs();
            try { font = Font(ren_, vfs_, path, role); }
            catch (const std::exception& e) { std::fprintf(stderr, "font load: %s\n", e.what()); }
        };
        load(hudFont_, "fonts/bodfontbody.gaf", FontRole::Text);
        load(bigFont_, "fonts/font48.gaf", FontRole::Art);
        load(statFont_, "fonts/b_times new roman (100b).gaf", FontRole::Text);
        if (!statFont_.ok()) load(statFont_, "fonts/ig_times new roman (100).gaf", FontRole::Text);
        load(scoreboardFont_, "fonts/ig_times new roman (100).gaf", FontRole::Text);
        scoreboardFont_.setLetterSpacing(0);
        // The embedded TrueType face (client/uifont.h) is not art; only its glyph sheets
        // go, and the next draw rebuilds them.
        ta::releaseUiTextGlyphs();
    }

    void GameView::reloadInterfaceArt(const ta::Settings& s) {
        loadInterfaceFonts();

        for (auto& [name, texture] : icons_) if (texture) gpuvram::destroy(texture);
        icons_.clear();   // build portraits reload smoothed (or not) on next use
        for (auto& [name, texture] : scoreboardLogos_) gpuvram::destroy(texture);
        scoreboardLogos_.clear();
        auto kill = [](SDL_Texture*& t) { gpuvram::destroy(t); t = nullptr; };
        kill(unitInfoBg_); kill(unitInfoIcon_);
        unitInfoIconFor_.clear();
        for (auto& texture : unitInfoOk_) kill(texture);
        unitInfoLabelFont_.destroyGlyphs(); unitInfoValueFont_.destroyGlyphs();
        if (cursorsInit_) cursors_.load(ren_, vfs_, &s);
        cursorMode_ = -1;
        hwCursorFailed_ = false;
    }

    ta::gaf::Palette GameView::guiPalette(const std::string& gaf) {
        std::string pp = "anims/" + gaf + ".pcx";
        try {
            return ta::gaf::Palette::fromBytes(vread(pp), pp);
        } catch (const std::exception&) {}
        try {
            return ta::gaf::Palette::fromBytes(vread("palettes/guipal.pal"),
                                                "palettes/guipal.pal");
        } catch (const std::exception&) {}
        return {};
    }

    SDL_Texture* GameView::loadGuiFrame(const std::string& gaf, const std::string& seq, int frame) {
        if (gaf.empty() || seq.empty()) return nullptr;
        std::string gp = "anims/" + gaf;
        if (gp.size() < 4 || gp.substr(gp.size() - 4) != ".gaf") gp += ".gaf";
        try {
            auto pal = guiPalette(gaf.size() >= 4 && gaf.substr(gaf.size() - 4) == ".gaf"
                                      ? gaf.substr(0, gaf.size() - 4)
                                      : gaf);
            for (auto& sq : ta::gaf::load(vread(gp), pal, -1, gp)) {
                if (sq.name != seq) continue;
                if (frame < 0 || size_t(frame) >= sq.frames.size()) frame = 0;
                if (sq.frames.empty()) return nullptr;
                auto& f = sq.frames[size_t(frame)];
                if (f.width == 0 || f.height == 0) return nullptr;
                SDL_Texture* t = ta::art::makeTexture(ren_, f.rgba, f.width, f.height);
                return t;
            }
        } catch (const std::exception&) {}
        return nullptr;
    }

    std::vector<std::string> GameView::conjureMenu(const std::string& builderType) const {
        const auto& all = registry_.buildable(builderType);
        std::vector<std::string> out;
        // The mission's whitelist binds the campaign player, not the AI a watcher has on show.
        const bool whitelisted = !missionAllowed_.empty() && hudPlayer() == localPlayer_;
        for (const auto& id : all)
            if (worldTables().buildAllowed(registry_.find(id)) &&
                (!whitelisted || std::find(missionAllowed_.begin(), missionAllowed_.end(), id) != missionAllowed_.end()))
                out.push_back(id);
        return out;
    }

    std::vector<ta::selportraits::Group> GameView::selTypeGroups() const {
        std::vector<std::pair<int, std::string>> live;
        live.reserve(selection_.size());
        for (int id : selection_)
            if (const UnitR* u = frameUnitP(id); u && u->alive() && u->type)
                live.push_back({id, u->type->id});
        return ta::selportraits::group(live);
    }

    ta::selportraits::Totals GameView::selTotals() const {
        std::vector<ta::selportraits::Vitals> v;
        v.reserve(selection_.size());
        for (int id : selection_)
            if (const UnitR* u = frameUnitP(id); u && u->alive() && u->type)
                v.push_back({u->hp, float(u->type->maxHp)});
        return ta::selportraits::total(v);
    }

    std::string GameView::selSquadLabel() const {
        if (selection_.size() < 2) return {};
        std::vector<std::pair<int, bool>> squads;
        for (int id : selection_)
            if (const UnitR* u = frameUnitP(id); u && u->alive() && u->type)
                squads.push_back({u->player == hudPlayer() ? int(u->squad) : 0, u->type->isBuilder});
        if (squads.empty() || squads.front().first == 0) return {};   // skip the roster scan
        std::vector<int> sel(selection_);
        std::sort(sel.begin(), sel.end());
        // The local squads' unselected units. Not a conjured site: it inherits its
        // builder's squad the moment it is laid, and is no member to command yet.
        std::vector<std::pair<int, bool>> rest;
        for (const UnitR* u : front().live)
            if (u->alive() && u->player == hudPlayer() && u->type && u->squad &&
                !u->underConstruction && !std::binary_search(sel.begin(), sel.end(), u->id))
                rest.push_back({int(u->squad), u->type->isBuilder});
        return ta::selportraits::squadLabel(ta::selportraits::selectionSquad(squads, rest));
    }

    float GameView::drawSelPortraits(SDL_FRect primary, float room, float gap, float px) {
        namespace sp = ta::selportraits;
        if (selection_.size() < 2) return 0;
        const auto groups = selTypeGroups();
        if (groups.empty()) return 0;
        const sp::Layout L = sp::layout(int(groups.size()), primary.w, primary.h, gap, room,
                                        kSelPortraitMinScale);
        const sp::Rect p0{primary.x, primary.y, primary.w, primary.h};
        auto rectOf = [&](int i) {
            sp::Rect r = sp::slot(L, i, p0, gap);
            return SDL_FRect{r.x, r.y, r.w, r.h};
        };
        auto hot = [&](const SDL_FRect& r) {
            return mouseX_ >= r.x && mouseX_ <= r.x + r.w && mouseY_ >= r.y && mouseY_ <= r.y + r.h;
        };
        // A count in the portrait's bottom-right corner on a dark plate (the build
        // menu's queued-count badge), sized down with a shrunk portrait.
        auto badge = [&](const SDL_FRect& r, const std::string& num, float bpx) {
            const float nw = blockWidth(num, bpx), pad = std::max(1.0f, bpx);
            SDL_FRect nb{r.x + r.w - nw - 2 * pad - 1, r.y + r.h - 7 * bpx - 2 * pad - 1,
                         nw + 2 * pad, 7 * bpx + 2 * pad};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 205);
            SDL_RenderFillRectF(ren_, &nb);
            blockText(num, nb.x + pad, nb.y + pad, bpx, {255, 235, 140, 255});
        };
        std::string tip;
        SDL_FRect tipAt{};
        for (int i = 0; i < L.shown; ++i) {
            const auto& g = groups[size_t(i)];
            const SDL_FRect r = rectOf(i);
            const auto* t = registry_.find(g.type);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);   // slot 0 too: one size for all
            SDL_RenderFillRectF(ren_, &r);
            SDL_Texture* ic = iconFor(g.type);
            if (!ic && t) ic = modelIconTex(g.type, playerColorSlot(hudPlayer()),
                                            t->maxVel > ta::sim::Fixed());
            if (ic) SDL_RenderCopyF(ren_, ic, nullptr, &r);
            const bool h = hot(r);
            SDL_SetRenderDrawColor(ren_, h ? 255 : 96, h ? 230 : 84, h ? 120 : 60, 255);
            SDL_RenderDrawRectF(ren_, &r);
            badge(r, std::to_string(g.ids.size()), std::max(1.0f, px * L.scale));
            if (h && t) { tip = t->name; tipAt = r; }
            selPortraitRects_.push_back({r, g.type});
        }
        if (L.overflow) {   // the types that did not fit: a "+K" tile, clicks swallowed
            const SDL_FRect r = rectOf(L.shown);
            const int hidden = int(groups.size()) - L.shown;
            SDL_SetRenderDrawColor(ren_, 14, 12, 10, 235);
            SDL_RenderFillRectF(ren_, &r);
            SDL_SetRenderDrawColor(ren_, 96, 84, 60, 255);
            SDL_RenderDrawRectF(ren_, &r);
            const std::string k = "+" + std::to_string(hidden);
            float kpx = std::max(1.0f, px * L.scale);
            kpx = std::min(kpx, (r.w - 4) / std::max(1.0f, blockWidth(k, 1)));
            blockText(k, r.x + (r.w - blockWidth(k, kpx)) * 0.5f, r.y + (r.h - 7 * kpx) * 0.5f,
                      kpx, {236, 226, 192, 255});
            if (hot(r)) { tip = std::to_string(hidden) + " MORE TYPES"; tipAt = r; }
            selPortraitRects_.push_back({r, std::string()});
        }
        if (!tip.empty()) {   // the hovered portrait's unit name, just above the bar
            const float tpx = std::max(1.0f, px), tw = blockWidth(tip, tpx), dh = ta::uiTextDescent(tpx);
            const float tx = std::clamp(tipAt.x + tipAt.w / 2 - tw / 2, 6.0f,
                                        std::max(6.0f, float(winW_) - tw - 6));
            const float ty = tipAt.y - 7 * tpx - dh - 14;   // above the strip
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 210);
            SDL_FRect tb{tx - 5, ty - 4, tw + 10, 7 * tpx + dh + 8};
            SDL_RenderFillRectF(ren_, &tb);
            blockText(tip, tx, ty, tpx, {255, 240, 190, 255});
        }
        return L.width;
    }

    bool GameView::selPortraitClick(float mx, float my, bool lmb) {
        if (selection_.size() < 2) return false;
        for (const auto& [r, type] : selPortraitRects_) {
            if (mx < r.x || mx > r.x + r.w || my < r.y || my > r.y + r.h) continue;
            if (!lmb || type.empty()) return true;   // other buttons / the "+K" tile: just swallow
            std::vector<int> ids;
            for (const auto& g : selTypeGroups())
                if (g.type == type) { ids = g.ids; break; }
            if (ids.empty()) return true;   // that type left the selection since the draw
            const bool remove = (SDL_GetModState() & KMOD_SHIFT) != 0;
            playClickTone();
            selection_ = ta::selportraits::click(selection_, ids, remove);
            if (!remove && !selection_.empty()) voice(selection_.front(), "select");
            return true;
        }
        return false;
    }

    std::string GameView::conjureTargetName(const UnitR* u) const {
        if (!u || !u->type) return {};
        if (u->buildSiteId != 0)
            if (const auto* site = frameUnitP(u->buildSiteId); site && site->type)
                return site->type->name;
        if (!u->buildQueue.empty() && u->buildQueue.front())
            return u->buildQueue.front()->name;
        return {};
    }

    void GameView::drawBar(float x, float y, float w, float h, float frac, SDL_Color c) {
        frac = std::clamp(frac, 0.0f, 1.0f);
        SDL_FRect r{x, y, w, h};
        SDL_SetRenderDrawColor(ren_, 8, 8, 8, 235);
        SDL_RenderFillRectF(ren_, &r);
        SDL_FRect f{x, y, w * frac, h};
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
        SDL_RenderFillRectF(ren_, &f);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 200);
        SDL_RenderDrawRectF(ren_, &r);
    }

    void GameView::colorSwatch(float x, float y, float s, int color, std::function<void(int)> action) {
        SDL_FRect r{x, y, s, s};
        SDL_Color c = playerColors_[color % 10];
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
        SDL_RenderFillRectF(ren_, &r);
        SDL_SetRenderDrawColor(ren_, lbHot(r) ? 255 : 30, lbHot(r) ? 255 : 30, 30, 255);
        SDL_RenderDrawRectF(ren_, &r);
        if (action) lobbyHots_.push_back({r, std::move(action)});
    }

    void GameView::drawUnitCounts(int winW) {
        int cnt[ta::sim::kMaxPlayers] = {};
        std::string sides[ta::sim::kMaxPlayers];
        const int np = frameNumPlayers();
        for (const UnitR* up : front().live) {
            if (!up->alive() || !up->type || up->player < 0 || up->player >= np) continue;
            ++cnt[up->player];
            if (sides[up->player].empty()) sides[up->player] = up->type->side;
        }
        std::vector<int> rows;
        bool showTeams = false;
        for (int p = 0; p < np; ++p) {
            if (framePlayer(p).built == 0 && cnt[p] == 0) continue;
            for (int q : rows)
                if (framePlayer(p).team == framePlayer(q).team) showTeams = true;
            rows.push_back(p);
        }
        if (showTeams) std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
            return framePlayer(a).team < framePlayer(b).team;
        });
        // Retail's compact F4 table: white serif text, faction/colour emblems,
        // translucent player rows, and Name / Kills / Losses / Score columns.
        // Independent saved scale; shrink only when the complete table cannot fit in the
        // map view (never under the minimap strip). It sits in the top-left column, under
        // the stream banner and the GAME TIME clock.
        const float requestedScale = (settings_ ? settings_->scorecardScale : 1.0f) * kHudBase;
        const auto board = ta::hudcorner::scoreboard(requestedScale, showTeams, np, float(mapViewW(winW)),
                                                      float(mapViewH(winH_)), topLeft_.next());
        const float scale = board.scale, top = board.top;
        topLeft_.place(top, 32 * scale * float(rows.size() + 1));
        const float x = 12 * scale, rowH = 32 * scale;
        const float nameW = 190 * scale, numberW = 90 * scale;
        const float teamW = showTeams ? 64 * scale : 0;
        const float numbersX = x + nameW + teamW;
        const float width = nameW + teamW + 3 * numberW;
        float fontScale = 1, fontTop = 0, fontH = 7;
        if (scoreboardFont_.ok()) scoreboardFont_.vbounds("Ag0123456789", 1, fontTop, fontH);
        fontScale = 20 * scale / std::max(1.0f, fontH);
        auto textWidth = [&](const std::string& text, float s) {
            return scoreboardFont_.ok() ? float(scoreboardFont_.width(text, s)) : blockWidth(text, s);
        };
        auto text = [&](const std::string& value, float tx, float y, float available,
                        bool center, SDL_Color color) {
            float s = fontScale;
            float tw = textWidth(value, s);
            if (tw > available) { s *= available / tw; tw = textWidth(value, s); }
            if (center) tx += (available - tw) * 0.5f;
            const float baseline = y + (rowH - fontH * s) * 0.5f - fontTop * s;
            if (scoreboardFont_.ok()) {
                scoreboardFont_.draw(ren_, value, tx + scale, baseline + scale, s, {0,0,0,220});
                scoreboardFont_.draw(ren_, value, tx, baseline, s, color);
            } else blockText(value, tx, baseline, s, color);
        };
        const SDL_Color white{245,245,245,255};
        text("Name", x, top, nameW, false, white);
        if (showTeams) text("Team", x + nameW, top, teamW, true, white);
        text("Kills", numbersX, top, numberW, true, white);
        text("Losses", numbersX + numberW, top, numberW, true, white);
        text("Score", numbersX + 2 * numberW, top, numberW, true, white);
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        float y = top + rowH;
        for (int t : rows) {
            const auto& player = framePlayer(t);
            SDL_FRect row{x, y, width, rowH};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 110);
            SDL_RenderFillRectF(ren_, &row);
            SDL_SetRenderDrawColor(ren_, 157, 145, 100, 155);
            SDL_RenderDrawLineF(ren_, x, y, x + width, y);
            SDL_RenderDrawLineF(ren_, x, y + rowH, x + width, y + rowH);
            std::string name = playerName_[t % ta::sim::kMaxPlayers];
            if (name.empty()) name = "P" + std::to_string(t + 1) + " " + sides[t];
            if (playerAi_[t % ta::sim::kMaxPlayers] && name.rfind("AI ", 0) != 0) name = "AI " + name;
            if (player.defeated) name += " (OUT)";
            const SDL_Color color = player.defeated ? SDL_Color{160,160,160,255} : white;
            std::string side = sides[t];
            // Keep the emblem after defeat, when no living unit supplies a side.
            if (!side.empty()) scoreboardSides_[t % ta::sim::kMaxPlayers] = side;
            else side = scoreboardSides_[t % ta::sim::kMaxPlayers];
            if (!player.side.empty()) side = player.side;
            const std::string seq = ta::factionLogoSequence(side);
            const std::string key = seq + std::to_string(colorSlot_[t % ta::sim::kMaxPlayers]);
            auto [it, inserted] = scoreboardLogos_.try_emplace(key, nullptr);
            if (inserted && !seq.empty())
                it->second = loadGuiFrame(ta::kFactionLogoGaf, seq, colorSlot_[t % ta::sim::kMaxPlayers]);
            SDL_FRect emblem{x + 2 * scale, y + 2 * scale, 28 * scale, 28 * scale};
            if (it->second) SDL_RenderCopyF(ren_, it->second, nullptr, &emblem);
            else {
                const auto c = playerColor(t);
                SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
                SDL_RenderFillRectF(ren_, &emblem);
            }
            text(name, x + 36 * scale, y, nameW - 42 * scale, false, color);
            if (showTeams) text(std::to_string(player.team + 1), x + nameW, y, teamW, true, color);
            text(std::to_string(player.kills), numbersX, y, numberW, true, color);
            text(std::to_string(player.losses), numbersX + numberW, y, numberW, true, color);
            text(std::to_string(player.score), numbersX + 2 * numberW, y, numberW, true, color);
            y += rowH;
        }
    }

    void GameView::drawObjectivesPanel(int winW, int /*winH*/) {
        if (missionObjectives_.empty()) return;
        const float x0 = 12;
        // Integer-sized glyphs have predictable bounds: the decorative book font
        // uses baseline offsets that made the old 21px rows overlap each other.
        const float s = 2.0f, lh = 22.0f;
        const float maxW = std::min(480.0f, winW * 0.40f);
        std::vector<std::pair<std::string, bool>> lines;
        if (showObjectives_) for (const std::string& obj : missionObjectives_) {
            std::string line, word;
            bool first = true;
            auto flush = [&] {
                if (line.empty()) return;
                lines.push_back({line, first});
                first = false;
                line.clear();
            };
            auto push = [&] {
                if (word.empty()) return;
                if (!line.empty() && blockWidth(line + " " + word, s) > maxW) flush();
                if (!line.empty()) line += ' ';
                for (char c : word) {
                    if (blockWidth(line + c, s) > maxW) flush();
                    line += c;
                }
                word.clear();
            };
            for (char c : obj) {
                if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
                    push();
                    if (c == '\n') flush();
                } else word += c;
            }
            push();
            flush();
        }
        const float panelW = showObjectives_ ? maxW + 34 : blockWidth("[O] OBJECTIVES", s) + 20;
        const float panelH = showObjectives_ ? 52 + float(lines.size()) * lh : 30;
        const float top = topLeft_.place(86, panelH) + 6;   // the box starts 6 above the text
        SDL_FRect bg{x0 - 6, top - 6, panelW, panelH};
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren_, 14, 13, 18, 235);
        SDL_RenderFillRectF(ren_, &bg);
        SDL_SetRenderDrawColor(ren_, 96, 84, 60, 255);
        SDL_RenderDrawRectF(ren_, &bg);
        blockText(showObjectives_ ? "OBJECTIVES" : "[O] OBJECTIVES",
                  x0, top, s, {240, 214, 148, 255});
        if (!showObjectives_) return;
        float y = top + 26;
        for (const auto& [text, isFirst] : lines) {
            if (isFirst) {
                SDL_FRect dot{x0 + 2, y + 5, 4, 4};
                SDL_SetRenderDrawColor(ren_, 240, 214, 148, 255);
                SDL_RenderFillRectF(ren_, &dot);
            }
            blockText(text, x0 + 14, y, s, {240, 242, 248, 255});
            y += lh;
        }
        blockText("[O] HIDE", x0, y + 4, s, {190, 198, 212, 255});
    }

    // The build row: the selected builder's menu as a row of pictures along the
    // bottom of the world view, as the TAK engine draws its conjure menu -- or several
    // rows, stacked upward, when a long menu does not fit (client/buildrow.h, the one
    // place the row's geometry is computed). Where the rows sit along the bottom is a
    // user preference (Options BUILD MENU: left / centered / right), their size BUILD
    // MENU SCALE on top of UI SCALE. The menu is the builder's TA build pages in page
    // and slot order (TypeRegistry::loadBuildTree), and a stockpiling unit's MAKENUKE /
    // MAKEANTI button is its one entry. Each picture shows how many are queued, +++
    // for repeated production, and the hovered one's name, description and cost.
    void GameView::drawPanel(int winW, int winH) {
        selPortraitRects_.clear();
        iconRects_.clear();
        buildMenuRect_ = {};
        buildRowTop_ = 0;
        stockIconRect_ = {};
        stockIconName_.clear();
        const UnitR* b = selectedBuilder();
        if (!b || b->player != hudPlayer()) return;
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        // Mission-filtered; an entry naming no known type takes no slot.
        std::vector<const ta::sim::UnitType*> menu;
        for (const auto& id : conjureMenu(b->type->id))
            if (const auto* bt = registry_.find(id)) menu.push_back(bt);
        const std::string& stock = stockButtonOf(b->type->id);
        const int n = int(menu.size()) + (stock.empty() ? 0 : 1);
        if (n <= 0) return;
        const float areaW = float(mapViewW(winW));
        // Square cells: TA's unitpics are 64x64.
        const auto lay = ta::buildrow::layout(n, float(winH), buildRowBaseH(), buildBarScale_,
                                              areaW, areaW, buildBarAlign_, 1.0f);
        // Everything drawn INSIDE an icon (the +++ badge, the queue count, the
        // tooltip) is sized from the scale in effect, so a shrunken icon does not
        // get full-size furniture spilling out of it.
        const float bScale = lay.scale * uiScale_;
        const float iconTop = lay.top;   // the top row: the tooltip and the BUILDING line sit over it
        buildMenuRect_ = {lay.box.x, lay.box.y, lay.box.w, lay.box.h};
        buildRowTop_ = lay.bandTop;
        {   // a recessed container behind the rows so the menu reads as one HUD strip
            const SDL_FRect& box = buildMenuRect_;
            SDL_SetRenderDrawColor(ren_, 14, 12, 10, 200);
            SDL_RenderFillRectF(ren_, &box);
            SDL_SetRenderDrawColor(ren_, 96, 84, 60, 255);
            SDL_RenderDrawRectF(ren_, &box);
        }
        auto fitPicture = [&](SDL_Texture* ic, const SDL_FRect& r) {
            int iw = 0, ih = 0;
            if (!ic || SDL_QueryTexture(ic, nullptr, nullptr, &iw, &ih) != 0 || iw <= 0 || ih <= 0) return false;
            // Retail portraits are 64x64 and generated thumbnails square: fit without distortion.
            const float k = std::min(r.w / float(iw), r.h / float(ih));
            const SDL_FRect d{r.x + (r.w - float(iw) * k) / 2, r.y + (r.h - float(ih) * k) / 2, float(iw) * k, float(ih) * k};
            SDL_RenderCopyF(ren_, ic, nullptr, &d);
            return true;
        };
        auto badge = [&](const std::string& t, const SDL_FRect& r, SDL_Color c) {   // bottom-right
            const float px = 2.2f * bScale, w = blockWidth(t, px);
            const SDL_FRect nb{r.x + r.w - w - 7 * bScale, r.y + r.h - 21 * bScale, w + 7 * bScale, 20 * bScale};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 205);
            SDL_RenderFillRectF(ren_, &nb);
            blockText(t, r.x + r.w - w - 4 * bScale, r.y + r.h - 18 * bScale, px, c);
        };
        std::string tip;
        float tipCentre = 0;
        for (int i = 0; i < n; ++i) {
            const auto& li = lay.icons[size_t(i)];
            const SDL_FRect r{li.x, li.y, li.w, li.h};
            const bool isStock = i == int(menu.size());
            const ta::sim::UnitType* bt = isStock ? nullptr : menu[size_t(i)];
            const bool allowed = isStock || worldTables().buildAllowed(bt);
            SDL_SetRenderDrawColor(ren_, 20, 18, 14, 235);
            const SDL_FRect rb{r.x - 1, r.y - 1, r.w + 2, r.h + 2};
            SDL_RenderFillRectF(ren_, &rb);
            SDL_Texture* ic = iconFor(isStock ? b->type->id : bt->id);
            if (!ic && bt) ic = modelIconTex(bt->id, playerColorSlot(hudPlayer()), !isStructure(bt));
            if (!fitPicture(ic, r)) {
                SDL_SetRenderDrawColor(ren_, 60, 55, 50, 255);
                SDL_RenderFillRectF(ren_, &r);
            }
            if (!allowed) {   // a unit the game's rules forbid: dimmed, as the build pages grey it
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 150);
                SDL_RenderFillRectF(ren_, &r);
            }
            const bool hot = mouseX_ >= r.x && mouseX_ <= r.x + r.w && mouseY_ >= r.y && mouseY_ <= r.y + r.h;
            SDL_SetRenderDrawColor(ren_, hot ? 255 : 110, hot ? 230 : 100, hot ? 120 : 70, 255);
            SDL_RenderDrawRectF(ren_, &r);
            if (isStock) {
                // The stockpile: the stock on hand, then " +N" still queued (0x4199b0, 0x439d80).
                const char* what = stock.size() >= 8 && stock.compare(stock.size() - 8, 8, "MAKEANTI") == 0 ? "ANTI" : "NUKE";
                const float px = 2.0f * bScale, w = blockWidth(what, px);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 180);
                const SDL_FRect pb{r.x + (r.w - w) / 2 - 3, r.y + 4 * bScale, w + 6, 18 * bScale};
                SDL_RenderFillRectF(ren_, &pb);
                blockText(what, r.x + (r.w - w) / 2, r.y + 6 * bScale, px, {255, 200, 120, 255});
                std::string caption;
                if (b->stock[0]) caption = std::to_string(int(b->stock[0]));
                if (b->stockQueued) caption += (caption.empty() ? "+" : " +") + std::to_string(b->stockQueued);
                if (!caption.empty()) badge(caption, r, {255, 235, 140, 255});
                stockIconRect_ = r;
                stockIconName_ = stock;
                if (hot) {
                    tip = std::string(what[0] == 'A' ? "BUILD ANTI-MISSILE" : "BUILD NUCLEAR MISSILE") +
                          "  LEFT +1  RIGHT -1  SHIFT 5";
                    tipCentre = r.x + r.w / 2;
                }
                continue;
            }
            // Repeated production: a bright +++ over the repeating unit's picture.
            if (b->repeatType == bt) {
                const float px = 2.8f * bScale, pw = blockWidth("+++", px);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 180);
                const SDL_FRect pb{r.x + (r.w - pw) / 2 - 3, r.y + 4 * bScale, pw + 6, 22 * bScale};
                SDL_RenderFillRectF(ren_, &pb);
                blockText("+++", r.x + (r.w - pw) / 2, r.y + 7 * bScale, px, {120, 255, 130, 255});
            }
            // How many are queued, bottom-right.
            if (int qc = frameQueuedCount(b->id, bt)) badge(std::to_string(qc), r, {255, 235, 140, 255});
            if (hot) {
                tip = bt->name;
                if (!bt->description.empty() && bt->description != bt->name) tip += " - " + bt->description;
                char cost[64];
                std::snprintf(cost, sizeof cost, "   %d METAL  %d ENERGY", int(bt->buildCostMetal), int(bt->buildCostEnergy));
                tip += cost;
                tipCentre = r.x + r.w / 2;
            }
            iconRects_.push_back({r, bt});
        }
        if (!tip.empty()) {
            const float px = 2.0f * bScale, tw = blockWidth(tip, px);
            const float tipx = std::max(6.0f, std::min(tipCentre - tw / 2, areaW - tw - 6));
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 210);
            const SDL_FRect tb{tipx - 6, iconTop - 28 * bScale, tw + 12, 26 * bScale};
            SDL_RenderFillRectF(ren_, &tb);
            blockText(tip, tipx, iconTop - 24 * bScale, px, {255, 240, 190, 255});
        } else if (!b->buildQueue.empty()) {
            // What it is building now: +++ for repeated production (its queue only
            // ever holds the one it re-queues), else how many are queued.
            char q[96];
            const auto* front = b->buildQueue.front();
            if (b->repeatType && front == b->repeatType && b->buildQueue.size() == 1)
                std::snprintf(q, sizeof q, "BUILDING %s (+++)", front->name.c_str());
            else
                std::snprintf(q, sizeof q, "BUILDING %s (%zu)", front->name.c_str(), b->buildQueue.size());
            const float px = 1.8f * bScale;
            const float qx = std::max(ta::buildrow::kEdge,
                                      std::min(lay.textX, areaW - ta::buildrow::kEdge - blockWidth(q, px)));
            blockText(q, qx, iconTop - 24 * bScale, px, {160, 210, 255, 255});
        }
    }

// A builder of yours with nothing to do: a mobile builder with no work order, site,
// production, reclaim or repair (a factory output's PARK and a flyer's landing are not
// work), or a factory with an empty queue and no repeat production.
bool GameView::idleBuilder(const UnitR& u) const {
    if (!u.alive() || !u.type || u.underConstruction || u.embarked()) return false;
    if (u.type->isStructure()) {
        return u.type->producesUnits() && u.buildQueue.empty() && !u.repeatType;
    }
    if (!u.type->isBuilder || u.repeatType) return false;
    if (u.buildSiteId || u.productionSiteId || u.reclaimId || u.repairId) return false;
    for (const auto& o : u.orders) {
        if (o.park || o.landing) continue;
        if (o.goal || o.buildType || o.targetId || o.reclaimFeat || o.reclaimArea || o.repairTarget ||
            o.patrol || o.guard || o.load || o.unload || o.attackMove) return false;
    }
    return true;
}

// IDLE BUILDERS (Options > Interface): a column at the right edge of the play area,
// one portrait per type of idle builder or factory of yours (a count when there are
// several), sized like the build menu's icons (bottom bar height; square, as TA's
// unitpics are 64 x 64, each framed tight round its picture) times its own
// IDLE BUILDERS SCALE, and shrunk to fit the column's height. Top, middle or bottom of the
// play area; at the bottom it stays above a build menu that reaches under it.
void GameView::drawIdleBuilders(int winW, int winH, float barTop) {
    idleRects_.clear();
    if (!idleBuilders_ || watchOnly() || !hudShown()) return;
    const int me = localPlayer_;
    for (const UnitR* up : front().live) {
        const UnitR& u = *up;
        if (u.player != me || !idleBuilder(u)) continue;
        auto it = std::find_if(idleRects_.begin(), idleRects_.end(),
                               [&](const IdleBuilderIcon& i) { return i.type == u.type; });
        if (it == idleRects_.end()) idleRects_.push_back({{}, u.type, {u.id}});
        else it->ids.push_back(u.id);
    }
    if (idleRects_.empty()) return;
    // The Commander, then the mobile builders, then the factories; within each, basic to
    // advanced, which in TA:K's data is cheapest to dearest (no tech level is
    // authored), and by name on a tie.
    const auto rank = [](const ta::sim::UnitType* t) { return t->commander ? 0 : t->isStructure() ? 2 : 1; };
    std::stable_sort(idleRects_.begin(), idleRects_.end(), [&](const IdleBuilderIcon& a, const IdleBuilderIcon& b) {
        if (rank(a.type) != rank(b.type)) return rank(a.type) < rank(b.type);
        if (a.type->buildCostMetal != b.type->buildCostMetal) return a.type->buildCostMetal < b.type->buildCostMetal;
        return a.type->name < b.type->name;
    });
    const float edge = ta::buildrow::kEdge;
    const float unitH = kBarHBase * uiScale_ - 10.0f, unitW = unitH, unitGap = 6.0f;
    // The column's room: the play area's height, above any build menu whose box
    // reaches under the column.
    // In the world view's coordinates (drawn through its viewport): winW is its width.
    const float right = float(winW) - edge;
    float bottom = barTop - edge;
    const float wantW = unitW * idleBuildersScale_;
    if (buildMenuRect_.w > 0 && buildMenuRect_.x + buildMenuRect_.w > right - wantW)
        bottom = std::min(bottom, buildRowTop_ - edge);
    // Under the top notices, and under the stats column that shares the corner.
    const float top = std::max(edge + 30.0f, statsBottom_ + edge);
    const float room = std::max(bottom - top, unitH * 0.4f);
    const int n = int(idleRects_.size());
    const float scale = std::min(idleBuildersScale_, room / (n * unitH + (n - 1) * unitGap));
    const float iw = unitW * scale, ih = unitH * scale, gap = unitGap * scale;
    const float colH = n * ih + (n - 1) * gap;
    const float y0 = idleBuilders_ == 1 ? top : idleBuilders_ == 3 ? bottom - colH : top + (room - colH) / 2;
    const float x = right - iw;
    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
    SDL_FRect box{x - 4, y0 - 4, iw + 8, colH + 8};
    SDL_SetRenderDrawColor(ren_, 14, 12, 10, 200);
    SDL_RenderFillRectF(ren_, &box);
    SDL_SetRenderDrawColor(ren_, 96, 84, 60, 255);
    SDL_RenderDrawRectF(ren_, &box);
    (void)winH;
    for (int i = 0; i < n; ++i) {
        auto& icon = idleRects_[size_t(i)];
        SDL_FRect r{x, y0 + float(i) * (ih + gap), iw, ih};   // narrowed to the picture below
        icon.r = r;
        SDL_Texture* tex = iconFor(icon.type->id);
        if (!tex) tex = modelIconTex(icon.type->id, playerColorSlot(me), !isStructure(icon.type));
        if (tex) {
            int tw = 0, th = 0;
            if (SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th) == 0 && tw > 0 && th > 0) {
                const float fit = std::min(r.w / float(tw), r.h / float(th));
                SDL_FRect dst{r.x + (r.w - float(tw) * fit) / 2, r.y + (r.h - float(th) * fit) / 2,
                              float(tw) * fit, float(th) * fit};
                SDL_RenderCopyF(ren_, tex, nullptr, &dst);
                r = dst;   // the frame, the hover and the count hug the picture
                icon.r = r;
            }
        } else {
            SDL_SetRenderDrawColor(ren_, 60, 55, 50, 255);
            SDL_RenderFillRectF(ren_, &r);
        }
        const bool hot = mouseX_ >= r.x && mouseX_ <= r.x + r.w && mouseY_ >= r.y && mouseY_ <= r.y + r.h;
        SDL_SetRenderDrawColor(ren_, hot ? 255 : 110, hot ? 230 : 100, hot ? 120 : 70, 255);
        SDL_RenderDrawRectF(ren_, &r);
        if (icon.ids.size() > 1) {   // how many of this type are idle, bottom right
            const std::string num = std::to_string(icon.ids.size());
            const float px = 2.2f * scale, nw = blockWidth(num, px);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 205);
            SDL_FRect nb{r.x + r.w - nw - 7 * scale, r.y + r.h - 21 * scale, nw + 7 * scale, 20 * scale};
            SDL_RenderFillRectF(ren_, &nb);
            blockText(num, r.x + r.w - nw - 4 * scale, r.y + r.h - 18 * scale, px, {255, 235, 140, 255});
        }
        if (hot) {   // the name, to the left of the column
            // Readable whatever the column's scale: at least the build menu's text size.
            const std::string tip = icon.type->name;
            const float px = std::max(2.6f, 3.2f * scale), tw = blockWidth(tip, px), th = 7.0f * px;
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 215);
            SDL_FRect tb{r.x - tw - 26, r.y + r.h / 2 - th / 2 - 7, tw + 16, th + 14};
            SDL_RenderFillRectF(ren_, &tb);
            blockText(tip, r.x - tw - 18, r.y + r.h / 2 - th / 2, px, {255, 240, 190, 255});
        }
    }
}

// A click on an IDLE BUILDERS portrait: the next idle unit of that type is selected and
// the camera goes to it (each click the next); Shift selects every one of them.
bool GameView::idleBuilderClick(float mx, float my, bool lmb) {
    for (const auto& icon : idleRects_) {
        const SDL_FRect& r = icon.r;
        if (mx < r.x || mx > r.x + r.w || my < r.y || my > r.y + r.h) continue;
        if (!lmb || icon.ids.empty()) return true;   // a right press on it is swallowed
        playClickTone();
        if (SDL_GetModState() & KMOD_SHIFT) {
            selection_ = icon.ids;
            centerOnSelection();
        } else {
            int& last = idleCycle_[icon.type];
            auto it = std::find(icon.ids.begin(), icon.ids.end(), last);
            const int next = (it == icon.ids.end() || it + 1 == icon.ids.end()) ? icon.ids.front() : *(it + 1);
            last = next;
            selection_ = {next};
            centerOn(next);
            voice(next, "select");
        }
        return true;
    }
    return false;
}

void GameView::drawGiveUnitsMenu(int winW, int winH) {
    giveUnitsHots_.clear();
    std::vector<int> players;
    for (int p=0;p<frameNumPlayers();++p)
        if (!resultParticipants_ || (*resultParticipants_ & (1u<<p))) players.push_back(p);
    const auto eligible=eligibleGiftRecipients();
    const auto shareMask=requestedSharing_.value_or(framePlayer(localPlayer_).shareResources);
    // The players, then SPECTATORS; up to ten players (sim::kMaxPlayers) shrink the panel
    // to the window's height rather than run off it.
    const float rows=172+44*float(players.size()+1);
    const float scale=std::min({1.0f,float(winW)/1080.0f,float(winH)/std::max(550.0f,rows+20)});
    const float width=1040*scale,rowH=38*scale;
    const float height=rows*scale;
    const float x=(winW-width)/2,y=(winH-height)/2;
    SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren_,0,0,0,160);
    SDL_FRect dim{0,0,float(winW),float(winH)};SDL_RenderFillRectF(ren_,&dim);
    SDL_FRect panel{x,y,width,height};
    SDL_SetRenderDrawColor(ren_,26,28,36,245);SDL_RenderFillRectF(ren_,&panel);
    SDL_SetRenderDrawColor(ren_,120,130,160,255);SDL_RenderDrawRectF(ren_,&panel);
    blockText("DIPLOMACY",x+24*scale,y+22*scale,2.5f*scale,{235,225,180,255});
    blockText("PLAYER",x+24*scale,y+66*scale,1.6f*scale,{190,190,200,255});
    // TA's auto-share switches (ShareMetal / ShareEnergy, TotalA.exe 0x418d04 /
    // 0x418dc4) are the local player's own, so they sit on your row.
    blockText("SHARE M  E",x+252*scale,y+66*scale,1.6f*scale,{190,190,200,255});
    blockText("CHAT",x+394*scale,y+66*scale,1.6f*scale,{190,190,200,255});
    blockText("VOICE",x+456*scale,y+66*scale,1.6f*scale,{190,190,200,255});
    blockText("VOLUME",x+524*scale,y+66*scale,1.6f*scale,{190,190,200,255});
    auto button=[&](const std::string& label,SDL_FRect r,int p,DiplomacyAction action,bool enabled) {
        const bool hover=enabled && screenMouseX_>=r.x && screenMouseX_<r.x+r.w && screenMouseY_>=r.y && screenMouseY_<r.y+r.h;
        SDL_SetRenderDrawColor(ren_,enabled?(hover?90:60):37,enabled?(hover?110:66):39,enabled?(hover?150:86):46,255);
        SDL_RenderFillRectF(ren_,&r);
        blockText(label,r.x+(r.w-blockWidth(label,1.7f*scale))/2,
            r.y+(r.h-7*1.7f*scale)/2,1.7f*scale,enabled?SDL_Color{235,235,240,255}:SDL_Color{105,108,118,255});
        if(enabled)giveUnitsHots_.push_back({r,p,action});
    };
    auto checkbox=[&](float cx,float by,int p,DiplomacyAction action,bool checked,bool enabled) {
        SDL_FRect r{x+cx*scale,by+8*scale,22*scale,22*scale};
        SDL_SetRenderDrawColor(ren_,enabled?165:65,enabled?175:68,enabled?190:75,255);
        SDL_RenderDrawRectF(ren_,&r);
        if(checked) {
            SDL_SetRenderDrawColor(ren_,enabled?225:85,enabled?220:88,enabled?160:95,255);
            for(int i=0;i<2;++i) {
                SDL_RenderDrawLineF(ren_,r.x+4*scale,r.y+(11+i)*scale,r.x+9*scale,r.y+(16+i)*scale);
                SDL_RenderDrawLineF(ren_,r.x+9*scale,r.y+(16+i)*scale,r.x+18*scale,r.y+(5+i)*scale);
            }
        }
        if(enabled)giveUnitsHots_.push_back({r,p,action});
    };
    // VOLUME: how loud that player's voice plays here, drawn as the VOICE CHAT page's
    // sliders are (track, fill, handle, value) and greyed like that page while voice chat
    // is off. A greyed slider shows its value but takes no input.
    const bool voiceOn=voice_ && voice_->enabled();
    auto volumeSlider=[&](float by,int p) {
        const float tx=x+524*scale,tw=140*scale,th=7*scale,ty=by+(rowH-th)/2;
        const float frac=float(voiceVolume_[size_t(p)])/100.0f;
        SDL_FRect track{tx,ty,tw,th};
        SDL_SetRenderDrawColor(ren_,48,52,66,255);SDL_RenderFillRectF(ren_,&track);
        SDL_FRect fill{tx,ty,tw*frac,th};
        if(voiceOn)SDL_SetRenderDrawColor(ren_,80,140,225,255);
        else SDL_SetRenderDrawColor(ren_,70,76,92,255);
        SDL_RenderFillRectF(ren_,&fill);
        SDL_FRect handle{tx+tw*frac-4*scale,ty-4*scale,8*scale,th+8*scale};
        if(voiceOn)SDL_SetRenderDrawColor(ren_,205,215,240,255);
        else SDL_SetRenderDrawColor(ren_,95,100,115,255);
        SDL_RenderFillRectF(ren_,&handle);
        const std::string pct=std::to_string(int(voiceVolume_[size_t(p)]))+"%";
        blockText(pct,tx+tw+14*scale,by+(rowH-7*1.6f*scale)/2,1.6f*scale,
            voiceOn?SDL_Color{150,195,235,255}:SDL_Color{90,100,115,255});
        // The hot area is the row's height over the track: a value is where along it.
        if(voiceOn)giveUnitsHots_.push_back({{tx,by,tw,rowH},p,DiplomacyAction::Volume});
    };
    float by=y+92*scale;
    for(int p:players) {
        std::string name=playerName_[p];
        if(name.empty())name="PLAYER "+std::to_string(p+1);
        if(p==localPlayer_)name+=" (YOU)";
        const float ts=std::min(1.9f*scale,210*scale/std::max(1.0f,blockWidth(name,1)));
        blockText(name,x+24*scale,by+(rowH-7*ts)/2,ts,{235,235,240,255});
        const bool ally=p!=localPlayer_ && framePlayer(p).team==framePlayer(localPlayer_).team;
        if(p==localPlayer_) {
            const bool alive=!framePlayer(localPlayer_).defeated;
            checkbox(270,by,1,DiplomacyAction::ShareMetal,shareMask & 1u,alive);
            checkbox(322,by,0,DiplomacyAction::ShareEnergy,shareMask & 2u,alive);
        }
        checkbox(402,by,p,DiplomacyAction::Chat,chatRecipients_ & (1u<<p),true);
        // VOICE: chat's column again, for voice chat. Your own voice never comes back to
        // you, so your own box is shown as it stands but cannot be changed.
        checkbox(468,by,p,DiplomacyAction::Voice,voiceRecipients_ & (1u<<p),p!=localPlayer_);
        if(voiceVolumeSlider(p))volumeSlider(by,p);
        if(ally)button("GIVE SELECTED UNITS",{x+738*scale,by,278*scale,rowH},p,DiplomacyAction::Give,eligible[size_t(p)]);
        by+=44*scale;
    }
    // SPECTATORS: whether anyone watching reads your chat and hears your voice. Both start
    // off each game; ticking them in reaches every spectator.
    {
        const float ts=1.9f*scale;
        blockText("SPECTATORS",x+24*scale,by+(rowH-7*ts)/2,ts,{190,196,212,255});
        checkbox(402,by,kDiplomacySpectators,DiplomacyAction::Chat,chatSpectators_,true);
        checkbox(468,by,kDiplomacySpectators,DiplomacyAction::Voice,voiceSpectators_,true);
    }
    button("CLOSE",{x+width-164*scale,y+height-56*scale,140*scale,rowH},-1,DiplomacyAction::Close,true);
}

    const GameView::PickBounds& GameView::pickBounds() {
        const auto& types = registry_.types();
        const auto& m = mapView_.map();
        const PickBoundsKey key{&types, types.size(), visuals_.size(), hitBoxes_.size(), &m, m.heights.data(),
                                m.heights.size(), int(m.seaLevel), kHeightScale_};
        if (pickBoundsValid_ && key == pickBoundsKey_) return pickBounds_;
        pickBoundsKey_ = key;
        pickBoundsValid_ = true;
        PickBounds b;
        // The deepest a wading or floating unit can sink (waterSink): rawHeight never
        // falls below the lowest heightmap corner (+1 for the bilinear sample's rounding).
        int low = 255;
        for (uint8_t v : m.heights) low = std::min(low, int(v));
        const float depth = m.heights.empty() || m.width <= 0 ? 0.0f
                            : std::max(0.0f, float(m.seaLevel) - float(low) + 1.0f);
        float sink = 0;
        for (const auto& [id, type] : types) {
            // The box the exact test would use for this type now: the cached one, else
            // the model's (computing it here caches exactly what the test would compute),
            // else the fallback box -- not cached, since the model may load later (that
            // changes visuals_.size(), part of the key, and the bound is rebuilt).
            const bool known = hitBoxes_.count(type.id) || visuals_.count(type.id);
            const SDL_FRect hb = known ? unitHitBox(&type) : kFallbackHitBox;
            b.boxes.add(hb.x, hb.y, hb.w, hb.h);
            b.maxCells = std::max({b.maxCells, type.footX, type.footZ});
            if (type.waterline > 0 && (type.canHover || type.floater))
                sink = std::max(sink, std::min(float(type.waterline), depth));
            if (type.canFly) b.cruiseAlt = std::max(b.cruiseAlt, float(type.cruiseAlt));
        }
        const float s = sink * kHeightScale_;
        b.sinkLo = std::min(0.0f, s);
        b.sinkHi = std::max(0.0f, s);
        pickBounds_ = b;
        return pickBounds_;
    }

    std::span<const uint32_t> GameView::pickCandidates(float mx, float my) {
        const Frame& f = front();
        const size_t n = f.live.size();
        const auto everything = [&] {
            for (size_t i = pickAll_.size(); i < n; ++i) pickAll_.push_back(uint32_t(i));
            return std::span<const uint32_t>(pickAll_.data(), n);
        };
        static const bool fullScan = ta::devFlag("TA_PICK_FULL_SCAN");
        // Every typed record registered, so its type's model is loaded (see pickRegistered_).
        const bool registered = pickRegistered_.complete && f.gen >= pickRegistered_.gen &&
                                f.unitSlots.size() <= pickRegistered_.idBound;
        std::span<const uint32_t> out;
        if (fullScan || pickForceFull_ || unitFullScan_ || f.unitGrid.size() != n || !registered) {
            out = everything();
        } else {
            const PickBounds& b = pickBounds();
            const float zm = mapView_.zoom();
            const float dotHalf = std::max(ta::tacticaldots::dotSide(b.maxCells, b.maxCells, zm, uiScale_) * 0.5f + 1.0f,
                                           ta::render::kPickMinHalf);
            // interpPose stays within maxInterpStep of the snapshot position (+1: rounding).
            const auto band = ta::render::pickBand(mx, my, zm, b.boxes, dotHalf, f.maxInterpStep + 1.0f, b.sinkLo, b.sinkHi);
            const auto rect = ta::render::anchorBandToWorld(band.left, band.top, band.right, band.bottom,
                                                             mapView_.offX(), mapView_.offY(), zm, unitLiftBounds());
            if (f.unitGrid.covers(rect.left, rect.top, rect.right, rect.bottom)) {
                out = everything();
            } else {
                f.unitGrid.query(rect.left, rect.top, rect.right, rect.bottom, pickQueryBits_, pickQuery_);
                if (f.flyerUnits.empty()) {
                    out = pickQuery_;
                } else {
                    // Flyers: altLift raises the anchor by up to max(altitude) * kProjY (a
                    // negative sink); their own query's units that fly join the ground
                    // candidates. All lists ascending: merges, each index once.
                    const float altitude = std::max(flyerAltitudeSeen_, b.cruiseAlt) * kProjY + 1.0f;
                    const auto airBand = ta::render::pickBand(mx, my, zm, b.boxes, dotHalf, f.maxInterpStep + 1.0f,
                                                               std::min(b.sinkLo, 0.0f) - altitude, b.sinkHi);
                    const auto air = ta::render::anchorBandToWorld(airBand.left, airBand.top, airBand.right,
                                                                    airBand.bottom, mapView_.offX(), mapView_.offY(),
                                                                    zm, unitLiftBounds());
                    f.unitGrid.query(air.left, air.top, air.right, air.bottom, pickQueryBits_, pickFlyers_);
                    pickMerged_.clear();
                    std::set_intersection(pickFlyers_.begin(), pickFlyers_.end(), f.flyerUnits.begin(),
                                          f.flyerUnits.end(), std::back_inserter(pickMerged_));
                    pickFlyers_.swap(pickMerged_);
                    pickMerged_.clear();
                    std::set_union(pickQuery_.begin(), pickQuery_.end(), pickFlyers_.begin(), pickFlyers_.end(),
                                   std::back_inserter(pickMerged_));
                    out = pickMerged_;
                }
            }
        }
#ifndef NDEBUG
        static const bool verify = ta::devFlag("TA_PICK_INDEX_VERIFY");
        if (verify && !pickForceFull_) {
            ++pickVerify_.queries;
            pickVerify_.live += n;
            pickVerify_.candidates += out.size();
            if (!std::is_sorted(out.begin(), out.end()) ||
                std::adjacent_find(out.begin(), out.end()) != out.end() || (!out.empty() && out.back() >= n))
                throw std::runtime_error("pick index verify: candidates are not ascending live indices");
            // Before the exact tests below cache anything: a query that skipped units has
            // no unit whose type's hit box the exact test could still cache as the fallback.
            if (out.size() < n)
                for (const UnitR* u : f.live)
                    if (u->type && !hitBoxes_.count(u->type->id) && !visuals_.count(u->type->id))
                        throw std::runtime_error("pick index verify: candidates skipped units while a type has no model");
            for (size_t i = 0; i < n; ++i)
                if (unitUnderCursor(*f.live[i], mx, my)) {
                    ++pickVerify_.hits;
                    if (!std::binary_search(out.begin(), out.end(), uint32_t(i)))
                        throw std::runtime_error("pick index verify: a unit under the cursor is not a candidate");
                }
        }
#endif
        return out;
    }

    std::span<const uint32_t> GameView::bodyCandidates(float wx, float wz) const {
        const Frame& f = front();
        const size_t n = f.live.size();
        static const bool fullScan = ta::devFlag("TA_PICK_FULL_SCAN");
        std::span<const uint32_t> out;
        if (fullScan || pickForceFull_ || unitFullScan_ || f.unitGrid.size() != n) {
            for (size_t i = bodyAll_.size(); i < n; ++i) bodyAll_.push_back(uint32_t(i));
            out = std::span<const uint32_t>(bodyAll_.data(), n);
        } else {
            // ReclaimPick::offer accepts only dx*dx + dz*dz < r*r: |dx|, |dz| < r. One
            // world px of slack for the float evaluation.
            const float r = ta::client::reclaimPickRadius(f.maxCorpseFoot, f.maxCorpseFoot) + 1.0f;
            f.unitGrid.query(wx - r, wz - r, wx + r, wz + r, bodyQueryBits_, bodyQuery_);
            out = bodyQuery_;
        }
#ifndef NDEBUG
        static const bool verify = ta::devFlag("TA_PICK_INDEX_VERIFY");
        if (verify && !pickForceFull_) {
            ++pickVerify_.bodyQueries;
            pickVerify_.bodyCandidates += out.size();
            if (!std::is_sorted(out.begin(), out.end()) ||
                std::adjacent_find(out.begin(), out.end()) != out.end() || (!out.empty() && out.back() >= n))
                throw std::runtime_error("pick index verify: body candidates are not ascending live indices");
            for (size_t i = 0; i < n; ++i) {
                const UnitR& u = *f.live[i];
                ta::client::ReclaimPick probe{wx, wz};
                probe.offer(0, u.x, u.z, u.corpseFootX, u.corpseFootZ);
                if (!probe.id) continue;
                ++pickVerify_.bodyHits;
                if (!std::binary_search(out.begin(), out.end(), uint32_t(i)))
                    throw std::runtime_error("pick index verify: a body pick circle holding the point is not a candidate");
            }
            // The cursor rarely rests on a body, so also probe one body's own centre
            // per query (round robin): the grid query there must return it.
            if (out.size() < n && f.maxCorpseFoot > 0) {
                std::vector<size_t> bodies;
                for (size_t i = 0; i < n; ++i)
                    if (f.live[i]->corpseFootX > 0 || f.live[i]->corpseFootZ > 0) bodies.push_back(i);
                if (!bodies.empty()) {
                    const size_t i = bodies[size_t(pickVerify_.bodyQueries) % bodies.size()];
                    const UnitR& u = *f.live[i];
                    const float r = ta::client::reclaimPickRadius(f.maxCorpseFoot, f.maxCorpseFoot) + 1.0f;
                    std::vector<uint64_t> bits;
                    std::vector<uint32_t> probeOut;
                    f.unitGrid.query(u.x - r, u.z - r, u.x + r, u.z + r, bits, probeOut);
                    ta::client::ReclaimPick probe{u.x, u.z};
                    probe.offer(0, u.x, u.z, u.corpseFootX, u.corpseFootZ);
                    if (probe.id) {
                        ++pickVerify_.bodyHits;
                        if (!std::binary_search(probeOut.begin(), probeOut.end(), uint32_t(i)))
                            throw std::runtime_error("pick index verify: a body is not a candidate at its own centre");
                    }
                }
            }
        }
#endif
        return out;
    }

    bool GameView::unitUnderCursor(const UnitR& u, float mx, float my, float* d2, float* size) {
        renderStats_.add(ta::render::Counter::CursorPickTests);
#ifndef NDEBUG
        ++debugPickTests_;
#endif
        if (!canPickUnit(u)) return false;
        if (dotsFrame_) {
            // Tactical Dots on screen: the hit region is the dot, with the same
            // 9px floor as a tiny model so a lone dot is still easy to click.
            SDL_FPoint c = dotCentre(u);
            float half = std::max(dotSide(u) * 0.5f + 1.0f, 9.0f);
            if (std::fabs(mx - c.x) > half || std::fabs(my - c.y) > half) return false;
            if (d2) { float dx = c.x - mx, dy = c.y - my; *d2 = dx * dx + dy * dy; }
            if (size) *size = 4 * half * half;
            renderStats_.add(ta::render::Counter::CursorPickHits);
            return true;
        }
        // A HIDDEN UNITS dot on screen (the unit behind scenery) is clickable like a
        // tactical dot, as well as the model's own box.
        if (hiddenDot(u.id)) {
            const SDL_FPoint c = dotCentre(u);
            const float half = std::max(dotSide(u) * 0.5f + 1.0f, 9.0f);
            if (std::fabs(mx - c.x) <= half && std::fabs(my - c.y) <= half) {
                if (d2) { float dx = c.x - mx, dy = c.y - my; *d2 = dx * dx + dy * dy; }
                if (size) *size = 4 * half * half;
                renderStats_.add(ta::render::Counter::CursorPickHits);
                return true;
            }
        }
        float zms = mapView_.zoom();
        SDL_FPoint p = unitScreen(u);
        const SDL_FRect& hb = unitHitBox(u.type);
        float ax = p.x, ay = p.y + 12.0f * zms;         // sprite draw anchor
        float cx = ax + (hb.x + hb.w * 0.5f) * zms;
        float cy = ay + (hb.y + hb.h * 0.5f) * zms;
        float hw = std::max(hb.w * zms * 0.5f, 9.0f);
        float hh = std::max(hb.h * zms * 0.5f, 9.0f);
        if (std::fabs(mx - cx) > hw || std::fabs(my - cy) > hh) return false;
        // Retail 0x520530: past the quad test, the cursor's GROUND cell (0x50e660)
        // in an open gate's doorway does not pick that gate, so hover, click and
        // right-click all fall through to whatever stands in, or beyond, the opening.
        if (u.yardOpen && u.type->gate) {
            float wx, wz;
            pickWorld(mx, my, wx, wz);
            if (openGateDoorwayAt(u, wx, wz)) return false;
        }
        if (d2) { float dx = cx - mx, dy = cy - my; *d2 = dx * dx + dy * dy; }
        if (size) *size = 4 * hw * hh;
        renderStats_.add(ta::render::Counter::CursorPickHits);
        return true;
    }

    void GameView::playClickTone(int semitone) {
        if (watchOnly()) return;   // nothing a watcher does is acknowledged
        constexpr bool kPitchedBongs = false;   // pending icd proof
        constexpr int kToneRef = 4;
        float rate = (kPitchedBongs && semitone > 0)
                         ? std::pow(2.0f, float(semitone - kToneRef) / 12.0f)
                         : 1.0f;
        std::string t = "tone" + side_;
        if (!sounds_.has(t)) return;
        // Peak-normalize the acknowledgment to a prominent UI level: retail's
        // DirectSound path played tones hot (full per-ear gain, no equal-power
        // pan loss, no mixing headroom); matching by amplitude alone leaves
        // both retail's tone (peak 0.31) and soft click.hpi replacements
        // (peak 0.39) buried under our /2-headroom + 0.707-pan chain. Target
        // ~0.7 per ear: gain = 0.7 / (peak * 0.5 * 0.707), capped so a
        // near-silent file can't amplify noise floor.
        float peak = sounds_.peakOf(t);
        float gain = peak > 0.01f
                         ? std::clamp(0.7f / (peak * 0.5f * 0.707f), 1.0f, 8.0f)
                         : 2.0f;
        sounds_.play(t, gain, rate);
    }

    // A chat sender's name colour: their player colour when the name is a seated
    // player's, a warm gold for this client's own notices, a neutral grey otherwise
    // (spectators). Dark player colours are lifted so a name never sinks into the
    // box's dark backing.
    SDL_Color GameView::chatSenderColor(const std::string& who, bool notice) const {
        auto same = [](const std::string& a, const std::string& b) {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
            });
        };
        // Gold only for notices this client raised itself: a relayed sender name is
        // whatever the other end chose, so "SERVER" from the wire gets no special look.
        if (notice) return {255, 214, 120, 255};
        SDL_Color c{200, 205, 215, 255};
        for (int i = 0; i < ta::sim::kMaxPlayers; ++i)
            if (!playerName_[i].empty() && same(playerName_[i], who)) { c = playerColor(i); break; }
        // Too dark to read on black (the blue player is): mix toward white, keeping the hue.
        const float lum = 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
        if (lum < 160) {
            const float k = std::min(0.6f, (160 - lum) / (255 - lum));
            c = {Uint8(c.r + (255 - c.r) * k), Uint8(c.g + (255 - c.g) * k), Uint8(c.b + (255 - c.b) * k), 255};
        }
        return c;
    }

    // The in-game chat box. Received lines read oldest to newest downwards, the newest
    // just above the composer; each starts with the sender's name in their colour and
    // wraps to the box width. The text is the HUD's plain Times face drawn in flat
    // colour with a drop shadow, over one translucent dark panel, so it reads over
    // bright sand and dark forest alike. Size is UI SCALE x CHAT SCALE; the box sits
    // left or centred over the play area (never under the command panel), above the
    // information bar and the build row's band with its TRAINING line, whether or not
    // a builder's row is up (chatbox.h hudFloor).
    void GameView::drawGameChat(int winW, int winH) {
        // The embedded face; the HUD font only if it ever failed to load.
        if (ta::uiFont().ok()) drawGameChatIn(ta::uiFont(), winW, winH);
        else drawGameChatIn(hudFont_, winW, winH);
    }

    template <class Face>
    void GameView::drawGameChatIn(const Face& font, int winW, int winH) {
        namespace cb = ta::chatbox;
        if (!mp_ || !font.ok() || (!chatTyping_ && gameChat_.empty())) return;

        const float areaW = float(mapViewW(winW));
        const float margin = std::round(10 * uiScale_);
        // Above the information bar AND the build row, its TRAINING line and tooltip
        // (chatbox.h hudFloor): the box never covers HUD text, at any scale.
        const float barTop = float(winH - barH());
        float bottom = cb::hudFloor(barTop, buildRowBaseH(), buildBarScale_, buildRowTop_,
                                    hudShown(), margin);
        if (hudShown() && selPanelTop_ > 0) bottom = std::min(bottom, selPanelTop_ - margin);   // nor the selection
        // The box may use 45% of the play area's height, so long as it fits above the
        // HUD; higher up it would start to cover the battle rather than annotate it.
        const float maxBoxH = std::max(0.0f, std::min(barTop * 0.45f, bottom - 2 * margin));

        float top1 = 0, h1 = 0;
        font.vbounds("Hdgjpqy", 1, top1, h1);
        h1 = std::max(h1, 1.0f);
        // Text 16 px tall at 100%, exactly as chosen; but never so big that three rows
        // (two lines and the composer) cannot fit in the room there is. That shrink stops
        // at 10 px text (or the chosen size, if smaller), below which it would be unreadable.
        const float sChosen = uiScale_ * chatScale_, sFloor = std::min(sChosen, 10.0f / 16.0f);
        float s = sChosen;
        auto rowH = [&](float sc) { return 16.0f * sc + 6.0f * sc; };
        while (s > sFloor && 3 * rowH(s) + 16 * s > maxBoxH) s = std::max(sFloor, s * 0.9f);
        const float textH = 16.0f * s;
        const float fs = textH / h1;          // font scale
        const float lineH = rowH(s), pad = std::round(8 * s), indent = std::round(14 * s);
        // The drop shadow follows the box scale (1 px at 100%, 2 px from 200%); the GAF
        // fallback keeps its font-relative one.
        const float shadow = std::is_same_v<Face, Font> ? std::max(1.0f, std::round(fs * 0.6f))
                                                        : std::max(1.0f, std::round(0.8f * s));
        const float wrapW = std::max(80.0f, std::min(areaW - 2 * margin, 760 * s) - 2 * pad);
        auto width = [&](const std::string& t) { return float(font.width(t, fs)); };

        struct Seg { std::string text; SDL_Color color; };
        struct Row { std::vector<Seg> segs; float x = 0, alpha = 1; bool composer = false; };
        std::vector<Row> rows;
        auto addMessage = [&](const std::string& head, SDL_Color headColor, const std::string& body,
                              SDL_Color bodyColor, float alpha, bool composer) {
            const float headW = width(head);
            const auto parts = cb::wrap(body, std::max(wrapW - headW, wrapW * 0.25f), wrapW - indent, width);
            for (size_t i = 0; i < parts.size(); ++i) {
                Row r;
                r.alpha = alpha;
                r.composer = composer;
                if (i == 0) {
                    r.segs.push_back({head, headColor});
                    r.x = 0;
                } else r.x = indent;
                r.segs.push_back({parts[i], bodyColor});
                rows.push_back(std::move(r));
            }
        };
        const SDL_Color bodyColor{238, 234, 222, 255};
        // While typing, the recent history is shown in full; otherwise only lines still
        // inside their hold + fade time.
        const int maxMsgs = chatTyping_ ? 8 : 6;
        std::vector<const GameChat*> msgs;
        for (auto it = gameChat_.rbegin(); it != gameChat_.rend() && int(msgs.size()) < maxMsgs; ++it)
            if (chatTyping_ || cb::lineAlpha(it->age) > 0) msgs.push_back(&*it);
        std::reverse(msgs.begin(), msgs.end());
        for (const GameChat* m : msgs) {
            const float a = chatTyping_ ? 1.0f : cb::lineAlpha(m->age);
            if (m->who.empty())   // a local system line (power codes, the poster): no name, all gold
                addMessage("", chatSenderColor(m->who, true), m->text, chatSenderColor(m->who, true), a, false);
            else
                addMessage(m->who + ": ", chatSenderColor(m->who, m->notice), m->text, bodyColor, a, false);
        }
        const size_t firstComposer = rows.size();
        if (chatTyping_) addMessage("SAY: ", {255, 214, 120, 255}, chatDraft_ + "_", {255, 248, 225, 255}, 1, true);
        if (rows.empty()) return;

        // Too tall: drop the oldest rows; then, for a draft taller than the room on its
        // own, scroll the composer so its last rows (the caret's) stay in view.
        size_t composerAt = firstComposer;
        auto sepH = [&] { return chatTyping_ && composerAt > 0 ? std::round(6 * s) : 0.0f; };
        auto boxH = [&] { return float(rows.size()) * lineH + 2 * pad - 6 * s + sepH(); };
        while (boxH() > maxBoxH && composerAt > 0) {
            rows.erase(rows.begin());
            --composerAt;
        }
        while (boxH() > maxBoxH && rows.size() > 1) rows.erase(rows.begin());
        // No room at all above a build row that fills the window: idle lines wait rather
        // than draw over the row's text (the composer, asked for, still shows at the top).
        if (!chatTyping_ && boxH() > maxBoxH) return;
        float contentW = 0, alpha = 0;
        for (const Row& r : rows) {
            float w = r.x;
            for (const Seg& g : r.segs) w += width(g.text);
            contentW = std::max(contentW, w);
            alpha = std::max(alpha, r.alpha);
        }
        // While typing the box keeps its full width, so it does not grow under the caret.
        const float boxW = (chatTyping_ ? wrapW : contentW) + 2 * pad;
        const cb::Rect box = cb::place(chatAlign_, boxW, boxH(), areaW, bottom, 0, margin);

        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_FRect bg{box.x, box.y, box.w, box.h};
        SDL_SetRenderDrawColor(ren_, 8, 8, 12, Uint8(200 * alpha));
        SDL_RenderFillRectF(ren_, &bg);
        SDL_SetRenderDrawColor(ren_, 96, 84, 60, Uint8(200 * alpha));   // the HUD's bronze frame
        SDL_RenderDrawRectF(ren_, &bg);

        // Clip so a row can never draw outside the box (a long unbroken name, say).
        SDL_Rect clip{int(box.x), int(box.y), int(std::ceil(box.w)), int(std::ceil(box.h))};
        const bool inSight = ta::cliprect::set(ren_, clip);   // off the target: no rows (Metal)
        float y = box.y + pad;
        for (size_t i = 0; inSight && i < rows.size(); ++i) {
            const Row& r = rows[i];
            if (r.composer && i == composerAt && i > 0) {   // a rule between history and composer
                SDL_SetRenderDrawColor(ren_, 96, 84, 60, 200);
                SDL_RenderDrawLineF(ren_, box.x + pad, y + sepH() / 2 - 3 * s, box.x + box.w - pad, y + sepH() / 2 - 3 * s);
                y += sepH();
            }
            float x = box.x + pad + r.x;
            const float baseY = y - top1 * fs;
            for (const Seg& g : r.segs) {
                const Uint8 a = Uint8(255 * r.alpha);
                font.drawSolid(ren_, g.text, x + shadow, baseY + shadow, fs, {0, 0, 0, a});
                font.drawSolid(ren_, g.text, x, baseY, fs, {g.color.r, g.color.g, g.color.b, a});
                x += width(g.text);
            }
            y += lineH;
        }
        SDL_RenderSetClipRect(ren_, nullptr);
    }

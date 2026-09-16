#include "client/gameview.h"
#include "client/artscale.h"
#include "client/gpuvram.h"
#include "client/statsfit.h"
#include "net/protocol.h"
#include "util/procmetrics.h"

// Out-of-line GameView method definitions (hud concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    void GameView::rightClickOrder(float wx, float wz, bool queue) {
        if (selection_.empty()) return;
        const auto* first = frameUnitP(selection_.front());
        // Selected transport with cargo: right-click = sail + disembark.
            if (first && first->type && first->type->canTransport &&
                !first->cargo.empty()) {
                ta::net::Command c;
                c.kind = ta::net::Cmd::Unload;
                c.unitId = first->id;
                c.x = wx;
                c.z = wz;
                issue(c);
                return;
            }
            // Clicking a friendly transport = board it.
            int friendlyTransport = -1;
            float bestT = 24 * 24;
            for (const UnitR* _up : front().live) {
                const UnitR& u = *_up;
                if (!u.alive() || !first || u.player != first->player || !u.type ||
                    !u.type->canTransport)
                    continue;
                float dx = u.x - wx, dz = u.z - wz;
                if (dx * dx + dz * dz < bestT) { bestT = dx * dx + dz * dz; friendlyTransport = u.id; }
            }
            if (friendlyTransport >= 0) {
                for (int id : selection_) {
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Load;
                    c.unitId = id;
                    c.targetId = friendlyTransport;
                    issue(c);
                }
                return;
            }
            // Right-clicking a friendly unit is contextual, never a move:
            //  - a conjure-in-progress: selected builders that can build that type
            //    resume/assist it (revives a decaying site); other units do nothing.
            //  - any other friendly unit: selected units that can attack guard it;
            //    the rest do nothing.
            // Either case consumes the click (no move fallthrough).
            {
                int siteId = -1;   float bestSite = 1e18f;
                int allyId = -1;   float bestAlly = 22.0f * 22.0f;
                for (const UnitR* _up : front().live) {
                    const UnitR& u = *_up;
                    if (!u.alive() || u.embarked() || !u.type || !first ||
                        !world_.allied(u.player, first->player)) continue;
                    float dx = u.x - wx, dz = u.z - wz, d = dx * dx + dz * dz;
                    if (u.underConstruction) {
                        // Any allied conjure (yours or a teammate's) can be revived.
                        float r = 20.0f + 8.0f * float(std::max(u.type->footX, u.type->footZ));
                        if (d < r * r && d < bestSite) { bestSite = d; siteId = u.id; }
                    } else if (d < bestAlly) { bestAlly = d; allyId = u.id; }
                }
                if (siteId >= 0) {
                    const auto* st = frameUnitP(siteId);
                    bool any = false;
                    for (int id : selection_) {
                        const auto* bu = frameUnitP(id);
                        if (!bu || !bu->type || !bu->type->isBuilder) continue;
                        const auto& menu = registry_.buildable(bu->type->id);
                        if (std::find(menu.begin(), menu.end(), st->type->id) == menu.end())
                            continue;
                        ta::net::Command c;
                        c.kind = ta::net::Cmd::Assist;
                        c.unitId = id; c.targetId = siteId; c.queue = queue;
                        issue(c);
                        any = true;
                    }
                    if (any) voice(selection_.front(), "move");
                    return;   // non-builders / can't-build-it: nothing happens
                }
                if (allyId >= 0) {
                    bool any = false;
                    for (int id : selection_) {
                        const auto* gu = frameUnitP(id);
                        if (!gu || !gu->type || gu->type->weapon.damage <= 0) continue;
                        ta::net::Command c;
                        c.kind = ta::net::Cmd::Guard;
                        c.unitId = id; c.targetId = allyId; c.queue = queue;
                        issue(c);
                        any = true;
                    }
                    if (any) voice(selection_.front(), "move");
                    return;   // non-attackers: nothing happens
                }
            }
            // Clicking near an enemy = attack; else formation move. (Allies are
            // not enemies -- clicking one falls through to a move, not an attack.)
            int enemy = -1;
            float best = 20 * 20;
            for (const UnitR* _up : front().live) {
                const UnitR& u = *_up;
                if (!u.alive() || u.embarked() || !first ||
                    world_.allied(u.player, first->player)) continue;
                float dx = u.x - wx, dz = u.z - wz;
                if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; enemy = u.id; }
            }
            // A reclaimer clicking directly on a reclaimable feature (with no enemy
            // there) reclaims just that one -- retail's single Reclaim.
            if (enemy < 0 && haveReclaimer()) {
                int fid = 0; bool fhit = false; float bestF = 1e18f;
                {   // Live read: the worker can reallocate this vector under us. One-shot
                    // on a click, so the lock costs nothing worth measuring -- unlike the
                    // per-frame cursor scan above, which uses the render-side snapshot.
                    std::unique_lock<std::mutex> lk(simMutex_, std::defer_lock);
                    if (useSimThread_) lk.lock();
                    for (const auto& f : world_.features()) {
                        if (!f.alive) continue;
                        float dx = f.x - wx, dz = f.z - wz, d = dx * dx + dz * dz;
                        float r = 18.0f + 8.0f * float(std::max(f.fx, f.fz));
                        if (d < r * r && d < bestF) { bestF = d; fid = f.id; fhit = true; }
                    }
                }
                // Corpses / statues / rubble under the click too (negative id).
                for (const UnitR* _cp : front().live) {
                    const UnitR& cu = *_cp;
                    if (cu.alive() || !cu.corpsePhase || cu.corpseFeat < 0 || !cu.type) continue;
                    if (size_t(cu.corpseFeat) >= world_.featureTypes().size() ||
                        !world_.featureTypes()[size_t(cu.corpseFeat)].reclaimable) continue;
                    float dx = cu.x - wx, dz = cu.z - wz, d = dx * dx + dz * dz;
                    float r = 18.0f + 8.0f * float(std::max(cu.type->footX, cu.type->footZ));
                    if (d < r * r && d < bestF) { bestF = d; fid = -cu.id; fhit = true; }
                }
                if (fhit) {
                    int builderId = firstReclaimer();
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Reclaim;
                    c.unitId = builderId;
                    c.targetId = fid;
                    c.queue = uint8_t(queue ? 1 : 0);
                    issue(c);
                    voice(builderId, "move");
                    return;
                }
            }
            if (enemy >= 0) {
                for (int id : selection_) {
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Attack;
                    c.unitId = id;
                    c.targetId = enemy;
                    c.queue = queue;
                    issue(c);
                }
                voice(selection_.front(), "attack");
            } else {
                voice(selection_.front(), "move");
                float cx = 0, cz = 0;
                int n = 0;
                for (int id : selection_)
                    if (const auto* u = frameUnitP(id)) { cx += u->x; cz += u->z; ++n; }
                if (n) { cx /= float(n); cz /= float(n); }
                for (int id : selection_) {
                    const auto* u = frameUnitP(id);
                    if (!u) continue;
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Move;
                    c.unitId = id;
                    c.x = wx + std::clamp(u->x - cx, -60.0f, 60.0f);
                    c.z = wz + std::clamp(u->z - cz, -60.0f, 60.0f);
                    c.queue = queue;
                    issue(c);
                }
            }
        }

    void GameView::drawCursorOverlay() {
        if (!cursorsInit_) { cursorsInit_ = true; cursors_.load(ren_, vfs_, settings_); }
        // A benchmark RUN is hands-off: hide the cursor entirely (OS + software). It
        // returns for the stats screen (benchStatsShown_) so DONE is clickable.
        if (benchmarkMode_ && !benchStatsShown_) {
            if (cursorMode_ != 0) { cursors_.releaseHardware(); SDL_ShowCursor(SDL_DISABLE); cursorMode_ = 0; }
            return;
        }
        if (!cursors_.ok()) {   // no cursor art -> just keep the OS arrow
            if (cursorMode_ != 1) { SDL_ShowCursor(SDL_ENABLE); cursorMode_ = 1; }
            return;
        }
        bool fightTint = false;
        ta::CursorId c = desiredCursor(fightTint);
        int sc = settings_ ? settings_->cursorScale : 1;
        SDL_Color tint = fightTint ? kFightMoveTint : SDL_Color{255, 255, 255, 255};

        // HARDWARE cursor: hand the sprite to the OS, which tracks the pointer position
        // itself -- so it stays smooth even when a heavy frame stalls our render loop.
        if (settings_ && settings_->hardwareCursor && !hwCursorFailed_) {
            if (cursorMode_ != 1) { SDL_ShowCursor(SDL_ENABLE); cursorMode_ = 1; }
            if (cursors_.applyHardware(c, sc, tint)) return;
            hwCursorFailed_ = true;   // platform rejected it (size cap?) -> software from here on
            std::fprintf(stderr, "cursor: hardware cursor unavailable -- using software\n");
        }

        // SOFTWARE cursor: hide the OS arrow and draw our own into the frame.
        if (cursorMode_ != 0) { cursors_.releaseHardware(); SDL_ShowCursor(SDL_DISABLE); cursorMode_ = 0; }
        // Pointer position in renderer-output pixels (the space mouse events are mapped
        // into). Before the first motion, sample the OS position and map it the same way.
        int mx, my;
        if (mouseX_ >= 0) { mx = int(mouseX_); my = int(mouseY_); }
        else {
            int wx, wy; SDL_GetMouseState(&wx, &wy);
            float lx, ly; SDL_RenderWindowToLogical(ren_, wx, wy, &lx, &ly);
            mx = int(lx); my = int(ly);
        }
        cursors_.draw(ren_, c, mx, my, sc, tint);
    }

    ta::CursorId GameView::desiredCursor(bool& fightTint) {
        fightTint = false;
        // Overlays / lobby: a plain arrow for clicking UI.
        if (inLobbyPhase() || exitMenu_ || options_)
            return ta::CursorId::Normal;
        // Build/conjure placement: green when it fits, red when blocked (matches the ghost).
        if (placing_ && mouseX_ >= 0) {
            float wx, wz; pickWorld(mouseX_, mouseY_, wx, wz);
            if (canPlaceLocked(placing_, wx, wz)) return ta::CursorId::Green;
            // Clearable doodads: the click works (it clears first), so keep it green.
            std::vector<int> feats;
            if (clearableAt(placing_, wx, wz, feats) && !feats.empty())
                return ta::CursorId::Green;
            return ta::CursorId::Red;
        }
        // Right-drag "clear this area": show the broom only once the pointer has moved
        // enough to actually be a box (the same 6px threshold that tells a right-CLICK
        // from a box on release). Before that, keep the ordinary hover cursor.
        if (reclaimDrag_ &&
            (std::fabs(mouseX_ - rdSx0_) >= 6.0f || std::fabs(mouseY_ - rdSy0_) >= 6.0f))
            return ta::CursorId::Reclaim;
        if (dragging_) return ta::CursorId::Normal;       // box-select drag
        if (pendingCmd_)  { fightTint = (pendingCmd_ == 'f'); return cursorForCmd(pendingCmd_); }
        if (mouseX_ < 0)  return ta::CursorId::Normal;
        float wx, wz; pickWorld(mouseX_, mouseY_, wx, wz);
        return hoverCursor(wx, wz);
    }

    ta::CursorId GameView::hoverCursor(float wx, float wz) {
        const auto* first = selection_.empty() ? nullptr : frameUnitP(selection_.front());

        // Selected transport carrying cargo -> unload cursor anywhere.
        if (first && first->type && first->type->canTransport && !first->cargo.empty())
            return ta::CursorId::Unload;

        if (first) {
            // One SCREEN-SPACE pass over the drawn sprites (unitUnderCursor: the
            // projected model bounds + lift + flyer altitude -- the exact region
            // a click selects), categorised; overlaps resolve to the nearest
            // sprite centre. The old world-space 20-22px centre radii ignored
            // footprints entirely: most of a keep hovered as bare ground.
            int loadId = -1, siteId = -1, ownId = -1, allyId = -1, enemy = -1;
            float bLoad = 1e30f, bSite = 1e30f, bOwn = 1e30f, bAlly = 1e30f,
                  bEnemy = 1e30f;
            for (const UnitR* _up : front().live) { const UnitR& u = *_up;
                if (!u.alive() || u.embarked() || !u.type) continue;
                float d = 0;
                if (!unitUnderCursor(u, mouseX_, mouseY_, &d)) continue;
                bool ally = world_.allied(u.player, first->player);
                if (ally && u.type->canTransport && u.player == first->player &&
                    !u.underConstruction) {
                    if (d < bLoad) { bLoad = d; loadId = u.id; }
                } else if (ally && u.underConstruction) {
                    if (d < bSite) { bSite = d; siteId = u.id; }
                } else if (ally && u.player == localPlayer_) {
                    if (d < bOwn) { bOwn = d; ownId = u.id; }
                } else if (ally) {
                    if (d < bAlly) { bAlly = d; allyId = u.id; }
                } else {
                    if (d < bEnemy) { bEnemy = d; enemy = u.id; }
                }
            }
            if (loadId >= 0) return ta::CursorId::Load;
            if (siteId >= 0) return ta::CursorId::Repair;   // assist a build/revive
            if (ownId  >= 0) return ta::CursorId::Select;
            if (allyId >= 0) return ta::CursorId::Green;
            if (enemy >= 0) {
                bool canAtk = false;
                for (int id : selection_)
                    if (const auto* a = frameUnitP(id))
                        if (a->type && a->type->weapon.damage > 0) { canAtk = true; break; }
                return canAtk ? ta::CursorId::Attack : ta::CursorId::Red;
            }

            // A reclaimable feature under the pointer (reclaimer selected) -> broom.
            if (haveReclaimer()) {
                // RENDER-SIDE snapshot, not world_.features(). This runs every frame (and
                // again under hardware-cursor mode), and the sim worker both mutates
                // feature fields and push_backs corpses onto that vector -- a reallocation
                // under this loop, not merely a torn read. features_ is ours, and
                // syncBurningFeatures keeps aliveVis and the footprint current under the
                // lock. Locking here instead would be correct but would put a per-frame
                // simMutex_ wait back into the HUD, which is what the feature-sync
                // generation counter just removed.
                for (const auto& f : features_) {
                    // hasSim, NOT just aliveVis. features_ holds every VISUAL feature,
                    // including decoration with no sim feature behind it (shoreline
                    // waves and the like); the snapshot reports those alive so they keep
                    // drawing. Testing aliveVis alone put the broom over scenery that no
                    // click could reclaim -- world_.features(), which this replaced, only
                    // ever contained real sim features.
                    if (!f.hasSim || !f.aliveVis) continue;
                    float dx = f.x - wx, dz = f.z - wz;
                    float r = 18.0f + 8.0f * float(std::max(f.fx, f.fz));
                    if (dx * dx + dz * dz < r * r) return ta::CursorId::Reclaim;
                }
            }

            // Empty ground: plain arrow. The Move cursor shows ONLY when the move order
            // is armed (Move button / hotkey), not merely from having a unit selected.
            return ta::CursorId::Normal;
        }

        // Nothing selected: highlight your own unit under the pointer, else the arrow.
        for (const UnitR* _up : front().live) { const UnitR& u = *_up;
            if (!u.alive() || u.embarked() || !u.type || u.player != localPlayer_) continue;
            float dx = u.x - wx, dz = u.z - wz;
            if (dx * dx + dz * dz < 22.0f * 22.0f) return ta::CursorId::Select;
        }
        return ta::CursorId::Normal;
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

    int GameView::cmdPanelW() const {
        if (gui_.gadgets.empty()) return panelW();
        return std::max(miniSize() + 12, int(128 * guiS()) + 8);
    }

    SDL_FRect GameView::guiCmdRect(const ta::gui::Gadget& g) const {
        float s = guiS();
        // 640-space y=480 (screen bottom in retail) maps just above our info bar so
        // the command panel and the existing bottom bar don't overlap.
        float baseY = winH_ - barH();
        return {winW_ - (640 - g.x) * s, baseY - (480 - g.y) * s, g.w * s, g.h * s};
    }

    SDL_FRect GameView::guiBarRect(const ta::gui::Gadget& g) const {
        float vs = float(barH()) / 49.0f;   // 49-tall retail bar -> barH() px
        float barTop = winH_ - barH();
        return {g.x * vs, barTop + (g.y - 431) * vs, g.w * vs, g.h * vs};
    }

    SDL_FRect GameView::minimapRect(int winW, int winH) const {
        float aspect = float(mapView_.map().blocksY) / float(mapView_.map().blocksX);
        if (fsRadar_) {
            // Retail sets the rect to exactly the world viewport (0, 0,
            // screenW-128, screenH-48) so the command panel and the bottom bar
            // stay visible. We fit the map's aspect INSIDE that viewport and
            // centre it rather than stretching to fill: retail's corner box is
            // square while ours already honours the map's shape, and stretching a
            // 2:1 map to a 16:9 viewport would distort every distance on it.
            float vw = float(mapViewW(winW)), vh = float(winH) - barH();
            float w = vw, h = vw * aspect;
            if (h > vh) { h = vh; w = vh / aspect; }
            return {(vw - w) * 0.5f, (vh - h) * 0.5f, w, h};
        }
        return {float(winW) - miniSize() - 10, 10, float(miniSize()), float(miniSize()) * aspect};
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

    void GameView::drawStatsPanel(int winW, int winH) {
        if (!statsPanel_ || !hudFont_.ok()) return;

        // WHERE THE GAP IS. The right strip holds the minimap at the top and the command
        // panel at the bottom; everything between them is the black space this fills.
        // Both edges move: the minimap is as tall as the map's aspect makes it, and the
        // command panel is anchored to the bottom bar and scaled by guiS(). So measure
        // them rather than assuming a layout -- a hard-coded gap would overlap the
        // command panel on a tall map, or float in mid-strip on a wide one.
        const float pad = std::max(4.0f, 6.0f * uiScale_);
        float x0 = float(mapViewW(winW)) + pad;
        float x1 = float(winW) - pad;
        // TAB (full-screen radar) moves the minimap out of the strip entirely and over
        // the world viewport, so the strip is free all the way to the top.
        float top = pad;
        if (!fsRadar_) {
            SDL_FRect mr = minimapRect(winW, winH);
            top = mr.y + mr.h + 2 + pad;
        }
        // Bottom edge: the top of the command panel art when a GUI is loaded, else the
        // bottom bar. A spectator gets neither, so the strip runs to the window edge.
        float bot = spectating_ ? float(winH) - pad : float(winH) - barH() - pad;
        if (!spectating_) {
            int mi = guiIdx("UnitMenu");
            if (mi >= 0 && mi < int(gui_.gadgets.size()))
                bot = guiCmdRect(gui_.gadgets[size_t(mi)]).y - pad;
        }
        float availW = x1 - x0, availH = bot - top;
        if (availW <= 8 || availH <= 4) return;

        // WHAT TO SHOW, most useful first. Rows are dropped from the END of this list,
        // so the ordering is the priority ordering: frame rate before link quality before
        // counts before process stats.
        //
        // Labels are kept to the width budget below (<=5 chars) and values to <=7, which
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
            std::snprintf(b, sizeof b, "%.2fX", actualSpeed_);
            rows.push_back({"SIM", b});
        }

        // One pass over the render snapshot for both counts -- the live world is never
        // touched here (the sim worker owns it; this is display only).
        int mine = 0, all = 0;
        for (const UnitR* up : front().live) {
            if (!up->alive() || !up->type) continue;
            ++all;
            if (up->player == localPlayer_) ++mine;
        }
        if (!spectating_) {
            std::snprintf(b, sizeof b, "%d", mine);
            rows.push_back({"YOURS", b});
        }
        std::snprintf(b, sizeof b, "%d", all);
        rows.push_back({"UNITS", b});

        // Game clock from the snapshot's tick, not wall time: a paused or catching-up
        // client should show the clock the SIM is at, which is what a player comparing
        // notes with anyone else in the game means by "how long in are we".
        uint32_t secs = front().gameTick / uint32_t(ta::net::kServerHz);
        std::snprintf(b, sizeof b, "%u:%02u", secs / 60, secs % 60);
        rows.push_back({"TIME", b});

        if (!spectating_) {
            std::snprintf(b, sizeof b, "%d", framePlayer(localPlayer_).kills);
            rows.push_back({"KILLS", b});
        }

        // Process stats. proc::sample() reads /proc (or the OS equivalent) so it is NOT
        // free per frame -- sample it once a second and reuse. Doing this per frame was
        // measurable on the very machines whose frame rate the panel exists to report.
        static uint32_t memAt = 0;
        static size_t memRss = 0;
        uint32_t now = SDL_GetTicks();
        if (memRss == 0 || now - memAt > 1000) {
            ta::proc::Sample ps = ta::proc::sample(0);
            if (ps.ok) memRss = ps.rssBytes;
            memAt = now;
        }
        if (memRss) {
            std::snprintf(b, sizeof b, "%zu MB", memRss >> 20);
            rows.push_back({"MEM", b});
        }
        std::snprintf(b, sizeof b, "%zu MB", gpuvram::bytes() >> 20);
        rows.push_back({"VID", b});

        // The block font the mana readout uses (5x7 cells, blockWidth = chars * 6 * px),
        // so the panel matches the HUD it sits in rather than introducing a second face.
        //
        // The size is FIXED -- it tracks the UI SCALE option and nothing else. Resizing
        // the window does not grow or shrink the readout; it only changes how many rows
        // there is room for. 2.2 is the largest that still clears the narrowest the strip
        // ever gets: cmdPanelW never goes below miniSize()+12 (= 180*uiScale + 12), and
        // both that floor and this scale track uiScale, so the budget clears it at every
        // UI SCALE setting rather than only at the default.
        // Budget: 5 label + 1 gap + 7 value characters.
        constexpr int kColBudget = 13;
        const float px = 2.2f * uiScale_;
        const ta::hud::StatsFit fit =
            ta::hud::fitStats(int(rows.size()), availW, availH, kColBudget,
                               /*glyphW=*/6.0f, /*glyphH=*/7.0f, px,
                               // Breathing room between rows. The gap under the minimap
                               // runs to hundreds of pixels while nine rows need barely a
                               // hundred, so there is room to spare -- and this only ever
                               // feeds the HEIGHT fit, never the type size.
                               /*rowPad=*/6.0f * uiScale_);
        if (!fit.visible) return;
        rows.resize(size_t(fit.rows));

        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        const SDL_Color mc{200, 215, 255, 255};   // the mana readout's colour
        float y = top;
        for (const auto& r : rows) {
            blockText(r.label, x0, y, px, mc);
            // Values right-align against the strip edge so the digits form a column and a
            // changing number does not shuffle the ones above it sideways.
            blockText(r.value, x1 - blockWidth(r.value, px), y, px, mc);
            y += fit.rowH;
        }
    }

    void GameView::drawMinimap(int winW, int winH) {
        (void)winW;
        if (!miniTex_) buildMinimap();
        SDL_FRect r = minimapRect(winW, winH);
        // Full-screen: black out the WHOLE world viewport first. Retail's radar
        // rect simply is the viewport, so no world shows around it; ours fits the
        // map's aspect inside, which would leave the terrain peeking through the
        // letterbox margins and read as a window rather than a map.
        SDL_FRect frame = fsRadar_
            ? SDL_FRect{0, 0, float(mapViewW(winW)), float(winH) - barH()}
            : SDL_FRect{r.x - 2, r.y - 2, r.w + 4, r.h + 4};
        SDL_SetRenderDrawColor(ren_, 30, 30, 40, 255);
        SDL_RenderFillRectF(ren_, &frame);
        if (miniTex_) SDL_RenderCopyF(ren_, miniTex_, nullptr, &r);
        if (fogTex_) SDL_RenderCopyF(ren_, fogTex_, nullptr, &r);

        float mapW = float(mapView_.map().blocksX) * 32;
        float mapH = float(mapView_.map().blocksY) * 32;
        auto toMini = [&](float wx, float wz) {
            return SDL_FPoint{r.x + wx / mapW * r.w, r.y + wz / mapH * r.h};
        };
        // All unit dots batched into one draw call (per-unit FillRect + colour
        // set was thousands of state changes a frame at large unit counts).
        overlayBatch_.clear();
        for (const UnitR* _up : front().live) { const UnitR& u = *_up;
            if (!u.alive() || u.embarked() || !u.type) continue;
            // A spectator (noFog_) sees every unit on the radar; a player sees only
            // allied units and enemies currently in view.
            if (!noFog_ && !alliedToLocal(u.player) && !cellVisibleR(u.x, u.z)) continue;
            SDL_FPoint p = toMini(u.x, u.z);
            SDL_Color tc = playerColor(u.player);
            pushQuad(overlayBatch_, p.x - 1.5f, p.y - 1.5f, 3, 3, tc);
        }
        if (!overlayBatch_.empty())
            SDL_RenderGeometry(ren_, nullptr, overlayBatch_.data(),
                               int(overlayBatch_.size()), nullptr, 0);
        // Camera view rectangle.
        float zm = mapView_.zoom();
        SDL_FPoint a = toMini(mapView_.offX(), mapView_.offY());
        SDL_FRect view{a.x, a.y, winW / zm / mapW * r.w,
                       (winH - barH()) / zm / mapH * r.h};
        SDL_SetRenderDrawColor(ren_, 240, 240, 240, 200);
        SDL_RenderDrawRectF(ren_, &view);
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
                           wz - (winH - int(barH())) / zm / 2);
        return true;
    }

    bool GameView::minimapOrder(float mx, float my, int winW, int winH, bool queue) {
        float wx, wz;
        if (selection_.empty() || !minimapToWorld(mx, my, winW, winH, wx, wz)) return false;
        for (int id : selection_) {
            const auto* u = frameUnitP(id);
            if (!u || u->player != localPlayer_) continue;
            ta::net::Command c;
            c.kind = ta::net::Cmd::Move;
            c.unitId = id;
            c.x = wx;
            c.z = wz;
            c.queue = queue ? 1 : 0;
            issue(c);
        }
        if (const auto* u = frameUnitP(selection_.front()); u && u->player == localPlayer_)
            voice(selection_.front(), "move");
        return true;
    }

    void GameView::issueArmedOrder(char cmd, float wx, float wz, bool queue, bool precise) {
        if (selection_.empty()) return;
        if (cmd == 'c') {   // clear/reclaim: reclaim the feature under the cursor
            if (!haveReclaimer()) return;
            int fid = 0; bool fhit = false; float bestF = 1e18f;
            {   // Live read under the lock: one-shot on an armed order (see above).
                std::unique_lock<std::mutex> lk(simMutex_, std::defer_lock);
                if (useSimThread_) lk.lock();
                for (const auto& f : world_.features()) {
                    if (!f.alive) continue;
                    float dx = f.x - wx, dz = f.z - wz, d = dx * dx + dz * dz;
                    float r = 18.0f + 8.0f * float(std::max(f.fx, f.fz));
                    if (d < r * r && d < bestF) { bestF = d; fid = f.id; fhit = true; }
                }
            }
            // Corpses / statues / rubble under the click too (negative id).
            for (const UnitR* _cp : front().live) {
                const UnitR& cu = *_cp;
                if (cu.alive() || !cu.corpsePhase || cu.corpseFeat < 0 || !cu.type) continue;
                if (size_t(cu.corpseFeat) >= world_.featureTypes().size() ||
                    !world_.featureTypes()[size_t(cu.corpseFeat)].reclaimable) continue;
                float dx = cu.x - wx, dz = cu.z - wz, d = dx * dx + dz * dz;
                float r = 18.0f + 8.0f * float(std::max(cu.type->footX, cu.type->footZ));
                if (d < r * r && d < bestF) { bestF = d; fid = -cu.id; fhit = true; }
            }
            if (!fhit) return;
            int builderId = firstReclaimer();
            ta::net::Command c;
            c.kind = ta::net::Cmd::Reclaim;
            c.unitId = builderId;
            c.targetId = fid;
            c.queue = queue ? 1 : 0;
            issue(c);
            voice(builderId, "move");
            return;
        }
        if (cmd == 'r') {   // repair: heal the damaged friendly under the cursor
            int builderId = -1;
            for (int id : selection_) {
                const auto* u = frameUnitP(id);
                if (u && u->type && u->type->isBuilder && u->type->canMove &&
                    u->player == localPlayer_) { builderId = id; break; }
            }
            if (builderId < 0) return;
            int tid = -1; float best = 28.0f * 28.0f;
            for (const UnitR* _up : front().live) { const UnitR& u = *_up;
                if (!u.alive() || u.embarked() || u.id == builderId || !u.type) continue;
                if (!world_.allied(u.player, localPlayer_)) continue;
                if (u.underConstruction || u.hp >= u.type->maxHp) continue;   // only damaged
                float dx = u.x - wx, dz = u.z - wz, d = dx * dx + dz * dz;
                if (d < best) { best = d; tid = u.id; }
            }
            if (tid < 0) return;
            ta::net::Command c;
            c.kind = ta::net::Cmd::Repair;
            c.unitId = builderId;
            c.targetId = tid;
            c.queue = queue ? 1 : 0;
            issue(c);
            voice(builderId, "move");
            return;
        }
        if (cmd == 'u') {   // unload: selected transport(s) sail to (wx,wz), disembark
            bool any = false;
            for (int id : selection_) {
                const auto* u = frameUnitP(id);
                if (!u || !u->type || !u->type->canTransport || u->cargo.empty()) continue;
                ta::net::Command c;
                c.kind = ta::net::Cmd::Unload;
                c.unitId = id;
                c.x = wx;
                c.z = wz;
                issue(c);
                any = true;
            }
            if (any) voice(selection_.front(), "move");
            return;
        }
        if (cmd == 'l') {   // load: the friendly unit under the cursor boards a transport
            int transportId = -1;
            for (int id : selection_) {
                const auto* u = frameUnitP(id);
                if (u && u->type && u->type->canTransport &&
                    int(u->cargo.size()) < u->type->transportCap) { transportId = id; break; }
            }
            const auto* t = transportId >= 0 ? frameUnitP(transportId) : nullptr;
            if (!t) return;
            int pid = -1; float best = 24.0f * 24.0f;
            for (const UnitR* _up : front().live) { const UnitR& u = *_up;
                if (!u.alive() || u.embarked() || u.id == transportId || !u.type) continue;
                if (u.player != t->player || u.type->canTransport) continue;
                float dx = u.x - wx, dz = u.z - wz, d = dx * dx + dz * dz;
                if (d < best) { best = d; pid = u.id; }
            }
            if (pid < 0) return;
            ta::net::Command c;
            c.kind = ta::net::Cmd::Load;
            c.unitId = pid;
            c.targetId = transportId;
            issue(c);
            voice(transportId, "move");
            return;
        }
        if (cmd == 'g') {   // guard: needs a friendly unit
            int buddy = -1;
            float best = precise ? 24.0f : 96.0f;   // generous radius on the minimap
            best *= best;
            for (const UnitR* _up : front().live) { const UnitR& u = *_up;
                if (!u.alive() || u.player != localPlayer_) continue;
                float dx = u.x - wx, dz = u.z - wz, d = dx * dx + dz * dz;
                if (d < best) { best = d; buddy = u.id; }
            }
            if (buddy < 0) return;
            for (int id : selection_) {
                if (id == buddy) continue;
                ta::net::Command c;
                c.kind = ta::net::Cmd::Guard;
                c.unitId = id;
                c.targetId = buddy;
                c.queue = queue ? 1 : 0;
                issue(c);
            }
            voice(selection_.front(), "guard");
            return;
        }
        // 'a' (attack) targets an enemy under a precise click; otherwise (and always
        // on the minimap) it is an attack-move to the ground point.
        int enemy = -1;
        if (cmd == 'a' && precise) {
            const auto* first = frameUnitP(selection_.front());
            float best = 20 * 20;
            for (const UnitR* _up : front().live) { const UnitR& u = *_up;
                if (!u.alive() || u.embarked() || !first ||
                    world_.allied(u.player, first->player))
                    continue;
                float dx = u.x - wx, dz = u.z - wz;
                if (dx * dx + dz * dz < best) { best = dx * dx + dz * dz; enemy = u.id; }
            }
        }
        for (int id : selection_) {
            ta::net::Command c;
            if (enemy >= 0) {
                c.kind = ta::net::Cmd::Attack;
                c.targetId = enemy;
            } else {
                c.kind = (cmd == 'f' || cmd == 'a') ? ta::net::Cmd::AttackMove
                         : cmd == 'p'               ? ta::net::Cmd::Patrol
                                                    : ta::net::Cmd::Move;
                c.x = wx;
                c.z = wz;
            }
            c.unitId = id;
            c.queue = queue ? 1 : 0;
            issue(c);
        }
        voice(selection_.front(),
              (cmd == 'a' || cmd == 'f') ? "attack" : cmd == 'p' ? "patrol" : "move");
    }

    bool GameView::minimapArmedOrder(float mx, float my, int winW, int winH) {
        if (!pendingCmd_) return false;
        float wx, wz;
        if (!minimapToWorld(mx, my, winW, winH, wx, wz)) return false;
        issueArmedOrder(pendingCmd_, wx, wz, (SDL_GetModState() & KMOD_SHIFT) != 0, false);
        pendingCmd_ = 0;
        return true;
    }

    void GameView::loadPanel(const std::string& side) {
        std::string base = "anims/" + side + "ingame";
        try {
            auto pal = ta::gaf::Palette::fromBytes(vread(base + ".pcx"), base + ".pcx");
            for (auto& sq : ta::gaf::load(vread(base + ".gaf"), pal, -1, base + ".gaf")) {
                if (sq.frames.empty()) continue;
                auto& f = sq.frames[0];
                if (sq.name == "AidPanel" || sq.name == "MainPanel") {
                    // panelW_/panelH_ keep the 1x LOGICAL size, which is what the HUD
                    // lays out in -- so the texture being built at 2x is invisible here.
                    panelTex_ = ta::art::makeTexture(ren_, f.rgba, f.width, f.height);
                    panelW_ = f.width;
                    panelH_ = f.height;
                } else if (sq.name == "AidBotPanel" || sq.name == "BottomPanel") {
                    botTex_ = ta::art::makeTexture(ren_, f.rgba, f.width, f.height);
                    botW_ = f.width;
                    botH_ = f.height;
                }
            }
        } catch (const std::exception&) {}
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

    void GameView::loadGui(const std::string& side) {
        for (auto& v : guiTex_)
            for (auto* t : v)
                if (t) gpuvram::destroy(t);
        guiTex_.clear();
        gui_ = {};
        std::string path = "guis/" + side + "ingame.gui";
        try {
            gui_ = ta::gui::parse(vread(path), path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "loadGui: %s: %s\n", path.c_str(), e.what());
            return;
        }
        guiTex_.resize(gui_.gadgets.size());
        for (size_t i = 0; i < gui_.gadgets.size(); ++i) {
            const auto& g = gui_.gadgets[i];
            for (const auto& im : g.imgs)
                guiTex_[i].push_back(loadGuiFrame(im.gaf, im.seq, im.frame));
        }
        if (ta::devEnv("TA_GUIDEBUG")) {
            std::fprintf(stderr, "== %s: %zu gadgets ==\n", path.c_str(),
                         gui_.gadgets.size());
            for (size_t i = 0; i < gui_.gadgets.size(); ++i) {
                const auto& g = gui_.gadgets[i];
                std::fprintf(stderr, "  [%2zu] t%-2d %-18s (%3d,%3d %3dx%3d)", i, g.type,
                             g.name.c_str(), g.x, g.y, g.w, g.h);
                for (size_t k = 0; k < g.imgs.size(); ++k)
                    std::fprintf(stderr, " %s:%s#%d%s", g.imgs[k].gaf.c_str(),
                                 g.imgs[k].seq.c_str(), g.imgs[k].frame,
                                 guiTex_[i][k] ? "" : "(!)");
                std::fprintf(stderr, "\n");
            }
        }
    }

    void GameView::loadOrderButtons() {
        auto grab = [&](const char* gaf, const char* seq, char cmd,
                        const char* label, int f0 = 0, int f1 = 1, int f2 = 2) {
            try {
                std::string pp = "anims/" + std::string(gaf) + ".pcx";
                std::string gp = "anims/" + std::string(gaf) + ".gaf";
                auto pal = ta::gaf::Palette::fromBytes(vread(pp), pp);
                for (auto& sq : ta::gaf::load(vread(gp), pal, -1, gp)) {
                    if (sq.name != seq || sq.frames.size() < 3) continue;
                    OrderBtn b;
                    b.cmd = cmd;
                    b.label = label;
                    int idx[3] = {f0, f1, f2};
                    for (int i = 0; i < 3; ++i) {
                        auto& f = sq.frames[size_t(idx[i])];
                        if (f.width == 0) continue;
                        // b.w/b.h keep the 1x logical size the button lays out in.
                        b.frames[i] = ta::art::makeTexture(ren_, f.rgba, f.width, f.height);
                        b.w = f.width;
                        b.h = f.height;
                    }
                    if (b.frames[0]) orderBtns_.push_back(b);
                }
            } catch (const std::exception&) {}
        };
        grab("actionbuttons", "MoveButton", 'm', "MOVE");
        grab("actionbuttons", "AttackButton", 'a', "ATTACK");
        grab("actionbuttons", "PatrolButton", 'p', "PATROL");
        grab("actionbuttons", "GuardButton", 'g', "GUARD");
        // Fight-move (Keys.TDF LOWER_F) reuses the Attack glyph, tinted.
        grab("actionbuttons", "AttackButton", 'f', "FIGHT-MOVE");
        grab("igcommonbuttons", "StopButton", 0, "STOP", 1, 2, 3);
    }

    SDL_FRect GameView::orderBtnRect(size_t i, int winW) const {
        return {float(winW) - 54, 220 + float(i) * 52, 44, 44};
    }

    void GameView::drawOrderColumn(int winW, int winH) {
        (void)winH;
        if (orderBtns_.empty() || selection_.empty()) return;
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_FRect col{float(winW) - 60, 210, 56,
                      float(orderBtns_.size()) * 52 + 12};
        SDL_SetRenderDrawColor(ren_, 20, 18, 16, 170);
        SDL_RenderFillRectF(ren_, &col);
        SDL_SetRenderDrawColor(ren_, 120, 105, 80, 255);
        SDL_RenderDrawRectF(ren_, &col);
        for (size_t i = 0; i < orderBtns_.size(); ++i) {
            const auto& b = orderBtns_[i];
            SDL_FRect r = orderBtnRect(i, winW);
            bool hot = mouseX_ >= r.x && mouseX_ <= r.x + r.w && mouseY_ >= r.y &&
                       mouseY_ <= r.y + r.h;
            bool armed = b.cmd && pendingCmd_ == b.cmd;
            SDL_Texture* t = armed && b.frames[2] ? b.frames[2]
                             : hot && b.frames[1] ? b.frames[1]
                                                  : b.frames[0];
            // Fight-move reuses the Attack glyph -- tint it so it reads apart from Attack
            // (matches the fight-move mouse cursor). Reset after, as the glyph may be shared.
            if (b.cmd == 'f')
                SDL_SetTextureColorMod(t, kFightMoveTint.r, kFightMoveTint.g, kFightMoveTint.b);
            SDL_RenderCopyF(ren_, t, nullptr, &r);
            if (b.cmd == 'f') SDL_SetTextureColorMod(t, 255, 255, 255);
            if (armed) {
                SDL_SetRenderDrawColor(ren_, 255, 220, 90, 255);
                SDL_RenderDrawRectF(ren_, &r);
            }
            if (hot) {
                float px = 1.8f;
                float tw = blockWidth(b.label, px);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 210);
                SDL_FRect tb{r.x - tw - 14, r.y + 14, tw + 10, 22};
                SDL_RenderFillRectF(ren_, &tb);
                blockText(b.label, r.x - tw - 9, r.y + 18, px, {235, 225, 180, 255});
            }
        }
        drawWeaponButtons(winW, winH);
    }

    const UnitR* GameView::multiWeaponSel() {
        if (selection_.empty()) return nullptr;
        const auto* u = frameUnitP(selection_.front());
        if (u && u->alive() && u->type && u->type->weapons.size() > 1) return u;
        return nullptr;
    }

    void GameView::drawWeaponButtons(int winW, int winH) {
        weaponRects_.clear();
        const auto* u = multiWeaponSel();
        if (!u) return;
        int n = int(u->type->weapons.size());
        const float bw = 26, gap = 4;
        // A row just above the HUD bar, right-aligned to the order column's right
        // edge (the row can be wider than the 56px column, so don't centre it or it
        // runs off the screen edge).
        float y = float(winH) - barH() - bw - 8;
        float row = n * bw + (n - 1) * gap;
        float x0 = float(winW) - 4 - row;
        for (int i = 0; i < n; ++i) {
            SDL_FRect r{x0 + i * (bw + gap), y, bw, bw};
            weaponRects_.push_back(r);
            bool active = u->weaponSlot == i;
            bool hot = mouseX_ >= r.x && mouseX_ <= r.x + r.w && mouseY_ >= r.y &&
                       mouseY_ <= r.y + r.h;
            SDL_SetRenderDrawColor(ren_, active ? 90 : 34, active ? 70 : 30,
                                   active ? 30 : 26, 235);
            SDL_RenderFillRectF(ren_, &r);
            SDL_SetRenderDrawColor(ren_, active ? 255 : (hot ? 200 : 120),
                                   active ? 220 : (hot ? 180 : 105),
                                   active ? 90 : 80, 255);
            SDL_RenderDrawRectF(ren_, &r);
            char lbl[16];
            std::snprintf(lbl, sizeof lbl, "%d", i + 1);
            blockText(lbl, r.x + 8, r.y + 6, 2.0f,
                      active ? SDL_Color{255, 240, 180, 255} : SDL_Color{205, 195, 165, 255});
            if (hot && !u->type->weapons[size_t(i)].name.empty()) {
                const std::string& nm = u->type->weapons[size_t(i)].name;
                float px = 1.6f, tw = blockWidth(nm.c_str(), px);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 210);
                SDL_FRect tb{r.x - tw - 14, r.y + 4, tw + 10, 20};
                SDL_RenderFillRectF(ren_, &tb);
                blockText(nm.c_str(), r.x - tw - 9, r.y + 7, px, {235, 225, 180, 255});
            }
        }
    }

    void GameView::selectWeapon(int slot) {
        for (int id : selection_) {
            const auto* u = frameUnitP(id);
            if (!u || !u->type || int(u->type->weapons.size()) <= slot) continue;
            ta::net::Command c;
            c.kind = ta::net::Cmd::SetWeapon;
            c.unitId = id;
            c.targetId = slot;
            issue(c);
        }
    }

    bool GameView::weaponButtonClick(float mx, float my) {
        for (size_t i = 0; i < weaponRects_.size(); ++i) {
            const auto& r = weaponRects_[i];
            if (mx < r.x || mx > r.x + r.w || my < r.y || my > r.y + r.h) continue;
            selectWeapon(int(i));
            return true;
        }
        return false;
    }

    int GameView::guiIdx(const char* name) const {
        for (size_t i = 0; i < gui_.gadgets.size(); ++i)
            if (gui_.gadgets[i].name == name) return int(i);
        return -1;
    }

    int GameView::guiIdxLeft(const char* name) const {
        int best = -1;
        for (size_t i = 0; i < gui_.gadgets.size(); ++i)
            if (gui_.gadgets[i].name == name &&
                (best < 0 || gui_.gadgets[i].x < gui_.gadgets[size_t(best)].x))
                best = int(i);
        return best;
    }

    int GameView::guiIdxRight(const char* name) const {
        int best = -1;
        for (size_t i = 0; i < gui_.gadgets.size(); ++i)
            if (gui_.gadgets[i].name == name &&
                (best < 0 || gui_.gadgets[i].x > gui_.gadgets[size_t(best)].x))
                best = int(i);
        return best;
    }

    std::vector<std::pair<int, char>> GameView::guiActiveButtons() const {
        std::vector<std::pair<int, char>> out;
        if (gui_.gadgets.empty() || selection_.empty()) return out;
        const UnitR* front = frameUnitP(selection_.front());
        if (!front || !front->type) return out;
        auto add = [&](const char* nm, char c) {
            int i = guiIdx(nm);
            if (i >= 0) out.push_back({i, c});
        };
        bool mobile = front->type->maxVel > 0;        // inverse of isStructure()
        bool armed = !front->type->weapons.empty();
        // The five order slots sit at fixed .gui positions that nothing else uses, so
        // retail keeps them on screen and swaps in the Disabled face when the order
        // doesn't apply (Gadget slot 0). Emitting them as UPPERCASE marks them
        // unavailable: the draw picks the disabled art and they stay unclickable.
        // The context slots below OVERLAP each other (HEAL/LOAD share one rect, CLEAR/
        // UNLOAD another, cloak/power a third), so those must stay hidden, never
        // blanked -- retail hides those the same way.
        add("MOVE", mobile ? 'm' : 'M');
        add("PATROL", mobile ? 'p' : 'P');
        add("GUARD", mobile ? 'g' : 'G');
        add("ATTACK", armed ? 'a' : 'A');
        add("STOP", 's');
        // Context orders: reclaim (a mobile reclaiming builder), and transport
        // load/unload. CLEAR and UNLOAD share a .gui slot (599,213) but a unit is
        // never both a reclaimer and a transport, so only one shows.
        bool builder = false, reclaimer = false;
        for (int id : selection_) {
            const auto* u = frameUnitP(id);
            if (!u || !u->alive() || !u->type || !u->type->isBuilder ||
                !u->type->canMove || u->player != localPlayer_)
                continue;
            builder = true;
            if (u->type->canReclaim) reclaimer = true;
        }
        if (builder) add("HEAL", 'r');       // repair a damaged friendly
        if (reclaimer) add("CLEAR", 'c');
        if (front->type->canTransport) {
            if (int(front->cargo.size()) < front->type->transportCap) add("LOAD", 'l');
            if (!front->cargo.empty()) add("UNLOAD", 'u');
        }
        // Combat stance radio: offensive/defensive/passive. Gated on the FBI's
        // unitstandorders, which is what retail's panel consults -- not on a
        // mobile-and-armed guess that only happens to agree on the shipped data.
        if (mobile && armed && front->type->canSetStance) {
            add("Offensive", 'O');
            add("Defensive", 'D');
            add("Passive", 'H');
        }
        // Cloak toggle (cloakers) OR power on/off (onOffable) -- they share the 303
        // row, and a unit has at most one of the two capabilities.
        if (front->type->canCloak) {
            add("Uncloaked", 'k');
            add("Cloaked", 'K');
        } else if (front->type->onOffable) {
            add("Inactive", 'F');
            add("Active", 'N');
        }
        int nw = int(front->type->weapons.size());
        if (nw > 1) {
            add("PrimaryWeapon", '1');
            add("SecondaryWeapon", '2');
            if (nw > 2) add("SpecialWeapon", '3');
        }
        return out;
    }

    void GameView::renderGui(int winW, int winH) {
        if (gui_.gadgets.empty()) { drawOrderColumn(winW, winH); return; }
        guiBtnRects_.clear();
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

        // Command panel background. ButtonPanel frame 0 is the idle dragon medallion;
        // frame 1 is the button-slot panel shown while a unit is selected (retail swaps
        // the medallion out for the order grid).
        int mi = guiIdx("UnitMenu");
        if (mi >= 0 && !guiTex_[mi].empty()) {
            int pf = !selection_.empty() && guiTex_[mi].size() > 1 && guiTex_[mi][1] ? 1 : 0;
            SDL_Texture* pt = guiTex_[mi][size_t(pf)] ? guiTex_[mi][size_t(pf)]
                                                      : guiTex_[mi][0];
            if (pt) {
                SDL_FRect r = guiCmdRect(gui_.gadgets[mi]);
                SDL_RenderCopyF(ren_, pt, nullptr, &r);
            }
        }

        const UnitR* selFront =
            !selection_.empty() ? frameUnitP(selection_.front()) : nullptr;
        for (auto [idx, cmd] : guiActiveButtons()) {
            const auto& g = gui_.gadgets[idx];
            auto& tex = guiTex_[idx];
            SDL_FRect r = guiCmdRect(g);
            // A disabled slot draws but never takes a click (uppercase = unavailable).
            if (!(cmd == 'M' || cmd == 'P' || cmd == 'G' || cmd == 'A'))
                guiBtnRects_.push_back({r, cmd});
            bool hot = mouseX_ >= r.x && mouseX_ <= r.x + r.w && mouseY_ >= r.y &&
                       mouseY_ <= r.y + r.h;
            bool active = false;
            if (cmd >= '1' && cmd <= '3')
                active = selFront && selFront->weaponSlot == (cmd - '1');
            else if (cmd == 'O') active = selFront && selFront->stance == 0;
            else if (cmd == 'D') active = selFront && selFront->stance == 1;
            else if (cmd == 'H') active = selFront && selFront->stance == 2;
            else if (cmd == 'K') active = selFront && selFront->cloakOn;
            else if (cmd == 'k') active = selFront && !selFront->cloakOn;
            else if (cmd == 'N') active = selFront && selFront->active;
            else if (cmd == 'F') active = selFront && !selFront->active;
            else if (cmd != 's')
                active = pendingCmd_ == cmd;
            // Retail's Gadget image slots are NOT (normal, hover, grey). Its per-class
            // slot-name table reads {"Disabled", "Pressed", "Unpressed"} for both the
            // Button and RadioButton classes, so:
            //   slot 0 = Disabled (the empty socket), 1 = Pressed, 2 = Unpressed (idle).
            // Button's constructor sets state 2, mouse-down sets 1 and mouse-up sets 2
            // back. We had 1 and 2 the other way round for push buttons, so every order
            // button sat permanently in its pushed-in face and "lit up" to its normal
            // one on hover. Toggles were already right: a selected radio shows Pressed.
            //
            // There is deliberately no toggle/push branch below. A toggle wants Pressed
            // while it is the SELECTED option and a push button wants it while ARMED,
            // and `active` already means exactly that for each -- so the list of toggle
            // letters that used to sit here had no reader and is gone.
            // An UPPERCASE order letter is the same order, marked unavailable for this
            // selection: draw the Disabled face and stay unclickable.
            bool disabled = cmd == 'M' || cmd == 'P' || cmd == 'G' || cmd == 'A';
            auto tx = [&](int i) -> SDL_Texture* {
                return i >= 0 && i < int(tex.size()) ? tex[size_t(i)] : nullptr;
            };
            // Weapon slots composite the weapon's own icon (anims/weaponpic) -- the
            // WPrimaryButton GAF is just an empty recess. The pic already includes the
            // frame, so it replaces the slot art.
            if (cmd >= '1' && cmd <= '3' && selFront) {
                int slot = cmd - '1';
                SDL_Texture* wt = slot < int(selFront->type->weapons.size())
                    ? weaponIcon(selFront->type->weapons[size_t(slot)].name, active || hot)
                    : nullptr;
                if (wt) SDL_RenderCopyF(ren_, wt, nullptr, &r);
                else {
                    char n[2] = {cmd, 0};
                    float px = std::max(1.4f, r.h / 18.0f), tw = blockWidth(n, px);
                    blockText(n, r.x + (r.w - tw) * 0.5f, r.y + r.h * 0.28f, px,
                              {200, 190, 160, 255});
                }
            } else {
                // A push button is Pressed while it is armed (waiting for you to
                // click a target) or during the click flash; a toggle is Pressed while
                // it is the selected option. Hover no longer changes the face -- it
                // only raises the tooltip, as in retail.
                bool held = active ||
                            (cmd == guiPressed_ && SDL_GetTicks() - guiPressedMs_ < 120);
                int slot = disabled ? 0 : (held ? 1 : 2);
                SDL_Texture* t = tx(slot);
                if (!t) t = tx(held ? 2 : 1);   // art missing that face: use the other
                if (!t) t = tx(0);
                if (t) SDL_RenderCopyF(ren_, t, nullptr, &r);
                // Armed with no distinct pressed face: fall back to the gold outline.
                if (active && (!t || slot != 1)) {
                    SDL_SetRenderDrawColor(ren_, 255, 220, 90, 255);
                    SDL_RenderDrawRectF(ren_, &r);
                }
            }
            if (hot && !g.cmd.empty()) {
                float px = 1.6f, tw = blockWidth(g.cmd.c_str(), px);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 210);
                SDL_FRect tb{r.x - tw - 14, r.y + r.h * 0.3f, tw + 10, 20};
                SDL_RenderFillRectF(ren_, &tb);
                blockText(g.cmd.c_str(), r.x - tw - 9, r.y + r.h * 0.3f + 3, px,
                          {235, 225, 180, 255});
            }
        }

        // Mana panel at the command-panel foot: the orb is a MANA BULB (its 24 frames
        // are liquid-fill levels, picked by mana fraction); "MANA X/Y" sits in a box
        // above it, with +income to the orb's left and -expenditure to its right.
        int cb = guiIdx("CrystalBall");
        if (cb >= 0) {
            const PlayerR& tm = framePlayer(localPlayer_);
            float cap = std::max(tm.metal.storage, 100.0f);
            SDL_FRect orb = guiCmdRect(gui_.gadgets[cb]);
            if (!guiTex_[cb].empty()) {
                int nf = int(guiTex_[cb].size());
                float frac = std::clamp(tm.metal.cur / cap, 0.0f, 1.0f);
                int fr = std::clamp(int(frac * float(nf - 1) + 0.5f), 0, nf - 1);
                if (guiTex_[cb][size_t(fr)])
                    SDL_RenderCopyF(ren_, guiTex_[cb][size_t(fr)], nullptr, &orb);
            }
            // "MANA" over "X/Y", both centred (H and V) in the panel's black HelpText
            // recess above the orb.
            int pmi = guiIdx("UnitMenu"), hti = guiIdx("HelpText");
            SDL_FRect panel = pmi >= 0 ? guiCmdRect(gui_.gadgets[pmi]) : orb;
            SDL_FRect mbox = hti >= 0 ? guiCmdRect(gui_.gadgets[hti])
                                      : SDL_FRect{panel.x + 6, orb.y - 60, panel.w - 12, 52};
            SDL_SetRenderDrawColor(ren_, 8, 8, 8, 235);
            SDL_RenderFillRectF(ren_, &mbox);
            SDL_SetRenderDrawColor(ren_, 70, 62, 44, 255);
            SDL_RenderDrawRectF(ren_, &mbox);
            char nums[32];
            std::snprintf(nums, sizeof nums, "%d/%d", int(tm.metal.cur), int(cap));
            float px = std::max(1.4f, mbox.h / 20.0f);
            float gap = 3, lineH = 7 * px;
            // Shrink to fit both lines within the recess (H and V).
            while (px > 1.0f && (2 * lineH + gap > mbox.h - 4 ||
                                 blockWidth(nums, px) > mbox.w - 6)) {
                px -= 0.1f; lineH = 7 * px;
            }
            float y0 = mbox.y + (mbox.h - (2 * lineH + gap)) * 0.5f;
            SDL_Color mc{200, 215, 255, 255};
            float w1 = blockWidth("MANA", px), w2 = blockWidth(nums, px);
            blockText("MANA", mbox.x + (mbox.w - w1) * 0.5f, y0, px, mc);
            blockText(nums, mbox.x + (mbox.w - w2) * 0.5f, y0 + lineH + gap, px, mc);
            // +income / -expenditure (conjure + repair drain, computed here) flanking orb.
            float expend = 0;
            for (const UnitR* _up : front().live) { const UnitR& un = *_up;
                if (un.player != localPlayer_ || !un.alive() || !un.type) continue;
                if (un.buildSiteId)
                    if (const auto* st = frameUnitP(un.buildSiteId);
                        st && st->type && st->underConstruction) {
                        float total = st->type->buildTime / std::max(un.type->workerTime, 0.01f);
                        expend += st->type->buildCost / std::max(total, 0.01f);
                    }
                if (un.repairId)
                    if (const auto* t2 = frameUnitP(un.repairId);
                        t2 && t2->type && t2->hp < t2->type->maxHp) {
                        float total = t2->type->buildTime / std::max(un.type->workerTime, 0.01f);
                        expend += t2->type->buildCost / std::max(total, 0.01f);
                    }
            }
            float ipx = std::max(1.5f, orb.h / 24.0f);
            char inb[16], outb[16];
            std::snprintf(inb, sizeof inb, "+%d", int(tm.metal.income + 0.5f));
            std::snprintf(outb, sizeof outb, "-%d", int(expend + 0.5f));
            float iy = orb.y + orb.h * 0.5f - 3.5f * ipx;
            blockText(inb, orb.x - blockWidth(inb, ipx) - 5, iy, ipx, {150, 225, 150, 255});
            blockText(outb, orb.x + orb.w + 5, iy, ipx, {230, 160, 150, 255});
        }
    }

    bool GameView::guiClick(float mx, float my) {
        for (auto& [r, cmd] : guiBtnRects_) {
            if (mx < r.x || mx > r.x + r.w || my < r.y || my > r.y + r.h) continue;
            playClickTone();
            // Retail pushes the button in on mouse-DOWN and releases it on UP, firing
            // the command there. We dispatch on down (changing that would also change
            // how dragging off a button cancels it), so latch a short press flash
            // instead -- the same visible feedback without moving the dispatch.
            guiPressed_ = cmd;
            guiPressedMs_ = SDL_GetTicks();
            if (cmd == 's') {
                for (int id : selection_) {
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Stop;
                    c.unitId = id;
                    issue(c);
                }
            } else if (cmd >= '1' && cmd <= '3') {
                selectWeapon(cmd - '1');
            } else if (cmd == 'O' || cmd == 'D' || cmd == 'H') {
                int st = cmd == 'O' ? 0 : cmd == 'D' ? 1 : 2;
                issuePerUnit(ta::net::Cmd::Stance, st);
            } else if (cmd == 'K' || cmd == 'k') {
                issuePerUnit(ta::net::Cmd::Cloak, cmd == 'K' ? 1 : 0);
            } else if (cmd == 'N' || cmd == 'F') {
                issuePerUnit(ta::net::Cmd::SetActive, cmd == 'N' ? 1 : 0);
            } else {
                pendingCmd_ = cmd;
            }
            return true;
        }
        return false;
    }

    std::vector<std::string> GameView::conjureMenu(const std::string& builderType) const {
        const auto& all = registry_.buildable(builderType);
        if (missionAllowed_.empty()) return all;
        std::vector<std::string> out;
        for (const auto& id : all)
            if (std::find(missionAllowed_.begin(), missionAllowed_.end(), id) != missionAllowed_.end())
                out.push_back(id);
        return out;
    }

    void GameView::drawGauge(const char* name, float frac, SDL_Color c) {
        int gi = guiIdxLeft(name);
        if (gi < 0) return;
        SDL_FRect r = guiBarRect(gui_.gadgets[gi]);
        if (r.h < 5) { r.y -= (5 - r.h); r.h = 5; }   // the retail gauge is 2px -- lift for legibility
        frac = std::clamp(frac, 0.0f, 1.0f);
        SDL_SetRenderDrawColor(ren_, 8, 8, 8, 230);
        SDL_RenderFillRectF(ren_, &r);
        SDL_FRect f{r.x, r.y, r.w * frac, r.h};
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
        SDL_RenderFillRectF(ren_, &f);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 200);
        SDL_RenderDrawRectF(ren_, &r);
    }

    bool GameView::drawGuiInfoBar(int winW, int winH) {
        if (gui_.gadgets.empty()) return false;
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        float barTop = float(winH - barH());
        // Chrome: InfoPanel stretched across the width, EndCap at the left.
        int bi = guiIdx("BottomBar");
        if (bi >= 0 && !guiTex_[bi].empty() && guiTex_[bi][0]) {
            SDL_FRect r{0, barTop, float(winW), float(barH())};
            SDL_RenderCopyF(ren_, guiTex_[bi][0], nullptr, &r);
        } else {
            SDL_SetRenderDrawColor(ren_, 42, 38, 34, 255);
            SDL_FRect bar{0, barTop, float(winW), float(barH())};
            SDL_RenderFillRectF(ren_, &bar);
        }
        int ei = guiIdx("BottomEnd");
        if (ei >= 0 && !guiTex_[ei].empty() && guiTex_[ei][0]) {
            SDL_FRect r = guiBarRect(gui_.gadgets[ei]);
            SDL_RenderCopyF(ren_, guiTex_[ei][0], nullptr, &r);
        }
        // Unit info as a FIXED, always-present group centred in the bar (like retail):
        // UnitInfo1 (selected unit: portrait + name + HP/mana bars), ActionText
        // (status), and UnitInfo2 (the conjured target: name + progress bar). Both info
        // blocks always render -- empty bars when absent -- so the layout never shifts.
        {
            float vs = float(barH()) / 49.0f;
            float groupW = (512.0f - 59.0f) * vs;   // UnitInfo1.x .. UnitInfo2 right edge
            float off = (float(winW) - groupW) * 0.5f - 59.0f * vs;
            auto place = [&](int gi) {
                SDL_FRect r = guiBarRect(gui_.gadgets[gi]);
                r.x += off;
                return r;
            };
            auto bar = [&](int gi, float frac, SDL_Color c) {
                if (gi < 0) return;
                SDL_FRect r = place(gi);
                if (r.h < 5) { r.y -= (5 - r.h) * 0.5f; r.h = 5; }
                drawBar(r.x, r.y, r.w, r.h, frac, c);
            };
            const UnitR* u =
                selection_.empty() ? nullptr : frameUnitP(selection_.front());
            if (u && (!u->alive() || !u->type)) u = nullptr;
            float tpx = std::max(2.0f, float(barH()) / 24.0f);

            // The conjured target (site under construction, or the head of a build queue).
            std::string tName; float tProg = 0;
            if (u) {
                if (u->buildSiteId) {
                    if (const auto* s = frameUnitP(u->buildSiteId); s && s->type) {
                        tName = s->type->name;
                        tProg = s->hp / std::max(1.0f, s->type->maxHp);
                    }
                } else if (!u->buildQueue.empty() && u->buildQueue.front()) {
                    tName = u->buildQueue.front()->name;
                    tProg = u->buildProgress / std::max(0.01f, u->buildQueue.front()->buildTime);
                }
            }

            // --- UnitInfo1: selected unit ---
            int ii = guiIdx("UnitImage");
            if (ii >= 0) {
                SDL_FRect pr = place(ii);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 255);
                SDL_RenderFillRectF(ren_, &pr);
                if (u) {
                    SDL_Texture* ic = iconFor(u->type->id);
                    if (!ic) ic = modelIconTex(u->type->id, colorSlot_[localPlayer_ & 7],
                                               u->type->maxVel > 0);
                    if (ic) SDL_RenderCopyF(ren_, ic, nullptr, &pr);
                }
                SDL_SetRenderDrawColor(ren_, 96, 84, 60, 255);
                SDL_RenderDrawRectF(ren_, &pr);
                if (u && u->veteran > 0) {
                    int xi = guiIdxLeft("Experience");
                    int tier = u->veteran >= 7 ? 2 : u->veteran >= 4 ? 1 : 0;
                    if (xi >= 0 && tier < int(guiTex_[xi].size()) && guiTex_[xi][size_t(tier)]) {
                        SDL_FRect cr{pr.x + 1, pr.y + pr.h - 22 * vs - 1, 11 * vs, 22 * vs};
                        SDL_RenderCopyF(ren_, guiTex_[xi][size_t(tier)], nullptr, &cr);
                    }
                }
            }
            if (int t1 = guiIdxLeft("UnitText"); u && t1 >= 0) {
                SDL_FRect nr = place(t1);
                blockText(u->type->name, nr.x, nr.y, tpx, {236, 226, 192, 255});
            }
            bar(guiIdxLeft("HealthBar"), u ? u->hp / std::max(1.0f, u->type->maxHp) : 0.0f,
                {210, 70, 60, 255});
            // TA's second bar is not a per-unit pool -- there are no casters. The
            // retail panel shows the unit's SHARE of the player economy here; until
            // the TA HUD lands it stays empty rather than showing a stale mana bar.
            bar(guiIdxLeft("ManaBar"), 0.0f, {90, 150, 255, 255});

            // --- ActionText: status (or +N MORE for a multi-selection) ---
            if (int ai = guiIdx("ActionText"); u && ai >= 0) {
                SDL_FRect ar = place(ai);
                std::string st = selection_.size() > 1
                    ? "+" + std::to_string(selection_.size() - 1) + " MORE"
                    : std::string(unitStatusText(u));
                blockText(st, ar.x, ar.y, tpx, {170, 205, 255, 255});
            }

            // --- UnitInfo2: the conjured target ---
            if (int t2 = guiIdxRight("UnitText"); !tName.empty() && t2 >= 0) {
                SDL_FRect nr = place(t2);
                std::transform(tName.begin(), tName.end(), tName.begin(), ::toupper);
                blockText(tName, nr.x, nr.y, tpx, {236, 226, 192, 255});
            }
            bar(guiIdxRight("HealthBar"), std::clamp(tProg, 0.0f, 1.0f), {210, 70, 60, 255});
            bar(guiIdxRight("ManaBar"), 0.0f, {90, 150, 255, 255});
        }
        return true;
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

    bool GameView::orderColumnClick(float mx, float my, int winW) {
        if (weaponButtonClick(mx, my)) return true;
        if (orderBtns_.empty() || selection_.empty()) return false;
        for (size_t i = 0; i < orderBtns_.size(); ++i) {
            SDL_FRect r = orderBtnRect(i, winW);
            if (mx < r.x || mx > r.x + r.w || my < r.y || my > r.y + r.h) continue;
            const auto& b = orderBtns_[i];
            if (b.cmd) {
                pendingCmd_ = b.cmd;
            } else {
                for (int id : selection_) {
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Stop;
                    c.unitId = id;
                    issue(c);
                }
            }
            return true;
        }
        return false;
    }

    SDL_Texture* GameView::weaponIcon(const std::string& wname, bool selected) {
        std::string base;
        for (char c : wname)
            if (c != ' ') base += char(std::tolower((unsigned char)c));
        std::string key = base + (selected ? "#s" : "#n");
        if (auto it = weaponIcons_.find(key); it != weaponIcons_.end()) return it->second;
        SDL_Texture* tex = nullptr;
        const char* suf = selected ? "sbh" : "sb";
        std::string paths[2] = {"anims/weaponpic/" + base + suf + ".jpg",
                                selected ? "anims/weaponpic/default_selected.jpg"
                                         : "anims/weaponpic/default_up.jpg"};
        for (const auto& path : paths) {
            try {
                auto img = ta::jpeg::load(vread(path));
                tex = ta::art::makeTexture(ren_, img.rgba, img.width, img.height);
                break;
            } catch (const std::exception&) {}
        }
        weaponIcons_[key] = tex;
        return tex;
    }

    void GameView::blockText(const std::string& s, float x, float y, float px, SDL_Color c) {
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
        float cx = x;
        for (char ch : s) {
            char u = char(std::toupper((unsigned char)ch));
            const uint8_t* cols = glyph5x7(u);
            if (!cols) { cx += 4 * px; continue; }   // space / unknown
            for (int col = 0; col < 5; ++col)
                for (int row = 0; row < 7; ++row)
                    if (cols[col] & (1 << row)) {
                        SDL_FRect r{cx + col * px, y + row * px, px, px};
                        SDL_RenderFillRectF(ren_, &r);
                    }
            cx += 6 * px;
        }
    }

    void GameView::colorSwatch(float x, float y, float s, int color, std::function<void()> action) {
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
        std::string sd[ta::sim::kMaxPlayers];
        int np = frameNumPlayers();
        for (const UnitR* _up : front().live) { const UnitR& u = *_up;
            if (!u.alive() || !u.type) continue;
            int t = u.player;
            if (t < 0 || t >= np) continue;
            ++cnt[t];
            if (sd[t].empty()) sd[t] = u.type->side;
        }
        // In a net game (or replay) this is a full scoreboard: every player is
        // listed with their name, team, and defeat status. Offline it stays the
        // compact "who has units" readout.
        bool board = mp_ || replayMode_;
        // Are there real alliances (a team with 2+ members)? If so, show a team tag.
        bool teams = false;
        { int tc[ta::sim::kMaxPlayers] = {};
          for (int t = 0; t < np; ++t) tc[framePlayer(t).team % ta::sim::kMaxPlayers]++;
          for (int t = 0; t < ta::sim::kMaxPlayers; ++t) if (tc[t] > 1) teams = true; }
        int rows = 0, totalUnits = 0;
        for (int t = 0; t < np; ++t) { if (board || cnt[t] > 0) ++rows; totalUnits += cnt[t]; }
        // A spectator sees the full economy: an extra MANA column (income) per faction.
        const bool showMana = spectating_;
        const float px = 2.6f, hx = 1.9f;          // row / header font scales (bigger)
        const float lh = 7 * px + 12, x = 14;
        float y = 14;
        const float nameX = x + (teams ? 46 : 0);
        // Wider panel with generous, non-overlapping columns (the old one crammed the
        // speed readout into the MANA header and the names into the numbers). Right-
        // align MANA / UNITS / KILLS; leave the name column plenty of room.
        const float panelW = showMana ? 640.0f : (board || teams) ? 400.0f : 300.0f;
        const float colKills = x + panelW - 70;
        const float colUnits = colKills - 104;
        const float colMana  = colUnits - 176;
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 180);
        // Two header lines (a meta line + the column labels) above the player rows.
        SDL_FRect bg{x - 8, y - 8, panelW, (rows + 2) * lh + 12};
        SDL_RenderFillRectF(ren_, &bg);
        char buf[80];
        // --- meta line: FPS, game speed, and (spectating) the global unit count ---
        std::snprintf(buf, sizeof buf, "FPS %d", int(fps_ + 0.5f));
        blockText(buf, x, y, px, SDL_Color{195, 195, 200, 255});
        if (mp_) {
            // requested game speed vs the ACTUAL speed the sim achieves (they diverge
            // when a client -- or the server, pacing to the slowest -- can't sustain it).
            float req = std::max(1, int(mp_->gameSpeed())) / 10.0f;
            char sb[48];
            std::snprintf(sb, sizeof sb, "SPEED %.1fx  ACTUAL %.1fx", req, actualSpeed_);
            SDL_Color scol = actualSpeed_ < req - 0.3f ? SDL_Color{240, 200, 110, 255}
                                                       : SDL_Color{150, 195, 160, 255};
            blockText(sb, x + blockWidth(buf, px) + 24, y + 4, hx, scol);
        }
        if (showMana) {   // global unit count across every faction, right-aligned
            std::snprintf(buf, sizeof buf, "TOTAL %d", totalUnits);
            blockText(buf, x + panelW - blockWidth(buf, hx) - 4, y + 4, hx,
                      SDL_Color{210, 215, 225, 255});
        }
        y += lh;
        // --- column-header line (left: elapsed clocks in a net game; right: labels) ---
        if (mp_) {
            int gsec = int(netTick_) / 30;   // game time = ticks / 30Hz
            char tb[64];
            if (gameStartMs_) {              // spectating: real (wall) time too
                int rsec = int((SDL_GetTicks64() - gameStartMs_) / 1000);
                std::snprintf(tb, sizeof tb, "GAME %d:%02d  REAL %d:%02d",
                              gsec / 60, gsec % 60, rsec / 60, rsec % 60);
            } else {
                std::snprintf(tb, sizeof tb, "GAME %d:%02d", gsec / 60, gsec % 60);
            }
            blockText(tb, x, y + 3, hx, SDL_Color{175, 180, 190, 255});
        }
        if (showMana) blockText("MANA", colMana, y + 3, hx, SDL_Color{150, 150, 155, 255});
        blockText("UNITS", colUnits, y + 3, hx, SDL_Color{150, 150, 155, 255});
        blockText("KILLS", colKills, y + 3, hx, SDL_Color{150, 150, 155, 255});
        y += lh;
        for (int t = 0; t < np; ++t) {
            if (!board && cnt[t] == 0) continue;
            // A slot nobody ever occupied has never spawned anything -- not even a
            // Monarch, which every real player gets at setup. Those used to be listed
            // on the F4 board as defeated, so an 8-slot game with 3 players showed five
            // phantom opponents marked OUT. Skip them entirely; `built` is the end-of-
            // game spawn counter, so a real player is >= 1 from the first tick and a
            // player who has since been wiped out still shows (correctly) as OUT.
            if (framePlayer(t).built == 0 && cnt[t] == 0) continue;
            bool dead = framePlayer(t).defeated;
            SDL_Color c = playerColor(t);
            if (dead) { c.r /= 2; c.g /= 2; c.b /= 2; }   // dim a knocked-out player
            if (teams) {   // small team tag, e.g. "T2"
                std::snprintf(buf, sizeof buf, "T%d", framePlayer(t).team + 1);
                blockText(buf, x, y, 1.8f, dead ? SDL_Color{110, 110, 115, 255}
                                                : SDL_Color{170, 175, 185, 255});
            }
            // Label: the player's name in a net game, else the faction. A faction
            // can repeat with >2 players, so the offline label carries a P# prefix.
            std::string s;
            if (mp_ && !playerName_[t & 7].empty()) {
                s = playerName_[t & 7];
                if (s.size() > 12) s = s.substr(0, 12);
                if (playerAi_[t & 7]) s = "AI - " + s;   // computer opponents: "AI - <name>"
            } else {
                s = sd[t].empty() ? std::string("--") : sd[t];
                std::transform(s.begin(), s.end(), s.begin(), ::toupper);
                if (np > 2) s = "P" + std::to_string(t + 1) + " " + s;
            }
            blockText(s, nameX, y, px, c);
            if (showMana) {   // current mana + income, e.g. "1234 +18"
                const PlayerR& pl = framePlayer(t);
                std::snprintf(buf, sizeof buf, "%d +%d", int(pl.metal.cur), int(pl.metal.income));
                blockText(buf, colMana, y, hx, c);
            }
            std::snprintf(buf, sizeof buf, "%d", cnt[t]);
            blockText(buf, colUnits, y, px, c);
            std::snprintf(buf, sizeof buf, "%d", framePlayer(t).kills);
            blockText(buf, colKills, y, px, c);
            if (dead) blockText("OUT", nameX + blockWidth(s, px) + 8, y, 1.7f,
                                SDL_Color{210, 90, 70, 255});
            y += lh;
        }
        (void)winW;
    }

    void GameView::drawObjectivesPanel(int winW, int /*winH*/) {
        if (missionObjectives_.empty()) return;
        const float x0 = 12, top = 92;
        if (!showObjectives_) {
            hudFont_.draw(ren_, "[O] OBJECTIVES", x0, top, 1.2f, {160, 168, 186, 210});
            return;
        }
        const float s = 1.4f, lh = 15 * s;
        const float maxW = std::min(360.0f, winW * 0.32f);
        // Word-wrap each objective to the panel width (proportional font -> measure).
        std::vector<std::pair<std::string, bool>> lines;   // (text, isFirstOfObjective)
        for (const std::string& obj : missionObjectives_) {
            std::string line, word;
            bool first = true;
            auto push = [&] {
                if (word.empty()) return;
                std::string cand = line.empty() ? word : line + " " + word;
                if (hudFont_.width(cand, s) <= maxW) line = cand;
                else { lines.push_back({line, first}); first = false; line = word; }
                word.clear();
            };
            for (char c : obj) { if (c == ' ') push(); else word += c; }
            push();
            if (!line.empty()) lines.push_back({line, first});
        }
        float panelW = maxW + 34, panelH = 26 + float(lines.size()) * lh + 22;
        SDL_FRect bg{x0 - 6, top - 6, panelW, panelH};
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren_, 14, 13, 18, 205);
        SDL_RenderFillRectF(ren_, &bg);
        SDL_SetRenderDrawColor(ren_, 96, 84, 60, 235);
        SDL_RenderDrawRectF(ren_, &bg);
        float y = top;
        hudFont_.draw(ren_, "OBJECTIVES", x0, y, 1.35f, {224, 196, 120, 255});
        y += 20;
        for (const auto& [text, isFirst] : lines) {
            if (isFirst) {
                SDL_FRect dot{x0 + 2, y + 4, 4, 4};
                SDL_SetRenderDrawColor(ren_, 210, 180, 90, 255);
                SDL_RenderFillRectF(ren_, &dot);
            }
            hudFont_.draw(ren_, text, x0 + 14, y, s, {206, 212, 228, 255});
            y += lh;
        }
        hudFont_.draw(ren_, "[O] hide", x0, y + 4, 1.1f, {140, 146, 162, 200});
    }

    void GameView::drawPanel(int winW, int winH) {
        // Bottom bar: the retail InfoPanel chrome + unit info when a .gui is loaded,
        // else our own stone strip. The build menu + mana readout below draw on top.
        bool guiBar = drawGuiInfoBar(winW, winH);
        SDL_FRect bar{0, float(winH - barH()), float(winW), float(barH())};
        if (!guiBar) {
            if (botTex_) {
                for (int x = 0; x < winW; x += botW_) {
                    SDL_Rect dst{x, winH - barH(), botW_, barH()};
                    SDL_RenderCopy(ren_, botTex_, nullptr, &dst);
                }
            } else if (panelTex_) {
                for (int x = 0; x < winW; x += panelW_) {
                    SDL_Rect src{0, 40, panelW_, barH()};
                    SDL_Rect dst{x, winH - barH(), panelW_, barH()};
                    SDL_RenderCopy(ren_, panelTex_, &src, &dst);
                }
            } else {
                SDL_SetRenderDrawColor(ren_, 42, 38, 34, 255);
                SDL_RenderFillRectF(ren_, &bar);
            }
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 90);
            SDL_RenderFillRectF(ren_, &bar);
            SDL_SetRenderDrawColor(ren_, 120, 105, 80, 255);
            SDL_RenderDrawLineF(ren_, 0, bar.y, float(winW), bar.y);
        }
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);

        char buf[96];
        SDL_Color fc = factionColor();
        // A solid faction-coloured panel with a dark frame — black text on top.
        auto shade = [&](float x, float w) {
            SDL_FRect z{x, bar.y + 4, w, barH() - 8.0f};
            SDL_SetRenderDrawColor(ren_, fc.r, fc.g, fc.b, 255);
            SDL_RenderFillRectF(ren_, &z);
            SDL_SetRenderDrawColor(ren_, 20, 18, 16, 255);
            SDL_RenderDrawRectF(ren_, &z);
        };

        // Bottom-LEFT: portrait + stats for the selected unit. Skipped when the retail
        // InfoPanel bar already drew the unit info (drawGuiInfoBar).
        if (!guiBar && !selection_.empty() && statFont_.ok()) {
            const auto* u = frameUnitP(selection_.front());
            if (u && u->alive() && u->type) {
                float px = 8;
                shade(px, 288);
                SDL_Texture* ic = iconFor(u->type->id);
                if (ic) {
                    SDL_FRect pr{px + 4, bar.y + 8, 56, barH() - 20.0f};
                    SDL_RenderCopyF(ren_, ic, nullptr, &pr);
                    SDL_SetRenderDrawColor(ren_, 20, 18, 16, 255);
                    SDL_RenderDrawRectF(ren_, &pr);
                }
                float tx = px + 70;
                SDL_Color blk{0, 0, 0, 255};
                blockText(u->type->name, tx, bar.y + 9, 2.6f, blk);
                std::snprintf(buf, sizeof buf, "HP %d/%d", int(u->hp),
                              int(u->type->maxHp));
                blockText(buf, tx, bar.y + 32, 2.3f, blk);
                if (selection_.size() > 1) {
                    std::snprintf(buf, sizeof buf, "+%zu MORE",
                                  selection_.size() - 1);
                    blockText(buf, tx, bar.y + 52, 1.8f, blk);
                }
            }
        }

        // Conjure menu: clickable build icons for the selected builder, in a
        // horizontal row just above the info bar. Where it sits along the bottom is
        // a user preference (Options "BUILD MENU": left / centered / right).
        iconRects_.clear();
        const auto* b = selectedBuilder();
        if (b) {
            const auto menu = conjureMenu(b->type->id);   // mission-filtered
            int n = int(menu.size());
            // Row size: the bar height (already uiScale-scaled) times the user's
            // extra BUILD MENU SCALE, so the row can grow independently of the HUD.
            float bScale = buildBarScale_;   // effective scale after the fit clamp below
            float iconSz = (float(barH()) - 10.0f) * buildBarScale_;
            float gap = 6.0f * buildBarScale_;
            float rowW = n > 0 ? (n - 1) * (iconSz + gap) + iconSz : 0;
            // BUILD MENU SCALE goes to 400%, which is more than a wide menu can spend
            // on a narrow window: 13 icons at 4x is several thousand pixels. Rather
            // than let the row run off the screen (where the icons are unreachable),
            // shrink it to whatever actually fits and keep the requested scale as a
            // ceiling. The same clamp covers the vertical: the row sits above the
            // command bar, so it must not grow past the space over it.
            if (n > 0) {
                const float availW = float(winW) - 20.0f;
                const float availH = std::max(bar.y - 10.0f, 24.0f);
                float fit = 1.0f;
                if (rowW > availW) fit = std::min(fit, availW / rowW);
                if (iconSz > availH) fit = std::min(fit, availH / iconSz);
                if (fit < 1.0f) {
                    iconSz *= fit;
                    gap *= fit;
                    rowW = (n - 1) * (iconSz + gap) + iconSz;
                }
                // Everything drawn INSIDE an icon (the +++ badge, the queue count, the
                // tooltip) is sized from the scale too, so it has to follow the clamp
                // or a shrunken icon gets full-size furniture spilling out of it.
                bScale = buildBarScale_ * fit;
            }
            float x0 = buildBarAlign_ == 1 ? (float(winW) - rowW) / 2.0f
                     : buildBarAlign_ == 2 ? float(winW) - rowW - 10.0f
                                           : 10.0f;
            x0 = std::max(x0, 10.0f);               // a huge menu never runs off-screen left
            float iconY = bar.y - iconSz - 5;       // sit just above the bar
            float x = x0;
            // A recessed container behind the row so the conjure menu reads as one HUD
            // strip (matching the InfoPanel bar's dark inset + bronze frame).
            if (n > 0 && guiBar) {
                SDL_FRect box{x0 - 5, iconY - 5, rowW + 10, iconSz + 10};
                SDL_SetRenderDrawColor(ren_, 14, 12, 10, 225);
                SDL_RenderFillRectF(ren_, &box);
                SDL_SetRenderDrawColor(ren_, 96, 84, 60, 255);
                SDL_RenderDrawRectF(ren_, &box);
            }
            for (int i = 0; i < n; ++i) {
                const auto* bt = registry_.find(menu[size_t(i)]);
                if (!bt) continue;
                SDL_FRect r{x, iconY, iconSz, iconSz};
                SDL_SetRenderDrawColor(ren_, 20, 18, 14, 235);
                SDL_FRect rb{r.x - 1, r.y - 1, r.w + 2, r.h + 2};
                SDL_RenderFillRectF(ren_, &rb);
                SDL_Texture* ic = iconFor(bt->id);
                // isStructure(), not the canmove flag: a Barracks icon should sit
                // square like the building it is, not turned like a soldier.
                if (!ic) ic = modelIconTex(bt->id, colorSlot_[localPlayer_ & 7],
                                           !isStructure(bt));
                if (ic) SDL_RenderCopyF(ren_, ic, nullptr, &r);
                else {
                    SDL_SetRenderDrawColor(ren_, 60, 55, 50, 255);
                    SDL_RenderFillRectF(ren_, &r);
                }
                bool hot = mouseX_ >= r.x && mouseX_ <= r.x + r.w &&
                           mouseY_ >= r.y && mouseY_ <= r.y + r.h;
                SDL_SetRenderDrawColor(ren_, hot ? 255 : 110, hot ? 230 : 100,
                                       hot ? 120 : 70, 255);
                SDL_RenderDrawRectF(ren_, &r);
                // Infinite-build marker: bright +++ over the repeating unit's icon.
                if (b->repeatType == bt) {
                    float px = 2.8f * bScale;   // badges track the row scale
                    float pw = blockWidth("+++", px);
                    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 180);
                    SDL_FRect pb{r.x + (r.w - pw) / 2 - 3, r.y + 4 * bScale,
                                 pw + 6, 22 * bScale};
                    SDL_RenderFillRectF(ren_, &pb);
                    blockText("+++", r.x + (r.w - pw) / 2, r.y + 7 * bScale, px,
                              {120, 255, 130, 255});
                }
                // Queued-count badge (bottom-right of the icon): how many are queued.
                if (int qc = frameQueuedCount(b->id, bt)) {
                    char num[8];
                    std::snprintf(num, sizeof num, "%d", qc);
                    float px = 2.2f * bScale, nw = blockWidth(num, px);
                    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 205);
                    SDL_FRect nb{r.x + r.w - nw - 7 * bScale,
                                 r.y + r.h - 21 * bScale,
                                 nw + 7 * bScale, 20 * bScale};
                    SDL_RenderFillRectF(ren_, &nb);
                    blockText(num, r.x + r.w - nw - 4 * bScale,
                              r.y + r.h - 18 * bScale, px, {255, 235, 140, 255});
                }
                if (hot) {
                    char tip[80];
                    std::snprintf(tip, sizeof tip, "%s  %d MANA", bt->name.c_str(),
                                  int(bt->buildCost));
                    float px = 2.0f * bScale;
                    float tw = blockWidth(tip, px);
                    float tipx = std::clamp(r.x + iconSz / 2 - tw / 2, 6.0f, winW - tw - 6);
                    SDL_SetRenderDrawColor(ren_, 0, 0, 0, 210);
                    SDL_FRect tb{tipx - 6, iconY - 28 * bScale,
                                 tw + 12, 26 * bScale};
                    SDL_RenderFillRectF(ren_, &tb);
                    blockText(tip, tipx, iconY - 24 * bScale, px, {255, 240, 190, 255});
                }
                iconRects_.push_back({r, bt});
                x += iconSz + gap;
            }
            if (!b->buildQueue.empty()) {
                char q[64];
                std::snprintf(q, sizeof q, "TRAINING %s (%zu)",
                              b->buildQueue.front()->name.c_str(),
                              b->buildQueue.size());
                blockText(q, x0, iconY - 24 * bScale, 1.8f * bScale,
                          {160, 210, 255, 255});
            }
        }

        // Bottom-RIGHT: mana -- only on our own bar. The retail GUI bar draws the mana
        // readout at the command-panel foot around the orb (renderGui) instead.
        if (!guiBar) {
            const PlayerR& tm = framePlayer(localPlayer_);
            float manaX = float(winW) - 192;
            shade(manaX - 8, 200);
            SDL_Color txt{0, 0, 0, 255};
            blockText("MANA", manaX, bar.y + 9, 2.0f, txt);
            std::snprintf(buf, sizeof buf, "%d/%d", int(tm.metal.cur),
                          int(std::max(tm.metal.storage, 100.0f)));
            blockText(buf, manaX, bar.y + 30, 2.3f, txt);
            std::snprintf(buf, sizeof buf, "+%d/SEC", int(tm.metal.income));
            blockText(buf, manaX, bar.y + 52, 1.8f, txt);
        }
    }


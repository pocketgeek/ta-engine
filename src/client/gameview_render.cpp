#include "client/nanoframe.h"
#include "client/nanoraster.h"
#include "client/shadowcomposite.h"
#include "util/virtualpath.h"
#include "client/streaming.h"
#include <set>
#include <cstring>
#include <functional>
#include "client/gameview.h"
#include "client/cliprect.h"
#include "client/cursortiming.h"
#include "client/buildghost.h"
#include "client/reclaimpick.h"
#include "client/orderoverlay.h"
#include "client/reclaimfade.h"
#include "client/featuredie.h"
#include "client/tabeamdraw.h"
#include "client/retaileffectframe.h"
#include "client/retailviewport.h"
#include "client/texturefilter.h"
#include "sim/retailheight.h"

// Out-of-line GameView method definitions (render concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    void GameView::draw(int winW, int winH) {
        // Keep the last-known window size current from the draw path too, not just
        // from SDL input events: a headless capture (dummy video driver) gets no
        // events, so without this winW_/winH_ stay 0 and the HUD (command panel,
        // culling) lays out against the wrong size. A real window syncs the same
        // values via input(), so this is a no-op there.
        winW_ = winW;
        winH_ = winH;
        topLeft_ = ta::hudcorner::Column(streamBannerBottom_ > 0 ? streamBannerBottom_ + ta::hudcorner::kGap : 0);
        if (replayMode_) {
            const float top = std::max(8 * uiScale_, topLeft_.next());
            replayBar_ = ta::replaybar::layout(float(mapViewW(winW)), uiScale_, top);
            topLeft_.place(top, replayBar_.box.h);
        }
        // Render telemetry: reset every frame; recording only for debug harnesses.
        // The render study records by default; TA_RENDER_STATS=0 switches it off
        // there, so the telemetry's own cost can be measured with one binary.
        static const bool kRenderStats = ta::devFlag("TA_RENDER_STATS", ta::devFlag("TA_RENDER_STUDY"));
        renderStats_.begin(kRenderStats);
        ta::render::gSubmit = {};
        using RS = ta::render::Counter;
        using RP = ta::render::Phase;
        emoteNowMs_ = SDL_GetTicks64();
        for (int p=0;p<8;++p)
            if (emoteTiming_[p].observe(framePlayer(p).emoteSequence, emoteNowMs_)) {
                discoWas_[p] = headbangWas_[p] = false;
            }
        manageMusic();
        discoSound();     // start the real-time disco track for the selected dancers
        headbangSound();  // ...and the metal track on headbang
        // Campaigns use a private server, but its connection/room handshake is
        // an implementation detail. Keep the loading plate up before GameStarting
        // as well as during world setup; never expose the multiplayer lobby.
        if (!missionStem_.empty() && inLobbyPhase()) {
            if (!loadScreen_) {
                loadScreen_ = std::make_unique<ta::LoadScreen>(ren_, vfs_, missionStem_, settings_);
                loadScreen_->step("PREPARING MISSION", 0);
            }
            loadScreen_->draw();
            if (!netError_.empty()) {
                hudBanner(netError_, 150, 2.0f, {255, 140, 120, 255}, winW);
                hudBanner("ESC TO RETURN", 190, 2.0f, {240, 240, 240, 255}, winW);
            }
            return;
        }
        // The lobby is over under it (startMpGame has closed it already when a game starts).
        if (bigMap_ && !inLobbyPhase()) bigMap_.reset();
        // The loading plate owns the screen from world setup until the first tick,
        // so the wait on the other players isn't a frozen lobby frame. It presents
        // itself, so return before the world is drawn over it.
        if (loadScreen_) {
            if (netTick_ > 0 || replayMode_ || !mp_ || loadScreen_->headless())
                loadScreen_.reset();
            else {
                for (int i = 0; i < 8; ++i)
                    if (mp_->slotLoaded(i)) loadScreen_->setSlotDone(i);
                loadScreen_->draw();   // main owns the present + the AA resolve
                return;
            }
        }
        if (inLobbyPhase()) {
            // The lobby always lays out at its design size (kLobbyW x kLobbyH logical)
            // and is scaled to fit + centred in the window -- so it shows fully at any
            // window size or aspect (never clustered top-left, never clipped). Renders
            // large for legible text; lbHot/lobbyInput undo the scale + centre offset.
            lobbyScale_ = std::min(winW / kLobbyW, winH / kLobbyH);
            float lw = winW / lobbyScale_, lh = winH / lobbyScale_;   // logical window
            lobbyOffX_ = std::floor((lw - kLobbyW) * 0.5f);
            lobbyOffY_ = std::floor((lh - kLobbyH) * 0.5f);
            // Darker surround + a thin frame so the centred lobby reads as a panel.
            // (SDL_RenderClear ignores the viewport, so the ground-clear lives here,
            // not in drawLobby.) Drawn in pixel space (scale 1, no viewport).
            SDL_SetRenderDrawColor(ren_, 8, 9, 13, 255);
            SDL_RenderClear(ren_);
            float px = lobbyOffX_ * lobbyScale_, py = lobbyOffY_ * lobbyScale_;
            float pw = kLobbyW * lobbyScale_, ph = kLobbyH * lobbyScale_;
            SDL_FRect panelBg{px, py, pw, ph};
            SDL_SetRenderDrawColor(ren_, 16, 18, 26, 255);
            SDL_RenderFillRectF(ren_, &panelBg);
            SDL_FRect frame{px - 2, py - 2, pw + 4, ph + 4};
            SDL_SetRenderDrawColor(ren_, 60, 66, 90, 255);
            SDL_RenderDrawRectF(ren_, &frame);
            // Content: scaled + viewport-offset so it draws inside the centred panel.
            SDL_Rect vp{int(lobbyOffX_), int(lobbyOffY_), int(kLobbyW), int(kLobbyH)};
            SDL_RenderSetScale(ren_, lobbyScale_, lobbyScale_);
            SDL_RenderSetViewport(ren_, &vp);
            drawLobby(int(kLobbyW), int(kLobbyH));
            SDL_RenderSetViewport(ren_, nullptr);
            SDL_RenderSetScale(ren_, 1.0f, 1.0f);
            if (bigMap_) drawBigMap(winW, winH);   // over the lobby, which still runs
            drawVoiceIndicator(winW, winH);   // voice works in the room too
            lobbyScreenShown();   // what a press is aimed at from here (lobbyPressStale)
            return;
        }
        // Pull any chat that arrived and age the overlay on a real wall clock, so
        // it fades even while the game is paused or catching up on ticks.
        if (mp_) {
            for (auto& m : mp_->takeChat()) {
                gameChat_.push_back({m.first, m.second, 0, false});
                if (chatEchoMs_ && m.second == chatEcho_) flushSystemChat();
            }
            uint64_t nowMs = SDL_GetTicks64();
            if (chatEchoMs_ && nowMs - chatEchoMs_ > kChatEchoWaitMs) flushSystemChat();
            float cdt = chatLastMs_ ? (nowMs - chatLastMs_) / 1000.0f : 0;
            chatLastMs_ = nowMs;
            if (!chatTyping_) for (auto& g : gameChat_) g.age += cdt;
            if (gameChat_.size() > 16) gameChat_.erase(gameChat_.begin(), gameChat_.end() - 16);
        }
        // Camera shake: nudge the map offset by a decaying oscillation for this
        // frame, so the whole world jolts; the offset is restored at the end so
        // the camera and UI stay put. shakemagnitude ~3 => a few px of jolt.
        float shakeBaseX = mapView_.offX(), shakeBaseY = mapView_.offY();
        // A MakePoster tile is pasted at its unshaken offset (posterStep): no shake.
        bool shaking = !posterFrame_ && shakeTime_ > 0 && shakeDur_ > 0;
        if (shaking) {
            float decay = shakeTime_ / shakeDur_;
            float amp = shakeMag_ * 3.0f * decay;    // pixels
            float dx = amp * std::sin(animClock_ * 91.0f);
            float dz = amp * std::cos(animClock_ * 73.0f);
            float zm = std::max(mapView_.zoom(), 1e-3f);
            mapView_.setOffset(shakeBaseX + dx / zm, shakeBaseY + dz / zm);
        }
        // From here the frame draws the world: fold the frame's telemetry (worker
        // pool, submission counters, terrain workers, memory) when draw() returns.
        struct TelemetryFold {
            GameView& view;
            ~TelemetryFold() { view.pool_.setStats(nullptr); if (view.renderStats_.enabled()) view.foldRenderStats(); }
        } telemetryFold{*this};
        // Workstream J: retained buffers are trimmed after the frame's last use and
        // before the fold above reports them (destroyed first: declared after it).
        struct RetainedTrim {
            GameView& view;
            ~RetainedTrim() {
                try { view.trimRetainedBuffers(); } catch (const std::exception&) { /* keep the buffers */ }
            }
        } retainedTrim{*this};
        if (renderStats_.enabled()) {
            poolStats_ = {};
            pool_.setStats(&poolStats_);
            noteFrameCapacities();
        }
        // Everything world-space (map, units, effects, bars) is clipped to the map
        // viewport so it never bleeds under the right-hand panel.
        int mvw = mapViewW(winW);
        SDL_Rect worldClip{0, 0, mvw, winH};
        SDL_RenderSetClipRect(ren_, &worldClip);
        mapView_.setUnderlay(miniTex_);   // low-res gap filler (null until the overview bakes)
        // The study's fixed clock: section idle trimming (and so what a capped frame has
        // resident) must not follow the build's speed.
        if (ta::devFlag("TA_RENDER_STUDY")) mapView_.setClockMs(uint64_t(animClock_*1000));
        {
            const double _t0 = double(SDL_GetPerformanceCounter());
            ta::render::ScopedPhase terrainPhase(renderStats_, RP::Terrain);
            mapView_.draw(mvw, winH, mapViewH(winH));   // scroll limits: above the bar
            profTerrainMs_ += (double(SDL_GetPerformanceCounter()) - _t0) /
                              (double(SDL_GetPerformanceFrequency()) / 1000.0);
        }
        float zm0 = mapView_.zoom();
        // Tactical Dots: decided once per frame (with hysteresis), so every pass
        // below -- and picking until the next frame -- agrees on dots vs models.
        // The floor is the map-fills-view clamp the wheel handler applies.
        dotsFrame_ = dotsSwitch_.update(tacticalDotsOpt_, tacticalDotsZoom_, zm0,
                                        mapView_.minZoom(mvw, mapViewH(winH)));

        // Painter list: features and units together, sorted by map z. Lives in a
        // member so its capacity survives across frames (it was the last per-frame
        // buffer here still heap-allocated fresh every draw).
        using Item = PaintItem;
        auto& items = paintItems_;
        items.clear();
        {
        ta::render::ScopedPhase candidatePhase(renderStats_, RP::Candidates);
        unitFullScan_ = ta::devFlag("TA_UNIT_FULL_SCAN");
#ifndef NDEBUG
        unitIndexVerify_ = ta::devFlag("TA_UNIT_INDEX_VERIFY");
        if (unitIndexVerify_) ++unitIndexVerifyStats_.frames;
#endif
        if (renderStats_.enabled()) {
            const Frame& shown = front();
            renderStats_.set(RS::UnitsLive, shown.live.size());
            renderStats_.set(RS::UnitGridBytes, shown.unitGrid.capacityBytes());
            if (shown.gen != unitGridShownGen_) {   // first frame showing this snapshot
                unitGridShownGen_ = shown.gen;
                renderStats_.addWorkNs(ta::render::Work::UnitGridBuild, shown.unitGridBuildNs);
            }
        }
        if(featureIndexDirty_) {
            featureIndex_.rebuild(features_.size(),[&](size_t i) {
                const auto& f=features_[i];
                return std::pair{f.x-terrainLiftX(f.x,f.z),f.z-terrainLift(f.x,f.z)};
            });
            featureIndexDirty_=false;
        }
        // Same 200-screen-pixel apron as the exact cull below. One world pixel
        // extra protects its different floating-point evaluation order at edges.
        const auto candidates=ta::devFlag("TA_FEATURE_FULL_SCAN") ? featureIndex_.all() :
            featureIndex_.query(mapView_.offX()-200/zm0-1,mapView_.offY()-200/zm0-1,
                mapView_.offX()+(winW+200)/zm0+1,mapView_.offY()+(winH+200)/zm0+1);
        for (size_t index:candidates) {
            const auto& f=features_[index];
            if (!f.tex) continue;   // burnt away to a stage with no art
            if (!f.aliveVis) continue;   // reclaimed away by a builder (snapshotted)
            if (fogCellR(f.x, f.z) == ta::retailfog::kUnexplored) continue;   // unexplored
            // Cull on the LIFTED position (features lift onto the relief like units).
            float sx = (f.x - mapView_.offX()) * zm0 - terrainLiftX(f.x, f.z) * zm0;
            float sy = (f.z - mapView_.offY()) * zm0 - terrainLift(f.x, f.z) * zm0;
            if (sx < -200 || sy < -200 || sx > winW + 200 || sy > winH + 200) continue;
            // Retail draws every feature whose TDF height is below 10 before any
            // unit (client/hiddenunits.h): the low layer. Taller ones sort with the
            // ground units by depth.
            namespace hu = ta::hiddenunits;
            items.push_back({f.z, nullptr, &f, f.low ? hu::kLayerLowFeature : hu::kLayerGround});
        }
        renderStats_.add(RS::FeatureCandidates, candidates.size());
        renderStats_.add(RS::FeaturesPainted, items.size());
        // The frustum cull (unitpass::painter*, client/unitindex.h); the broad phase
        // queries the band of anchors it can accept.
        namespace unitpass = ta::render::unitpass;
        const ta::render::AnchorView anchorView{mapView_.offX(), mapView_.offY(), zm0, mvw, winH};
        const auto unitCandidate = [&](const UnitR& r) {
            if ((r.deadFor >= float(r.corpseAnimationTicks)/30.0f && !r.corpsePhase) || r.embarked()) return;
            if (r.corpsePhase && r.corpseFeat >= 0) {
                static const bool kCorpLog = ta::devEnv("TA_BURNLOG") != nullptr;
                static float lastLog = -10;
                if (kCorpLog && animClock_ >= lastLog + 2.0f) {
                    lastLog = animClock_;
                    std::fprintf(stderr, "corpse draw: id=%d %s at world %.0f,%.0f screen %.0f,%.0f\n",
                                 r.id, r.type->id.c_str(), r.x, r.z,
                                 (r.x - mapView_.offX()) * zm0, (r.z - mapView_.offY()) * zm0);
                }
            }
            // Unregistered (e.g. a type whose model failed to load): not drawable,
            // and every render path does unitType_.at(u.id) -- skip it here so none
            // of them throw (a throw in the parallel projection aborts the process).
            if (!unitType_.registered(r.id)) return;
            if (!unitShownR(r)) return;   // another side's unit out of sight (LosOn)
            // Frustum cull: only units whose anchor falls in (or just outside) the
            // map viewport are projected and drawn. The margin is generous and
            // asymmetric -- models extend well above their anchor, so a unit above
            // the top edge can still show its lower body. Cull on the LIFTED anchor
            // (where the unit is actually drawn), else a unit lifted onto the screen
            // from just below the edge on high ground vanishes.
            if (!unitpass::painterInView(unitpass::painter(r.x, r.z, uLiftX(r), uLiftY(r), anchorView), anchorView)) return;
            // Airborne units go in the late layer (see PaintItem). A flyer that has
            // LANDED is on the ground and sorts normally -- which is retail's gate
            // too, since its occupancy flips when it puts down.
            int layer = 0;
            if (r.type && r.type->canFly) {
                auto ait = anims_.find(r.id);
                bool aloft = ait == anims_.end() || ait->second.altitude > 1.0f;
                if (aloft) layer = 1;
            }
            items.push_back({r.z, &r, nullptr, layer});
        };
        // Tactical Dots replace every unit model (corpses and dying units included,
        // which the minimap does not show either), so no unit enters the painter
        // list: no projection, no shadow, no body batch.
        if (!dotsFrame_) {
            // Broad phase (workstream L): only the units whose snapshot position can
            // put the lifted anchor inside the cull band are examined, in live order,
            // so the painter items -- and the stable sort's equal-depth order -- are
            // exactly those of a scan over every unit (front().live mirrors
            // world_.units()). Interpolation, model extents and flyer altitude are
            // not inputs of this cull (its margins stand for them); the lift is.
            const auto& live = front().live;
            const auto unitIndices = unitCandidates(unitpass::painterBand(anchorView));
            renderStats_.add(RS::UnitsScanned, unitIndices.size());
            [[maybe_unused]] const size_t indexed = items.size();   // read by the index verify below
            for (uint32_t i : unitIndices) unitCandidate(*live[i]);
#ifndef NDEBUG
            if (unitIndexVerify_) {
                // The same exact test over every unit must accept the same units in
                // the same order with the same keys.
                const size_t mark = items.size();
                for (const UnitR* up : live) unitCandidate(*up);
                const size_t n = mark - indexed;
                bool same = items.size() - mark == n;
                for (size_t k = 0; same && k < n; ++k) {
                    const Item& a = items[indexed + k];
                    const Item& b = items[mark + k];
                    same = a.u == b.u && a.layer == b.layer && std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z);
                }
                if (!same) throw std::runtime_error("unit index verify: painter candidates differ from a full scan");
                for (const UnitR* up : live)
                    if (unitType_.registered(up->id) != (unitType_.count(up->id) != 0))
                        throw std::runtime_error("unit index verify: registration record differs from the registry");
                items.resize(mark);
                ++unitIndexVerifyStats_.passes;
                unitIndexVerifyStats_.live += live.size();
                unitIndexVerifyStats_.candidates += unitIndices.size();
                unitIndexVerifyStats_.accepted += n;
            }
#endif
        }
        renderStats_.add(RS::UnitCandidates, items.size() - renderStats_.get(RS::FeaturesPainted));
        // Shots and effects join the list by depth (PaintItem::fx), after the units
        // and features, so the stable sort draws one with an equal key last. Debug
        // TA_FX_RETAIL_ORDER=1 queues none: every one in the late pass, retail's order.
        collectWorldFx(mvw, winH, !ta::devFlag("TA_FX_RETAIL_ORDER"));
        }
        {
        ta::render::ScopedPhase sortPhase(renderStats_, RP::PainterSort);
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.layer != b.layer) return a.layer < b.layer;   // low features, ground, then air
            // (Equal keys keep insertion order: features, then units, then effects.)
            return a.z < b.z;
        });
        }
        renderStats_.add(RS::PainterItems, items.size());

        // Project every visible unit's model in parallel before drawing. This is
        // the heavy per-frame CPU work (matrix-transforming each unit's model tree
        // and building its vertex buffer); the render thread then only submits the
        // finished geometry, one texture-batched draw call per unit. Without this
        // the whole frame is single-threaded and pegs one core at large unit counts.
        // Each visible unit first resolves the geometry entry IT owns.
        claimGeometryEntries();
#ifndef NDEBUG
        if(ta::devFlag("TA_GEOMETRY_OWNERSHIP_VERIFY"))verifyGeometryOwnership();
#endif
        // Fraction through the current sim-tick interval, for motion interpolation this
        // frame (clamped: a late tick just holds at the newest pose until it arrives).
        // The presentation clock (presentFrame) has already set it from the display time.
        if (!present_.on)
            interpAlpha_ = front().tickDurMs > 0.0f
                ? std::clamp(float(SDL_GetTicks64() - front().tickMs) / front().tickDurMs, 0.0f, 1.0f)
                : 1.0f;
#ifndef NDEBUG
        if(ta::devFlag("TA_RENDER_STUDY"))interpAlpha_=0.5f;
#endif
        // Build the texture atlas for every colour slot in view (main thread; the
        // parallel pass below only reads the finished atlas pointers).
        const double _atl0 = double(SDL_GetPerformanceCounter());
        uint32_t atlasSeen = 0;   // build each in-view colour slot's atlas ONCE, not per unit
        for (const auto* u : visUnits_) {
            int slot = playerColorSlot(u->player);
            uint32_t bit = (slot >= 0 && slot < 32) ? (1u << slot) : 0u;
            if (!bit || !(atlasSeen & bit)) { atlasFor(slot); atlasSeen |= bit; }
        }
        // Workstream M: last frame's palette counters into the session totals (the
        // telemetry fold read them first), then baked or indexed palette sheets.
        paletteTotals_.indexedDraws += paletteIndexedDraws_;
        paletteTotals_.spans += paletteSpans_;
        paletteTotals_.fallbacks += paletteFallbacks_;
        paletteTotals_.switches += paletteSwitches_;
        paletteTotals_.uploadBytes += paletteUploadBytes_;
        paletteUploadBytes_ = paletteIndexedDraws_ = paletteFallbacks_ = paletteSwitches_ = 0;
        paletteSpans_ = paletteSpanCommands_ = 0;
        nativePassTotals_.passes += nativePasses_;
        nativePassTotals_.commands += nativePassCommands_;
        nativePassTotals_.refused += nativePassRefused_;
        nativePassTotals_.verified = nativePass_.verified();
        nativePassTotals_.verifyFailures = nativePass_.verifyFailures();
        nativePasses_ = nativePassCommands_ = nativePassRefused_ = 0;
        updatePaletteMode();
        ++(paletteMode_ == PaletteMode::Indexed ? paletteTotals_.indexedFrames : paletteTotals_.bakedFrames);
        thread_local std::vector<const PieceMeta*> palettePrepared;
        palettePrepared.clear();
        for(const auto* u:visUnits_) {
            if(!isStructure(u->type))continue;
            auto type=unitType_.find(u->id);
            if(type==unitType_.end())continue;
            auto visual=visuals_.find(u->replacementModel ? u->replacementModel->id : type->second);
            if(visual!=visuals_.end() && std::find(palettePrepared.begin(),palettePrepared.end(),
                &visual->second.meta)==palettePrepared.end()) {
                palettePrepared.push_back(&visual->second.meta);
                preparePaletteTextures(visual->second.meta);
            }
        }
        // Retail updates registered model textures every simulation tick,
        // independently of visibility or the construction state of any unit.
        animateGlowTextures();
        profAtlasMs_ += (double(SDL_GetPerformanceCounter()) - _atl0) /
                        (double(SDL_GetPerformanceFrequency()) / 1000.0);
        renderStats_.addMs(RP::AtlasMaintain, (double(SDL_GetPerformanceCounter()) - _atl0) /
                (double(SDL_GetPerformanceFrequency()) / 1000.0));
        profUnits_ += uint64_t(visUnits_.size());   // so PROF ms/frame can be read per unit
        // Sample the shadow toggle HERE, on the main thread, before the workers launch --
        // and BEFORE deriving the draw gate from it. Reading the gate first used last
        // frame's value, so switching shadows on built the geometry and then skipped
        // submitting it for one frame.
        shadowsOnFrame_ = shadowsOn();
        const bool kNoShadow = !shadowsOnFrame_;
        // Resolve the lazy height datum/debug scales on this thread. Worker
        // projection and terrain occlusion then read immutable height state.
        if(!liftReady_)(void)heightAbove(0,0);
        // Distant-image lookup inputs (workstream G), fixed before the pass: each
        // worker asks the cache whether its unit's image will be shown without a
        // repaint, and if so leaves the body (not the shadow) for later. The cache
        // is only read during the pass; prepare() below uses the same clock.
        selSet_.clear();
        selSet_.insert(selection_.begin(),selection_.end());
        distantNow_=ta::devFlag("TA_RENDER_STUDY") ? uint64_t(animClock_*1000) : SDL_GetTicks64();
        SDL_RenderGetScale(ren_,&distantScaleX_,&distantScaleY_);
        distantEarly_=!ta::devFlag("TA_DISTANT_LOOKUP_LATE");
        distantOff_=ta::devFlag("TA_DISTANT_MODELS_OFF");
        double _pt0 = double(SDL_GetPerformanceCounter());
        {
        const auto buildRange = [this](size_t b, size_t e) {
            WorkerScratch& ws = workerScratch();
            for (size_t i = b; i < e; ++i) {
                buildUnitGeom(*visUnits_[i], *visGeom_[i], ws.tris, true);
                ws.note();
            }
        };
        readSchedules();
        if (scheduleUniform_) pool_.parallelFor(visUnits_.size(), buildRange);
        else {
            // Workstream I: chunks of about equal estimated work, from what each
            // entry's previous build redid (UnitGeom::nextCost; cold entries carry
            // a full-build estimate). Only a hint: a misprediction costs balance,
            // never correctness -- every unit still writes only its own entry.
            {
                ta::render::ScopedPhase planPhase(renderStats_, RP::SchedulePlan);
                geomPlan_.begin(schedules_.geometry);
                geomPlan_.addUnits(visCost_, ta::render::GeometryCostModel::kUnitNs);
                geomPlan_.finish(pool_.threads());
            }
            schedThreadsGeom_ += geomPlan_.participants();
            schedEstimateGeomNs_ += geomPlan_.estimateNs();
#ifndef NDEBUG
            if (scheduleVerifyOn_) geomBuildCalls_.store(0, std::memory_order_relaxed);
#endif
            runPlanned('g', geomPlan_, buildRange);
#ifndef NDEBUG
            if (scheduleVerifyOn_) {
                verifyPlan(geomPlan_, visUnits_.size());
                if (geomBuildCalls_.load() != visUnits_.size())
                    throw std::runtime_error("schedule verify: a visible unit was not built exactly once");
                ++scheduleVerify_.frames; ++scheduleVerify_.geometryPasses;
                scheduleVerify_.geometryBuilds += visUnits_.size();
                scheduleVerify_.parallelPasses += geomPlan_.participants() > 1;
            }
#endif
        }
        }
        double _ptFreq = double(SDL_GetPerformanceFrequency()) / 1000.0;
#ifndef NDEBUG
        // Calibration (TA_SCHEDULE_SAMPLES, workstream I): every 15th frame, one
        // line per visible unit -- stages, body stages, key words, body vertices,
        // shadow points (opaque, masked), runs, key and build ns -- to fit
        // GeometryCostModel against (run serially: TA_RENDER_POOL_THREADS=1).
        if (const char* path = ta::devEnv("TA_SCHEDULE_SAMPLES"); path && renderStats_.enabled()) {
            static uint64_t sampleFrame = 0;
            if (++sampleFrame % 15 == 0) if (FILE* f = std::fopen(path, "a")) {
                for (size_t i = 0; i < visUnits_.size(); ++i) {
                    const UnitGeom& g = *visGeom_[i];
                    std::fprintf(f, "G %u %u %zu %zu %zu %zu %zu %u %u\n", unsigned(g.stages), unsigned(g.bodyRun),
                                 g.localKey.size(), g.verts.size(), g.shadowLocal.size(), g.maskedShadowLocal.size(),
                                 g.runs.size(), g.keyNs, g.buildNs);
                }
                std::fclose(f);
            }
        }
#endif
        profProjMs_ += (double(SDL_GetPerformanceCounter()) - _pt0) / _ptFreq;
        if (renderStats_.enabled()) {
            renderStats_.addMs(RP::GeometryPrep, (double(SDL_GetPerformanceCounter()) - _pt0) / _ptFreq);
            tallyRenderStats();
        }
        double _st0 = double(SDL_GetPerformanceCounter());

        // Is this unit drawn whole by drawUnit (needs clip rects / interleaved
        // effects) rather than folded into the shared batches?
        auto special = [&](const UnitR& u, const UnitGeom& g) {
            return specialUnit(u, g.canFly, g.occY, g.ay);
        };

        // Keep construction, clipped models, translucent bodies and special
        // effects on their original path. Tiny ordinary bodies can submit one
        // cached quad; simulation and animation VMs continue at their normal rate.
        distantModelItems_.resize(visUnits_.size());
        bool anyDeferred=false;
        for(size_t i=0;i<visUnits_.size();++i) {
            const auto& u=*visUnits_[i];const auto& g=visGeom(i);
            auto& item=distantModelItems_[i];
            item.id=g.handleKey();item.revision=g.revision;item.x=g.ax;item.y=g.ay;
            item.textureTick=g.animatedTexture?front().gameTick:0;
            item.zoom=zm0;item.source=g.verts;item.runs=g.runs;item.texture=nullptr;
            item.eligible=distantEligible(u,g.canFly,g.occY,g.ay);
            item.bodyDeferred=g.bodyDeferred;
            anyDeferred=anyDeferred || g.bodyDeferred;
        }
        size_t distantCatchup=0;
        {
        ta::render::ScopedPhase distantPhase(renderStats_, RP::DistantPrepare);
#ifndef NDEBUG
        // Debug/tests: sweep (expiry, page release, migration) every frame; run the
        // cache on SDL's software renderer (pages then composite with plain blending).
        distantModelCache_.sweepEveryPrepare=ta::devFlag("TA_DISTANT_SWEEP_EVERY_FRAME");
        distantModelCache_.allowSoftware=ta::devFlag("TA_DISTANT_SOFTWARE");
#endif
        distantModelCache_.prepare(ren_,distantModelItems_,distantNow_,[](void* self,size_t index) {
            static_cast<GameView*>(self)->materializeDistantBody(index);
        },this);
        // A deferred body whose image was not shown after all (the cache returned
        // early: unsupported renderer, invalid output scale) is generated now, before
        // the body pass reads it. Expected never in practice (telemetry counts it).
        for(size_t i=0;anyDeferred && i<visUnits_.size();++i)
            if(distantModelItems_[i].bodyDeferred && !distantModelItems_[i].texture) {
                materializeDistantBody(i);++distantCatchup;
            }
        }
        renderStats_.add(RS::DistantBodyCatchup, distantCatchup);
        renderStats_.add(RS::DistantBodyLate, distantModelCache_.lateBodies);
        renderStats_.add(RS::DistantBoundsReused, distantModelCache_.boundsReused);
        renderStats_.add(RS::DistantPageReleases, distantModelCache_.pageReleases);
        renderStats_.add(RS::DistantMigrated, distantModelCache_.migrated);
        renderStats_.set(RS::DistantPages, distantModelCache_.livePages());
        renderStats_.set(RS::DistantTiles, distantModelCache_.liveTiles());
        renderStats_.set(RS::DistantMetadataBytes, distantModelCache_.metadataBytes());
#ifndef NDEBUG
        if(ta::devFlag("TA_DISTANT_VERIFY"))verifyDistantDeferral(distantCatchup);
#endif
        renderStats_.add(RS::DistantImages, distantModelCache_.used);
        renderStats_.add(RS::DistantBakes, distantModelCache_.refreshed);
        renderStats_.add(RS::TargetSwitches, distantModelCache_.targetSwitches);
        renderStats_.add(RS::DistantHits, distantModelCache_.hits);
        renderStats_.add(RS::DistantHeld, distantModelCache_.held);
        renderStats_.add(RS::DistantMissNew, distantModelCache_.missNew);
        renderStats_.add(RS::DistantMissScale, distantModelCache_.missScale);
        renderStats_.add(RS::DistantMissContent, distantModelCache_.missContent);
        renderStats_.add(RS::DistantRejectBudget, distantModelCache_.rejectBudget);
        renderStats_.add(RS::DistantRejectShape, distantModelCache_.rejectShape);
        renderStats_.add(RS::DistantRejectPages, distantModelCache_.rejectPages);
        static uint64_t distantLogAt=0;
        if(ta::devFlag("TA_DISTANT_STATS") && SDL_GetTicks64()-distantLogAt>=1000) {
            distantLogAt=SDL_GetTicks64();
            size_t reused=0,buildings=0,vertices=0,runs=0,moving=0,walking=0;
            for(size_t i=0;i<visUnits_.size();++i) {
                const auto& g=visGeom(i);reused+=g.reused;buildings+=isStructure(visUnits_[i]->type);
                vertices+=g.verts.size();runs+=g.runs.size();
                moving+=visUnits_[i]->moving();walking+=visUnits_[i]->walking();
            }
            std::fprintf(stderr,"DISTANT models=%zu refreshed=%zu visible=%zu buildings=%zu geometry_reused=%zu verts=%zu runs=%zu moving=%zu walking=%zu\n",
                distantModelCache_.used,distantModelCache_.refreshed,visUnits_.size(),buildings,reused,vertices,runs,moving,walking);
        }

        // Retail-style per-unit silhouette coverage. Each projected
        // unit is rasterized into its own atlas tile, so folds inside one model form
        // one silhouette while later units still darken the scene independently.
        // Bounded targets retain the output render scale; unsupported or exhausted
        // targets fall back to the direct geometry path.
        shadowSilhouetteAtlasActive_ = false;
        size_t shadowAtlasPackedUnits=0,shadowAtlasPackedPages=0,shadowAtlasReusedUnits=0;
        // Telemetry: the frame's tile area incl. padding (what the pages must hold;
        // the units examined before an early failure) and the page limit applied.
        uint64_t shadowDemandPx=0;
        size_t shadowPageLimit=0;
        const char* shadowAtlasFallback="not requested";
#ifndef NDEBUG
        const bool verifyShadowCache=ta::devFlag("TA_SHADOW_CACHE_VERIFY");
#endif
        // Persistent tile cells (workstream F, client/shadowatlas.h). Policy, on the
        // presentation clock: a cell whose unit is not in the atlas is retained for
        // 2 s and 30 frames (and freed first under space pressure); a page that
        // stays empty for 2 s / 60 frames is destroyed; when every cell would fit in
        // half of the other pages for 2 s / 60 frames, up to 64 tiles per frame
        // migrate off the last page; pages shrink after 5 s / 120 frames of demand
        // that fits a page of half the size with the same headroom. A full repack
        // (compaction) runs when a cell cannot be placed and the atlas is
        // fragmented (demand <= 3/4 of capacity), at most once per 60 frames unless
        // nothing else fits -- or for free, when every tile of the frame is
        // redrawn anyway, or >= 90% are and the atlas is loose.
        //
        // Page count (perf follow-up K, client/shadowatlas.h): up to the base pages
        // as before; past them (extended pages, up to maxPages, within three quarters
        // of the GPU cap) only where the base pages cannot hold the frame's tiles --
        // the four-page atlas fell back to direct shadows there -- or are full when a
        // tile needs a cell -- it repacked every tile there. An extended page that
        // empties is kept 8 s / 240 frames (not 2 s), so demand crossing a page
        // boundary back and forth reuses it instead of creating and destroying it;
        // extended pages yield under GPU pressure (the atlas is reset), after which
        // none is added for 10 s / 300 frames.
        struct AtlasPolicyTimes {
            uint64_t retainMs=2000, retainFrames=30, emptyMs=2000, emptyFrames=60;
            uint64_t sparseMs=2000, sparseFrames=60, shrinkMs=5000, shrinkFrames=120, compactCooldown=60;
            uint64_t extendedEmptyMs=8000, extendedEmptyFrames=240, cooldownMs=10000, cooldownFrames=300;
            size_t migratePerFrame=64;
            size_t maxPages=ta::render::kShadowMaxPages;
            int pageMax=2048;
            bool repack=false;
        };
        static const AtlasPolicyTimes atlasTimes=[] {
            AtlasPolicyTimes t;
#ifndef NDEBUG
            // Debug: short windows and small pages, so a short verification run
            // expires, evicts, compacts, migrates, releases and shrinks
            // (render_shadow_atlas*); TA_SHADOW_REPACK runs the former per-frame
            // packer (a reference for capture comparisons, slot for slot: four pages
            // at most, like that packer); TA_SHADOW_PAGES_MAX=N sets the page count
            // limit (4 = the base pages only, the pre-K limit).
            if (const char* v=ta::devEnv("TA_SHADOW_ATLAS_DELAY_MS")) {
                const uint64_t ms=uint64_t(std::max(0,std::atoi(v)));
                t.retainMs=t.emptyMs=t.sparseMs=ms; t.shrinkMs=2*ms;
                t.retainFrames=t.emptyFrames=t.sparseFrames=4; t.shrinkFrames=8; t.compactCooldown=8;
                t.extendedEmptyMs=t.cooldownMs=4*ms; t.extendedEmptyFrames=t.cooldownFrames=16;
            }
            if (const char* v=ta::devEnv("TA_SHADOW_PAGE_MAX")) t.pageMax=std::max(64,std::atoi(v));
            if (const char* v=ta::devEnv("TA_SHADOW_PAGES_MAX")) t.maxPages=size_t(std::clamp(std::atoi(v),1,64));
            t.repack=ta::devFlag("TA_SHADOW_REPACK");
            if (t.repack) t.maxPages=std::min(t.maxPages,ta::render::kShadowBasePages);
#endif
            return t;
        }();
        const size_t basePages=std::min(ta::render::kShadowBasePages,atlasTimes.maxPages);
        const auto gpuBudget=[] {
            return ta::render::ShadowGpuBudget{gpuvram::bytes(),gpuvram::cap(),gpuvram::tightened()};
        };
        const uint64_t atlasNowMs=ta::devFlag("TA_RENDER_STUDY") ? uint64_t(animClock_*1000) : SDL_GetTicks64();
        struct AtlasFrameCounts {
            uint64_t allocs=0,kept=0,freed=0,expired=0,evicted=0,compactions=0,freeCompactions=0,migrated=0,
                     releases=0,shrinks=0,pageAllocs=0,failures=0,sheds=0,extendedReleases=0;
        } atlasCounts;
        auto& atlas=shadowAtlas_;
        using ta::render::RenderHandle;
        const auto fitCellStamps=[&] {
            if (shadowCellStamps_.size()<atlas.indexBound()) {
                shadowCellStamps_.resize(atlas.indexBound());
                shadowCellStampValid_.resize(atlas.indexBound(),0);
            }
        };
        const auto freeCell=[&](RenderHandle h) {
            if (!atlas.release(h)) return false;
            shadowCellStampValid_[h.index]=0;
            ++atlasCounts.freed;
            return true;
        };
        const auto destroyPage=[&](size_t p) {
            spareShadowPage(shadowSilhouettePages_[p].texture,shadowSilhouetteWidth_,shadowSilhouetteHeight_);
            shadowSilhouettePages_[p]={};
            atlas.releasePage(p);
            shadowSilhouettePages_.resize(atlas.pageSlots());
        };
        // Once per drawn frame, before this frame's tiles are placed: expire cells
        // absent too long and destroy pages that stayed empty. With no page left,
        // the next demand chooses the page size from scratch.
        const auto maintainShadowAtlas=[&] {
            auto& pol=shadowAtlasPolicy_;
            ++pol.frame;
            if (atlasTimes.repack) return;   // the former packer kept everything until reset
            // Extended pages yield when the GPU budget comes under pressure: the whole
            // atlas goes (cells and stamps with it) and is rebuilt on base pages only
            // -- or falls back -- until the cooldown ends.
            if (!shadowSparePages_.empty() &&
                (gpuvram::overCap() || ta::render::shadowExtendedPressure(gpuBudget()))) dropShadowSpares();
            if (atlas.livePages()>basePages && ta::render::shadowExtendedPressure(gpuBudget())) {
                resetShadowAtlas(false);
                pol.extendedAfterFrame=pol.frame+atlasTimes.cooldownFrames;
                pol.extendedAfterMs=atlasNowMs+atlasTimes.cooldownMs;
                ++atlasCounts.sheds;
                return;
            }
            std::vector<RenderHandle> expired;
            atlas.forEach([&](RenderHandle h,const ta::render::ShadowAllocation& a) {
                if (pol.frame-a.lastFrame>atlasTimes.retainFrames && atlasNowMs-a.lastMs>atlasTimes.retainMs)
                    expired.push_back(h);
            });
            for (RenderHandle h:expired) if (freeCell(h)) ++atlasCounts.expired;
            for (size_t p=0;p<shadowSilhouettePages_.size();++p) {
                auto& page=shadowSilhouettePages_[p];
                if (!atlas.pageLive(p)) continue;
                if (atlas.pageCells(p)) { page.emptySinceFrame=0; continue; }
                if (!page.emptySinceFrame) { page.emptySinceFrame=pol.frame; page.emptySinceMs=atlasNowMs; continue; }
                // Slots past the base pages hold the extended pages (low slots fill first).
                const bool extended=p>=basePages;
                if (pol.frame-page.emptySinceFrame>=(extended ? atlasTimes.extendedEmptyFrames : atlasTimes.emptyFrames) &&
                    atlasNowMs-page.emptySinceMs>=(extended ? atlasTimes.extendedEmptyMs : atlasTimes.emptyMs)) {
                    destroyPage(p);
                    ++atlasCounts.releases;
                    atlasCounts.extendedReleases+=extended;
                }
            }
            if (!atlas.livePages() && shadowSilhouetteWidth_) resetShadowAtlas();
            atlas.trimHandles();
        };
        const auto buildShadowSilhouetteAtlas = [&](bool force=false) -> bool {
            const auto fail=[&](const char* why) { shadowAtlasFallback=why; return false; };
            // Telemetry: allocation/packing runs until the first page is drawn;
            // then each page alternates transform (prep) and clears + submission
            // (raster). Reference bakes (force) are verification, not timed.
            std::optional<ta::render::ScopedPhase> allocPhase;
            if (!force) allocPhase.emplace(renderStats_, RP::ShadowAlloc);
            if (!force) maintainShadowAtlas();
            SDL_RendererInfo info{};
            // Debug: TA_SHADOW_ATLAS_SOFTWARE also runs the atlas on SDL's software
            // renderer, so tile reuse can be verified without a GPU (ctest).
            if (SDL_GetRendererInfo(ren_, &info) != 0 ||
                !((info.flags & SDL_RENDERER_ACCELERATED) || ta::devFlag("TA_SHADOW_ATLAS_SOFTWARE")) ||
                !SDL_RenderTargetSupported(ren_)) return fail("renderer has no accelerated targets");
            float scaleX=1.0f, scaleY=1.0f;
            SDL_RenderGetScale(ren_, &scaleX, &scaleY);
            if (!(scaleX > 0.0f) || !(scaleY > 0.0f) ||
                !std::isfinite(scaleX) || !std::isfinite(scaleY)) return fail("invalid output scale");
            int pageW=info.max_texture_width>0 ? std::min(atlasTimes.pageMax,info.max_texture_width) : atlasTimes.pageMax;
            int pageH=info.max_texture_height>0 ? std::min(atlasTimes.pageMax,info.max_texture_height) : atlasTimes.pageMax;
            if (pageW < 16 || pageH < 16) return fail("renderer target size too small");
            // Page count (perf follow-up K): a frame starts at the base pages unless the
            // atlas holds extended pages already, and reaches past them where the base
            // pages cannot hold its tiles or are full (below), while extended pages are
            // allowed: not after a pressure shed (cooldown), not while the cap is
            // tightened. Extended pages already held are never more than may be used.
            auto& pol=shadowAtlasPolicy_;
            const bool extendedAllowed=atlasTimes.maxPages>basePages && !gpuvram::tightened() &&
                pol.frame>=pol.extendedAfterFrame && atlasNowMs>=pol.extendedAfterMs;
            const size_t frameMaxPages=extendedAllowed ? atlasTimes.maxPages : basePages;
            if (!force) shadowPageLimit=frameMaxPages;
            size_t pageBytes=size_t(pageW)*size_t(pageH)*4;
            if (!pageBytes || pageBytes > SIZE_MAX/atlasTimes.maxPages) return fail("invalid page allocation size");

            struct Work {
                UnitGeom* geom;
                size_t page;
                int slotX,slotY,pixelX,pixelY,width,height;
                bool dirty=true;
                bool fresh=false;     // placed in a new cell this frame
                size_t opaqueOffset=0,maskBegin=0,maskEnd=0;
                // The unit's persistent cell (workstream F) and the tile decision
                // (the stamps live in shadowTileStamps_/shadowTileBefore_, kept out
                // of the sorted items).
                RenderHandle tile{};
                ta::render::TileVerdict verdict=ta::render::TileVerdict::New;
                bool checkPoints=false;
            };
            std::vector<Work> work;
            work.reserve(items.size());
            for (const auto& item:items) {
                if (!item.u) continue;
                const int slot=geomSlot(item.u->id);
                if (slot<0) continue;
                UnitGeom& g=visGeom(size_t(slot));
                g.shadowAtlasPage=nullptr;
                if (g.shadowVerts.empty() && g.maskedShadowVerts.empty()) continue;
                // Every masked run needs the paired black/original-alpha texture.
                // If a page could not be created, the caller uses the original path
                // for the complete frame instead of mixing partially baked shadows.
                for (const auto& run:g.maskedShadowRuns)
                    if (!run.first || shadowCoverageTextures_.find(run.first)==shadowCoverageTextures_.end())
                        return fail("coverage texture unavailable");
                const auto [minX,minY,maxX,maxY]=g.shadowBounds;
                if (!std::isfinite(minX) || !std::isfinite(minY) ||
                    !std::isfinite(maxX) || !std::isfinite(maxY)) return fail("invalid projected bounds");
                const double leftD=std::floor(double(minX)*scaleX);
                const double topD=std::floor(double(minY)*scaleY);
                const double rightD=std::ceil(double(maxX)*scaleX);
                const double bottomD=std::ceil(double(maxY)*scaleY);
                if (leftD < double(std::numeric_limits<int>::min()/2) || topD < double(std::numeric_limits<int>::min()/2) ||
                    rightD > double(std::numeric_limits<int>::max()/2) || bottomD > double(std::numeric_limits<int>::max()/2))
                    return fail("projected bounds exceed page coordinates");
                if (rightD-leftD>pageW-2 || bottomD-topD>pageH-2)
                    return fail("shadow exceeds bounded page");
                const int pixelX=int(leftD),pixelY=int(topD);
                const int width=std::max(1,int(rightD)-pixelX);
                const int height=std::max(1,int(bottomD)-pixelY);
                if (width+2>pageW || height+2>pageH) return fail("one unit exceeds page size");

                if (!force) shadowDemandPx+=uint64_t(width+2)*uint64_t(height+2);
                work.push_back({&g,0,0,0,pixelX,pixelY,width,height});
            }
            if (work.empty()) return fail("no visible unit shadows");
            // Page size. Small scenes should not clear a 16 MiB target for one
            // commander: pages grow (at once) to hold the largest tile and until the
            // frame's tiles take <= 3/4 of one page -- the former rule. Since F they
            // also shrink, after sustained demand that pages of half the size hold
            // with the same headroom. A size change recreates every page.
            const auto pageDims=[&](uint64_t areaScale) {
                int wW=std::min(pageW,256),wH=std::min(pageH,256);
                uint64_t area=0;
                for (const Work& w:work) {
                    area+=uint64_t(w.width+2)*uint64_t(w.height+2);
                    while (wW<w.width+2) wW=std::min(pageW,wW*2);
                    while (wH<w.height+2) wH=std::min(pageH,wH*2);
                }
                area*=areaScale;
                while (area>uint64_t(wW)*wH*3/4 && (wW<pageW || wH<pageH)) {
                    wW=std::min(pageW,wW*2);wH=std::min(pageH,wH*2);
                }
                return std::pair<int,int>{wW,wH};
            };
            if (!force) {
                const auto [wantW,wantH]=pageDims(1);
                if (wantW>shadowSilhouetteWidth_ || wantH>shadowSilhouetteHeight_) {
                    const int newW=std::max(wantW,shadowSilhouetteWidth_),newH=std::max(wantH,shadowSilhouetteHeight_);
                    resetShadowAtlas();
                    shadowSilhouetteWidth_=newW;shadowSilhouetteHeight_=newH;
                    atlas.reset(newW,newH);
                } else if (!atlasTimes.repack) {
                    const auto [smallW,smallH]=pageDims(2);
                    if (smallW<shadowSilhouetteWidth_ || smallH<shadowSilhouetteHeight_) {
                        if (!pol.shrinkSinceFrame) { pol.shrinkSinceFrame=pol.frame; pol.shrinkSinceMs=atlasNowMs; }
                        else if (pol.frame-pol.shrinkSinceFrame>=atlasTimes.shrinkFrames &&
                                 atlasNowMs-pol.shrinkSinceMs>=atlasTimes.shrinkMs) {
                            const int newW=std::min(smallW,shadowSilhouetteWidth_),newH=std::min(smallH,shadowSilhouetteHeight_);
                            resetShadowAtlas();
                            shadowSilhouetteWidth_=newW;shadowSilhouetteHeight_=newH;
                            atlas.reset(newW,newH);
                            ++atlasCounts.shrinks;
                        }
                    } else pol.shrinkSinceFrame=0;
                }
            }
            pageW=shadowSilhouetteWidth_;pageH=shadowSilhouetteHeight_;
            pageBytes=size_t(pageW)*size_t(pageH)*4;
            const uint64_t pageArea=uint64_t(pageW)*uint64_t(pageH);
            size_t pageLimit=atlas.livePages()>basePages ? std::max(frameMaxPages,atlas.livePages()) : basePages;

            // Cells. Each unit keeps the cell it had while its tile still fits (and
            // the cell is at most 1.5x the area a fresh one would take). Otherwise
            // its old cell is freed and it gets a new one -- exact width, shelf
            // height of its class, best fit -- after the frame's other new tiles,
            // sorted by height. A unit leaving, entering or resizing moves nobody
            // else. The stamp of the unit's previous cell is carried to the new one,
            // so the decision below reads Relocated and never inherits a stranger's
            // pixels.
            const auto cellW=[&](const Work& w){return w.width+2;};
            const auto cellH=[&](const Work& w){return ta::render::shadowCellHeight(w.height+2,pageH);};
            auto& before=shadowTileBefore_;
            auto& hasBefore=shadowTileHasBefore_;
            if (!force) {
                before.resize(work.size());
                hasBefore.assign(work.size(),0);
            }
            const auto carry=[&](size_t wi,RenderHandle h) {
                if (!hasBefore[wi] && atlas.valid(h) && shadowCellStampValid_[h.index]) {
                    before[wi]=shadowCellStamps_[h.index];hasBefore[wi]=1;
                }
            };
            const uint64_t frame=pol.frame;
            const auto bind=[&](size_t wi,RenderHandle h) {
                Work& w=work[wi];
                ta::render::ShadowAllocation* a=atlas.get(h);
                a->ownerKey=w.geom->handleKey();a->entry=w.geom;a->lastFrame=frame;a->lastMs=atlasNowMs;
                w.geom->shadowTile=h;w.tile=h;w.fresh=true;
                fitCellStamps();
                shadowCellStampValid_[h.index]=0;
            };
            // Pages are added to the allocator LOGICALLY while cells are placed; the
            // GPU targets are created only once the whole frame has been placed
            // (realizePages below). A frame that does not fit -- the allocator's own
            // or the former packer's attempt -- therefore creates no texture, and
            // the logical pages it added go with it (dropPendingPages). Otherwise a
            // scene whose tiles fit the page limit by area but not by packing would
            // create pages, empty them on the failed repack, destroy them 2 s later
            // and create them again (VRAM churn while every frame falls back). A
            // pending page is a live allocator page whose texture is still null.
            bool budgetRefused=false;
            size_t pendingPages=0;
            // A base page needs only to fit the cap; an extended page must leave a
            // quarter of it (ta::render::shadowPageFits). Pending pages count as held.
            const auto createPage=[&]() -> bool {
                if (atlas.livePages()>=pageLimit) return false;
                if (gpuAllocBlocked() || !ta::render::shadowPageFits(atlas.livePages(),pendingPages*pageBytes,pageBytes,
                                                                      gpuBudget(),basePages)) {
                    budgetRefused=true; return false;
                }
                const size_t p=size_t(atlas.addPage());
                if (p>=shadowSilhouettePages_.size()) shadowSilhouettePages_.resize(p+1);
                shadowSilhouettePages_[p]={};
                ++pendingPages;
                return true;
            };
            const auto dropPendingPages=[&] {
                for (size_t p=0;p<atlas.pageSlots();++p)
                    if (atlas.pageLive(p) && !shadowSilhouettePages_[p].texture) atlas.releasePage(p);
                shadowSilhouettePages_.resize(atlas.pageSlots());
                pendingPages=0;
            };
            const auto realizePages=[&]() -> bool {
                for (size_t p=0;p<atlas.pageSlots() && pendingPages;++p) {
                    if (!atlas.pageLive(p) || shadowSilhouettePages_[p].texture) continue;
                    if (!atlas.pageCells(p)) { atlas.releasePage(p); --pendingPages; continue; }
                    SDL_Texture* texture=takeShadowSpare(pageW,pageH);
                    if (texture) ++shadowPageReuses_;
                    else {
                        texture=gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,pageW,pageH);
                        if (!texture) { noteGpuAllocFail(); budgetRefused=true; return false; }
                        renderStats_.add(RS::ShadowPageAllocs);
                        ++atlasCounts.pageAllocs;
                    }
                    SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
                    SDL_SetTextureColorMod(texture,0,0,0);
                    SDL_SetTextureAlphaMod(texture,255-kShadowLevel);
                    SDL_SetTextureScaleMode(texture,unitScaleMode());
                    shadowSilhouettePages_[p].texture=texture;
                    --pendingPages;
                }
                shadowSilhouettePages_.resize(atlas.pageSlots());
                return true;
            };
            // Nothing of this frame fits: no cell survives, every stamp goes, and the
            // logical pages added this frame are dropped (no texture was created).
            const auto failAllocation=[&](const char* why) {
                pol.failFrame=frame;
                ++atlasCounts.failures;
                atlas.releaseAll();
                std::fill(shadowCellStampValid_.begin(),shadowCellStampValid_.end(),uint8_t(0));
                for (Work& w:work) w.geom->shadowTile={};
                dropPendingPages();
                return fail(why);
            };
            if (force) {
                // The verification re-bake redraws this frame's tiles in the cells
                // the normal pass gave them; it changes no allocation.
                for (Work& w:work) {
                    const ta::render::ShadowAllocation* a=atlas.get(w.geom->shadowTile);
                    if (!a || a->ownerKey!=w.geom->handleKey() || a->lastFrame!=frame)
                        throw std::logic_error("shadow re-bake: tile without this frame's cell");
                    w.tile=w.geom->shadowTile;
                }
            } else {
                auto& needs=shadowTileNeeds_;
                needs.clear();
                // Tiles that are redrawn this frame wherever they sit: new, other
                // coverage, other size or output scale, other zoom, other sub-pixel
                // phase of the placement (a whole-pixel translation keeps its phase and
                // may still be reused; see sameTilePoints). A wrong guess here costs a
                // redraw, never a stale tile. When all (or nearly all, in a loose
                // atlas) are redrawn anyway, moving them costs nothing: repack.
                size_t dirtyAnyway=0;
                uint64_t tilePx=0;
                for (size_t wi=0;wi<work.size();++wi) {
                    Work& w=work[wi];
                    UnitGeom& g=*w.geom;
                    tilePx+=uint64_t(w.width+2)*uint64_t(w.height+2);
                    ta::render::ShadowAllocation* a=atlas.get(g.shadowTile);
                    if (a && (a->ownerKey!=g.handleKey() || a->entry!=&g)) {
                        // The cell of this entry's previous occupant (generations are
                        // unique, so nobody else can reach it).
                        freeCell(g.shadowTile);a=nullptr;
                    }
                    if (a) carry(wi,g.shadowTile);
                    {
                        using ta::render::sameBits;
                        const ShadowTileStamp& b=before[wi];
                        dirtyAnyway+=!hasBefore[wi] || b.source!=w.geom || b.owner!=g.owner ||
                            b.coverage!=g.shadowRevision || b.width!=w.width || b.height!=w.height ||
                            !sameBits(b.placement.zoom,g.shadowPlacement.zoom) ||
                            !sameBits(b.scaleX,scaleX) || !sameBits(b.scaleY,scaleY) ||
                            !sameBits(b.placement.sx-std::floor(b.placement.sx),
                                      g.shadowPlacement.sx-std::floor(g.shadowPlacement.sx)) ||
                            !sameBits(b.placement.sy-std::floor(b.placement.sy),
                                      g.shadowPlacement.sy-std::floor(g.shadowPlacement.sy));
                    }
                    if (a && !atlasTimes.repack &&
                        ta::render::shadowCellKeeps(a->cell.w,a->cell.h,w.width+2,w.height+2,pageH)) {
                        w.tile=g.shadowTile;a->lastFrame=frame;a->lastMs=atlasNowMs;
                    } else {
                        if (a) freeCell(g.shadowTile);
                        g.shadowTile={};
                        needs.push_back(wi);
                    }
                }
                // More tile area than the frame's page limit holds: no packer fits it
                // (the former one included), so fall back at once instead of trying;
                // the cells left unused expire and their pages go (the former code
                // kept them). (Pages are already at the largest size here: they grow
                // first whenever the frame's tiles exceed 3/4 of one page.) More than
                // the base pages hold -- where the four-page atlas fell back at once --
                // goes straight to the extended pages, when this frame may use them.
                if (tilePx>uint64_t(frameMaxPages)*pageArea) return fail("tile area exceeds page limit");
                if (tilePx>uint64_t(basePages)*pageArea) {
                    // Past the base pages: only as many pages as the GPU budget admits
                    // now (live ones included). A frame they cannot hold falls back at
                    // once, without a packing attempt -- like the four-page atlas did.
                    const auto budget=gpuBudget();
                    const size_t live=atlas.livePages();
                    size_t affordable=live;
                    while (affordable<frameMaxPages && !gpuAllocBlocked() &&
                           ta::render::shadowPageFits(affordable,(affordable-live)*pageBytes,pageBytes,budget,basePages))
                        ++affordable;
                    if (tilePx>uint64_t(affordable)*pageArea) return fail("VRAM budget reached");
                    pageLimit=std::max(pageLimit,frameMaxPages);
                }
                // After a frame whose repack did not fit, go straight to the former
                // packer (its cost, not three attempts) until a frame fits again.
                const bool failedLast=pol.failFrame && pol.failFrame+1==frame;
                bool compact=atlasTimes.repack || failedLast, freeRepack=false, allRedrawn=false;
                if (!compact && work.size()>=16 && dirtyAnyway==work.size()) {
                    // Every tile is redrawn wherever it sits (zoom, a sub-pixel pan, a reset):
                    // the former packer with exact sizes places them in O(1) each, cheaper
                    // than best-fitting every outgrown tile, on exactly the former slots
                    // (same page size: such frames draw the former pixels), and the clears
                    // become one page clear. Nothing kept is lost.
                    compact=freeRepack=allRedrawn=true;
                } else if (!compact && work.size()>=16 && dirtyAnyway*10>=work.size()*9) {
                    // Loose: tiles spread over more pages than a tight pack needs
                    // (former packer ~80% full), or a quarter of the shelf area free.
                    size_t pagesWithCells=0;
                    for (size_t p=0;p<atlas.pageSlots();++p) pagesWithCells+=atlas.pageCells(p)>0;
                    const uint64_t tightPages=(tilePx*5/4+pageArea-1)/pageArea;
                    const auto st=atlas.stats();
                    if (pagesWithCells>tightPages || st.shelfFreePx*4>st.shelfPx) compact=freeRepack=true;
                }
                if (!compact && !needs.empty()) {
                    std::stable_sort(needs.begin(),needs.end(),[&](size_t a,size_t b) {
                        const int ha=cellH(work[a]),hb=cellH(work[b]);
                        return ha!=hb ? ha>hb : cellW(work[a])>cellW(work[b]);
                    });
                    uint64_t demandPx=0;
                    for (const Work& w:work) demandPx+=uint64_t(cellW(w))*uint64_t(cellH(w));
                    bool evicted=false;
                    for (size_t wi:needs) {
                        const Work& w=work[wi];
                        RenderHandle h=atlas.allocate(cellW(w),cellH(w),false);
                        if (!h.valid() && !evicted) {
                            // Space pressure: cells of units not in this frame's atlas go first.
                            evicted=true;
                            std::vector<RenderHandle> absent;
                            atlas.forEach([&](RenderHandle c,const ta::render::ShadowAllocation& a) {
                                if (a.lastFrame!=frame) absent.push_back(c);
                            });
                            for (RenderHandle c:absent) if (freeCell(c)) ++atlasCounts.evicted;
                            h=atlas.allocate(cellW(w),cellH(w),false);
                        }
                        if (!h.valid()) {
                            // Fragmented (the frame's cells need <= 3/4 of the pages):
                            // repack rather than grow, at most once per cooldown.
                            const bool fragmented=demandPx*4<=uint64_t(atlas.livePages())*pageArea*3;
                            if (fragmented && frame-pol.lastCompactFrame>=atlasTimes.compactCooldown) { compact=true; break; }
                            // Full (perf follow-up K): grow past the base pages, where the
                            // four-page atlas could only repack -- every frame a tile did
                            // not fit, so near its limit most frames relocated and redrew
                            // every tile. A fragmented atlas still repacks (below).
                            if (!fragmented) pageLimit=std::max(pageLimit,frameMaxPages);
                            if (createPage()) h=atlas.allocate(cellW(w),cellH(w),false);
                        }
                        if (!h.valid()) { compact=true; break; }
                        bind(wi,h);
                    }
                }
                if (!compact && !atlasTimes.repack) {
                    // Page reclamation: when every cell would fit in half of the other
                    // pages for a while, move a bounded number of tiles per frame off
                    // the last page (into pages below it only); its absent cells go at
                    // once. The emptied page is destroyed by the empty-page rule.
                    size_t last=SIZE_MAX;
                    for (size_t p=0;p<atlas.pageSlots();++p) if (atlas.pageLive(p)) last=p;
                    const size_t live=atlas.livePages();
                    uint64_t cellPx=0;
                    for (size_t p=0;p<atlas.pageSlots();++p) cellPx+=atlas.pageCellPx(p);
                    if (live>=2 && atlas.pageCells(last) && cellPx*2<=uint64_t(live-1)*pageArea) {
                        if (!pol.sparseSinceFrame) { pol.sparseSinceFrame=frame; pol.sparseSinceMs=atlasNowMs; }
                        else if (pol.migratePage<0 && frame-pol.sparseSinceFrame>=atlasTimes.sparseFrames &&
                                 atlasNowMs-pol.sparseSinceMs>=atlasTimes.sparseMs) pol.migratePage=int(last);
                    } else { pol.sparseSinceFrame=0; pol.migratePage=-1; }
                    if (pol.migratePage>=0 && size_t(pol.migratePage)!=last) pol.migratePage=-1;
                    if (pol.migratePage>=0) {
                        const size_t p=size_t(pol.migratePage);
                        std::vector<RenderHandle> absent;
                        atlas.forEach([&](RenderHandle c,const ta::render::ShadowAllocation& a) {
                            if (a.lastFrame!=frame && size_t(a.cell.page)==p) absent.push_back(c);
                        });
                        for (RenderHandle c:absent) if (freeCell(c)) ++atlasCounts.evicted;
                        size_t moved=0;
                        for (size_t wi=0;wi<work.size() && moved<atlasTimes.migratePerFrame;++wi) {
                            Work& w=work[wi];
                            const ta::render::ShadowAllocation* a=atlas.get(w.tile);
                            if (!a || size_t(a->cell.page)!=p) continue;
                            const RenderHandle h=atlas.allocate(cellW(w),cellH(w),false,p);
                            if (!h.valid()) { pol.migratePage=-1; pol.sparseSinceFrame=0; break; }
                            carry(wi,w.tile);
                            freeCell(w.tile);
                            bind(wi,h);
                            ++moved;
                        }
                        atlasCounts.migrated+=moved;
                        if (!atlas.pageCells(p)) pol.migratePage=-1;
                    }
                }
                if (compact) {
                    // Full repack: the former packer (tiles sorted by height, a cursor
                    // per page), first with class-height cells, then with exact sizes --
                    // which places every tile exactly where the former code did, so the
                    // atlas never falls back where it did not. Every tile moves (redrawn;
                    // on a free repack nearly all were redrawn anyway).
                    atlas.forEach([&](RenderHandle c,const ta::render::ShadowAllocation& a) {
                        if (a.lastFrame!=frame) ++atlasCounts.evicted;
                        (void)c;
                    });
                    for (size_t wi=0;wi<work.size();++wi) if (work[wi].tile.valid()) carry(wi,work[wi].tile);
                    atlasCounts.freed+=atlas.liveCells();
                    if (freeRepack) ++atlasCounts.freeCompactions;
                    else if (!atlasTimes.repack) { pol.lastCompactFrame=frame; ++atlasCounts.compactions; }
                    std::vector<size_t> order(work.size());
                    for (size_t i=0;i<order.size();++i) order[i]=i;
                    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {
                        return work[a].height!=work[b].height ? work[a].height>work[b].height : work[a].width>work[b].width;
                    });
                    const auto pass=[&](bool padded) -> bool {
                        atlas.releaseAll();
                        std::fill(shadowCellStampValid_.begin(),shadowCellStampValid_.end(),uint8_t(0));
                        for (Work& w:work) { w.tile={}; w.fresh=false; w.geom->shadowTile={}; }
                        for (size_t wi:order) {
                            const Work& w=work[wi];
                            const int cw=padded ? cellW(w) : w.width+2,ch=padded ? cellH(w) : w.height+2;
                            RenderHandle h=atlas.allocate(cw,ch,true);
                            if (!h.valid() && createPage()) h=atlas.allocate(cw,ch,true);
                            if (!h.valid()) return false;
                            bind(wi,h);
                        }
                        return true;
                    };
                    const auto repackAll=[&] {
                        return (!atlasTimes.repack && !failedLast && !allRedrawn && pass(true)) || pass(false);
                    };
                    bool packed=repackAll();
                    if (!packed && pageLimit<frameMaxPages && !budgetRefused) {
                        // The base pages cannot hold this frame (the four-page atlas fell
                        // back here): the same repack with the extended pages. (After a
                        // budget refusal an extended page, which must leave a quarter of
                        // the cap, would be refused too.)
                        pageLimit=frameMaxPages;
                        packed=repackAll();
                    }
                    if (!packed)
                        return failAllocation(budgetRefused ? "VRAM budget reached" : "page limit reached");
                }
                // Every cell is placed: create the GPU targets of the pages added this
                // frame. (A creation failure is rare -- the budget was checked as pages
                // were added -- and backs off through noteGpuAllocFail.)
                if (!realizePages()) return failAllocation("VRAM budget reached");
                for (const Work& w:work) {
                    if (w.fresh) ++atlasCounts.allocs; else ++atlasCounts.kept;
                }
            }
            std::vector<uint8_t> pageUsed(shadowSilhouettePages_.size(),0);
            for (Work& w:work) {
                const ta::render::ShadowCell& c=atlas.get(w.tile)->cell;
                w.page=size_t(c.page);w.slotX=c.x;w.slotY=c.y;
                pageUsed[w.page]=1;
                ++shadowAtlasPackedUnits;
            }
            size_t usedPages=0;
            for (uint8_t u:pageUsed) usedPages+=u;
            shadowAtlasPackedPages=usedPages;
#ifndef NDEBUG
            if (!force && verifyShadowCache) {
                std::string why;
                if (!atlas.validate(&why)) throw std::logic_error("shadow atlas inconsistent: "+why);
                for (const Work& w:work) {
                    const ta::render::ShadowCell& c=atlas.get(w.tile)->cell;
                    if (w.width+2>c.w || w.height+2>c.h) throw std::logic_error("shadow tile exceeds its cell");
                }
                ++shadowVerify_.validated;
            }
#endif

            // Tile decisions (workstream E, client/shadowtiles.h), before any page is
            // touched: each tile is keyed on its unit's shadow coverage and exact
            // shadow placement, not on the body's geometry revision or screen pixel,
            // and compared with the stamp of the unit's own cell (F). A placement
            // change is kept only if every tile point recomputes bit-identical;
            // those point checks are independent and read only finished geometry, so
            // many of them run on the pool. Decisions do not depend on the order
            // they are made in.
            const bool noCache=ta::devFlag("TA_SHADOW_NOCACHE");
            auto& workStamps=shadowTileStamps_;
            workStamps.resize(work.size());
            {
                auto& checks=shadowTileChecks_;
                checks.clear();
                size_t checkPoints=0;
                for (size_t wi=0;wi<work.size();++wi) {
                    Work& w=work[wi];
                    const UnitGeom& g=*w.geom;
                    auto& stamp=workStamps[wi];
                    stamp.source=w.geom;stamp.owner=g.owner;stamp.coverage=g.shadowRevision;
                    stamp.revision=g.revision;stamp.placement=g.shadowPlacement;
                    stamp.pixelX=w.pixelX;stamp.pixelY=w.pixelY;stamp.page=int(w.page);
                    stamp.slotX=w.slotX;stamp.slotY=w.slotY;
                    stamp.width=w.width;stamp.height=w.height;stamp.scaleX=scaleX;stamp.scaleY=scaleY;
                    if (force || noCache) continue;
                    const ShadowTileStamp* old=hasBefore[wi] ? &before[wi] : nullptr;
                    w.verdict=ta::render::classifyShadowTile(old,stamp,[&] {w.checkPoints=true;return false;});
                    if (w.checkPoints) {
                        checks.push_back(wi);
                        checkPoints+=g.shadowLocal.size()+g.maskedShadowLocal.size();
                    }
                }
                const auto check=[&](size_t begin,size_t end) {
                    for (size_t c=begin;c<end;++c) {
                        Work& w=work[checks[c]];
                        const UnitGeom& g=*w.geom;
                        const ShadowTileStamp& now=workStamps[checks[c]];
                        const ShadowTileStamp& old=before[checks[c]];
                        // Same coverage => same local points; the old tile points are
                        // recomputed from them with the old placement.
                        const bool same=g.shadowVerts.size()==g.shadowLocal.size() &&
                            g.maskedShadowVerts.size()==g.maskedShadowLocal.size() &&
                            ta::render::sameTilePoints(g.shadowLocal,
                                [&](size_t i){return g.shadowVerts[i];},old,now) &&
                            ta::render::sameTilePoints(g.maskedShadowLocal,
                                [&](size_t i){return g.maskedShadowVerts[i].position;},old,now);
                        if (same) w.verdict=ta::render::TileVerdict::ReuseTranslated;
                    }
                };
                if (scheduleUniform_) {
                    if (checkPoints>=32768 && checks.size()>=16) pool_.parallelFor(checks.size(),check);
                    else check(0,checks.size());
                } else if (!checks.empty()) {
                    // Workstream I: planned by points to recompute.
                    using RSch=ta::render::RenderSchedules;
                    {
                        ta::render::ScopedPhase planPhase(renderStats_, RP::SchedulePlan);
                        shadowPlan_.begin(schedules_.shadowCheck);
                        for (size_t c:checks) {
                            const UnitGeom& g=*work[c].geom;
                            shadowPlan_.add(RSch::kShadowCheckPerPointNs*double(g.shadowLocal.size()+g.maskedShadowLocal.size()));
                        }
                        shadowPlan_.finish(pool_.threads());
                    }
                    schedThreadsShadow_+=shadowPlan_.participants();
#ifndef NDEBUG
                    std::vector<ta::render::TileVerdict> pre;
                    if (scheduleVerifyOn_) for (size_t c:checks) pre.push_back(work[c].verdict);
#endif
                    runPlanned('s',shadowPlan_,check);
#ifndef NDEBUG
                    if (scheduleVerifyOn_) {
                        // The planned checks equal a serial re-run from the same
                        // verdicts: kept as translated exactly when the points recompute
                        // bit-identical.
                        verifyPlan(shadowPlan_,checks.size());
                        std::vector<ta::render::TileVerdict> parallel;
                        for (size_t c=0;c<checks.size();++c) {
                            parallel.push_back(work[checks[c]].verdict);
                            work[checks[c]].verdict=pre[c];
                        }
                        check(0,checks.size());
                        for (size_t c=0;c<checks.size();++c)
                            if (work[checks[c]].verdict!=parallel[c])
                                throw std::runtime_error("schedule verify: shadow point check differs from a serial run");
                        scheduleVerify_.checks+=checks.size();
                        scheduleVerify_.parallelPasses+=shadowPlan_.participants()>1;
                    }
#endif
                }
            }

            SDL_Texture* previousTarget=SDL_GetRenderTarget(ren_);
            SDL_Rect previousViewport{};
            SDL_Rect previousClip{};
            float previousScaleX=1.0f,previousScaleY=1.0f;
            SDL_BlendMode previousBlend=SDL_BLENDMODE_NONE;
            Uint8 previousR=0,previousG=0,previousB=0,previousA=255;
            SDL_RenderGetViewport(ren_,&previousViewport);
            const SDL_bool clipWasEnabled=SDL_RenderIsClipEnabled(ren_);
            if (clipWasEnabled) SDL_RenderGetClipRect(ren_,&previousClip);
            SDL_RenderGetScale(ren_,&previousScaleX,&previousScaleY);
            SDL_GetRenderDrawBlendMode(ren_,&previousBlend);
            SDL_GetRenderDrawColor(ren_,&previousR,&previousG,&previousB,&previousA);
            const auto restoreRenderer=[&] {
                SDL_SetRenderTarget(ren_,previousTarget);
                SDL_RenderSetScale(ren_,previousScaleX,previousScaleY);
                SDL_RenderSetViewport(ren_,&previousViewport);
                SDL_RenderSetClipRect(ren_,clipWasEnabled ? &previousClip : nullptr);
                SDL_SetRenderDrawBlendMode(ren_,previousBlend);
                SDL_SetRenderDrawColor(ren_,previousR,previousG,previousB,previousA);
            };

            bool ok=true;
            auto& opaque=shadowAtlasOpaqueScratch_;
            for (size_t p=0;p<pageUsed.size() && ok;++p) {
                if (!pageUsed[p]) continue;
                if(!force && !allocPhase)allocPhase.emplace(renderStats_, RP::ShadowAlloc);
                std::vector<SDL_Rect> dirtyRects;
                for(Work& w:work) if(w.page==p) {
                    const size_t wi=size_t(&w-work.data());
                    const ShadowTileStamp& stamp=workStamps[wi];
                    using ta::render::TileVerdict;
                    const TileVerdict verdict=w.verdict;
                    w.dirty=force || noCache || !ta::render::reuses(verdict);
                    if(!force && renderStats_.enabled()) {
                        if(noCache)renderStats_.add(RS::ShadowDirtyForced);
                        else switch(verdict) {
                            case TileVerdict::ReuseBody: renderStats_.add(RS::ShadowReusedBody); break;
                            case TileVerdict::ReuseTranslated: renderStats_.add(RS::ShadowReusedTranslated); break;
                            case TileVerdict::New: renderStats_.add(RS::ShadowDirtyNew); break;
                            case TileVerdict::Owner: renderStats_.add(RS::ShadowDirtyOwner); break;
                            case TileVerdict::Content: renderStats_.add(RS::ShadowDirtyContent); break;
                            case TileVerdict::Relocated: renderStats_.add(RS::ShadowDirtyRelocated); break;
                            case TileVerdict::Resized: renderStats_.add(RS::ShadowDirtyResized); break;
                            case TileVerdict::Moved: renderStats_.add(RS::ShadowDirtyMoved); break;
                            case TileVerdict::Reuse: break;
                        }
                    }
#ifndef NDEBUG
                    if(!force && !noCache && verifyShadowCache) {
                        // Differential check against the pre-E key: E keeps every tile
                        // the old key kept (and more); the pixel comparison after the
                        // frame proves each kept tile equal to a complete re-bake.
                        const ShadowTileStamp* old=hasBefore[wi] ? &before[wi] : nullptr;
                        const bool original=ta::render::originalRuleReuses(old,stamp);
                        if(original && !ta::render::reuses(verdict))
                            throw std::runtime_error("shadow tile redrawn although the pre-E key kept it");
                        ++shadowVerify_.tiles;shadowVerify_.original+=original;
                        shadowVerify_.reused+=ta::render::reuses(verdict);
                        shadowVerify_.body+=verdict==TileVerdict::ReuseBody;
                        shadowVerify_.translated+=verdict==TileVerdict::ReuseTranslated;
                        shadowVerify_.kept+=ta::render::reuses(verdict) && !w.fresh;
                    }
#endif
                    if(!force) {
                        shadowCellStamps_[w.tile.index]=stamp;
                        shadowCellStampValid_[w.tile.index]=1;
                    }
                    if(!w.dirty)++shadowAtlasReusedUnits;
                    if(w.dirty)dirtyRects.push_back({w.slotX,w.slotY,w.width+2,w.height+2});
                    if(w.dirty && !force) {
                        renderStats_.add(RS::ShadowTilesDirty);
                        renderStats_.add(RS::ShadowDirtyPixels,uint64_t(w.width+2)*uint64_t(w.height+2));
                    }
                }
                if(dirtyRects.empty())continue;
                allocPhase.reset();
                std::optional<ta::render::ScopedPhase> rasterPhase;
                if(!force)rasterPhase.emplace(renderStats_, RP::ShadowRaster);
                if(!force)renderStats_.add(RS::TargetSwitches);
                if (SDL_SetRenderTarget(ren_,shadowSilhouettePages_[p].texture)!=0) {ok=false;break;}
                SDL_RenderSetScale(ren_,1.0f,1.0f);
                SDL_RenderSetViewport(ren_,nullptr);
                SDL_RenderSetClipRect(ren_,nullptr);
                SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(ren_,0,0,0,0);
                // Clear each redrawn tile with its padding: a recycled cell must not
                // keep coverage from its old occupant. Cells never overlap, so the
                // whole page is cleared only when every cell on it is redrawn (a
                // retained cell's pixels must survive).
                if ((dirtyRects.size()==atlas.pageCells(p) ? SDL_RenderClear(ren_) :
                     SDL_RenderFillRects(ren_,dirtyRects.data(),int(dirtyRects.size())))!=0) {ok=false;break;}
                // Tiles on a page never overlap. Batch opaque faces from every
                // unit first, then group cutout faces by their coverage page. This
                // avoids per-unit texture changes without changing unit silhouette
                // coverage or the separate per-unit composite pass.
                noteNeed(Retained::ShadowOpaque,opaque);   // refilled per page (workstream J)
                {
                    size_t maskBytes=0;
                    for (const auto& [texture,vertices]:shadowAtlasMaskScratch_) maskBytes+=vertices.size()*sizeof(SDL_Vertex);
                    noteNeed(Retained::ShadowMask,maskBytes);
                }
                opaque.clear();
                auto& maskBatches=shadowAtlasMaskScratch_;
                for (auto& [texture,vertices]:maskBatches) vertices.clear();
                // Allocate the page once, not once per unit. Source and destination
                // cannot overlap; independent points keep this loop vectorizable
                // without changing the established float operation order.
                size_t pointCount=0;
                for (Work& w:work) if (w.page==p && w.dirty) {
                    w.opaqueOffset=pointCount;pointCount+=w.geom->shadowVerts.size();
                }
                opaque.resize(pointCount);
                struct MaskWork {
                    const SDL_Vertex* source;
                    std::vector<SDL_Vertex>* batch;
                    size_t offset,count;
                };
                std::vector<MaskWork> masks;
                size_t maskPointCount=0;
                for (Work& w:work) if (w.page==p && w.dirty) {
                    w.maskBegin=masks.size();
                    size_t offset=0;
                    for (const auto& run:w.geom->maskedShadowRuns) {
                        auto& batch=maskBatches[shadowCoverageTextures_.at(run.first)];
                        const size_t count=size_t(run.second),oldSize=batch.size();
                        batch.resize(oldSize+count);
                        masks.push_back({w.geom->maskedShadowVerts.data()+offset,&batch,oldSize,count});
                        offset+=count;maskPointCount+=count;
                    }
                    w.maskEnd=masks.size();
                }
                // Resolve textures and allocate batches on the render thread.
                // Map element references survive rehash; workers only access the
                // finished vectors and write disjoint slices in original run order.
                const auto transform=[&](size_t begin,size_t end) {
                    for(size_t wi=begin;wi<end;++wi) {
                        const Work& w=work[wi];
                        if(w.page!=p || !w.dirty)continue;
                        const SDL_FPoint* __restrict src=w.geom->shadowVerts.data();
                        const size_t count=w.geom->shadowVerts.size();
                        SDL_FPoint* __restrict dst=count ? opaque.data()+w.opaqueOffset : nullptr;
                        // The tile transform (ta::render::tilePoint) is also what the
                        // reuse check recomputes.
                        for (size_t i=0;i<count;++i)
                            dst[i]=ta::render::tilePoint(src[i],scaleX,scaleY,w.pixelX,w.pixelY,w.slotX,w.slotY);
                        for(size_t mi=w.maskBegin;mi<w.maskEnd;++mi) {
                            const auto& task=masks[mi];
                            SDL_Vertex* __restrict vertices=task.batch->data()+task.offset;
                            for(size_t i=0;i<task.count;++i) {
                                SDL_Vertex v=task.source[i];
                                v.position=ta::render::tilePoint(v.position,scaleX,scaleY,w.pixelX,w.pixelY,
                                                                  w.slotX,w.slotY);
                                vertices[i]=v;
                            }
                        }
                    }
                };
                // Forced reference bakes stay serial for atlas pixel comparisons.
                rasterPhase.reset();
                {
                std::optional<ta::render::ScopedPhase> prepPhase;
                if(!force)prepPhase.emplace(renderStats_, RP::ShadowPrep);
                if(force || ta::devFlag("TA_SHADOW_SERIAL_PREP")) transform(0,work.size());
                else if(scheduleUniform_) {
                    if(pointCount+maskPointCount>=32768) pool_.parallelFor(work.size(),transform);
                    else transform(0,work.size());
                } else {
                    // Workstream I: only this page's dirty tiles, planned by their
                    // points (opaque positions and masked vertices to transform).
                    using RSch=ta::render::RenderSchedules;
                    auto& items=shadowPrepItems_;
                    std::optional<ta::render::ScopedPhase> planPhase(std::in_place,renderStats_,RP::SchedulePlan);
                    noteNeed(Retained::ShadowPrepItems,items);
                    items.clear();
                    shadowPlan_.begin(schedules_.shadowPrep);
                    for(size_t wi=0;wi<work.size();++wi) {
                        const Work& w=work[wi];
                        if(w.page!=p || !w.dirty)continue;
                        items.push_back(uint32_t(wi));
                        size_t masked=0;
                        for(size_t mi=w.maskBegin;mi<w.maskEnd;++mi)masked+=masks[mi].count;
                        shadowPlan_.add(RSch::kShadowPrepPerPointNs*double(w.geom->shadowVerts.size())+
                                        RSch::kShadowPrepPerMaskedNs*double(masked));
                    }
                    shadowPlan_.finish(pool_.threads());
                    planPhase.reset();
                    schedThreadsShadow_+=shadowPlan_.participants();
                    runPlanned('p',shadowPlan_,[&](size_t b,size_t e) {
                        for(size_t k=b;k<e;++k)transform(items[k],items[k]+1);
                    });
#ifndef NDEBUG
                    if(scheduleVerifyOn_) {
                        // The transform is a pure function of the sources: a serial
                        // re-run over every tile must leave the outputs bit-identical.
                        verifyPlan(shadowPlan_,items.size());
                        const std::vector<SDL_FPoint> parallelOpaque=opaque;
                        std::vector<std::pair<const std::vector<SDL_Vertex>*,std::vector<SDL_Vertex>>> parallelMasks;
                        for(const auto& [texture,vertices]:maskBatches)parallelMasks.push_back({&vertices,vertices});
                        transform(0,work.size());
                        const auto same=[](const auto& a,const auto& b) {
                            return a.size()==b.size() && (a.empty() || !std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0])));
                        };
                        bool ok=same(parallelOpaque,opaque);
                        for(const auto& [now,before]:parallelMasks)ok=ok && same(*now,before);
                        if(!ok)throw std::runtime_error("schedule verify: shadow tile transform differs from a serial run");
                        ++scheduleVerify_.prepPasses;scheduleVerify_.prepPoints+=pointCount+maskPointCount;
                        scheduleVerify_.parallelPasses+=shadowPlan_.participants()>1;
                    }
#endif
                }
                }
                if(!force)rasterPhase.emplace(renderStats_, RP::ShadowRaster);
                SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_NONE);
                if (shadowOpaqueSubmit_.draw(ren_,opaque,
                        !ta::devFlag("TA_SHADOW_SDL_SUBMIT"))!=0) {
                    ok=false;break;
                }
                for (auto& [coverage,vertices]:maskBatches) {
                    if (vertices.empty()) continue;
                    if (bodySubmit_.draw(ren_,coverage,vertices,
                            !ta::devFlag("TA_SHADOW_SDL_SUBMIT"))!=0) {ok=false;break;}
                }
            }
            allocPhase.reset();
            restoreRenderer();
            if(!force)renderStats_.add(RS::TargetSwitches);   // back to the previous target
            if (!ok) {
                // Pages are in an unknown state: nothing retained may be reused.
                std::fill(shadowCellStampValid_.begin(),shadowCellStampValid_.end(),uint8_t(0));
                return fail("target draw operation failed");
            }
            for (const Work& w:work) {
                UnitGeom& g=*w.geom;
                g.shadowAtlasPage=shadowSilhouettePages_[w.page].texture;
                g.shadowAtlasSrc={w.slotX+1,w.slotY+1,w.width,w.height};
                g.shadowAtlasDst={float(w.pixelX)/scaleX,float(w.pixelY)/scaleY,
                                  float(w.width)/scaleX,float(w.height)/scaleY};
            }
            return true;
        };

        // Pass 1: every normal unit's shadow, as a projected silhouette of its model,
        // matching the model projection used by retail's Glide renderer.
        //
        // GROUND units only sit under all bodies. An airborne flyer's shadow is held
        // back and drawn between the ground bodies and the air bodies, so it falls ON
        // what is beneath it, which is where retail's per-unit draw order puts it.
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        const double _sh0 = double(SDL_GetPerformanceCounter());
        if (!kNoShadow && !ta::devEnv("TA_SHADOW_DIRECT")) {
            {
            ta::render::ScopedPhase packPhase(renderStats_, RP::ShadowPack);
            shadowSilhouetteAtlasActive_=buildShadowSilhouetteAtlas();
            }
            renderStats_.add(RS::ShadowUnits, shadowAtlasPackedUnits);
            renderStats_.add(RS::ShadowTilesReused, shadowAtlasReusedUnits);
            renderStats_.add(RS::ShadowPages, shadowAtlasPackedPages);
            renderStats_.add(RS::ShadowFallback, shadowSilhouetteAtlasActive_ ? 0 : 1);
            renderStats_.set(RS::ShadowDemandPx, shadowDemandPx);
            renderStats_.set(RS::ShadowPageLimit, shadowPageLimit);
            if (renderStats_.enabled()) {
                renderStats_.add(RS::ShadowCellAllocs, atlasCounts.allocs);
                renderStats_.add(RS::ShadowCellKept, atlasCounts.kept);
                renderStats_.add(RS::ShadowCellFreed, atlasCounts.freed);
                renderStats_.add(RS::ShadowCellExpired, atlasCounts.expired);
                renderStats_.add(RS::ShadowCellEvicted, atlasCounts.evicted);
                renderStats_.add(RS::ShadowCompactions, atlasCounts.compactions);
                renderStats_.add(RS::ShadowCompactionsFree, atlasCounts.freeCompactions);
                renderStats_.add(RS::ShadowMigrated, atlasCounts.migrated);
                renderStats_.add(RS::ShadowPageReleases, atlasCounts.releases);
                renderStats_.add(RS::ShadowPageShrinks, atlasCounts.shrinks);
                const auto st=atlas.stats();
                uint64_t retained=0;
                atlas.forEach([&](ta::render::RenderHandle,const ta::render::ShadowAllocation& a) {
                    retained+=a.lastFrame!=shadowAtlasPolicy_.frame;
                });
                renderStats_.set(RS::ShadowCells, st.cells);
                renderStats_.set(RS::ShadowCellsRetained, retained);
                renderStats_.set(RS::ShadowAtlasCapacityPx, st.capacityPx);
                renderStats_.set(RS::ShadowAtlasCellPx, st.cellPx);
                uint64_t tilePx=0;
                for (const auto& t:shadowTileStamps_) tilePx+=uint64_t(t.width+2)*uint64_t(t.height+2);
                renderStats_.set(RS::ShadowAtlasTilePx, shadowSilhouetteAtlasActive_ ? tilePx : 0);
                renderStats_.set(RS::ShadowAtlasShelfFreePx, st.shelfFreePx);
                renderStats_.set(RS::ShadowAtlasGpuBytes, st.capacityPx*4);
                renderStats_.set(RS::ShadowExtendedPages, st.pages>basePages ? st.pages-basePages : 0);
                renderStats_.add(RS::ShadowPressureSheds, atlasCounts.sheds);
            }
#ifndef NDEBUG
            shadowVerify_.allocs+=atlasCounts.allocs;
            shadowVerify_.compactions+=atlasCounts.compactions+atlasCounts.freeCompactions;
            shadowVerify_.migrated+=atlasCounts.migrated;shadowVerify_.evicted+=atlasCounts.evicted;
            shadowVerify_.pageReleases+=atlasCounts.releases;shadowVerify_.shrinks+=atlasCounts.shrinks;
            shadowVerify_.pageAllocs+=atlasCounts.pageAllocs;shadowVerify_.failures+=atlasCounts.failures;
            // Perf follow-up K: frames drawn with extended pages, the most pages held,
            // pressure sheds, and frames with shadow tiles that fell back to direct
            // shadows (any reason).
            shadowVerify_.extendedFrames+=shadowSilhouetteAtlasActive_ && atlas.livePages()>basePages;
            shadowVerify_.maxPages=std::max<uint64_t>(shadowVerify_.maxPages,atlas.livePages());
            shadowVerify_.sheds+=atlasCounts.sheds;shadowVerify_.extendedReleases+=atlasCounts.extendedReleases;
            shadowVerify_.fallbacks+=!shadowSilhouetteAtlasActive_ && shadowDemandPx>0;
            if (atlasCounts.sheds) { shadowVerify_.lastShedFrame=shadowAtlasPolicy_.frame; shadowVerify_.regrowGap=0; }
            else if (shadowVerify_.lastShedFrame && !shadowVerify_.regrowGap && shadowSilhouetteAtlasActive_ &&
                     atlas.livePages()>basePages)
                shadowVerify_.regrowGap=shadowAtlasPolicy_.frame-shadowVerify_.lastShedFrame;
#endif
            if(shadowSilhouetteAtlasActive_ && ta::devFlag("TA_SHADOW_CACHE_VERIFY")) {
                const auto readPages=[&] {
                    auto* target=SDL_GetRenderTarget(ren_);
                    float sx=1,sy=1;SDL_RenderGetScale(ren_,&sx,&sy);
                    SDL_Rect viewport{},clip{};SDL_RenderGetViewport(ren_,&viewport);
                    const bool clipped=SDL_RenderIsClipEnabled(ren_);
                    SDL_RenderGetClipRect(ren_,&clip);
                    std::vector<std::vector<uint32_t>> pixels(shadowSilhouettePages_.size());
                    for(size_t i=0;i<pixels.size();++i) {
                        if(!shadowSilhouettePages_[i].texture)continue;   // released page slot
                        pixels[i].resize(size_t(shadowSilhouetteWidth_)*shadowSilhouetteHeight_);
                        if(SDL_SetRenderTarget(ren_,shadowSilhouettePages_[i].texture)!=0 ||
                           SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_RGBA32,pixels[i].data(),
                                                shadowSilhouetteWidth_*4)!=0)
                            throw std::runtime_error("shadow cache verification read failed");
                    }
                    SDL_SetRenderTarget(ren_,target);
                    SDL_RenderSetScale(ren_,sx,sy);SDL_RenderSetViewport(ren_,&viewport);
                    SDL_RenderSetClipRect(ren_,clipped ? &clip : nullptr);
                    return pixels;
                };
                const auto cached=readPages();
                if(!buildShadowSilhouetteAtlas(true))
                    throw std::runtime_error("shadow cache reference build failed");
                const auto reference=readPages();
                // This frame's tiles (their stamps carry the page since F).
                for(const auto& tile:shadowTileStamps_) {
                        const size_t p=size_t(tile.page);
                        for(int y=tile.slotY;y<tile.slotY+tile.height+2;++y) {
                            const size_t offset=size_t(y)*shadowSilhouetteWidth_+tile.slotX;
                            if(std::memcmp(cached[p].data()+offset,reference[p].data()+offset,
                                           size_t(tile.width+2)*4)) {
                                for(int x=0;x<tile.width+2;++x)if(cached[p][offset+x]!=reference[p][offset+x]) {
                                    std::fprintf(stderr,"shadow mismatch page=%zu owner=%d slot=%d,%d at=%d,%d cached=%08x reference=%08x reused=%zu\n",
                                        p,tile.owner,tile.slotX,tile.slotY,tile.slotX+x,y,cached[p][offset+x],reference[p][offset+x],shadowAtlasReusedUnits);
                                    break;
                                }
                                throw std::runtime_error("retained shadow tile pixel mismatch");
                            }
                        }
                }
#ifndef NDEBUG
                ++shadowVerify_.frames;
#endif
            }
            shadowAtlasFallback=shadowSilhouetteAtlasActive_ ? "active" : shadowAtlasFallback;
            static int lastState=-1;
            static const char* lastReason=nullptr;
            static size_t lastUnits=SIZE_MAX,lastPages=SIZE_MAX;
            const int state=shadowSilhouetteAtlasActive_ ? 1 : 0;
            if (ta::devEnv("TA_PROF") && (shadowAtlasFallback!=lastReason || state!=lastState ||
                shadowAtlasPackedUnits!=lastUnits || shadowAtlasPackedPages!=lastPages)) {
                std::fprintf(stderr,"shadow-atlas %s: %zu units, %zu pages (%s)\n",
                    state ? "active" : "fallback",shadowAtlasPackedUnits,shadowAtlasPackedPages,
                    shadowAtlasFallback);
                lastState=state;lastUnits=shadowAtlasPackedUnits;lastPages=shadowAtlasPackedPages;
                lastReason=shadowAtlasFallback;
            }
        } else if (atlas.livePages()>basePages) {
            // Perf follow-up K: with the shadow pass off (Options SHADOWS) no frame
            // maintains the atlas, so nothing would expire, release or shed its
            // extended pages: give them back at once. (The base pages stay, as before.)
            resetShadowAtlas();
        }
#ifndef NDEBUG
        shadowVerify_.heldPages=atlas.livePages();
#endif
        if(ta::devFlag("TA_CACHE_STATS") && ++geometryFrame_%120==0) {
            size_t reused=0;
            for(size_t i=0;i<visUnits_.size();++i)reused+=visGeom(i).reused;
            std::fprintf(stderr,"CACHE geometry=%zu/%zu shadow_tiles=%zu/%zu\n",reused,
                visUnits_.size(),shadowAtlasReusedUnits,shadowAtlasPackedUnits);
        }
        airShadows_.clear();
        airShadowOp_ = SIZE_MAX;
        airOp_ = SIZE_MAX;
        // Composite each unit's baked silhouette once. The direct fallback uses
        // compact worker buffers and ordered cutout runs, avoiding a second
        // full-scene copy. Ground and airborne layers retain their existing
        // placement relative to bodies and scenery.
        if (!kNoShadow) {
            ta::render::ScopedPhase compositePhase(renderStats_, RP::ShadowComposite);
            std::vector<const UnitGeom*> groundShadows;
            groundShadows.reserve(items.size());
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_MOD);
            for (const auto& it : items) {
                if (!it.u) continue;
                const int gs = geomSlot(it.u->id);
                if (gs < 0) continue;
                const UnitGeom& gsh = visGeom(size_t(gs));
                if (gsh.shadowVerts.empty() && gsh.maskedShadowVerts.empty()) continue;
                // An AIRBORNE flyer's shadow falls on whatever is beneath it, so drawing
                // it here -- before any body -- lets every ground unit and feature paint
                // over it. Retail has no global shadow pre-pass at all: the Glide path
                // emits each unit's shadow INSIDE that unit's draw (0x4ee700 -> 0x4ec720
                // -> 0x4ec7b0 -> 0x4ede50 -> 0x4eda80), so a flyer drawn after the ground
                // units drops its shadow on top of them. Hold these back and drain them
                // between the ground bodies and the air bodies, which is where that same
                // ordering puts them. items is already sorted ground-layer-then-air, so
                // the boundary exists for free.
                //
                // Costs nothing: the vertices are identical, only their position in the
                // draw order changes. Measured at ~1180 units, shadow 2.2 -> 2.3ms.
                //
                // Only the AIRBORNE layer gets this treatment, and the reason is
                // measured. Going the whole way -- emitting every unit's shadow inside
                // its own draw, as retail does -- was prototyped for real and costs:
                //
                //     draw ops   6322 -> 8443  (+34%: 1095 shadow ops, and the body
                //                               segments fragment 6271 -> 7297 because
                //                               each flip has to close the running run)
                //     present    5.7 -> 7.7ms  (the extra flushes, GPU side)
                //     fps        57 -> 51      (-11% at ~1180 visible units)
                //
                // Eleven percent to let a GROUND unit's shadow fall on the unit behind
                // it is not a trade worth making; the flyer case is where the effect is
                // actually visible, and it costs nothing because it is ONE boundary
                // rather than one per unit.
                if (it.layer == 1) { airShadows_.push_back(&gsh); continue; }
                groundShadows.push_back(&gsh);
            }
            drawShadowBatch(groundShadows);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        }
        profShadowMs_ += (double(SDL_GetPerformanceCounter()) - _sh0) / _ptFreq;

        // HIDDEN UNITS: the units a later tree, rock or building overlaps; with FADE
        // those covers fade in the body plan below (nothing with the option off).
        findHiddenUnits(mvw, winH);

        const double _bdy0 = double(SDL_GetPerformanceCounter());
        // Pass 2: bodies (feature sprites + unit models) in depth order. Unit
        // models are accumulated into one batch and flushed only when the texture
        // changes or a feature/special unit interrupts the run -- so a crowd of one
        // unit type collapses to a handful of draw calls instead of one per unit.
        // Plan the draw order serially (no vertex copies), scatter the vertex
        // copies across the worker pool, then replay the draws. This spreads the
        // ~1.5M-vertex body assembly that used to peg one core, while keeping the
        // exact painter order (segments broken by features / special units).
        const double _asm0 = double(SDL_GetPerformanceCounter());
        copyTasks_.clear();
        drawOps_.clear();
        fxOrder_.clear();
        bodySources_.clear();
        bodyRanges_.clear();
        bodyArenaFirsts_.clear();
        bodyArenaCounts_.clear();
        SDL_Texture* segTex = nullptr;
        uint32_t segFirst = 0;
        int segCount = 0;
        auto closeSeg = [&] {
            if (segCount > 0) {
                DrawOp op{nullptr, nullptr, segTex, -1, segCount};
                op.firstRange = segFirst;
                op.ranges = uint32_t(bodyRanges_.size()) - segFirst;
                drawOps_.push_back(op);
                segCount = 0;
            }
            // Force the next run to open a new segment at the current range. Without
            // this, a unit whose texture matches segTex but follows a feature/special
            // unit (which closed the segment) would extend the PREVIOUS segment's
            // anchor and draw an earlier unit's vertices in its place.
            segTex = nullptr;
        };
        auto openSeg = [&](SDL_Texture* tex) {
            if (tex != segTex || segCount == 0) {
                closeSeg();
                segTex = tex;
                segFirst = uint32_t(bodyRanges_.size());
            }
        };
        for (const auto& it : items) {
            // The ground -> air boundary: the held-back flyer shadows go here, and the
            // replay draws the air bodies only after the shots and effects (airOp_).
            // Close the running segment so it lands on a real op boundary rather than
            // inside a batched run.
            if (it.layer == 1 && airOp_ == SIZE_MAX) {
                closeSeg();
                airOp_ = drawOps_.size();
                if (!airShadows_.empty()) airShadowOp_ = airOp_;
            }
            if (it.fx >= 0) {
                // A world effect: consecutive ones share one op (a run of
                // fxOrder_), never across the ground -> air boundary.
                closeSeg();
                fxOrder_.push_back(uint32_t(it.fx));
                DrawOp* last = drawOps_.empty() ? nullptr : &drawOps_.back();
                if (last && last->fxCount && (airOp_ == SIZE_MAX || drawOps_.size() > airOp_)) {
                    ++last->fxCount;
                } else {
                    DrawOp op{nullptr, nullptr, nullptr, 0, 0};
                    op.fxFirst = int32_t(fxOrder_.size() - 1);
                    op.fxCount = 1;
                    drawOps_.push_back(op);
                    renderStats_.add(RS::FxOps);
                }
                continue;
            }
            if (it.f) {
                closeSeg();
                drawOps_.push_back({nullptr, it.f, nullptr, 0, 0});
            } else {
                const auto& u = *it.u;
                int gslot = geomSlot(u.id);
                if (gslot < 0) continue;
                const UnitGeom& g = visGeom(size_t(gslot));
                // HIDDEN UNITS, FADE: a building over a unit, drawn whole and faded
                // (drawUnit's translucent body: its own op, out of the batches).
                const uint8_t fade = hiddenStructureAlpha(u);
                if (special(u, g)) {
                    closeSeg();
                    drawOps_.push_back({&u, nullptr, nullptr, 0, 0});
                    drawOps_.back().fade = fade;
                    renderStats_.add(RS::SpecialUnits);
                    continue;
                }
                if (fade != 255) {
                    closeSeg();
                    drawOps_.push_back({&u, nullptr, nullptr, 0, 0});
                    drawOps_.back().fade = fade;
                    continue;
                }
                const auto& distant=distantModelItems_[size_t(gslot)];
                if(distant.texture) {
                    openSeg(distant.texture);
                    const uint32_t src = uint32_t(bodySources_.size());
                    bodySources_.push_back({distant.quad.data(), 6, g.handle.index, g.handle.generation, 0});
                    bodyRanges_.push_back({src, 0, 6});
                    segCount += 6;
                    continue;
                }
                if (g.runs.empty()) continue;
                // A body left pending behind a held image (G) is never drawn live; if
                // it were, it gets no retained identity rather than a stale range.
                const uint32_t src = uint32_t(bodySources_.size());
                bodySources_.push_back({g.verts.data(), uint32_t(g.verts.size()),
                                        g.bodyPending ? ta::render::kArenaNoSlot : g.handle.index,
                                        g.handle.generation, g.revision});
                uint32_t offset = 0;
                for (const auto& r : g.runs) {
                    openSeg(r.first);
                    bodyRanges_.push_back({src, offset, uint32_t(r.second)});
                    segCount += r.second;
                    offset += uint32_t(r.second);
                }
            }
        }
        closeSeg();

        // Workstream H: native-size commands draw from the retained vertex arena
        // (client/vertexarena.h) when most of their content is resident or
        // unchanged since the last frame; only changed sources are copied, into
        // arena storage no pending draw reads. Otherwise (camera motion, a moving
        // army, no capable renderer, GPU budget) every command is assembled and
        // streamed exactly as before.
        using ta::render::ArenaMode;
        // Commands of at least this many vertices draw from the arena: the native
        // threshold (smaller batches take SDL's path, where priming costs more than
        // SDL's conversion). Debug TA_BODY_ARENA_MIN changes it (tests, experiments).
        size_t kNativeMin = ta::GeometrySubmit::kNativeMinVertices;
        if (const char* v = ta::devEnv("TA_BODY_ARENA_MIN")) kNativeMin = size_t(std::max(1, std::atoi(v)));
        const bool bodyDirect = !ta::devFlag("TA_BODY_SDL_SUBMIT");
        // Workstream N: native passes in the body replay (debug TA_NATIVE_PASS=off|
        // palette|all; TA_NATIVE_PASS_VERIFY compares the GL state after every pass
        // with the state SDL primed it with). The pure-SDL reference opens none.
        nativeMin_ = kNativeMin;
        nativePassMode_ = NativePassMode::All;
        if (const char* v = ta::devEnv("TA_NATIVE_PASS")) {
            if (!std::strcmp(v, "off")) nativePassMode_ = NativePassMode::Off;
            else if (!std::strcmp(v, "palette")) nativePassMode_ = NativePassMode::Palette;
        }
        nativePass_.setVerify(ta::devFlag("TA_NATIVE_PASS_VERIFY"));
        nativePassArena_ = true;
        if (const char* v = ta::devEnv("TA_NATIVE_PASS_ARENA")) nativePassArena_ = std::atoi(v) != 0;
        nativePassArenaMin_ = kNativePassArenaMin;
        if (const char* v = ta::devEnv("TA_NATIVE_PASS_ARENA_MIN")) nativePassArenaMin_ = size_t(std::max(1, std::atoi(v)));
        // (A simulated arena -- debug TA_BODY_ARENA_SOFTWARE -- has no GL buffer to draw from.)
        nativePassesOn_ = bodyDirect && nativePassMode_ != NativePassMode::Off &&
                          !ta::devFlag("TA_BODY_ARENA_SOFTWARE") &&
                          nativePass_.ready(ren_) && bodySubmit_.nativeCapable(ren_);
        bodyArenaOn_ = bodyDirect && !ta::devFlag("TA_BODY_ARENA_OFF");
        bodyArenaVerifyOn_ = ta::devFlag("TA_BODY_ARENA_VERIFY");
        for (auto& buffer : bodyArenaBuffers_) {
            buffer.setSimulated(ta::devFlag("TA_BODY_ARENA_SOFTWARE"));
            buffer.endFrame();
        }
        ++bodyArenaFrame_;
        bool arenaPlanned = false, arenaFrame = false;
        if (const char* v = ta::devEnv("TA_BODY_ARENA_HEADROOM")) {   // debug: tighter storage (tests)
            auto policy = bodyArena_.policy();
            policy.headroom = std::max(1.2, std::atof(v));
            policy.minHeadroom = std::min(policy.minHeadroom, policy.headroom - 0.05);
            policy.minCapacity = 1024;
            policy.granule = 256;
            policy.migrateMin = 16;
            policy.migrateFraction = 0.02;
            bodyArena_.setPolicy(policy);
        }
        // The frame's span-texture cache starts here: the N2 planning below and the
        // replay share it (GL names are valid for this frame only).
        spanTextures_.clear();
        // Workstream N: the frame's native passes, planned once (N2: commands a pass
        // draws take the arena at any size -- no priming of their own there; others
        // need 512+).
        if (nativePassesOn_) planNativePasses();
        if (bodyArenaOn_) {
            bodyArena_.begin(bodyArenaFrame_, distantNow_);
            for (DrawOp& op : drawOps_) {
                if (!op.tex || (size_t(op.count) < kNativeMin && !op.inPass)) continue;
                noteNeed(Retained::ArenaRanges, bodyArenaRanges_);
                bodyArenaRanges_.clear();
                for (uint32_t k = op.firstRange; k < op.firstRange + op.ranges; ++k) {
                    const BodyRange& r = bodyRanges_[k];
                    BodySource& src = bodySources_[r.source];
                    if (src.arena == UINT32_MAX)
                        src.arena = bodyArena_.add({src.data, src.count, src.slot, src.generation, src.revision});
                    bodyArenaRanges_.push_back({src.arena, r.offset, r.count});
                }
                op.arenaCommand = int32_t(bodyArena_.addCommand(bodyArenaRanges_));
            }
            arenaPlanned = true;
            auto& lead = bodyArenaBuffers_[0];
            const bool capable = lead.simulated() ||
                (bodySubmit_.nativeCapable(ren_) && bodyArenaBuffers_[0].ready() && bodyArenaBuffers_[1].ready());
            if (bodyArena_.plan(capable, bodyArenaBudget()) != ArenaMode::Stream) {
                // Storage actions in order: release a drained (or dropped) storage,
                // allocate the current one, then write (below).
                const int cur = bodyArena_.curSlot();
                if (const int drop = bodyArena_.releaseSlot(); drop >= 0) {
                    bodyArenaBuffers_[drop].release();
                    std::vector<SDL_Vertex>().swap(bodyArenaMirror_[drop]);
                }
                bool ok = !bodyArena_.allocate() ||
                          bodyArenaBuffers_[cur].allocate(bodyArena_.newCapacity() * sizeof(SDL_Vertex));
                if (ok && bodyArena_.allocate() && bodyArenaVerifyOn_ && !lead.simulated())
                    bodyArenaMirror_[cur].assign(bodyArena_.newCapacity(), SDL_Vertex{{-7777, -7777}, {1, 2, 3, 4}, {9, 9}});
                if (ok) arenaFrame = true;
                else {
                    bodyArena_.end(false);
                    arenaPlanned = false;
                    releaseBodyArena();
                }
            }
        } else if (bodyArena_.capacity() || bodyArenaBuffers_[0].capacityBytes() || bodyArenaBuffers_[1].capacityBytes())
            releaseBodyArena();
        const auto fromArena = [&](const DrawOp& op) {
            return arenaFrame && op.tex && (size_t(op.count) >= kNativeMin || op.inPass);
        };
        // Assembled commands: their ranges copied into bodyVerts_ (plan serially).
        // The arena's changed sources are staged right behind them, in arena
        // order, and go up with one glBufferSubData (no memory of their own).
        int destOff = 0;
        const auto queueAssembled = [&](bool arenaOps) {
            for (DrawOp& op : drawOps_) {
                if (!op.tex || op.count <= 0 || op.start >= 0 || fromArena(op) != arenaOps) continue;
                op.start = destOff;
                for (uint32_t k = op.firstRange; k < op.firstRange + op.ranges; ++k) {
                    const BodyRange& r = bodyRanges_[k];
                    copyTasks_.push_back({bodySources_[r.source].data + r.offset, r.count, uint32_t(destOff)});
                    bodySources_[r.source].assembled += r.count;
                    destOff += int(r.count);
                }
            }
        };
        queueAssembled(false);
        size_t uploaded = 0, uploadedTwice = 0;
        const size_t staged = size_t(destOff);
        if (arenaFrame) {
            for (const auto& up : bodyArena_.uploads()) {
                const auto& src = bodyArena_.sources()[up.source];
                copyTasks_.push_back({src.data, src.count, uint32_t(staged + (up.dst - bodyArena_.writeBegin()))});
                uploaded += src.count;
            }
            // A source goes up whole (its slot holds every run, so a later frame
            // can draw any of them from it), so the runs it also has in small,
            // assembled commands are copied twice. Counted, not avoided: avoiding
            // it means one upload call per such source instead of one per frame.
            for (const BodySource& src : bodySources_)
                if (src.assembled && src.arena != UINT32_MAX && bodyArena_.written(src.arena))
                    uploadedTwice += src.assembled;
        }
        destOff += int(uploaded);
        // Grow the copy buffer by a bounded step (1/16 above the need), never
        // std::vector's doubling, which kept twice the vertex need resident once
        // a frame's uploads went a little past the capacity. Contents are only
        // kept when a frame re-assembles behind the commands already copied.
        const auto runCopies = [&](bool keep) {
            const size_t need = size_t(destOff);
            if (need > bodyVerts_.capacity()) {
                if (!keep) bodyVerts_.clear();
                bodyVerts_.reserve(need + need / 16);
            }
            bodyVerts_.resize(need);
            SDL_Vertex* const out = bodyVerts_.data();
            const auto copyRange = [this, out](size_t b, size_t e) {
                for (size_t i = b; i < e; ++i) {
                    const CopyTask& t = copyTasks_[i];
                    std::copy(t.src, t.src + t.count, out + t.dst);
                }
            };
            if (scheduleUniform_) pool_.parallelFor(copyTasks_.size(), copyRange);
            else {
                // Workstream I: planned by bytes (plus a per-range cost).
                {
                    ta::render::ScopedPhase planPhase(renderStats_, RP::SchedulePlan);
                    copyPlan_.begin(schedules_.bodyCopy);
                    for (const CopyTask& t : copyTasks_)
                        copyPlan_.add(ta::render::RenderSchedules::kCopyPerTaskNs +
                                      ta::render::RenderSchedules::kCopyPerByteNs * double(t.count * sizeof(SDL_Vertex)));
                    copyPlan_.finish(pool_.threads());
                }
                schedThreadsCopy_ += copyPlan_.participants();
                runPlanned('c', copyPlan_, copyRange);
#ifndef NDEBUG
                if (scheduleVerifyOn_) {
                    // Each task copies into its own destination range: a serial re-run
                    // into a scratch buffer must match the planned result exactly.
                    verifyPlan(copyPlan_, copyTasks_.size());
                    std::vector<SDL_Vertex> serial(bodyVerts_.size());
                    for (const CopyTask& t : copyTasks_) std::copy(t.src, t.src + t.count, serial.data() + t.dst);
                    for (const CopyTask& t : copyTasks_)
                        if (t.count && std::memcmp(serial.data() + t.dst, out + t.dst, t.count * sizeof(SDL_Vertex)))
                            throw std::runtime_error("schedule verify: body copy differs from a serial run");
                    ++scheduleVerify_.copyPasses;
                    scheduleVerify_.copyVertices += need;
                    scheduleVerify_.parallelPasses += copyPlan_.participants() > 1;
                }
#endif
            }
        };
        runCopies(false);
        size_t copied = size_t(destOff);   // assembled + staged uploads (+ a write failure's re-assembly)
        size_t arenaRetained = 0, arenaUploaded = 0, arenaTwice = 0;
        if (arenaFrame) {
            const int cur = bodyArena_.curSlot();
            bool ok = bodyArenaBuffers_[cur].write(bodyArena_.writeBegin(), bodyVerts_.data() + staged, uploaded);
#ifndef NDEBUG
            // Debug TA_BODY_ARENA_FAIL_WRITE=N: the first write with uploads in each
            // window of N frames fails (tests the fallback).
            if (const char* v = ta::devEnv("TA_BODY_ARENA_FAIL_WRITE"); v && ok && uploaded && std::atoi(v) > 0) {
                static uint64_t failedWindow = 0;
                if (const uint64_t window = bodyArenaFrame_ / uint64_t(std::atoi(v)); window > failedWindow) {
                    failedWindow = window;
                    ok = false;
                }
            }
#endif
            const ta::render::ArenaFrameStats arenaStats = bodyArena_.stats();
            if (ok) {
                if (bodyArenaVerifyOn_ && !bodyArenaBuffers_[cur].simulated())
                    std::copy(bodyVerts_.begin() + long(staged), bodyVerts_.begin() + long(staged + uploaded),
                              bodyArenaMirror_[cur].begin() + long(bodyArena_.writeBegin()));
                for (DrawOp& op : drawOps_) {
                    if (!fromArena(op)) continue;
                    const size_t begin = bodyArenaFirsts_.size();
                    const uint32_t command = uint32_t(op.arenaCommand);
                    const uint32_t first = bodyArena_.commandFirstRange(command);
                    op.arenaFirst = int32_t(begin);
                    op.arenaSlot = int8_t(bodyArena_.commandSlot(command));
                    for (uint32_t k = 0; k < op.ranges; ++k) {
                        const BodyRange& r = bodyRanges_[op.firstRange + k];
                        ta::render::appendArenaRange(bodyArenaFirsts_, bodyArenaCounts_, begin,
                            bodyArena_.first(command, first + k), r.count);
                        if (!bodyArena_.written(bodySources_[r.source].arena)) arenaRetained += r.count;
                    }
                    op.arenaRanges = uint32_t(bodyArenaFirsts_.size() - begin);
                }
                arenaUploaded = uploaded;
                arenaTwice = uploadedTwice;
                if (bodyArenaVerifyOn_) verifyBodyArena(arenaStats);
            }
            bodyArena_.end(ok);
            arenaPlanned = false;
            renderStats_.add(arenaStats.mode == ArenaMode::Append ? RS::ArenaAppend : RS::ArenaRebuild);
            if (!ok) {
                // The storage contents are lost: draw these commands assembled, in
                // place of the staged uploads (the assembled commands before them stay).
                releaseBodyArena();
                renderStats_.add(RS::ArenaFallbacks);
                arenaFrame = false;
                copyTasks_.clear();
                for (DrawOp& op : drawOps_) op.arenaFirst = -1;
                destOff = int(staged);
                queueAssembled(false);
                runCopies(true);
                copied += size_t(destOff) - staged;
                bodyArenaFirsts_.clear();
                bodyArenaCounts_.clear();
#ifndef NDEBUG
                ++bodyArenaVerify_.fallbacks;
#endif
            }
        }
        if (arenaPlanned) {   // streamed: still record what was seen (the stable test)
#ifndef NDEBUG
            ++bodyArenaVerify_.streamReasons[size_t(bodyArena_.stats().reason)];
            ++bodyArenaVerify_.streams;
#endif
            bodyArena_.end(true);
            renderStats_.add(RS::ArenaStream);
        }
        if (bodyArena_.capacity() &&
            (bodyArena_.shouldRelease(bodyArenaFrame_, distantNow_) || bodyArenaPressure()))
            releaseBodyArena();
        bodyVertexTotal_ = 0;
        for (const DrawOp& op : drawOps_) if (op.tex && op.count > 0) bodyVertexTotal_ += size_t(op.count);
#ifndef NDEBUG
        if (bodyArenaVerifyOn_) verifyBodyAssembled(arenaFrame ? uploaded : 0);
#endif
        renderStats_.add(RS::BodyVertsCopied, copied);
        renderStats_.add(RS::BodyBytesCopied, copied * sizeof(SDL_Vertex));
        renderStats_.add(RS::ArenaVertsRetained, arenaRetained);
        renderStats_.add(RS::ArenaVertsUploaded, arenaUploaded);
        renderStats_.add(RS::ArenaVertsTwice, arenaTwice);
        ta::render::countUploadBytes(arenaUploaded * sizeof(SDL_Vertex));
        size_t arenaBytes = 0, arenaInflight = 0, mirrorBytes = 0;
        for (int k = 0; k < ta::render::kArenaSlots; ++k) {
            if (!bodyArenaBuffers_[k].simulated()) arenaBytes += bodyArenaBuffers_[k].capacityBytes();
            arenaInflight += bodyArenaBuffers_[k].inFlightBytes();
            mirrorBytes += bodyArenaMirror_[k].capacity() * sizeof(SDL_Vertex);
        }
        renderStats_.set(RS::ArenaCapacityBytes, arenaBytes);
        renderStats_.set(RS::ArenaInflightBytes, arenaInflight);
        renderStats_.set(RS::ArenaCpuBytes, bodyArena_.cpuBytes() +
            mirrorBytes + bodyArenaGather_.capacity() * sizeof(SDL_Vertex) +
            bodyArenaRanges_.capacity() * sizeof(ta::render::ArenaRange));
        renderStats_.set(RS::BodyBufferCapacityBytes, bodyVerts_.capacity() * sizeof(SDL_Vertex));
        renderStats_.set(RS::BodyBufferUsedBytes, bodyVerts_.size() * sizeof(SDL_Vertex));
        renderStats_.addMs(RP::BodyAssembly, (double(SDL_GetPerformanceCounter()) - _asm0) / _ptFreq);
        const double _bsub0 = double(SDL_GetPerformanceCounter());
        // The deferred air shadows run INSIDE the body window but belong to the shadow
        // phase. Time them separately and back them out below, or body and shadow both
        // report the same work.
        double airShadowMs = 0;
        auto drainAirShadows = [&] {
            if (airShadows_.empty()) return;
            const double _as0 = double(SDL_GetPerformanceCounter());
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_MOD);
            drawShadowBatch(airShadows_);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            noteNeed(Retained::AirShadows, airShadows_);
            airShadows_.clear();
            const double ms = (double(SDL_GetPerformanceCounter()) - _as0) / _ptFreq;
            profShadowMs_ += ms;
            airShadowMs += ms;
        };
#ifndef NDEBUG
        debugFeatureFlameDrawCount_ = 0;
        debugFeatureSmokeDrawCount_ = 0;
        debugPaintOrder_.clear();
        debugAirOps_ = 0;
#endif
        // Replays drawOps_[first, last). Called twice: the ground and feature bodies
        // here, the airborne bodies after the shots and effects (airOp_).
        const auto replayOps = [&](size_t first, size_t last) {
        for (size_t opi = first; opi < last; ++opi) {
            if (opi == airShadowOp_) drainAirShadows();
            const DrawOp& op = drawOps_[opi];
            if (nativePassesOn_ && op.passEnd > int32_t(opi)) {
                // Workstream N: a run of body commands in one native pass (M: from an
                // indexed palette run only), as planned before the arena.
                if (const size_t end = drawNativeSpan(opi); end > opi) { opi = end - 1; continue; }
            }
            if (op.fxCount) {   // shots and effects in their depth place (collectWorldFx)
                drawFxSpan(fxOrder_.data() + op.fxFirst, op.fxCount);
#ifndef NDEBUG
                debugFxReplayed_ += op.fxCount;
#endif
                continue;
            }
            if (op.f) {
                const auto& f = *op.f;
                ta::render::countLogicalDraw();   // the feature body (its shadow/flames/smoke not counted)
                // Lift the decal onto the terrain relief just like a unit, so a metal
                // patch sits at the height its heightmap claims (and units
                // built on it line up) instead of the decal being flat.
                float lfx = terrainLiftX(f.x, f.z) * zm0, lfy = terrainLift(f.x, f.z) * zm0;
                // RECLAIMED, fading out: retail draws only a white silhouette of the
                // frame at the fade's alpha -- no shadow, no flames (reclaimfade.h).
                if (const int steps = f.aliveVis ? ta::reclaimfade::fadeSteps(f.fadeEnd, front().gameTick) : 0) {
                    const FeatureSprite sprite = featureSprite(f, zm0);
                    SDL_Texture* white = sprite.tex && sprite.art && !sprite.burning
                        ? fadeSilhouette(*sprite.art, sprite.frame) : nullptr;
                    const uint8_t alpha = uint8_t(unsigned(ta::reclaimfade::alphaByte(steps)) *
                                                  hiddenFeatureAlpha(f) / 255u);
                    if (!white || !alpha) continue;
                    if (sprite.sway) {
                        const SDL_FRect& dst = sprite.dst;
                        const SDL_Color wc{255, 255, 255, alpha};
                        SDL_Vertex v[4] = {
                            {{dst.x + sprite.shear, dst.y}, wc, {0, 0}},
                            {{dst.x + dst.w + sprite.shear, dst.y}, wc, {1, 0}},
                            {{dst.x + dst.w, dst.y + dst.h}, wc, {1, 1}},
                            {{dst.x, dst.y + dst.h}, wc, {0, 1}},
                        };
                        static const int fadeIdx[6] = {0, 1, 2, 0, 2, 3};
                        SDL_RenderGeometry(ren_, white, v, 4, fadeIdx, 6);
                    } else {
                        SDL_SetTextureAlphaMod(white, alpha);
                        SDL_RenderCopyF(ren_, white, nullptr, &sprite.dst);
                    }
                    continue;
                }
                const bool swayTree=f.tree && settings_ && settings_->treeSway;
                const float phase=f.x*0.043f+f.z*0.029f;
                const float sway=swayTree ? std::sin(animClock_*1.1f+phase)*0.7f+
                                           std::sin(animClock_*2.7f+phase*1.7f)*0.3f : 0;
                const uint32_t burnAge = int32_t(front().gameTick-f.burnStarted) < 0
                    ? 0 : front().gameTick-f.burnStarted;
                const bool overlayBurn = f.burnVis && (f.frontFlame || f.backFlame);
                // Dying: only a seqnamedieshad shadow (4fd7cc; none ships), so none.
                const bool dying = f.dieEnd && f.dieArt;
                const FeatArt* shadowArt = dying ? f.dieArt
                    : f.burnVis && !overlayBurn ? f.burnArt : f.art;
                const uint32_t shadowAge = dying
                    ? uint32_t(ta::featuredie::elapsed(f.dieArt->totalTicks, f.dieEnd, front().gameTick))
                    : f.burnVis ? (overlayBurn ? f.burnStarted : burnAge) : front().gameTick;
                SDL_Texture* shadow = nullptr;
                FeatArt::FGeom sg;
                if (shadowArt) {
                    const auto index = ta::retailEffectFrame(shadowArt->shadowDurations,
                        shadowArt->shadowLoop, shadowAge);
                    if (index && *index < shadowArt->shadowFrames.size()) {
                        shadow = shadowArt->shadowFrames[*index];
                        sg = shadowArt->shadowGeom[*index];
                    }
                }
                if (shadow && shadowsOnFrame_) {
                    SDL_FRect sd{(f.x - mapView_.offX() - float(sg.xoff)) * zm0 - lfx,
                                 (f.z - mapView_.offY() - float(sg.yoff)) * zm0 - lfy,
                                 float(sg.w) * zm0, float(sg.h) * zm0};
                    if (swayTree) {
                        // Authored feature shadows can extend above or below
                        // their ground anchor. Move the far end with the crown,
                        // leaving the trunk anchor fixed in either orientation.
                        const float shear=sway*float(f.h)*zm0*0.04f;
                        const float reach=float(sg.h)*0.5f<float(sg.yoff)
                            ? -float(std::max(1,sg.yoff))
                            : float(std::max(1,sg.h-sg.yoff));
                        const float top=-float(sg.yoff)/reach*shear;
                        const float bottom=(float(sg.h)-float(sg.yoff))/reach*shear;
                        const SDL_Color white{255,255,255,255};
                        SDL_Vertex v[4]={{{sd.x+top,sd.y},white,{0,0}},
                            {{sd.x+sd.w+top,sd.y},white,{1,0}},
                            {{sd.x+sd.w+bottom,sd.y+sd.h},white,{1,1}},
                            {{sd.x+bottom,sd.y+sd.h},white,{0,1}}};
                        static const int indices[6]={0,1,2,0,2,3};
                        SDL_RenderGeometry(ren_,shadow,v,4,indices,6);
                    } else SDL_RenderCopyF(ren_, shadow, nullptr, &sd);
                }
                // A burning feature keeps its burn art under Tactical Dots; only its
                // flame overlays and smoke are world effects (worldFxFrame()).
                const auto drawFeatureSmoke = [&] {
                    if(!worldFxFrame())return;
                    const auto found=featureSmokeSprites_.find(f.simId);
                    if(found==featureSmokeSprites_.end())return;
                    for(const auto& sprite:found->second) {
                        if(sprite.particle.frame>=sprite.art->frames.size())continue;
                        const auto& frame=sprite.art->frames[sprite.particle.frame];
                        const auto& point=sprite.particle.position;
                        const int x=point[0]>>16,y=point[1]>>16,z=point[2]>>16;
                        (void)heightAbove(float(x),float(z));
                        const float sx=(float(x)-mapView_.offX())*zm0;
                        const float sy=(float(z-(y>>1))-mapView_.offY())*zm0;
                        if(!ta::retailViewportCenterAdmitted(sx,sy,
                                float(mapViewW(winW_)),float(winH_)))continue;
                        SDL_FRect dst{sx-frame.ax*zm0,
                            sy-frame.ay*zm0,
                            frame.w*zm0,frame.h*zm0};
                        SDL_SetTextureAlphaMod(frame.tex,frame.encoding==4 ? 255 : 128);
                        SDL_RenderCopyF(ren_,frame.tex,nullptr,&dst);
                        SDL_SetTextureAlphaMod(frame.tex,255);
#ifndef NDEBUG
                        ++debugFeatureSmokeDrawCount_;
#endif
                    }
                };
                const auto drawFlame = [&](const EffectAnim* animation) {
                    if (!f.burnVis || !animation || !worldFxFrame()) return;
                    const auto index = ta::retailEffectFrame(animation->durations, false,
                        burnAge);
                    if (!index || *index >= animation->frames.size()) return;
                    const auto& frame = animation->frames[*index];
                    SDL_FRect dst{(f.x-mapView_.offX()-frame.ax)*zm0-lfx,
                                  (f.z-mapView_.offY()-frame.ay)*zm0-lfy,
                                  frame.w*zm0, frame.h*zm0};
                    SDL_RenderCopyF(ren_, frame.tex, nullptr, &dst);
#ifndef NDEBUG
                    ++debugFeatureFlameDrawCount_;
#endif
                };
                drawFlame(f.backFlame);
                // The sprite (burn playback, animation frame, sway): featureSprite.
                // HIDDEN UNITS, FADE: a tree or rock over a unit drawn whole, faded.
                const FeatureSprite sprite = featureSprite(f, zm0);
                const uint8_t alpha = hiddenFeatureAlpha(f);
                if (!sprite.tex) {
                    // nothing to draw (a finished one-shot animation)
                } else if (sprite.burning || (!sprite.sway && alpha != 255)) {
                    Uint8 was = 255;
                    if (alpha != 255) { SDL_GetTextureAlphaMod(sprite.tex, &was); SDL_SetTextureAlphaMod(sprite.tex, alpha); }
                    SDL_RenderCopyF(ren_, sprite.tex, nullptr, &sprite.dst);
                    if (alpha != 255) SDL_SetTextureAlphaMod(sprite.tex, was);
                } else if (sprite.sway) {
                    // Wind sway (Options; beyond-retail -- retail trees are static
                    // single-frame GAFs): shear the crown sideways on two blended
                    // gust sines, pivoting at the trunk base (the GAF anchor sits
                    // there). Per-tree phase so a forest ripples instead of rocking
                    // in unison. Display-only; the sim never sees it.
                    const SDL_FRect& dst = sprite.dst;
                    const float shear = sprite.shear;
                    const SDL_Color wc{255, 255, 255, alpha};
                    SDL_Vertex v[4] = {
                        {{dst.x + shear, dst.y}, wc, {0, 0}},
                        {{dst.x + dst.w + shear, dst.y}, wc, {1, 0}},
                        {{dst.x + dst.w, dst.y + dst.h}, wc, {1, 1}},
                        {{dst.x, dst.y + dst.h}, wc, {0, 1}},
                    };
                    static const int wIdx[6] = {0, 1, 2, 0, 2, 3};
                    SDL_RenderGeometry(ren_, sprite.tex, v, 4, wIdx, 6);
                } else {
                    SDL_RenderCopyF(ren_, sprite.tex, nullptr, &sprite.dst);
                }
                drawFlame(f.frontFlame);
                drawFeatureSmoke();
            } else if (op.u) {
                ta::render::countLogicalDraw();   // one special unit (drawUnit issues several)
                drawUnit(*op.u, op.fade);   // (FADE: a faded building, see-through)
            } else if (op.count > 0 && op.tex) {   // null atlas page: skip, no white
                renderStats_.add(RS::BodySubmissions);
                if (op.arenaFirst >= 0) {
                    const std::span<const GLint> firsts(bodyArenaFirsts_.data() + op.arenaFirst, op.arenaRanges);
                    const std::span<const GLsizei> counts(bodyArenaCounts_.data() + op.arenaFirst, op.arenaRanges);
                    const auto& arenaBuffer = bodyArenaBuffers_[op.arenaSlot];
                    const bool simulated = arenaBuffer.simulated();
                    ta::NativeProgram* program = paletteProgram(op.tex);
                    const int drawn = simulated ? 1 : bodySubmit_.drawBuffer(ren_, op.tex, arenaBuffer.id(),
                                                                             firsts, counts, size_t(op.count), program);
                    if (program && drawn == 0) ++paletteIndexedDraws_;
                    if (drawn == 0) {
                        renderStats_.add(RS::ArenaDraws);
                        renderStats_.add(RS::ArenaRanges, firsts.size());
                    } else if (drawn < 0) {
                        // SDL error while priming: nothing drawn, as draw() would
                        // have failed the same way. Not an arena draw.
                        renderStats_.add(RS::ArenaFallbacks);
                        if (program) {   // an indexed run went undrawn: baked from the next frame
                            ++paletteFallbacks_;
                            paletteIndexedFailed_ = true;
                        }
                    } else {
                        // Not drawable from the buffer here: the simulated arena (its
                        // vertices gathered through the ranges -- the software-renderer
                        // test of the bookkeeping) or a texture this renderer cannot
                        // sample natively (the same vertices, from the sources).
                        bodyArenaGather_.clear();
                        if (simulated) {
                            for (size_t k = 0; k < firsts.size(); ++k)
                                bodyArenaGather_.insert(bodyArenaGather_.end(),
                                    arenaBuffer.simulatedData() + firsts[k],
                                    arenaBuffer.simulatedData() + firsts[k] + counts[k]);
                            renderStats_.add(RS::ArenaDraws);
                            renderStats_.add(RS::ArenaRanges, firsts.size());
                        } else {
                            for (uint32_t k = op.firstRange; k < op.firstRange + op.ranges; ++k) {
                                const BodyRange& r = bodyRanges_[k];
                                const SDL_Vertex* data = bodySources_[r.source].data + r.offset;
                                bodyArenaGather_.insert(bodyArenaGather_.end(), data, data + r.count);
                            }
                            renderStats_.add(RS::ArenaFallbacks);
                        }
                        if (program) drawModelRun(op.tex, bodyArenaGather_.data(), int(bodyArenaGather_.size()), false);
                        else bodySubmit_.draw(ren_, op.tex, bodyArenaGather_, bodyDirect);
                    }
                } else if (paletteProgram(op.tex)) {
                    // Indexed palette runs always draw natively, whatever their size.
                    drawModelRun(op.tex, bodyVerts_.data() + op.start, op.count, false);
                } else {
                    bodySubmit_.draw(ren_,op.tex,std::span<const SDL_Vertex>(bodyVerts_.data()+op.start,size_t(op.count)),
                                     bodyDirect);
                }
            }
        }
        };
        const size_t airFirst = std::min(airOp_, drawOps_.size());
        replayOps(0, airFirst);
#ifndef NDEBUG
        debugPaintOrder_ += 'g';
#endif
        // The held-back flyer shadows drain here, at the ground -> air boundary: over
        // the ground bodies, under the shots, effects and air bodies. This also covers
        // an air layer that produced no draw ops at all (every flyer special-cased or
        // slotless), which would otherwise silently DROP those shadows rather than
        // merely misorder them. drainAirShadows() clears as it goes. It sits BEFORE the
        // submit timer closes so that every air-shadow draw falls inside the same phase
        // as the rest of them.
        drainAirShadows();
#ifndef NDEBUG
        if (nativePass_.verifyFailures())
            throw std::runtime_error("native pass verify: " + nativePass_.lastFailure());
#endif
        // The shots and effects drawn in the replay (fxSpanMs_) are effects work:
        // backed out of the body phases like the air shadows.
        const double groundFxMs = fxSpanMs_;
        profSubmitMs_ += -groundFxMs + (double(SDL_GetPerformanceCounter()) - _st0) / _ptFreq;
        renderStats_.addMs(RP::BodySubmit, -airShadowMs - groundFxMs +
                           (double(SDL_GetPerformanceCounter()) - _bsub0) / _ptFreq);
        renderStats_.addMs(RP::ShadowComposite, airShadowMs);
        renderStats_.addMs(RP::Effects, groundFxMs);
        profFxMs_ += groundFxMs;

        // Air shadows ran inside this window but belong to the shadow phase; back them
        // out so body and shadow do not both report the same work.
        profBodyMs_ += -airShadowMs - groundFxMs + (double(SDL_GetPerformanceCounter()) - _bdy0) /
                       (double(SDL_GetPerformanceFrequency()) / 1000.0);
        // Tactical Dots stand in for the unit bodies: over the terrain and features
        // (a dot under a tree crown would vanish). Shots and effects do not draw
        // on a dots frame at all (worldFxFrame()).
        if (dotsFrame_) drawTacticalDots(mvw, winH);
        // HIDDEN UNITS (DOTS): units the scenery or a building painted over get their
        // dot on top (nothing on a dots frame or with another setting).
        drawHiddenDots(mvw, winH);
        // Ghosts of the local player's queued (shift) build orders. Only the units the
        // snapshot lists as holding such an order (Frame::buildGhostUnits, in live
        // order) can draw one (workstream L: no walk over every unit).
        // The order being worked has a site of its own, drawn in its painter place
        // (under any flyer); it gets no second ghost over everything
        // (ta::render::buildGhostDrawn).
        const int hudOwner = hudPlayer();
        const auto buildGhosts = [&](const UnitR& u, bool draw) {
            if (!u.alive() || u.player != hudOwner) return false;
            bool any = false;
            const UnitR* site = u.buildSiteId ? frameUnitP(u.buildSiteId) : nullptr;
            // Every queued build is an order now; the order sits at the builder's
            // working position, so step back to where the site actually goes.
            for (size_t i = 0; i < u.orders.size(); ++i)
                if (ta::render::buildGhostDrawn(u, i, site)) {
                    const auto& o = u.orders[i];
                    any = true;
                    if (draw) renderStats_.add(RS::GhostsQueued);
                    if (draw) drawGhostAt(o.buildType,
                        o.buildRectangle ? o.buildX.toFloat() : o.x.toFloat(),
                        o.buildRectangle ? o.buildZ.toFloat() : o.z.toFloat() - float(o.buildType->footZ) * 8 - 24);
                }
            return any;
        };
        {
            // Two names for the same scope: t_ghosts (measurement B's interactive cases)
            // and t_build_ghosts (workstream E's ghost scenes).
            ta::render::ScopedPhase ghostPhase(renderStats_, RP::Ghosts);
            ta::render::ScopedPhase buildGhostPhase(renderStats_, RP::BuildGhosts);
            const Frame& frame = front();
#ifndef NDEBUG
            if (unitIndexVerify_) {
                overlayIdsIndexed_.clear(); overlayIdsFull_.clear();
                for (uint32_t i : frame.buildGhostUnits)
                    if (buildGhosts(*frame.live[i], true)) overlayIdsIndexed_.push_back(frame.live[i]->id);
                for (const UnitR* up : frame.live) if (buildGhosts(*up, false)) overlayIdsFull_.push_back(up->id);
                if (overlayIdsIndexed_ != overlayIdsFull_)
                    throw std::runtime_error("unit index verify: build ghosts differ from a full scan");
                ++unitIndexVerifyStats_.passes;
                unitIndexVerifyStats_.live += frame.live.size();
                unitIndexVerifyStats_.candidates += frame.buildGhostUnits.size();
                unitIndexVerifyStats_.accepted += overlayIdsFull_.size();
                unitIndexVerifyStats_.ghosts += overlayIdsFull_.size();
            } else
#endif
            for (uint32_t i : frame.buildGhostUnits) buildGhosts(*frame.live[i], true);
#ifndef NDEBUG
            // TA_GHOST_ALL_TYPES (render study, perf follow-up E): also a ghost of EVERY
            // registry type each frame, on a grid over the view and 30% around it (on
            // screen, straddling the edges, off screen), alternately red-washed. With
            // TA_GHOST_VERIFY this puts every model, including those textured from
            // outside the colour atlas, through the reference check.
            static const bool allTypes = ta::devFlag("TA_GHOST_ALL_TYPES");
            if (allTypes) {
                const float zmA = mapView_.zoom();
                const float x0 = mapView_.offX() - 0.3f * float(mvw) / zmA, x1 = mapView_.offX() + 1.3f * float(mvw) / zmA;
                const float z0 = mapView_.offY() - 0.3f * float(winH) / zmA, z1 = mapView_.offY() + 1.3f * float(winH) / zmA;
                const size_t cols = size_t(std::max(1.0, std::ceil(std::sqrt(double(registry_.types().size())))));
                size_t k = 0;
                for (const auto& [id, type] : registry_.types()) {
                    drawGhostAt(&type, x0 + (x1 - x0) * (float(k % cols) + 0.37f) / float(cols),
                                z0 + (z1 - z0) * (float(k / cols) + 0.61f) / float(cols), k % 2 == 1);
                    ++k;
                }
            }
#endif
        }
        renderStats_.set(ta::render::Counter::GhostCacheBytes, ghostGeometryBytes_);

        // The late fx pass: only the world effects collectWorldFx left out of the
        // painter list (far outside the view), in the old family order -- shots,
        // particles, rings, blood, debris, glows, explosions, unit smoke.
        // Everything near the view drew in its depth place in the replay above.
        float zm = mapView_.zoom();
        {
            const double _t0 = double(SDL_GetPerformanceCounter());
            ta::render::ScopedPhase effectsPhase(renderStats_, RP::Effects);
            if (worldFxFrame()) drawLateFx();
            profFxMs_ += (double(SDL_GetPerformanceCounter()) - _t0) /
                         (double(SDL_GetPerformanceFrequency()) / 1000.0);
#ifndef NDEBUG
            debugPaintOrder_ += 'e';
#endif
        }
        // The airborne bodies (and their own attached effects), over every ground
        // shot and effect (retail 0x4fcbab..0x4fcc27 after 52b210 and 492070) and
        // under the fog, as retail's world pass draws them.
        {
            const double _air0 = double(SDL_GetPerformanceCounter());
            const double fxBefore = fxSpanMs_;
            ta::render::ScopedPhase airPhase(renderStats_, RP::BodySubmit);
            replayOps(airFirst, drawOps_.size());
            const double airFxMs = fxSpanMs_ - fxBefore;   // an airborne flyer's own effects
            renderStats_.addMs(RP::BodySubmit, -airFxMs);
            renderStats_.addMs(RP::Effects, airFxMs);
            profFxMs_ += airFxMs;
#ifndef NDEBUG
            debugPaintOrder_ += 'a';
            debugAirOps_ = drawOps_.size() - airFirst;
            if (nativePass_.verifyFailures())
                throw std::runtime_error("native pass verify: " + nativePass_.lastFailure());
#endif
            spanTextures_.clear();   // GL names are valid for this frame's replays only
            const double ms = (double(SDL_GetPerformanceCounter()) - _air0) / _ptFreq - airFxMs;
            profSubmitMs_ += ms;
            profBodyMs_ += ms;
        }

        {
            const double _t0 = double(SDL_GetPerformanceCounter());
            ta::render::ScopedPhase fogPhase(renderStats_, RP::Fog);
            drawFog();
            profFogMs_ += (double(SDL_GetPerformanceCounter()) - _t0) /
                          (double(SDL_GetPerformanceFrequency()) / 1000.0);
        }
        // A MakePoster tile has no cursor: no placement ghosts or drag boxes, which
        // would repeat at the same screen spot in every tile.
        if (posterFrame_) {
            // nothing at the cursor
        } else if (buildDrag_ && placing_) {
            float mx, mz;
            pickWorld(mouseX_, mouseY_, mx, mz);
            for (auto& [x, z] : buildLinePositions(bdX0_, bdZ0_, mx, mz))
                drawGhostAt(placing_, x, z, !canPlacePreview(placing_, x, z));
            // The spacing, beside the cursor: for a moment after the wheel changes it,
            // and for the whole drag while it is not the tight default.
            if (buildLineGap_ > 0 || buildLineGapShown_ > 0) {
                const std::string gap = "GAP " + std::to_string(buildLineGap_);
                const float px = 1.8f, tw = blockWidth(gap, px);
                // Above and right of the pointer: below it, the build strip would cover it.
                const float lx = mouseX_ + 18, ly = std::max(4.0f, mouseY_ - 34);
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 190);
                SDL_FRect tb{lx - 4, ly - 4, tw + 8, 20};
                SDL_RenderFillRectF(ren_, &tb);
                blockText(gap, lx, ly, px, {230, 230, 200, 255});
            }
        } else if (placing_) {
            drawGhost();
        }
        if (!posterFrame_ && rightDrag_ && areaBoxAt(rdAnchor_, mouseX_, mouseY_)) {
            // "Clear this area" box, with a marker on every reclaimable feature it
            // currently catches. Shown only once the release would be an area clear
            // (client/areadrag.h), so a right-click order never flashes it. The press
            // corner is re-projected from the map, so it stays on its ground when the
            // camera moves mid-drag.
            float zm = mapView_.zoom();
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            const auto a = dragStartOnScreen(rdAnchor_);
            float x0 = std::min(a.x, mouseX_), y0 = std::min(a.y, mouseY_);
            float x1 = std::max(a.x, mouseX_), y1 = std::max(a.y, mouseY_);
            SDL_FRect box{x0, y0, x1 - x0, y1 - y0};
            SDL_SetRenderDrawColor(ren_, 255, 170, 40, 40);
            SDL_RenderFillRectF(ren_, &box);
            SDL_SetRenderDrawColor(ren_, 255, 190, 70, 220);
            SDL_RenderDrawRectF(ren_, &box);
            float mx, mz;
            pickWorld(mouseX_, mouseY_, mx, mz);
            float minx = std::min(rdX0_, mx), maxx = std::max(rdX0_, mx);
            float minz = std::min(rdZ0_, mz), maxz = std::max(rdZ0_, mz);
            SDL_SetRenderDrawColor(ren_, 255, 210, 90, 230);
            // The render snapshot, not world_: no per-frame simMutex_ while dragging.
            // Same reclaimable predicate as the click (reclaimTargetAt) and the same
            // footprint-cell selection as the sim's tickReclaimArea.
            const int mapW = mapView_.map().width;
            // The sim clamps the box to the map before sampling it from the min corner.
            const float limX = float(std::max(0, mapW - 1) * 16), limZ = float(std::max(0, mapView_.map().height - 1) * 16);
            minx = std::clamp(minx, 0.f, limX); maxx = std::clamp(maxx, 0.f, limX);
            minz = std::clamp(minz, 0.f, limZ); maxz = std::clamp(maxz, 0.f, limZ);
            auto mark = [&](float x, float z) {
                float fsx = (x - mapView_.offX()) * zm - terrainLiftX(x, z) * zm;
                float fsy = (z - mapView_.offY()) * zm - terrainLift(x, z) * zm;
                SDL_FRect m{fsx - 4, fsy - 4, 8, 8};
                SDL_RenderDrawRectF(ren_, &m);
            };
            for (const auto& f : features_) {
                if (!f.hasSim || !f.aliveVis || !f.reclaimable || !canPickPoint(f.x, f.z)) continue;
                if (ta::client::reclaimFootprintInBox(f.simId % mapW, f.simId / mapW, f.fx, f.fz, minx, maxx, minz, maxz))
                    mark(f.x, f.z);
            }
            // Corpses / statues / rubble get the same box markers.
            for (const UnitR* cu : front().live) {
                if (cu->alive() || !cu->corpsePhase || !cu->corpseReclaimable || !canPickPoint(cu->x, cu->z)) continue;
                if (ta::client::reclaimFootprintInBox(cu->corpseCellX, cu->corpseCellZ, cu->corpseFootX, cu->corpseFootZ,
                                                       minx, maxx, minz, maxz))
                    mark(cu->x, cu->z);
            }
        }

        // Selection membership as a hash set: the old code did frameUnitP(id) (a
        // linear scan) per selected unit and std::find(selection_) per world unit
        // -- both O(n^2) once a big army was selected, which tanked the frame.
        // The distant-body pass already populated selSet_ for this frame.
        // Selection rings: iterate units once, batched into a single draw
        // (viewport-culled, thin quads).
        overlayBatch_.clear();
        if (!selSet_.empty()) {
            float zms = mapView_.zoom();
            // Retail's selection indicator (0x4fd0d0) is not a bracket or a sprite --
            // it is TWO counter-rotating dashed ellipses lying on the unit's ground
            // plate, coloured by health. Straight from the binary:
            //   * angle = ((perUnitPhase + tick) << 14) / 30 BAM, so 16384/30 BAM a
            //     tick = 120 ticks = 4s per revolution. One ring runs +A, the other
            //     -A, hence counter-rotating.
            //   * 6 dashes per ring at 0x2aaa BAM = 60.0 degrees apart, each dash an
            //     0xccc BAM = 18.0 degree chord (retail computes 2 points per dash and
            //     joins them, so they are chords, not arcs). 12 dashes, 24 points.
            //   * radii = 1.2 (the double at 0x5f03f0) times the half-extents of the
            //     ROOT piece's box, centred on that box (client/selectionring.h).
            //   * every point turns by the unit's three angles (0x535d50 with
            //     unit+0x7c), so the ring keeps the plate's place and shape as the
            //     unit turns. [unit+2] is a per-unit phase added to the tick, so
            //     neighbouring units are out of step with each other.
            const uint64_t ringTime=ta::devFlag("TA_RENDER_STUDY")
                ? uint64_t(animClock_*1000) : SDL_GetTicks64();
            const float spin = float(ringTime % 4000) / 4000.0f;   // 1 rev / 4s
            // Iterate the (few) selected ids, not the whole world -- frameUnitP(id)
            // is O(1). (A duplicate id would just redraw the same brackets in place.)
            for (int selId : selection_) {
                if (dotsFrame_) break;   // a selected dot carries its own outline
                const UnitR* up = frameUnitP(selId);
                if (!up || !up->alive() || !up->type) continue;
                const UnitR& u = *up;
                // unitScreen puts the model's origin on screen (the +12 undoes its
                // body bias, as the click test does) and already carries terrain lift
                // and flyer altitude, so the ring tracks a unit up a cliff or in the
                // air.
                SDL_FPoint hp = unitScreen(u);
                const RingBox& rb = unitRingBox(u.type);
                float ix, iz, ih; interpPose(u, ix, iz, ih);
                if (u.replacementModel) ih = u.replacementHeading;
                const Xform body = bodyTransform(u);
                const float cosF = std::cos(-ih), sinF = std::sin(-ih);
                const float ox0 = hp.x, oy0 = hp.y + 12.0f * zms;
                const float rx = 1.2f * rb.halfX, ry = 1.2f * rb.halfZ;
                const SDL_FPoint mid = selectionRingPoint(rb, 0, 0, 1, 0, body, cosF, sinF);
                const float cx = ox0 + mid.x * zms, cy = oy0 + mid.y * zms;
                // Cull against the RING, not its centre: a big unit just off the edge
                // still has half a ring inside the viewport.
                const float reach = std::max(rx, ry) * zms;
                if (cx + reach < 0 || cx - reach > mvw || cy + reach < 0 || cy - reach > winH) continue;
                // Health ramp, exactly retail's two halves: full green -> yellow at
                // half -> red at death, blue always 0.
                int hp16 = std::max(0, int(u.hp)), mx = std::max(1, int(u.type->maxHp));
                SDL_Color col{255, 255, 0, 255};
                if (hp16 * 2 >= mx) {
                    col.r = Uint8(std::min(255, 255 * (mx - hp16) / std::max(1, mx / 2)));
                    col.g = 255;
                } else {
                    col.r = 255;
                    col.g = Uint8(std::min(255, 255 * hp16 / std::max(1, mx / 2)));
                }
                // Per-unit phase so neighbouring units are out of step, as retail's
                // [unit+2] does.
                float phase = float(selId * 37 % 360) / 360.0f;
                float th = std::max(1.0f, 1.2f * zms);
                const float kTwoPi = 6.28318530718f;
                const float kDash = kTwoPi * 18.0f / 360.0f;
                for (int ring = 0; ring < 2; ++ring) {
                    // One ring runs with the clock, the other against it.
                    float a0 = (ring == 0 ? (spin + phase) : -(spin + phase)) * kTwoPi;
                    float dash = (ring == 0 ? -kDash : kDash);
                    float ox = (ring == 1 ? 1.0f : 0.0f);   // retail nudges ring 2 by 1px
                    for (int k = 0; k < 6; ++k) {
                        float t0 = a0 + float(k) * (kTwoPi / 6.0f);
                        float t1 = t0 + dash;
                        const SDL_FPoint p0 = selectionRingPoint(rb, rx, ry, std::cos(t0), std::sin(t0), body, cosF, sinF);
                        const SDL_FPoint p1 = selectionRingPoint(rb, rx, ry, std::cos(t1), std::sin(t1), body, cosF, sinF);
                        pushSeg(overlayBatch_,
                                ox0 + ox + p0.x * zms, oy0 + p0.y * zms,
                                ox0 + ox + p1.x * zms, oy0 + p1.y * zms,
                                th, col);
                    }
                }
                // (Order markers are drawn by drawOrderTrails -- retail puts the
                // order kind's own animated CURSOR at each waypoint, not a ring.)
            }
            drawOrderTrails(mvw, winH);

#ifndef NDEBUG
            // TA_HITBOX=1: outline the box unitUnderCursor actually tests, so a
            // "I can't click its head" report can be measured instead of guessed at.
            if (ta::devEnv("TA_HITBOX")) {
                for (int selId : selection_) {
                    const UnitR* hp = frameUnitP(selId);
                    if (!hp || !hp->alive() || !hp->type) continue;
                    SDL_FPoint p = unitScreen(*hp);
                    const SDL_FRect& hb = unitHitBox(hp->type);
                    float hax = p.x, hay = p.y + 12.0f * zms;
                    SDL_FRect r{hax + hb.x * zms, hay + hb.y * zms,
                                hb.w * zms, hb.h * zms};
                    SDL_SetRenderDrawColor(ren_, 255, 40, 220, 255);
                    SDL_RenderDrawRectF(ren_, &r);
                    SDL_FRect a{p.x - 2, p.y - 2, 4, 4};   // the anchor itself
                    SDL_SetRenderDrawColor(ren_, 255, 230, 0, 255);
                    SDL_RenderFillRectF(ren_, &a);
                }
            }
#endif

            // No mark on an attack order's target: retail draws none (it only puts the
            // Attack cursor on it, with Shift held -- drawOrderTrails). Red corner
            // brackets drawn here were ours.
            if (!overlayBatch_.empty()) {
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
                SDL_RenderGeometry(ren_, nullptr, overlayBatch_.data(),
                                   int(overlayBatch_.size()), nullptr, 0);
            }
        }
        drawRangeCircles(mvw, winH);
        drawUnitSpots(mvw, winH);

        // Workstream L: each per-unit overlay below examines only the units the grid
        // returns for its own screen band -- the band its exact test accepts for the
        // lifted anchor, shifted by the overlay's offset -- in live order, so batches
        // keep their order; every exact test is unchanged. accept(u, draw) is the
        // overlay: it returns whether u is drawn and draws it when `draw`.
        // TA_UNIT_INDEX_VERIFY re-runs each test over every unit without drawing and
        // compares the accepted ids, in order.
        namespace unitpass = ta::render::unitpass;
        const ta::render::AnchorView overlayView{mapView_.offX(), mapView_.offY(), zm, mvw, winH};
        const auto overlayPass = [&](int kind, const ta::render::ScreenBand& band, auto&& accept) {
            const auto& live = front().live;
            const auto indices = unitCandidates(band);
            (void)kind;
#ifndef NDEBUG
            if (unitIndexVerify_) {
                overlayIdsIndexed_.clear(); overlayIdsFull_.clear();
                for (uint32_t i : indices) if (accept(*live[i], true)) overlayIdsIndexed_.push_back(live[i]->id);
                for (const UnitR* up : live) if (accept(*up, false)) overlayIdsFull_.push_back(up->id);
                if (overlayIdsIndexed_ != overlayIdsFull_)
                    throw std::runtime_error("unit index verify: an overlay's units differ from a full scan");
                ++unitIndexVerifyStats_.passes;
                unitIndexVerifyStats_.live += live.size();
                unitIndexVerifyStats_.candidates += indices.size();
                unitIndexVerifyStats_.accepted += overlayIdsFull_.size();
                unitIndexVerifyStats_.overlays[kind] += overlayIdsFull_.size();
                return;
            }
#endif
            for (uint32_t i : indices) accept(*live[i], true);
        };
        // Squad markers sit below the unit by a footprint-dependent offset: their
        // band covers every type's footprint depth.
        const auto [footMin, footMax] = unitFootZRange();
        // The bar, the countdown and the squad number ride a flyer at its altitude
        // (altLift, as the unit and its ring are drawn), so a band also takes the
        // ground anchors up to the highest flyer altitude below it.
        const float airLift = front().flyerUnits.empty() ? 0.0f
                            : (std::max(flyerAltitudeSeen_, pickBounds().cruiseAlt) * kProjY + 1.0f) * zm;
        const auto withAir = [&](ta::render::ScreenBand b) { b.bottom += airLift; return b; };

        // Health bars -- viewport-culled and batched into one draw call (each was two
        // state-changing FillRects, so a damaged crowd used to break the render batch
        // thousands of times a frame). A unit being built or conjured gets its bar
        // like any other (its HP grows with the work), and it is the ONLY progress
        // shown in the world: retail draws no bar over a factory or a builder (the
        // KINGDOMS.icd unit-overlay loop draws only the damage bar and the squad
        // number), so the producer's progress lives in the info panel instead.
        overlayBatch_.clear();
#ifndef NDEBUG
        debugHealthBarIds_.clear();
#endif
        // Tactical Dots: no per-unit bars. Options "HEALTH BARS": 0 = never, 1 = only
        // damaged units (default), 2 = every unit, full HP included. (Retail has one
        // toggle, off by default, that bars every own unit on screen.)
        if (!dotsFrame_ && healthBars_ != 0)
            overlayPass(0, withAir(unitpass::healthBarBand(overlayView)), [&](const UnitR& u, bool draw) {
            if (!u.alive() || u.embarked() || !u.type) return false;
            if (u.underConstruction && !u.buildBegun) return false;   // ghost: no bar
            if (!unitShownR(u)) return false;
            float frac = std::clamp(u.hp / u.type->maxHp, 0.0f, 1.0f);
            if (healthBars_ == 1 && frac >= 1.0f) return false;
            float bw = 26 * zm, bh = std::max(2.0f, 3 * zm);
            const auto [bx, by] = unitpass::healthBar(u.x, u.z, uLiftX(u), uLiftY(u) + altLift(u), overlayView);
            if (!unitpass::healthBarInView({bx, by}, overlayView)) return false;
            if (!draw) return true;
#ifndef NDEBUG
            debugHealthBarIds_.push_back(u.id);
#endif
            pushQuad(overlayBatch_, bx - 1, by - 1, bw + 2, bh + 2,
                     SDL_Color{10, 10, 10, 220});
            pushQuad(overlayBatch_, bx, by, bw * frac, bh,
                     SDL_Color{uint8_t(230 * (1 - frac) + 40 * frac),
                               uint8_t(200 * frac + 40 * (1 - frac)), 40, 255});
            return true;
        });
        if (!overlayBatch_.empty()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(ren_, nullptr, overlayBatch_.data(),
                               int(overlayBatch_.size()), nullptr, 0);
        }

        // Self-destruct countdown: a pulsing red number over each armed unit. This and
        // the squad numbers below are UI, left out of a poster.
        if (hudFont_.ok() && !posterFrame_)
            overlayPass(1, withAir(unitpass::selfDestructBand(overlayView)), [&](const UnitR& u, bool draw) {
                if (!u.alive() || u.embarked() || !u.type || u.selfDestructT < 0) return false;
                if (!unitShownR(u)) return false;
                const auto [sx, sy] = unitpass::selfDestruct(u.x, u.z, uLiftX(u), uLiftY(u) + altLift(u), overlayView);
                if (!unitpass::selfDestructInView({sx, sy}, overlayView)) return false;
                if (!draw) return true;
                char b[8];
                std::snprintf(b, sizeof b, "%d", int(std::ceil(u.selfDestructT)));
                bool bright = (u.selfDestructT - std::floor(u.selfDestructT)) > 0.5f;
                SDL_Color c{255, uint8_t(bright ? 200 : 60), 40, 255};
                hudFont_.draw(ren_, b, sx - 5, sy, 2.2f, c);
                return true;
            });

        // Control-squad marker: retail-style bright-green number centred BELOW the
        // unit (under where the selection ring/bar sits), "<N>F" for a formation.
        // The number is the recall key (squad 10 shows as "0"). Skipped when zoomed
        // far out so it doesn't clutter the field. Drawn in FLAT green: tinting the
        // shaded tan glyph art gave a dark, mottled outline that was hard to read.
        if (hudFont_.ok() && zm > 0.55f && !posterFrame_)
            overlayPass(2, withAir(unitpass::squadMarkerBand(overlayView, footMin, footMax)), [&](const UnitR& u, bool draw) {
                if (!u.alive() || u.embarked() || !u.type || u.player != hudOwner ||
                    u.squad == 0)
                    return false;
                if (u.underConstruction && !u.buildBegun) return false;
                const auto [cx, cy] = unitpass::squadMarker(u.x, u.z, uLiftX(u), uLiftY(u) + altLift(u), u.type->footZ, overlayView);
                if (!unitpass::squadMarkerInView({cx, cy}, overlayView)) return false;
                if (!draw) return true;
                int num = std::abs(int(u.squad));
                char key = (num == 10) ? '0' : char('0' + num);
                std::string lbl(1, key);
                if (u.squad < 0) lbl += 'F';   // formation
                float sc = std::clamp(1.1f * zm, 0.9f, 1.7f);
                float tw = float(hudFont_.width(lbl, sc));
                hudFont_.drawSolid(ren_, lbl, cx - tw / 2, cy, sc,
                                   SDL_Color{70, 240, 90, 255});   // the selection-UI green
                return true;
            });

        // Restore the un-shaken camera so the HUD/panel stays rock-steady.
        if (shaking) mapView_.setOffset(shakeBaseX, shakeBaseY);
        // A MakePoster tile (posterStep) is the world alone.
        if (posterFrame_) { SDL_RenderSetClipRect(ren_, nullptr); return; }

        // Done with world-space: drop the clip and draw the right-hand panel and
        // its minimap + order column on a solid strip (never over the map).
        SDL_RenderSetClipRect(ren_, nullptr);
        // Neutral panel strip behind the minimap (kept for spectators too -- it's plain
        // chrome, not the faction art). The bottom bar (drawPanel) and TA's sidebar
        // page and top bar (renderTaHud) are PLAYER-only: a spectator isn't a player
        // and gets neither, just the minimap and the F4 board.
        SDL_SetRenderDrawColor(ren_, 16, 14, 12, 255);
        // The strip normally stops short to leave room for the player's bottom
        // InfoPanel bar; a spectator has no bottom bar, so it must reach the
        // bottom edge (else a barH()-tall gap shows under the right panel).
        float stripH = hudShown() ? float(winH) - barH() : float(winH);
        SDL_FRect panelStrip{float(mvw), 0, float(winW - mvw), stripH};
        SDL_RenderFillRectF(ren_, &panelStrip);
        {
            const double _t0 = double(SDL_GetPerformanceCounter());
            drawMinimap(winW, winH);
            profHudMs_ += (double(SDL_GetPerformanceCounter()) - _t0) /
                          (double(SDL_GetPerformanceFrequency()) / 1000.0);
        }
        if (hudShown()) {
            drawPanel(winW, winH);
            renderTaHud(winW, winH);   // TA sidebar page + top bar (client/gameview_tahud.cpp)
        } else {   // nothing on screen to click
            iconRects_.clear();selPortraitRects_.clear();
            buildMenuRect_={};buildRowTop_=0;
        }
        // After the command panel: the readout fills the strip BETWEEN the two, and it
        // measures the panel's drawn position to find its own bottom edge.
        drawStatsPanel(winW, winH);
        // The top-left column (client/hudcorner.h), top down: the GAME TIME clock, the F4
        // scoreboard, the mission objectives, then (later) the voice speakers.
        if (front().scenarioClock) {
            const unsigned seconds=front().gameTick/30;
            char clock[64];
            std::snprintf(clock,sizeof clock,"GAME TIME %u:%02u:%02u",seconds/3600,(seconds/60)%60,seconds%60);
            const float y=topLeft_.place(32,28);
            const SDL_FRect background{8,y,blockWidth(clock,2.f)+12,28};
            SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren_,14,13,18,235);
            SDL_RenderFillRectF(ren_,&background);
            blockText(clock,14,y+6,2.f,{240,242,248,255});
        }
        if (showCounts_) drawUnitCounts(winW);
        if (hudShown()) drawObjectivesPanel(winW, winH);
        if (showHDebug_) drawHDebug();

        if (mp_ && hudFont_.ok()) {
            // Dev net-status readout (TA_NETDEBUG): off by default -- it sat over the
            // bottom-right panel. The BEHIND-BY lag warning below always shows.
            static const bool kNetDebug = ta::devEnv("TA_NETDEBUG") != nullptr;
            if (kNetDebug) {
                char nb[64];
                std::snprintf(nb, sizeof nb, "NET P%d  TICK %u", localPlayer_ + 1, netTick_);
                hudFont_.draw(ren_, nb, 12, float(winH) - barH() - 16, 1.4f,
                              {140, 200, 255, 255});
            }
            // "Behind by N s": how far this client's view lags the live game --
            // the depth of received-but-unplayed bundles (30 Hz). The adaptive
            // buffer keeps only a tiny intended reserve (~netDelay_ ticks, <0.2 s),
            // so surface this only once the lag is clearly abnormal: the machine
            // can't keep up, or a link stall is draining faster than it refills.
            // Only shown past 2 s so ordinary jitter never flashes the warning.
            // Escalates amber -> red toward the ~10 s server reconnect threshold.
            float behindSec = float(mp_->bufferedBundles()) / float(ta::net::kServerHz);
            if (behindSec > 2.0f && outcome_ == 0 && !paused_) {
                char bb[48];
                std::snprintf(bb, sizeof bb, "BEHIND BY %.1fs", behindSec);
                float t = std::min(1.0f, behindSec / 10.0f);
                SDL_Color col{255, uint8_t(210 - int(150 * t)), uint8_t(90 - int(60 * t)), 255};
                hudBanner(bb, 78, 1.8f, col, winW);
            }
            if (!netError_.empty())
                hudBanner("NETWORK: " + netError_, 150, 2.0f, {255, 140, 120, 255}, winW);
        }
        if (mp_ && !replayMode_) drawNetWaiting(winW);
        if (noticeTimer_ > 0 && hudFont_.ok() && !notice_.empty())
            hudBanner(notice_, replayMode_ ? std::max(120.0f, replayBar_.box.y + replayBar_.box.h + 8) : 120,
                      2.5f, {255, 230, 120, 255}, winW);
        if (paused_ && !replayMode_ && bigFont_.ok()) {   // a replay's bar says so

            const char* msg = "PAUSED";
            float tw = float(bigFont_.width(msg, 1.2f));
            bigFont_.draw(ren_, msg, (winW - tw) / 2, 60, 1.2f, {255, 230, 120, 255});
        }

        // Victory / defeat banner. A spectator/replay viewer isn't a participant,
        // so it names the winner instead of "VICTORY"/"DEFEAT". A watched game the
        // server ended (watchOver_) shows it whether or not anyone won; so does a replay
        // standing at the end of a recording that ended decided (replayResult()).
        if ((outcome_ != 0 || watchOver_ || replayResult()) && bigFont_.ok()) {
            std::string msg;
            SDL_Color col{255, 220, 90, 255};
            if (spectating_ || replayMode_) {
                int wt = frameWinningTeam();
                msg = wt >= 0 ? "TEAM " + std::to_string(wt + 1) + " WINS" : "GAME OVER";
            } else {
                msg = outcome_ > 0 ? "VICTORY" : "DEFEAT";
                if (outcome_ < 0) col = {255, 90, 70, 255};
            }
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_FRect shade{0, float(winH) / 2 - 60, float(winW), 120};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 150);
            SDL_RenderFillRectF(ren_, &shade);
            float tw = float(bigFont_.width(msg, 1.5f));
            bigFont_.draw(ren_, msg, (winW - tw) / 2, float(winH) / 2 + 24, 1.5f, col);
            const char* hint = (!spectating_ && !replayMode_ && missionStem_.empty()) || watchOver_
                ? "RESULTS IN 3 SECONDS" : "PRESS ESC FOR MENU";
            blockText(hint, (winW - blockWidth(hint, 2.0f)) / 2, float(winH) / 2 + 74, 2.0f,
                      {220, 220, 230, 255});
        }

        // Marquee only once the drag covers real area: under the same small
        // threshold the release is treated as a CLICK-select, so a couple of
        // pixels of hand jitter shouldn't flash a box. Its press corner stays on
        // the map when the camera moves (client/areadrag.h).
        if (const auto a = dragStartOnScreen(dragAnchor_);
            dragging_ && (armedPress_ ? areaBoxAt(dragAnchor_, dragX1_, dragY1_)
                                      : ta::client::areadrag::isSelectBox(dragAnchor_, a, dragX1_, dragY1_, uiScale_))) {
            SDL_FRect r{std::min(a.x, dragX1_), std::min(a.y, dragY1_),
                        std::abs(dragX1_ - a.x), std::abs(dragY1_ - a.y)};
            SDL_SetRenderDrawColor(ren_, 120, 255, 150, 200);
            SDL_RenderDrawRectF(ren_, &r);
        }

        // Spectator badge: a live watcher can pan/zoom but issues no orders. Use the
        // crisp 5x7 block font (the scaled GAF hudFont smeared/overlapped here).
        if (spectating_) {
            std::string m = "SPECTATING";
            if (benchmarkMode_) {   // append a countdown from the benchmark duration to 0
                // Real seconds left of the run (the server ends it on its own clock).
                int rem = front().benchEnded ? 0 : std::max(0, ta::sim::kBenchmarkSeconds - benchmarkElapsedSec());
                char buf[40];
                std::snprintf(buf, sizeof buf, "BENCHMARKING  %d:%02d", rem / 60, rem % 60);
                m = buf;
            }
            float px = 3.0f;
            float tw = blockWidth(m, px), th = 7 * px;
            float bx = (winW - tw) / 2, by = 24;
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_FRect bg{bx - 14, by - 8, tw + 28, th + 16};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 150);
            SDL_RenderFillRectF(ren_, &bg);
            blockText(m, bx, by, px, {235, 175, 110, 255});
        }

        if (replayMode_) drawReplayBar();

        drawVoiceIndicator(winW, winH);   // who is talking (voice chat), top-left

        drawGameChat(winW, winH);   // in-game chat box (Options: CHAT POSITION / SCALE)

        // In-game exit menu overlay -- drawn last so it sits above the HUD. Screen
        // space (like the game-over banner); hit-rects are rebuilt here each frame
        // and consumed by input() (see the exitMenu_ branch).
        if (giveUnitsMenu_) drawGiveUnitsMenu(winW, winH);
        if (exitMenu_) {
            exitHots_.clear();
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_FRect dim{0, 0, float(winW), float(winH)};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 150);
            SDL_RenderFillRectF(ren_, &dim);

            // A seated player can give up: the server takes it as a Leave, so the units
            // go the room's forfeit way, and main() shows the defeat's result screen.
            const bool canSurrender = canReturnToMenu_ && mp_ && !watchOnly() && outcome_ == 0 &&
                                      mp_->state() == ta::net::MpClient::State::InGame;
            // RESUME, GAME, GRAPHICS, USER INTERFACE, AUDIO, VOICE CHAT, CONTROLS and YOUTUBE
            // STREAMING, then SURRENDER, MAIN MENU or (launched without the menu) QUIT.
            const int nBtn = 8 + (canSurrender ? 1 : 0) + (canReturnToMenu_ && !canSurrender ? 1 : 0) +
                             (!canReturnToMenu_ ? 1 : 0);
            const float bw = 280, pad = 32, titlePx = 3.0f;
            const float gap = winH < 640 ? 6.0f : 14.0f;   // tighter on a short window
            const float titleH = 7 * titlePx + 24;
            // Buttons shrink on a short window (the minimum is 480 high) rather than spill.
            const float bh = std::clamp((float(winH) - 2 * pad - titleH - 24 - (nBtn - 1) * gap) / float(nBtn), 30.0f, 50.0f);
            const float pw = bw + pad * 2;
            const float ph = pad * 2 + titleH + nBtn * bh + (nBtn - 1) * gap;
            const float px0 = (winW - pw) / 2, py0 = (winH - ph) / 2;

            SDL_FRect panel{px0, py0, pw, ph};
            SDL_SetRenderDrawColor(ren_, 26, 28, 36, 240);
            SDL_RenderFillRectF(ren_, &panel);
            SDL_SetRenderDrawColor(ren_, 120, 130, 160, 255);
            SDL_RenderDrawRectF(ren_, &panel);

            const char* title = "GAME MENU";
            blockText(title, px0 + (pw - blockWidth(title, titlePx)) / 2, py0 + pad, titlePx,
                      {235, 225, 180, 255});

            float bx = px0 + pad, by = py0 + pad + titleH;
            auto btn = [&](const std::string& label, std::function<void()> action) {
                SDL_FRect r{bx, by, bw, bh};
                bool hot = mouseX_ >= r.x && mouseX_ <= r.x + r.w &&
                           mouseY_ >= r.y && mouseY_ <= r.y + r.h;
                SDL_SetRenderDrawColor(ren_, hot ? 90 : 60, hot ? 110 : 66, hot ? 150 : 86, 255);
                SDL_RenderFillRectF(ren_, &r);
                SDL_SetRenderDrawColor(ren_, hot ? 180 : 90, hot ? 200 : 100, hot ? 240 : 130, 255);
                SDL_RenderDrawRectF(ren_, &r);
                float tpx = 2.5f, tw = blockWidth(label, tpx);
                blockText(label, bx + (bw - tw) / 2, by + (bh - 7 * tpx) / 2, tpx,
                          {228, 232, 242, 255});
                exitHots_.push_back({r, std::move(action)});
                by += bh + gap;
            };
            btn("RESUME", [this] { exitMenu_ = false; surrenderArmed_ = false; });
            btn("GAME", [this] { exitMenu_ = false; openOptions(ta::OptionsPage::Game); });
            btn("GRAPHICS", [this] { exitMenu_ = false; openOptions(ta::OptionsPage::Graphics); });
            btn("USER INTERFACE", [this] { exitMenu_ = false; openOptions(ta::OptionsPage::Interface); });
            btn("AUDIO", [this] { exitMenu_ = false; openAudioOptions(); });
            btn("VOICE CHAT", [this] { exitMenu_ = false; openVoiceOptions(); });
            btn("CONTROLS", [this] { exitMenu_ = false; openHotkeys(); });   // hotkey rebinding
            btn("YOUTUBE STREAMING", [this] {
                exitMenu_ = false;
                SDL_Event event{};event.type=SDL_USEREVENT;event.user.code=ta::kStreamingEvent;
                SDL_PushEvent(&event);
            });
            if (canSurrender) btn(surrenderArmed_ ? "CONFIRM SURRENDER" : "SURRENDER", [this] {
                if (!std::exchange(surrenderArmed_, true)) return;
                outcome_ = -1;   // main() sends the Leave on the way out
                exitMenu_ = false;
                menuRequested_ = true;
            });
            if (canReturnToMenu_ && !canSurrender) btn("MAIN MENU", [this] { menuRequested_ = true; });
            if (!canReturnToMenu_) btn("QUIT", [this] { quitRequested_ = true; });
        }
        drawUnitInfo(winW, winH);   // retail Unit Info dialog, above the HUD
        if (benchStatsShown_) renderBenchmarkStats(winW, winH);   // benchmark results, above the frozen game
        if (options_) options_->render(winW, winH);   // topmost of all
        if (hotkeysScreen_) hotkeysScreen_->render(winW, winH);   // above Options
    }

    // The replay transport, in the game's own menu colours and plain interface text, on a
    // solid fill so it reads the same over any terrain.
    void GameView::drawReplayBar() {
        namespace rb = ta::replaybar;
        const rb::Layout& l = replayBar_;
        if (l.box.w <= 0) return;
        const auto rect = [](const rb::Rect& r) { return SDL_FRect{r.x, r.y, r.w, r.h}; };
        const auto fill = [&](const SDL_FRect& r, SDL_Color c) {
            SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
            SDL_RenderFillRectF(ren_, &r);
        };
        const auto frame = [&](const SDL_FRect& r, SDL_Color c) {
            SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, c.a);
            SDL_RenderDrawRectF(ren_, &r);
        };
        const auto triangle = [&](SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_Color color) {
            const SDL_Vertex v[3] = {{a, color, {}}, {b, color, {}}, {c, color, {}}};
            SDL_RenderGeometry(ren_, nullptr, v, 3, nullptr, 0);
        };
        constexpr SDL_Color kText{228, 232, 242, 255}, kGold{235, 205, 110, 255};
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        fill(rect(l.box), {26, 28, 36, 240});
        frame(rect(l.box), {120, 130, 160, 255});

        // The timeline: elapsed in gold, a knob on the playhead, or on where a jump is
        // going, or where the drag has it. Hovering reads the time under the pointer.
        const size_t length = replayLength();
        const float pixel = std::max(1.0f, uiScale_);
        fill(rect(l.track), {14, 15, 20, 255});
        SDL_FRect elapsed = rect(l.track);
        elapsed.w = rb::tickToX(l, replayTick_, length) - l.track.x;
        fill(elapsed, kGold);
        frame(rect(l.track), {90, 100, 130, 255});
        const size_t marked = replayDragging_ ? replayDragTick_ : replaySeekTo_.value_or(replayTick_);
        const float knobW = 5 * pixel, knobX = rb::tickToX(l, marked, length);
        const SDL_FRect knob{knobX - knobW / 2, l.track.y - 3 * pixel, knobW, l.track.h + 6 * pixel};
        fill(knob, {245, 240, 225, 255});
        frame(knob, {60, 66, 86, 255});

        // Row 2: flat buttons drawn as shapes, so they look the same in either UI font.
        const auto button = [&](const rb::Rect& r) {
            const SDL_FRect f = rect(r);
            const bool hot = !exitMenu_ && r.hit(mouseX_, mouseY_);
            fill(f, {Uint8(hot ? 90 : 60), Uint8(hot ? 110 : 66), Uint8(hot ? 150 : 86), 255});
            frame(f, {Uint8(hot ? 180 : 90), Uint8(hot ? 200 : 100), Uint8(hot ? 240 : 130), 255});
        };
        for (const rb::Rect* r : {&l.back, &l.play, &l.forward, &l.slower, &l.faster}) button(*r);
        const auto chevrons = [&](const rb::Rect& r, float dir) {
            const float h = r.h * 0.42f, w = h * 0.8f, cy = r.y + r.h / 2;
            for (int i = 0; i < 2; ++i) {
                const float x = r.x + r.w / 2 + dir * (float(i) - 1) * w;
                triangle({x, cy - h / 2}, {x, cy + h / 2}, {x + dir * w, cy}, kText);
            }
        };
        chevrons(l.back, -1);
        chevrons(l.forward, 1);
        {
            const float h = l.play.h * 0.46f, cx = l.play.x + l.play.w / 2, cy = l.play.y + l.play.h / 2;
            if (paused_) {
                triangle({cx - h * 0.4f, cy - h / 2}, {cx - h * 0.4f, cy + h / 2}, {cx + h * 0.5f, cy}, kText);
            } else {
                const float w = h * 0.3f;
                fill({cx - w * 1.4f, cy - h / 2, w, h}, kText);
                fill({cx + w * 0.4f, cy - h / 2, w, h}, kText);
            }
        }
        const auto dash = [&](const rb::Rect& r, bool plus) {
            const float w = r.h * 0.42f, t = std::max(2.0f, r.h * 0.11f);
            const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
            fill({cx - w / 2, cy - t / 2, w, t}, kText);
            if (plus) fill({cx - t / 2, cy - w / 2, t, w}, kText);
        };
        dash(l.slower, false);
        dash(l.faster, true);
        const float px = l.textPx, textH = 7 * px;
        const std::string speed = rb::speedText(speedMult(), replayRealRate_);
        const float speedPx = px * std::min(1.0f, l.speed.w / blockWidth(speed, px));   // the long ones shrink to fit
        blockText(speed, l.speed.x + (l.speed.w - blockWidth(speed, speedPx)) / 2,
                  l.speed.y + (l.speed.h - 7 * speedPx) / 2, speedPx, {235, 225, 180, 255});
        // Hours make the label longer than a narrow bar leaves room for: it shrinks to fit.
        const std::string time = rb::timeText(replayTick_, length) + " / " + rb::timeText(length, length);
        const float timePx = px * std::min(1.0f, rb::timeRoom(l) / std::max(1.0f, blockWidth(time, px)));
        blockText(time, l.timeRight - blockWidth(time, timePx), l.speed.y + (l.speed.h - 7 * timePx) / 2, timePx, kText);

        if (const bool hover = !exitMenu_ && l.timeline.hit(mouseX_, mouseY_); hover || replayDragging_) {
            const size_t tick = replayDragging_ ? replayDragTick_ : rb::xToTick(l, mouseX_, length);
            const std::string at = rb::timeText(tick, length);
            const float w = blockWidth(at, px) + 12 * pixel, h = textH + 8 * pixel;
            const float x = std::clamp(rb::tickToX(l, tick, length) - w / 2, l.box.x, l.box.x + l.box.w - w);
            const SDL_FRect tip{x, l.box.y + l.box.h + 2 * pixel, w, h};
            fill(tip, {26, 28, 36, 240});
            frame(tip, {120, 130, 160, 255});
            blockText(at, x + 6 * pixel, tip.y + 4 * pixel, px, kText);
        }
    }

    // Render thread, before the parallel geometry pass: every visible unit, in
    // painter order, resolves the geometry entry it owns (client/geometryowners.h),
    // then entries of destroyed / long-absent units are reclaimed. Depth order only
    // decides the order of visGeom_; it never moves a unit to another entry.
    // Debug calibration (TA_SCHEDULE_SAMPLES=<file>, render telemetry on): one
    // line per planned pass -- kind, items, chunks, participants, estimate and
    // measured wall time (ns). Run with TA_RENDER_POOL_THREADS=1 for serial
    // per-pass costs to fit the rates in client/workschedule.h against.
    void GameView::runPlanned(char kind, const ta::render::WorkPlan& plan,
                              const std::function<void(size_t,size_t)>& fn) {
#ifndef NDEBUG
        static const char* const samples = ta::devEnv("TA_SCHEDULE_SAMPLES");
        if (samples && renderStats_.enabled()) {
            const auto t0 = std::chrono::steady_clock::now();
            pool_.parallelChunks(plan.bounds(), plan.participants(), fn);
            const double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
            if (FILE* f = std::fopen(samples, "a")) {
                std::fprintf(f, "%c %zu %zu %u %.0f %.0f\n", kind, plan.items(), plan.chunks(),
                             plan.participants(), plan.estimateNs(), ns);
                std::fclose(f);
            }
            return;
        }
#else
        (void)kind;
#endif
        pool_.parallelChunks(plan.bounds(), plan.participants(), fn);
    }

#ifndef NDEBUG
    void GameView::verifyPlan(const ta::render::WorkPlan& plan, size_t items) const {
        const auto b = plan.bounds();
        bool ok = !b.empty() && b.front() == 0 && b.back() == items && plan.items() == items &&
                  plan.participants() >= 1;
        for (size_t k = 1; ok && k < b.size(); ++k) ok = b[k] > b[k - 1];
        if (!ok) throw std::runtime_error("schedule verify: malformed work plan");
    }
#endif

    void GameView::readSchedules() {
        if (schedulesRead_) return;
        schedulesRead_ = true;
#ifndef NDEBUG
        if (const char* v = ta::devEnv("TA_RENDER_SCHEDULE")) scheduleUniform_ = std::strcmp(v, "uniform") == 0;
        scheduleVerifyOn_ = ta::devFlag("TA_SCHEDULE_VERIFY");
        const auto policy = [](const char* name, ta::render::SchedulePolicy& p) {
            const char* v = ta::devEnv(name);
            if (!v) return;
            double a = 0, b = 0, c = 0; unsigned m = p.maxParticipants;
            const int n = std::sscanf(v, "%lf,%lf,%lf,%u", &a, &b, &c, &m);
            if (n >= 3) { p.serialBelowNs = a; p.workerNs = b; p.chunkNs = c; p.maxParticipants = n >= 4 ? m : p.maxParticipants; }
        };
        policy("TA_SCHEDULE_GEOMETRY", schedules_.geometry);
        policy("TA_SCHEDULE_SHADOW_CHECK", schedules_.shadowCheck);
        policy("TA_SCHEDULE_SHADOW_PREP", schedules_.shadowPrep);
        policy("TA_SCHEDULE_COPY", schedules_.bodyCopy);
#endif
    }

    // Workstream L: the lift (world px, subtracted from the position before the zoom)
    // any unit's anchor can carry. Ground units sample heightAbove (the absolute
    // terrain height); a flyer rides its datum, the dilated sector maximum of the same
    // heights floored at the sea; a wading or floating body rides at most the sea. So
    // all are at most max(highest height, sea level). Cached per map and height settings.
    ta::render::LiftBounds GameView::unitLiftBounds() {
        const auto& m = mapView_.map();
        if (!liftReady_) (void)heightAbove(0, 0);   // reads the debug scales
        const LiftBoundsKey key{&m, m.heights.data(), m.heights.size(), kHeightScale_, kHeightScaleX_};
        if (key.map != unitLiftKey_.map || key.heights != unitLiftKey_.heights || key.size != unitLiftKey_.size ||
            key.scale != unitLiftKey_.scale || key.scaleX != unitLiftKey_.scaleX) {
            unitLiftKey_ = key;
            int top = 0;
            for (uint8_t v : m.heights) top = std::max(top, int(v));
            // +1: margin over the largest corner (the sampler never exceeds it).
            const float above = m.heights.empty() || m.width <= 0 ? 0.0f
                                : float(std::max(top, m.seaLevel)) + 1.0f;
            const float y = above * kHeightScale_, x = above * kHeightScaleX_;
            unitLift_ = {std::min(0.0f, x), std::max(0.0f, x), std::min(0.0f, y), std::max(0.0f, y)};
        }
        return unitLift_;
    }

    // Workstream L: indices into front().live (ascending) of every unit whose lifted
    // anchor -- (x - offX) * zoom - liftX * zoom, (z - offY) * zoom - liftY * zoom with
    // the snapshot position -- may fall inside `band` (screen px, inclusive). A
    // superset: callers keep their exact test. Valid until the next call.
    // Workstream L: the smallest and largest footprint depth (footZ) of any unit
    // type, for the overlays whose offset from the anchor depends on it.
    std::pair<int, int> GameView::unitFootZRange() {
        const auto& types = registry_.types();
        if (unitFootKey_.first != &types || unitFootKey_.second != types.size()) {
            unitFootKey_ = {&types, types.size()};
            int lo = 1, hi = 1;
            bool any = false;
            for (const auto& [id, type] : types) {
                lo = any ? std::min(lo, type.footZ) : type.footZ;
                hi = any ? std::max(hi, type.footZ) : type.footZ;
                any = true;
            }
            unitFoot_ = {lo, hi};
        }
        return unitFoot_;
    }

    std::span<const uint32_t> GameView::unitCandidates(const ta::render::ScreenBand& band) {
        using RS = ta::render::Counter;
        const Frame& f = front();
        renderStats_.add(RS::UnitIndexQueries);
        // The whole list, in order (kept as an identity table, grown as needed).
        const auto everything = [&] {
            const size_t n = f.live.size();
            for (size_t i = unitAll_.size(); i < n; ++i) unitAll_.push_back(uint32_t(i));
            renderStats_.add(RS::UnitIndexReturned, n);
            return std::span<const uint32_t>(unitAll_.data(), n);
        };
        if (unitFullScan_ || f.unitGrid.size() != f.live.size()) {
            renderStats_.add(RS::UnitIndexFullScans);
            return everything();
        }
        const auto rect = ta::render::anchorBandToWorld(band.left, band.top, band.right, band.bottom, mapView_.offX(),
                                                         mapView_.offY(), mapView_.zoom(), unitLiftBounds());
        // A view holding every unit (the 10,000-visible case) needs no gather.
        if (f.unitGrid.covers(rect.left, rect.top, rect.right, rect.bottom)) return everything();
        ta::render::UnitGrid::QueryStats qs;
        f.unitGrid.query(rect.left, rect.top, rect.right, rect.bottom, unitQueryBits_, unitQuery_, &qs);
        renderStats_.add(RS::UnitIndexReturned, unitQuery_.size());
        renderStats_.add(RS::UnitIndexRows, qs.rows);
        return unitQuery_;
    }

    void GameView::claimGeometryEntries() {
#ifndef NDEBUG
        // Debug: shorter retention / trim windows, so a short verification run
        // recycles, expires and trims entries (render_geometry_ownership).
        static const bool configured = [this] {
            auto config = geomEntries_.config();
            if (const char* v = ta::devEnv("TA_GEOMETRY_RETAIN_MS")) config.retainMs = uint64_t(std::max(0, std::atoi(v)));
            if (const char* v = ta::devEnv("TA_GEOMETRY_TRIM_MS")) config.freeTrimMs = uint64_t(std::max(0, std::atoi(v)));
            if (ta::devEnv("TA_GEOMETRY_RETAIN_MS")) config.retainMinFrames = 2;
            geomEntries_.setConfig(config);
            return true;
        }();
        (void)configured;
#endif
        // Presentation clock: the study's fixed animation clock, wall time otherwise
        // (the same choice as the distant-image refresh).
        geomEntries_.beginFrame(ta::devFlag("TA_RENDER_STUDY") ? uint64_t(animClock_*1000) : SDL_GetTicks64());
        const Frame& frame = front();
        visUnits_.clear();
        visGeom_.clear();
        visCost_.clear();
        geomIndex_.assign(frame.units.size(), -1);   // compact frame slot -> visible index
        for (const auto& it : paintItems_) if (it.u) {
            const int slot = frame.slot(it.u->id);
            if (slot < 0) continue;
            ta::render::RenderHandle handle;
            UnitGeom* g = geomEntries_.claim({it.u->id, frame.worldEpoch, it.u->type, it.u->replacementModel}, &handle);
            if (!g) continue;   // duplicate owner in one frame (never expected)
            g->handle = handle;
            geomIndex_[size_t(slot)] = int(visUnits_.size());
            visUnits_.push_back(it.u);
            visGeom_.push_back(g);
            visCost_.push_back(g->nextCost);   // same cache line as handle (workstream I)
        }
        // Alive = still in this snapshot under the same identity. Corpses keep their
        // unit; a unit that switched model or type is a new identity.
        geomEntries_.reclaim([&](const ta::render::UnitOwner& o) {
            if (o.epoch != frame.worldEpoch) return false;
            const UnitR* u = frame.unit(o.id);
            return u && u->type == o.type && u->replacementModel == o.model;
        });
    }

#ifndef NDEBUG
    // TA_GEOMETRY_OWNERSHIP_VERIFY: every visible unit holds a distinct entry
    // owned by exactly its identity, and no entry carries another unit's build.
    void GameView::verifyGeometryOwnership() {
        const Frame& frame = front();
        const auto fail = [](const char* what) { throw std::runtime_error(std::string("geometry ownership: ") + what); };
        if (visGeom_.size() != visUnits_.size()) fail("painter references out of step");
        std::vector<const UnitGeom*> entries(visGeom_.begin(), visGeom_.end());
        std::sort(entries.begin(), entries.end());
        if (std::adjacent_find(entries.begin(), entries.end()) != entries.end()) fail("two units share an entry");
        for (size_t i = 0; i < visUnits_.size(); ++i) {
            const UnitR& u = *visUnits_[i];
            const UnitGeom& g = visGeom(i);
            const auto* owner = geomEntries_.ownerOf(&g);
            if (!owner) fail("visible unit holds a released entry");
            if (owner->id != u.id || owner->epoch != frame.worldEpoch || owner->type != u.type ||
                owner->model != u.replacementModel) fail("entry owned by another identity");
            if (!geomEntries_.valid(g.handle) || geomEntries_.handleOf(u.id) != g.handle) fail("stale entry handle");
            // Content from a previous build must be this unit's (or none: cold).
            if (g.owner >= 0 && g.owner != u.id) fail("entry carries another unit's geometry");
            if (!g.localKey.empty() && g.localKey[0] != uint64_t(u.id)) fail("entry key names another unit");
            const int slot = frame.slot(u.id);
            if (slot < 0 || geomIndex_[size_t(slot)] != int(i)) fail("slot index out of step");
        }
        const auto& s = geomEntries_.stats();
        auto& t = ownershipTotals_;
        t.acquired += s.acquired; t.replaced += s.replaced; t.duplicates += s.duplicates;
        t.releasedDestroyed += s.releasedDestroyed; t.releasedExpired += s.releasedExpired;
        t.releasedBudget += s.releasedBudget; t.trimmed += s.trimmed; t.chunksFreed += s.chunksFreed;
        t.refused += s.refused;
        t.visible = std::max(t.visible, s.visible); t.retained = std::max(t.retained, s.retained);
        t.allocated = std::max(t.allocated, s.allocated);
        ++ownershipChecks_;
    }
#endif

    // Render thread, after the parallel geometry pass: fold the workers' per-entry
    // results into this frame's stats, and account the retained geometry pool.
    void GameView::tallyRenderStats() {
        using RS = ta::render::Counter;
        using MC = ta::render::MissCause;
        using RW = ta::render::Work;
        renderStats_.add(RS::VisibleUnits, visUnits_.size());
        uint64_t keyNs = 0, buildNs = 0;
        for (size_t i = 0; i < visUnits_.size(); ++i) {
            const UnitGeom& g = visGeom(i);
            keyNs += g.keyNs;
            // Workstream G: bodies held back behind a held distant image, and
            // pending bodies generated now (body stages run beyond this frame's).
            const size_t bodyVerts = g.verts.size();
            renderStats_.add(RS::DistantBodyDeferred, g.bodyDeferred);
            renderStats_.add(RS::DistantBodyResumed, (g.bodyRun & ~g.stages) ? 1u : 0u);
            if (g.reused) {
                renderStats_.add(RS::GeometryHits);
                if (g.bodyRun) buildNs += g.buildNs;
                if (g.bodyRun & ta::render::kStageLocal) renderStats_.add(RS::VertsGenerated, bodyVerts);
                continue;
            }
            if (g.localKey.empty()) continue;   // nothing drawable (no model / unbegun site)
            buildNs += g.buildNs;
            renderStats_.add(RS::GeometryMisses);
            renderStats_.addMiss(MC(g.missCause));
            renderStats_.add(RS::GeometryOwnerReassign, g.ownerChanged);
            renderStats_.add(RS::GeomReallocs, g.reallocs);
            const auto changed = [&](MC c) { return (g.missMask & ta::render::missBit(c)) ? 1u : 0u; };
            renderStats_.add(RS::InvalPose, changed(MC::Pose));
            renderStats_.add(RS::InvalMaterial, changed(MC::Material));
            renderStats_.add(RS::InvalPlacement, changed(MC::Placement));
            renderStats_.add(RS::InvalZoom, changed(MC::Zoom));
            renderStats_.add(RS::InvalResource, changed(MC::Resource));
            renderStats_.add(RS::InvalSettings, changed(MC::Settings));
            // Work done (workstream D): a local rebuild generates every vertex; a
            // partial update rewrites positions and/or colours of retained ones.
            const size_t shadowVerts = g.shadowVerts.size() + g.maskedShadowVerts.size();
            if (g.stages & ta::render::kStageLocal) {
                renderStats_.add(RS::GeomLocalBuilds);
                renderStats_.add(RS::GeomShadowKept, g.shadowKept);
                renderStats_.add(RS::GeomShadowSkipped, g.shadowSkipped);
                if (g.bodyRun & ta::render::kStageLocal) renderStats_.add(RS::VertsGenerated, bodyVerts);
                renderStats_.add(RS::ShadowVertsGenerated, shadowVerts);
            } else {
                if (g.bodyRun & ta::render::kStageLocal) renderStats_.add(RS::VertsGenerated, bodyVerts);
                if (g.stages & ta::render::kStagePlacement) {
                    renderStats_.add(RS::GeomPlacementOnly);
                    renderStats_.add(RS::VertsPlaced, shadowVerts +
                        ((g.bodyRun & ta::render::kStagePlacement) ? bodyVerts : 0));
                }
                if (g.stages & ta::render::kStageMaterial) {
                    renderStats_.add(RS::GeomMaterialOnly);
                    if (g.bodyRun & ta::render::kStageMaterial) renderStats_.add(RS::VertsRecoloured, bodyVerts);
                }
            }
        }
        renderStats_.addWorkNs(RW::GeometryKey, keyNs);
        renderStats_.addWorkNs(RW::GeometryBuild, buildNs);
        size_t used = 0, capacity = 0;
        const auto account = [&](const auto& v) {
            used += v.size() * sizeof(v[0]);
            capacity += v.capacity() * sizeof(v[0]);
        };
        geomEntries_.forEachPayload([&](const UnitGeom& g) {
            g.forEachVector(account);       // stage keys, local and screen output
            capacity += sizeof(UnitGeom);   // the entry itself (pool storage)
        });
        const auto& pool = geomEntries_.stats();
        capacity += pool.indexBytes + pool.metaBytes;
        renderStats_.set(RS::GeomPoolEntries, geomEntries_.live());
        renderStats_.set(RS::GeomPoolUsedBytes, used);
        renderStats_.set(RS::GeomPoolCapacityBytes, capacity);
        renderStats_.add(RS::GeomEntryAcquired, pool.acquired);
        renderStats_.add(RS::GeomEntryReplaced, pool.replaced);
        renderStats_.add(RS::GeomEntryReleasedDestroyed, pool.releasedDestroyed);
        renderStats_.add(RS::GeomEntryReleasedExpired, pool.releasedExpired);
        renderStats_.add(RS::GeomEntryReleasedBudget, pool.releasedBudget);
        renderStats_.add(RS::GeomEntryTrimmed, pool.trimmed);
        renderStats_.add(RS::GeomEntryRefused, pool.duplicates + pool.refused);
        renderStats_.set(RS::GeomEntriesRetained, pool.retained);
        renderStats_.set(RS::GeomEntriesWarmFree, pool.warmFree);
        renderStats_.set(RS::GeomEntriesAllocated, pool.allocated);
        renderStats_.set(RS::GeomRetainedBytes, pool.retainedBytes);
        renderStats_.set(RS::GeomWarmFreeBytes, pool.warmFreeBytes);
        // Workstream J: entries of units in view (plus the index) cannot be evicted;
        // retained absent entries and warm free capacity are what C's budgets reclaim.
        renderStats_.set(RS::GeomPinnedBytes, capacity - std::min(capacity, pool.retainedBytes + pool.warmFreeBytes));
    }

    // Workstream J: per-participant scratch (see WorkerScratch in gameview.h).
    GameView::WorkerScratch& GameView::workerScratch() {
        const unsigned slot = ThreadPool::participant(pool_);
#ifndef NDEBUG
        // Slot 0 is the render thread's; any other thread here would share it.
        if (!slot && std::this_thread::get_id() != renderThread_)
            throw std::logic_error("render scratch used off the render thread and the pool");
#endif
        return *workerScratch_[slot];
    }

    size_t GameView::WorkerScratch::usedBytes() const {
        const auto bytes = [](const auto& v) { return v.size() * sizeof(v[0]); };
        return bytes(tris) + bytes(order) + bytes(key) + bytes(previousRuns) + bytes(shadowVerts) +
               bytes(collectVerts);
    }

    size_t GameView::WorkerScratch::capacityBytes() const {
        const auto bytes = [](const auto& v) { return v.capacity() * sizeof(v[0]); };
        const auto piece = [&](const auto& self, const PreparedPiece& p) -> size_t {
            size_t n = bytes(p.rotated) + bytes(p.children);
            for (const auto& c : p.children) n += self(self, c);
            return n;
        };
        return bytes(tris) + bytes(order) + bytes(key) + bytes(previousRuns) + bytes(shadowVerts) +
               bytes(collectVerts) + piece(piece, prepared);
    }

    void GameView::WorkerScratch::release() {
        std::vector<Tri>().swap(tris);
        std::vector<size_t>().swap(order);
        prepared = PreparedPiece{};
        std::vector<uint64_t>().swap(key);
        std::vector<std::pair<SDL_Texture*, int>>().swap(previousRuns);
        std::vector<ShadowCollectVertex>().swap(shadowVerts);
        std::vector<CollectVertex>().swap(collectVerts);
        peak = 0;
    }

    // Frame end, render thread, no pool pass running: every retained per-frame buffer
    // and every worker scratch slot is observed against its trim policy
    // (client/cpubudget.h). Buffers are shrunk only after a whole window of low
    // demand, never below their contents.
    void GameView::trimRetainedBuffers() {
        ++trimFrame_;
        const uint64_t now = ta::devFlag("TA_RENDER_STUDY") ? uint64_t(animClock_ * 1000) : SDL_GetTicks64();
#ifndef NDEBUG
        const bool verify = ta::devFlag("TA_CPU_TRIM_VERIFY");
        if (verify) ++cpuTrimVerify_.frames;
#else
        constexpr bool verify = false;
#endif
        const auto observe = [&](Retained id, auto& v) {
            auto& slot = retained_[size_t(id)];
            using T = std::decay_t<decltype(v[0])>;
            constexpr size_t elem = sizeof(T);
            const size_t need = std::max(slot.peak, v.size() * elem);
            slot.peak = 0;
            if (verify && slot.trimmedTo && v.capacity() * elem > slot.trimmedTo) {
                ++cpuTrimVerify_.regrowths;
                slot.trimmedTo = 0;
            }
            if (const size_t target = slot.tracker.observe(need, v.capacity() * elem, trimFrame_, now, trimPolicy_)) {
                std::vector<uint8_t> before;
                if constexpr (std::is_trivially_copyable_v<T>)
                    if (verify) before.assign(reinterpret_cast<const uint8_t*>(v.data()),
                                              reinterpret_cast<const uint8_t*>(v.data() + v.size()));
                const size_t size = v.size();
                if (const size_t released = ta::render::shrinkVector(v, target / elem)) {
                    ++trimsThisFrame_;
                    trimBytesThisFrame_ += released;
                    if (verify) {
                        if (v.size() != size) throw std::runtime_error("cpu trim verify: a trim changed a buffer's size");
                        if constexpr (std::is_trivially_copyable_v<T>) {
                            if (size && std::memcmp(before.data(), v.data(), before.size()))
                                throw std::runtime_error("cpu trim verify: a trim changed a buffer's contents");
                            ++cpuTrimVerify_.contentChecks;
                        }
                        if (slot.lastTrimFrame && trimFrame_ - slot.lastTrimFrame < trimPolicy_.windowFrames)
                            throw std::runtime_error("cpu trim verify: a buffer was trimmed twice within one window");
                        slot.lastTrimFrame = trimFrame_;
                        slot.trimmedTo = v.capacity() * elem;
                        ++cpuTrimVerify_.trims;
                        cpuTrimVerify_.released += released;
                    }
                }
            }
        };
        observe(Retained::BodyVerts, bodyVerts_);
        observe(Retained::PaintItems, paintItems_);
        observe(Retained::DrawOps, drawOps_);
        observe(Retained::CopyTasks, copyTasks_);
        observe(Retained::DistantItems, distantModelItems_);
        observe(Retained::BodySources, bodySources_);
        observe(Retained::BodyRanges, bodyRanges_);
        observe(Retained::ArenaFirsts, bodyArenaFirsts_);
        observe(Retained::ArenaCounts, bodyArenaCounts_);
        observe(Retained::ArenaRanges, bodyArenaRanges_);
        observe(Retained::VisUnits, visUnits_);
        observe(Retained::VisGeom, visGeom_);
        observe(Retained::VisCost, visCost_);
        observe(Retained::GeomIndex, geomIndex_);
        observe(Retained::AirShadows, airShadows_);
        observe(Retained::ShadowPrepItems, shadowPrepItems_);
        observe(Retained::ShadowOpaque, shadowAtlasOpaqueScratch_);
        observe(Retained::ShadowComposite, shadowCompositeBatch_);
        observe(Retained::ShadowTileStamps, shadowTileStamps_);
        observe(Retained::ShadowTileBefore, shadowTileBefore_);
        observe(Retained::ShadowTileHasBefore, shadowTileHasBefore_);
        observe(Retained::ShadowTileChecks, shadowTileChecks_);
        observe(Retained::ShadowTileNeeds, shadowTileNeeds_);
        // F's per-cell stamps are sized by the cell index bound (its handle trimming
        // shrinks the size, never the capacity); their contents are kept.
        observe(Retained::ShadowCellStamps, shadowCellStamps_);
        observe(Retained::ShadowCellStampValid, shadowCellStampValid_);
        observe(Retained::ModelTris, tris_);
        observe(Retained::ModelTriBatch, triBatch_);
        {   // The per-coverage-page mask batches: one map, trimmed as a whole (scratch
            // refilled per page, so releasing every vector loses nothing).
            auto& slot = retained_[size_t(Retained::ShadowMask)];
            size_t used = 0, capacity = 0;
            for (const auto& [texture, vertices] : shadowAtlasMaskScratch_) {
                used += vertices.size() * sizeof(SDL_Vertex);
                capacity += vertices.capacity() * sizeof(SDL_Vertex);
            }
            const size_t need = std::max(slot.peak, used);
            slot.peak = 0;
            if (slot.tracker.observe(need, capacity, trimFrame_, now, trimPolicy_) && capacity) {
                std::unordered_map<SDL_Texture*, std::vector<SDL_Vertex>>().swap(shadowAtlasMaskScratch_);
                ++trimsThisFrame_;
                trimBytesThisFrame_ += capacity;
            }
        }
        // Worker scratch: between passes the render thread owns every slot
        // (ThreadPool::participant). A slot whose capacity outlived its use is
        // released whole; its worker regrows it to what it needs next.
        for (size_t i = 0; i < workerScratch_.size(); ++i) {
            WorkerScratch& ws = *workerScratch_[i];
            ws.note();
            const size_t capacity = ws.capacityBytes();
            const size_t need = ws.peak;
            ws.peak = 0;
            if (workerTrim_[i].observe(need, capacity, trimFrame_, now, trimPolicy_) && capacity) {
                ws.release();
                ++trimsThisFrame_;
                trimBytesThisFrame_ += capacity;
                if (verify) { ++cpuTrimVerify_.workerTrims; cpuTrimVerify_.released += capacity; }
            }
        }
    }

    // The per-frame buffers whose capacity is retained across frames. One list,
    // so the reallocation count and the used/capacity accounting agree.
    void GameView::noteFrameCapacities() {
        size_t scratch = 0;
        for (const auto& [texture, vertices] : shadowAtlasMaskScratch_) scratch += vertices.capacity();
        telemetryCursor_.capacity = {paintItems_.capacity(), drawOps_.capacity(), copyTasks_.capacity(),
            distantModelItems_.capacity(), visUnits_.capacity(), bodyVerts_.capacity(),
            shadowAtlasOpaqueScratch_.capacity() + shadowCompositeBatch_.capacity(), scratch,
            bodySources_.capacity(), bodyRanges_.capacity(), bodyArenaFirsts_.capacity()};
    }

    // Render thread, once per frame when draw() returns: everything that is not
    // tallied in place -- worker pool, submission counters, terrain workers and
    // uploads (as deltas of their cumulative counters), GPU creations and the
    // CPU-side used/retained bytes.
    void GameView::foldRenderStats() {
        using RS = ta::render::Counter;
        using RP = ta::render::Phase;
        using RW = ta::render::Work;
        auto& stats = renderStats_;
        stats.add(RS::PoolDispatches, poolStats_.dispatches);
        stats.add(RS::PoolSerialRuns, poolStats_.serial);
        stats.add(RS::PoolItems, poolStats_.items);
        stats.add(RS::PoolPosted, poolStats_.posted);
        stats.add(RS::PoolIdleWakes, poolStats_.idleWakes);
        stats.add(RS::PoolChunks, poolStats_.chunks);
        stats.add(RS::SchedGeometryThreads, schedThreadsGeom_);
        stats.add(RS::SchedShadowThreads, schedThreadsShadow_);
        stats.add(RS::SchedCopyThreads, schedThreadsCopy_);
        stats.add(RS::SchedGeometryEstimateUs, uint64_t(schedEstimateGeomNs_ * 1e-3 + 0.5));
        schedThreadsGeom_ = schedThreadsShadow_ = schedThreadsCopy_ = 0;
        schedEstimateGeomNs_ = 0;
        stats.addMs(RP::PoolDispatch, double(poolStats_.dispatchNs) * 1e-6);
        stats.addMs(RP::PoolWait, double(poolStats_.waitNs) * 1e-6);
        stats.addWorkNs(RW::PoolBusy, poolStats_.busyNs);
        stats.addWorkNs(RW::PoolWake, poolStats_.wakeNs);

        const auto& submit = ta::render::gSubmit;
        stats.add(RS::LogicalDraws, submit.logical);
        stats.add(RS::NativeDraws, submit.native);
        stats.add(RS::SdlGeometryDraws, submit.sdl);
        stats.add(RS::RetainedDraws, submit.retained);
        stats.add(RS::Flushes, submit.flushes);
        stats.add(RS::NativePrimes, submit.primes);
        stats.add(RS::TargetSwitches, submit.targetSwitches);
        stats.add(RS::VertsSubmitted, submit.verts);
        stats.add(RS::BytesUploaded, submit.bytes);
        stats.add(RS::TextureUploadBytes, submit.textureBytes);

        auto& cursor = telemetryCursor_;
        const auto chunks = mapView_.chunkStats();
        const auto sections = mapView_.sectionStats();
        const auto delta = [](uint64_t now, uint64_t before) { return now >= before ? now - before : now; };
        stats.set(RS::TerrainChunkBytes, chunks.bytes);
        stats.set(RS::TerrainChunkImages, chunks.images);
        stats.set(RS::TerrainChunkUploads, chunks.uploads);
        stats.set(RS::TerrainSlotCreates, chunks.slotCreates);
        stats.set(RS::TerrainSlotReuses, chunks.slotReuses);
        stats.set(RS::TerrainSlotPooledBytes, chunks.pooledBytes);
        stats.set(RS::TerrainSectionReuses, sections.reuses);
        stats.set(RS::TerrainSectionPooledBytes, sections.pooledBytes);
        stats.set(RS::ShadowPageReuses, shadowPageReuses_);
        stats.set(RS::ShadowSparePageBytes, shadowSpareBytes());
        stats.set(RS::TerrainChunkPending, chunks.pending);
        stats.set(RS::TerrainChunkVisible, chunks.visible);
        stats.set(RS::TerrainChunkReady, chunks.ready);
        stats.set(RS::TerrainChunkStandin, chunks.standin);
        stats.set(RS::TerrainChunkHoles, chunks.holes);
        stats.set(RS::TerrainUnderlayTiles, mapView_.underlayTiles());
        stats.set(RS::TerrainSectionBytes, mapView_.sectionBytes());
        stats.add(RS::TerrainJobsQueued, delta(chunks.jobsQueued, cursor.chunks.jobsQueued));
        stats.add(RS::TerrainJobsCompleted, delta(chunks.jobsCompleted, cursor.chunks.jobsCompleted));
        stats.add(RS::TerrainJobsCancelled, delta(chunks.jobsCancelled, cursor.chunks.jobsCancelled));
        stats.add(RS::TerrainJobsDiscarded, delta(chunks.jobsDiscarded, cursor.chunks.jobsDiscarded));
        stats.add(RS::TerrainJobsUsed, delta(chunks.jobsUsed, cursor.chunks.jobsUsed));
        stats.add(RS::TerrainJobsObsolete, delta(chunks.jobsObsolete, cursor.chunks.jobsObsolete));
        stats.add(RS::TerrainJobsAborted, delta(chunks.jobsAborted, cursor.chunks.jobsAborted));
        stats.add(RS::TerrainJobsNearby, delta(chunks.jobsNearby, cursor.chunks.jobsNearby));
        stats.add(RS::TerrainJobsOffview, delta(chunks.jobsOffview, cursor.chunks.jobsOffview));
        stats.add(RS::TerrainUploadDeferred, delta(chunks.uploadsDeferred, cursor.chunks.uploadsDeferred));
        stats.add(RS::TerrainGatedFrames, delta(chunks.framesGated, cursor.chunks.framesGated));
        stats.add(RS::TerrainSectionUploads, delta(sections.uploads, cursor.sections.uploads));
        // Decodes by any thread (section worker, chunk worker, finish frames), as the
        // compositor counts them (before J: the section worker's attempts, hits included).
        stats.add(RS::TerrainSectionDecodes, delta(sections.cpu.decodes, cursor.sections.cpu.decodes));
        stats.set(RS::TerrainMipCpuBytes, chunks.mipBytes);
        stats.set(RS::TerrainMipPinnedBytes, chunks.mipPinnedBytes);
        stats.set(RS::TerrainMipPeakBytes, chunks.mipPeakBytes);
        stats.add(RS::TerrainMipEvictions, delta(chunks.mipEvictions, cursor.chunks.mipEvictions));
        stats.add(RS::TerrainMipRegens, delta(chunks.mipRegens, cursor.chunks.mipRegens));
        stats.add(RS::TerrainMipTrims, delta(chunks.mipTrims, cursor.chunks.mipTrims));
        stats.set(RS::TerrainSectionCpuBytes, sections.cpu.usedBytes);
        stats.set(RS::TerrainSectionPinnedBytes, sections.cpu.pinnedBytes);
        stats.set(RS::TerrainSectionYoungBytes, sections.cpu.youngBytes);
        stats.set(RS::TerrainSectionPeakBytes, sections.cpu.peakBytes);
        stats.add(RS::TerrainSectionEvictions, delta(sections.cpu.evictions, cursor.sections.cpu.evictions));
        stats.add(RS::TerrainSectionRedecodes, delta(sections.cpu.redecodes, cursor.sections.cpu.redecodes));
        stats.add(RS::TerrainSectionPrefetchSkipped, delta(sections.prefetchSkipped, cursor.sections.prefetchSkipped));
        const uint64_t uploadNs = delta(chunks.uploadNs, cursor.chunks.uploadNs) +
            delta(sections.uploadNs, cursor.sections.uploadNs);
        stats.addMs(RP::TerrainUpload, double(uploadNs) * 1e-6);
        stats.addWorkNs(RW::TerrainUpload, uploadNs);
        stats.addWorkNs(RW::TerrainCompose, delta(chunks.composeNs, cursor.chunks.composeNs));
        stats.addWorkNs(RW::TerrainWasted, delta(chunks.wastedNs, cursor.chunks.wastedNs));
        stats.addWorkNs(RW::SectionDecode, delta(sections.decodeNs, cursor.sections.decodeNs));
        cursor.chunks = chunks;
        cursor.sections = sections;

        stats.set(RS::GpuTrackedBytes, gpuvram::bytes());
        stats.set(RS::PaletteMode, uint64_t(paletteMode_));
        stats.set(RS::PalettePages, palettePages_.size());
        stats.set(RS::PaletteGpuBytes, paletteGpuBytes());
        if (const size_t key = paletteTextureSources_.size() * 65537 + paletteTextureSheets_.size();
            key != paletteCpuKey_) { paletteCpuKey_ = key; paletteCpuCache_ = paletteCpuBytes(); }
        stats.set(RS::PaletteCpuBytes, paletteCpuCache_);
        stats.add(RS::PaletteUploadBytes, paletteUploadBytes_);
        stats.add(RS::PaletteIndexedDraws, paletteIndexedDraws_);
        stats.add(RS::PaletteFallbacks, paletteFallbacks_);
        stats.add(RS::PaletteSwitches, paletteSwitches_);
        stats.add(RS::PaletteSpans, paletteSpans_);
        stats.add(RS::PaletteSpanCommands, paletteSpanCommands_);
        stats.add(RS::NativePasses, nativePasses_);
        stats.add(RS::NativePassCommands, nativePassCommands_);
        stats.add(RS::NativePassRefused, nativePassRefused_);
        stats.add(RS::GpuTextureCreates, delta(gpuvram::g_creates, cursor.gpuCreates));
        stats.add(RS::GpuTextureDestroys, delta(gpuvram::g_destroys, cursor.gpuDestroys));
        cursor.gpuCreates = gpuvram::g_creates;
        cursor.gpuDestroys = gpuvram::g_destroys;

        // Reallocations: capacity growth of the retained per-frame buffers.
        const auto before = cursor.capacity;
        noteFrameCapacities();
        uint64_t grew = 0;
        for (size_t i = 0; i < before.size(); ++i) grew += cursor.capacity[i] > before[i];
        stats.add(RS::FrameReallocs, grew);

        size_t listUsed = 0, listCapacity = 0;
        const auto account = [&](const auto& v) {
            listUsed += v.size() * sizeof(v[0]);
            listCapacity += v.capacity() * sizeof(v[0]);
        };
        account(paintItems_); account(drawOps_); account(copyTasks_); account(distantModelItems_);
        account(bodySources_); account(bodyRanges_); account(bodyArenaFirsts_); account(bodyArenaCounts_);
        account(visUnits_); account(geomIndex_); account(airShadows_);
        // Workstream I: the ordered entry references, their cost copies, the
        // shadow-prep item list and the three plans' chunk bounds.
        account(visGeom_); account(visCost_); account(shadowPrepItems_);
        for (const auto* plan : {&geomPlan_, &shadowPlan_, &copyPlan_}) {
            listUsed += plan->usedBytes();
            listCapacity += plan->capacityBytes();
        }
        stats.set(RS::FrameListsUsedBytes, listUsed);
        stats.set(RS::FrameListsCapacityBytes, listCapacity);
        size_t scratchUsed = 0, scratchCapacity = 0;
        const auto scratch = [&](const auto& v) {
            scratchUsed += v.size() * sizeof(v[0]);
            scratchCapacity += v.capacity() * sizeof(v[0]);
        };
        scratch(shadowAtlasOpaqueScratch_); scratch(shadowCompositeBatch_);
        scratch(shadowTileStamps_); scratch(shadowTileBefore_); scratch(shadowTileHasBefore_);
        scratch(shadowTileChecks_); scratch(shadowTileNeeds_);
        scratch(shadowCellStamps_); scratch(shadowCellStampValid_);
        scratchCapacity += shadowAtlas_.retainedBytes();   // cell records, shelves, free lists
        for (const auto& [texture, vertices] : shadowAtlasMaskScratch_) scratch(vertices);
        stats.set(RS::ShadowScratchUsedBytes, scratchUsed);
        stats.set(RS::ShadowScratchCapacityBytes, scratchCapacity);

        // Workstream J: retained-buffer trims (this frame, already done), worker
        // scratch, and the CPU totals (see the accounting table in the progress record).
        stats.add(RS::CpuTrims, trimsThisFrame_);
        stats.add(RS::CpuTrimBytes, trimBytesThisFrame_);
        trimsThisFrame_ = trimBytesThisFrame_ = 0;
        size_t workerUsed = 0, workerCapacity = 0;
        for (const auto& ws : workerScratch_) { workerUsed += ws->usedBytes(); workerCapacity += ws->capacityBytes(); }
        stats.set(RS::WorkerScratchUsedBytes, workerUsed);
        stats.set(RS::WorkerScratchCapacityBytes, workerCapacity);
        size_t modelUsed = 0, modelCapacity = 0;   // render-thread model scratch (drawUnit, debris, shots)
        for (const auto* v : {&tris_}) { modelUsed += v->size() * sizeof(Tri); modelCapacity += v->capacity() * sizeof(Tri); }
        modelUsed += triBatch_.size() * sizeof(SDL_Vertex);
        modelCapacity += triBatch_.capacity() * sizeof(SDL_Vertex);
        const size_t bodyUsed = bodyVerts_.size() * sizeof(SDL_Vertex), bodyCapacity = bodyVerts_.capacity() * sizeof(SDL_Vertex);
        stats.set(RS::BodyBufferCapacityBytes, bodyCapacity);   // after this frame's trim
        const uint64_t geomUsed = stats.get(RS::GeomPoolUsedBytes), geomCapacity = stats.get(RS::GeomPoolCapacityBytes);
        const uint64_t geomPinned = stats.get(RS::GeomPinnedBytes);
        const uint64_t arenaCpu = stats.get(RS::ArenaCpuBytes), distantMeta = stats.get(RS::DistantMetadataBytes);
        // Sections a consumer holds or the camera used within the working-set window
        // are not evictable now.
        const uint64_t sectionUsed = sections.cpu.usedBytes, sectionPinned = sections.cpu.pinnedBytes + sections.cpu.youngBytes;
        const uint64_t mipUsed = chunks.mipBytes, mipPinned = chunks.mipPinnedBytes;
        // used: bytes holding this frame's data or cached content; retained: allocated.
        const uint64_t frameUsed = bodyUsed + listUsed + scratchUsed + workerUsed + modelUsed;
        const uint64_t frameCapacity = bodyCapacity + listCapacity + scratchCapacity + workerCapacity + modelCapacity;
        const uint64_t used = geomUsed + frameUsed + sectionUsed + mipUsed + arenaCpu + distantMeta;
        const uint64_t retained = geomCapacity + frameCapacity + sectionUsed + mipUsed + arenaCpu + distantMeta;
        // pinned: needed now (entries in view, images a job/upload holds, this frame's
        // buffer contents); evictable: what the budgets/trims may release.
        const uint64_t pinned = geomPinned + sectionPinned + mipPinned + frameUsed;
        const uint64_t evictable = (geomCapacity - std::min(geomCapacity, geomPinned)) +
            (sectionUsed - std::min(sectionUsed, sectionPinned)) + (mipUsed - std::min(mipUsed, mipPinned)) +
            (frameCapacity - std::min(frameCapacity, frameUsed));
        cpuPeakBytes_ = std::max(cpuPeakBytes_, size_t(retained));
#ifndef NDEBUG
        if (ta::devFlag("TA_CPU_TRIM_VERIFY")) {
            cpuTrimVerify_.peakRetained = std::max<uint64_t>(cpuTrimVerify_.peakRetained, retained);
            cpuTrimVerify_.finalRetained = retained;
        }
#endif
        stats.set(RS::CpuCacheUsedBytes, used);
        stats.set(RS::CpuCacheRetainedBytes, retained);
        stats.set(RS::CpuCachePinnedBytes, pinned);
        stats.set(RS::CpuCacheEvictableBytes, evictable);
        stats.set(RS::CpuCachePeakBytes, cpuPeakBytes_);
    }

    const ta::tdo::Model* GameView::ghostModel(const std::string& typeId) {
        // Through loadVisual, which builds the PieceMeta tree. This used to insert the
        // entry with meta{} and a comment saying collect() would compute the answers
        // live -- true of the ghost's OWN draw, which passes no meta, but the entry it
        // left behind is the one every later unit draw uses, and that one does pass it.
        // See loadVisual: the square shadow on the Beast Handler was this.
        try {
            return loadVisual(typeId);
        } catch (const std::exception&) {}
        return nullptr;
    }

    void GameView::preparePaletteTextures(const PieceMeta& meta) {
        if(!palettePreparedModels_.insert(&meta).second)return;
        std::set<std::string> names;
        const auto gather=[&](const auto& self,const PieceMeta& piece)->void {
            if(!piece.skip)names.insert(piece.primTex.begin(),piece.primTex.end());
            for(const auto& child:piece.children)self(self,child);
        };
        gather(gather,meta);
        std::vector<std::pair<size_t,const std::string*>> ordered;
        for(const auto& name:names) {
            const auto source=paletteTextureSources_.find(name);
            if(source==paletteTextureSources_.end())continue;
            size_t area=0;
            for(const auto& frame:source->second.frames)
                area=std::max(area,size_t(frame.width)*frame.height);
            ordered.push_back({area,&name});
        }
        std::stable_sort(ordered.begin(),ordered.end(),
            [](const auto& a,const auto& b){return a.first>b.first;});
        for(const auto& [area,name]:ordered)preparePaletteTexture(*name);
    }

    void GameView::preparePaletteTexture(const std::string& name) {
        namespace pal = ta::render::palette;
        if(paletteTextureSheets_.count(name))return;
        const auto source=paletteTextureSources_.find(name);
        if(source==paletteTextureSources_.end())return;
        if(paletteMode_==PaletteMode::Unset)updatePaletteMode();
        const bool indexed=paletteMode_==PaletteMode::Indexed;
        const pal::Bank& bank=paletteBanks_[source->second.bank];
        auto& sheets=paletteTextureSheets_[name];
        sheets.resize(source->second.frames.size());
        constexpr int side=pal::kPageSide;
        thread_local std::vector<uint8_t> pixels;
        for(size_t frame=0;frame<sheets.size();++frame) {
            const auto& input=source->second.frames[frame];
            const int w=input.width,h=input.height;
            if(!pal::PageLayout::fitsPage(w,h))continue;
            // The pre-M packer (palettesheets.h): best free rectangle, else a new page
            // while the form's budget allows one.
            auto slot=paletteLayout_.find(w,h);
            if(!slot) {
                if(palettePages_.size()>=pal::kMaxPages || gpuAllocBlocked())continue;
                SDL_Texture* texture=nullptr;
                if(indexed) {
                    // Key texture (the runs' texture: priming, batching) + index page.
                    if(!gpuvram::wouldFit(pal::kIndexPageBytes+4))continue;
                    texture=gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,1,1);
                    if(!texture) {noteGpuAllocFail();continue;}
                    const uint8_t white[4]={255,255,255,255};
                    SDL_UpdateTexture(texture,nullptr,white,4);
                    if(!paletteIndexed_.createPage()) {gpuvram::destroy(texture);noteGpuAllocFail();continue;}
                } else {
                    if(!gpuvram::wouldFit(pal::kBakedPageBytes))continue;
                    texture=gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,side,side);
                    if(!texture) {noteGpuAllocFail();continue;}
                    SDL_SetTextureScaleMode(texture,unitScaleMode());
                }
                SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
                palettePages_.push_back({texture,0});
                slot=paletteLayout_.addPage();
            }
            PalettePage& page=palettePages_[slot->page];
            const pal::Rect rect=paletteLayout_.sheetRect(*slot,w,h);
            uint32_t sheetId=pal::kNoSheet;
            if(indexed) {
                if(page.sheets>=pal::kMaxSheetsPerPage)continue;
                const uint8_t bankId=source->second.bank;
                if(paletteBanksUploaded_.size()<=bankId)paletteBanksUploaded_.resize(size_t(bankId)+1,false);
                if(!paletteBanksUploaded_[bankId]) {
                    if(!paletteIndexed_.setBank(bankId,bank))continue;
                    paletteBanksUploaded_[bankId]=true;
                    paletteUploadBytes_+=size_t(pal::kShadeLevels)*256*4;
                }
                sheetId=page.sheets;
                if(!paletteIndexed_.uploadEntry(slot->page,sheetId,pal::entryFor(rect,w,h,bankId)) ||
                   !paletteIndexed_.upload(slot->page,rect.x/pal::kBandColumns,rect.y/pal::kBandRows,input,sheetId))continue;
                ++page.sheets;
                paletteUploadBytes_+=size_t(w)*size_t(h)*pal::kIndexTexelBytes+pal::kTableTexelsPerSheet*pal::kIndexTexelBytes;
            } else {
                pal::expandSheet(input,bank,pixels);
                SDL_Rect area{rect.x,rect.y,rect.w,rect.h};
                if(SDL_UpdateTexture(page.texture,&area,pixels.data(),w*16)!=0)continue;
                paletteUploadBytes_+=pixels.size();
                if(ta::devFlag("TA_PALETTE_VERIFY")) {
                    // Exercise the actual uploaded subrect, including the four-column
                    // band layout, on software and accelerated SDL backends alike.
                    auto* check=gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_TARGET,area.w,area.h);
                    if(!check)throw std::runtime_error("palette verification target allocation failed");
                    auto* previous=SDL_GetRenderTarget(ren_);
                    SDL_BlendMode blend;SDL_GetTextureBlendMode(page.texture,&blend);
                    SDL_SetTextureBlendMode(page.texture,SDL_BLENDMODE_NONE);
                    SDL_SetRenderTarget(ren_,check);
                    const int copied=SDL_RenderCopy(ren_,page.texture,&area,nullptr);
                    std::vector<uint8_t> readback(pixels.size());
                    const int read=SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_RGBA32,
                        readback.data(),w*16);
                    SDL_SetRenderTarget(ren_,previous);
                    SDL_SetTextureBlendMode(page.texture,blend);
                    gpuvram::destroy(check);
                    if(copied || read || readback!=pixels)
                        throw std::runtime_error("palette texture upload mismatch: "+name);
                    std::fprintf(stderr,"PALETTE_VERIFY %s frame=%zu levels=20 pixels=%zu pages=%zu\n",
                        name.c_str(),frame,size_t(w)*h,palettePages_.size());
                }
            }
            sheets[frame]={page.texture,{float(rect.x)/side,float(rect.y)/side,
                                        float(w)/side,float(h)/side},sheetId};
            paletteLayout_.commit(*slot,w,h);
        }
    }

    // Which palette form to hold: indexed on the OpenGL backend when the program
    // builds, native draws are possible (opengl, integer output scale), units are
    // point-sampled (bilinear is not bit-identical; TA_PALETTE_INDEXED_LINEAR
    // tries it) and no indexed draw has failed; baked otherwise (debug
    // TA_PALETTE_BAKED forces it: the same-binary reference). A change releases
    // the held form and rebuilds everything that captured its textures.
    void GameView::updatePaletteMode() {
        // Bakes draw their runs through drawModelRun in EVERY build: SDL cannot draw
        // an indexed run (it would sample the 1x1 key texture), and a baked-form run
        // goes to SDL_RenderGeometry there exactly as without the drawer. Installed
        // here, next to the decision, so the indexed form is never chosen without it.
        distantModelCache_.setRunDrawer(&GameView::drawModelRunThunk,this);
        const bool linear=ta::texturefilter::unitsLinear(textureFilter_);
        paletteIndexed_.setMarkOutside(ta::devFlag("TA_PALETTE_MARK_EDGES"));
        // Debug TA_BODY_SDL_SUBMIT (the pure-SDL submission reference) takes the
        // baked form too: indexed runs, and the spans they open, are native draws.
        bool indexed=!paletteIndexedFailed_ && !ta::devFlag("TA_PALETTE_BAKED") &&
                     !ta::devFlag("TA_BODY_SDL_SUBMIT") &&
                     (!linear || ta::devFlag("TA_PALETTE_INDEXED_LINEAR")) &&
                     paletteIndexed_.ready(ren_) && bodySubmit_.nativeCapable(ren_);
        const PaletteMode wanted=indexed ? PaletteMode::Indexed : PaletteMode::Baked;
        paletteLinear_=indexed && linear;
        // A new GL context forgets the index pages (paletteindexed.h): the keys that
        // still name them are rebuilt like a switch, never drawn without their page.
        const bool lost=paletteMode_==PaletteMode::Indexed && paletteIndexed_.pages()!=palettePages_.size();
        if(wanted==paletteMode_ && !lost)return;
        if(paletteMode_!=PaletteMode::Unset) {
            ++paletteSwitches_;
            releasePaletteSheets();
            geomEntries_.makeAllCold();   // geometry captured the old form's textures and coordinates
            distantModelCache_.clear();   // ... and so did the images baked from it
            releaseBodyArena();
        }
        paletteMode_=wanted;
    }

    // Release both forms' pages and the layout; sources and banks stay (CPU).
    void GameView::releasePaletteSheets() {
        for(auto& page:palettePages_)gpuvram::destroy(page.texture);
        palettePages_.clear();
        paletteTextureSheets_.clear();
        palettePreparedModels_.clear();
        paletteLayout_.clear();
        paletteIndexed_.releasePages();
        paletteBanksUploaded_.clear();
    }

    size_t GameView::paletteGpuBytes() const {
        return paletteIndexed_.gpuBytes()+palettePages_.size()*
            (paletteMode_==PaletteMode::Indexed ? size_t(4) : ta::render::palette::kBakedPageBytes);
    }

    // Sources (index + alpha per texel), banks, sheet records (capacities).
    size_t GameView::paletteCpuBytes() const {
        size_t bytes=paletteBanks_.capacity()*sizeof(ta::render::palette::Bank);
        for(const auto& bank:paletteBanks_)bytes+=bank.shades.capacity();
        for(const auto& [name,source]:paletteTextureSources_) {
            bytes+=sizeof(source)+name.capacity()+source.frames.capacity()*sizeof(ta::render::palette::Frame);
            for(const auto& frame:source.frames)bytes+=frame.la.capacity();
        }
        for(const auto& [name,sheets]:paletteTextureSheets_)
            bytes+=name.capacity()+sheets.capacity()*sizeof(PaletteTextureSheet);
        return bytes;
    }

    int GameView::palettePage(SDL_Texture* texture) const {
        if(paletteMode_!=PaletteMode::Indexed || !texture)return -1;
        for(size_t page=0;page<palettePages_.size();++page)
            if(palettePages_[page].texture==texture)return int(page);
        return -1;
    }
    ta::NativeProgram* GameView::paletteProgram(SDL_Texture* texture) {
        const int page=palettePage(texture);
        return page<0 ? nullptr : paletteIndexed_.program(size_t(page),paletteLinear_);
    }

    // Can this run's texture join a native pass (NativePassGL::joinable: SDL's RGBA
    // program, BLEND or the distant-image pages' premultiplied mode, no colour/alpha
    // mod, normalized coordinates)? Cached for the frame's replay.
    const GameView::SpanTexture& GameView::spanTexture(SDL_Texture* texture) {
        for(const auto& entry:spanTextures_)if(entry.texture==texture)return entry;
        const auto joinable=nativePass_.joinable(bodySubmit_,ren_,texture,ta::DistantModels::pageBlendMode());
        spanTextures_.push_back({texture,joinable.name,joinable.premultiplied});
        return spanTextures_.back();
    }

    void GameView::useSpanTexture(SDL_Texture* texture) {
        const int page=palettePage(texture);
        const SpanTexture& entry=page>=0 ? SpanTexture{} : spanTexture(texture);
        useSpanTexture(page,entry.name,entry.premultiplied);
    }
    void GameView::useSpanTexture(int page, GLuint name, bool premultiplied) {
        if(page>=0) {
            GLuint program=0,textures[3]={0,0,0};
            if(paletteIndexed_.passBinding(size_t(page),paletteLinear_,program,textures)) {
                nativePass_.useProgram(program,textures);
                ++paletteIndexedDraws_;
            }
        } else nativePass_.useTexture(name,premultiplied);
    }

    bool GameView::drawRunsNative(const std::vector<std::pair<SDL_Texture*, int>>& runs, const SDL_Vertex* vertices) {
        if(!nativePassesOn_ || paletteMode_!=PaletteMode::Indexed)return false;
        SDL_Texture* prime=nullptr;
        for(const auto& [texture,count]:runs) {
            if(!texture || count<=0)continue;
            if(palettePage(texture)>=0) { if(!prime)prime=texture; }
            else if(!spanTexture(texture).name)return false;
        }
        if(!prime)return false;
        if(!nativePass_.begin(bodySubmit_,ren_,prime,false)) {++nativePassRefused_;return false;}
        int offset=0;
        uint64_t commands=0;
        for(const auto& [texture,count]:runs) {
            if(texture && count>0) {
                useSpanTexture(texture);
                bodySubmit_.passDraw(std::span<const SDL_Vertex>(vertices+offset,size_t(count)));
                ++commands;
            }
            offset+=count;
        }
        nativePass_.end(bodySubmit_);
        ++paletteSpans_;
        ++nativePasses_;
        nativePassCommands_+=commands;
        return true;
    }

    GameView::SpanScan GameView::scanNativeSpan(size_t first) {
        // Every command up to the next boundary or a texture that cannot join. The
        // prime is the first BLEND texture (palette keys are BLEND), else the first
        // premultiplied one (then no BLEND texture is in the run).
        SpanScan scan;
        SDL_Texture* premultipliedPrime=nullptr;
        size_t end=first;
        for(;end<drawOps_.size();++end) {
            if(end>first && (end==airShadowOp_ || end==airOp_))break;
            DrawOp& op=drawOps_[end];
            if(op.boundary())break;
            if(op.count<=0 || !op.tex)continue;   // skipped by the replay anyway (null atlas page)
            op.page=int16_t(palettePage(op.tex));
            if(op.page>=0) {
                scan.nativeAnyway=scan.palette=true;
                if(!scan.prime)scan.prime=op.tex;
                continue;
            }
            const SpanTexture& texture=spanTexture(op.tex);
            op.spanName=texture.name;
            op.premultiplied=texture.premultiplied;
            if(!texture.name)break;
            if(size_t(op.count)>=nativeMin_)scan.nativeAnyway=true;
            if(!texture.premultiplied) { if(!scan.prime)scan.prime=op.tex; }
            else if(!premultipliedPrime)premultipliedPrime=op.tex;
        }
        scan.end=end;
        scan.primePremultiplied=!scan.prime;
        if(!scan.prime)scan.prime=premultipliedPrime;
        return scan;
    }

    void GameView::planNativePasses() {
        // Arena commands are native-size or (N2) planned here, so this segmentation is
        // the one the replay draws. Palette mode keeps the workstream-M rule: a pass
        // starts only at an indexed run.
        const bool arena=nativePassArena_ && nativePassMode_==NativePassMode::All;
        for(size_t i=0;i<drawOps_.size();) {
            DrawOp& head=drawOps_[i];
            if(head.boundary() || head.count<=0 || !head.tex ||
               (nativePassMode_==NativePassMode::Palette && palettePage(head.tex)<0)) {++i;continue;}
            const SpanScan scan=scanNativeSpan(i);
            if(scan.nativeAnyway && scan.end>i) {
                head.passEnd=int32_t(scan.end);
                head.passPrime=scan.prime;
                head.passPremultipliedPrime=scan.primePremultiplied;
                head.passPalette=scan.palette;
                // N2: every command of the pass may draw from the arena (a minimum size,
                // debug TA_NATIVE_PASS_ARENA_MIN, measured slower: Experiment 16).
                if(arena)
                    for(size_t k=i;k<scan.end;++k)
                        drawOps_[k].inPass=drawOps_[k].tex && size_t(std::max(0,drawOps_[k].count))>=nativePassArenaMin_;
            }
            i=std::max(scan.end,i+1);
        }
    }

    size_t GameView::drawNativeSpan(size_t first) {
        using RS = ta::render::Counter;
        const DrawOp& head=drawOps_[first];
        const size_t end=size_t(head.passEnd);
        if(!nativePass_.begin(bodySubmit_,ren_,head.passPrime,head.passPremultipliedPrime)) {
            ++nativePassRefused_;
            if(!nativePass_.usable())nativePassesOn_=false;   // refused by the state check: not this context
            return first;
        }
        uint64_t commands=0;
        for(size_t i=first;i<end;++i) {
            const DrawOp& op=drawOps_[i];
            if(op.count<=0 || !op.tex)continue;
            useSpanTexture(op.page,op.spanName,op.premultiplied);
            renderStats_.add(RS::BodySubmissions);
            ++commands;
            if(op.arenaFirst>=0) {
                const std::span<const GLint> firsts(bodyArenaFirsts_.data()+op.arenaFirst,op.arenaRanges);
                const std::span<const GLsizei> counts(bodyArenaCounts_.data()+op.arenaFirst,op.arenaRanges);
                bodySubmit_.passDrawBuffer(bodyArenaBuffers_[op.arenaSlot].id(),firsts,counts,size_t(op.count));
                renderStats_.add(RS::ArenaDraws);
                renderStats_.add(RS::ArenaRanges,firsts.size());
            } else {
                bodySubmit_.passDraw(std::span<const SDL_Vertex>(bodyVerts_.data()+op.start,size_t(op.count)));
            }
        }
        nativePass_.end(bodySubmit_);
        ++nativePasses_;
        nativePassCommands_+=commands;
        if(head.passPalette) {++paletteSpans_;paletteSpanCommands_+=commands;}
        return end;
    }

    int GameView::drawModelRun(SDL_Texture* texture, const SDL_Vertex* vertices, int count, bool countSdl) {
        if(ta::NativeProgram* program=paletteProgram(texture)) {
            const int drawn=bodySubmit_.draw(ren_,texture,std::span<const SDL_Vertex>(vertices,size_t(count)),
                                             true,0,{0,0},program);
            if(drawn==0) {++paletteIndexedDraws_;return 0;}
            ++paletteFallbacks_;
            if(!paletteIndexedFailed_)std::fprintf(stderr,"palette: indexed draw failed; using baked pages\n");
            paletteIndexedFailed_=true;
            return 1;
        }
        if(countSdl)ta::render::countSdlGeometry(size_t(count),size_t(count)*sizeof(SDL_Vertex));
        return SDL_RenderGeometry(ren_,texture,vertices,count,nullptr,0);
    }
    int GameView::drawModelRunThunk(void* self, SDL_Renderer*, SDL_Texture* texture, const SDL_Vertex* vertices, int count) {
        auto* view=static_cast<GameView*>(self);
        if(view->palettePage(texture)>=0)++view->paletteTotals_.bakeRuns;
        return view->drawModelRun(texture,vertices,count,true);
    }

    // The animated model textures that have an atlas rect, in registry order, with
    // the frame each selects now. The order is fixed for one asset generation, so
    // AnimatedAtlasTracker can index its per-atlas record by position, and the list
    // is only rebuilt when that generation changes (texture reload, layout); the
    // pointers it holds (texture vectors, animation clocks) are map nodes that live
    // as long as that generation. Sequences whose frames did not all load (fewer than
    // two textures) are not animated in the atlas: a new atlas paints their selected
    // frame once, as before.
    void GameView::collectAnimatedRegions() {
        if (animatedRegionsAsset_ == assetGeneration_.current()) {
            for (size_t i = 0; i < animatedRegions_.size(); ++i)
                animatedRegions_[i].frame = animatedRegionClocks_[i]->frame;
            return;
        }
        animatedRegionsAsset_ = assetGeneration_.current();
        animatedRegions_.clear();
        animatedRegionClocks_.clear();
        for (const auto& name : animatedTex_) {
            auto rit = atlasRect_.find(name);
            auto tit = textures_.find(name);
            auto ait = modelTextureAnimations_.find(name);
            if (rit == atlasRect_.end() || tit == textures_.end() || tit->second.size() < 2 ||
                ait == modelTextureAnimations_.end())
                continue;
            animatedRegions_.push_back({rit->second, &tit->second, ait->second.frame});
            animatedRegionClocks_.push_back(&ait->second);
        }
    }

    void GameView::animateGlowTextures() {
        if (animatedTex_.empty()) return;
        bool changed = false;
        for (auto& [name, animation] : modelTextureAnimations_) {
            const size_t frame = ta::retailEffectFrame(animation.durations,
                animation.loop, front().gameTick).value_or(animation.durations.size());
            changed |= frame != animation.frame;
            animation.frame = frame;
        }
        const auto renderer = rendererGeneration_.current(), asset = assetGeneration_.current();
#ifndef NDEBUG
        static const bool verify = ta::devFlag("TA_ATLAS_VERIFY");
#endif
        if (!changed) {
            // Every atlas recorded under the current generations was brought up to date
            // by the last update or painted since with these same frames: nothing to do.
            bool known = true;
            for (size_t slot = 0; slot < atlasTex_.size() && known; ++slot)
                known = !atlasTex_[slot] || atlasTracker_.known(slot, renderer, asset);
            if (known) {
#ifndef NDEBUG
                if (verify) { collectAnimatedRegions(); verifyAtlases(); }
#endif
                return;
            }
        }
        collectAnimatedRegions();
        // Only atlases holding a stale frame are visited, and a changed sequence
        // repaints its own region (and gutter) in each -- not every animated region of
        // every atlas.
        ta::render::AtlasPaintCounts counts;
        SDL_Texture* prev = nullptr;
        bool switched = false;
        {
            ta::render::AtlasPainter painter(ren_, &counts);
            for (size_t slot = 0; slot < atlasTex_.size(); ++slot) {
                SDL_Texture* atlas = atlasTex_[slot];
                if (!atlas || atlasTracker_.current(slot, renderer, asset, animatedRegions_))
                    continue;
                if (!switched) { prev = SDL_GetRenderTarget(ren_); switched = true; }
                SDL_SetRenderTarget(ren_, atlas);
                renderStats_.add(ta::render::Counter::TargetSwitches);
                atlasTracker_.update(slot, renderer, asset, animatedRegions_, painter);
            }
            painter.finish();   // while the atlas is still the target its copies belong to
        }
        if (switched) {
            SDL_SetRenderTarget(ren_, prev);
            renderStats_.add(ta::render::Counter::TargetSwitches);
            renderStats_.add(ta::render::Counter::AtlasRepaintPasses);
        }
        renderStats_.add(ta::render::Counter::AtlasRegionsPainted, counts.regions);
        renderStats_.add(ta::render::Counter::AtlasPixelsPainted, counts.pixels);
        renderStats_.add(ta::render::Counter::AtlasCopies, counts.copies);
        renderStats_.add(ta::render::Counter::AtlasFlushes, counts.flushes);
#ifndef NDEBUG
        if (verify) verifyAtlases();
#endif
    }

    // Every silhouette page and cell goes (renderer reset, page size change, last
    // page released). Cell handles keep counting generations, so an entry's stale
    // shadowTile never names a later cell; every stamp is dropped with its cell.
    void GameView::spareShadowPage(SDL_Texture* texture,int w,int h) {
        if (!texture) return;
        if (shadowSparePages_.size()>=ta::render::kShadowBasePages || w<=0 || h<=0) { gpuvram::destroy(texture); return; }
        shadowSparePages_.push_back({texture,w,h});
    }

    SDL_Texture* GameView::takeShadowSpare(int w,int h) {
        for (size_t i=0;i<shadowSparePages_.size();++i)
            if (shadowSparePages_[i].w==w && shadowSparePages_[i].h==h) {
                SDL_Texture* t=shadowSparePages_[i].texture;
                shadowSparePages_.erase(shadowSparePages_.begin()+std::ptrdiff_t(i));
                return t;
            }
        dropShadowSpares();   // another page size: these will not be wanted again soon
        return nullptr;
    }

    void GameView::dropShadowSpares() {
        for (auto& s:shadowSparePages_) gpuvram::destroy(s.texture);
        shadowSparePages_.clear();
    }

    void GameView::resetShadowAtlas(bool spare) {
        for (auto& page:shadowSilhouettePages_) if (page.texture) {
            if (spare) spareShadowPage(page.texture,shadowSilhouetteWidth_,shadowSilhouetteHeight_);
            else gpuvram::destroy(page.texture);
        }
        if (!spare) dropShadowSpares();
        shadowSilhouettePages_.clear();
        shadowSilhouetteWidth_=shadowSilhouetteHeight_=0;
        shadowAtlas_.reset(0,0);
        shadowAtlas_.trimHandles();
        shadowCellStamps_.clear();shadowCellStampValid_.clear();
        // The frame counter and the failure mark survive: while frames keep falling
        // back (their pages released, so the atlas resets), the next one still goes
        // straight to the former packer -- one attempt, as before F. So does the
        // extended-page cooldown after a pressure shed (perf follow-up K).
        const auto kept=shadowAtlasPolicy_;
        shadowAtlasPolicy_={};
        shadowAtlasPolicy_.frame=kept.frame;shadowAtlasPolicy_.failFrame=kept.failFrame;
        shadowAtlasPolicy_.extendedAfterFrame=kept.extendedAfterFrame;
        shadowAtlasPolicy_.extendedAfterMs=kept.extendedAfterMs;
    }

    // Workstream H: the retained body vertex arena (client/vertexarena.h).
    void GameView::releaseBodyArena() {
        if (bodyArena_.capacity() || bodyArenaBuffers_[0].capacityBytes() || bodyArenaBuffers_[1].capacityBytes())
            renderStats_.add(ta::render::Counter::ArenaReleases);
        bodyArena_.release();
        for (int k = 0; k < ta::render::kArenaSlots; ++k) {
            bodyArenaBuffers_[k].release();
            std::vector<SDL_Vertex>().swap(bodyArenaMirror_[k]);
        }
    }
    // Vertices the arena may (re)allocate now. It is an optimization, so it uses
    // the GPU budget only up to three quarters of the cap (caches that obey the
    // cap keep their room) and nothing while the cap is tightened after a
    // failure. Current and in-flight storage are already in gpuvram::bytes().
    size_t GameView::bodyArenaBudget() const {
        if (gpuvram::blocked() || gpuvram::tightened()) return 0;
        const size_t limit = gpuvram::cap() / 4 * 3, held = gpuvram::bytes();
        return held < limit ? (limit - held) / sizeof(SDL_Vertex) : 0;
    }
    // Yield the arena when the GPU budget comes under pressure.
    bool GameView::bodyArenaPressure() const {
        return gpuvram::blocked() || gpuvram::tightened() || gpuvram::bytes() > gpuvram::cap() / 8 * 7;
    }
    // Debug (TA_BODY_ARENA_VERIFY): every arena command gathered from the arena
    // contents (the simulated storage, or a CPU mirror of every GL upload)
    // through its merged ranges must equal the command assembled from its
    // sources -- exactly what the streaming path would draw.
    void GameView::verifyBodyArena(const ta::render::ArenaFrameStats& stats) {
#ifdef NDEBUG
        (void)stats;   // debug verifier only
#else
        std::vector<SDL_Vertex> drawn, expected;
        for (const DrawOp& op : drawOps_) {
            if (op.arenaFirst < 0) continue;
            const auto& buffer = bodyArenaBuffers_[op.arenaSlot];
            const SDL_Vertex* storage = buffer.simulated() ? buffer.simulatedData() : bodyArenaMirror_[op.arenaSlot].data();
            const size_t storageVerts = buffer.simulated() ? buffer.capacityBytes() / sizeof(SDL_Vertex)
                                                           : bodyArenaMirror_[op.arenaSlot].size();
            drawn.clear(); expected.clear();
            for (uint32_t k = 0; k < op.arenaRanges; ++k) {
                const size_t first = size_t(bodyArenaFirsts_[size_t(op.arenaFirst) + k]);
                const size_t count = size_t(bodyArenaCounts_[size_t(op.arenaFirst) + k]);
                if (first + count > storageVerts ||
                    (op.arenaSlot == bodyArena_.curSlot() && first + count > bodyArena_.writeEnd()))
                    throw std::runtime_error("body arena: range past the written storage");
                drawn.insert(drawn.end(), storage + first, storage + first + count);
            }
            for (uint32_t k = op.firstRange; k < op.firstRange + op.ranges; ++k) {
                const BodyRange& r = bodyRanges_[k];
                const SDL_Vertex* data = bodySources_[r.source].data + r.offset;
                expected.insert(expected.end(), data, data + r.count);
            }
            if (drawn.size() != size_t(op.count) || expected.size() != drawn.size() ||
                std::memcmp(drawn.data(), expected.data(), drawn.size() * sizeof(SDL_Vertex)) != 0) {
                char what[160];
                std::snprintf(what, sizeof what, "body arena: command of %d vertices (%u ranges) differs from its sources",
                              op.count, op.arenaRanges);
                throw std::runtime_error(what);
            }
            ++bodyArenaVerify_.commands;
            bodyArenaVerify_.vertices += drawn.size();
        }
        ++bodyArenaVerify_.frames;
        bodyArenaVerify_.uploaded += stats.uploadedVerts;
        bodyArenaVerify_.retained += stats.verts - std::min(stats.verts, stats.uploadedVerts);
        bodyArenaVerify_.oldCommands += stats.oldCommands;
        if (stats.mode == ta::render::ArenaMode::Append) ++bodyArenaVerify_.appends;
        else if (stats.rollover) ++bodyArenaVerify_.rollovers;
        else ++bodyArenaVerify_.rebuilds;
        bodyArenaVerify_.drains += bodyArena_.releaseSlot() >= 0;
#endif
    }

    // Debug (TA_BODY_ARENA_VERIFY): every assembled command equals its sources,
    // and the assembled commands and staged uploads fill bodyVerts_ without a gap
    // (a write failure re-assembles in place of the staged uploads).
    void GameView::verifyBodyAssembled(size_t stagedVerts) {
#ifdef NDEBUG
        (void)stagedVerts;   // debug verifier only
#else
        size_t total = 0;   // commands are checked inside the buffer; their sum must fill it
        for (const DrawOp& op : drawOps_) {
            if (!op.tex || op.count <= 0 || op.arenaFirst >= 0) continue;
            if (op.start < 0 || size_t(op.start) + size_t(op.count) > bodyVerts_.size())
                throw std::runtime_error("body arena: assembled command outside the copy buffer");
            const SDL_Vertex* out = bodyVerts_.data() + op.start;
            for (uint32_t k = op.firstRange; k < op.firstRange + op.ranges; ++k) {
                const BodyRange& r = bodyRanges_[k];
                if (std::memcmp(out, bodySources_[r.source].data + r.offset, r.count * sizeof(SDL_Vertex)) != 0)
                    throw std::runtime_error("body arena: assembled command differs from its sources");
                out += r.count;
            }
            total += size_t(op.count);
            ++bodyArenaVerify_.assembled;
            bodyArenaVerify_.assembledVertices += size_t(op.count);
        }
        if (total + stagedVerts != bodyVerts_.size())
            throw std::runtime_error("body arena: gap or overrun in the copy buffer");
#endif
    }

    void GameView::invalidateRenderTargets() {
        rendererGeneration_.bump();   // every cached target placement is now stale
        mapView_.invalidateRenderTargets();
        fogSubmit_.clear();fogUpload_.clear();fogTexGen_=~0u;
        miniFogUpload_.clear();miniFogTexGen_=~0u;
        distantModelCache_.clear();
        ghostGeometry_.clear(); ghostGeometryBytes_ = 0;   // keyed on the atlases destroyed below
        geomEntries_.makeAllCold();   // owners stay; every entry rebuilds against the new targets
        for (SDL_Texture* t : atlasTex_) if (t) gpuvram::destroy(t);
        atlasTex_.clear();
        resetShadowAtlas(false);
        shadowAtlasOpaqueScratch_.clear();
        shadowAtlasMaskScratch_.clear();
        shadowTileStamps_.clear();shadowTileChecks_.clear();
        shadowSilhouetteAtlasActive_=false;
        atlasTracker_.clear();   // whatever was painted died with the atlases
        releaseBodyArena();      // retained body vertices (H): re-uploaded on the next stable frame
    }

    void GameView::destroyGpuTextures() {
        for (auto& [id, nano] : nanoTextures_) if (nano.texture) SDL_DestroyTexture(nano.texture);
        nanoTextures_.clear();
        invalidateRenderTargets();   // colour atlases + the shadow mask
        assetGeneration_.bump();     // model textures (atlas sources) die below
        auto kill = [](SDL_Texture*& t) { if (t) { gpuvram::destroy(t); t = nullptr; } };
        for (auto& [n, frames] : textures_)
            for (SDL_Texture* t : frames) if (t) gpuvram::destroy(t);
        textures_.clear();
        releasePaletteSheets();
        paletteIndexed_.release();
        paletteTextureSources_.clear();paletteBanks_.clear();
        paletteMode_=PaletteMode::Unset;
        for (auto& page:shadowMaskPages_) {
            if(page.texture)gpuvram::destroy(page.texture);
            if(page.coverage)gpuvram::destroy(page.coverage);
        }
        shadowCoverageTextures_.clear();
        shadowMaskPages_.clear();
        shadowMasks_.clear();
        for (auto& [n, t] : icons_) if (t) gpuvram::destroy(t);
        icons_.clear();
        for (auto& [n, t] : modelIcons_) if (t) gpuvram::destroy(t);
        modelIcons_.clear();

        kill(unitInfoBg_);
        for (auto& tex : unitInfoOk_) kill(tex);
        unitInfoLabelFont_.destroyGlyphs(); unitInfoValueFont_.destroyGlyphs();
        kill(unitInfoIcon_);
        unitInfoIconFor_.clear();
        for (auto& [n, s] : shadowTex_) if (s.tex) gpuvram::destroy(s.tex);
        shadowTex_.clear();
        // FeatArt: .tex ALIASES frames[0] (see featureArtFor) -- destroy the
        // body/shadow frame arrays only; FeatureInst merely borrows these pointers.
        features_.clear();
        featureIndexDirty_=true;
        for (auto& [n, a] : featureArt_) a.destroyTextures();
        featureArt_.clear();
        for (auto& [n, ea] : effectAnims_)
            for (auto& f : ea.frames) if (f.tex) gpuvram::destroy(f.tex);
        effectAnims_.clear();
        explosionsLoaded_ = false;
        fogSubmit_.clear();fogUpload_.clear();miniFogUpload_.clear();
        kill(fogTex_);
        kill(miniFogTex_);
        kill(mapPreviewTex_);
        distantModelCache_.clear();
        distantModelItems_.clear();
        hudFont_.destroyGlyphs();
        bigFont_.destroyGlyphs();
        statFont_.destroyGlyphs();
        scoreboardFont_.destroyGlyphs();
        ta::releaseUiTextGlyphs();   // the TrueType face's sheets (client/uitext.h)
        for (auto& [name, texture] : scoreboardLogos_) if (texture) gpuvram::destroy(texture);
        scoreboardLogos_.clear();
    }

    // Retail's BiLinearFilter (VisualOptions +0x13, default off) is read only by the
    // 3D-hardware unit pass: it bilinearly stretches the unit and unit-shadow images
    // that pass rendered at reduced resolution (KINGDOMS.icd 0x4ebdcc). Terrain,
    // scenery, effects and the interface stay point-sampled whatever it says. This
    // engine draws units at full resolution, so the same switch samples what makes
    // up a unit and its shadow -- the model-texture pages and the shadow silhouette
    // pages -- and nothing else. That is Texture Filtering's UNITS level; pages created
    // later read the level when built.
    SDL_ScaleMode GameView::unitScaleMode() const {
        return ta::texturefilter::unitsLinear(textureFilter_) ? SDL_ScaleModeLinear : SDL_ScaleModeNearest;
    }
    void GameView::setTextureFilter(int level) {
        level = ta::texturefilter::clampLevel(level);
        const bool unitsWas = ta::texturefilter::unitsLinear(textureFilter_);
        textureFilter_ = level;
        if (unitsWas != ta::texturefilter::unitsLinear(level)) {
            SDL_RenderFlush(ren_);   // queued draws must not pick up the new mode
            const SDL_ScaleMode mode = unitScaleMode();
            for (SDL_Texture* t : atlasTex_) if (t) SDL_SetTextureScaleMode(t, mode);
            if (paletteMode_ == PaletteMode::Baked)   // indexed keys are never sampled; the form follows next frame
                for (auto& page : palettePages_) if (page.texture) SDL_SetTextureScaleMode(page.texture, mode);
            for (auto& page : shadowSilhouettePages_) if (page.texture) SDL_SetTextureScaleMode(page.texture, mode);
        }
        updateWorldFilter();
    }
    // EVERYTHING (a non-retail extension: retail had no camera zoom) also smooths the
    // world art a zoomed-in view magnifies -- terrain, feature sprites, effect and
    // projectile sprites and the ground-shadow sprites -- but only past the zoom gate
    // in client/texturefilter.h. Runs once a frame before drawing and touches textures
    // only when the answer flips; art loaded later reads worldScaleMode() when built.
    void GameView::updateWorldFilter() {
        const bool on = ta::texturefilter::worldLinear(textureFilter_, mapView_.zoom(), worldLinear_);
        if (on == worldLinear_) return;
        SDL_RenderFlush(ren_);
        worldLinear_ = on;
        const SDL_ScaleMode mode = worldScaleMode();
        const auto set = [mode](SDL_Texture* t) { if (t) SDL_SetTextureScaleMode(t, mode); };
        for (auto& [key, art] : featureArt_) {
            for (SDL_Texture* t : art.frames) set(t);
            for (SDL_Texture* t : art.shadowFrames) set(t);
            for (SDL_Texture* t : art.fadeFrames) set(t);
        }
        for (auto& [name, anim] : effectAnims_) for (auto& f : anim.frames) set(f.tex);
        for (auto& [name, shadow] : shadowTex_) set(shadow.tex);
        mapView_.setBilinear(on);
    }

    void GameView::buildAtlasLayout() {
        atlasLaidOut_ = true;
        assetGeneration_.bump();   // region rects (and the animated-region list) change
        struct Item { const std::string* name; int w, h; };
        std::vector<Item> items;
        items.reserve(textures_.size());
        for (auto& [name, frames] : textures_) {
            if (frames.empty()) continue;
            int w = 0, h = 0;
            SDL_QueryTexture(frames[0], nullptr, nullptr, &w, &h);
            if (w <= 0 || h <= 0 || w > 512 || h > 512) continue;   // skip oddities
            items.push_back({&name, w, h});
        }
        // Tallest first packs tightest.
        std::sort(items.begin(), items.end(),
                  [](const Item& a, const Item& b) { return a.h > b.h; });
        const int pad = 2, W = 2048;
        int x = pad, y = pad, shelfH = 0;
        for (const auto& it : items) {
            if (x + it.w + pad > W) { x = pad; y += shelfH + pad; shelfH = 0; }
            atlasRect_[*it.name] = SDL_Rect{x, y, it.w, it.h};
            x += it.w + pad;
            shelfH = std::max(shelfH, it.h);
        }
        atlasW_ = W;
        atlasH_ = y + shelfH + pad;
    }

    SDL_Texture* GameView::atlasFor(int slot) {
        if (slot < 0) slot = 0;
        if (!atlasLaidOut_) buildAtlasLayout();
        if (atlasW_ <= 0) return nullptr;
        if (int(atlasTex_.size()) <= slot) atlasTex_.resize(size_t(slot) + 1, nullptr);
        if (atlasTex_[slot]) return atlasTex_[slot];
        // ESSENTIAL, unlike the bake pages: every body of this colour samples this atlas,
        // and a null one skips them (the units vanish). So neither the GPU cap nor another
        // subsystem's failure backoff may refuse it. Over the cap, the terrain caches give
        // back what they hold off-screen first; then it is created regardless (~11 MiB
        // per colour, at most ten). Only its OWN failure delays a retry, by a second, so
        // a NULL from the driver is not re-requested every body of every frame.
        if (SDL_GetTicks64() < atlasRetryAt_) return nullptr;
        const size_t bytes = size_t(atlasW_) * size_t(atlasH_) * 4;
        if (!gpuvram::wouldFit(bytes)) mapView_.shedGpu(bytes);
        SDL_Texture* atlas = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_TARGET, atlasW_, atlasH_);
        if (!atlas) { noteGpuAllocFail(); atlasRetryAt_ = SDL_GetTicks64() + 1000; return nullptr; }
        SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND);
        SDL_Texture* prev = SDL_GetRenderTarget(ren_);
        SDL_SetRenderTarget(ren_, atlas);
        renderStats_.add(ta::render::Counter::AtlasCreated);
        renderStats_.add(ta::render::Counter::TargetSwitches, 2);   // in and back out
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
        SDL_RenderClear(ren_);
        // Most model textures look the same in every colour atlas, so a second colour
        // starts as a copy of an atlas already complete under these generations and
        // repaints only what differs (see paintAtlasContents).
        SDL_Texture* from = nullptr;
        for (size_t other = 0; other < atlasTex_.size() && !from; ++other)
            if (atlasTex_[other] && int(other) != slot &&
                atlasTracker_.known(other, rendererGeneration_.current(), assetGeneration_.current()))
                from = atlasTex_[other];
        ta::render::AtlasPaintCounts counts;
        {
            ta::render::AtlasPainter painter(ren_, &counts);
            paintAtlasContents(slot, painter, from);
            painter.finish();   // before leaving the atlas target
        }
        renderStats_.add(ta::render::Counter::AtlasCopies, counts.copies);
        renderStats_.add(ta::render::Counter::AtlasFlushes, counts.flushes);
        SDL_SetRenderTarget(ren_, prev);
        // NEAREST unless Bilinear Filtering is on; the gutters keep LINEAR in-rect.
        SDL_SetTextureScaleMode(atlas, unitScaleMode());
        atlasTex_[slot] = atlas;
        // Completely painted: the tracker now knows which frame every animated region
        // holds, so the next update repaints only what changes after this.
        atlasTracker_.created(size_t(slot), rendererGeneration_.current(),
                              assetGeneration_.current(), animatedRegions_);
        return atlas;
    }

    // Everything a colour atlas holds, painted into the current (cleared) target: each
    // model texture's slot variant with its source blend mode, and each animated
    // region exactly as AnimatedAtlasTracker repaints it (blend NONE, the frame
    // selected now, transparent once expired). Leaves animatedRegions_ describing
    // what was painted. Also the reference for TA_ATLAS_VERIFY.
    //
    // With `from` (another complete colour atlas of this layout) the target starts as
    // a 1:1 copy of it, and only regions whose content can differ are cleared (rect +
    // gutter) and repainted: textures with per-colour variants, every animated
    // region, and the untracked sequences below. Everything else -- single-frame
    // textures, gutters and padding -- is the same in every colour atlas.
    void GameView::paintAtlasContents(int slot, ta::render::AtlasPainter& painter, SDL_Texture* from) {
        collectAnimatedRegions();
        if (from) painter.duplicate(from);
        for (const auto& [name, r] : atlasRect_) {
            auto it = textures_.find(name);
            if (it == textures_.end() || it->second.empty()) continue;
            // Atlases can also be created after the main animation update
            // (projectiles, debris and previews). Start with the selected frame,
            // not a player-colour index that lasts until the next repaint.
            const auto animation = modelTextureAnimations_.find(name);
            if (animation != modelTextureAnimations_.end() && it->second.size() >= 2)
                continue;   // an animated region: painted below
            // Same in every colour atlas: one frame and no clock.
            if (from && animation == modelTextureAnimations_.end() && it->second.size() < 2) continue;
            if (from) painter.clear(r);
            const size_t ci = animation != modelTextureAnimations_.end()
                ? animation->second.frame
                : (size_t(slot) < it->second.size() ? size_t(slot) : 0);
            if (ci >= it->second.size()) continue;
            painter.copy(it->second[ci], r, nullptr);
        }
        for (const auto& region : animatedRegions_) ta::render::paintAnimatedRegion(painter, region);
    }

#ifndef NDEBUG
    // TA_ATLAS_VERIFY (debug): after each update, every colour atlas must equal, byte
    // for byte -- content, gutters and padding -- a fresh atlas completely painted
    // with the frames selected now. Covers frame changes, expiry, late creation and
    // renderer resets in whatever scene runs. Throws on the first mismatch.
    void GameView::verifyAtlases() {
        if (atlasW_ <= 0 || atlasH_ <= 0) return;
        SDL_Texture* prev = SDL_GetRenderTarget(ren_);
        const size_t bytes = size_t(atlasW_) * size_t(atlasH_) * 4;
        std::vector<uint8_t> live(bytes), reference(bytes);
        for (size_t slot = 0; slot < atlasTex_.size(); ++slot) {
            SDL_Texture* atlas = atlasTex_[slot];
            if (!atlas) continue;
            SDL_Texture* scratch = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_TARGET, atlasW_, atlasH_);
            if (!scratch) throw std::runtime_error("atlas verify: scratch target allocation failed");
            SDL_SetRenderTarget(ren_, scratch);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
            SDL_RenderClear(ren_);
            {
                ta::render::AtlasPainter painter(ren_);
                paintAtlasContents(int(slot), painter);
                painter.finish();
            }
            const int readRef = SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_RGBA32,
                                                     reference.data(), atlasW_ * 4);
            SDL_SetRenderTarget(ren_, atlas);
            const int readLive = SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_RGBA32,
                                                      live.data(), atlasW_ * 4);
            SDL_SetRenderTarget(ren_, prev);
            gpuvram::destroy(scratch);
            if (readRef || readLive) throw std::runtime_error("atlas verify: readback failed");
            ++atlasVerifyChecks_;
            if (live == reference) continue;
            size_t at = 0, differing = 0;
            for (size_t i = 0; i < bytes; i += 4)
                if (std::memcmp(&live[i], &reference[i], 4)) { if (!differing++) at = i / 4; }
            const int x = int(at % size_t(atlasW_)), y = int(at / size_t(atlasW_));
            std::string region = "(none)";
            for (const auto& [name, r] : atlasRect_)
                if (x >= r.x - 1 && x <= r.x + r.w && y >= r.y - 1 && y <= r.y + r.h) region = name;
            std::fprintf(stderr, "ATLAS_VERIFY MISMATCH slot=%zu tick=%u pixels=%zu first=(%d,%d) region=%s\n",
                slot, unsigned(front().gameTick), differing, x, y, region.c_str());
            throw std::runtime_error("animated atlas differs from a complete repaint");
        }
        atlasVerifyRegions_ = animatedRegions_.size();
    }
#endif

    void GameView::preparePiece(const ta::tdo::Object& object,const PieceMeta& meta,
                                const Xform& parent,const Anim* anim,float cy,float sy,
                                PreparedPiece& out) {
        const auto* piece=pieceFor(anim,object.name);
        out.hidden=piece && !piece->visible;
        out.castsShadow=!piece || piece->castsShadow();
        out.shaded=!piece || piece->shaded;
        out.cached=!piece || piece->cached;
        out.transform=scriptTransform(parent,object.x,object.y,object.z,piece);
        out.rotated.resize(!meta.skip && !out.hidden ? object.vertices.size()/3 : 0);
        for (size_t i=0;i<out.rotated.size();++i) {
            float point[3];
            out.transform.apply(object.vertices[i*3],object.vertices[i*3+1],object.vertices[i*3+2],point);
            out.rotated[i]={point[0]*cy+point[2]*sy,point[1],-point[0]*sy+point[2]*cy};
        }
        if(out.children.size()<object.children.size())out.children.resize(object.children.size());
        for (size_t i=0;i<object.children.size();++i)
            preparePiece(object.children[i],meta.children[i],out.transform,anim,cy,sy,out.children[i]);
    }

    void GameView::drawShadowBatch(std::span<const UnitGeom* const> shadows) {
        SDL_Texture* texture=nullptr;
        shadowCompositeBatch_.clear();
        auto flush=[&] {
            if(!shadowCompositeBatch_.empty()) {
                bodySubmit_.draw(ren_,texture,shadowCompositeBatch_,!ta::devFlag("TA_SHADOW_SDL_SUBMIT"));
                noteNeed(Retained::ShadowComposite,shadowCompositeBatch_);
                shadowCompositeBatch_.clear();
                renderStats_.add(ta::render::Counter::ShadowCompositeRuns);
            }
        };
        for(const auto* g:shadows) {
            if(!shadowSilhouetteAtlasActive_ || !g->shadowAtlasPage || ta::devFlag("TA_SHADOW_BATCH_OFF")) {
                flush();texture=nullptr;drawUnitShadow(*g);
                renderStats_.add(ta::render::Counter::ShadowDirectUnits);
                renderStats_.add(ta::render::Counter::ShadowCompositeRuns);
                continue;
            }
            if(texture!=g->shadowAtlasPage) {flush();texture=g->shadowAtlasPage;}
            ta::appendShadowComposite(shadowCompositeBatch_,g->shadowAtlasSrc,g->shadowAtlasDst,
                                       shadowSilhouetteWidth_,shadowSilhouetteHeight_,255-kShadowLevel);
            profShadowVerts_+=g->shadowVerts.size()+g->maskedShadowVerts.size();
        }
        flush();
    }

    void GameView::buildUnitGeom(const UnitR& u, UnitGeom& g, std::vector<Tri>& scratch, bool mayDeferBody) {
        using namespace ta::render;
#ifndef NDEBUG
        if(scheduleVerifyOn_)geomBuildCalls_.fetch_add(1,std::memory_order_relaxed);
#endif
        g.reused=false;g.stages=0;g.bodyRun=0;g.bodyDeferred=false;
        // Telemetry (worker side): per-entry results the render thread tallies.
        // renderStats_ is not written during the parallel pass.
        const bool timed=renderStats_.enabled();
        const auto clockNs=[] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        const int64_t keyStart=timed ? clockNs() : 0;
        g.missMask=0;g.ownerChanged=false;g.reallocs=0;g.shadowKept=false;g.shadowSkipped=false;g.keyNs=g.buildNs=0;
        const auto clear=[&] {
            g.canFly=u.type && u.type->canFly;
            g.localKey.clear();g.verts.clear();g.runs.clear();
            g.shadowVerts.clear();g.maskedShadowVerts.clear();g.maskedShadowRuns.clear();
            g.localPos.clear();g.baseColor.clear();g.shadowLocal.clear();g.maskedShadowLocal.clear();
            g.shadowAtlasPage=nullptr;
            g.nano=false;g.nanoPixels.clear();
        };
        if (u.underConstruction && !u.buildBegun) {clear();return;}
        auto ut=unitType_.find(u.id);
        if (ut==unitType_.end()) {clear();return;}
        auto vt=visuals_.find(u.replacementModel ? u.replacementModel->id : ut->second);
        if (vt==visuals_.end()) {clear();return;}
        const Anim* anim = nullptr;
        auto at = anims_.find(u.id);
        if (at != anims_.end() && !u.replacementModel && !u.corpsePhase) anim = &at->second;

        float zm = mapView_.zoom();
        int slot = playerColorSlot(u.player);
        // Interpolated pose so the unit glides between 30Hz sim ticks (lift computed at the
        // interpolated spot so it stays seated on the terrain as it moves).
        float ix, iz, ih; interpPose(u, ix, iz, ih);
        if (u.replacementModel) {
            ix=u.replacementX;iz=u.replacementZ;ih=u.replacementHeading;
        }
        // A flyer holds its height above the coarse dilated datum, not the relief
        // directly beneath it -- see flyerGround.
        const bool flying = u.type && u.type->canFly;
        float liftX = flying ? flyerDatum(u) * kHeightScaleX_ : terrainLiftX(ix, iz);
        float liftY = flying ? flyerDatum(u) * kHeightScale_ : terrainLift(ix, iz);
        float ax = (ix - mapView_.offX()) * zm - liftX * zm;
        float ay = (iz - mapView_.offY()) * zm - liftY * zm - altLift(u) * zm;
        // FBI waterline: a wading god or a floating hull sits BELOW the water
        // surface, so the sink is added where the terrain lift is subtracted.
        // Zero on land and for every type that carries neither canhover nor floater.
        ay += waterSink(u.type, ix, iz) * zm;
        // A water grave is drawn at its feature's height as it sinks (uLiftY).
        if (u.corpsePhase && u.corpseSinking) ay = (iz - mapView_.offY()) * zm - uLiftY(u) * zm;
        // A decayed body sinks into the ground over its last 60 updates.
        if (u.corpsePhase) ay += u.corpseDecaySink * kHeightScale_ * zm;
        // Body attitude precedes the animated piece tree for both ground and
        // flying units. Convert the retail mirrored basis at the same boundary.
        // Retail applies the birth heading to buildings as well as movers.
        // Their scripts can counter-rotate a build pad independently of the body.
        float facing = -ih;
        // Disco emote: a dancing unit spins, bobs and hue-cycles using the
        // same real-time clock as its music and ten-second duration.
        // The simulation only sequences the trigger and target selection.
        bool disco = dancing(u);
        float discoBob = 0.0f, discoMix = 0.0f;
        SDL_Color discoCol{};
        if (disco) {
            float t = emoteSeconds(u.player);
            facing += t * 6.2831853f;                                // ~1 rev/sec
            discoBob = std::fabs(std::sin(t * 8.0f)) * 11.0f * zm;    // bounce, px
            discoCol = discoHue(t * 0.8f);                           // body tint hue
            discoMix = 0.5f;
        }
        // Headbang emote (Shift+H): no spin -- a sharp downward nod synced to the metal
        // beat (~152 BPM), the body whipping side to side and flashing red on each bang.
        if (headbanging(u)) {
            float ph = emoteSeconds(u.player) * 2.533f * 6.2831853f;             // ~152 bangs/min
            float bang = std::pow(std::max(0.0f, std::sin(ph)), 2.0f);
            discoBob = -bang * 16.0f * zm;                           // dip DOWN (nod)
            facing += std::sin(ph) * 0.55f;                          // hair-whip
            discoCol = SDL_Color{210, 40, 40, 255};                 // deep metal red
            discoMix = bang * 0.5f;                                  // flash on the bang
            disco = true;                                            // reuse the tint path
        }
        const PieceMeta* meta = &vt->second.meta;
        // Animated textures play only on dont-cache pieces (a cached one keeps its
        // first frame: collect(), 0x459b67). Those are redrawn every frame with
        // the playing frame by a zbuffer=1 type (its height-buffered bitmap,
        // 0x4586d6 -> 0x459641 arg 0 -> 0x459b78); any other type draws them
        // directly (0x4593d4 -> 0x4584d0), playing only while a structure is
        // activated (unit+0x10e bit 0) or a mobile unit's mover has speed
        // (mover+0x20), else at the first frame (0x458845..0x45886c, 0x458635).
        if (meta == &vt->second.meta && meta->animated && u.type && !u.corpsePhase &&
            !u.type->zbuffer && !(isStructure(u.type) ? u.active : u.moverMoving))
            meta = &vt->second.still;
        SDL_Texture* atlas = (slot >= 0 && size_t(slot) < atlasTex_.size())
                                 ? atlasTex_[size_t(slot)] : nullptr;
        const float occlusion=wallOcclusionY(u.x,u.z);
        const float altitude=anim ? anim->altitude : 0.0f;
        const float birthP=birthProgress(u.id);
        // TA's own under-construction look replaces the conjure tint (nanoframe.h).
        const bool nano=u.type && u.underConstruction && u.buildBegun && !u.replacementModel &&
                        u.replacementOpacity<0 && !ta::devFlag("TA_NO_NANOFRAME");
        const bool conjuring=u.type && ((u.underConstruction && !nano) || birthP<1.0f);
        const float p=u.replacementOpacity>=0 ? u.replacementOpacity : !u.type ? 1.0f : u.underConstruction
            ? std::clamp(u.hp/u.type->maxHp,0.0f,1.0f) : birthP;
        const bool ghost=u.type && u.type->ghost;

        // Exact stage keys (client/geometrystages.h). Each stage is reused only
        // while every input it reads is identical; identity is explicit too.
        // LOCAL: identity/resources, pose (heading incl. emote spin, body
        // attitude, piece poses incl. hidden/shadow flags, corpse state), palette
        // slot and animated texture frames, construction (shadow), shadow toggle.
        std::vector<uint64_t>& key=workerScratch().key;
        key.clear();
        const auto word=[&](uint64_t value){key.push_back(value);};
        word(uint64_t(u.id));word(reinterpret_cast<uintptr_t>(u.type));
        word(reinterpret_cast<uintptr_t>(&vt->second));
        word(reinterpret_cast<uintptr_t>(atlas));word(uint64_t(u.player));word(uint64_t(slot));
        {const auto attitude=bodyAttitude(u);word(attitude.first);word(attitude.second);}word(u.underConstruction);
        word(u.corpsePhase);word(reinterpret_cast<uintptr_t>(meta));word(shadowsOnFrame_);word(floatBits(facing));
#ifndef NDEBUG
        if (key.size()!=kLocalKeyFixedWords) throw std::logic_error("local geometry key layout");
#endif
        // Texture playback may hold one frame for multiple simulation ticks.
        // Use its actual inputs, so an unchanged frame retains exact geometry.
        if(!ta::devFlag("TA_TEXTURE_TICK_REFERENCE"))
            for(const auto* frame:meta->animationFrames)word(*frame);
        else if(meta->animated)word(front().gameTick);
        const LocalKeyLayout layout{uint32_t(key.size())};
        if(anim) {
            word(reinterpret_cast<uintptr_t>(anim->pieceNames));
            const auto poses=!anim->capturedPose.empty() ? anim->capturedPose :
                anim->vm ? std::span<const ta::cob::PieceState>(anim->vm->pieces()) :
                           std::span<const ta::cob::PieceState>{};
            for(const auto& pose:poses) {
                word(pose.visible);word(pose.cached);word(pose.shaded);word(pose.rendered);
                for(float value:pose.move)word(floatBits(value));
                for(float value:pose.rot)word(floatBits(value));
            }
        }
        // MATERIAL: veterancy, ghost, conjuring progress (+ clock), emote/statue tint.
        const MaterialKey materialKey{uint64_t(u.studyTint),ghost,conjuring,disco,
            uint64_t(discoCol.r)|(uint64_t(discoCol.g)<<8)|(uint64_t(discoCol.b)<<16),
            floatBits(discoMix),floatBits(p),conjuring ? floatBits(animClock_) : 0u,
            uint64_t(255-u.corpseFade())};
        // PLACEMENT: zoom, interpolated anchor (lift, sink, altitude), emote bob,
        // flyer altitude (shadow lean), wall occlusion line (draw-only clip).
        const PlacementKey placementKey{floatBits(zm),floatBits(ax),floatBits(ay),floatBits(discoBob),
            floatBits(altitude),floatBits(occlusion)};
        const bool reference=ta::devFlag("TA_GEOMETRY_NOCACHE") || ta::devFlag("TA_GEOMETRY_REFERENCE");
        const bool localValid=!reference && !g.localKey.empty() && key==g.localKey;
        // "Materialising" = the summon fade-in/shimmer: either a site still conjuring
        // (progress = HP fraction) or a unit just summoned from a building (progress =
        // its viewer-only birth ramp). Its pulse reads the placed screen y.
        const uint8_t stages=planStages(localValid,placementKey==g.placementKey,
                                        materialKey==g.materialKey,conjuring);
        const bool cacheHit=stages==0;
        g.reused=cacheHit;
        g.animatedTexture=meta->animated;
        const bool verify=ta::devFlag("TA_GEOMETRY_VERIFY");
        // Distant-image lookup before body generation (workstream G). When this
        // unit's image is shown without a repaint this frame (the cache reads
        // neither its revision nor its vertices then), the body stages are left
        // pending; the shadow stages run as always, and the revision still
        // advances, so the image's own freshness rule sees every change. The
        // vertex-count rule is kept exact: the pending body has the vertex count
        // this frame's body would have (bodyVertexCountStable).
        bool deferBody=false;
        if(mayDeferBody && distantEarly_ && !reference && !g.localKey.empty() && (stages|g.bodyPending) &&
           zm<=ta::DistantModels::kMaxZoom && g.verts.size()>=ta::DistantModels::kMinVertices &&
           distantEligible(u,flying,occlusion,ay) &&
           (localValid || bodyVertexCountStable(g.localKey,g.localLayout,key,layout)))
            deferBody=distantModelCache_.holds(g.handleKey(),zm,distantScaleX_,distantScaleY_,distantNow_);
        const uint8_t bodyStages=deferBody ? uint8_t(0) : uint8_t(stages|g.bodyPending);
        g.bodyDeferred=deferBody;
#ifndef NDEBUG
        if(verify) {
            if(cacheHit && meta->animated && g.verifiedTick!=front().gameTick)
                ++animatedTextureChecks_;
            g.verifiedTick=front().gameTick;
        }
#endif
        const int64_t keyEnd=timed ? clockNs() : 0;
        g.keyNs=uint32_t(std::min<int64_t>(keyEnd-keyStart,UINT32_MAX));
        // Workstream I: the next build is predicted to redo what this one redoes
        // (moving units keep moving, still ones stay hits); sizes after the build.
        // A hit's estimate is the key check alone: it must not touch the vector
        // headers on the entry's later cache lines, which a hit never reads
        // (measured: reading them cost ~0.1 us per hit, Experiment 11).
        using CM=ta::render::GeometryCostModel;
        const auto predictNext=[&] {
            g.nextCost=CM::pack(CM::estimateNs(stages,bodyStages,key.size(),g.verts.size(),
                                               g.shadowLocal.size()+g.maskedShadowLocal.size()));
        };
        // The nanoframe follows the build fraction and the game tick: rebuilt every frame.
        g.nano=nano;
        // A structure being built keeps its textures' first frames (its per-frame
        // redraw is skipped, 0x45960d); a mobile unit's play (0x459641).
        if (nano) buildNanoframe(u,g,vt->second,isStructure(u.type) ? &vt->second.still : &vt->second.meta,
                                 anim,atlas,facing,zm,ax,ay-discoBob);
        else if (!g.nanoPixels.empty()) g.nanoPixels.clear();
        if(cacheHit && !bodyStages && !verify) {
            g.nextCost=CM::pack(CM::estimateNs(0,0,key.size(),0,0));
            return;
        }

        UnitGeomInputs in;
        in.visual=&vt->second;in.meta=meta;in.anim=anim;in.atlas=atlas;in.facing=facing;
        in.reuse=!ta::devFlag("TA_GEOMETRY_REFERENCE");
        in.body={zm,ax,ay,discoBob};
        // The shadow leans by the flyer's altitude from the same anchor.
        in.shadow={zm,ax + kShadowLX * altitude * zm,ay + (kProjY - kShadowLZ) * altitude * zm};
        in.material.vetGold=(!conjuring && u.studyTint >= 4)
            ? float(std::min(u.studyTint, 10) - 3) / 7.0f * 0.5f : 0.0f;
        in.material.conjuring=conjuring;in.material.progress=p;in.material.clock=animClock_;
        in.material.emote=disco;in.material.emoteColour=discoCol;in.material.emoteMix=discoMix;
        // FBI ghost: a spectral body is see-through. Retail switches the renderer
        // to its translucent blend mode for these; we scale vertex alpha instead.
        in.material.spectral=ghost && !conjuring;
        in.material.fade=u.corpseFade();
        // Body-only local change: the silhouette's own inputs are all unchanged.
        in.shadowValid=!localValid && !reference && !g.localKey.empty() &&
            shadowInputsEqual(g.localKey,g.localLayout,key,layout,meta->animationFrameMasks);

        using MC=MissCause;
        if(!cacheHit) {
            g.ownerChanged=g.owner>=0 && g.owner!=u.id;
            if(reference)g.missCause=uint8_t(MC::Reference);
            else if(g.localKey.empty())g.missCause=uint8_t(MC::Cold);
            else if(g.owner!=u.id)g.missCause=uint8_t(MC::Owner);
            else if(timed) {
                // Every input category that changed (for the Inval* counters) and
                // the one the miss is filed under.
                uint32_t mask=localValid ? 0u : localKeyChanges(g.localKey,g.localLayout,key,layout);
                mask|=materialKeyChanges(g.materialKey,materialKey)|placementKeyChanges(g.placementKey,placementKey);
                g.missMask=mask ? mask : missBit(MC::Changed);
                g.missCause=uint8_t(primaryMissCause(g.missMask));
            } else g.missCause=uint8_t(MC::Changed);
        }
        // Capacity before the update, to count reallocations (telemetry).
        std::array<size_t,12> capacityBefore{};
        if(timed) {size_t k=0;g.forEachVector([&](const auto& v){capacityBefore[k++]=v.capacity();});}
        // A cold entry's shadow revision belongs to whatever held it before.
        const bool cold=g.localKey.empty();
        if(stages & kStageLocal) {g.localKey=key;g.localLayout=layout;g.owner=u.id;}
        g.placementKey=placementKey;g.materialKey=materialKey;
        g.canFly=flying;
        g.ax=ax;g.ay=ay;g.alt=altitude;g.occY=occlusion;
        if(stages) {
            // One value per update, never repeated by this entry or any other.
            const uint64_t revision=geometryRevision_.fetch_add(1,std::memory_order_relaxed)+1;
            g.revision=revision;
            if(stages & kStageLocal)g.localRevision=revision;
            if(stages & kStagePlacement)g.placementRevision=revision;
            if(stages & kStageMaterial)g.materialRevision=revision;
        }
        g.stages=stages;
        // Debug: the previous shadow output, to check the change detection below.
        thread_local UnitGeom previousShadow;
        const bool checkShadowChange=verify && (stages & kStageLocal);
        if(checkShadowChange) {
            previousShadow.shadowLocal=g.shadowLocal;previousShadow.maskedShadowLocal=g.maskedShadowLocal;
            previousShadow.maskedShadowVerts=g.maskedShadowVerts;previousShadow.maskedShadowRuns=g.maskedShadowRuns;
        }
        const bool shadowChanged=runUnitGeomStages(u,g,stages,bodyStages,in,scratch);
        predictNext();
        g.bodyRun=bodyStages;
        g.bodyPending=deferBody ? uint8_t(g.bodyPending|stages) : uint8_t(0);
        g.shadowKept=(stages & kStageLocal) && !shadowChanged && !cold;
        g.shadowSkipped=(stages & kStageLocal) && in.shadowValid;
        if((stages & kStageLocal) && !g.shadowKept)g.shadowRevision=g.localRevision;
        if(checkShadowChange) {
            // Masked positions are rewritten by placement; compare their UVs/colours.
            const auto bytes=[](const auto& a,const auto& b) {
                return a.size()==b.size() && (a.empty() ||
                    std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);
            };
            bool unchanged=bytes(previousShadow.shadowLocal,g.shadowLocal) &&
                bytes(previousShadow.maskedShadowLocal,g.maskedShadowLocal) &&
                previousShadow.maskedShadowRuns==g.maskedShadowRuns &&
                previousShadow.maskedShadowVerts.size()==g.maskedShadowVerts.size();
            for(size_t i=0;unchanged && i<g.maskedShadowVerts.size();++i) {
                const auto& a=previousShadow.maskedShadowVerts[i];const auto& b=g.maskedShadowVerts[i];
                unchanged=!std::memcmp(&a.color,&b.color,sizeof a.color) &&
                          !std::memcmp(&a.tex_coord,&b.tex_coord,sizeof a.tex_coord);
            }
            if(unchanged==shadowChanged)
                throw std::runtime_error("shadow coverage change detection disagrees with the output");
        }
        if(timed) {
            size_t k=0;
            g.forEachVector([&](const auto& v){g.reallocs+=v.capacity()>capacityBefore[k++];});
            g.buildNs=uint32_t(std::min<int64_t>(clockNs()-keyEnd,UINT32_MAX));
        }
        if(verify && localValid) {
            // Every reuse, complete or partial, must equal a complete rebuild from the
            // same inputs, byte for byte.
            thread_local UnitGeom referenceGeom;
            referenceGeom.makeCold();
            UnitGeomInputs complete=in;
            complete.shadowValid=false;   // the reference collects everything
            runUnitGeomStages(u,referenceGeom,kStageLocal|kStagePlacement|kStageMaterial,
                              kStageLocal|kStagePlacement|kStageMaterial,complete,scratch);
            const auto bytes=[](const auto& a,const auto& b) {
                return a.size()==b.size() && (a.empty() ||
                    std::memcmp(a.data(),b.data(),a.size()*sizeof(a[0]))==0);
            };
            // A body left pending (G) is compared when it is generated; its shadow now.
            const bool bodyCurrent=!g.bodyPending;
            if((bodyCurrent && (!bytes(g.verts,referenceGeom.verts) || g.runs!=referenceGeom.runs ||
                                !bytes(g.localPos,referenceGeom.localPos) || !bytes(g.baseColor,referenceGeom.baseColor))) ||
               !bytes(g.shadowVerts,referenceGeom.shadowVerts) ||
               !bytes(g.maskedShadowVerts,referenceGeom.maskedShadowVerts) ||
               g.maskedShadowRuns!=referenceGeom.maskedShadowRuns ||
               std::memcmp(g.shadowBounds.data(),referenceGeom.shadowBounds.data(),sizeof g.shadowBounds) ||
               !samePlacement(g.shadowPlacement,referenceGeom.shadowPlacement) ||
               !bytes(g.shadowLocal,referenceGeom.shadowLocal) ||
               !bytes(g.maskedShadowLocal,referenceGeom.maskedShadowLocal))
                throw std::runtime_error(cacheHit ? "cached unit geometry mismatch"
                    : (stages & kStagePlacement) ? "re-placed unit geometry differs from a rebuild"
                    : "re-coloured unit geometry differs from a rebuild");
#ifndef NDEBUG
            if(cacheHit)++geometryChecks_;
            if(stages & kStagePlacement)++placementChecks_;
            if(stages & kStageMaterial)++materialChecks_;
#endif
        }
    }

    // Drawn whole through drawUnit (clip rects, interleaved effects) instead of the
    // shared batches; never a distant image.
    bool GameView::specialUnit(const UnitR& u, bool canFly, float occY, float ay) const {
        bool occluded = !canFly && occY < ay - 2.0f;
        bool conjuring = u.underConstruction && u.type;
        // Repair targets and completed/cancelled workers can still have
        // particles even without an active construction/reclaim order.
        // Interleave those particles with the model through drawUnit.
        bool working = u.type && (u.buildSiteId != 0 || u.reclaimId != 0);
        return occluded || conjuring || working || !u.constructionParticles.empty() ||
               dancing(u) || headbanging(u);
    }

    // Keep construction, clipped models, translucent bodies and special effects on
    // their original path. Tiny ordinary bodies can submit one cached quad;
    // simulation and animation VMs continue at their normal rate. (Read by the
    // geometry workers too: selSet_ is filled before the parallel pass.)
    bool GameView::distantEligible(const UnitR& u, bool canFly, float occY, float ay) const {
        return !distantOff_ && !specialUnit(u, canFly, occY, ay) &&
            !u.moving() && !u.walking() && !u.corpsePhase && !u.replacementModel &&
            !(u.type && u.type->ghost) && !selSet_.contains(u.id);
    }

    // Workstream G: generate a body that was left pending, on the render thread.
    // Same frame, same inputs: every stage key matches, so only the pending body
    // stages run and the revision does not move.
    void GameView::materializeDistantBody(size_t index) {
        WorkerScratch& ws = workerScratch();
        UnitGeom& g = visGeom(index);
        buildUnitGeom(*visUnits_[index], g, ws.tris, false);
        ws.note();
        auto& item = distantModelItems_[index];
        item.source = g.verts; item.runs = g.runs; item.revision = g.revision; item.bodyDeferred = false;
    }

    void GameView::verifyDistantDeferral(size_t catchup) {
#ifdef NDEBUG
        (void)catchup;   // debug verifier only
#else
        // Every deferred body, regenerated now in a copy of its entry, must keep
        // the entry's revision (no stage input changed: the body was what lagged),
        // its vertex count (the eligibility rule read the pending body's count)
        // and its shadow output (the shadow stages ran this frame); and its image
        // must have been shown (or the catch-up above generated it).
        thread_local std::vector<Tri> scratch;
        thread_local UnitGeom copy;
        ++distantVerify_.frames;
        distantVerify_.catchup += catchup;
        distantVerify_.late += distantModelCache_.lateBodies;
        for (size_t i = 0; i < visUnits_.size(); ++i) {
            const UnitGeom& g = visGeom(i);
            if (!g.bodyDeferred) continue;
            ++distantVerify_.deferred;
            if (!distantModelItems_[i].texture)
                throw std::runtime_error("deferred body drawn without its distant image");
            copy = g;
            buildUnitGeom(*visUnits_[i], copy, scratch, false);
            const auto bytes = [](const auto& a, const auto& b) {
                return a.size() == b.size() && (a.empty() ||
                    std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
            };
            if (copy.revision != g.revision || copy.stages)
                throw std::runtime_error("deferred body: stage inputs changed within the frame");
            if (copy.verts.size() != g.verts.size())
                throw std::runtime_error("deferred body: vertex count differs from the pending body");
            if (!bytes(copy.shadowVerts, g.shadowVerts) || !bytes(copy.maskedShadowVerts, g.maskedShadowVerts) ||
                copy.maskedShadowRuns != g.maskedShadowRuns || copy.shadowRevision != g.shadowRevision)
                throw std::runtime_error("deferred body: shadow output stale");
            ++distantVerify_.checked;
        }
#endif
    }

    bool GameView::runUnitGeomStages(const UnitR& u, UnitGeom& g, uint8_t stages, uint8_t bodyStages,
                                     const UnitGeomInputs& in, std::vector<Tri>& scratch) {
        bool shadowChanged=false;
        using namespace ta::render;
        const Visual& visual=*in.visual;
        const bool verify=ta::devFlag("TA_GEOMETRY_VERIFY");
        WorkerScratch& ws=workerScratch();
        PreparedPiece& prepared=ws.prepared;
        // Sorted compact indices into the collector output (scratch), kept for the
        // single-pass check below.
        std::vector<size_t>& triangleOrder=ws.order;
        const Xform base=bodyTransform(u);
        // The piece transforms serve the body collection and the silhouette; with
        // the body held back (G) they are prepared only when the silhouette is
        // collected.
        if (in.reuse && ((bodyStages & kStageLocal) ||
                         ((stages & kStageLocal) && shadowsOnFrame_ && (!in.shadowValid || verify))))
            preparePiece(visual.model.root,*in.meta,base,in.anim,
                         std::cos(in.facing),std::sin(in.facing),prepared);
        if(bodyStages & kStageLocal) {
            g.verts.clear();g.runs.clear();g.localPos.clear();g.baseColor.clear();
            scratch.clear();
            collect(scratch, in.atlas, visual.model.root, base, in.anim, in.facing, u.player, false,
                    true, false, nullptr, in.meta,false,in.reuse ? &prepared : nullptr,isStructure(u.type));
            if (verify) {
                thread_local std::vector<Tri> reference;
                reference.clear();
                collect(reference,in.atlas,visual.model.root,base,in.anim,in.facing,u.player,false,
                        true,false,nullptr,in.meta,false,nullptr,isStructure(u.type));
                if (reference.size()!=scratch.size()) throw std::runtime_error("body geometry count mismatch");
                for (size_t i=0;i<scratch.size();++i) {
                    const auto& a=scratch[i];const auto& b=reference[i];
                    if(a.tex!=b.tex || a.depth!=b.depth) throw std::runtime_error("body geometry order mismatch");
                    for(int j=0;j<3;++j)
                        if(std::memcmp(&a.v[j],&b.v[j],sizeof(SDL_Vertex)))
                            throw std::runtime_error("body geometry vertex mismatch");
                }
            }
            // Sort compact indices, not complete triangles. The collector's vertex
            // records stay put; stable indices preserve the exact equal-depth order.
            // Each render worker owns its scratch ordering between unit builds.
            triangleOrder.resize(scratch.size());
            for (size_t i=0;i<triangleOrder.size();++i) triangleOrder[i]=i;
            std::stable_sort(triangleOrder.begin(),triangleOrder.end(),
                [&](size_t a,size_t b) { return scratch[a].depth>scratch[b].depth; });
            // Local body: positions apart (placement rewrites screen positions
            // from them), UVs and base colours in the vertices.
            SDL_Texture* cur = nullptr;
            int runStart = 0;
            bool white=true;
            g.verts.resize(triangleOrder.size()*3);
            g.localPos.resize(g.verts.size());
            size_t out=0;
            for (size_t triangleIndex : triangleOrder) {
                const auto& t=scratch[triangleIndex];
                if (t.tex != cur) {
                    if (int(out) > runStart)
                        g.runs.push_back({cur, int(out) - runStart});
                    cur = t.tex;
                    runStart = int(out);
                }
                for (int i = 0; i < 3; ++i,++out) {
                    g.verts[out]=t.v[i];
                    g.localPos[out]=t.v[i].position;
                    white = white && isWhite(t.v[i].color);
                }
            }
            if (int(out) > runStart)
                g.runs.push_back({cur, int(out) - runStart});
            if (!white) {
                g.baseColor.resize(g.verts.size());
                for (size_t i=0;i<g.verts.size();++i) g.baseColor[i]=g.verts[i].color;
            }
        }
        if(bodyStages & kStagePlacement) placeBody(g.localPos,g.verts,in.body);
        if(bodyStages & kStageMaterial) applyBodyMaterial(g.verts,g.baseColor,in.material);
        if((bodyStages & kStageLocal) && verify) verifyMonolithicBody(g,scratch,triangleOrder,in);
        if(stages & kStageLocal) {
            // Skip the BUILD too when shadows are off, not just the draw -- otherwise the
            // toggle saves the 3.2ms submit but still pays to walk the model and emit ~700k
            // vertices nobody looks at, which is most of the point of switching it off.
            if (in.shadowValid) {
                // Body-only local change (workstream E): the retained silhouette is
                // exactly what collecting it again would produce. Debug: prove it.
                if (verify) {
                    thread_local UnitGeom collected;
                    collected.shadowLocal=g.shadowLocal;collected.maskedShadowLocal=g.maskedShadowLocal;
                    collected.maskedShadowVerts=g.maskedShadowVerts;collected.maskedShadowRuns=g.maskedShadowRuns;
                    if ((shadowsOnFrame_ ? buildUnitShadow(u,collected,visual.model.root,*in.meta,in.anim,in.facing,
                                                           scratch,in.reuse ? &prepared : nullptr)
                                         : !g.shadowLocal.empty() || !g.maskedShadowLocal.empty()))
                        throw std::runtime_error("a body-only local change altered the shadow silhouette");
#ifndef NDEBUG
                    ++shadowSkipChecks_;
#endif
                }
            } else if (shadowsOnFrame_)
                shadowChanged=buildUnitShadow(u, g, visual.model.root, *in.meta, in.anim, in.facing,
                                              scratch,in.reuse ? &prepared : nullptr);
            else {
                shadowChanged=!g.shadowLocal.empty() || !g.maskedShadowLocal.empty();
                g.shadowLocal.clear();g.maskedShadowLocal.clear();
                g.maskedShadowVerts.clear();g.maskedShadowRuns.clear();
            }
        }
        if(stages & kStagePlacement) {
            g.shadowPlacement=in.shadow;
            ShadowBoundsAccumulator bounds;
            g.shadowVerts.resize(g.shadowLocal.size());
            for (size_t i=0;i<g.shadowLocal.size();++i) {
                g.shadowVerts[i]=placeShadowPoint(g.shadowLocal[i],in.shadow);
                bounds.add(g.shadowVerts[i]);
            }
            for (size_t i=0;i<g.maskedShadowLocal.size();++i) {
                g.maskedShadowVerts[i].position=placeShadowPoint(g.maskedShadowLocal[i],in.shadow);
                bounds.add(g.maskedShadowVerts[i].position);
            }
            g.shadowBounds=bounds.bounds();
        }
        // Differential validation of the shadow fast path (positions, masks, order,
        // corpse culling) against the full collector, after placement.
        static const bool verifyShadow=ta::devEnv("TA_SHADOW_VERIFY")!=nullptr;
        // Mines and other buried units are skipped: they cast their visible geometry including pieces
        // their scripts mark DONT_CACHE / RENDER_OFF (a deliberate retail deviation in
        // buildUnitShadow), which the reference collector does not model.
        if (verifyShadow && (stages & kStageLocal) && shadowsOnFrame_ &&
            !u.underConstruction && castsBlobShadow(u.type)) {
            const bool corpseCull = u.corpsePhase;
            const float sx=in.shadow.sx,sy=in.shadow.sy,zm=in.shadow.zoom;
            scratch.clear();
            collect(scratch,nullptr,visual.model.root,base,in.anim,in.facing,
                    u.player,false,true,true,nullptr,in.meta,corpseCull);
            size_t opaque=0,masked=0,maskRun=0;
            int inRun=0;
            auto samePoint=[](SDL_FPoint a,SDL_FPoint b){return a.x==b.x && a.y==b.y;};
            for (const auto& triangle:scratch) {
                if (triangle.tex) {
                    if (maskRun<g.maskedShadowRuns.size() && inRun==g.maskedShadowRuns[maskRun].second) {
                        ++maskRun;inRun=0;
                    }
                    if (maskRun>=g.maskedShadowRuns.size() ||
                        g.maskedShadowRuns[maskRun].first!=triangle.tex ||
                        inRun+3>g.maskedShadowRuns[maskRun].second || masked+3>g.maskedShadowVerts.size())
                        throw std::runtime_error("shadow fast path mask/order mismatch");
                    inRun+=3;
                    for (int k=0;k<3;++k) {
                        const auto& before=triangle.v[k];const auto& after=g.maskedShadowVerts[masked++];
                        if (!samePoint(after.position,{sx+before.position.x*zm,sy+before.position.y*zm}) ||
                            !samePoint(after.tex_coord,before.tex_coord) ||
                            after.color.r!=before.color.r || after.color.g!=before.color.g ||
                            after.color.b!=before.color.b || after.color.a!=before.color.a)
                            throw std::runtime_error("shadow fast path masked vertex mismatch");
                    }
                } else for (const auto& vertex:triangle.v) {
                    if (opaque>=g.shadowVerts.size() || !samePoint(g.shadowVerts[opaque++],
                        {sx+vertex.position.x*zm,sy+vertex.position.y*zm}))
                        throw std::runtime_error("shadow fast path opaque vertex/order mismatch");
                }
            }
            if (opaque!=g.shadowVerts.size() || masked!=g.maskedShadowVerts.size() ||
                (masked && (maskRun+1!=g.maskedShadowRuns.size() || inRun!=g.maskedShadowRuns.back().second)))
                throw std::runtime_error("shadow fast path triangle count mismatch");
        }
        return shadowChanged;
    }

    // The body transform and colour code of the former single pass (before the
    // stages), verbatim, over the same sorted collector output.
    void GameView::verifyMonolithicBody(const UnitGeom& g, const std::vector<Tri>& sorted,
                                        std::span<const size_t> order, const UnitGeomInputs& in) {
#ifdef NDEBUG
        (void)g;(void)sorted;(void)order;(void)in;   // debug verifier only
#else
        const float zm=in.body.zoom,ax=in.body.ax,ay=in.body.ay,discoBob=in.body.bob;
        const auto& m=in.material;
        const float p=m.progress;
        const bool conjuring=m.conjuring,disco=m.emote,spectral=m.spectral;
        const SDL_Color discoCol=m.emoteColour;
        const float discoMix=m.emoteMix,vetGold=m.vetGold;
        Uint8 alpha = Uint8(p * 255.0f);
        size_t out=0;
        for (size_t triangleIndex : order) {
            const auto& t=sorted[triangleIndex];
            for (int i = 0; i < 3; ++i) {
                SDL_Vertex v = t.v[i];
                v.position.x = v.position.x * zm + ax;
                v.position.y = v.position.y * zm + ay - discoBob;   // disco bounce (0 otherwise)
                if (vetGold > 0) {
                    v.color.r = Uint8(v.color.r + (255 - v.color.r) * vetGold);
                    v.color.g = Uint8(v.color.g + (200 - v.color.g) * vetGold * 0.85f);
                    v.color.b = Uint8(v.color.b * (1.0f - vetGold * 0.7f));
                }
                if (conjuring) {
                    float pulse = 0.5f + 0.5f * std::sin(m.clock * 7.0f +
                                                         v.position.y * 0.03f);
                    float glow = (1.0f - p) * pulse;
                    v.color.a = alpha;
                    v.color.r = Uint8(v.color.r * (1.0f - 0.75f * glow));
                    v.color.g = Uint8(v.color.g * (1.0f - 0.25f * glow));
                }
                if (disco) {   // blend the body toward the cycling disco hue
                    v.color.r = Uint8(int(v.color.r) + int((int(discoCol.r) - int(v.color.r)) * discoMix));
                    v.color.g = Uint8(int(v.color.g) + int((int(discoCol.g) - int(v.color.g)) * discoMix));
                    v.color.b = Uint8(int(v.color.b) + int((int(discoCol.b) - int(v.color.b)) * discoMix));
                }
                if (spectral) v.color.a = Uint8(int(v.color.a) * 55 / 100);
                if (out>=g.verts.size() || std::memcmp(&v,&g.verts[out],sizeof v))
                    throw std::runtime_error("staged body geometry differs from the single-pass transform");
                ++out;
            }
        }
        if (out!=g.verts.size()) throw std::runtime_error("staged body vertex count differs from the single pass");
        ++monolithicChecks_;
#endif
    }

    // Local stage: reuses `scratch` (whatever the caller had in it is already consumed).
    // Positions are model-projected (before zoom, anchor and altitude lean);
    // placement turns them into screen positions.
    bool GameView::buildUnitShadow(const UnitR& u, UnitGeom& g,
                                   const ta::tdo::Object& root, const PieceMeta& meta,
                                   const Anim* anim, float facing,
                                   std::vector<Tri>& scratch, const PreparedPiece* prepared) {
        if (u.underConstruction || !castsBlobShadow(u.type)) {
            const bool changed=!g.shadowLocal.empty() || !g.maskedShadowLocal.empty();
            g.shadowLocal.clear();g.maskedShadowLocal.clear();
            g.maskedShadowVerts.clear();g.maskedShadowRuns.clear();
            return changed;
        }
        // The output is rewritten in place and each element compared with the one
        // it replaces, so whether the coverage changed is known exactly without a
        // copy or a hash. Runs are tiny: compared with a copy.
        bool same=true;
        size_t opaqueCount=0,maskedCount=0;
        const auto putOpaque=[&](SDL_FPoint point) {
            if (opaqueCount<g.shadowLocal.size()) {
                same=same && !std::memcmp(&g.shadowLocal[opaqueCount],&point,sizeof point);
                g.shadowLocal[opaqueCount]=point;
            } else {same=false;g.shadowLocal.push_back(point);}
            ++opaqueCount;
        };
        // Masked vertex: position written by placement (from maskedShadowLocal).
        const auto putMasked=[&](SDL_FPoint point,SDL_FPoint uv) {
            const SDL_Vertex vertex{point,{255,255,255,255},uv};
            if (maskedCount<g.maskedShadowLocal.size()) {
                const SDL_Vertex& old=g.maskedShadowVerts[maskedCount];
                same=same && !std::memcmp(&g.maskedShadowLocal[maskedCount],&point,sizeof point) &&
                     !std::memcmp(&old.color,&vertex.color,sizeof vertex.color) &&
                     !std::memcmp(&old.tex_coord,&uv,sizeof uv);
                g.maskedShadowLocal[maskedCount]=point;g.maskedShadowVerts[maskedCount]=vertex;
            } else {same=false;g.maskedShadowLocal.push_back(point);g.maskedShadowVerts.push_back(vertex);}
            ++maskedCount;
        };
        std::vector<std::pair<SDL_Texture*,int>>& previousRuns=workerScratch().previousRuns;
        previousRuns.assign(g.maskedShadowRuns.begin(),g.maskedShadowRuns.end());
        g.maskedShadowRuns.clear();
        scratch.clear();
        // Only a body that is actually LYING FLAT culls. See the shadow branch in
        // collect() for why this must not widen: the cull tears UPRIGHT single-sided
        // geometry, which is why living units keep both faces.
        //
        // "Dead" alone was too broad: it covered the death ANIMATION (before the
        // scripted handoff), still standing or mid-fall. corpsePhase is exactly
        // "finished falling, lying on the ground", which is the only case the
        // flat-face artifact arises in.
        const bool corpseCull = u.corpsePhase;
        const float cy=std::cos(facing),sn=std::sin(facing);
        // This traversal emits opaque positions directly. The body collector's
        // texture/atlas lookup, UVs, colours, depth and full Tri staging are not
        // needed for an opaque shadow. Keep its piece/fan order and arithmetic.
        using ShadowVertex=ShadowCollectVertex;
        std::vector<ShadowVertex>& vertices=workerScratch().shadowVerts;
        const auto collectShadow=[&](auto&& self,const ta::tdo::Object& object,
                                     const Xform& parent,const PieceMeta* cached,bool isRoot,
                                     const PreparedPiece* pose)->void {
            const auto* piece=pose ? nullptr : pieceFor(anim,object.name);
            const Xform transform=pose ? pose->transform : scriptTransform(parent,object.x,object.y,object.z,piece);
            PieceMeta local;
            if (!cached) { pieceMetaFor(object,isRoot,local);cached=&local; }
            const bool casts = pose ? pose->castsShadow : !piece || piece->castsShadow();
            if (!cached->skip && casts) {
                for (size_t primitiveIndex=0;primitiveIndex<object.primitives.size();++primitiveIndex) {
                    if (int32_t(primitiveIndex)==object.selectionPrimitive) continue;
                    const auto& primitive=object.primitives[primitiveIndex];
                    if (primitive.indices.size()<3) continue;
                    SDL_Texture* mask=nullptr;
                    SDL_FRect maskUV{0,0,1,1};
                    const std::vector<ShadowMaskFrame>* masks=nullptr;
                    bool animated=false;
                    const auto& name=primitiveIndex<cached->primTex.size()
                        ? cached->primTex[primitiveIndex] : primitive.texture;
                    if (primitiveIndex<cached->primShadowMasks.size()) {
                        masks=cached->primShadowMasks[primitiveIndex];
                        animated=cached->primAnimated[primitiveIndex];
                    } else {
                        const auto found=shadowMasks_.find(name);
                        if (found!=shadowMasks_.end()) masks=&found->second;
                        animated=animatedTex_.count(name)!=0;
                    }
                    if (masks) {
                        const size_t frame=animated ? modelTextureAnimations_.at(name).frame
                            : size_t(playerColorSlot(u.player))%masks->size();
                        if (frame<masks->size()) {mask=(*masks)[frame].texture;maskUV=(*masks)[frame].uv;}
                    }
                    vertices.resize(primitive.indices.size());
                    for (size_t i=0;i<primitive.indices.size();++i) {
                        auto& vertex=vertices[i];
                        const size_t index=size_t(primitive.indices[i])*3;
                        vertex.valid=index+2<object.vertices.size();
                        if (!vertex.valid) continue;
                        if (pose) {
                            const auto& point=pose->rotated[index/3];
                            vertex.x=point[0];vertex.y=point[1];vertex.z=point[2];
                        } else {
                            float position[3];
                            transform.apply(object.vertices[index],object.vertices[index+1],
                                            object.vertices[index+2],position);
                            vertex.x=position[0]*cy+position[2]*sn;
                            vertex.y=position[1];
                            vertex.z=-position[0]*sn+position[2]*cy;
                        }
                        vertex.position={vertex.x+kShadowLX*vertex.y,
                                         -(vertex.z+kShadowLZ*vertex.y)*kProjZ};
                    }
                    for (size_t i=1;i+1<primitive.indices.size();++i) {
                        const size_t indices[3]={0,i,i+1};
                        const auto& a=vertices[0];const auto& b=vertices[i];const auto& c=vertices[i+1];
                        if (!a.valid || !b.valid || !c.valid) continue;
                        if (corpseCull) {
                            const float ax=b.x-a.x,ay=b.y-a.y,az=b.z-a.z;
                            const float bx=c.x-a.x,by=c.y-a.y,bz=c.z-a.z;
                            const float ny=az*bx-ax*bz,nz=ax*by-ay*bx;
                            if (ny*kSortY-nz*kSortZ<=0.0f) continue;
                        }
                        if (mask) {
                            if (g.maskedShadowRuns.empty() || g.maskedShadowRuns.back().first!=mask)
                                g.maskedShadowRuns.push_back({mask,0});
                            g.maskedShadowRuns.back().second+=3;
                            static constexpr SDL_FPoint uv[4]={{0,0},{1,0},{1,1},{0,1}};
                            for (int k=0;k<3;++k)
                                putMasked(vertices[indices[k]].position,
                                          {maskUV.x+uv[indices[k]&3].x*maskUV.w,
                                           maskUV.y+uv[indices[k]&3].y*maskUV.h});
                        } else {
                            for (size_t index:indices) putOpaque(vertices[index].position);
                        }
                    }
                }
            }
            // Hidden parents still transform their visible children.
            for (size_t i=0;i<object.children.size();++i)
                self(self,object.children[i],transform,
                     i<cached->children.size() ? &cached->children[i] : nullptr,false,
                     pose ? &pose->children[i] : nullptr);
        };
        collectShadow(collectShadow,root,bodyTransform(u),&meta,true,prepared);
        same=same && opaqueCount==g.shadowLocal.size() && maskedCount==g.maskedShadowLocal.size() &&
             previousRuns==g.maskedShadowRuns;
        g.shadowLocal.resize(opaqueCount);
        g.maskedShadowLocal.resize(maskedCount);g.maskedShadowVerts.resize(maskedCount);
        return !same;
    }

    // TotalA.exe 0x458dd0, drawn as retail draws it: the unit rasterised into its
    // own height-buffered bitmap, one texel per game pixel (client/nanoraster.h),
    // recoloured by each pixel's height byte (0x458d30), then every primitive's
    // outline depth-tested against the same bytes (0x458fa0). drawUnit uploads
    // the bitmap and scales it by the zoom.
    void GameView::buildNanoframe(const UnitR& u, UnitGeom& g, const Visual& visual, const PieceMeta* meta,
                                  const Anim* anim, SDL_Texture* atlas, float facing, float zm,
                                  float ax, float ay) {
        using namespace ta::nanoframe;
        thread_local std::vector<Tri> tris;
        thread_local NanoCollect nc;
        thread_local std::vector<RasterVertex> verts;
        thread_local std::vector<int32_t> vertexStart;
        thread_local std::vector<size_t> order;
        thread_local std::vector<RasterRow> rows;
        thread_local std::vector<uint8_t> zbuffer, covered;
        tris.clear();nc.clear();

        collect(tris,atlas,visual.model.root,bodyTransform(u),anim,facing,
                u.player,false,true,false,nullptr,meta,false,nullptr,isStructure(u.type),&nc);

        int first=0,second=0;
        shimmer(uint32_t(u.id),front().gameTick,first,second);
        const Bands bands=ta::nanoframe::bands(u.buildRemaining,first,second);
        const uint8_t lower=uint8_t(lowerCut(bands)),upper=uint8_t(bands.level);
#ifndef NDEBUG
        static const bool log=ta::devFlag("TA_NANOFRAME_LOG");
        if (log) std::fprintf(stderr,"nanoframe: unit %d tick %u remaining %.4f level %d bands %d/%d/%d\n",
                              u.id,front().gameTick,u.buildRemaining,bands.level,bands.below,bands.band,bands.above);
#endif
        const auto rgba=[](SDL_Color c) {
            return uint32_t(c.r)<<24 | uint32_t(c.g)<<16 | uint32_t(c.b)<<8 | uint32_t(c.a);
        };
        const auto green=[&](int index) {
            return rgba(nanoPalette_[size_t(std::clamp(index-0xa0,0,15))]);
        };
        // Whole-pixel vertices (0x459027..0x459051): x = X>>16,
        // y = -(Z>>16) - ((Y>>16)>>1), height = (Y>>16) + 50, or 125 for a
        // digger (UnitDef +0x241 bit 30, 0x459a2f).
        const int32_t bias=u.type && u.type->digger ? kDiggerHeightBias : kHeightBias;
        verts.clear();
        int32_t minX=INT32_MAX,minY=INT32_MAX,maxX=INT32_MIN,maxY=INT32_MIN;
        for (const auto& p:nc.points) {
            RasterVertex v;
            const int32_t up=int32_t(std::floor(p[1]));
            v.x=int32_t(std::floor(p[0]));
            v.y=int32_t(std::floor(-p[2]))-(up>>1);
            v.h=up+bias;
            minX=std::min(minX,v.x);maxX=std::max(maxX,v.x);
            minY=std::min(minY,v.y);maxY=std::max(maxY,v.y);
            verts.push_back(v);
        }
        g.nanoPixels.clear();g.nanoW=g.nanoH=0;
        if (verts.empty()) return;
        const int32_t w=std::min(maxX-minX+2,1024),h=std::min(maxY-minY+2,1024);
        g.nanoW=w;g.nanoH=h;
        g.nanoPixels.assign(size_t(w)*size_t(h),0u);
        zbuffer.assign(size_t(w)*size_t(h),0);   // cleared to 0 (0x4598e4)
        covered.assign(size_t(w)*size_t(h),0);
        for (auto& v:verts) {v.x-=minX;v.y-=minY;}

        // Pieces from the last to the first (0x45993c..0x459bdf), each piece's
        // primitives in order.
        order.resize(nc.prims.size());
        for (size_t i=0;i<order.size();++i) order[i]=i;
        std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {
            return nc.prims[a].piece>nc.prims[b].piece;
        });
        thread_local std::vector<RasterVertex> poly;
        const auto polygon=[&](const NanoCollect::Prim& prim) {
            poly.assign(verts.begin()+prim.first,verts.begin()+prim.first+prim.count);
            return poly.data();
        };
        for (size_t index:order) {
            const auto& prim=nc.prims[index];
            // A textured primitive is drawn only as a quad (0x459b1a); a coloured
            // one with any count (0x4c1000).
            if (prim.frame && prim.count!=4) continue;
            RasterVertex* v=polygon(prim);
            const ta::render::palette::Bank* bank=prim.frame && prim.bank>=0 && size_t(prim.bank)<paletteBanks_.size()
                ? &paletteBanks_[size_t(prim.bank)] : nullptr;
            if (prim.frame) {
                // Corner k of the texture (0x4c87a6: (0,0), (w-1,0), (w-1,h-1), (0,h-1)).
                const int32_t tw=prim.frame->width-1,th=prim.frame->height-1;
                const int32_t cu[4]={0,tw,tw,0},cv[4]={0,0,th,th};
                for (int k=0;k<4;++k) {v[k].u=cu[k];v[k].v=cv[k];}
            }
            uint32_t flat=rgba(SDL_Color{170,170,180,255});
            if (!prim.frame && prim.colorIndex>=0 && nanoGamePaletteSet_)
                flat=nanoGamePalette_[size_t(prim.colorIndex)];
            walkPolygon(v,int(prim.count),rows,[&](int32_t y,const RasterRow& row) {
                if (y<0 || y>=h) return;
                const int32_t span=row.right-row.left;
                const int32_t dh=(row.hr-row.hl)/span;
                const int32_t du=(row.ur-row.ul)/span,dv=(row.vr-row.vl)/span;
                int32_t hz=row.hl,tu=row.ul,tv=row.vl;
                for (int32_t x=row.left;x<row.right;++x,hz+=dh,tu+=du,tv+=dv) {
                    if (x<0 || x>=w) continue;
                    const size_t at=size_t(y)*size_t(w)+size_t(x);
                    const uint8_t byte=heightByte(hz);
                    if (zbuffer[at]>byte) continue;
                    uint32_t colour=flat;
                    if (prim.frame) {
                        const int32_t sx=std::clamp(tu>>16,0,prim.frame->width-1);
                        const int32_t sy=std::clamp(tv>>16,0,prim.frame->height-1);
                        const size_t texel=size_t(sy)*size_t(prim.frame->width)+size_t(sx);
                        if (texel*2+1>=prim.frame->la.size() || !prim.frame->la[texel*2+1]) continue;
                        uint8_t index=prim.frame->la[texel*2];
                        if (bank) {
                            if (prim.level>=0 && !bank->shades.empty())
                                index=bank->shades[size_t(prim.level)*256+index];
                            const auto* e=bank->palette[index];
                            colour=rgba(SDL_Color{e[0],e[1],e[2],255});
                        }
                    }
                    zbuffer[at]=byte;covered[at]=1;
                    g.nanoPixels[at]=colour;
                }
            });
        }
        // 0x458d30: every drawn pixel by its height byte against the cut.
        for (size_t at=0;at<covered.size();++at) {
            if (!covered[at]) continue;
            const uint8_t n=zbuffer[at];
            const int code=n<lower ? bands.below : n<upper ? bands.band : bands.above;
            if (code==kTransparent) g.nanoPixels[at]=0;
            else if (code!=kKeep) g.nanoPixels[at]=green(code);
        }
        // 0x458fa0 -> 0x4c0820 -> 0x4c0a90: each primitive's row ends in the
        // second green where the buffer's byte is not above its own; the byte is
        // written (a cut-away surface still hides what lies below it).
        const uint32_t outline=green(second);
        for (size_t index:order) {
            const auto& prim=nc.prims[index];
            walkPolygon(polygon(prim),int(prim.count),rows,[&](int32_t y,const RasterRow& row) {
                if (y<0 || y>=h) return;
                const auto plot=[&](int32_t x,int32_t hz) {
                    if (x<0 || x>=w) return;
                    const size_t at=size_t(y)*size_t(w)+size_t(x);
                    const uint8_t byte=heightByte(hz);
                    if (zbuffer[at]>byte) return;
                    g.nanoPixels[at]=outline;zbuffer[at]=byte;
                };
                plot(row.left,row.hl);
                plot(row.right,row.hr);
            });
        }
        g.nanoDst={ax+float(minX)*zm,ay+float(minY)*zm,float(w)*zm,float(h)*zm};
    }

    // The nanoframe bitmap (buildNanoframe) on screen: one streaming texture per
    // unit being built, nearest-scaled by the zoom as retail's pixels are.
    void GameView::drawNanoframe(int unitId, const UnitGeom& g) {
        if (g.nanoPixels.empty() || g.nanoW <= 0 || g.nanoH <= 0) return;
        ++nanoTextureFrame_;
        NanoTexture& slot = nanoTextures_[unitId];
        if (!slot.texture || slot.w < g.nanoW || slot.h < g.nanoH) {
            if (slot.texture) SDL_DestroyTexture(slot.texture);
            slot.w = std::max(slot.w, g.nanoW); slot.h = std::max(slot.h, g.nanoH);
            slot.texture = SDL_CreateTexture(ren_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING,
                                             slot.w, slot.h);
            if (slot.texture) {
                SDL_SetTextureBlendMode(slot.texture, SDL_BLENDMODE_BLEND);
                SDL_SetTextureScaleMode(slot.texture, SDL_ScaleModeNearest);
            }
        }
        slot.used = nanoTextureFrame_;
        if (!slot.texture) return;
        const SDL_Rect area{0, 0, g.nanoW, g.nanoH};
        SDL_UpdateTexture(slot.texture, &area, g.nanoPixels.data(), g.nanoW * 4);
        SDL_RenderCopyF(ren_, slot.texture, &area, &g.nanoDst);
        // Drop the textures of units no longer being built (every 256 draws).
        if (nanoTextureFrame_ - nanoTexturePruned_ >= 256) {
            nanoTexturePruned_ = nanoTextureFrame_;
            for (auto it = nanoTextures_.begin(); it != nanoTextures_.end();) {
                if (nanoTextureFrame_ - it->second.used > 256) {
                    if (it->second.texture) SDL_DestroyTexture(it->second.texture);
                    it = nanoTextures_.erase(it);
                } else ++it;
            }
        }
    }

    void GameView::drawUnit(const UnitR& u, uint8_t bodyAlpha) {
        // A placed-but-not-yet-started site shows as a faint ghost until the
        // builder arrives and it begins conjuring for real.
        if (u.underConstruction && !u.buildBegun) {
            if (u.type) drawGhostAt(u.type, u.x, u.z);
            return;
        }
        auto vt = visuals_.find(unitType_.at(u.id));
        if (vt == visuals_.end()) return;
        // No Anim lookup here any more: the pose was applied during the parallel
        // projection below, so this function only submits the result.
        // The model projection (collect + sort + screen transform + colour) was
        // done for every visible unit in parallel on the worker pool this frame;
        // here we just look up the result and submit its draw calls.
        int gslot = geomSlot(u.id);
        if (gslot < 0) return;
        UnitGeom& g = visGeom(size_t(gslot));
        float zm = mapView_.zoom();
        float ax = g.ax, ay = g.ay;
        // Terrain occlusion: if a wall between the unit and the camera projects its
        // top above the unit's feet, clip the model to that line and re-draw the
        // hidden part as a faint player-tinted silhouette showing through the wall.
        // Flyers ride above the terrain, so a wall never hides them.
        float occY = g.occY;
        bool occluded = !g.canFly && occY < ay - 2.0f;
        int outW = 0, outH = 0;
        SDL_bool hadClip = SDL_FALSE;
        SDL_Rect prevClip{};
        bool topShows = true;   // false: the wall top is at or above the target's top edge
        if (occluded) {
            SDL_GetRendererOutputSize(ren_, &outW, &outH);
            hadClip = SDL_RenderIsClipEnabled(ren_);
            if (hadClip) SDL_RenderGetClipRect(ren_, &prevClip);
            int line = std::clamp(int(occY), 0, outH);
            SDL_Rect top{0, 0, outW, line};   // only pixels above the wall top show
            topShows = ta::cliprect::set(ren_, top);   // an empty clip draws nothing (Metal: skip)
        }
        // Shadows are emitted once by the dedicated ground/air passes. Special
        // body rendering must not composite the same shadow a second time.
        // Disco dance floor: a pulsing, hue-cycling glow disc under a dancing unit.
        if (topShows && dancing(u)) {
            float t = emoteSeconds(u.player);
            SDL_Color dc = discoHue(t * 0.8f + 0.5f);   // offset from the body tint
            float rad = (float(std::max(u.type->footX, u.type->footZ)) * 12.0f + 22.0f)
                        * (0.85f + 0.15f * std::sin(t * 8.0f)) * zm;   // pulse with the bob
            const int N = 24;
            std::vector<SDL_Vertex> fan;
            fan.reserve(N * 3);
            SDL_Vertex ctr{{ax, ay}, {dc.r, dc.g, dc.b, 150}, {0, 0}};
            auto rim = [&](float a) {
                return SDL_Vertex{{ax + std::cos(a) * rad, ay + std::sin(a) * rad * 0.5f},
                                  {dc.r, dc.g, dc.b, 0}, {0, 0}};
            };
            for (int i = 0; i < N; ++i) {
                fan.push_back(ctr);
                fan.push_back(rim(float(i) / N * 6.2831853f));
                fan.push_back(rim(float(i + 1) / N * 6.2831853f));
            }
            SDL_BlendMode pbm;
            SDL_GetRenderDrawBlendMode(ren_, &pbm);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_ADD);
            SDL_RenderGeometry(ren_, nullptr, fan.data(), int(fan.size()), nullptr, 0);
            SDL_SetRenderDrawBlendMode(ren_, pbm);
        }
        // Headbang: a red mosh-pit glow that flares on each downbeat.
        if (topShows && headbanging(u)) {
            float ph = emoteSeconds(u.player) * 2.533f * 6.2831853f;
            float bang = std::pow(std::max(0.0f, std::sin(ph)), 2.0f);
            SDL_Color dc{Uint8(150 + 90 * bang), Uint8(20 + 20 * bang), 20, 255};
            float rad = (float(std::max(u.type->footX, u.type->footZ)) * 12.0f + 22.0f)
                        * (0.75f + 0.45f * bang) * zm;
            const int N = 24;
            std::vector<SDL_Vertex> fan;
            fan.reserve(N * 3);
            SDL_Vertex ctr{{ax, ay}, {dc.r, dc.g, dc.b, Uint8(120 + 100 * bang)}, {0, 0}};
            auto rim = [&](float a) {
                return SDL_Vertex{{ax + std::cos(a) * rad, ay + std::sin(a) * rad * 0.5f},
                                  {dc.r, dc.g, dc.b, 0}, {0, 0}};
            };
            for (int i = 0; i < N; ++i) {
                fan.push_back(ctr);
                fan.push_back(rim(float(i) / N * 6.2831853f));
                fan.push_back(rim(float(i + 1) / N * 6.2831853f));
            }
            SDL_BlendMode pbm;
            SDL_GetRenderDrawBlendMode(ren_, &pbm);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_ADD);
            SDL_RenderGeometry(ren_, nullptr, fan.data(), int(fan.size()), nullptr, 0);
            SDL_SetRenderDrawBlendMode(ren_, pbm);
        }
        // Submit the pre-built, depth-sorted vertex runs -- one SDL_RenderGeometry
        // per texture (usually 1 per unit). The veterancy/conjure colour tint was
        // already baked into the vertices on the worker pool.
        // Workstream M: a unit with indexed palette runs draws all its runs in one
        // native pass when every run texture can join (one priming, not one per run).
        if (!topShows) {
            // Nothing above the wall top: only the silhouette below.
        } else if (g.nano) {
            drawNanoframe(u.id, g);
        } else if (bodyAlpha != 255) {
            drawFadedBody(gslot, bodyAlpha);
        } else if (!drawRunsNative(g.runs, g.verts.data())) {
            int off = 0;
            for (const auto& r : g.runs) {
                // A null run texture (an atlas page that failed to allocate under
                // VRAM pressure -- corpse meshes load mid-game) must be SKIPPED:
                // SDL_RenderGeometry(nullptr,..) paints the raw vertex colours as a
                // solid white shape. Invisible-until-retry beats a white flash.
                if (r.first) drawModelRun(r.first, g.verts.data() + off, r.second, false);
                off += r.second;
            }
        }


        // Occluded: re-draw the hidden lower part as a faint, flat player-coloured
        // silhouette through the wall, so a unit behind cover is never fully lost.
        if (occluded) {
            int line = std::clamp(int(occY), 0, outH);
            SDL_Rect bot{0, line, outW, std::max(0, outH - line)};
            const bool botShows = ta::cliprect::set(ren_, bot);   // empty: no silhouette (Metal: skip)
            SDL_Color tc = playerColor(u.player);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            // The silhouette is flat, untextured and single-colour, so the whole
            // model collapses to ONE draw call: re-tint every vertex and submit.
            noteNeed(Retained::ModelTriBatch, triBatch_);
            triBatch_.clear();
            for (const SDL_Vertex& sv : g.verts) {
                SDL_Vertex v = sv;
                v.color = SDL_Color{tc.r, tc.g, tc.b, 70};
                triBatch_.push_back(v);
            }
            if (botShows && !triBatch_.empty())
                SDL_RenderGeometry(ren_, nullptr, triBatch_.data(), int(triBatch_.size()), nullptr, 0);
            if (hadClip) SDL_RenderSetClipRect(ren_, &prevClip);
            else SDL_RenderSetClipRect(ren_, nullptr);
        }
    }

    size_t GameView::drawTacticalDots(int mvw, int winH) {
        // Every unit the minimap would show, as a flat square in its player colour
        // -- the minimap's dot, drawn in the world. One untextured geometry batch:
        // no model projection, no atlas, no shadow, no render target.
        overlayBatch_.clear();
#ifndef NDEBUG
        debugDotIds_.clear();
#endif
        const SDL_Color rim{12, 12, 16, 220};     // dark edge: reads on grass and sand
        size_t drawn = 0;
        // Ground first, airborne flyers second, so a flyer's dot sits on top of the
        // army it is flying over, as its model would.
        for (int pass = 0; pass < 2; ++pass) {
            for (const UnitR* up : front().live) {
                const UnitR& u = *up;
                if (!mainViewVisible(u)) continue;   // a dot is the main view: no radar
                bool aloft = false;
                if (u.type->canFly) {
                    auto ait = anims_.find(u.id);
                    aloft = ait == anims_.end() || ait->second.altitude > 1.0f;
                }
                if (aloft != (pass == 1)) continue;
                const SDL_FPoint c = dotCentre(u);   // terrain lift + flyer altitude
                const float half = dotSide(u) * 0.5f;
                if (c.x + half < -4 || c.x - half > mvw + 4 ||
                    c.y + half < -4 || c.y - half > winH + 4) continue;
                appendDot(overlayBatch_, u, c, rim);
                ++drawn;
#ifndef NDEBUG
                debugDotIds_.push_back(u.id);
#endif
            }
        }
        if (!overlayBatch_.empty()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(ren_, nullptr, overlayBatch_.data(),
                               int(overlayBatch_.size()), nullptr, 0);
        }
#ifndef NDEBUG
        debugDotCount_ = drawn;
#endif
        return drawn;
    }

    void GameView::appendDot(std::vector<SDL_Vertex>& b, const UnitR& u, SDL_FPoint c, const SDL_Color& rim) {
        const SDL_Color selRim{255, 255, 255, 255};
        const float side = dotSide(u), half = side * 0.5f;
        const bool selected = selSet_.contains(u.id);
        // Selected: a bright 2px outline; otherwise a 1px rim.
        const float edge = selected ? 2.0f : 1.0f;
        pushQuad(b, c.x - half - edge, c.y - half - edge,
                 side + 2 * edge, side + 2 * edge, selected ? selRim : rim);
        SDL_Color col = playerColor(u.player);
        // An unfinished conjure reads as half there, like its ghosted model.
        if (u.underConstruction) col.a = 150;
        pushQuad(b, c.x - half, c.y - half, side, side, col);
    }

    void GameView::drawRing(float wx, float wz, float r) {
        float zm = mapView_.zoom();
        float lift = terrainLift(wx, wz) * zm;
        float liftX = terrainLiftX(wx, wz) * zm;
        SDL_SetRenderDrawColor(ren_, 90, 255, 120, 255);
        SDL_FPoint pts[25];
        for (int i = 0; i <= 24; ++i) {
            float a = float(i) / 24 * 2 * 3.14159f;
            pts[i] = {(wx + std::cos(a) * r - mapView_.offX()) * zm - liftX,
                      (wz + std::sin(a) * r * 0.7f - mapView_.offY()) * zm - lift};
        }
        SDL_RenderDrawLinesF(ren_, pts, 25);
    }

    const GameView::ShadowTex* GameView::shadowFor(const std::string& art) {
        if (art.empty()) return nullptr;
        if (!shadowsLoaded_) {
            shadowsLoaded_ = true;
            const auto* pal = featurePalette("");   // TA: the game palette (palettes/palette.pal)
            if (pal) try {
                for (auto& sq : ta::gaf::load(vread("anims/shadows.gaf"), *pal, -1,
                                               "anims/shadows.gaf")) {
                    if (sq.frames.empty() || sq.frames[0].width == 0) continue;
                    auto& fr = sq.frames[0];
                    std::vector<uint8_t> px = fr.rgba;   // silhouette -> translucent black
                    for (size_t i = 0; i + 3 < px.size(); i += 4) {
                        px[i] = px[i + 1] = px[i + 2] = 0;
                        px[i + 3] = px[i + 3] ? 90 : 0;
                    }
                    SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                       SDL_TEXTUREACCESS_STATIC,
                                                       fr.width, fr.height);
                    SDL_UpdateTexture(t, nullptr, px.data(), fr.width * 4);
                    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(t, worldScaleMode());
                    std::string name = sq.name;
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    shadowTex_[name] = {t, fr.width, fr.height, fr.xoff, fr.yoff};
                }
            } catch (const std::exception&) {}
        }
        auto it = shadowTex_.find(art);
        return it != shadowTex_.end() ? &it->second : nullptr;
    }

    void GameView::loadFeatureDefs() {
        if (!featureDefs_.empty()) return;
        try {
            for (const std::string& path : vfs_.list("features")) {
                if (ta::vpath::extension(path) != ".tdf") continue;
                try {
                    auto root = vtdf(path);
                    for (const auto& n : root.childOrder) {
                        std::string k = n;
                        std::transform(k.begin(), k.end(), k.begin(), ::tolower);
                        featureDefs_[k] = root.children.at(n);
                    }
                } catch (const std::exception&) {}
            }
        } catch (const std::exception&) {}
    }

    const ta::gaf::Palette* GameView::featurePalette(std::string world) {
        std::transform(world.begin(), world.end(), world.begin(), ::tolower);
        auto it = featurePals_.find(world);
        if (it != featurePals_.end()) return &it->second;
        // Total Annihilation draws every feature through the one game palette
        // (palettes/palette.pal); per-world PCX palettes are a mod/legacy fallback.
        const std::string cands[] = {std::string("palette.pal"), world + "_features.pcx", world + ".pcx"};
        for (const std::string& cand : cands) {
            try {
                std::string pp = "palettes/" + cand;
                return &featurePals_
                            .emplace(world, ta::gaf::Palette::fromBytes(vread(pp), pp))
                            .first->second;
            } catch (const std::exception&) {}
        }
        return nullptr;
    }

    // Upload RGBA with the transparent texels' RGB BLED from their opaque
    // neighbours: GAF art keeps RGB=black under alpha 0, so bilinear sampling
    // mixes that black into the visible edge -- a 1px dark contour traced
    // around every feathered sprite (grass tufts on sand made it obvious).
    // Colour-bleeding fixes the fringe with ordinary BLEND mode, so it works
    // on every renderer backend (custom/premultiplied blend modes are NOT
    // supported by the software renderer and silently fall back to opaque).
    static void premulUpload(SDL_Texture* t, const std::vector<uint8_t>& rgba, int w) {
        std::vector<uint8_t> px = rgba;
        int h = w > 0 ? int(px.size() / 4) / w : 0;
        // Two passes so the bleed reaches diagonal/1px-gap texels too.
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<uint8_t> src = px;
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    size_t i = (size_t(y) * w + x) * 4;
                    if (src[i + 3] != 0) continue;   // visible texel: keep
                    int r = 0, g = 0, b = 0, n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                            size_t j = (size_t(ny) * w + nx) * 4;
                            if (src[j + 3] == 0 &&
                                (pass == 0 || (src[j] | src[j+1] | src[j+2]) == 0))
                                continue;   // neighbour has no colour to lend
                            r += src[j]; g += src[j + 1]; b += src[j + 2]; ++n;
                        }
                    if (n) { px[i] = uint8_t(r / n); px[i + 1] = uint8_t(g / n);
                             px[i + 2] = uint8_t(b / n); }
                }
        }
        SDL_UpdateTexture(t, nullptr, px.data(), w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }

    // Retail feature playback: one shared clock per TYPE (every instance of a
    // sequence shows the identical frame -- variety comes from the 16 different wave
    // sequences, not phase), 30Hz engine ticks holding each frame for its GAF delay
    // (waves ship delay=2 -> 15fps), looping continuously. Each frame is drawn at its
    // OWN size + anchor -- the wave art authors motion that way. Burning: seqnameburn
    // playback replaces the standing art (retail swaps to the burn anim + flame
    // overlays on ignition). The body pass draws exactly this; the HIDDEN UNITS
    // cover tests read it.
    GameView::FeatureSprite GameView::featureSprite(const FeatureInst& f, float zm) {
        FeatureSprite out;
        // Lifted onto the terrain relief just like a unit (see the body pass).
        const float lfx = terrainLiftX(f.x, f.z) * zm, lfy = terrainLift(f.x, f.z) * zm;
        const auto place = [&](int fw, int fh, int fxo, int fyo) {
            out.dst = {(f.x - mapView_.offX() - float(fxo)) * zm - lfx,
                       (f.z - mapView_.offY() - float(fyo)) * zm - lfy,
                       float(fw) * zm, float(fh) * zm};
        };
        const auto maskOf = [](const FeatArt* art, size_t idx) -> const ta::hiddenunits::AlphaMask* {
            return art && idx < art->masks.size() ? &art->masks[idx] : nullptr;
        };
        // Destroyed, playing seqnamedie once (494ce0 mode 0): the frame its clock
        // has reached, counted back from the tick featuredead replaces it.
        if (f.dieEnd && f.dieArt && f.dieArt->tex) {
            const FeatArt* da = f.dieArt;
            const size_t idx = std::min(da->frames.size() - 1, ta::featuredie::frame(da->tickEnd,
                ta::featuredie::elapsed(da->totalTicks, f.dieEnd, front().gameTick)));
            out.tex = da->frames[idx];
            const auto& g = da->fgeom[idx];
            place(g.w, g.h, g.xoff, g.yoff);
            out.mask = maskOf(da, idx);
            out.burning = true;
            return out;
        }
        if (f.burnVis && f.burnArt && f.burnArt->tex) {
            const uint32_t burnAge = int32_t(front().gameTick-f.burnStarted) < 0
                ? 0 : front().gameTick-f.burnStarted;
            const FeatArt* ba = f.burnArt;
            out.tex = ba->tex;
            size_t idx = 0;
            int fw = ba->w, fh = ba->h, fxo = ba->xoff, fyo = ba->yoff;
            if (ba->frames.size() > 1 && ba->totalTicks > 0) {
                int tick = int(burnAge % uint32_t(ba->totalTicks));
                idx = size_t(std::upper_bound(ba->tickEnd.begin(), ba->tickEnd.end(), tick) -
                             ba->tickEnd.begin());
                if (idx >= ba->frames.size()) idx = 0;
                out.tex = ba->frames[idx];
                const auto& g = ba->fgeom[idx];
                fw = g.w; fh = g.h; fxo = g.xoff; fyo = g.yoff;
            }
            place(fw, fh, fxo, fyo);
            out.mask = maskOf(ba, idx);
            out.burning = true;
            return out;
        }
        out.tex = f.tex;
        size_t idx = 0;
        int fw = f.w, fh = f.h, fxo = f.xoff, fyo = f.yoff;
        if (f.frames && !f.frames->empty() && f.art && f.art->totalTicks > 0) {
            const uint32_t age = front().gameTick;
            int tick = int(age % uint32_t(f.art->totalTicks));
            idx = size_t(std::upper_bound(f.art->tickEnd.begin(), f.art->tickEnd.end(), tick) -
                         f.art->tickEnd.begin());
            if (idx >= f.frames->size()) idx = 0;
            out.tex = (*f.frames)[idx];
            const auto& g = f.art->fgeom[idx];
            fw = g.w; fh = g.h; fxo = g.xoff; fyo = g.yoff;
            if (!f.art->bodyLoop && age >= uint32_t(f.art->totalTicks)) out.tex = nullptr;
        }
        place(fw, fh, fxo, fyo);
        out.mask = out.tex ? maskOf(f.art, idx) : nullptr;
        out.art = f.art;
        out.frame = idx;
        if (f.tree && settings_ && settings_->treeSway) {
            const float phase = f.x*0.043f+f.z*0.029f;
            const float sway = std::sin(animClock_*1.1f+phase)*0.7f+
                               std::sin(animClock_*2.7f+phase*1.7f)*0.3f;
            out.sway = true;
            out.shear = sway * out.dst.h * 0.04f;
        }
        return out;
    }

    void GameView::FeatArt::destroyTextures() {
        for (SDL_Texture* t : frames) if (t) gpuvram::destroy(t);
        for (SDL_Texture* t : shadowFrames) if (t) gpuvram::destroy(t);
        for (SDL_Texture* t : fadeFrames) if (t) gpuvram::destroy(t);
        frames.clear(); shadowFrames.clear(); fadeFrames.clear();
        tex = shadow = nullptr;
    }

    // The reclaim fade's white silhouette of one frame (client/reclaimfade.h),
    // made from the frame's opacity mask on first use.
    SDL_Texture* GameView::fadeSilhouette(const FeatArt& art, size_t frame) {
        if (frame >= art.masks.size()) return nullptr;
        if (art.fadeFrames.size() < art.frames.size()) art.fadeFrames.resize(art.frames.size(), nullptr);
        SDL_Texture*& made = art.fadeFrames[frame];
        if (made) return made;
        const auto& mask = art.masks[frame];
        if (mask.width() <= 0 || mask.height() <= 0) return nullptr;
        made = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                               mask.width(), mask.height());
        if (!made) return nullptr;
        const std::vector<uint8_t> px = ta::reclaimfade::silhouette(mask);
        SDL_UpdateTexture(made, nullptr, px.data(), mask.width() * 4);
        SDL_SetTextureBlendMode(made, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(made, worldScaleMode());
        return made;
    }

    GameView::FeatArt* GameView::featureArtFor(const ta::tdf::Node& def,
                                               const char* seqKey, const char* shadKey) {
        std::string file = def.valueOr("filename", "");
        std::string seq = def.valueOr(seqKey, "");
        std::string seqShad = def.valueOr(shadKey, "");
        if (seq.empty()) return nullptr;   // e.g. a def without seqnameburn
        const std::string world = def.valueOr("world", "");
        const bool burn = std::string_view(seqKey) == "seqnameburn";
        // seqnamedie and seqnamedieshad load nonlooping (493eff, 493f4f).
        const bool die = std::string_view(seqKey) == "seqnamedie";
        const bool animate = burn || die || def.numberOr("animating", 0) != 0;
        const std::array<std::string, 6> key{file, seq, seqShad, world,
                                           animate ? "1" : "0", burn ? "1" : die ? "2" : "0"};
        auto it = featureArt_.find(key);
        if (it != featureArt_.end()) return it->second.tex ? &it->second : nullptr;
        FeatArt a{};
        const auto* pal = featurePalette(world);
        if (pal) {
            try {
                std::string f = file;
                std::transform(f.begin(), f.end(), f.begin(), ::tolower);
                auto ieq = [](const std::string& x, const std::string& y) {
                    if (x.size() != y.size()) return false;
                    for (size_t i = 0; i < x.size(); ++i)
                        if (std::tolower(x[i]) != std::tolower(y[i])) return false;
                    return true;
                };
                for (auto& sq : ta::gaf::load(vread("anims/" + f + ".gaf"), *pal, -1,
                                               "anims/" + f + ".gaf")) {
                    if (sq.frames.empty()) continue;
                    auto& fr = sq.frames[0];
                    if (ieq(sq.name, seq)) {
                        a.bodyLoop = !die && (!animate || sq.loopFlag != 0);
                        size_t nf = animate ? sq.frames.size() : 1;
                        // Keep EVERY frame at its own size/anchor: wave sequences
                        // author their motion through per-frame w/h/xoff/yoff (the
                        // foam sweeps by re-anchoring each tightly-cropped frame),
                        // so dropping non-uniform frames froze them.
                        int acc = 0;
                        for (size_t fi = 0; fi < nf; ++fi) {
                            auto& ff = sq.frames[fi];
                            // Blank frames still occupy their authored clock slot.
                            // A transparent backing texture keeps them loadable even
                            // when the first frame has no drawable geometry.
                            const bool blank = ff.width == 0 || ff.height == 0;
                            SDL_Texture* t = gpuvram::create(
                                ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                blank ? 1 : ff.width, blank ? 1 : ff.height);
                            if (!t) continue;   // VRAM-pressure alloc fail: drop the frame
                            if (blank) premulUpload(t, std::vector<uint8_t>(4, 0), 1);
                            else premulUpload(t, ff.rgba, ff.width);
                            SDL_SetTextureScaleMode(t, worldScaleMode());
                            a.frames.push_back(t);
                            a.masks.emplace_back();
                            if (!blank) a.masks.back().build(ff.rgba.data(), ff.width, ff.height);
                            a.fgeom.push_back({ff.width, ff.height, ff.xoff, ff.yoff});
                            acc += std::max(1, int(ff.retailDelayTicks));
                            a.tickEnd.push_back(acc);
                        }
                        a.totalTicks = acc;
                        if (!a.frames.empty()) {
                            a.tex = a.frames[0];
                            a.w = a.fgeom[0].w; a.h = a.fgeom[0].h;
                            a.xoff = a.fgeom[0].xoff; a.yoff = a.fgeom[0].yoff;
                        }
                    } else if (!seqShad.empty() && ieq(sq.name, seqShad)) {
                        // Retail forces burn-shadow sequences nonlooping at load.
                        a.shadowLoop = !burn && !die && (!animate || sq.loopFlag != 0);
                        const size_t count = animate ? sq.frames.size() : 1;
                        for (size_t index = 0; index < count; ++index) {
                            const auto& frame = sq.frames[index];
                            const bool blank = frame.width == 0 || frame.height == 0;
                            std::vector<uint8_t> px = blank ? std::vector<uint8_t>(4, 0) : frame.rgba;
                            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                                px[i] = px[i + 1] = px[i + 2] = 0;
                                px[i + 3] = px[i + 3] ? 90 : 0;
                            }
                            auto* texture = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC, blank ? 1 : frame.width, blank ? 1 : frame.height);
                            if (texture) {
                                premulUpload(texture, px, blank ? 1 : frame.width);
                                SDL_SetTextureScaleMode(texture, worldScaleMode());
                            }
                            a.shadowFrames.push_back(texture);
                            a.shadowGeom.push_back({frame.width, frame.height, frame.xoff, frame.yoff});
                            a.shadowDurations.push_back(frame.retailDelayTicks);
                        }
                        a.shadow = a.shadowFrames.front();
                        a.sw = fr.width; a.sh = fr.height;
                        a.sxoff = fr.xoff; a.syoff = fr.yoff;
                    }
                }
            } catch (const std::exception&) {}
        }
        featureArt_[key] = a;
        return featureArt_[key].tex ? &featureArt_[key] : nullptr;
    }

    bool GameView::addFeature(const std::string& rawName, int cx, int cz) {
        loadFeatureDefs();
        std::string key = rawName;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        auto di = featureDefs_.find(key);
        static const bool kFeatLog = ta::devEnv("TA_FEATLOG") != nullptr;
        if (di == featureDefs_.end()) {
            if (kFeatLog) std::fprintf(stderr, "feature %s: no definition (%zu defs)\n", key.c_str(), featureDefs_.size());
            return false;
        }
        FeatArt* a = featureArtFor(di->second);
        if (!a) {
            if (kFeatLog) std::fprintf(stderr, "feature %s: no art (%s/%s)\n", key.c_str(),
                                       di->second.valueOr("filename", "").c_str(), di->second.valueOr("seqname", "").c_str());
            return false;
        }
        FeatureInst inst;
        inst.tex = a->tex;
        inst.frames = &a->frames;
        inst.art = a;   // per-frame geometry + GAF tick timeline (stable: map node)
        inst.shadow = a->shadow;
        inst.w = a->w; inst.h = a->h; inst.xoff = a->xoff; inst.yoff = a->yoff;
        inst.sw = a->sw; inst.sh = a->sh; inst.sxoff = a->sxoff; inst.syoff = a->syoff;
        inst.fx = int(di->second.numberOr("footprintx", 1));
        inst.fz = int(di->second.numberOr("footprintz", 1));
        inst.x = float(cx * 16 + inst.fx * 8);
        inst.z = float(cz * 16 + inst.fz * 8);
        inst.name = key;
        inst.simId = cz * mapView_.map().width + cx;
        featInstIds_.insert(inst.simId);
        std::string cat = di->second.valueOr("category", "");
        std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
        inst.tree = (cat == "trees");
        inst.low = ta::hiddenunits::lowFeature(int(di->second.numberOr("height", 0)));
        features_.push_back(inst);
        featureIndexDirty_=true;
        // NO NAV BLOCKING HERE. Feature blocking belongs to the SIM, and
        // registerMapFeatures/setupMatch already does it -- into the shared obst_
        // overlay, with this exact rule and footprint. Doing it again here wrote to
        // nav_.cells_, a layer the REFEREE never touches because it never runs the
        // viewer, so the client's ground grid gained blockers the server's lacked
        // from load. nav_ is what losBetween reads, so auto-acquisition saw different
        // line-of-sight on the two peers, picked different targets, and desynced
        // (a Cannoneer at tick 252 of an 8-AI stress game -- one unit in 15215).
        // Movement never noticed because it uses the per-class grids, not nav_.
        return true;
    }

    // Burn-out art swap: the sim swapped this feature to its featureburnt stage
    // in place (same cell/id); re-point the instance at the burnt feature's art.
    void GameView::swapFeatureArt(FeatureInst& fi, const std::string& name) {
        fi.name = name;
        fi.burnArt = nullptr;
        fi.dieArt = nullptr;
        fi.frontFlame = fi.backFlame = nullptr;
        fi.tree = fi.simType == fi.treeType;   // a burnt stump/smudge doesn't sway
        auto di = featureDefs_.find(name);
        fi.low = di != featureDefs_.end() &&
                 ta::hiddenunits::lowFeature(int(di->second.numberOr("height", 0)));
        FeatArt* a = di != featureDefs_.end() ? featureArtFor(di->second) : nullptr;
        if (!a) { fi.tex = nullptr; fi.frames = nullptr; fi.art = nullptr;
                  fi.shadow = nullptr; return; }   // no art: vanish (draw skips)
        fi.tex = a->tex; fi.frames = &a->frames; fi.art = a; fi.shadow = a->shadow;
        fi.w = a->w; fi.h = a->h; fi.xoff = a->xoff; fi.yoff = a->yoff;
        fi.sw = a->sw; fi.sh = a->sh; fi.sxoff = a->sxoff; fi.syoff = a->syoff;
    }

    // Per-frame sync of sim burning state onto the visual features: ignition
    // starts the seqnameburn playback + smoke, burn-out swaps to the burnt art.
    void GameView::syncBurningFeatures() {
        // tryLiveWorld(): this runs on the RENDER thread but reads live world_
        // state the worker mutates. gameview.h documents the invariant -- the
        // render side reads the published snapshot, and any remaining live read
        // locks -- and this was the one place breaking it. Not a benign torn
        // int either: featureAt() hands back a pointer INTO the feature vector,
        // and area reclaim removes features, so the worker can reallocate that
        // vector under this loop.
        // COPY under the lock, then do the visual work outside it. Holding
        // simMutex_ across the whole scan meant the worker waited on art
        // replacement, atlas lookups, texture loads and effect spawns. Render-side
        // acquisition is nonblocking so neither thread waits on the other's work.
        // The generation counter gates both the locked scan and the visual apply.
        // Smoke emission and motion use the simulation tick queue, not this scan.
        std::vector<FeatSim>& simState = featSimState_;
        const uint32_t featGen = worldTables().featGeneration();   // atomic; no lock needed
        const bool featDirty = featGen != lastFeatGen_ || simState.size() != features_.size();
        if (!featDirty && !ta::devFlag("TA_FEATURE_FULL_SYNC")) return;
        struct NewFeat { int id; std::string name; };
        std::vector<NewFeat> fresh;
        // A simulation tick can take seconds in a crowded match. Keep drawing
        // the previous visual state while it owns the lock; retry next frame.
        // Copy existing and newly created features under ONE acquisition so a
        // failed retry cannot mark unseen corpses as already synchronized.
        auto refresh = [&] {
            if (!featDirty) return true;
            const auto live = tryLiveWorld();
            if (!live) return false;
            const ta::sim::World& world = **live;
            if (world.featureTypes().empty()) return false;
            simState.clear();
            simState.reserve(features_.size());
            for (const auto& fi : features_) {
                const auto* sf = world.feature(fi.simId);
                simState.push_back(sf ? FeatSim{sf->type, sf->alive && sf->burn != 0,
                                                sf->alive, sf->fx, sf->fz, true, sf->burnStarted, world.featureReclaimable(*sf),sf->blocks,
                                                ta::reclaimfade::fadeEnd(world.tickCount(), sf->fadeLeft),
                                                sf->dieLeft ? world.tickCount() + sf->dieLeft : 0}
                                      : FeatSim{-1, false, true, 1, 1, false});
            }
            // The type NAMES are read below too, and featureTypes() is filled at map
            // load and never mutated after -- but copy the ones we need anyway
            // rather than reach back into world_ unlocked.
            for (auto& st : simState)
                if (st.type >= 0 && size_t(st.type) < world.featureTypes().size())
                    burnNames_[size_t(st.type)] = world.featureTypes()[size_t(st.type)].name;
            for (const auto& sf : world.features()) {
                if (!sf.alive || sf.type < 0 || featInstIds_.count(sf.id)) continue;
                if (size_t(sf.type) >= world.featureTypes().size()) continue;
                fresh.push_back({sf.id, world.featureTypes()[size_t(sf.type)].name});
            }
            lastFeatGen_ = featGen;
            return true;
        };
        if (!refresh()) return;
        if (simState.size() != features_.size()) return;   // nothing synced yet
        size_t fidx = 0;
        for (auto& fi : features_) {
            const FeatSim st = simState[fidx++];
            fi.aliveVis = st.alive ? 1 : 0;   // what the draw loop reads
            if (st.hasSim) {
                if(fi.fx!=st.fx || fi.fz!=st.fz)featureIndexDirty_=true;
                fi.fx = st.fx; fi.fz = st.fz;
                fi.x = float((fi.simId % mapView_.map().width) * 16 + fi.fx * 8);
                fi.z = float((fi.simId / mapView_.map().width) * 16 + fi.fz * 8);
            }
            fi.reclaimable=st.reclaimable;
            fi.blocking=st.blocking;
            fi.fadeEnd=st.fadeEnd;
            fi.dieEnd=st.dieEnd;
            fi.hasSim = st.hasSim ? 1 : 0;    // ...and whether it is reclaimable at all
            if (st.type < 0) continue;
            if (fi.simType == -2) {                             // first sight
                fi.simType = st.type;
                if (fi.tree) fi.treeType = st.type;
            }
            else if (st.type != fi.simType) {                   // burnt-stage swap
                fi.simType = st.type;
                swapFeatureArt(fi, burnNames_[size_t(st.type)]);
            }
            if (!st.dieEnd) fi.dieArt = nullptr;
            else if (!fi.dieArt) {                              // death edge
                auto di = featureDefs_.find(fi.name);
                if (di != featureDefs_.end())
                    fi.dieArt = featureArtFor(di->second, "seqnamedie", "seqnamedieshad");
            }
            const bool b = st.burning;
            if (b && !fi.burnVis) {                             // ignition edge
                auto di = featureDefs_.find(fi.name);
                fi.burnStarted = st.burnStarted;
                fi.frontFlame = fi.backFlame = nullptr;
                if (di != featureDefs_.end()) {
                    fi.burnArt = featureArtFor(di->second, "seqnameburn",
                                               "seqnameburnshad");
                    const auto flame = [&](const char* key) -> const EffectAnim* {
                        const auto name = di->second.valueOr(key, "");
                        return name.empty() ? nullptr : effectFor(name);
                    };
                    fi.frontFlame = flame("seqnamefrontflame");
                    fi.backFlame = flame("seqnamebackflame");
                }
                static const bool kBurnLog = ta::devEnv("TA_BURNLOG") != nullptr;
                if (kBurnLog)
                    std::fprintf(stderr, "burn-vis %s art=%d front=%zu back=%zu\n", fi.name.c_str(),
                                 fi.burnArt && fi.burnArt->tex ? 1 : 0,
                                 fi.frontFlame ? fi.frontFlame->frames.size() : 0,
                                 fi.backFlame ? fi.backFlame->frames.size() : 0);

            }
            fi.burnVis = b ? 1 : 0;

        }
        // Features the SIM created mid-game (corpses) get a visual instance on
        // first sight. No nav blocking here -- the sim owns corpse blocking.
        // Same rule: copy under the lock, load art outside it (addFeature can pull
        // a GAF off disk, which is not something to hold the sim behind).
        for (const auto& sf : fresh) {
            addFeature(sf.name, sf.id % mapView_.map().width, sf.id / mapView_.map().width);
            featInstIds_.insert(sf.id);   // even on art failure: don't retry every frame
        }
    }

    void GameView::loadFeatures() {
        features_.clear();   // full rebuild -- safe to call again on a map change
        featureIndexDirty_=true;
        featInstIds_.clear();
        featSimState_.clear();lastFeatGen_=UINT32_MAX;
        const auto& names = mapView_.map().featureNames;
        if (names.empty()) return;
        loadFeatureDefs();
        const auto& map = mapView_.map();
        // Draw only what the sim installed: of two overlapping TNT features retail
        // keeps the later one (495360/496380) and its draw scan (4fc8d9) sees only
        // installed cells, so the earlier one is gone -- not drawn, picked, shown
        // on the minimap or offered to the order trail. The raw plane is only the
        // fallback for a world that never ran the install (no placement plane).
        const auto& installed = world_.installedMapFeatures();
        const auto& plane = installed.size() == map.features.size() ? installed : map.features;
        int placed = 0;
        for (int cz = 0; cz < map.height; ++cz)
            for (int cx = 0; cx < map.width; ++cx) {
                uint16_t v = plane[size_t(cz) * map.width + cx];
                if (v >= names.size()) continue;
                if (addFeature(names[v], cx, cz))
                    ++placed;
            }
        std::printf("features: %d placed\n", placed);
        featuresAtLoad_ = features_.size();
    }

    bool GameView::buildIconClick(float mx, float my, bool lmb, bool rmb) {
        if (!lmb && !rmb) return false;
        const auto* b = selectedBuilder();
        if (!b) return false;   // selection can change before the next HUD draw
        for (const auto& [r, bt] : iconRects_) {
            if (mx < r.x || mx > r.x + r.w || my < r.y || my > r.y + r.h) continue;
            playClickTone();
            if (!bt) return true;   // consume the click even if it can't act
            uint16_t mod = SDL_GetModState();
            bool ctrl = (mod & KMOD_CTRL) != 0, shift = (mod & KMOD_SHIFT) != 0;
            // Ctrl+left-click = infinite production (RepeatTrain), FIRST so it also
            // works for a mobile conjurer (a beast tamer): the sim spawns each new
            // unit beside the producer and re-queues, no manual placement needed.
            // A structure is always placed (the sim refuses to repeat one), so
            // Ctrl+click on its icon starts a placement like a plain click.
            if (lmb && ctrl && !shift && !isStructure(bt)) {
                ta::net::Command c;
                c.kind = ta::net::Cmd::RepeatTrain;
                c.unitId = b->id;
                std::snprintf(c.type, sizeof c.type, "%s", bt->id.c_str());
                issue(c);
                return true;
            }
            if (isStructure(bt) || !isStructure(b->type)) {
                if (lmb) { placing_ = bt;shiftBuildPlaced_=false; } // fresh manual placement
                return true;
            }
            ta::net::Command c;
            c.unitId = b->id;
            std::snprintf(c.type, sizeof c.type, "%s", bt->id.c_str());
            c.kind = lmb ? ta::net::Cmd::Train : ta::net::Cmd::Unqueue;
            c.targetId = (ctrl && shift) ? 10 : shift ? 5 : 1;
            issue(c);
            return true;
        }
        return false;
    }

    SDL_Texture* GameView::iconFor(const std::string& typeId) {
        auto it = icons_.find(typeId);
        if (it != icons_.end()) return it->second;
        SDL_Texture* tex = nullptr;
        try {
            // Through makeTexture like the rest of the GUI art: with Smooth GUI Art on,
            // the portrait is edge-upscaled 2x and filtered linearly, so it stops
            // looking blocky at large build-bar scales. Callers size from their own
            // boxes (or the texture's aspect), so the 2x dimensions change no layout.
            auto img = ta::jpeg::load(vread("anims/buildpic/" + typeId + ".jpg"));
            tex = ta::art::makeTexture(ren_, img.rgba, img.width, img.height);
        } catch (const std::exception&) {}
        icons_[typeId] = tex;
        return tex;
    }

    SDL_Texture* GameView::modelIconTex(const std::string& id, int slot, bool canMove) {
        auto keyp = std::make_pair(id, slot);
        if (auto it = modelIcons_.find(keyp); it != modelIcons_.end()) return it->second;
        if (gpuAllocBlocked()) return nullptr;   // retry after the alloc backoff lapses
        modelIcons_[keyp] = nullptr;   // cache the attempt (success or failure) up front
        if (!ghostModel(id)) return nullptr;
        auto vt = visuals_.find(id);
        if (vt == visuals_.end()) return nullptr;
        SDL_Texture* atlas = atlasFor(slot);
        std::vector<Tri> scratch;
        // `canMove` here is the CALLER's word for "show it turned a little", not the
        // FBI flag -- see the call sites, which pass isStructure(). A build icon is a
        // portrait, so the 3/4 turn is a presentation choice, not a facing.
        float facing = canMove ? -0.6f : 0.0f;   // slight 3/4 turn reads as a portrait
        collect(scratch, atlas, vt->second.model.root, Xform{}, nullptr, facing, 0, false);
        if (scratch.empty()) return nullptr;
        std::stable_sort(scratch.begin(), scratch.end(),
                         [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
        float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
        for (auto& t : scratch)
            for (int i = 0; i < 3; ++i) {
                minX = std::min(minX, t.v[i].position.x);
                minY = std::min(minY, t.v[i].position.y);
                maxX = std::max(maxX, t.v[i].position.x);
                maxY = std::max(maxY, t.v[i].position.y);
            }
        const int ICON = 64;
        SDL_Texture* tgt = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_TARGET, ICON, ICON);
        // Transient VRAM failure: un-cache so the icon renders after the backoff
        // (the up-front nullptr stays only for permanent no-model failures).
        if (!tgt) { noteGpuAllocFail(); modelIcons_.erase(keyp); return nullptr; }
        SDL_SetTextureBlendMode(tgt, SDL_BLENDMODE_BLEND);
        SDL_Texture* prev = SDL_GetRenderTarget(ren_);
        SDL_SetRenderTarget(ren_, tgt);
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
        SDL_RenderClear(ren_);
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        float span = std::max({maxX - minX, maxY - minY, 1.0f});
        float s = 0.82f * float(ICON) / span;
        float ox = float(ICON) * 0.5f - (minX + maxX) * 0.5f * s;
        float oy = float(ICON) * 0.5f - (minY + maxY) * 0.5f * s;
        for (auto& t : scratch) {
            SDL_Vertex v[3];
            for (int i = 0; i < 3; ++i) {
                v[i] = t.v[i];
                v[i].position.x = t.v[i].position.x * s + ox;
                v[i].position.y = t.v[i].position.y * s + oy;
            }
            SDL_RenderGeometry(ren_, t.tex, v, 3, nullptr, 0);
        }
        SDL_SetRenderTarget(ren_, prev);
        modelIcons_[keyp] = tgt;
        return tgt;
    }

    void GameView::drawHDebug() {
        const auto& m = mapView_.map();
        if (m.heights.empty()) return;
        float zm = mapView_.zoom();
        // Tint cells whose height is well above ground (candidate "wall" cells).
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        int cx0 = std::max(0, int(mapView_.offX()) / 16 - 1);
        int cz0 = std::max(0, int(mapView_.offY()) / 16 - 1);
        int cx1 = std::min(m.width, cx0 + int(1000 / zm / 16) + 3);
        int cz1 = std::min(m.height, cz0 + int(1000 / zm / 16) + 3);
        for (int cz = cz0; cz < cz1; ++cz)
            for (int cx = cx0; cx < cx1; ++cx) {
                int h = m.heights[size_t(cz) * m.width + cx];
                int d = h - modalHeight_;
                if (d < 8) continue;
                Uint8 a = Uint8(std::min(150, 30 + d));
                SDL_SetRenderDrawColor(ren_, 220, 40, 40, a);
                SDL_FRect r{(cx * 16.0f - mapView_.offX()) * zm,
                            (cz * 16.0f - mapView_.offY()) * zm, 16 * zm, 16 * zm};
                SDL_RenderFillRectF(ren_, &r);
            }
        char buf[64];
        for (const UnitR* _up : front().live) { const UnitR& u = *_up;
            if (!u.alive() || !u.type) continue;
            if (!unitShownR(u)) continue;
            float sx = (u.x - mapView_.offX()) * zm;
            float rawY = (u.z - mapView_.offY()) * zm;
            float lift = terrainLift(u.x, u.z);
            float liftX = terrainLiftX(u.x, u.z) * zm;
            float liftY = rawY - lift * zm;
            // raw foot (magenta) and lifted foot (cyan, incl. sideways tilt)
            SDL_SetRenderDrawColor(ren_, 255, 0, 255, 255);
            SDL_FRect rr{sx - 2, rawY - 2, 4, 4};
            SDL_RenderFillRectF(ren_, &rr);
            SDL_SetRenderDrawColor(ren_, 0, 255, 255, 255);
            SDL_FRect lr{sx - liftX - 2, liftY - 2, 4, 4};
            SDL_RenderFillRectF(ren_, &lr);
            SDL_SetRenderDrawColor(ren_, 255, 0, 255, 200);
            SDL_RenderDrawLineF(ren_, sx, rawY, sx, liftY);
            int cx = std::clamp(int(u.x) / 16, 0, m.width - 1);
            int cz = std::clamp(int(u.z) / 16, 0, m.height - 1);
            int h = m.heights[size_t(cz) * m.width + cx];
            std::snprintf(buf, sizeof buf, "h%d L%d", h, int(lift + 0.5f));
            blockText(buf, sx + 5, liftY - 30, 1.4f, SDL_Color{255, 255, 120, 255});
            // Computed occlusion clip line (green): units are clipped above this.
            float occ = wallOcclusionY(u.x, u.z);
            if (occ < rawY) {
                SDL_SetRenderDrawColor(ren_, 40, 255, 40, 255);
                SDL_RenderDrawLineF(ren_, sx - 30, occ, sx + 30, occ);
            }
        }
    }

    double GameView::fogSeconds() {
        // Retail times RollingFog and WavyFog with the real clock (0x5ba760), not the
        // game's: the mist keeps moving while the game is paused. TA_FOG_STILL holds
        // it at t = 0 for captures that compare frames.
        static const bool still = ta::devFlag("TA_FOG_STILL");
        static const char* fixed = ta::devEnv("TA_FOG_TIME");   // captures: a chosen moment
        if (fixed) return std::atof(fixed);
        if (still || fogStill_) return 0.0;
        const uint64_t now = SDL_GetPerformanceCounter();
        if (!fogClock0_) fogClock0_ = now;
        return double(now - fogClock0_) / double(SDL_GetPerformanceFrequency());
    }

    bool GameView::syncFogTexture(SDL_Texture*& texture, ta::FogUpload& upload, uint32_t& generation) {
        const Frame& f = front();
        const int tw = f.fogW * upload.scale(), th = f.fogH * upload.scale();
        int qw = 0, qh = 0;
        if (texture && (SDL_QueryTexture(texture, nullptr, nullptr, &qw, &qh) != 0 || qw != tw || qh != th)) {
            gpuvram::destroy(texture); texture = nullptr;
        }
        if (!texture) {
            upload.clear(); generation = ~0u;
            texture = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, tw, th);
            if (!texture) return false;
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        }
        // A changed plane need not change a texel; FogUpload sends only what did.
        if (generation != f.fogGen &&
            upload.update(texture, f.fog, f.fogW, f.fogH, ta::devFlag("TA_FOG_FULL_UPLOAD")))
            generation = f.fogGen;
        return true;
    }

    // Retail's Glide fog (0x497a40 -> 0x498270; the device's capability 4, which
    // Glide reports, selects it over the vertex-alpha twin 0x497ae0). The plane's
    // cells become ARGB4444 texels two per cell each way (0x498b60), the texture
    // is bilinear-filtered (render state 0x11 = 2; only the FogDebug toggle makes
    // it point-sampled), alpha-blended and modulated by the vertex colour, and
    // drawn as a grid of quads: RollingFog colours every vertex with the side's
    // FogColor times a brightness that rolls in time and space, WavyFog moves the
    // inner vertices by up to 10 px. Retail's grid is the screen cut 6 x 6; ours is
    // anchored to the map so the waves do not slide when the camera pans.
    //
    // The plane is in PROJECTED coordinates (x, z - y/2): its rows already are
    // the rows of the tile art units are drawn on, so the overlay is flat -- no
    // per-corner terrain lift. Units draw at z - h/2 too, so no offset either.
    void GameView::drawFog() {
        if (noFog_) return;
        const Frame& f = front();
        if (f.fog.empty() || f.fogW <= 0 || f.fogH <= 0) return;
        if (!syncFogTexture(fogTex_, fogUpload_, fogTexGen_)) return;
        SDL_SetTextureScaleMode(fogTex_, ta::devFlag("TA_FOG_DEBUG") ? SDL_ScaleModeNearest : SDL_ScaleModeLinear);
        if (fogColorSide_ != side_) {   // the local side's FogColor (sidedata.tdf, default white)
            fogColorSide_ = side_;
            fogColor_ = {255, 255, 255, 255};
            if (const auto data = vfs_.tryRead("gamedata/sidedata.tdf")) try {
                const auto sides = ta::tdf::parseText(std::string(data->begin(), data->end()), "gamedata/sidedata.tdf");
                for (const auto& [key, node] : sides.children) {
                    if (ta::hpi::MountSet::key(node.valueOr("nameprefix", "")) != ta::hpi::MountSet::key(side_)) continue;
                    int r = 255, g = 255, b = 255;
                    if (std::sscanf(node.valueOr("fogcolor", "255 255 255").c_str(), "%d %d %d", &r, &g, &b) == 3)
                        fogColor_ = {Uint8(std::clamp(r, 0, 255)), Uint8(std::clamp(g, 0, 255)),
                                     Uint8(std::clamp(b, 0, 255)), 255};
                }
            } catch (const std::exception&) {}
        }
        const float zm = mapView_.zoom();
        // The plane and the bodies share retail's projection, so no offset.
        const float lift = 0.0f;
        const float ox = mapView_.offX(), oy = mapView_.offY();
        const float mapW = float(f.fogW) * 32.0f, planeH = float(f.fogH) * 32.0f;
        // The map's art rows [0, planeH] show the projected rows [-lift, planeH - lift];
        // the band above row 0 repeats row 0 (clamped texture rows, as retail's edges).
        const float pTop = -lift, pBottom = planeH - lift;
        const float step = std::max(160.0f, 80.0f / std::max(zm, 1e-3f));
        constexpr float kMargin = 24.0f;   // past the view: more than the 10 px wave
        auto lines = [&](float lo, float hi, float view0, float view1, std::vector<float>& out) {
            out.clear();
            const float a = std::max(lo, view0 - kMargin - step), b = std::min(hi, view1 + kMargin + step);
            if (a >= b) return;
            out.push_back(a);
            for (float v = (std::floor(a / step) + 1) * step; v < b; v += step)
                if (v > a) out.push_back(v);
            out.push_back(b);
        };
        static thread_local std::vector<float> xs, ps;
        lines(0.0f, mapW, ox, ox + float(winW_) / zm, xs);
        lines(pTop, pBottom, oy - lift, oy - lift + float(winH_) / zm, ps);
        if (xs.size() < 2 || ps.size() < 2) return;
        static const bool rolling = !ta::devFlag("TA_FOG_NO_ROLLING"), wavy = !ta::devFlag("TA_FOG_NO_WAVY");
        const ta::retailfog::Wave wave(fogSeconds());
        const size_t nx = xs.size(), np = ps.size();
        static thread_local std::vector<SDL_Vertex> grid;
        grid.resize(nx * np);
        for (size_t j = 0; j < np; ++j)
            for (size_t i = 0; i < nx; ++i) {
                const float X = xs[i], P = ps[j];
                float dx = 0, dy = 0;
                if (wavy) {
                    if (X > 0.0f && X < mapW) dx = wave.wave(X, P);
                    if (P > pTop && P < pBottom) dy = wave.wave(X + dx, P);
                }
                const float b = rolling ? wave.brightness(X, P) : 1.0f;
                SDL_Vertex& v = grid[j * nx + i];
                v.position = {(X + dx - ox) * zm, (P + dy + lift - oy) * zm};
                v.tex_coord = {X / mapW, std::clamp(P, 0.0f, planeH) / planeH};
                v.color = {Uint8(float(fogColor_.r) * b), Uint8(float(fogColor_.g) * b),
                           Uint8(float(fogColor_.b) * b), 255};
            }
        fogVerts_.clear();
        fogVerts_.reserve((nx - 1) * (np - 1) * 6);
        for (size_t j = 0; j + 1 < np; ++j)
            for (size_t i = 0; i + 1 < nx; ++i) {
                const SDL_Vertex &a = grid[j * nx + i], &b = grid[j * nx + i + 1],
                                 &c = grid[(j + 1) * nx + i + 1], &d = grid[(j + 1) * nx + i];
                fogVerts_.insert(fogVerts_.end(), {a, b, c, a, c, d});
            }
        fogSubmit_.draw(ren_, fogTex_, fogVerts_, !ta::devFlag("TA_FOG_SDL_SUBMIT"));
    }

    void GameView::drawGhost() {
        float zm = mapView_.zoom();
        // Resolve the elevated cell drawn under the cursor. Draw a translucent ghost
        // of the thing being placed, lifted onto the relief exactly like the finished
        // unit/building -- a plain ghost where it can go, red-washed where it can't.
        float wx, wz;
        pickWorld(mouseX_, mouseY_, wx, wz);
        bool ok = canPlacePreview(placing_, wx, wz);
        // A site blocked only by clearable doodads isn't a refusal any more -- the
        // click sends the builder to clear it first -- so don't red-wash it. The
        // notice tells the player what the click will actually do.
        if (!ok && placePreviewClearable(placing_, wx, wz))
            ok = true;
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        drawGhostAt(placing_, wx, wz, !ok);
        // Small name tag above the ghost so the player still sees what's queued.
        float cxp = (wx - mapView_.offX()) * zm - terrainLiftX(wx, wz) * zm;
        float czp = (wz - mapView_.offY()) * zm - terrainLift(wx, wz) * zm;
        float hh = float(placing_->footZ) * 8 * zm;
        float px = 1.8f;
        float tw = blockWidth(placing_->name, px);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 190);
        SDL_FRect tb{cxp - tw / 2 - 4, czp - hh - 24, tw + 8, 20};
        SDL_RenderFillRectF(ren_, &tb);
        blockText(placing_->name, cxp - tw / 2, czp - hh - 20, px, {230, 230, 200, 255});
    }

    const GameView::EffectAnim* GameView::effectFor(const std::string& animName) {
        auto it = effectAnims_.find(animName);
        if (it != effectAnims_.end())
            return it->second.frames.empty() ? nullptr : &it->second;
        EffectAnim ea;
        // Retail 4bd980 loads fx.pcx into game+17410, passed to effect banks
        // including smoke and damage flames. Feature palettes are different.
        ta::gaf::Palette effectPalette;
        try { effectPalette=ta::gaf::Palette::fromBytes(vread("palettes/fx.pcx"),"palettes/fx.pcx"); }
        catch(const std::exception&) {} // retain the decoder's missing-palette fallback
        // "file:sequence" targets a specific GAF sequence (e.g. "flames:flame large");
        // A bare name uses the file of that name and its like-named sequence.
        // Native 537550 returns null when the requested name is absent.
        std::string file = animName, seqWant = animName;
        if (auto c = animName.find(':'); c != std::string::npos) {
            file = animName.substr(0, c);
            seqWant = animName.substr(c + 1);
        }
        seqWant=ta::hpi::MountSet::key(seqWant);
        for (const std::string suf : {"_4444.taf", "_1555.taf", ".taf", ".gaf"}) {
            if (!ea.frames.empty()) break;
            try {
                std::string ap = "anims/" + file + suf;
                auto seqs = ta::gaf::load(vread(ap), effectPalette, -1, ap);
                const ta::gaf::Sequence* seq = nullptr;
                for (auto& s : seqs) {
                    if (ta::hpi::MountSet::key(s.name) == seqWant) { seq = &s; break; }
                }
                if (!seq) continue;
                ea.loop=seq->loopFlag!=0;
                for (auto& fr : seq->frames) {
                    // Blank frames still occupy an authored frame index and
                    // duration. Dropping them shifts every subsequent frame,
                    // including indices supplied by the simulation storm clock.
                    const bool blank=fr.width==0 || fr.height==0;
                    SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                       SDL_TEXTUREACCESS_STATIC,
                                                       blank ? 1 : fr.width, blank ? 1 : fr.height);
                    // Colour-bled like feature art (premulUpload): only RGB under alpha 0
                    // changes, which neither BLEND nor ADD ever shows when sampled
                    // nearest, while Everything's linear sampling no longer mixes the
                    // GAF black into every sprite edge.
                    if (t) {
                        premulUpload(t, blank ? std::vector<uint8_t>(4, 0) : fr.rgba, blank ? 1 : fr.width);
                        SDL_SetTextureScaleMode(t, worldScaleMode());
                    }
                    // Glide adds only format-4 frames without flag FF. Indexed
                    // and 1555 sprites use ordinary coverage in the default
                    // draw mode; SDL alpha preserves their transparent pixels.
                    SDL_SetTextureBlendMode(t,fr.encoding==4 && fr.blendFlag!=255 ?
                        SDL_BLENDMODE_ADD : SDL_BLENDMODE_BLEND);
                    ea.durations.push_back(fr.retailDelayTicks);
                    ea.frames.push_back({t, fr.width, fr.height, fr.xoff, fr.yoff,
                        int(fr.retailDelayTicks),fr.encoding});
                }
            } catch (const std::exception&) {}
        }
        auto& stored = effectAnims_[animName];
        stored = std::move(ea);
        return stored.frames.empty() ? nullptr : &stored;
    }

    void GameView::updateEffects(float dt) {
        const auto& terrain=mapView_.map();
#ifndef NDEBUG
        size_t bloodBefore=0;
        if(ta::devEnv("TA_BLOOD_TEST"))
            for(const auto& debris:detachedPieces_)bloodBefore+=debris.blood.size();
#endif
        std::erase_if(detachedPieces_,[&](DetachedPiece& debris) {
            while(int32_t(front().gameTick-debris.tick)>0) {
                ++debris.tick;
                const auto result=debris.motion.tick(uint8_t(terrain.seaLevel),8155,debrisLavaWorld_,debrisNoSeaTrigger_,
                    [&](const auto& position) {
                        return terrain.heights.empty() ? -1 : ta::retailDebrisTerrainHeight(
                            position[0],position[2],terrain.width,terrain.height,[&](int x,int z) {
                                return terrain.heights[size_t(z)*size_t(terrain.width)+size_t(x)];
                            });
                    });
                if(!result.alive) {
                    loadExplosionClasses();
                    if(result.light>=0)
                        explosionGlows_.push_back({0,0,0,debris.tick,unsigned(result.light),debris.motion.position});
                    if(result.effect>=0 && size_t(result.effect)<explosionClassOrder_.size()) {
                        const auto& p=debris.motion.position;
                        if(spawnEffect(explosionClassOrder_[size_t(result.effect)],
                                       float(p[0])/65536.f,float(p[2])/65536.f)) {
                            effects_.back().worldPosition=p;effects_.back().started=debris.tick;
                        }
                    }
                    return true;
                }
                std::erase_if(debris.blood,[&](auto& particle) {
                    const auto contact=particle.tick(8155,uint8_t(terrain.seaLevel),[&](const auto& p) {
                        return terrain.heights.empty() ? -1 : ta::sim::retailTerrainHeight(
                            p[0],p[2],terrain.width,terrain.height,[&](int x,int z) {
                                return terrain.heights[size_t(z)*size_t(terrain.width)+size_t(x)];
                            });
                    });
                    if(contact.stain) {
                        if(bloodStains_.size()==2048)bloodStains_.pop_front();
                        bloodStains_.push_back(particle);
                    }
                    return !contact.alive;
                });
            }
            return false;
        });
#ifndef NDEBUG
        if(bloodBefore && ta::devEnv("TA_BLOOD_TEST")) {
            size_t bloodAfter=0;
            for(const auto& debris:detachedPieces_)bloodAfter+=debris.blood.size();
            if(!bloodAfter) {
                if(bloodStains_.size()!=100)
                    throw std::runtime_error("live blood fixture lost particles before ground contact");
                std::fprintf(stderr,"PASS: live blood fixture reached 100 persistent stains, no airborne particles\n");
            }
        }
#endif
        for (auto& b : beams_) b.age += dt;
        std::erase_if(beams_, [](const BeamFx& b) { return b.age > b.life; });
        std::erase_if(explosionGlows_,[this](const ExplosionGlow& glow) {
            return front().gameTick-glow.started>uint32_t(ta::retailGlowEnvelope(glow.kind).duration);
        });
        for (auto& e : effects_) e.age += dt;
        std::erase_if(effects_, [this](const EffectInst& e) {
            if (!e.anim || e.anim->frames.empty()) return true;
            if(e.authoredTiming) {
                return !ta::retailEffectFrame(e.anim->durations,false,front().gameTick-e.started);
            }
            float local = e.age - e.delay;
            return local >= effLoopLen(e) * float(std::max(e.loops, 1));
        });
    }

    void GameView::spawnRing(const std::string& anim, float x, float z, float delay,
                   float dur, int sprites, float maxR) {
        const EffectAnim* ea = effectFor(anim);
        if (ea) rings_.push_back({ea, x, z, 0.0f, delay, dur, maxR,
                                  std::clamp(sprites, 6, 48)});
    }

    void GameView::updateRings(float dt) {
        for (auto& r : rings_) r.age += dt;
        std::erase_if(rings_, [](const RingFx& r) { return r.age - r.delay >= r.dur; });
    }

    void GameView::buildDetachedShadow(const DetachedPiece& debris, float zoom) {
        detachedShadowVerts_.clear();
        detachedMaskedShadows_.clear();
        if (!shadowsOnFrame_) return;

        Anim snapshot;
        snapshot.pieceNames = &debris.names;
        snapshot.capturedPose = debris.poses;
        constexpr float radians = 6.28318530717959f / 65536.0f;
        noteNeed(Retained::ModelTris, tris_);
        tris_.clear();
        // A detached piece is a real model root, not the ground-reference root of
        // a unit model. Retail sends it through the same projected-model shadow
        // path as a live unit (4ee700 -> 4ec720); keep the root visible here.
        collect(tris_, nullptr, debris.model,
                modelBodyTransform(debris.motion.rotation[0], debris.motion.rotation[2]),
                &snapshot, -float(debris.motion.rotation[1]) * radians,
                debris.player, false, false, true);

        const auto& position = debris.motion.position;
        const int x = position[0] >> 16;
        const int y = position[1] >> 16;
        const int z = position[2] >> 16;
        const float altitude = std::max(0.0f, float(y) - rawHeight(float(x), float(z)));
        const float zm = zoom;
        // The body anchor is z - y/2. Retail's shadow projection uses z - y/4,
        // so relative to that body anchor the detached shadow moves +y/4 in both
        // screen axes, exactly like buildUnitShadow's altitude offset.
        const float sx = (float(x) - mapView_.offX()) * zm + kShadowLX * altitude * zm;
        const float sy = (float(z - (y >> 1)) - mapView_.offY()) * zm +
                         (kProjY - kShadowLZ) * altitude * zm;
        detachedShadowVerts_.reserve(tris_.size() * 3);
        detachedMaskedShadows_.reserve(tris_.size());
        for (const Tri& triangle : tris_) {
            if (triangle.tex) {
                Tri masked = triangle;
                for (auto& vertex : masked.v)
                    vertex.position = {sx + vertex.position.x * zm,
                                       sy + vertex.position.y * zm};
                detachedMaskedShadows_.push_back(masked);
            } else {
                for (const auto& vertex : triangle.v)
                    detachedShadowVerts_.push_back({sx + vertex.position.x * zm,
                                                    sy + vertex.position.y * zm});
            }
        }
    }

    void GameView::drawDetachedShadow() {
        static const SDL_Color color{kShadowLevel, kShadowLevel, kShadowLevel, 255};
        static const float uv[2] = {0, 0};
        if (!detachedShadowVerts_.empty())
            SDL_RenderGeometryRaw(ren_, nullptr, &detachedShadowVerts_[0].x, sizeof(SDL_FPoint),
                                  &color, 0, uv, 0, int(detachedShadowVerts_.size()),
                                  nullptr, 0, 0);
        for (const auto& triangle : detachedMaskedShadows_)
            SDL_RenderGeometry(ren_, triangle.tex, triangle.v, 3, nullptr, 0);
    }

    // ---- world effects in the painter list --------------------------------------
    // Retail draws every shot and effect after all the ground units and features
    // (docs/retail-engine.md, "The world draw order"), so a spear thrown behind a
    // tree flew over its crown. At the owner's request (2026-11) each shot and
    // effect is a painter item of its own instead, keyed by the ground z under it,
    // so it sorts with the units and the tall features (PaintItem::fx). The late
    // pass after the ground replay is kept for the items left out of the painter
    // list (far outside the view) and draws them in the old family order.
    int GameView::fxOwnerLayer(int owner) const {
        namespace hu = ta::hiddenunits;
        const UnitR* u = frameUnitP(owner);
        if (!u || !u->type || !u->type->canFly) return hu::kLayerGround;
        // The painter list's own airborne test (unitCandidate): aloft unless landed.
        const auto it = anims_.find(owner);
        return it == anims_.end() || it->second.altitude > 1.0f ? hu::kLayerAir : hu::kLayerGround;
    }

    void GameView::collectWorldFx(int mvw, int winH, bool painter) {
        namespace hu = ta::hiddenunits;
        fxItems_.clear();++beamDrawFrame_;
#ifndef NDEBUG
        debugGlowDrawCount_=0;
        debugSmokeDrawCount_=0;debugDamageFlameDrawCount_=0;debugPointDrawCount_=0;
        debugFxReplayed_=0;debugFxLate_=0;
        if(ta::devEnv("TA_GLOW_TEST") && explosionGlows_.empty()) {
            const float cx=mapView_.map().blocksX*16.f,cz=mapView_.map().blocksY*16.f;
            for(unsigned kind=0;kind<3;++kind)
                explosionGlows_.push_back({cx+(float(kind)-1)*100,cz,0,front().gameTick,kind});
        }
#endif
        fxSpanMs_ = 0;
        if (!worldFxFrame()) return;
        (void)heightAbove(0,0);   // the lazy height datum, before any item draws
        const float zm=mapView_.zoom(),offX=mapView_.offX(),offY=mapView_.offY();
        // Depth order is only worth its batch breaks near the view. An item whose
        // ground point lies further outside it than this is not queued; the late
        // pass draws it, so nothing is dropped. Generous on purpose: sprites and
        // shot models scale with the zoom, and altitude lifts a shot from below.
        const float margin=256.0f+512.0f*zm,below=margin+512.0f*zm;
        const auto add=[&](FxKind kind,const void* p,uint32_t a,float x,float z,int layer=hu::kLayerGround) {
            const float sx=(x-offX)*zm,sy=(z-offY)*zm;
            const bool queued=painter && sx>=-margin && sx<=float(mvw)+margin &&
                              sy>=-margin && sy<=float(winH)+below;
            fxItems_.push_back({p,a,0,kind,queued});
            if (queued) paintItems_.push_back({z+kFxDepthBias,nullptr,nullptr,layer,int32_t(fxItems_.size()-1)});
        };
        const auto whole=[](int32_t v) {return float(int(std::bit_cast<int16_t>(uint16_t(uint32_t(v)>>16))));};
        const Frame& frame=front();
        // Hitscan bolts: a model or sprite where its head is; a drawn bolt at its
        // southern end, so it is over the unit at either end of it.
        for(const auto& b:beams_) {
            const float t=std::clamp(b.age/std::max(b.life,1e-3f),0.0f,1.0f);
            if(t>=1.0f || (!cellVisibleR(b.x2,b.z2) && !noFog_))continue;
            const float hx=b.x1+(b.x2-b.x1)*t,hz=b.z1+(b.z2-b.z1)*t;
            if(b.model.empty() && b.sprite.empty() && b.z1>hz) add(FxKind::Beam,&b,0,b.x1,b.z1);
            else add(FxKind::Beam,&b,0,hx,hz);
        }
        // Projectiles, at the ground point under them.
        for(const auto& p:frame.projectiles) {
            if(p.taLineOfSight && p.wsrc && (p.wsrc->renderType==0 || p.wsrc->renderType==7)) {
                // A beam or lightning line (0x49be60): shown when its head's
                // point is (0x49bee8), sorted at its southern end.
                const float x=whole(p.position[0]),y=whole(p.position[1]),z=whole(p.position[2]);
                if(!projectedVisibleR(x,z-float(int(y)>>1)))continue;
                const float tx=whole(p.taTail[0]),tz=whole(p.taTail[2]);
                if(tz>z) add(FxKind::Projectile,&p,0,tx,tz);
                else add(FxKind::Projectile,&p,0,x,z);
                continue;
            }
            const bool native3d=p.ballistic3d || p.guided3d || p.taLineOfSight;
            const float x=native3d ? ta::sim::Fixed::raw(p.position[0]).toFloat() : p.x.toFloat();
            const float z=native3d ? ta::sim::Fixed::raw(p.position[2]).toFloat() : p.z.toFloat();
            if(native3d ? !projectedVisibleR(x,z-float(int(whole(p.position[1]))>>1)) : !cellVisibleR(x,z))continue;
            add(FxKind::Projectile,&p,0,x,z);
        }
        // Impact sparks, smoke puffs and blood spray (the screen cull the particle draw always had).
        for(const auto& p:particles_) {
            const float sx=(p.x-offX)*zm,sy=(p.z-offY)*zm-p.alt*zm;
            if(sx<-40 || sx>float(winW_)+40 || sy<-80 || sy>float(winH_)+40)continue;
            if(!cellVisibleR(p.x,p.z))continue;
            add(FxKind::Particle,&p,0,p.x,p.z);
        }
        // Shockwave rings, a sprite at a time, each at its own point of the circle.
        for(const auto& r:rings_) {
            const float local=r.age-r.delay;
            if(local<0 || !r.anim || r.anim->frames.empty() || !cellVisibleR(r.x,r.z))continue;
            const float radius=r.maxR*std::clamp(local/std::max(r.dur,1e-3f),0.0f,1.0f);
            for(int k=0;k<r.sprites;++k) {
                const float a=6.2831853f*float(k)/float(r.sprites);
                add(FxKind::Ring,&r,uint32_t(k),r.x+std::cos(a)*radius,r.z+std::sin(a)*radius);
            }
        }
        // Blood: a stain lies on the ground, so it is flat scenery (under every
        // unit, kLayerLowFeature); a drop still in flight sorts by depth.
        for(const auto& stain:bloodStains_)
            if(noFog_ || effectVisibleR(stain.position))
                add(FxKind::BloodStain,&stain,0,float(stain.position[0]>>16),float(stain.position[2]>>16),hu::kLayerLowFeature);
        for(const auto& debris:detachedPieces_)
            for(const auto& drop:debris.blood)
                if(noFog_ || effectVisibleR(drop.position))
                    add(FxKind::DebrisBlood,&drop,0,float(drop.position[0]>>16),float(drop.position[2]>>16));
        // Detached debris (its model and its shadow), at the ground under it.
        for(const auto& debris:detachedPieces_)
            if(noFog_ || effectVisibleR(debris.motion.position))
                add(FxKind::Debris,&debris,0,float(debris.motion.position[0]>>16),float(debris.motion.position[2]>>16));
        for(const auto& glow:explosionGlows_) {
            if(!noFog_ && (glow.worldPosition ? !effectVisibleR(*glow.worldPosition) :
                                               !cellVisibleR(glow.x,glow.z)))continue;
            if(frame.gameTick-glow.started>uint32_t(ta::retailGlowEnvelope(glow.kind).duration))continue;
            if(glow.worldPosition) add(FxKind::Glow,&glow,0,float((*glow.worldPosition)[0]>>16),float((*glow.worldPosition)[2]>>16));
            else add(FxKind::Glow,&glow,0,glow.x,glow.z);
        }
        // A unit's own effects go where the unit is: never under it (its z, when
        // larger, keys them), and into the air layer with an airborne flyer.
        for(const auto& e:effects_) {
            if(!e.anim || e.anim->frames.empty() || e.age-e.delay<0)continue;
            if(e.worldPosition ? !effectVisibleR(*e.worldPosition) : (!noFog_ && !cellVisibleR(e.x,e.z)))continue;
            if(e.worldPosition) add(FxKind::Effect,&e,0,float((*e.worldPosition)[0]>>16),float((*e.worldPosition)[2]>>16));
            else add(FxKind::Effect,&e,0,e.x,e.z);
        }
        const float viewW=float(mapViewW(winW_)),viewH=float(winH_);
        const auto admitted=[&](const std::array<int32_t,3>& position) {
            const int x=position[0]>>16,y=position[1]>>16,z=position[2]>>16;
            return ta::retailViewportCenterAdmitted((float(x)-offX)*zm,(float(z-(y>>1))-offY)*zm,viewW,viewH);
        };
        // Native cargo's hidden flag skips the owner's entire drawable, including
        // attached effect lists (their lifetime state is kept).
        const auto attached=[&](int owner,const std::array<int32_t,3>& position,FxKind kind,const void* p) {
            const auto* u=frameUnitP(owner);
            const float z=float(position[2]>>16);
            add(kind,p,uint32_t(owner),float(position[0]>>16),u ? std::max(z,u->z) : z,fxOwnerLayer(owner));
        };
        for(const auto& [owner,particles]:pointParticles_) {
            if(const auto* unit=frameUnitP(owner);unit && unit->embarked())continue;
            for(const auto& particle:particles)
                if(admitted(particle.position))attached(owner,particle.position,FxKind::Point,&particle);
        }
        for(const auto& [owner,sprites]:smokeSprites_) {
            if(const auto* unit=frameUnitP(owner);unit && unit->embarked())continue;
            for(const auto& sprite:sprites)
                if(sprite.particle.frame<sprite.art->frames.size() && admitted(sprite.particle.position))
                    attached(owner,sprite.particle.position,FxKind::Smoke,&sprite);
        }
        for(const auto& [owner,sprites]:damageFlames_) {
            if(const auto* unit=frameUnitP(owner);unit && unit->embarked())continue;
            for(const auto& sprite:sprites)
                if(sprite.clock.active && sprite.clock.frame<sprite.art->frames.size() && admitted(sprite.position))
                    attached(owner,sprite.position,FxKind::DamageFlame,&sprite);
        }
        const float kLinger = 0.8f;   // seconds after the last emit to keep drawing
        for (const auto& [id, a] : anims_) {
            const bool fire = a.fireFx && a.fireT < kLinger;
            const bool smk = a.smokeFx && a.smokeT < kLinger;
            if (!fire && !smk) continue;
            const auto* u = frameUnitP(id);
            if (!u || !u->type || u->embarked() || !unitShownR(*u)) continue;
            add(FxKind::UnitFx,&a,uint32_t(id),u->x,u->z,fxOwnerLayer(id));
        }
        renderStats_.add(ta::render::Counter::FxItems, fxItems_.size());
        if (painter)
            renderStats_.add(ta::render::Counter::FxQueued, size_t(std::count_if(fxItems_.begin(),fxItems_.end(),
                [](const FxItem& item) {return item.queued;})));
    }

    void GameView::drawFxSpan(const uint32_t* order, size_t count) {
        const double t0 = double(SDL_GetPerformanceCounter());
        // Every item starts and ends on the BLEND draw mode; consecutive particles
        // share one geometry call (battles keep hundreds live).
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        partBatch_.clear();
        const auto flush = [&] {
            if (partBatch_.empty()) return;
            SDL_RenderGeometry(ren_, nullptr, partBatch_.data(), int(partBatch_.size()), nullptr, 0);
            partBatch_.clear();
        };
        for (size_t i = 0; i < count; ++i) {
            const FxItem& item = fxItems_[order[i]];
            if (item.kind != FxKind::Particle) flush();
            drawFxItem(item);
        }
        flush();
        fxSpanMs_ += (double(SDL_GetPerformanceCounter()) - t0) / (double(SDL_GetPerformanceFrequency()) / 1000.0);
    }

    void GameView::drawLateFx() {
        fxLate_.clear();
        for (uint32_t i = 0; i < fxItems_.size(); ++i) if (!fxItems_[i].queued) fxLate_.push_back(i);
#ifndef NDEBUG
        debugFxLate_ += fxLate_.size();
#endif
        if (!fxLate_.empty()) drawFxSpan(fxLate_.data(), fxLate_.size());
    }

    void GameView::drawFxItem(const FxItem& item) {
        const float zm = mapView_.zoom();
        switch (item.kind) {
        case FxKind::Beam:
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_ADD);
            drawFxBeam(*static_cast<const BeamFx*>(item.p));
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            return;
        case FxKind::Projectile:
            drawFxProjectile(*static_cast<const ta::sim::Projectile*>(item.p));
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            return;
        case FxKind::Particle: {   // into partBatch_, which drawFxSpan draws
            const auto& p = *static_cast<const Particle*>(item.p);
            float sx = (p.x - mapView_.offX()) * zm;
            float sy = (p.z - mapView_.offY()) * zm - p.alt * zm;
            float t = std::clamp(p.life / std::max(p.maxLife, 1e-3f), 0.0f, 1.0f);
            sx -= terrainLiftX(p.x, p.z) * zm;
            sy -= terrainLift(p.x, p.z) * zm;
            float r = p.size * zm * (p.kind == 1 ? (1.4f - t) : p.kind == 2 ? 0.5f : t);
            Uint8 a = p.kind == 2 ? 255 : Uint8(std::clamp(t * 255.0f, 0.0f, 255.0f));
            if (p.kind == 2) r = std::max(r, 0.5f);   // a nano dot stays at least a pixel
            pushQuad(partBatch_, sx - r, sy - r, 2 * r, 2 * r, SDL_Color{p.r, p.g, p.b, a});
            return;
        }
        case FxKind::Ring: {
            const auto& r = *static_cast<const RingFx*>(item.p);
            float local = r.age - r.delay;
            float t = std::clamp(local / std::max(r.dur, 1e-3f), 0.0f, 1.0f);
            float radius = r.maxR * t;
            int nf = int(r.anim->frames.size());
            const EFrame& f = r.anim->frames[size_t(std::clamp(int(t * nf), 0, nf - 1))];
            float a = 6.2831853f * float(item.a) / float(r.sprites);
            float wx = r.x + std::cos(a) * radius, wz = r.z + std::sin(a) * radius;
            float sx = (wx - mapView_.offX()) * zm - f.ax * zm - terrainLiftX(r.x, r.z) * zm;
            float sy = (wz - mapView_.offY()) * zm - f.ay * zm - terrainLift(r.x, r.z) * zm;
            SDL_FRect dst{sx, sy, f.w * zm, f.h * zm};
            SDL_RenderCopyF(ren_, f.tex, nullptr, &dst);
            return;
        }
        case FxKind::BloodStain:
        case FxKind::DebrisBlood: {
            const auto& particle = *static_cast<const ta::RetailBloodParticle*>(item.p);
            const int x=particle.position[0]>>16,y=particle.position[1]>>16,z=particle.position[2]>>16;
            const float sx=(float(x)-mapView_.offX())*zm;
            const float sy=(float(z-(y>>1))-mapView_.offY())*zm;
            SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(ren_,uint8_t(particle.color>>16),uint8_t(particle.color>>8),
                uint8_t(particle.color),uint8_t(particle.color>>24));
            SDL_RenderDrawPointF(ren_,sx,sy);
            SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_BLEND);
            return;
        }
        case FxKind::Debris: {
            const auto& debris = *static_cast<const DetachedPiece*>(item.p);
            SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_NONE);
            buildDetachedShadow(debris, zm);
            if (!detachedShadowVerts_.empty() || !detachedMaskedShadows_.empty()) {
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_MOD);
                drawDetachedShadow();
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
            }
            Anim snapshot;snapshot.pieceNames=&debris.names;snapshot.capturedPose=debris.poses;
            noteNeed(Retained::ModelTris, tris_);
            tris_.clear();
            constexpr float radians=6.28318530717959f/65536.f;
            collect(tris_,atlasFor(playerColorSlot(debris.player)),debris.model,
                modelBodyTransform(debris.motion.rotation[0],debris.motion.rotation[2]),
                &snapshot,-float(debris.motion.rotation[1])*radians,debris.player,false,false);
            std::stable_sort(tris_.begin(),tris_.end(),
                [](const Tri& a,const Tri& b) {return a.depth>b.depth;});
            const auto& position=debris.motion.position;
            const int x=position[0]>>16,y=position[1]>>16,z=position[2]>>16;
            const float sx=(float(x)-mapView_.offX())*zm;
            const float sy=(float(z-(y>>1))-mapView_.offY())*zm;
            for(auto& triangle:tris_) {
                for(auto& vertex:triangle.v) {
                    vertex.position.x=vertex.position.x*zm+sx;
                    vertex.position.y=vertex.position.y*zm+sy;
                }
            }
            drawModelTriangles();
            SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_BLEND);
            return;
        }
        case FxKind::Glow: {
            const auto& glow = *static_cast<const ExplosionGlow*>(item.p);
            const auto envelope=ta::retailGlowEnvelope(glow.kind);
            const uint32_t elapsed=front().gameTick-glow.started;
            const auto radii=ta::retailGlowRadii(int32_t(elapsed),envelope.duration,envelope.begin,envelope.end);
            float x=(glow.x-mapView_.offX()-terrainLiftX(glow.x,glow.z))*zm;
            float y=(glow.z-mapView_.offY()-terrainLift(glow.x,glow.z)-glow.alt)*zm;
            if(glow.worldPosition) {
                const auto& p=*glow.worldPosition;
                x=(float(p[0]>>16)-mapView_.offX())*zm;
                y=(float((p[2]>>16)-((p[1]>>16)>>1))-mapView_.offY())*zm;
            }
            const auto fan=ta::retailGlowMesh(x,y,float(radii[0])*zm,float(radii[1])*zm,196);
            std::array<SDL_Vertex,36> triangles;
            for(unsigned i=0;i<12;++i)for(unsigned j=0;j<3;++j) {
                const auto& vertex=fan[j==0 ? 0:i+j];
                triangles[i*3+j]={{vertex.x,vertex.y},{255,255,255,uint8_t(vertex.color>>24)},{0,0}};
            }
            SDL_RenderGeometry(ren_,nullptr,triangles.data(),int(triangles.size()),nullptr,0);
#ifndef NDEBUG
            ++debugGlowDrawCount_;
#endif
            return;
        }
        case FxKind::Effect: {
            const auto& e = *static_cast<const EffectInst*>(item.p);
            const float local = e.age - e.delay;
            const int nf = int(e.anim->frames.size());
            int fi=0;
            if(e.authoredTiming) {
                const auto frame=ta::retailEffectFrame(e.anim->durations,false,front().gameTick-e.started);
                if(!frame || *frame>=e.anim->frames.size())return;
                fi=int(*frame);
            } else {
                const float per=effLoopLen(e);
                const float within=local-std::floor(local/per)*per;
                fi=std::clamp(int(within/per*float(nf)),0,nf-1);
            }
            const EFrame& f = e.anim->frames[size_t(fi)];
            float sx = (e.x - mapView_.offX()) * zm - f.ax * zm - terrainLiftX(e.x, e.z) * zm;
            float sy = (e.z - mapView_.offY()) * zm - f.ay * zm - terrainLift(e.x, e.z) * zm
                       - e.alt * zm;
            if(e.worldPosition) {
                const auto& position=*e.worldPosition;
                const int x=position[0]>>16,y=position[1]>>16,z=position[2]>>16;
                sx=(float(x)-mapView_.offX()-f.ax)*zm;
                sy=(float(z-(y>>1))-mapView_.offY()-f.ay)*zm;
            }
            SDL_FRect dst{sx, sy, f.w * zm, f.h * zm};
            SDL_RenderCopyF(ren_, f.tex, nullptr, &dst);
            return;
        }
        case FxKind::Point: {
            const auto& particle = *static_cast<const ta::RetailPointParticle*>(item.p);
            const int x=particle.position[0]>>16,y=particle.position[1]>>16,z=particle.position[2]>>16;
            const float sx=(float(x)-mapView_.offX())*zm;
            const float sy=(float(z-(y>>1))-mapView_.offY())*zm;
            const auto color=particle.color();
            SDL_SetRenderDrawColor(ren_,color[0],color[1],color[2],color[3]);
            SDL_RenderDrawPointF(ren_,sx,sy);
#ifndef NDEBUG
            ++debugPointDrawCount_;
#endif
            return;
        }
        case FxKind::Smoke:
        case FxKind::DamageFlame: {
            // Native smoke and damage flames: format-4 frames opaque, others half.
            const auto blit = [&](const EFrame& f, const std::array<int32_t,3>& position) {
                const int x=position[0]>>16,y=position[1]>>16,z=position[2]>>16;
                const float sx=(float(x)-mapView_.offX())*zm;
                const float sy=(float(z-(y>>1))-mapView_.offY())*zm;
                SDL_FRect dst{sx-f.ax*zm,sy-f.ay*zm,f.w*zm,f.h*zm};
                SDL_SetTextureAlphaMod(f.tex,f.encoding==4 ? 255 : 128);
                SDL_RenderCopyF(ren_,f.tex,nullptr,&dst);
                SDL_SetTextureAlphaMod(f.tex,255);
            };
            if (item.kind == FxKind::Smoke) {
                const auto& sprite = *static_cast<const SmokeSprite*>(item.p);
                blit(sprite.art->frames[sprite.particle.frame], sprite.particle.position);
#ifndef NDEBUG
                ++debugSmokeDrawCount_;
#endif
            } else {
                const auto& flame = *static_cast<const DamageFlameSprite*>(item.p);
                blit(flame.art->frames[flame.clock.frame], flame.position);
#ifndef NDEBUG
                ++debugDamageFlameDrawCount_;
#endif
            }
            return;
        }
        case FxKind::UnitFx: {
            // Persistent per-unit ambient fire/smoke, cycled off the continuous
            // animClock so it flows smoothly.
            const int id = int(item.a);
            const auto& a = *static_cast<const Anim*>(item.p);
            const auto* u = frameUnitP(id);
            if (!u) return;
            const float kLinger = 0.8f;
            auto draw = [&](const EffectAnim* ea, int piece, float fps) {
                float x, z, lift;
                scriptEffectOrigin(*u, a, piece, x, z, lift);
                int nf = int(ea->frames.size());
                int fi = int(animClock_ * fps + float(id) * 0.37f) % nf;
                if (fi < 0) fi += nf;
                const EFrame& f = ea->frames[size_t(fi)];
                float sx = (x - mapView_.offX()) * zm - f.ax * zm - terrainLiftX(u->x, u->z) * zm;
                float sy = (z - mapView_.offY()) * zm - f.ay * zm - terrainLift(u->x, u->z) * zm
                           - lift * zm;
                SDL_FRect dst{sx, sy, f.w * zm, f.h * zm};
                SDL_RenderCopyF(ren_, f.tex, nullptr, &dst);
            };
            if (a.smokeFx && a.smokeT < kLinger) draw(a.smokeFx, a.smokePiece, 12.0f);
            if (a.fireFx && a.fireT < kLinger) draw(a.fireFx, a.firePiece, 18.0f);   // flame over its smoke
            return;
        }
        }
    }


    // Hitscan flashes. A Line-of-Sight weapon applies its damage instantly and
    // spawns no projectile, so this bolt IS the shot: without it the King's
    // lightning weapons all fire invisibly.
    // Lightning zig-zags; other beams draw straight. Called on the ADD draw mode.
    void GameView::drawFxBeam(const BeamFx& b) {
        const float zm = mapView_.zoom();
        // The bolt is drawn muzzle -> travelling head, so a long shot visibly
        // reaches out rather than appearing whole. Retail's colour ramp is flat,
        // so hold full strength and fade only over the last quarter -- an even
        // fade over a one-second bolt reads as a fizzle rather than a strike.
        float t = std::clamp(b.age / std::max(b.life, 1e-3f), 0.0f, 1.0f);
        if (t >= 1.0f) return;
        if (!cellVisibleR(b.x2, b.z2) && !noFog_) return;
        if (!b.model.empty()) {
            // Line-of-sight describes hit delivery, not appearance. Crossbow
            // bolts and harpoons use the same model path as travelling shots.
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            ta::sim::Projectile position;
            const bool captured=b.fromPosition && b.position;
            if(captured)for(size_t axis=0;axis<3;++axis)
                position.position[axis]=int32_t(double((*b.fromPosition)[axis])+
                    (double((*b.position)[axis])-double((*b.fromPosition)[axis]))*t);
            drawShotModel(b.model,b.player,b.x1+(b.x2-b.x1)*t,b.z1+(b.z2-b.z1)*t,
                          (8+b.alt1+(b.alt2-b.alt1)*t)*zm,
                          3.14159265358979323846f-std::atan2(b.x2-b.x1,b.z2-b.z1),
                          captured ? &position : nullptr);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_ADD);
            return;
        }
        if (!b.sprite.empty()) {
            // Status LOS classes share retail's ordinary projectile draw
            // routine (52c8c0): weaponart is a travelling sprite, not a beam.
            if (const auto* art = effectFor(b.sprite)) {
                const auto index = ta::retailEffectFrame(art->durations, art->loop,
                                                         uint32_t(b.age * 30.0f));
                if (index && *index < art->frames.size()) {
                    const auto& fr = art->frames[*index];
                    const float x = b.x1 + (b.x2 - b.x1) * t;
                    const float z = b.z1 + (b.z2 - b.z1) * t;
                    const float alt = b.alt1 + (b.alt2 - b.alt1) * t;
                    const float sx = (x - mapView_.offX() - terrainLiftX(x, z)) * zm;
                    float sy = (z - mapView_.offY() - terrainLift(x, z) - 12 - alt) * zm;
                    if(b.fromPosition && b.position) {
                        const float y1=float((*b.fromPosition)[1]>>16);
                        const float y2=float((*b.position)[1]>>16);
                        sy=(z-(y1+(y2-y1)*t)*0.5f-mapView_.offY())*zm;
                    }
                    const auto origin = ta::retailEffectSpriteOrigin(sx, sy, fr.ax, fr.ay, zm);
                    SDL_FRect dst{origin.x, origin.y, fr.w * zm, fr.h * zm};
                    SDL_SetTextureAlphaMod(fr.tex, 255);
                    SDL_RenderCopyF(ren_, fr.tex, nullptr, &dst);
                }
            }
            return;
        }
        float f = 1.0f - std::max(0.0f, (t - 0.75f) / 0.25f);
        auto sx = [&](float x, float z) {
            return (x - mapView_.offX()) * zm - terrainLiftX(x, z) * zm;
        };
        auto sy = [&](float x, float z, float alt) {
            return (z - mapView_.offY()) * zm - 12 * zm - terrainLift(x, z) * zm - alt * zm;
        };
        float ax = sx(b.x1, b.z1), ay = sy(b.x1, b.z1, b.alt1);
        float fullx = sx(b.x2, b.z2), fully = sy(b.x2, b.z2, b.alt2);
        float bx = ax + (fullx - ax) * t, by = ay + (fully - ay) * t;
        // Three passes -- outer halo, middle body, hot core -- so a weapon's
        // own colours read the way retail's layered bolt does.
        struct Pass { const uint8_t* c; float wob; int reps; };
        const Pass passes[3] = {{b.outer, 3.4f, 3}, {b.middle, 1.8f, 2}, {b.inner, 0.7f, 1}};
        // Jag count follows the DRAWN length, so a stub isn't over-zigzagged
        // while it is still reaching out (retail sizes its jag vertices from
        // screen length the same way).
        float blen = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
        int segs = b.lightning ? std::clamp(int(blen / 24.0f), 2, 10) : 1;
        for (const Pass& ps : passes) {
            SDL_SetRenderDrawColor(ren_, ps.c[0], ps.c[1], ps.c[2], Uint8(230 * f));
            for (int rep = 0; rep < ps.reps; ++rep) {
                float px = ax, py = ay;
                for (int s = 1; s <= segs; ++s) {
                    float t2 = float(s) / float(segs);
                    float nx = ax + (bx - ax) * t2, ny = ay + (by - ay) * t2;
                    if (b.lightning && s < segs) {
                        int j = (s * 7919 + rep * 131 + int(b.age * 2000)) % 11 - 5;
                        float dx = bx - ax, dy = by - ay;
                        float dl = std::max(std::sqrt(dx * dx + dy * dy), 1e-3f);
                        nx += -dy / dl * float(j) * ps.wob;
                        ny += dx / dl * float(j) * ps.wob;
                    }
                    SDL_RenderDrawLineF(ren_, px, py, nx, ny);
                    px = nx; py = ny;
                }
            }
        }
    }

    // TotalA.exe's line-drawn shots (0x49be60; client/tabeamdraw.h): a beam
    // (rendertype 0) or lightning (7) between the head and the tail, opaque
    // palette colours through the game's colour map. Each 1 px line of the
    // 640x480 game becomes a zoom-wide band along its major axis.
    void GameView::drawTaLineShot(const ta::sim::Projectile& p) {
        const auto& w=*p.wsrc;
        const auto map=[&](uint8_t index){return taGuiColourMap_[index];};
        std::vector<ta::TaShotLine> lines;
        if(w.renderType==0) lines=ta::taBeamLines(p.position,p.taTail,map(w.color),w.color2,map(w.color2));
        else {
            ta::TaCrtRand rand;
            rand.state=beamDrawFrame_*2654435761u ^ uint32_t(p.fromId)*40503u ^ p.taLaunch;
            lines=ta::taLightningLines(p.position,p.taTail,map(w.color),rand);
        }
        if(lines.empty())return;
        const float zm=mapView_.zoom(),ox=mapView_.offX(),oy=mapView_.offY();
        std::vector<SDL_Vertex> vertices;vertices.reserve(lines.size()*6);
        for(const auto& line:lines) {
            const uint32_t packed=nanoGamePalette_[line.colour];
            const SDL_Color c{Uint8(packed>>24),Uint8(packed>>16),Uint8(packed>>8),255};
            const bool xMajor=std::abs(line.x2-line.x1)>=std::abs(line.y2-line.y1);
            // Pixel (x, y) covers [x, x+1) x [y, y+1): a band one pixel thick
            // across the major axis, from the first pixel to past the last.
            float ax=float(line.x1),ay=float(line.y1),bx=float(line.x2),by=float(line.y2);
            if(xMajor) { if(ax>bx){std::swap(ax,bx);std::swap(ay,by);} bx+=1; }
            else { if(ay>by){std::swap(ax,bx);std::swap(ay,by);} by+=1; }
            const float px=xMajor ? 0.f : 1.f,py=xMajor ? 1.f : 0.f;   // across the band
            const auto at=[&](float x,float y){return SDL_Vertex{{(x-ox)*zm,(y-oy)*zm},c,{0,0}};};
            const SDL_Vertex q0=at(ax,ay),q1=at(bx,by),q2=at(bx+px,by+py),q3=at(ax+px,ay+py);
            vertices.insert(vertices.end(),{q0,q1,q2,q0,q2,q3});
        }
        SDL_SetRenderDrawBlendMode(ren_,SDL_BLENDMODE_BLEND);
        SDL_RenderGeometry(ren_,nullptr,vertices.data(),int(vertices.size()),nullptr,0);
    }

    // One projectile, drawn per weapon family.
    void GameView::drawFxProjectile(const ta::sim::Projectile& p) {
        const float zm = mapView_.zoom();
        if(p.taLineOfSight && p.wsrc && (p.wsrc->renderType==0 || p.wsrc->renderType==7)) {
            drawTaLineShot(p);
            return;
        }
        const bool native3d=p.ballistic3d || p.guided3d || p.taLineOfSight;
        const float renderX=native3d ? ta::sim::Fixed::raw(p.position[0]).toFloat() : p.x.toFloat();
        const float renderZ=native3d ? ta::sim::Fixed::raw(p.position[2]).toFloat() : p.z.toFloat();
        // A native 3D shot carries its height: test its own projected cell.
        if (native3d ? !projectedVisibleR(renderX, renderZ -
                           float(int(std::bit_cast<int16_t>(uint16_t(uint32_t(p.position[1]) >> 16))) >> 1))
                     : !cellVisibleR(renderX, renderZ)) return;
        float t = std::clamp(float(p.age) / std::max(float(p.flight), 1.0f), 0.0f, 1.0f);
        // Legacy 2D fixture shots have no vertical trajectory. Production
        // weapons carry native XYZ; do not invent a height from live units.
        const float palt = 0.0f;
        const float shotWorldY=native3d ? ta::sim::Fixed::raw(p.position[1]).toFloat() : 0.0f;
        const float velocityX=native3d ? ta::sim::Fixed::raw(p.velocity[0]).toFloat() : p.vx.toFloat();
        const float velocityY=native3d ? ta::sim::Fixed::raw(p.velocity[1]).toFloat() : 0.0f;
        const float velocityZ=native3d ? ta::sim::Fixed::raw(p.velocity[2]).toFloat() : p.vz.toFloat();
        const float velocityLength=std::max(std::sqrt(velocityX*velocityX+
            velocityY*velocityY+velocityZ*velocityZ),1e-3f);
        float nativeSpriteX=0,nativeSpriteY=0;
        if(native3d) {
            auto whole=[](int32_t v){return int(std::bit_cast<int16_t>(uint16_t(uint32_t(v)>>16)));};
            const int x=whole(p.position[0]),y=whole(p.position[1]),z=whole(p.position[2]);
            (void)heightAbove(float(x),float(z));
            nativeSpriteX=(float(x)-mapView_.offX())*zm;
            nativeSpriteY=(float(z-(y>>1))-mapView_.offY())*zm;
        }
        // Ground shadow (shadowgaf/shadowart, 218 weapons) and light pool
        // (lightmap, 36): both sit on the GROUND under the shot, not at its
        // altitude, which is what sells the shot as being up in the air.
        if (p.wsrc) {
            float gx = (renderX - mapView_.offX()) * zm - terrainLiftX(renderX, renderZ) * zm;
            float gy = (renderZ - mapView_.offY()) * zm - terrainLift(renderX, renderZ) * zm;
            if (p.wsrc->lightMap > 0) {
                // A soft additive pool tinted by the shot's own family, so a
                // fireball throws warm light on the ground as it passes.
                float r = (10.0f + 8.0f * float(p.wsrc->lightMap)) * zm;
                Uint8 lr = 255, lg = 230, lb = 160;
                if (p.fx == ta::sim::WeaponFx::Lightning) { lr = 190; lg = 215; lb = 255; }
                else if (p.fx == ta::sim::WeaponFx::Fire) { lr = 255; lg = 160; lb = 70; }
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_ADD);
                for (int ring = 3; ring >= 1; --ring) {
                    float rr = r * float(ring) / 3.0f;
                    SDL_SetRenderDrawColor(ren_, lr, lg, lb, Uint8(26));
                    SDL_FRect q{gx - rr, gy - rr * 0.5f, rr * 2, rr};
                    SDL_RenderFillRectF(ren_, &q);
                }
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            }
            if (shadowsOnFrame_ && !p.wsrc->shadowArt.empty()) {
                // shadowgaf is always "shadows"; effectFor's "file:sequence"
                // form picks the named sequence out of it.
                if (const EffectAnim* sh = effectFor("shadows:" + p.wsrc->shadowArt)) {
                    const auto index=ta::retailEffectFrame(sh->durations,sh->loop,uint32_t(p.age));
                    if(index && *index<sh->frames.size()) {
                        const auto& fr=sh->frames[*index];
                        // effectFor caches its textures additively for glowing
                        // effects; a shadow has to DARKEN instead.
                        SDL_SetTextureBlendMode(fr.tex, SDL_BLENDMODE_BLEND);
                        SDL_SetTextureAlphaMod(fr.tex, 110);
                        float fw = float(fr.w) * zm, fh = float(fr.h) * zm;
                        SDL_FRect dst{gx - fw * 0.5f, gy - fh * 0.5f, fw, fh};
                        SDL_RenderCopyF(ren_, fr.tex, nullptr, &dst);
                    }
                }
            }
        }
        // A shot with a real mesh (arrows, spears, boulders) is drawn as that
        // mesh, yawed along its flight so an arrow actually points where it is
        // going. Projectile models point along +z (the arrowhead is at
        // +z and the fletching at -z), opposite the unit-model forward axis.
        const std::string* projectileModel = p.wsrc ? &p.wsrc->shotModel : nullptr;
        if (projectileModel && !projectileModel->empty()) {
            if(native3d) {
                drawShotModel(*projectileModel,p.fromPlayer,renderX,renderZ,0,
                    3.14159265358979323846f-float(p.angles[1])*(6.283185307179586f/65536.f),&p);
                return;
            }
            bool bal = p.wsrc->ballistic;
            const float flightSec = float(p.flight) / 30.0f;
            float peak = bal ? std::min(95.0f, flightSec * 55.0f)
                             : std::min(18.0f, flightSec * 12.0f);
            float h = 8 + 4 * peak * t * (1 - t);
            // Ballistic 3DO shots retain the authored spinheading rate too.
            // The native model draw receives that accumulated heading; mirror
            // the same 30 Hz COB-unit conversion used for spinning shot art.
            const float spin = float(p.age) / 30.0f * float(p.wsrc->spinRate) *
                               (6.2831853071795864769f / 65536.0f);
            drawShotModel(*projectileModel, p.fromPlayer, renderX, renderZ,
                          h * zm + palt, 3.14159265358979323846f -
                              std::atan2(p.vx.toFloat(), p.vz.toFloat()) + spin);
            return;
        }
        // Authored projectile art. Retail draws most shots as a real sprite
        // sequence (weaponart -> anims/<name>_4444.taf): cannonballs, fireballs,
        // iceballs, meteors, lightning balls. Ours drew every one of them as the
        // same yellow streak. Same loader the explosion/flame effects use.
        const std::string& shotSprite = p.wsrc ? p.wsrc->weaponArt : std::string();
        if (p.wsrc && !shotSprite.empty()) {
            if (const EffectAnim* ea = effectFor(shotSprite)) {
                // Ballistic shots arc; flat shots ride just above the ground.
                bool bal = p.wsrc->ballistic;
                const float flightSec = float(p.flight) / 30.0f;
                float peak = bal ? std::min(95.0f, flightSec * 55.0f)
                                 : std::min(18.0f, flightSec * 12.0f);
                float h = 8 + 4 * peak * t * (1 - t);
                float sx = native3d ? nativeSpriteX :
                    (renderX - mapView_.offX()) * zm - terrainLiftX(renderX, renderZ) * zm;
                float sy = native3d ? nativeSpriteY :
                    (renderZ - mapView_.offY()) * zm - h * zm
                        - terrainLift(renderX, renderZ) * zm - palt;
                const auto index=ta::retailEffectFrame(ea->durations,ea->loop,uint32_t(p.age));
                if(!index || *index>=ea->frames.size())return;
                const auto& fr = ea->frames[*index];
                float fw = float(fr.w) * zm, fh = float(fr.h) * zm;
                const auto origin=ta::retailEffectSpriteOrigin(sx,sy,fr.ax,fr.ay,zm);
                SDL_FRect dst{origin.x,origin.y,fw,fh};
                if (p.wsrc->spinRate != 0)
                    SDL_RenderCopyExF(ren_, fr.tex, nullptr, &dst,
                                      // age is TICKS and spinRate is raw COB units per second:
                                      // degrees = (ticks/30) * units * 360/65536.
                                      double(double(p.age) / 30.0 * double(p.wsrc->spinRate)
                                             * 360.0 / 65536.0),
                                      nullptr, SDL_FLIP_NONE);
                else
                    SDL_RenderCopyF(ren_, fr.tex, nullptr, &dst);
                return;
            }
        }
        if (p.fx == ta::sim::WeaponFx::Lightning) {
            // Flat, fast, jagged blue-white bolt from source toward target.
            float sx = native3d ? nativeSpriteX :
                (renderX - mapView_.offX()) * zm - terrainLiftX(renderX, renderZ) * zm;
            float sy = native3d ? nativeSpriteY :
                (renderZ - mapView_.offY()) * zm - 12 * zm - terrainLift(renderX, renderZ) * zm - palt;
            float len = 22.0f;
            float bx = -velocityX, bz = -velocityZ;
            float bl = native3d ? velocityLength : std::max(std::sqrt(bx * bx + bz * bz), 1e-3f);
            bx /= bl; bz /= bl;
            float px = sx, py = sy;
            SDL_SetRenderDrawColor(ren_, 210, 230, 255, 255);
            for (int s = 1; s <= 4; ++s) {
                float d = len * zm * s / 4.0f;
                float jitter = ((s * 1327 + int(float(p.age) / 30.0f * 900)) % 7 - 3) * 1.6f * zm;
                float nx = sx + bx * d - bz * jitter;
                float ny = sy + bz * d + bx * jitter -
                    (native3d ? velocityY / velocityLength * d * 0.5f : 12 * zm * s / 4.0f);
                SDL_RenderDrawLineF(ren_, px, py, nx, ny);
                px = nx; py = ny;
            }
        } else if (p.fx == ta::sim::WeaponFx::Fire) {
            // Flame breath: a short stream of flickering orange/yellow puffs
            // trailing behind the leading tip, not a single fireball.
            float bx = -velocityX, by = -velocityY, bz = -velocityZ;
            float bl = native3d ? velocityLength : std::max(std::sqrt(bx * bx + bz * bz), 1e-3f);
            bx /= bl; by /= bl; bz /= bl;
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            for (int s = 0; s < 5; ++s) {
                float back = s * 6.0f;   // world px behind the tip
                float wob = ((s * 811 + int(p.age * 1000)) % 5 - 2) * 2.0f;
                float fx = renderX + bx * back - bz * wob;
                float fz = renderZ + bz * back + bx * wob;
                float sx = native3d ? (fx-mapView_.offX())*zm :
                    (fx - mapView_.offX()) * zm - terrainLiftX(fx, fz) * zm;
                float sy = native3d ?
                    (fz-(shotWorldY+by*back)*0.5f-mapView_.offY())*zm :
                    (fz - mapView_.offY()) * zm - 12 * zm - terrainLift(fx, fz) * zm - palt;
                float r = (4.0f - s * 0.6f) * zm;   // shrinks toward the tail
                Uint8 aA = Uint8(200 - s * 30);
                // outer orange
                SDL_SetRenderDrawColor(ren_, 230, 90, 25, aA);
                SDL_FRect o{sx - r, sy - r, 2 * r, 2 * r};
                SDL_RenderFillRectF(ren_, &o);
                // hot yellow core at the leading puffs
                if (s < 2) {
                    SDL_SetRenderDrawColor(ren_, 255, 220, 110, aA);
                    SDL_FRect c{sx - r * 0.45f, sy - r * 0.45f, r * 0.9f, r * 0.9f};
                    SDL_RenderFillRectF(ren_, &c);
                }
            }
        } else {
            // Arrow/bolt/cannonball: a yellow streak. Ballistic weapons (FBI
            // type = Ballistic) lob a high arc scaled by flight time; other
            // shots (line-of-sight bolts) fly nearly flat.
            bool bal = p.wsrc && p.wsrc->ballistic;
            float peak = bal ? std::min(95.0f, p.flight * 55.0f)
                             : std::min(18.0f, p.flight * 12.0f);
            float h = 8 + 4 * peak * t * (1 - t);
            float sx = native3d ? nativeSpriteX :
                (renderX - mapView_.offX()) * zm - terrainLiftX(renderX, renderZ) * zm;
            float sy = native3d ? nativeSpriteY :
                (renderZ - mapView_.offY()) * zm - h * zm - terrainLift(renderX, renderZ) * zm - palt;
            SDL_SetRenderDrawColor(ren_, 255, 235, 140, 255);
            // vx/vz are px per TICK now; the trail length was tuned against px/s.
            const float trailX=native3d ? -velocityX/velocityLength*12.0f :
                -p.vx.toFloat()*30.0f*0.035f;
            const float trailY=native3d ? (-velocityZ+velocityY*0.5f)/velocityLength*12.0f :
                -p.vz.toFloat()*30.0f*0.035f;
            SDL_RenderDrawLineF(ren_,sx,sy,sx+trailX*zm,
                sy+trailY*zm+(t<0.5f?2.5f:-2.5f)*zm);
        }
    }


    void GameView::updateParticles(float dt) {
        static const bool kLog = ta::devEnv("TA_FXLOG") != nullptr;
        if (kLog && !particles_.empty()) {
            static size_t peak = 0;
            if (particles_.size() > peak) {
                peak = particles_.size();
                std::fprintf(stderr, "particles: %zu live (peak)\n", peak);
            }
        }
        // Wind pushes SMOKE only (retail's standard smoke/steam effects add the wind
        // vector to the position each tick; sparks and debris are ballistic). It goes
        // into the POSITION, not the velocity -- the damping two lines down would
        // otherwise eat it within a second.
        float windPx = windSpeed_ * (8.0f * 30.0f / 65536.0f) * dt;
        float windDX = std::sin(windHeading_) * windPx;
        float windDZ = std::cos(windHeading_) * windPx;
        for (auto& p : particles_) {
            p.life -= dt;
            p.x += p.vx * dt; p.z += p.vz * dt;
            if (p.kind == 2) { p.alt += p.valt * dt; continue; }   // nanolathe: straight line
            if (p.kind == 1) { p.x += windDX; p.z += windDZ; }
            p.alt += p.valt * dt;
            p.valt -= (p.kind == 1 ? 6.0f : 90.0f) * dt;   // smoke floats, sparks fall
            p.vx *= 0.92f; p.vz *= 0.92f;
        }
        std::erase_if(particles_, [](const Particle& p) { return p.life <= 0; });
    }

    // ---- the order line ------------------------------------------------------
    // Retail (icd 0x4d5700) strings `pathicon` beads along each leg of a selected
    // unit's order list at a FIXED 48-world-unit spacing, starting the comb at
    // ((tick - orderIssuedTick) % 30) * 48/30 -- so the whole line slides exactly
    // one spacing per second toward the destination, and each order's line crawls
    // on its own phase. The first leg runs from the unit itself to its first
    // order, a leg shorter than a pixel gets no beads, and the bead is blitted
    // with no colour remap (no player colour).
    //
    // Two retail details deliberately kept: an ATTACK order gets no line at all --
    // retail's per-order-kind descriptor flags the 17 attack/pickup/unload kinds
    // as end-marker-only -- and only the first few selected units get beads, which
    // is what keeps a 200-unit selection from turning the map into soup.
    //
    // SHIFT-GATED, as retail had it: the overlay is drawn only while shift is
    // physically held (0x4fcc46 polls GetAsyncKeyState(VK_SHIFT)). This was dropped
    // once, on the argument that an always-on line shows a queue at the moment you
    // build it -- but shift is already the key you are holding WHILE you build one,
    // so the line is there exactly when it is wanted and out of the way otherwise.
    //
    // Ownership is gated even though retail did not bother: our click-select has no
    // owner filter, and the render snapshot carries every unit's orders, so drawing
    // trails for anything but our own units would hand the player a readout of
    // enemy intent.
    void GameView::refreshOrderTrailTargets() {
        orderTrailTargets_.clear();
        for(const auto& f:features_) {
            if(!f.aliveVis || !f.hasSim || !f.reclaimable || !canPickPoint(f.x,f.z))continue;
            orderTrailTargets_.push_back({f.simId,ta::sim::Fixed::fromFloat(f.x),ta::sim::Fixed::fromFloat(f.z),
                f.simId%mapView_.map().width,f.simId/mapView_.map().width,f.fx,f.fz,f.blocking});
        }
        for(const auto* u:front().live) {
            if(u->alive() || !u->corpsePhase || !u->corpseReclaimable || !canPickPoint(u->x,u->z))continue;
            orderTrailTargets_.push_back({-u->id,ta::sim::Fixed::fromFloat(u->x),ta::sim::Fixed::fromFloat(u->z),
                u->corpseCellX,u->corpseCellZ,u->corpseFootX,u->corpseFootZ,false});
        }
        // Fog generations can advance even if none of this area's known
        // targets changed. Reuse the planned chain across those recomputes.
        uint64_t h=14695981039346656037ull;
        const auto add=[&](uint64_t v){h^=v;h*=1099511628211ull;};
        for(const auto& t:orderTrailTargets_) {
            for(auto v:{t.id,t.x.v,t.z.v,t.cellX,t.cellZ,t.footX,t.footZ})add(uint32_t(v));
            add(t.blocks);
        }

        orderTrailSceneSignature_=h;
    }


    void GameView::drawOrderTrails(int mvw, int winH) {
        if (selection_.empty() || !cursors_.ok()) return;
        // Held right now -- polled rather than tracked through key events, so it is
        // right even if focus changed while the key was down.
        if (!(SDL_GetModState() & KMOD_SHIFT)) return;
        const float zm = mapView_.zoom();
        const float kSpacing = 48.0f;          // world units, straight off retail
        // At low zoom the beads would overlap into a smear (48 world units is only
        // 12 screen px at 0.25x), so stop drawing rather than draw mush -- the same
        // call the waypoint rings already make.
        if (kSpacing * zm < 10.0f) return;
        const int scale = std::clamp(int(zm + 0.5f), 1, 3);
        const size_t frames = std::max<size_t>(1, cursors_.frameCount(ta::CursorId::PathIcon));
        // Retail shows the order line only while SHIFT is physically held -- the
        // whole overlay pass is behind a GetAsyncKeyState(VK_SHIFT) test at
        // 0x4fcc46. The beads are the part that reads as clutter when a big
        // selection is standing still, so they take the gate; the end markers stay
        // on, since they are what makes a selection's orders legible at a glance.
        const bool shiftHeld = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const uint32_t tick = front().gameTick;
        int drawn = 0;
        bool targetsRefreshed=false;
        for (int selId : selection_) {
            const UnitR* up = frameUnitP(selId);
            if (!up || !up->alive() || !up->type) continue;
            if (up->player != hudPlayer()) continue;       // never leak enemy intent
            if (up->displayedOrders().empty()) continue;
            // Retail beads only its few "focus" units but puts an END MARKER on
            // every selected unit's orders (the dots flag is per unit; the marker
            // is not), so the caps differ on purpose.
            const bool beads = drawn < kTrailUnits && shiftHeld;
            const auto& actual=up->displayedOrders();
            const std::vector<RenderOrder>* trail=&actual;
            // Persistent jobs queue one child at a time. Draw the area boundary
            // and expand its remaining known work, followed by the queued tail.
            // Cache only the few beaded units; ordinary queues need no planning.
            bool areas=false;
            for(const auto& o:actual)if(o.goal && o.reclaimArea) {
                areas=true;
                // The corners are ground points the drag picked through the terrain
                // lift (pickWorld), so each is drawn lifted back onto the raised tile
                // art; drawn flat, the box slid south of the drag on high ground.
                const float x0=o.x.toFloat(),z0=o.z.toFloat(),x1=o.buildX.toFloat(),z1=o.buildZ.toFloat();
                const auto corner=[&](float wx,float wz) {
                    return SDL_FPoint{(wx-mapView_.offX())*zm-terrainLiftX(wx,wz)*zm,
                                      (wz-mapView_.offY())*zm-terrainLift(wx,wz)*zm};
                };
                const SDL_FPoint box[5]{corner(x0,z0),corner(x1,z0),corner(x1,z1),corner(x0,z1),corner(x0,z0)};
                // Retail's 0x4d5180: white, yellow when the ordering unit is
                // selected -- and every unit drawn here is.
                const uint32_t argb=ta::client::retailAreaOrderArgb(true);
                SDL_SetRenderDrawColor(ren_,Uint8(argb>>16),Uint8(argb>>8),Uint8(argb),Uint8(argb>>24));
                SDL_RenderDrawLinesF(ren_,box,5);
            }
            if(areas && beads) {
                auto& cache=orderTrailCache_[size_t(drawn)];
                auto signature=ta::orderTrailSignature(actual)^reinterpret_cast<uintptr_t>(up->type);

                if(cache.unitId!=selId || cache.signature!=signature || cache.featGen!=lastFeatGen_ ||
                   cache.fogGen!=front().fogGen || cache.noFog!=noFog_ || tick-cache.tick>=30u) {
                    if(!targetsRefreshed) {refreshOrderTrailTargets();targetsRefreshed=true;}
                    if(cache.unitId!=selId || cache.signature!=signature ||
                       cache.sceneSignature!=orderTrailSceneSignature_ || tick-cache.tick>=30u)
                        ta::expandOrderTrail(actual,*up->type,up->player,
                            ta::sim::Fixed::fromFloat(up->x),ta::sim::Fixed::fromFloat(up->z),
                            orderTrailTargets_,cache.orders);
                    cache.unitId=selId;cache.signature=signature;cache.featGen=lastFeatGen_;
                    cache.fogGen=front().fogGen;cache.noFog=noFog_;cache.tick=tick;
                    cache.sceneSignature=orderTrailSceneSignature_;
                }
                trail=&cache.orders;
            }
            ++drawn;
            float px = up->x, pz = up->z;                  // running position
            // The current leg's line ends on the unit's OWN Legion spot when it has
            // one (drawUnitSpots marks it), not on the click every member shares.
            const bool spot = spotLeg(*up, 0);
            bool firstGoal = true;
            for (const auto& o : *trail) {
                // Only the ends of the player's own orders are line vertices; the
                // Route waypoints between them are the navigator's business, exactly
                // as retail's order list held goals and not path nodes.
                if (!o.goal) continue;
                const bool atSpot = spot && firstGoal;
                firstGoal = false;
                if (o.reclaimArea) continue;
                float qx = o.x.toFloat(), qz = o.z.toFloat();
                if (o.targetId && !o.guard && !o.load) {
                    // An attack step marks its target where the player can see it.
                    // A rally's attack entry never moves, so its point stays empty.
                    const UnitR* tu = frameUnitP(o.targetId);
                    if (tu && tu->alive() && canPickUnit(*tu)) { qx = tu->x; qz = tu->z; }
                    else if (o.x.v == 0 && o.z.v == 0) continue;
                }
                if (atSpot) { qx = up->spotX; qz = up->spotZ; }
                const bool line = o.targetId == 0;         // attack orders: marker only
                if (beads && line) {
                    float dx = qx - px, dz = qz - pz;
                    float len = std::sqrt(dx * dx + dz * dz);
                    if (len >= 1.0f) {
                        float ux = dx / len, uz = dz / len;
                        float phase = float((tick - o.issuedTick) % 30u) * kSpacing / 30.0f;
                        // Clip in world space with conservative byte-height
                        // relief margins. Large clears should not spend frame
                        // time sampling thousands of entirely offscreen legs.
                        // Begin at the first visible bead, keeping its phase and
                        // index, so a long leg stays dotted anywhere along it.
                        float enter=0,leave=len;
                        const auto clip=[&](float p,float u,float lo,float hi) {
                            if(std::abs(u)<1e-6f)return p>=lo && p<=hi;
                            float a=(lo-p)/u,b=(hi-p)/u;if(a>b)std::swap(a,b);
                            enter=std::max(enter,a);leave=std::min(leave,b);return enter<=leave;
                        };
                        const float padX=16/zm+255*std::abs(kHeightScaleX_);
                        const float padZ=16/zm+255*std::abs(kHeightScale_);
                        const bool visible=clip(px,ux,mapView_.offX()-padX,mapView_.offX()+mvw/zm+padX) &&
                            clip(pz,uz,mapView_.offY()-padZ,mapView_.offY()+winH/zm+padZ);
                        int bead=std::max(0,int(std::ceil((enter-phase)/kSpacing)));
                        int count=0;
                        for (float t = phase+bead*kSpacing; visible && t < len && t<=leave && count < kTrailBeads;
                             t += kSpacing, ++bead,++count) {
                            float wx = px + ux * t, wz = pz + uz * t;
                            float sx = (wx - mapView_.offX()) * zm - terrainLiftX(wx, wz) * zm;
                            float sy = (wz - mapView_.offY()) * zm - terrainLift(wx, wz) * zm;
                            if (sx < -16 || sx > float(mvw) + 16 || sy < -16 || sy > float(winH) + 16)
                                continue;
                            cursors_.drawFrame(ren_, ta::CursorId::PathIcon,
                                               size_t(bead) % frames, int(sx), int(sy), scale);
                        }
                    }
                }
                // The end marker: retail draws the order kind's OWN animated mouse
                // cursor at the waypoint (icd 0x4d5930 indexes a per-order-kind
                // cursor table at g[0x174e8]), stepping the frame from the game
                // tick. A queued move therefore shows an animated CursorMove where
                // it is going, patrol shows CursorPatrol, attack CursorAttack, and
                // so on. We drew a small green ring instead, which is nothing
                // retail ever put on the map.
                ta::CursorId marker = ta::CursorId::Move;
                bool haveMarker = true;
                if (o.buildType) {
                    marker=ta::CursorId::FindSite;
                    haveMarker=o.trailPreview; // existing queued sites already have ghosts
                }
                else if (o.revive)          marker = ta::CursorId::Revive;
                else if (o.reclaimFeat || o.reclaimArea) marker = ta::CursorId::Reclaim;
                else if (o.repairTarget)    marker = ta::CursorId::Repair;
                else if (o.load)            marker = ta::CursorId::Load;
                else if (o.unload)          marker = ta::CursorId::Unload;
                else if (o.guard)           marker = ta::CursorId::Defend;
                else if (o.targetId)        marker = ta::CursorId::Attack;
                else if (o.patrol)          marker = ta::CursorId::Patrol;
                // Fight-move: its waypoint is marked with the Attack glyph (ours;
                // the armed pointer itself is Move, as retail's).
                else if (o.attackMove)      marker = ta::CursorId::Attack;
                if (haveMarker) {
                    // Draw the marker where the PLAYER CLICKED, not where the
                    // unit will end up. order() snaps a destination the unit
                    // cannot stand on -- a click on a mountain -- to the nearest
                    // cell it fits in, and without this the marker visibly jumps
                    // off the cursor to the spot the engine picked. The bead
                    // trail still runs to the real destination, since that is
                    // the route the unit walks.
                    const float kx = o.clickX.toFloat() != 0.0f ? o.clickX.toFloat() : qx;
                    const float kz = o.clickZ.toFloat() != 0.0f ? o.clickZ.toFloat() : qz;
                    float mx = (kx - mapView_.offX()) * zm - terrainLiftX(kx, kz) * zm;
                    float my = (kz - mapView_.offY()) * zm - terrainLift(kx, kz) * zm;
                    if (mx > -40 && mx < float(mvw) + 40 && my > -40 && my < float(winH) + 40) {
                        size_t n = std::max<size_t>(1, cursors_.frameCount(marker));
                        const size_t markerFrame = ta::cursorOrderMarkerFrameAt(
                            n, cursors_.firstFrameDelayTicks(marker), tick);
                        cursors_.drawFrame(ren_, marker, markerFrame,
                                           int(mx), int(my), scale);
                    }
                }
                px = qx; pz = qz;                          // advance either way
            }
        }
    }

    // ---- destination markers (ours, not retail) ----------------------------------
    // A group order gives each member its own spot round the click -- Legion's
    // formation, arrival-layout and packed slots for the surface movers it plans, and
    // for every other unit (all of them in Retail mode) its own point from the click
    // (sim/groupspots.h; TotalA.exe sends far members to the click itself) -- so the
    // click marker alone does not say where a given body will stand. When the player
    // gives a move, fight-move or patrol order,
    // each of those units' own spot gets a thin outline of the unit's footprint in the
    // player's colour, held for a moment and faded out over kSpotFlashMs; a spot handed
    // out a few ticks later (a big click is handed out over several) appears when it is
    // handed out. With Shift held every selected unit's spot stays outlined -- its order
    // line ends there -- until it gets there and the leg ends.
    //
    // An outline rather than another cursor glyph: retail's order markers are the order
    // kind's own animated cursor at the click, uncoloured, and a second glyph per unit
    // would bury that one under a crowd. A footprint box in player colour reads as "a
    // body this size stands here", tells a big unit's spot from a small one's, and stays
    // legible packed edge to edge (each is inset 2 px so neighbours do not merge).
    //
    // Display only: the spot is captured with the unit's orders (UnitR::hasSpot), so it
    // exists only for units whose orders this client reads, and only the viewed player's
    // are drawn. A unit with no spot of its own (a Legion member still queued for its
    // slot, attack/guard/work legs) keeps the click marker alone.
    bool GameView::spotLeg(const UnitR& u, uint32_t minIssued) {
        if (!u.hasSpot || u.holdsRally()) return false;
        for (const auto& o : u.orders) {
            if (!o.goal) continue;
            return o.issuedTick >= minIssued && !o.targetId && !o.buildType && !o.reclaimFeat &&
                   !o.repairTarget && !o.load && !o.unload && !o.guard && !o.reclaimArea;
        }
        return false;
    }

    void GameView::noteSpotFlash(int unitId) {
        if (unitId <= 0) return;
        const SpotFlash f{unitId, front().gameTick, SDL_GetTicks64(), 0};
        for (auto& g : spotFlash_) if (g.id == unitId) { g = f; return; }
        if (spotFlash_.size() >= kSpotFlashMax) spotFlash_.erase(spotFlash_.begin());
        spotFlash_.push_back(f);
    }

    void GameView::drawUnitSpots(int mvw, int winH) {
        const bool shiftHeld = (SDL_GetModState() & KMOD_SHIFT) != 0;
        if (spotFlash_.empty() && (!shiftHeld || selection_.empty())) return;
        const uint64_t now = SDL_GetTicks64();
        const float zm = mapView_.zoom();
        const float th = std::max(1.0f, zm);
        spotBatch_.clear();
        int drawn = 0;
        const auto mark = [&](const UnitR& u, float alpha) {
            if (drawn >= kSpotDrawMax || alpha <= 0.0f) return;
            const float hx = std::max(4.0f, float(u.type->footX) * 8.0f - 2.0f) * zm;
            const float hz = std::max(4.0f, float(u.type->footZ) * 8.0f - 2.0f) * zm;
            // On the tile art at the spot, where the body will be drawn lifted to.
            const float cx = (u.spotX - mapView_.offX()) * zm - terrainLiftX(u.spotX, u.spotZ) * zm;
            const float cy = (u.spotZ - mapView_.offY()) * zm - terrainLift(u.spotX, u.spotZ) * zm;
            if (cx + hx < 0 || cx - hx > float(mvw) || cy + hz < 0 || cy - hz > float(winH)) return;
            SDL_Color c = playerColor(u.player);
            c.a = Uint8(std::clamp(alpha, 0.0f, 1.0f) * 230.0f);
            pushSeg(spotBatch_, cx - hx, cy - hz, cx + hx, cy - hz, th, c);
            pushSeg(spotBatch_, cx + hx, cy - hz, cx + hx, cy + hz, th, c);
            pushSeg(spotBatch_, cx + hx, cy + hz, cx - hx, cy + hz, th, c);
            pushSeg(spotBatch_, cx - hx, cy + hz, cx - hx, cy - hz, th, c);
            ++drawn;
        };
        for (auto& f : spotFlash_) {
            const UnitR* u = frameUnitP(f.id);
            if (!u || !u->alive() || !u->type || u->player != hudPlayer()) { f.id = 0; continue; }
            // Waits until the order is in the unit's queue and its spot handed out.
            if (!spotLeg(*u, f.tick)) {
                if (f.seenMs) f.id = 0;   // the leg ended (or lost its spot) while shown
                continue;
            }
            if (!f.seenMs) f.seenMs = now;
            if (shiftHeld && selSet_.count(f.id)) continue;   // held whole below
            const uint64_t age = now - f.seenMs, hold = kSpotFlashMs * 3 / 5;
            mark(*u, age < hold ? 1.0f : 1.0f - float(age - hold) / float(kSpotFlashMs - hold));
        }
        std::erase_if(spotFlash_, [&](const SpotFlash& f) {
            return f.id == 0 || (f.seenMs ? now - f.seenMs >= kSpotFlashMs : now - f.issuedMs >= kSpotWaitMs);
        });
        if (shiftHeld)
            for (int id : selection_) {
                const UnitR* u = frameUnitP(id);
                if (u && u->alive() && u->type && u->player == hudPlayer() && spotLeg(*u, 0)) mark(*u, 1.0f);
            }
        if (!spotBatch_.empty()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(ren_, nullptr, spotBatch_.data(), int(spotBatch_.size()), nullptr, 0);
        }
    }

    // ---- Shift range circles (retail 0x4d6530, client/orderoverlay.h) -----------
    // With Shift held retail rings each selected own unit, and the own unit under
    // the pointer (main view or minimap), with its mincloakdistance while it is
    // cloaked (white), and a kamikaze type with a red blast pulse and a red
    // trigger ring. Each ring point sits on the higher of the unit and the ground
    // under the point.
    void GameView::drawRangeCircles(int mvw, int winH) {
        if (!(SDL_GetModState() & KMOD_SHIFT)) return;
        const auto area = pointerArea();
        const bool pointerPicks = area == ta::client::PointerArea::World || area == ta::client::PointerArea::Minimap;
        if (selection_.empty() && !pointerPicks) return;
        const float zm = mapView_.zoom();
        const uint32_t tick = front().gameTick;
        const float th = std::max(1.0f, zm);
        overlayBatch_.clear();
        const auto rings = [&](const UnitR& u) {
            if (!u.alive() || !u.type || u.embarked() || u.player != hudPlayer()) return;
            const auto& orders = u.orders;
            if (!ta::client::missionsShowRangeCircles(orders.size(), [&](size_t i) {
                    return orders[i].load || orders[i].unload || orders[i].reclaimArea; }))
                return;
            const auto& t = *u.type;
            ta::client::RangeCircleInput in;
            in.minCloakDistance = t.minCloakDist; in.cloaked = u.cloaked;
            in.kamikaze = t.kamikaze; in.hasExplodeAs = t.hasExplodeAs; in.explodeAoe = t.explodeAs.aoe;
            in.kamikazeDistance = t.kamikazeDist; in.sightDistance = t.sight;
            in.mobile = !isStructure(&t);
            const auto circles = ta::client::retailRangeCircles(in, tick);
            if (circles.empty()) return;
            float cx, cz, ch; interpPose(u, cx, cz, ch);
            const float unitLift = uLiftY(u) + altLift(u);
            const auto screen = [&](float wx, float wz) {
                const float lift = std::max(unitLift, terrainLift(wx, wz));
                return SDL_FPoint{(wx - mapView_.offX()) * zm - terrainLiftX(wx, wz) * zm,
                                  (wz - mapView_.offY()) * zm - lift * zm};
            };
            const auto onScreen = [&](SDL_FPoint p) { return p.x >= 0 && p.x < float(mvw) && p.y >= 0 && p.y < float(winH); };
            for (const auto& c : circles) {
                const int n = ta::client::retailCircleSegments(c.radius);
                if (n <= 0) continue;
                // Whole ring off screen: skip it before sampling the relief.
                const float reach = float(c.radius) * zm + 128.0f * zm;
                const SDL_FPoint mid = screen(cx, cz);
                if (mid.x + reach < 0 || mid.x - reach > float(mvw) || mid.y + reach < 0 || mid.y - reach > float(winH))
                    continue;
                const SDL_Color col{Uint8(c.argb >> 16), Uint8(c.argb >> 8), Uint8(c.argb), Uint8(c.argb >> 24)};
                auto o = ta::client::retailCirclePoint(c.radius, n, 0);
                SDL_FPoint prev = screen(cx + o.dx, cz + o.dz);
                for (int k = 1; k <= n; ++k) {
                    o = ta::client::retailCirclePoint(c.radius, n, k);
                    const SDL_FPoint next = screen(cx + o.dx, cz + o.dz);
                    if (onScreen(prev) || onScreen(next))
                        pushSeg(overlayBatch_, prev.x, prev.y, next.x, next.y, th, col);
                    prev = next;
                }
            }
        };
        for (int id : selection_)
            if (const UnitR* up = frameUnitP(id)) rings(*up);
        // 0x521260 picks the hovered unit in the main view or on the minimap.
        if (pointerPicks)
            if (const UnitR* hovered = hoveredUnit(area);
                hovered && !selSet_.count(hovered->id))
                rings(*hovered);
        if (!overlayBatch_.empty()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(ren_, nullptr, overlayBatch_.data(), int(overlayBatch_.size()), nullptr, 0);
        }
    }

    void GameView::drawShotModel(const std::string& name, int player, float x, float z,
                       float altPx, float facing, const ta::sim::Projectile* native) {
        const ta::tdo::Model* model = ghostModel(name);
        if (!model) return;
        // These retail bolt meshes have their arrowhead on -Z; the other
        // directional projectile meshes (including araarrow) point along +Z.
        if (name == "verbal1" || name == "verbal1_vet") facing += 3.14159265f;
        noteNeed(Retained::ModelTris, tris_);
        tris_.clear();
        SDL_Texture* atlas = atlasFor(playerColorSlot(player));
        // Projectile roots contain the shot itself, unlike unit roots whose
        // ground-reference plates are suppressed by collect().
        const Xform base=native ? modelBodyTransform(native->angles[2],native->angles[0]) : Xform{};
        collect(tris_, atlas, model->root, base, nullptr, facing, player,
                false, false);
        if (tris_.empty()) return;
        std::stable_sort(tris_.begin(), tris_.end(),
                         [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
        float zm = mapView_.zoom();
        float ax = (x - mapView_.offX()) * zm - terrainLiftX(x, z) * zm;
        float ay = (z - mapView_.offY()) * zm - terrainLift(x, z) * zm - altPx;
        if(native) {
            const int y=std::bit_cast<int16_t>(uint16_t(uint32_t(native->position[1])>>16));
            (void)heightAbove(x,z);
            ax=(x-mapView_.offX())*zm;
            ay=(z-float(y>>1)-mapView_.offY())*zm;
        }
        for (auto& t : tris_) for (auto& v : t.v) {
            v.position.x = v.position.x * zm + ax;
            v.position.y = v.position.y * zm + ay;
        }
        drawModelTriangles();
    }

    // The model-local ghost of `model` under the current atlas (see GhostGeometry):
    // filled on first use and whenever the atlas or a generation it was made under
    // changed. Collection and sort are drawGhostAt's former code, unchanged.
    const GameView::GhostGeometry& GameView::ghostGeometry(const ta::tdo::Model* model,
                                                           SDL_Texture* atlas) {
        // A renderer reset or an asset reload makes every entry stale at once.
        if (ghostRendererGen_ != rendererGeneration_.current() ||
            ghostAssetGen_ != assetGeneration_.current() || ghostGeometry_.size() > 256) {
            ghostGeometry_.clear();
            ghostGeometryBytes_ = 0;
            ghostRendererGen_ = rendererGeneration_.current();
            ghostAssetGen_ = assetGeneration_.current();
        }
        auto [it, fresh] = ghostGeometry_.try_emplace(model);
        GhostGeometry& g = it->second;
        if (!fresh && g.atlas == atlas && g.atlasW == atlasW_ && g.atlasH == atlasH_) return g;
        ghostGeometryBytes_ -= g.tris.capacity() * sizeof(Tri);
        renderStats_.add(ta::render::Counter::GhostCollects);
        g.tris.clear();
        workerScratch().unpackedTextures = 0;
        collect(g.tris, atlas, model->root, Xform{}, nullptr, 0.0f, hudPlayer());
        std::stable_sort(g.tris.begin(), g.tris.end(),
                  [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
        g.atlas = atlas; g.atlasW = atlasW_; g.atlasH = atlasH_;
        g.exact = workerScratch().unpackedTextures == 0;
        g.empty = g.tris.empty();
        g.minX = g.minY = 0; g.maxX = g.maxY = 0;
        if (!g.empty) {
            g.minX = g.maxX = g.tris[0].v[0].position.x;
            g.minY = g.maxY = g.tris[0].v[0].position.y;
            for (const auto& t : g.tris) for (const auto& v : t.v) {
                g.minX = std::min(g.minX, v.position.x); g.maxX = std::max(g.maxX, v.position.x);
                g.minY = std::min(g.minY, v.position.y); g.maxY = std::max(g.maxY, v.position.y);
            }
        }
        // Only the bounds of an inexact model are reusable: its triangles are
        // collected again for every ghost drawn.
        if (!g.exact) std::vector<Tri>().swap(g.tris);
        ghostGeometryBytes_ += g.tris.capacity() * sizeof(Tri);
        return g;
    }

    void GameView::drawGhostAt(const ta::sim::UnitType* type, float x, float z,
                     bool invalid) {
        renderStats_.add(ta::render::Counter::GhostDraws);
        const ta::tdo::Model* model = ghostModel(type->id);
        if (!model) return;
        renderStats_.add(ta::render::Counter::GhostPreps);
        SDL_Texture* atlas = atlasFor(playerColorSlot(hudPlayer()));
#ifndef NDEBUG
        static const bool reference = ta::devFlag("TA_GHOST_REFERENCE");
        static const bool verify = ta::devFlag("TA_GHOST_VERIFY");
#else
        constexpr bool reference = false;
#endif
        const GhostGeometry* g = reference ? nullptr : &ghostGeometry(model, atlas);
        float zm = mapView_.zoom();
        // The ghost lifts onto the relief exactly like the finished unit/building will.
        float ax = (x - mapView_.offX()) * zm - terrainLiftX(x, z) * zm;
        float ay = (z - mapView_.offY()) * zm - terrainLift(x, z) * zm;
        // Wholly outside the clip (the world viewport; else the render viewport), the
        // ghost draws no pixel: skip it before any triangle work. The placed bounds
        // are the model-local bounds under the same placement (monotone in each
        // coordinate); one pixel of margin.
        SDL_Rect clip{};
        if (SDL_RenderIsClipEnabled(ren_)) SDL_RenderGetClipRect(ren_, &clip);
        else { SDL_RenderGetViewport(ren_, &clip); clip.x = clip.y = 0; }
        const auto outside = [&](float x0, float y0, float x1, float y1, float margin) {
            const float sx0 = x0 * zm + ax, sx1 = x1 * zm + ax;
            const float sy0 = y0 * zm + ay, sy1 = y1 * zm + ay;
            return std::max(sx0, sx1) < float(clip.x) - margin ||
                   std::min(sx0, sx1) > float(clip.x + clip.w) + margin ||
                   std::max(sy0, sy1) < float(clip.y) - margin ||
                   std::min(sy0, sy1) > float(clip.y + clip.h) + margin;
        };
        const bool culled = g && (g->empty || outside(g->minX, g->minY, g->maxX, g->maxY, 1.0f));
#ifndef NDEBUG
        if (verify && g) {
            // The reference path: drawGhostAt's former collection and sort.
            ghostVerifyTris_.clear();
            collect(ghostVerifyTris_, atlas, model->root, Xform{}, nullptr, 0.0f, hudPlayer());
            std::stable_sort(ghostVerifyTris_.begin(), ghostVerifyTris_.end(),
                      [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
            const auto fail = [&](const char* what) {
                throw std::runtime_error(std::string("ghost verify: ") + what + " (" + type->id + ")");
            };
            if (ghostVerifyTris_.empty() != g->empty) fail("emptiness differs from the reference");
            float minX = 0, minY = 0, maxX = 0, maxY = 0;
            if (!ghostVerifyTris_.empty()) {
                minX = maxX = ghostVerifyTris_[0].v[0].position.x;
                minY = maxY = ghostVerifyTris_[0].v[0].position.y;
            }
            for (const auto& t : ghostVerifyTris_) for (const auto& v : t.v) {
                minX = std::min(minX, v.position.x); maxX = std::max(maxX, v.position.x);
                minY = std::min(minY, v.position.y); maxY = std::max(maxY, v.position.y);
            }
            if (std::bit_cast<uint32_t>(minX) != std::bit_cast<uint32_t>(g->minX) ||
                std::bit_cast<uint32_t>(maxX) != std::bit_cast<uint32_t>(g->maxX) ||
                std::bit_cast<uint32_t>(minY) != std::bit_cast<uint32_t>(g->minY) ||
                std::bit_cast<uint32_t>(maxY) != std::bit_cast<uint32_t>(g->maxY))
                fail("bounds differ from the reference");
            ++ghostVerify_.checked;
            if (culled) {
                // No reference vertex may land inside the clip.
                if (!ghostVerifyTris_.empty() && !outside(minX, minY, maxX, maxY, 0.0f))
                    fail("a culled ghost reaches the clip");
                ++ghostVerify_.culled;
            } else if (g->exact) {
                if (g->tris.size() != ghostVerifyTris_.size()) fail("triangle count differs from the reference");
                for (size_t i = 0; i < g->tris.size(); ++i) {
                    const Tri& a = g->tris[i];
                    const Tri& b = ghostVerifyTris_[i];
                    if (a.tex != b.tex || std::memcmp(a.v, b.v, sizeof a.v) != 0 ||
                        std::bit_cast<uint32_t>(a.depth) != std::bit_cast<uint32_t>(b.depth))
                        fail("retained triangles differ from the reference");
                }
                ++ghostVerify_.hits;
            } else {
                ++ghostVerify_.inexact;
            }
        }
#endif
        if (culled) {
            renderStats_.add(ta::render::Counter::GhostCulled);
            return;
        }
        noteNeed(Retained::ModelTris, tris_);
        if (g && g->exact) {
            tris_.assign(g->tris.begin(), g->tris.end());
            renderStats_.add(ta::render::Counter::GhostCacheHits);
        } else {
            tris_.clear();
            collect(tris_, atlas, model->root, Xform{}, nullptr, 0.0f, hudPlayer());
            std::stable_sort(tris_.begin(), tris_.end(),
                      [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
            renderStats_.add(ta::render::Counter::GhostCollects);
#ifndef NDEBUG
            if (verify && g) ++ghostVerify_.collected;
#endif
        }
        renderStats_.add(ta::render::Counter::GhostTris, tris_.size());
        for (auto& t : tris_) for (auto& v : t.v) {
            v.position.x = v.position.x * zm + ax;
            v.position.y = v.position.y * zm + ay;
            if (invalid) {
                // Can't build here: wash the ghost red instead of the box.
                v.color.a = 150;
                v.color.r = Uint8(std::min(255, int(v.color.r * 0.6f) + 110));
                v.color.g = Uint8(v.color.g * 0.30f);
                v.color.b = Uint8(v.color.b * 0.30f);
            } else {
                v.color.a = 130;
                v.color.r = Uint8(v.color.r * 0.55f);
                v.color.g = Uint8(v.color.g * 0.8f);
            }
        }
        drawModelTriangles();
    }

    void GameView::drawUnitShadow(const UnitGeom& g) {
        if (!batchShadowMasks_.has_value()) {
            SDL_RendererInfo info{};
            batchShadowMasks_=SDL_GetRendererInfo(ren_,&info)==0 && (info.flags&SDL_RENDERER_ACCELERATED)!=0;
        }
        profShadowVerts_ += g.shadowVerts.size() + g.maskedShadowVerts.size();
        if (shadowSilhouetteAtlasActive_ && g.shadowAtlasPage) {
            SDL_RenderCopyF(ren_, g.shadowAtlasPage, &g.shadowAtlasSrc, &g.shadowAtlasDst);
            return;
        }
        static const SDL_Color color{kShadowLevel,kShadowLevel,kShadowLevel,255};
        static const float uv[2] = {0,0};
        if (!g.shadowVerts.empty())
            SDL_RenderGeometryRaw(ren_,nullptr,&g.shadowVerts[0].x,sizeof(SDL_FPoint),
                                  &color,0,uv,0,int(g.shadowVerts.size()),nullptr,0,0);
        int offset=0;
        for (const auto& run:g.maskedShadowRuns) {
            ta::submitShadowMaskRun(ren_,run.first,g.maskedShadowVerts.data()+offset,
                                     run.second,*batchShadowMasks_);
            offset+=run.second;
        }
    }

    void GameView::collect(std::vector<Tri>& out, SDL_Texture* atlas, const ta::tdo::Object& o,
                 const Xform& parent, const Anim* anim, float heading, int player,
                 bool mirror, bool isRoot, bool shadow,
                 RadialExtent* ext, const PieceMeta* meta,
                 bool shadowCull, const PreparedPiece* prepared,
                 bool paletteLighting, NanoCollect* nano) {
        if (mirror || ext) prepared=nullptr; // special preview/extent paths use the reference transform
        const ta::cob::PieceState* ps = prepared ? nullptr : pieceFor(anim, o.name);
        // Retail tests visibility per piece; hidden parents still transform
        // their children (including after a non-subtree EXPLODE).
        const bool hidden = prepared ? prepared->hidden : ps && !ps->visible;
        Xform xf = prepared ? prepared->transform : scriptTransform(parent,o.x,o.y,o.z,ps);
        // Precomputed when the model was registered; computed here only for a model
        // that has no cached tree (ghost previews, build-icon portraits). Same
        // function either way.
        PieceMeta local;
        if (!meta) { pieceMetaFor(o, isRoot, local); meta = &local; }
        const bool shadowDisabled = shadow && (prepared ? !prepared->castsShadow :
                                                        ps && !ps->castsShadow());
        const bool groundPlate = meta->skip || hidden || shadowDisabled;
        // A cached piece (the default; DONT_CACHE clears it) is drawn into the
        // unit's object bitmap with every texture at its first frame
        // (0x4586a0(obj, 0, 1) -> 0x459830 arg 1 -> 0x459b67: 0x4b7f30(tex, 0));
        // only dont-cache pieces are redrawn with the playing frame (0x459b78).
        const bool cachedPiece = prepared ? prepared->cached : !ps || ps->cached;
        const PieceMeta* const pieceMeta = meta;
        // (A unit being built is redrawn whole every frame, 0x459641 arg 0: no
        // cached layer then, its textures playing; buildNanoframe passes the still
        // tree for a structure, whose per-frame draw is skipped, 0x45960d.)
        if (!shadow && !nano && cachedPiece && meta->stillTwin) meta = meta->stillTwin;
        if (nano) ++nano->pieces;
        float cy = std::cos(heading), sy = std::sin(heading);
        // The body and the nanoframe take retail's primitive order (sorted at load,
        // selection primitive left out: client/primitivedraw.h); the shadow keeps
        // the file order its fast path (buildUnitShadow) reproduces.
        const bool retailOrder = !shadow && meta->drawOrder.size() + (o.selectionPrimitive != -1 &&
            !o.primitives.empty() ? 1 : 0) == o.primitives.size();
        const size_t drawn = retailOrder ? meta->drawOrder.size() : o.primitives.size();
        for (size_t slot = 0; slot < drawn; ++slot) {
            if (groundPlate) break;
            const size_t pi = retailOrder ? size_t(meta->drawOrder[slot]) : slot;
            if (!retailOrder && int32_t(pi) == o.selectionPrimitive) continue;
            const auto& p = o.primitives[pi];
            if (p.indices.size() < 3) continue;
            SDL_Texture* tex = nullptr;
            const SDL_Rect* arect = nullptr;
            const std::string& name = pi < meta->primTex.size() ? meta->primTex[pi]
                                                                : p.texture;
            if (!name.empty()) {
                const SDL_Rect* packed = nullptr;
                if (pi < meta->primAtlas.size()) packed = meta->primAtlas[pi];
                else {
                    // Transient metadata (ghosts/projectiles) still resolves live.
                    auto rit = atlasRect_.find(name);
                    if (rit != atlasRect_.end()) packed = &rit->second;
                }
                if (atlas && packed) {
                    tex = atlas;             // whole model shares one atlas texture
                    arect = packed;
                } else {
                    // Fallback for any texture not packed into the atlas.
                    ++workerScratch().unpackedTextures;
                    auto it = textures_.find(name);
                    if (it != textures_.end() && !it->second.empty()) {
                        // Unpacked previews/projectiles use the same animation
                        // frame as the atlas, or the remapped player logo colour.
                        const size_t ci = animatedTex_.count(name)
                            ? modelTextureAnimations_.at(name).frame
                            : (it->second.size() == 1 ? 0 : size_t(playerColorSlot(player)));
                        tex = ci < it->second.size() ? it->second[ci] : nullptr;
                    }
                }
            }
            SDL_Texture* shadowMask = nullptr;
            SDL_FRect shadowUV{0,0,1,1};
            if (shadow) {
                const std::vector<ShadowMaskFrame>* masks=nullptr;
                bool animated=false;
                if (pi<meta->primShadowMasks.size()) {
                    masks=meta->primShadowMasks[pi];
                    animated=meta->primAnimated[pi];
                } else {
                    auto it=shadowMasks_.find(name);
                    if (it!=shadowMasks_.end()) masks=&it->second;
                    animated=animatedTex_.count(name)!=0;
                }
                if (masks) {
                    const size_t frame=animated ? modelTextureAnimations_.at(name).frame
                                                : size_t(playerColorSlot(player)) % masks->size();
                    if(frame < masks->size()) {
                        shadowMask=(*masks)[frame].texture;shadowUV=(*masks)[frame].uv;
                    }
                }
            }
            // Transform each of this primitive's vertices ONCE. The fan below
            // visits vertex 0 in every triangle and each middle vertex twice, so
            // doing the transform inside the triangle loop did it 3*(n-2) times
            // for n vertices -- six transforms for a quad that needs four. The
            // UVs still come from the fan position (retail maps a quad's corners
            // to the four UV corners), so only the geometry is shared.
            using VOut = CollectVertex;
            std::vector<VOut>& vcache = workerScratch().collectVerts;
            vcache.resize(p.indices.size());
            for (size_t vi2 = 0; vi2 < p.indices.size(); ++vi2) {
                VOut& vo = vcache[vi2];
                size_t vi = size_t(p.indices[vi2]) * 3;
                if (vi + 2 >= o.vertices.size()) { vo.ok = false; continue; }
                vo.ok = true;
                float w[3];
                if (!prepared) xf.apply(o.vertices[vi], o.vertices[vi + 1], o.vertices[vi + 2], w);
                else { w[0]=0; w[1]=prepared->rotated[vi/3][1]; w[2]=0; }
                const float wx = mirror ? -w[0] : w[0];   // un-mirror mirrored models on X
                const float rx = prepared ? prepared->rotated[vi/3][0] : wx * cy + w[2] * sy;
                const float rz = prepared ? prepared->rotated[vi/3][2] : -wx * sy + w[2] * cy;
                // TA billboards lean back (+y and +z together); moving away (+z)
                // reads upward on screen, adding to height. The coefficients are
                // retail's own (icd 0x421dad): all of z, half of y.
                const float ry = w[1] * kProjY + rz * kProjZ;
                vo.px = rx; vo.py = w[1]; vo.pz = rz;
                // Farthest first: depth from the camera goes as (z - 2y).
                vo.d = rz * kSortZ - w[1] * kSortY;
                if (ext) {
                    // Pre-rotation x/z, so this is independent of `heading`.
                    const float r = std::sqrt(wx * wx + w[2] * w[2]);
                    const float base = -(w[1] * kProjY);   // the y term of -ry
                    ext->maxR = std::max(ext->maxR, r);
                    ext->minY = std::min(ext->minY, base - r * kProjZ);
                    ext->maxY = std::max(ext->maxY, base + r * kProjZ);
                    ext->any = true;
                }
                if (shadow) {
                    // Lean by the light: the silhouette of the unit, not a disc
                    // under it. Retail's shear is (+y/4, +y/4) in screen space
                    // (0x4ec250 flag 1), i.e. down AND right.
                    //
                    // PLUS on both, and verify any change to these signs on a
                    // GROUND unit. A flyer is the wrong test: its shadow also
                    // carries the altitude term applied at the anchor, and that
                    // term can outweigh and mask the vertex term, so a flyer probe
                    // reports the sign of the wrong thing. It told me to negate x,
                    // and negating x sent every ground unit's shadow leaning left.
                    // With alt = 0 a ground unit isolates exactly this expression.
                    vo.pos = {rx + kShadowLX * w[1], -(rz + kShadowLZ * w[1]) * kProjZ};
                } else {
                    vo.pos = {rx, -ry};
                }
            }
            if (nano && !shadow) {
                // The whole primitive, unculled: the nanoframe's walk culls and
                // depth-tests per pixel (nanoraster.h).
                bool whole = true;
                for (const auto& vo : vcache) whole = whole && vo.ok;
                if (whole) {
                    NanoCollect::Prim prim;
                    prim.first = uint32_t(nano->points.size());
                    prim.count = uint32_t(vcache.size());
                    prim.piece = nano->pieces;
                    for (const auto& vo : vcache) nano->points.push_back({vo.px, vo.py, vo.pz});
                    if (name.empty()) prim.colorIndex = int32_t(p.colorIndex & 0xff);
                    else if (const int flat = ta::primitivedraw::flatColourOf(name); flat >= 0)
                        prim.colorIndex = flat;
                    else if (const auto source = paletteTextureSources_.find(name);
                             source != paletteTextureSources_.end() && !source->second.frames.empty()) {
                        const auto& frames = source->second.frames;
                        size_t frame = animatedTex_.count(name) ? modelTextureAnimations_.at(name).frame
                                     : frames.size() == 1 ? 0 : size_t(playerColorSlot(player));
                        prim.frame = &frames[std::min(frame, frames.size() - 1)];
                        prim.bank = source->second.bank;
                        if (paletteLighting) {
                            prim.level = 15;
                            if (prepared ? prepared->shaded : !ps || ps->shaded) {
                                const auto& a=vcache[0];const auto& b=vcache[1];const auto& c=vcache[2];
                                const float ux=b.px-a.px,uy=b.py-a.py,uz=b.pz-a.pz;
                                const float vx=c.px-a.px,vy=c.py-a.py,vz=c.pz-a.pz;
                                float nx=uy*vz-uz*vy,ny=uz*vx-ux*vz,nz=ux*vy-uy*vx;
                                if (mirror) {nx=-nx;ny=-ny;nz=-nz;}
                                prim.level = ta::retailModelShadeLevel(nx,ny,nz);
                            }
                        }
                    }
                    nano->prims.push_back(prim);
                }
            }
            // A textured primitive is drawn only as a quad (0x4585e2, 0x459b1a); a
            // flat one with any count. (The nanoframe outline above takes it still.)
            const bool drawsBody = shadow || p.indices.size() == 4 || ta::primitivedraw::flatColourOf(name) >= 0;
            for (size_t i = 1; drawsBody && i + 1 < p.indices.size(); ++i) {
                size_t idx[3] = {0, i, i + 1};
                Tri tri{};
                tri.tex = tex;
                float depth = 0;
                bool ok = true;
                float px[3], py[3], pz[3];   // rotated model space, for the facing test
                for (int k = 0; k < 3; ++k) {
                    const VOut& vo = vcache[idx[k]];
                    if (!vo.ok) { ok = false; break; }
                    px[k] = vo.px; py[k] = vo.py; pz[k] = vo.pz;
                    depth += vo.d;
                    tri.v[k].position = vo.pos;
                    // The UV comes from the FAN POSITION, not the vertex: retail
                    // maps a quad's four corners to the four UV corners, so a
                    // vertex reused at another fan position takes a different one.
                    static const SDL_FPoint uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                    SDL_FPoint c = uv[idx[k] & 3];
                    tri.v[k].tex_coord = arect
                        ? SDL_FPoint{(float(arect->x) + c.x * float(arect->w)) / float(atlasW_),
                                     (float(arect->y) + c.y * float(arect->h)) / float(atlasH_)}
                        : c;
                    tri.v[k].color = tex ? SDL_Color{255, 255, 255, 255}
                                         : SDL_Color{170, 170, 180, 255};
                }
                if (!ok) continue;
                if (shadow) {
                    // shadowCull: honour the BODY's cull for THIS unit. Off for living
                    // units -- the measured reasons are below. On for CORPSES, where not
                    // culling produces an artifact the no-cull rule never anticipated: a
                    // body lying flat presents a large face PARALLEL to the projection
                    // plane, so it projects at maximal area as a clean parallelogram --
                    // and the body pass culls that same face, so the slab has no visible
                    // object casting it. Reported from a screenshot as a square shadow
                    // around some dead units; found as a single 4190px^2 shadow triangle
                    // on corpses whose deadFor kept climbing.
                    //
                    // Culling a flat corpse cannot punch a meaningful hole: what survives
                    // is the upward-facing surface, which is exactly what you can see of
                    // it. The hole problem below is about UPRIGHT geometry.
                    if (shadowCull) {
                        const float cax = px[1]-px[0], cay = py[1]-py[0], caz = pz[1]-pz[0];
                        const float cbx = px[2]-px[0], cby = py[2]-py[0], cbz = pz[2]-pz[0];
                        const float cny = caz * cbx - cax * cbz;
                        const float cnz = cax * cby - cay * cbx;
                        float fc = cny * kSortY - cnz * kSortZ;
                        if (mirror) fc = -fc;
                        if (fc <= 0.0f) continue;
                    }
                    // NO BACKFACE CULL. This is the single biggest cost in a crowded
                    // frame, so it has been measured three ways rather than assumed --
                    // and the reason it cannot be culled is a property of the SHIPPED
                    // MODELS, not of the projection.
                    //
                    // Census over the loaded models: 1039 pieces, of which only 61 are
                    // closed edge-manifold solids. 94% are OPEN single-sided sheets, and
                    // they hold 95.7% of the triangles. An open sheet has no back face to
                    // discard -- cull the one face it has and the shadow loses it
                    // outright. There is barely any redundancy here to reclaim.
                    //
                    // Measured by rasterising each unit's shadow with and without a cull
                    // and comparing coverage over ~3,000,000 real unit poses:
                    //
                    //   cull vs CAMERA dir (0,2,-1)  -> loses 9.8% of covered pixels,
                    //                                   96.4% of units affected
                    //   cull vs LIGHT  dir (-1,4,-1) -> loses 5.5%, 74.7% of units
                    //
                    // The light direction is the correct one for a shadow -- the
                    // projection collapses along it, since a displacement (a,b,c) leaves
                    // the shadow fixed iff a = -kShadowLX*b and c = -kShadowLZ*b -- and
                    // halving the damage confirms that. It is still a tear. A
                    // piece-aware cull (light-cull closed pieces, keep both faces on open
                    // ones) would be exactly correct, and is not worth building: it can
                    // only ever reach the 4.3% of triangles that sit in closed pieces.
                    //
                    // For the record, culling IS worth a lot in cost terms -- light-cull
                    // gives 709k -> 445k vertices, shadow 3.2ms -> 2.1ms, 54 -> 62 fps.
                    // The fidelity is what rules it out, not the speed.
                    //
                    // Opaque colour, not alpha: the level is a flat multiply and the
                    // batch is drawn with SDL_BLENDMODE_MOD. Retail's shadow has no
                    // internal structure -- sampling a retail shot gives 124/225,
                    // 106/192, 92/170, a flat multiply to 0.55 everywhere, which is
                    // what its alloca'd span buffer (0x4eda83, 0x5f08 bytes) buys.
                    // kShadowLevel is that 0.55.
                    // No per-vertex colour or texcoord written here: buildUnitShadow
                    // keeps POSITIONS ONLY and the flat grey is supplied once at
                    // stride 0 when the geometry is submitted. Writing them per vertex
                    // was filling ~8.5 MB a frame with the same two constants.
                    tri.tex = shadowMask;
                    if (shadowMask) {
                        static const SDL_FPoint uv[4] = {{0,0},{1,0},{1,1},{0,1}};
                        for (int k=0;k<3;++k) {
                            const auto point=uv[idx[k] & 3];
                            tri.v[k].tex_coord = {shadowUV.x+point.x*shadowUV.w,
                                                  shadowUV.y+point.y*shadowUV.h};
                            tri.v[k].color = {255,255,255,255};
                        }
                    }
                    tri.depth = 0;
                    out.push_back(tri);
                    continue;
                }
                // BACKFACE CULL. Retail culls -- it imports grCullMode -- and
                // without it both faces of a two-sided piece are drawn at almost
                // the same depth, so which one wins is arbitrary and a cape
                // comes out as a patchwork of its own front and back.
                //
                // Tested against the VIEW DIRECTION rather than a screen-space
                // winding sign, because the transform negates y and may mirror
                // x, and reasoning about the resulting handedness is how sign
                // errors get in. 3DO polygons are wound so (v1-v0)x(v2-v0)
                // points outward -- measured over the shipped models, 88-97% of
                // primitives agree -- and the camera lies along (y,z) = (2,-1),
                // the direction the projection collapses once collect()'s
                // negation of ry is taken into account (see kProjY/kProjZ).
                // Mirroring reflects the model and so flips the normal.
                {
                    const float ax = px[1] - px[0], ay = py[1] - py[0], az = pz[1] - pz[0];
                    const float bx = px[2] - px[0], by = py[2] - py[0], bz = pz[2] - pz[0];
                    const float ny = az * bx - ax * bz;      // normal Y
                    const float nz = ax * by - ay * bx;      // normal Z
                    float facing = ny * kSortY - nz * kSortZ;   // dot with (0,+2,-1)
                    if (mirror) facing = -facing;
                    if (facing <= 0.0f) continue;
                }
                if(paletteLighting && !name.empty()) {
                    auto lit=paletteTextureSheets_.find(name);
                    if(lit!=paletteTextureSheets_.end()) {
                        const size_t frame=animatedTex_.count(name)
                            ? modelTextureAnimations_.at(name).frame
                            : (size_t(playerColorSlot(player))<lit->second.size()
                                ? size_t(playerColorSlot(player)) : 0);
                        if(frame<lit->second.size() && lit->second[frame].texture) {
                            int level=15;
                            if(prepared ? prepared->shaded : !ps || ps->shaded) {
                                // Native shades the primitive from its first three
                                // vertices, not separately for each triangulated fan.
                                const auto& a=vcache[0];const auto& b=vcache[1];const auto& c=vcache[2];
                                const float ax=b.px-a.px,ay=b.py-a.py,az=b.pz-a.pz;
                                const float bx=c.px-a.px,by=c.py-a.py,bz=c.pz-a.pz;
                                float nx=ay*bz-az*by,ny=az*bx-ax*bz,nz=ax*by-ay*bx;
                                if(mirror) {nx=-nx;ny=-ny;nz=-nz;}
                                level=ta::retailModelShadeLevel(nx,ny,nz);
                            }
                            const auto& sheet=lit->second[frame];
                            tri.tex=sheet.texture;
                            static constexpr SDL_FPoint uv[4]={{0,0},{1,0},{1,1},{0,1}};
                            for(int k=0;k<3;++k) {
                                const auto c=uv[idx[k]&3];
                                tri.v[k].tex_coord={sheet.uv.x+(float((level-5)%4)+c.x)*sheet.uv.w,
                                    sheet.uv.y+(float((level-5)/4)+c.y)*sheet.uv.h};
                                // Indexed form: the sheet id as an exact integer offset
                                // (palettesheets.h); the program recovers both.
                                if(sheet.sheet!=ta::render::palette::kNoSheet)
                                    ta::render::palette::encodeSheet(tri.v[k].tex_coord.x,
                                        tri.v[k].tex_coord.y,sheet.sheet);
                            }
                        }
                    }
                }
                tri.depth = depth / 3;
                out.push_back(tri);
            }
        }
        meta = pieceMeta;   // the children choose for themselves
        for (size_t ci = 0; ci < o.children.size(); ++ci) {
            const PieceMeta* cm = (ci < meta->children.size()) ? &meta->children[ci] : nullptr;
            collect(out, atlas, o.children[ci], xf, anim, heading, player, mirror, false,
                    shadow, ext, cm, shadowCull, prepared ? &prepared->children[ci] : nullptr,paletteLighting,nano);
        }
    }

    // The disconnect wait on screen: one line per seat the server is waiting for, each
    // counting down to the AI takeover, and our own line while we reconnect. UI font,
    // centred on a dark panel, in the band below the status lines.
    void GameView::drawNetWaiting(int winW) {
        const uint64_t now = SDL_GetTicks64(), wall = ta::net::MpClient::clockMs();
        std::vector<std::string> lines;
        auto secs = [](uint64_t at, uint64_t t) { return at > t ? int((at - t + 999) / 1000) : 0; };
        if (reconnectUntilMs_)
            lines.push_back("CONNECTION LOST -- RECONNECTING  " + std::to_string(secs(reconnectUntilMs_, now)));
        else
            for (const auto& w : mp_->waiting()) {
                if (w.slot >= 8) continue;
                const std::string& name = playerName_[w.slot];
                lines.push_back("WAITING FOR " + (name.empty() ? "PLAYER " + std::to_string(w.slot + 1) : name) +
                                "  " + std::to_string(secs(w.untilMs, wall)));
            }
        const float px = 3.0f, padX = 14, padY = 10;
        float y = 200;
        for (const auto& line : lines) {
            const float tw = blockWidth(line, px), th = 7 * px + ta::uiTextInkDescent(line, px);
            const SDL_FRect bg{(float(winW) - tw) / 2 - padX, y, tw + 2 * padX, th + 2 * padY};
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 190);
            SDL_RenderFillRectF(ren_, &bg);
            SDL_SetRenderDrawColor(ren_, 255, 255, 255, 40);
            SDL_RenderDrawRectF(ren_, &bg);
            blockText(line, (float(winW) - tw) / 2, y + padY, px, {255, 214, 110, 255});
            y += bg.h + 8;
        }
    }

    float GameView::hudBanner(const std::string& msg, float y, float scale, SDL_Color col,
                    int winW, float leftX) {
        if (!hudFont_.ok() || msg.empty()) return y;
        float tw = float(hudFont_.width(msg, scale));
        float topOff, th;
        hudFont_.vbounds(msg, scale, topOff, th);   // real text extent (draw() lifts by yoff)
        float x = leftX < 0 ? (float(winW) - tw) / 2 : leftX;
        const float padX = 10, padTop = 6, padBot = 6;
        // `y` is the TOP of the banner box; the text is inset by padTop inside it. Offset
        // the draw origin so the text's true visual top lands at y+padTop -- so the box
        // wraps the text instead of drawing below it (draw() renders glyphs above `y`).
        float drawY = y + padTop - topOff;
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        SDL_FRect bg{x - padX, y, tw + 2 * padX, th + padTop + padBot};
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 180);
        SDL_RenderFillRectF(ren_, &bg);
        SDL_SetRenderDrawColor(ren_, 255, 255, 255, 40);   // faint hairline for definition
        SDL_RenderDrawRectF(ren_, &bg);
        hudFont_.draw(ren_, msg, x, drawY, scale, col);
        return y + th + padTop + padBot + 6;
    }

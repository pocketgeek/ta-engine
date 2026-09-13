#include "client/gameview.h"

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
        manageMusic();
        discoSound();     // fire the disco track from a monarch when its player starts dancing
        headbangSound();  // ...and the metal track on headbang
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
            return;
        }
        // Pull any chat that arrived and age the overlay on a real wall clock, so
        // it fades even while the game is paused or catching up on ticks.
        if (mp_) {
            for (auto& m : mp_->takeChat()) gameChat_.push_back({m.first, m.second, 0});
            uint64_t nowMs = SDL_GetTicks64();
            float cdt = chatLastMs_ ? (nowMs - chatLastMs_) / 1000.0f : 0;
            chatLastMs_ = nowMs;
            if (!chatTyping_) for (auto& g : gameChat_) g.age += cdt;
            if (gameChat_.size() > 16) gameChat_.erase(gameChat_.begin(), gameChat_.end() - 16);
        }
        // Camera shake: nudge the map offset by a decaying oscillation for this
        // frame, so the whole world jolts; the offset is restored at the end so
        // the camera and UI stay put. shakemagnitude ~3 => a few px of jolt.
        float shakeBaseX = mapView_.offX(), shakeBaseY = mapView_.offY();
        bool shaking = shakeTime_ > 0 && shakeDur_ > 0;
        if (shaking) {
            float decay = shakeTime_ / shakeDur_;
            float amp = shakeMag_ * 3.0f * decay;    // pixels
            float dx = amp * std::sin(animClock_ * 91.0f);
            float dz = amp * std::cos(animClock_ * 73.0f);
            float zm = std::max(mapView_.zoom(), 1e-3f);
            mapView_.setOffset(shakeBaseX + dx / zm, shakeBaseY + dz / zm);
        }
        // Everything world-space (map, units, effects, bars) is clipped to the map
        // viewport so it never bleeds under the right-hand panel.
        int mvw = mapViewW(winW);
        SDL_Rect worldClip{0, 0, mvw, winH};
        SDL_RenderSetClipRect(ren_, &worldClip);
        mapView_.setUnderlay(miniTex_);   // low-res gap filler (null until the overview bakes)
        mapView_.draw(mvw, winH);
        float zm0 = mapView_.zoom();

        // Painter list: features and units together, sorted by map z. Lives in a
        // member so its capacity survives across frames (it was the last per-frame
        // buffer here still heap-allocated fresh every draw).
        using Item = PaintItem;
        auto& items = paintItems_;
        items.clear();
        const auto& vis = frameVisibility();
        int vw = frameVisW();
        for (const auto& f : features_) {
            if (!f.tex) continue;   // burnt away to a stage with no art
            if (!world_.featureAliveAt(f.x, f.z)) continue;   // reclaimed away by a builder
            int cx = int(f.x) / 16, cz = int(f.z) / 16;
            if (!noFog_ && !vis.empty() && (cx < 0 || cz < 0 || cx >= vw ||
                                 vis[size_t(cz) * vw + cx] == 0))
                continue;   // unexplored
            // Cull on the LIFTED position (features lift onto the relief like units).
            float sx = (f.x - mapView_.offX()) * zm0 - terrainLiftX(f.x, f.z) * zm0;
            float sy = (f.z - mapView_.offY()) * zm0 - terrainLift(f.x, f.z) * zm0;
            if (sx < -200 || sy < -200 || sx > winW + 200 || sy > winH + 200) continue;
            // Mana deposit markers are flat ground decals a lodestone is built
            // on top of, so bias their sort key back to keep them painted
            // under the building rather than over it.
            float key = f.mana ? f.z - 24.0f : f.z;
            items.push_back({key, nullptr, &f, 0});
        }
        for (const UnitR* _up : front().live) {
            const UnitR& r = *_up;   // this tick's snapshot (front().live mirrors world_.units())
            if ((r.deadFor >= 4.0f && !r.corpsePhase) || r.embarked()) continue;
            if (r.corpsePhase && r.corpseFeat >= 0) {
                static const bool kCorpLog = tak::devEnv("TAK_BURNLOG") != nullptr;
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
            if (!unitType_.count(r.id)) continue;
            if (!noFog_ && !r.alliedToLocal && !cellVisibleR(r.x, r.z)) continue;
            // Frustum cull: only units whose anchor falls in (or just outside) the
            // map viewport are projected and drawn. The margin is generous and
            // asymmetric -- models extend well above their anchor, so a unit above
            // the top edge can still show its lower body. Cull on the LIFTED anchor
            // (where the unit is actually drawn), else a unit lifted onto the screen
            // from just below the edge on high ground vanishes.
            float sx = (r.x - mapView_.offX()) * zm0 - uLiftX(r) * zm0;
            float sy = (r.z - mapView_.offY()) * zm0 - uLiftY(r) * zm0;
            if (sx < -160 || sx > mvw + 160 || sy < -260 || sy > winH + 120) continue;
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
        }
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.layer != b.layer) return a.layer < b.layer;   // ground, then air
            return a.z < b.z;
        });

        // Project every visible unit's model in parallel before drawing. This is
        // the heavy per-frame CPU work (matrix-transforming each unit's model tree
        // and building its vertex buffer); the render thread then only submits the
        // finished geometry, one texture-batched draw call per unit. Without this
        // the whole frame is single-threaded and pegs one core at large unit counts.
        ++sprTick_;   // frame counter for sprite-page LRU (stamps SprPage.lastUse below)
        visUnits_.clear();
        geomIndex_.assign(front().units.size(), -1);   // id -> slot; -1 = not in view
                                                        // (front().units is id-indexed, sized to cover every live id)
        for (const auto& it : items)
            if (it.u && it.u->id >= 0 && size_t(it.u->id) < geomIndex_.size()) {
                geomIndex_[size_t(it.u->id)] = int(visUnits_.size());
                visUnits_.push_back(it.u);
            }
        if (geomPool_.size() < visUnits_.size()) geomPool_.resize(visUnits_.size());
        // Fraction through the current sim-tick interval, for motion interpolation this
        // frame (clamped: a late tick just holds at the newest pose until it arrives).
        interpAlpha_ = front().tickDurMs > 0.0f
            ? std::clamp(float(SDL_GetTicks64() - front().tickMs) / front().tickDurMs, 0.0f, 1.0f)
            : 1.0f;
        // Build the texture atlas for every colour slot in view (main thread; the
        // parallel pass below only reads the finished atlas pointers).
        bool builtGlow = false;
        uint32_t atlasSeen = 0;   // build each in-view colour slot's atlas ONCE, not per unit
        for (const auto* u : visUnits_) {
            int slot = colorSlot_[u->player & 7];
            uint32_t bit = (slot >= 0 && slot < 32) ? (1u << slot) : 0u;
            if (!bit || !(atlasSeen & bit)) { atlasFor(slot); atlasSeen |= bit; }
            if (!u->underConstruction)
                if (auto it = anims_.find(u->id);
                    it != anims_.end() && it->second.usesGlow)
                    builtGlow = true;
        }
        // Cycle lodestone/mana/fire crystal frames -- but only once a built glow-unit
        // is on screen, so a still-conjuring lodestone stays dark until it's finished.
        animateGlowTextures(builtGlow);
        // Ensure an impostor sprite exists for every visible model when zoomed out
        // enough that LOD may kick in (main thread; the parallel pass only reads it).
        // Budgeted: a few NEW bakes per frame, so a first zoom-out over a mixed army
        // spreads its render-target allocations/captures across frames instead of
        // bursting them all into one already-slow frame (units not yet baked just
        // draw as full models for a few more frames).
        if (lodEnabled_ && mapView_.zoom() < kLodZoomGate) {
            int budget = 2;
            for (const auto* u : visUnits_) {
                if (!u->type) continue;
                auto key = std::make_pair(unitType_.at(u->id), colorSlot_[u->player & 7]);
                if (impostors_.count(key)) continue;
                if (budget-- <= 0) break;
                ensureImpostor(key.first, key.second, u->type->canMove);
            }
        }
        // Bake sprite sheets for visible models when sprite mode is on -- one NEW
        // set per frame (each is kSprFacings x kSprFrames render-target captures).
        if (spritesEnabled_) {
            // Pass 1: mark every ON-SCREEN sprite set's page used THIS frame, so the LRU
            // in newSprPage() never evicts a page whose sprites are still visible.
            for (const auto* u : visUnits_) {
                if (!u->type) continue;
                auto sit = sprites_.find(std::make_pair(unitType_.at(u->id), colorSlot_[u->player & 7]));
                if (sit != sprites_.end() && sit->second.ready && sit->second.page)
                    for (auto& pg : sprPages_)
                        if (pg.tex == sit->second.page) { pg.lastUse = sprTick_; break; }
            }
            // Pass 2: bake ONE new set this frame (spread the cost); a bake may evict a
            // page NOT stamped above -- never a visible one, never the in-progress page.
            for (const auto* u : visUnits_) {
                if (!u->type) continue;
                auto key = std::make_pair(unitType_.at(u->id), colorSlot_[u->player & 7]);
                if (sprites_.count(key)) continue;
                bakeSprites(key.first, key.second, u->type->canMove, u->type->canFly);
                break;
            }
        }
        double _pt0 = double(SDL_GetPerformanceCounter());
        pool_.parallelFor(visUnits_.size(), [this](size_t b, size_t e) {
            thread_local std::vector<Tri> scratch;
            for (size_t i = b; i < e; ++i)
                buildUnitGeom(*visUnits_[i], geomPool_[i], scratch);
        });
        double _ptFreq = double(SDL_GetPerformanceFrequency()) / 1000.0;
        profProjMs_ += (double(SDL_GetPerformanceCounter()) - _pt0) / _ptFreq;
        // Tally impostor vs full-model draws this frame (for TAK_PROF).
        for (size_t _i = 0; _i < visUnits_.size(); ++_i) {
            const auto& _g = geomPool_[_i];
            if (_g.runs.empty()) continue;
            if (impAtlas_ && _g.runs[0].first == impAtlas_) ++lodDrawn_;
            else ++fullDrawn_;
        }
        double _st0 = double(SDL_GetPerformanceCounter());

        // Is this unit drawn whole by drawUnit (needs clip rects / interleaved
        // effects) rather than folded into the shared batches?
        auto special = [&](const UnitR& u, const UnitGeom& g) {
            bool occluded = !g.canFly && g.occY < g.ay - 2.0f;
            bool conjuring = u.underConstruction && u.type;
            // A worker (conjuring a site or reclaiming) routes through drawUnit too, so
            // the build/reclaim nano-sparkle -- drawn there over BOTH the worker and its
            // target -- shows for any builder/reclaimer, not just occluded/dancing ones.
            bool working = u.type && (u.buildSiteId != 0 || u.reclaimId != 0);
            return occluded || conjuring || working || dancing(u) || headbanging(u);
        };

        // Pass 1: every normal unit's ground shadows, batched. Soft blobs go into
        // one untextured triangle batch; FBI shadow sprites are batched per shadow
        // texture. Drawn first so all shadows sit under all bodies. (Special units
        // draw their own shadow inside drawUnit in pass 2.)
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        shadowBatch_.clear();
        shadowLo_ = {1e30f, 1e30f};
        shadowHi_ = {-1e30f, -1e30f};
        for (const auto& it : items) {
            if (!it.u) continue;
            const auto& u = *it.u;
            // PROJECTED SILHOUETTE. The unit's own triangles, flattened onto
            // the ground and leaned away from it by the light -- which is why a
            // shadow follows a Monarch's sword and cape instead of sitting under
            // him as a disc.
            //
            // Two earlier attempts are worth naming. A fixed 14x5 blob was our
            // own invention, the same size for every unit. The model's ground
            // plate (AraGP, a flat unit-sized quad at y=0) looked like a better
            // answer and is not one: it can only ever be a rectangle, and a
            // silhouette is not.
            if (u.alive() && !u.underConstruction && castsBlobShadow(u.type)) {
                auto vt = visuals_.find(u.type->id);
                // A unit whose BODY draws as an impostor must not walk its full
                // model for a shadow: that inverts the LOD, making the cheap
                // units the expensive ones. They are a few pixels across, where
                // the silhouette cannot read anyway -- the sprite shadow path
                // skipped them for the same reason, and retail has its own
                // shadow LOD in ShadowScale.
                int gsl = geomSlot(u.id);
                const bool impostor =
                    impAtlas_ && gsl >= 0 && !geomPool_[size_t(gsl)].runs.empty() &&
                    geomPool_[size_t(gsl)].runs[0].first == impAtlas_;
                if (vt != visuals_.end() && !impostor) {
                    const Anim* anim = nullptr;
                    if (auto at = anims_.find(u.id); at != anims_.end())
                        anim = &at->second;
                    const float facing =
                        (u.type->canMove || u.type->canFly) ? -u.heading : 0.0f;
                    shadowTris_.clear();
                    collect(shadowTris_, nullptr, vt->second.model.root, Xform{},
                            anim, facing, u.player, false, true, /*shadow=*/true);
                    // Anchor on the BODY, not on a re-derived ground point, and
                    // apply retail's delta. Both projections in 0x4ec250 start
                    // from the same vertex, so the shadow is always exactly
                    // (+y/4, +y/4) from the model in screen space -- and pinning
                    // it to the body is the only way to stay true to that through
                    // our own lift conventions. Re-deriving the ground point got
                    // this wrong for flyers: they are exempt from the terrain
                    // lift, so the shadow picked up a lift the body never had and
                    // the horizontal lean cancelled out to nothing.
                    int gs = geomSlot(u.id);
                    if (gs < 0) continue;
                    const UnitGeom& gsh = geomPool_[size_t(gs)];
                    const float alt = gsh.alt;
                    const float sx = gsh.ax + kShadowLX * alt * zm0;
                    // ay carries alt*kProjY of up-screen lift; the shadow wants
                    // only alt*kShadowLZ of it, so the difference comes back down.
                    const float sy = gsh.ay + (kProjY - kShadowLZ) * alt * zm0;
                    // Accumulate the composite bounds here rather than rescanning
                    // the whole vertex buffer afterwards.
                    for (const Tri& t : shadowTris_)
                        for (int k = 0; k < 3; ++k) {
                            SDL_Vertex v = t.v[k];
                            v.position = {sx + v.position.x * zm0,
                                          sy + v.position.y * zm0};
                            shadowLo_.x = std::min(shadowLo_.x, v.position.x);
                            shadowLo_.y = std::min(shadowLo_.y, v.position.y);
                            shadowHi_.x = std::max(shadowHi_.x, v.position.x);
                            shadowHi_.y = std::max(shadowHi_.y, v.position.y);
                            shadowBatch_.push_back(v);
                        }
                }
            }
        }
        // Composite the silhouettes through a COVERAGE MASK rather than drawing
        // them straight onto the scene. Drawn directly, every triangle blends
        // separately and the shadow darkens wherever the shape folds over itself
        // -- a wing across a body, a sword across a cape -- which is the thing
        // that read as wrong against retail. Retail rasterises the whole
        // silhouette into one span buffer and blends it once, so a shadowed pixel
        // is a flat 0.55 multiply no matter how many polygons covered it.
        //
        // White ground + grey coverage + one MOD blit reproduces exactly that.
        if (!shadowBatch_.empty()) {
            SDL_Texture* prev = SDL_GetRenderTarget(ren_);
            int mw = 0, mh = 0;
            if (prev) SDL_QueryTexture(prev, nullptr, nullptr, &mw, &mh);
            else SDL_GetRendererOutputSize(ren_, &mw, &mh);
            if (mw > 0 && mh > 0 &&
                (!shadowMask_ || shadowMaskW_ != mw || shadowMaskH_ != mh)) {
                if (shadowMask_) gpuvram::destroy(shadowMask_);
                shadowMask_ = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA8888,
                                              SDL_TEXTUREACCESS_TARGET, mw, mh);
                shadowMaskW_ = mw; shadowMaskH_ = mh;
                if (shadowMask_)
                    SDL_SetTextureBlendMode(shadowMask_, SDL_BLENDMODE_MOD);
            }
            if (shadowMask_) {
                // Only the batch's bounding box is touched. Clearing and blitting
                // a whole 7680x2160 target twice a frame for a few hundred small
                // silhouettes is most of the cost of this feature and none of the
                // benefit; shadows cluster wherever the units are.
                const float lo_x = shadowLo_.x, hi_x = shadowHi_.x;
                const float lo_y = shadowLo_.y, hi_y = shadowHi_.y;
                float rsx = 1.0f, rsy = 1.0f;
                SDL_RenderGetScale(ren_, &rsx, &rsy);
                SDL_Rect box{std::clamp(int(std::floor(lo_x * rsx)) - 1, 0, mw),
                             std::clamp(int(std::floor(lo_y * rsy)) - 1, 0, mh),
                             0, 0};
                box.w = std::clamp(int(std::ceil(hi_x * rsx)) + 1, 0, mw) - box.x;
                box.h = std::clamp(int(std::ceil(hi_y * rsy)) + 1, 0, mh) - box.y;
                if (box.w > 0 && box.h > 0) {
                    SDL_SetRenderTarget(ren_, shadowMask_);
                    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
                    SDL_SetRenderDrawColor(ren_, 255, 255, 255, 255);  // MOD identity
                    SDL_RenderSetScale(ren_, 1.0f, 1.0f);   // box is in pixels
                    SDL_RenderFillRect(ren_, &box);
                    SDL_RenderSetScale(ren_, rsx, rsy);
                    SDL_RenderGeometry(ren_, nullptr, shadowBatch_.data(),
                                       int(shadowBatch_.size()), nullptr, 0);
                    SDL_SetRenderTarget(ren_, prev);
                    // The mask holds pixel-space coverage (it was drawn under the
                    // renderer's own scale), so composite it 1:1.
                    SDL_RenderSetScale(ren_, 1.0f, 1.0f);
                    SDL_RenderCopy(ren_, shadowMask_, &box, &box);
                    SDL_RenderSetScale(ren_, rsx, rsy);
                }
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            } else {   // no render target available: straight draw, stacking and all
                SDL_RenderGeometry(ren_, nullptr, shadowBatch_.data(),
                                   int(shadowBatch_.size()), nullptr, 0);
            }
        }

        // Pass 2: bodies (feature sprites + unit models) in depth order. Unit
        // models are accumulated into one batch and flushed only when the texture
        // changes or a feature/special unit interrupts the run -- so a crowd of one
        // unit type collapses to a handful of draw calls instead of one per unit.
        // Plan the draw order serially (no vertex copies), scatter the vertex
        // copies across the worker pool, then replay the draws. This spreads the
        // ~1.5M-vertex body assembly that used to peg one core, while keeping the
        // exact painter order (segments broken by features / special units).
        copyTasks_.clear();
        drawOps_.clear();
        int destOff = 0;
        SDL_Texture* segTex = nullptr;
        int segStart = 0, segCount = 0;
        auto closeSeg = [&] {
            if (segCount > 0) {
                drawOps_.push_back({nullptr, nullptr, segTex, segStart, segCount});
                segCount = 0;
            }
            // Force the next run to re-anchor segStart to the current destOff. Without
            // this, a unit whose texture matches segTex but follows a feature/special
            // unit (which closed the segment) keeps the PREVIOUS segment's segStart and
            // gets drawn from the wrong vertices -- so it renders an earlier unit's body
            // and vanishes from its own spot (camera-dependent, as depth order shifts).
            segTex = nullptr;
        };
        for (const auto& it : items) {
            if (it.f) {
                closeSeg();
                drawOps_.push_back({nullptr, it.f, nullptr, 0, 0});
            } else {
                const auto& u = *it.u;
                int gslot = geomSlot(u.id);
                if (gslot < 0) continue;
                const UnitGeom& g = geomPool_[size_t(gslot)];
                if (special(u, g)) {
                    closeSeg();
                    drawOps_.push_back({&u, nullptr, nullptr, 0, 0});
                    continue;
                }
                int src = 0;
                for (const auto& r : g.runs) {
                    if (r.first != segTex) { closeSeg(); segTex = r.first; segStart = destOff; }
                    copyTasks_.push_back({gslot, src, r.second, destOff});
                    destOff += r.second; segCount += r.second; src += r.second;
                }
            }
        }
        closeSeg();
        bodyVerts_.resize(size_t(destOff));
        pool_.parallelFor(copyTasks_.size(), [this](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) {
                const CopyTask& t = copyTasks_[i];
                const auto& gv = geomPool_[size_t(t.geom)].verts;
                std::copy(gv.begin() + t.src, gv.begin() + t.src + t.count,
                          bodyVerts_.begin() + t.dst);
            }
        });
        for (const DrawOp& op : drawOps_) {
            if (op.f) {
                const auto& f = *op.f;
                // Lift the decal onto the terrain relief just like a unit, so a mana
                // deposit sits at the height its heightmap claims (and lodestones/units
                // built on it line up) instead of the decal being flat.
                float lfx = terrainLiftX(f.x, f.z) * zm0, lfy = terrainLift(f.x, f.z) * zm0;
                if (f.shadow) {
                    SDL_FRect sd{(f.x - mapView_.offX() - float(f.sxoff)) * zm0 - lfx,
                                 (f.z - mapView_.offY() - float(f.syoff)) * zm0 - lfy,
                                 float(f.sw) * zm0, float(f.sh) * zm0};
                    SDL_RenderCopyF(ren_, f.shadow, nullptr, &sd);
                }
                // Retail feature playback: one shared clock per TYPE (every
                // instance of a sequence shows the identical frame -- variety
                // comes from the 16 different wave sequences, not phase), 30Hz
                // engine ticks holding each frame for its GAF delay (waves ship
                // delay=2 -> 15fps), looping continuously. Each frame is drawn at
                // its OWN size + anchor -- the wave art authors motion that way.
                SDL_Texture* tex = f.tex;
                int fw = f.w, fh = f.h, fxo = f.xoff, fyo = f.yoff;
                // Burning: seqnameburn playback replaces the standing art
                // (retail swaps to the burn anim + flame overlays on ignition).
                if (f.burnVis && f.burnArt && f.burnArt->tex) {
                    const FeatArt* ba = f.burnArt;
                    tex = ba->tex; fw = ba->w; fh = ba->h; fxo = ba->xoff; fyo = ba->yoff;
                    if (ba->frames.size() > 1 && ba->totalTicks > 0) {
                        int tick = int(animClock_ * 30.0f) % ba->totalTicks;
                        size_t idx = size_t(std::upper_bound(ba->tickEnd.begin(),
                                                             ba->tickEnd.end(), tick) -
                                            ba->tickEnd.begin());
                        if (idx >= ba->frames.size()) idx = 0;
                        tex = ba->frames[idx];
                        const auto& g = ba->fgeom[idx];
                        fw = g.w; fh = g.h; fxo = g.xoff; fyo = g.yoff;
                    }
                    SDL_FRect dst{(f.x - mapView_.offX() - float(fxo)) * zm0 - lfx,
                                  (f.z - mapView_.offY() - float(fyo)) * zm0 - lfy,
                                  float(fw) * zm0, float(fh) * zm0};
                    SDL_RenderCopyF(ren_, tex, nullptr, &dst);
                    continue;
                }
                if (f.frames && f.frames->size() > 1 && f.art && f.art->totalTicks > 0) {
                    int tick = int(animClock_ * 30.0f) % f.art->totalTicks;
                    size_t idx = size_t(std::upper_bound(f.art->tickEnd.begin(),
                                                         f.art->tickEnd.end(), tick) -
                                        f.art->tickEnd.begin());
                    if (idx >= f.frames->size()) idx = 0;
                    tex = (*f.frames)[idx];
                    const auto& g = f.art->fgeom[idx];
                    fw = g.w; fh = g.h; fxo = g.xoff; fyo = g.yoff;
                }
                SDL_FRect dst{(f.x - mapView_.offX() - float(fxo)) * zm0 - lfx,
                              (f.z - mapView_.offY() - float(fyo)) * zm0 - lfy,
                              float(fw) * zm0, float(fh) * zm0};
                if (f.tree && settings_ && settings_->treeSway) {
                    // Wind sway (Options; beyond-retail -- retail trees are static
                    // single-frame GAFs): shear the crown sideways on two blended
                    // gust sines, pivoting at the trunk base (the GAF anchor sits
                    // there). Per-tree phase so a forest ripples instead of rocking
                    // in unison. Display-only; the sim never sees it.
                    float ph = f.x * 0.043f + f.z * 0.029f;
                    float sway = std::sin(animClock_ * 1.1f + ph) * 0.7f +
                                 std::sin(animClock_ * 2.7f + ph * 1.7f) * 0.3f;
                    float shear = sway * dst.h * 0.04f;
                    const SDL_Color wc{255, 255, 255, 255};
                    SDL_Vertex v[4] = {
                        {{dst.x + shear, dst.y}, wc, {0, 0}},
                        {{dst.x + dst.w + shear, dst.y}, wc, {1, 0}},
                        {{dst.x + dst.w, dst.y + dst.h}, wc, {1, 1}},
                        {{dst.x, dst.y + dst.h}, wc, {0, 1}},
                    };
                    static const int wIdx[6] = {0, 1, 2, 0, 2, 3};
                    if (tex) SDL_RenderGeometry(ren_, tex, v, 4, wIdx, 6);
                } else {
                    SDL_RenderCopyF(ren_, tex, nullptr, &dst);
                }
            } else if (op.u) {
                drawUnit(*op.u);
            } else if (op.count > 0 && op.tex) {   // null atlas page: skip, no white
                SDL_RenderGeometry(ren_, op.tex, bodyVerts_.data() + op.start,
                                   op.count, nullptr, 0);
            }
        }
        profSubmitMs_ += (double(SDL_GetPerformanceCounter()) - _st0) / _ptFreq;

        // Ghosts of the local player's queued (shift) build orders.
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (!u.alive() || u.player != localPlayer_) continue;
            // Every queued build is an order now; the order sits at the builder's
            // working position, so step back to where the site actually goes.
            for (const auto& o : u.orders)
                if (o.buildType)
                    drawGhostAt(o.buildType, o.x, o.z - float(o.buildType->footZ) * 8 - 24);
        }

        // Projectiles: drawn per weapon family (only where visible).
        float zm = mapView_.zoom();
        // Wandering storms. These are roaming hazards with their own three-part
        // animation (wanderstartart while it spins up, wanderloopart while it
        // roams); without it a Tornado or a god's vortex tears through an army
        // completely invisibly. Drawn before the shots so units read on top of it.
        for (const auto& st : front().storms) {
            if (!st.w) continue;
            if (!noFog_ && !cellVisibleR(st.x, st.z)) continue;
            bool spinUp = st.arm > 0.0f;
            const std::string& artName = spinUp && !st.w->wanderStart.empty()
                                             ? st.w->wanderStart : st.w->wanderLoop;
            if (artName.empty()) continue;
            const EffectAnim* ea = effectFor(artName);
            if (!ea || ea->frames.empty()) continue;
            // The loop cycles; the spin-up plays through once and holds its last
            // frame until the storm arms.
            float phase = spinUp ? (st.w->buildUp - st.arm) : st.left;
            size_t fi = size_t(std::max(0.0f, phase) * 20.0f);
            fi = spinUp ? std::min(fi, ea->frames.size() - 1) : fi % ea->frames.size();
            const auto& fr = ea->frames[fi];
            float sx = (st.x - mapView_.offX()) * zm - terrainLiftX(st.x, st.z) * zm;
            float sy = (st.z - mapView_.offY()) * zm - terrainLift(st.x, st.z) * zm;
            float fw = float(fr.w) * zm, fh = float(fr.h) * zm;
            // Anchored at the storm's FOOT: these sprites are tall columns whose
            // anchor sits near the base, so the funnel stands on the ground.
            SDL_FRect dst{sx - float(fr.ax) * zm, sy - float(fr.ay) * zm, fw, fh};
            SDL_RenderCopyF(ren_, fr.tex, nullptr, &dst);
        }
        // Hitscan flashes first, behind the shots. A Line-of-Sight weapon applies
        // its damage instantly and spawns no projectile, so this bolt IS the shot:
        // without it the King's Thunder, the Creon tasers and the Zhon lightning
        // all fire invisibly. Lightning zig-zags; other beams draw straight.
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_ADD);
        for (const auto& b : beams_) {
            // The bolt is drawn muzzle -> travelling head, so a long shot visibly
            // reaches out rather than appearing whole. Retail's colour ramp is flat,
            // so hold full strength and fade only over the last quarter -- an even
            // fade over a one-second bolt reads as a fizzle rather than a strike.
            float t = std::clamp(b.age / std::max(b.life, 1e-3f), 0.0f, 1.0f);
            if (t >= 1.0f) continue;
            float f = 1.0f - std::max(0.0f, (t - 0.75f) / 0.25f);
            if (!cellVisibleR(b.x2, b.z2) && !noFog_) continue;
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
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
        for (const auto& p : front().projectiles) {
            if (!cellVisibleR(p.x, p.z)) continue;
            float t = std::clamp(p.age / std::max(p.flight, 0.05f), 0.0f, 1.0f);
            // Flyer shots: lift the whole trajectory by the altitude interpolated
            // from the firing unit down to the target (0.8x, matching the sprite
            // lift), so a drake's breath leaves its mouth and arcs to the ground.
            float palt = (unitAltById(p.fromId) * (1 - t) + unitAltById(p.targetId) * t)
                         * 0.8f * zm;
            // Ground shadow (shadowgaf/shadowart, 218 weapons) and light pool
            // (lightmap, 36): both sit on the GROUND under the shot, not at its
            // altitude, which is what sells the shot as being up in the air.
            if (p.wsrc) {
                float gx = (p.x - mapView_.offX()) * zm - terrainLiftX(p.x, p.z) * zm;
                float gy = (p.z - mapView_.offY()) * zm - terrainLift(p.x, p.z) * zm;
                if (p.wsrc->lightMap > 0) {
                    // A soft additive pool tinted by the shot's own family, so a
                    // fireball throws warm light on the ground as it passes.
                    float r = (10.0f + 8.0f * float(p.wsrc->lightMap)) * zm;
                    Uint8 lr = 255, lg = 230, lb = 160;
                    if (p.fx == tak::sim::WeaponFx::Lightning) { lr = 190; lg = 215; lb = 255; }
                    else if (p.fx == tak::sim::WeaponFx::Fire) { lr = 255; lg = 160; lb = 70; }
                    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_ADD);
                    for (int ring = 3; ring >= 1; --ring) {
                        float rr = r * float(ring) / 3.0f;
                        SDL_SetRenderDrawColor(ren_, lr, lg, lb, Uint8(26));
                        SDL_FRect q{gx - rr, gy - rr * 0.5f, rr * 2, rr};
                        SDL_RenderFillRectF(ren_, &q);
                    }
                    SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
                }
                if (!p.wsrc->shadowArt.empty()) {
                    // shadowgaf is always "shadows"; effectFor's "file:sequence"
                    // form picks the named sequence out of it.
                    if (const EffectAnim* sh = effectFor("shadows:" + p.wsrc->shadowArt)) {
                        const auto& fr = sh->frames[size_t(int(p.age * 12.0f)) % sh->frames.size()];
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
            // A shot with a real mesh (arrows, spears, boulders) is drawn as that
            // mesh, yawed along its flight so an arrow actually points where it is
            // going. Models face -heading, like every other mover.
            if (p.wsrc && !p.wsrc->shotModel.empty()) {
                bool bal = p.wsrc->ballistic;
                float peak = bal ? std::min(95.0f, p.flight * 55.0f)
                                 : std::min(18.0f, p.flight * 12.0f);
                float h = 8 + 4 * peak * t * (1 - t);
                drawShotModel(p.wsrc->shotModel, p.fromPlayer, p.x, p.z,
                              h * zm + palt, -std::atan2(p.vx, p.vz));
                continue;
            }
            // Authored projectile art. Retail draws most shots as a real sprite
            // sequence (weaponart -> anims/<name>_4444.taf): cannonballs, fireballs,
            // iceballs, meteors, lightning balls. Ours drew every one of them as the
            // same yellow streak. Same loader the explosion/flame effects use.
            const std::string& shotSprite = p.wsrc ? (p.wsrc->weaponArt.empty()
                                                          ? p.wsrc->shotArt
                                                          : p.wsrc->weaponArt)
                                                   : std::string();
            if (p.wsrc && !shotSprite.empty()) {
                if (const EffectAnim* ea = effectFor(shotSprite)) {
                    // Ballistic shots arc; flat shots ride just above the ground.
                    bool bal = p.wsrc->ballistic;
                    float peak = bal ? std::min(95.0f, p.flight * 55.0f)
                                     : std::min(18.0f, p.flight * 12.0f);
                    float h = 8 + 4 * peak * t * (1 - t);
                    float sx = (p.x - mapView_.offX()) * zm - terrainLiftX(p.x, p.z) * zm;
                    float sy = (p.z - mapView_.offY()) * zm - h * zm
                               - terrainLift(p.x, p.z) * zm - palt;
                    const auto& fr = ea->frames[size_t(int(p.age * 20.0f)) % ea->frames.size()];
                    float fw = float(fr.w) * zm, fh = float(fr.h) * zm;
                    SDL_FRect dst{sx - fw * 0.5f, sy - fh * 0.5f, fw, fh};
                    // nimbus: an additive glow riding under the sprite.
                    if (p.wsrc->nimbus) {
                        SDL_SetTextureAlphaMod(fr.tex, 90);
                        SDL_FRect g{sx - fw, sy - fh, fw * 2, fh * 2};
                        SDL_RenderCopyF(ren_, fr.tex, nullptr, &g);
                        SDL_SetTextureAlphaMod(fr.tex, 255);
                    }
                    if (p.wsrc->spinRate != 0.0f)
                        SDL_RenderCopyExF(ren_, fr.tex, nullptr, &dst,
                                          double(p.age * p.wsrc->spinRate * 57.2957795f),
                                          nullptr, SDL_FLIP_NONE);
                    else
                        SDL_RenderCopyF(ren_, fr.tex, nullptr, &dst);
                    continue;
                }
            }
            if (p.fx == tak::sim::WeaponFx::Lightning) {
                // Flat, fast, jagged blue-white bolt from source toward target.
                float sx = (p.x - mapView_.offX()) * zm - terrainLiftX(p.x, p.z) * zm;
                float sy = (p.z - mapView_.offY()) * zm - 12 * zm - terrainLift(p.x, p.z) * zm
                           - palt;
                float len = 22.0f;
                float bx = -p.vx, bz = -p.vz;
                float bl = std::max(std::sqrt(bx * bx + bz * bz), 1e-3f);
                bx /= bl; bz /= bl;
                float px = sx, py = sy;
                SDL_SetRenderDrawColor(ren_, 210, 230, 255, 255);
                for (int s = 1; s <= 4; ++s) {
                    float d = len * zm * s / 4.0f;
                    float jitter = ((s * 1327 + int(p.age * 900)) % 7 - 3) * 1.6f * zm;
                    float nx = sx + bx * d - bz * jitter;
                    float ny = sy + bz * d + bx * jitter - 12 * zm * s / 4.0f;
                    SDL_RenderDrawLineF(ren_, px, py, nx, ny);
                    px = nx; py = ny;
                }
            } else if (p.fx == tak::sim::WeaponFx::Fire) {
                // Flame breath: a short stream of flickering orange/yellow puffs
                // trailing behind the leading tip, not a single fireball.
                float bx = -p.vx, bz = -p.vz;
                float bl = std::max(std::sqrt(bx * bx + bz * bz), 1e-3f);
                bx /= bl; bz /= bl;
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
                for (int s = 0; s < 5; ++s) {
                    float back = s * 6.0f;   // world px behind the tip
                    float wob = ((s * 811 + int(p.age * 1000)) % 5 - 2) * 2.0f;
                    float fx = p.x + bx * back - bz * wob;
                    float fz = p.z + bz * back + bx * wob;
                    float sx = (fx - mapView_.offX()) * zm - terrainLiftX(fx, fz) * zm;
                    float sy = (fz - mapView_.offY()) * zm - 12 * zm - terrainLift(fx, fz) * zm
                               - palt;
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
                float sx = (p.x - mapView_.offX()) * zm - terrainLiftX(p.x, p.z) * zm;
                float sy = (p.z - mapView_.offY()) * zm - h * zm - terrainLift(p.x, p.z) * zm
                           - palt;
                SDL_SetRenderDrawColor(ren_, 255, 235, 140, 255);
                SDL_RenderDrawLineF(ren_, sx, sy, sx - p.vx * 0.035f * zm,
                                    sy - p.vz * 0.035f * zm + (t < 0.5f ? 2.5f : -2.5f) * zm);
            }
        }
        drawParticles();
        drawEffects();
        drawUnitFx();

        drawFog();
        if (buildDrag_ && placing_) {
            float mx, mz;
            pickWorld(mouseX_, mouseY_, mx, mz);
            for (auto& [x, z] : buildLinePositions(bdX0_, bdZ0_, mx, mz))
                drawGhostAt(placing_, x, z, !canPlaceLocked(placing_, x, z));
        } else if (placing_) {
            drawGhost();
        }
        if (reclaimDrag_ && (std::abs(mouseX_ - rdSx0_) >= 6.0f ||
                             std::abs(mouseY_ - rdSy0_) >= 6.0f)) {
            // Screen-space "clear this area" box, with a marker on every reclaimable
            // feature it currently catches. Same 6px arming threshold as the
            // marquee: a plain right-click order shouldn't flash the box.
            float zm = mapView_.zoom();
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            float x0 = std::min(rdSx0_, mouseX_), y0 = std::min(rdSy0_, mouseY_);
            float x1 = std::max(rdSx0_, mouseX_), y1 = std::max(rdSy0_, mouseY_);
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
            for (const auto& f : world_.features()) {
                if (!f.alive || f.x < minx || f.x > maxx || f.z < minz || f.z > maxz) continue;
                float fsx = (f.x - mapView_.offX()) * zm - terrainLiftX(f.x, f.z) * zm;
                float fsy = (f.z - mapView_.offY()) * zm - terrainLift(f.x, f.z) * zm;
                SDL_FRect m{fsx - 4, fsy - 4, 8, 8};
                SDL_RenderDrawRectF(ren_, &m);
            }
            // Corpses / statues / rubble get the same box markers.
            for (const UnitR* _cp : front().live) {
                const UnitR& cu = *_cp;
                if (cu.alive() || !cu.corpsePhase || cu.corpseFeat < 0 || !cu.type) continue;
                if (size_t(cu.corpseFeat) >= world_.featureTypes().size() ||
                    !world_.featureTypes()[size_t(cu.corpseFeat)].reclaimable)
                    continue;
                if (cu.x < minx || cu.x > maxx || cu.z < minz || cu.z > maxz) continue;
                float fsx = (cu.x - mapView_.offX()) * zm - terrainLiftX(cu.x, cu.z) * zm;
                float fsy = (cu.z - mapView_.offY()) * zm - terrainLift(cu.x, cu.z) * zm;
                SDL_FRect m{fsx - 4, fsy - 4, 8, 8};
                SDL_RenderDrawRectF(ren_, &m);
            }
        }

        // Selection membership as a hash set: the old code did frameUnitP(id) (a
        // linear scan) per selected unit and std::find(selection_) per world unit
        // -- both O(n^2) once a big army was selected, which tanked the frame.
        selSet_.clear();
        selSet_.insert(selection_.begin(), selection_.end());
        // Selection rings: iterate units once, batched into a single draw
        // (viewport-culled, thin quads).
        shadowBatch_.clear();
        if (!selSet_.empty()) {
            float zms = mapView_.zoom();
            // Retail's selection indicator (0x4fd0d0) is not a bracket or a sprite --
            // it is TWO counter-rotating dashed ellipses on the ground plane, at the
            // model's mid height, coloured by health. Straight from the binary:
            //   * angle = ((perUnitPhase + tick) << 14) / 30 BAM, so 16384/30 BAM a
            //     tick = 120 ticks = 4s per revolution. One ring runs +A, the other
            //     -A, hence counter-rotating.
            //   * 6 dashes per ring at 0x2aaa BAM = 60.0 degrees apart, each dash an
            //     0xccc BAM = 18.0 degree chord (retail computes 2 points per dash and
            //     joins them, so they are chords, not arcs). 12 dashes, 24 points.
            //   * radii = 1.2 (the double at 0x5f03f0) times the model bbox's x and z
            //     half-extents; centre y = (min.y+max.y)/2.
            // The ring does NOT turn with the unit: [unit+2] is a per-unit phase added
            // to the tick, so neighbouring units are out of step with each other, but
            // each ring stays axis-aligned in world space.
            // The ring floats at the model's mid height, so it uses the same
            // vertical projection the model does: retail's half (icd 0x421dad),
            // not the cosine of a tilt we invented.
            const float ct = kProjY;
            const float spin = float(SDL_GetTicks64() % 4000) / 4000.0f;   // 1 rev / 4s
            // Iterate the (few) selected ids, not the whole world -- frameUnitP(id)
            // is O(1). (A duplicate id would just redraw the same brackets in place.)
            for (int selId : selection_) {
                const UnitR* up = frameUnitP(selId);
                if (!up || !up->alive() || !up->type) continue;
                const UnitR& u = *up;
                // unitScreen puts the model's origin on screen (the +12 undoes its
                // body bias, as the click test does) and already carries terrain lift
                // and flyer altitude, so the ring tracks a unit up a cliff or in the
                // air. Sizing comes from the MODEL bbox rather than unitHitBox, whose
                // bounds are already projected and so cannot say how deep a unit is.
                SDL_FPoint hp = unitScreen(u);
                const RingBox& rb = unitRingBox(u.type);
                // The model origin on screen; +y in model space reads UP, so the ring
                // floating at mid height sits midY*cos(tilt) above it.
                float cx = hp.x, cy = hp.y + 12.0f * zms - rb.midY * ct * zms;
                // Ground-plane offsets are NOT foreshortened: the terrain is a flat
                // tile mosaic where world z maps to screen y 1:1 (pickWorld inverts
                // exactly that), and retail's own projection is screenY = z - y/2 --
                // also 1:1 in z. Only HEIGHT is squashed, by cos(tilt), which is what
                // lifts the ring to mid height above.
                float rx = 1.2f * rb.halfX * zms;
                float ry = 1.2f * rb.halfZ * zms;
                // Cull against the RING, not its centre: a big unit just off the edge
                // still has half a ring inside the viewport.
                if (cx + rx < 0 || cx - rx > mvw || cy + ry < 0 || cy - ry > winH) continue;
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
                if (rx < 6.0f || ry < 2.0f) {
                    // Tiny on screen (a whole army zoomed out): the dashes would be
                    // sub-pixel, so drop to one marker quad -- 12x less geometry.
                    float s = std::max(2.0f, rx * 0.6f);
                    pushQuad(shadowBatch_, cx - s, cy - s * 0.65f, 2 * s, 2 * s * 0.65f, col);
                    continue;
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
                        pushSeg(shadowBatch_,
                                cx + ox + rx * std::cos(t0), cy - ry * std::sin(t0),
                                cx + ox + rx * std::cos(t1), cy - ry * std::sin(t1),
                                th, col);
                    }
                }
                // (Order markers are drawn by drawOrderTrails -- retail puts the
                // order kind's own animated CURSOR at each waypoint, not a ring.)
            }
            drawOrderTrails(mvw, winH);

#ifndef NDEBUG
            // TAK_HITBOX=1: outline the box unitUnderCursor actually tests, so a
            // "I can't click its head" report can be measured instead of guessed at.
            if (tak::devEnv("TAK_HITBOX")) {
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

            // Attack-target indicator: RED brackets on any enemy a selected unit is
            // ordered to attack, so you can see what you've told them to hit.
            const SDL_Color red{245, 70, 60, 255};
            auto& targets = targetSet_;   // member: reused across frames
            targets.clear();
            for (int sid : selection_)
                if (const auto* su = frameUnitP(sid))
                    for (const auto& o : su->orders)
                        if (o.targetId > 0) targets.insert(o.targetId);
            for (int tid : targets) {
                const auto* t = frameUnitP(tid);
                if (!t || !t->alive() || !t->type) continue;
                SDL_FPoint p = unitScreen(frameUnit(tid));   // includes flyer altitude
                float cx = p.x, cy = p.y + 12.0f * zms;   // undo unitScreen's body bias
                if (cx < -40 || cx > mvw + 40 || cy < -40 || cy > winH + 40) continue;
                // Match the green selection brackets: sized to the target's footprint.
                float halfX = std::max(std::max(t->type->footX, 1) * 8.0f, 11.0f);
                float halfZ = std::max(std::max(t->type->footZ, 1) * 8.0f, 8.0f);
                float rx = halfX * zms, ry = halfZ * zms;
                float Lx = std::max(3.0f, rx * 0.4f), Ly = std::max(3.0f, ry * 0.4f);
                float th = std::max(1.0f, 1.2f * zms);
                for (int sx = -1; sx <= 1; sx += 2)
                    for (int sy = -1; sy <= 1; sy += 2) {
                        float px = cx + sx * rx, py = cy + sy * ry;
                        pushQuad(shadowBatch_, std::min(px, px - sx * Lx), py - th * 0.5f,
                                 Lx, th, red);
                        pushQuad(shadowBatch_, px - th * 0.5f,
                                 std::min(py, py - sy * Ly), th, Ly, red);
                    }
            }
            if (!shadowBatch_.empty()) {
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
                SDL_RenderGeometry(ren_, nullptr, shadowBatch_.data(),
                                   int(shadowBatch_.size()), nullptr, 0);
            }
        }

        // Health bars for damaged or selected units -- viewport-culled and batched
        // into one draw call (each was two state-changing FillRects, so a damaged
        // crowd used to break the render batch thousands of times a frame).
        shadowBatch_.clear();
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (!u.alive() || u.embarked() || !u.type) continue;
            if (u.underConstruction && !u.buildBegun) continue;   // ghost: no bar
            if (!alliedToLocal(u.player) && !cellVisibleR(u.x, u.z)) continue;
            float frac = std::clamp(u.hp / u.type->maxHp, 0.0f, 1.0f);
            // Options "HEALTH BARS": 0 = never, 1 = only damaged units (default,
            // the retail behaviour), 2 = every unit, full HP included.
            if (healthBars_ == 0) break;
            if (healthBars_ == 1 && frac >= 1.0f) continue;
            float bw = 26 * zm, bh = std::max(2.0f, 3 * zm);
            float bx = (u.x - mapView_.offX()) * zm - bw / 2 - uLiftX(u) * zm;
            float by = (u.z - mapView_.offY()) * zm - 30 * zm - uLiftY(u) * zm;
            if (bx < -40 || bx > mvw + 40 || by < -40 || by > winH + 40) continue;
            pushQuad(shadowBatch_, bx - 1, by - 1, bw + 2, bh + 2,
                     SDL_Color{10, 10, 10, 220});
            pushQuad(shadowBatch_, bx, by, bw * frac, bh,
                     SDL_Color{uint8_t(230 * (1 - frac) + 40 * frac),
                               uint8_t(200 * frac + 40 * (1 - frac)), 40, 255});
        }
        if (!shadowBatch_.empty()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(ren_, nullptr, shadowBatch_.data(),
                               int(shadowBatch_.size()), nullptr, 0);
        }

        // Self-destruct countdown: a pulsing red number over each armed unit.
        if (hudFont_.ok())
            for (const UnitR* _up : front().live) {
                const UnitR& u = *_up;
                if (!u.alive() || u.embarked() || !u.type || u.selfDestructT < 0) continue;
                if (!alliedToLocal(u.player) && !cellVisibleR(u.x, u.z)) continue;
                float sx = (u.x - mapView_.offX()) * zm - uLiftX(u) * zm;
                float sy = (u.z - mapView_.offY()) * zm - 44 * zm - uLiftY(u) * zm;
                if (sx < -20 || sx > mvw + 20 || sy < -20 || sy > winH + 20) continue;
                char b[8];
                std::snprintf(b, sizeof b, "%d", int(std::ceil(u.selfDestructT)));
                bool bright = (u.selfDestructT - std::floor(u.selfDestructT)) > 0.5f;
                SDL_Color c{255, uint8_t(bright ? 200 : 60), 40, 255};
                hudFont_.draw(ren_, b, sx - 5, sy, 2.2f, c);
            }

        // Control-squad marker: retail-style bright-green number centred BELOW the
        // unit (under where the selection ring/bar sits), "<N>F" for a formation.
        // The number is the recall key (squad 10 shows as "0"). Skipped when zoomed
        // far out so it doesn't clutter the field.
        if (hudFont_.ok() && zm > 0.55f)
            for (const UnitR* _up : front().live) {
                const UnitR& u = *_up;
                if (!u.alive() || u.embarked() || !u.type || u.player != localPlayer_ ||
                    u.squad == 0)
                    continue;
                if (u.underConstruction && !u.buildBegun) continue;
                int num = std::abs(int(u.squad));
                char key = (num == 10) ? '0' : char('0' + num);
                std::string lbl(1, key);
                if (u.squad < 0) lbl += 'F';   // formation
                float cx = (u.x - mapView_.offX()) * zm - uLiftX(u) * zm;
                float cy = (u.z - mapView_.offY()) * zm - uLiftY(u) * zm
                           + (float(std::max(u.type->footZ, 1)) * 8.0f + 7.0f) * zm;
                if (cx < -20 || cx > mvw + 20 || cy < -30 || cy > winH + 30) continue;
                float sc = std::clamp(1.1f * zm, 0.9f, 1.7f);
                float tw = float(hudFont_.width(lbl, sc));
                hudFont_.draw(ren_, lbl, cx - tw / 2, cy, sc,
                              SDL_Color{70, 240, 90, 255});   // the selection-UI green
            }

        // Production progress above busy buildings -- viewport-culled and batched
        // into one draw call, same treatment as the health bars above (the two
        // state-changing FillRects per building broke the render batch each time,
        // and map-wide AI production drew bars at off-screen coordinates).
        shadowBatch_.clear();
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (!u.alive() || u.buildQueue.empty() || !u.type) continue;
            if (!alliedToLocal(u.player) && !cellVisibleR(u.x, u.z)) continue;
            float total = u.buildQueue.front()->buildTime /
                          std::max(u.type->workerTime, 0.01f);
            float frac = std::clamp(u.buildProgress / total, 0.0f, 1.0f);
            float bw = 40 * zm, bh = std::max(3.0f, 4 * zm);
            float bx = (u.x - mapView_.offX()) * zm - bw / 2 - uLiftX(u) * zm;
            float by = (u.z - mapView_.offY()) * zm - float(u.type->footZ) * 8 * zm - 14 * zm
                       - uLiftY(u) * zm;
            if (bx < -60 || bx > mvw + 60 || by < -60 || by > winH + 60) continue;
            pushQuad(shadowBatch_, bx - 1, by - 1, bw + 2, bh + 2,
                     SDL_Color{10, 10, 10, 220});
            pushQuad(shadowBatch_, bx, by, bw * frac, bh,
                     SDL_Color{90, 170, 255, 255});
        }
        if (!shadowBatch_.empty()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(ren_, nullptr, shadowBatch_.data(),
                               int(shadowBatch_.size()), nullptr, 0);
        }

        // Player mana bar top left (legacy; only without the bottom bar). A spectator
        // isn't a player -- no personal mana readout.
        if (!panelTex_ && !spectating_) {
            const PlayerR& tm = framePlayer(localPlayer_);
            float cap = std::max(tm.storage, 100.0f);
            SDL_FRect bg{10, 10, 180, 12};
            SDL_SetRenderDrawColor(ren_, 20, 20, 30, 230);
            SDL_RenderFillRectF(ren_, &bg);
            SDL_FRect fg{12, 12, 176 * std::clamp(tm.mana / cap, 0.0f, 1.0f), 8};
            SDL_SetRenderDrawColor(ren_, 80, 200, 255, 255);
            SDL_RenderFillRectF(ren_, &fg);
            if (hudFont_.ok() && !panelTex_) {
                char buf[96];
                std::snprintf(buf, sizeof buf, "MANA %d/%d  +%d", int(tm.mana), int(cap),
                              int(tm.income));
                hudFont_.draw(ren_, buf, 198, 21, 1.5f, {170, 225, 255, 255});
                if (!selection_.empty()) {
                    const auto* u = frameUnitP(selection_.front());
                    if (u && u->alive() && u->type) {
                        std::snprintf(buf, sizeof buf, "%s  %d/%d", u->type->name.c_str(),
                                      int(u->hp), int(u->type->maxHp));
                        hudFont_.draw(ren_, buf, 12, 40, 1.5f, {220, 220, 190, 255});
                        if (u->type->isBuilder) {
                            const auto menu = conjureMenu(u->type->id);   // mission-filtered
                            std::string m;
                            for (size_t i = 0; i < menu.size() && i < 6; ++i) {
                                const auto* bt = registry_.find(menu[i]);
                                m += std::to_string(i + 1) + ":" +
                                     (bt ? bt->name : menu[i]) + "  ";
                            }
                            if (!m.empty())
                                hudFont_.draw(ren_, m, 12, 58, 1.5f, {180, 200, 170, 255});
                            if (!u->buildQueue.empty()) {
                                std::snprintf(buf, sizeof buf, "TRAINING %s (%zu queued)",
                                              u->buildQueue.front()->name.c_str(),
                                              u->buildQueue.size());
                                hudFont_.draw(ren_, buf, 12, 76, 1.5f, {150, 200, 255, 255});
                            }
                        }
                    }
                }
            }
        }

        // Restore the un-shaken camera so the HUD/panel stays rock-steady.
        if (shaking) mapView_.setOffset(shakeBaseX, shakeBaseY);

        // Done with world-space: drop the clip and draw the right-hand panel and
        // its minimap + order column on a solid strip (never over the map).
        SDL_RenderSetClipRect(ren_, nullptr);
        // Neutral panel strip behind the minimap (kept for spectators too -- it's plain
        // chrome, not the faction art). The command panel + mana bulb + conjure menu
        // (renderGui) and the bottom InfoPanel chrome (drawPanel) are PLAYER-only: a
        // spectator isn't a player and gets neither, just the minimap and the F4 board.
        SDL_SetRenderDrawColor(ren_, 16, 14, 12, 255);
        // The strip normally stops short to leave room for the player's bottom
        // InfoPanel bar; a spectator has no bottom bar, so it must reach the
        // bottom edge (else a barH()-tall gap shows under the right panel).
        float stripH = spectating_ ? float(winH) : float(winH) - barH();
        SDL_FRect panelStrip{float(mvw), 0, float(winW - mvw), stripH};
        SDL_RenderFillRectF(ren_, &panelStrip);
        drawMinimap(winW, winH);
        if (!spectating_) {
            renderGui(winW, winH);
            drawPanel(winW, winH);
            drawObjectivesPanel(winW, winH);
        }
        if (showCounts_) drawUnitCounts(winW);
        if (showHDebug_) drawHDebug();

        // Mission briefing (first 30s) and event notices.
        if (briefTimer_ > 0 && hudFont_.ok()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_FRect bg{float(winW) - 560, 8, 552,
                         14.0f + 16.0f * float(briefing_.size())};
            SDL_SetRenderDrawColor(ren_, 10, 10, 20, 170);
            SDL_RenderFillRectF(ren_, &bg);
            float y = 24;
            for (const auto& l : briefing_) {
                hudFont_.draw(ren_, l, bg.x + 8, y, 1.4f, {220, 215, 180, 255});
                y += 16;
            }
        }
        if (mp_ && hudFont_.ok()) {
            // Dev net-status readout (TAK_NETDEBUG): off by default -- it sat over the
            // bottom-right mana panel. The BEHIND-BY lag warning below always shows.
            static const bool kNetDebug = tak::devEnv("TAK_NETDEBUG") != nullptr;
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
            float behindSec = float(mp_->bufferedBundles()) / float(tak::net::kServerHz);
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
        if (pendingCmd_ && hudFont_.ok()) {
            const char* msg = pendingCmd_ == 'a'   ? "ATTACK: CLICK TARGET"
                              : pendingCmd_ == 'f' ? "FIGHT-MOVE: CLICK DESTINATION"
                              : pendingCmd_ == 'p' ? "PATROL: CLICK WAYPOINT"
                              : pendingCmd_ == 'g' ? "GUARD: CLICK FRIENDLY UNIT"
                              : pendingCmd_ == 'c' ? "RECLAIM: CLICK FEATURE"
                              : pendingCmd_ == 'r' ? "REPAIR: CLICK DAMAGED UNIT"
                              : pendingCmd_ == 'l' ? "LOAD: CLICK UNIT TO CARRY"
                              : pendingCmd_ == 'u' ? "UNLOAD: CLICK DESTINATION"
                                                   : "MOVE: CLICK DESTINATION";
            hudBanner(msg, 100, 1.6f, {255, 200, 120, 255}, winW, 12);
        }
        if (noticeTimer_ > 0 && hudFont_.ok() && !notice_.empty())
            hudBanner(notice_, 120, 2.5f, {255, 230, 120, 255}, winW);
        if (paused_ && bigFont_.ok()) {
            const char* msg = "PAUSED";
            float tw = float(bigFont_.width(msg, 1.2f));
            bigFont_.draw(ren_, msg, (winW - tw) / 2, 60, 1.2f, {255, 230, 120, 255});
        }

        // Victory / defeat banner. A spectator/replay viewer isn't a participant,
        // so it names the winner instead of "VICTORY"/"DEFEAT".
        if (outcome_ != 0 && bigFont_.ok()) {
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
            const char* hint = "PRESS ESC FOR MENU";
            blockText(hint, (winW - blockWidth(hint, 2.0f)) / 2, float(winH) / 2 + 74, 2.0f,
                      {220, 220, 230, 255});
        }

        // Marquee only once the drag covers real area: under the same 6px
        // threshold the release is treated as a CLICK-select, so a couple of
        // pixels of hand jitter shouldn't flash a box.
        if (dragging_ && (std::abs(dragX1_ - dragX0_) >= 6.0f ||
                          std::abs(dragY1_ - dragY0_) >= 6.0f)) {
            SDL_FRect r{std::min(dragX0_, dragX1_), std::min(dragY0_, dragY1_),
                        std::abs(dragX1_ - dragX0_), std::abs(dragY1_ - dragY0_)};
            SDL_SetRenderDrawColor(ren_, 120, 255, 150, 200);
            SDL_RenderDrawRectF(ren_, &r);
        }

        // Spectator badge: a live watcher can pan/zoom but issues no orders. Use the
        // crisp 5x7 block font (the scaled GAF hudFont smeared/overlapped here).
        if (spectating_) {
            std::string m = "SPECTATING";
            if (benchmarkMode_) {   // append a countdown from the benchmark duration to 0
                uint32_t end = world_.benchmarkEndTick(), gt = front().gameTick;
                int rem = (end == 0) ? int(end) : (gt < end ? int((end - gt + 29) / 30) : 0);
                if (end == 0) rem = 60;   // not started yet -> full duration
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

        // Replay scrubber: elapsed/total time and a progress bar above the HUD.
        if (replayMode_ && hudFont_.ok()) {
            int cur = int(replayTick_) / 30, tot = int(replayLength()) / 30;
            char sb[64];
            std::snprintf(sb, sizeof sb, "REPLAY  %d:%02d / %d:%02d%s",
                          cur / 60, cur % 60, tot / 60, tot % 60,
                          paused_ ? "  PAUSED" : "");
            float bw = 360, bx = (winW - bw) / 2, by = float(winH) - 118;
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_FRect bg{bx - 8, by - 22, bw + 16, 44};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 150);
            SDL_RenderFillRectF(ren_, &bg);
            SDL_FRect track{bx, by + 6, bw, 6};
            SDL_SetRenderDrawColor(ren_, 90, 95, 110, 220);
            SDL_RenderFillRectF(ren_, &track);
            float frac = replayLength() ? float(replayTick_) / float(replayLength()) : 0;
            SDL_FRect fill{bx, by + 6, bw * frac, 6};
            SDL_SetRenderDrawColor(ren_, 235, 205, 110, 255);
            SDL_RenderFillRectF(ren_, &fill);
            hudFont_.draw(ren_, sb, bx, by - 14, 1.6f, {235, 230, 210, 255});
        }

        // In-game chat: recent lines bottom-left, plus a composer while typing.
        if (mp_ && hudFont_.ok() && (chatTyping_ || !gameChat_.empty())) {
            const float kFade = 10.0f;   // seconds a line stays up when not typing
            // Gather what will be drawn (bottom-up) so we can back it with one
            // translucent strip -- readable over bright terrain.
            std::vector<std::pair<std::string, SDL_Color>> lines;
            if (chatTyping_)
                lines.push_back({"SAY> " + chatDraft_ + "_", {255, 245, 180, 255}});
            int shown = 0;
            for (auto it = gameChat_.rbegin(); it != gameChat_.rend() && shown < 6; ++it) {
                if (!chatTyping_ && it->age > kFade) continue;
                lines.push_back({it->who + ": " + it->text, {225, 228, 236, 255}});
                ++shown;
            }
            float x = 14, y = float(winH) - barH() - 14;
            float wMax = 0;
            for (auto& l : lines) wMax = std::max(wMax, float(hudFont_.width(l.first, 1.7f)));
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_FRect bg{x - 8, y - float(lines.size()) * 21 + 4,
                         std::min(wMax + 16, float(winW) - x), float(lines.size()) * 21 + 6};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 140);
            SDL_RenderFillRectF(ren_, &bg);
            for (auto& l : lines) {           // lines[0] is the composer / newest
                float sc = (&l == &lines.front() && chatTyping_) ? 1.8f : 1.6f;
                hudFont_.draw(ren_, l.first, x, y, sc, l.second);
                y -= 21;
            }
        }

        // In-game exit menu overlay -- drawn last so it sits above the HUD. Screen
        // space (like the game-over banner); hit-rects are rebuilt here each frame
        // and consumed by input() (see the exitMenu_ branch).
        if (exitMenu_) {
            exitHots_.clear();
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_FRect dim{0, 0, float(winW), float(winH)};
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 150);
            SDL_RenderFillRectF(ren_, &dim);

            const float bw = 280, bh = 50, gap = 14, pad = 32, titlePx = 3.0f;
            const int nBtn = canReturnToMenu_ ? 5 : 4;
            const float titleH = 7 * titlePx + 24;
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
            btn("RESUME", [this] { exitMenu_ = false; });
            btn("OPTIONS", [this] { exitMenu_ = false; openOptions(); });
            btn("CONTROLS", [this] { exitMenu_ = false; openHotkeys(); });   // hotkey rebinding
            if (canReturnToMenu_) btn("MAIN MENU", [this] { menuRequested_ = true; });
            btn("QUIT", [this] { quitRequested_ = true; });
        }
        drawUnitInfo(winW, winH);   // retail Unit Info dialog, above the HUD
        if (benchStatsShown_) renderBenchmarkStats(winW, winH);   // benchmark results, above the frozen game
        if (options_) options_->render(winW, winH);   // topmost of all
        if (hotkeysScreen_) hotkeysScreen_->render(winW, winH);   // above Options
    }

    const tak::tdo::Model* GameView::ghostModel(const std::string& typeId) {
        auto it = visuals_.find(typeId);
        if (it != visuals_.end()) return &it->second.model;
        try {
            visuals_[typeId] = {tak::tdo::load(vread("objects3d/" + typeId + ".3do"))};
            return &visuals_[typeId].model;
        } catch (const std::exception&) {}
        return nullptr;
    }

    void GameView::animateGlowTextures(bool live) {
        AaScaleReset _sr(ren_);   // bakes render at 1:1 even when whole-frame AA is on
        if (animatedTex_.empty()) return;
        int frame = live ? int(animClock_ * 4.0f) : 0;   // ~4 fps
        SDL_Texture* prev = SDL_GetRenderTarget(ren_);
        bool onAny = false;
        for (SDL_Texture* atlas : atlasTex_) {
            if (!atlas) continue;
            SDL_SetRenderTarget(ren_, atlas);
            onAny = true;
            for (const auto& name : animatedTex_) {
                auto rit = atlasRect_.find(name);
                auto tit = textures_.find(name);
                if (rit == atlasRect_.end() || tit == textures_.end() ||
                    tit->second.size() < 2)
                    continue;
                SDL_Texture* f = tit->second[size_t(frame) % tit->second.size()];
                SDL_Rect r = rit->second;
                SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
                SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
                SDL_RenderFillRect(ren_, &r);          // clear the region (transparent)
                SDL_BlendMode fb;
                SDL_GetTextureBlendMode(f, &fb);
                SDL_SetTextureBlendMode(f, SDL_BLENDMODE_NONE);
                SDL_RenderCopy(ren_, f, nullptr, &r);  // overwrite with this frame's RGBA
                SDL_SetTextureBlendMode(f, fb);
            }
        }
        if (onAny) SDL_SetRenderTarget(ren_, prev);
    }

    void GameView::autoTuneSprites(float frameMs) {
        frameEma_ = frameEma_ * 0.85f + frameMs * 0.15f;
        if (spriteMode_ == SPR_ON)  { spritesEnabled_ = true;  return; }
        if (spriteMode_ == SPR_OFF) {
            spritesEnabled_ = false;
            if (!sprPages_.empty()) freeSpritePages();
            return;
        }
        // AUTO. Sprites help only a real crowd, so gate both on frames slower than
        // ~55fps (18ms) AND enough on-screen units -- that keeps a startup/asset
        // hitch with few units from latching them on. A kept-up frame reads ~16.6ms
        // (cap) or faster, so it can't reveal how much headroom a light scene has;
        // turn sprites back off by crowd size (wide 64-on / 32-off gap = no flapping).
        if (!spritesEnabled_) {
            if (frameEma_ > 18.0f && visUnits_.size() >= 64) spritesEnabled_ = true;
        } else if (visUnits_.size() < 32) {
            spritesEnabled_ = false;
            freeSpritePages();   // return the 64MB/page VRAM while sprites are off
        }
    }

    void GameView::invalidateRenderTargets() {
        freeSpritePages();
        for (SDL_Texture* t : atlasTex_) if (t) gpuvram::destroy(t);
        atlasTex_.clear();
        impostors_.clear();
        if (impAtlas_) { gpuvram::destroy(impAtlas_); impAtlas_ = nullptr; }
        if (shadowMask_) { gpuvram::destroy(shadowMask_); shadowMask_ = nullptr; }
        shadowMaskW_ = shadowMaskH_ = 0;
        impCurX_ = impCurY_ = impShelfH_ = 0;
    }

    void GameView::freeSpritePages() {
        for (auto& p : sprPages_) if (p.tex) gpuvram::destroy(p.tex);
        sprPages_.clear();
        sprites_.clear();
    }

    void GameView::destroyGpuTextures() {
        invalidateRenderTargets();   // sprite pages + colour atlases + impostor atlas
        auto kill = [](SDL_Texture*& t) { if (t) { gpuvram::destroy(t); t = nullptr; } };
        for (auto& [n, frames] : textures_)
            for (SDL_Texture* t : frames) if (t) gpuvram::destroy(t);
        textures_.clear();
        for (auto& [n, frames] : buildFx_)
            for (SDL_Texture* t : frames) if (t) gpuvram::destroy(t);
        buildFx_.clear();
        for (auto& row : guiTex_)
            for (SDL_Texture* t : row) if (t) gpuvram::destroy(t);
        guiTex_.clear();
        for (auto& b : orderBtns_)
            for (SDL_Texture*& t : b.frames) kill(t);
        orderBtns_.clear();
        for (auto& [n, t] : icons_) if (t) gpuvram::destroy(t);
        icons_.clear();
        for (auto& [n, t] : modelIcons_) if (t) gpuvram::destroy(t);
        modelIcons_.clear();
        for (auto& [n, t] : weaponIcons_) if (t) gpuvram::destroy(t);
        weaponIcons_.clear();
        kill(unitInfoBg_); kill(unitInfoOk_); kill(unitInfoIcon_);
        unitInfoIconFor_.clear();
        for (auto& [n, s] : shadowTex_) if (s.tex) gpuvram::destroy(s.tex);
        shadowTex_.clear();
        // FeatArt: .tex ALIASES frames[0] (see featureArtFor) -- destroy the
        // frames + shadow only; FeatureInst merely borrows these pointers.
        features_.clear();
        for (auto& [n, a] : featureArt_) {
            for (SDL_Texture* t : a.frames) if (t) gpuvram::destroy(t);
            if (a.shadow) gpuvram::destroy(a.shadow);
        }
        featureArt_.clear();
        for (auto& [n, ea] : effectAnims_)
            for (auto& f : ea.frames) if (f.tex) gpuvram::destroy(f.tex);
        effectAnims_.clear();
        explosionsLoaded_ = false;
        kill(fogTex_);
        kill(panelTex_);
        kill(botTex_);
        kill(mapPreviewTex_);
        hudFont_.destroyGlyphs();
        bigFont_.destroyGlyphs();
        statFont_.destroyGlyphs();
    }

    void GameView::buildAtlasLayout() {
        atlasLaidOut_ = true;
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
        AaScaleReset _sr(ren_);
        if (slot < 0) slot = 0;
        if (!atlasLaidOut_) buildAtlasLayout();
        if (atlasW_ <= 0) return nullptr;
        if (int(atlasTex_.size()) <= slot) atlasTex_.resize(size_t(slot) + 1, nullptr);
        if (atlasTex_[slot]) return atlasTex_[slot];
        if (gpuAllocBlocked()) return nullptr;   // don't retry a failed alloc per frame
        SDL_Texture* atlas = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                               SDL_TEXTUREACCESS_TARGET, atlasW_, atlasH_);
        if (!atlas) { noteGpuAllocFail(); return nullptr; }
        SDL_SetTextureBlendMode(atlas, SDL_BLENDMODE_BLEND);
        SDL_Texture* prev = SDL_GetRenderTarget(ren_);
        SDL_SetRenderTarget(ren_, atlas);
        SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0);
        SDL_RenderClear(ren_);
        for (const auto& [name, r] : atlasRect_) {
            auto it = textures_.find(name);
            if (it == textures_.end() || it->second.empty()) continue;
            size_t ci = size_t(slot) < it->second.size() ? size_t(slot) : 0;
            SDL_Rect dst = r;
            SDL_RenderCopy(ren_, it->second[ci], nullptr, &dst);
        }
        SDL_SetRenderTarget(ren_, prev);
        SDL_SetTextureScaleMode(atlas, SDL_ScaleModeNearest);   // no atlas edge bleed
        atlasTex_[slot] = atlas;
        return atlas;
    }

    void GameView::bakeSprites(const std::string& typeId, int slot, bool canMove, bool canFly) {
        AaScaleReset _sr(ren_);
        auto key = std::make_pair(typeId, slot);
        if (sprites_.count(key)) return;
        // During an allocation backoff, don't reserve the key yet -- so the bake
        // happens properly once VRAM pressure clears, instead of never.
        if (gpuAllocBlocked()) return;
        sprites_[key] = SpriteSet{};   // reserve (not ready)
        auto vt = visuals_.find(typeId);
        if (vt == visuals_.end()) return;
        std::vector<std::string> names;
        auto vm = loadTypeVm(typeId, names);
        if (!vm) return;
        Anim tmp;
        tmp.pieceNames = &names;   // local to this bake; read-only while tmp is alive
        tmp.vm = std::move(vm);
        tmp.flyGate = flyGateOf(*tmp.vm);
        tmp.moveGate = walkGateOf(tmp.vm->file());
        bool hasWalk = hasWalkCycle(tmp.vm->file());
        bool animated = canMove || canFly;
        // (Re)start the locomotion animation from the top -- used before each bake
        // attempt so a retry on a fresh page re-captures the same frames.
        auto initAnim = [&] {
            tmp.vm->reset();
            if (canFly) { tmp.vm->setStatic(tmp.flyGate, 1); tmp.vm->start("fly");
                          for (int s = 0; s < 8; ++s) tmp.vm->tick(1.0f / 30); }
            else if (canMove && hasWalk) {
                // Ground walk scripts gate their leg motion on their moving-flag
                // static (walkGateOf: 0 for most, 3 for the Veruna monarch); without it
                // walk_legs no-ops and every baked frame is the same standing pose. Set
                // it, exactly as the live update loop does, so the bake captures a real
                // walk cycle.
                tmp.vm->setStatic(tmp.moveGate, 1);
                tmp.vm->start("walk") || tmp.vm->start("walk_legs");
            }
            else {
                // Static buildings AND mobile no-walk movers (ships' oars, wheeled
                // vehicles' wheels/props): run the COB constructor exactly as registerUnit
                // does for the live unit, then let it settle, so the bake reflects the same
                // default piece visibility (e.g. the Death Totem's Create hides its
                // vetskull* pieces) and captures a moving unit's ambient loop.
                tmp.vm->start("Create");
                for (int s = 0; s < 6; ++s) tmp.vm->tick(1.0f / 30);
            }
        };
        // Advance the bake VM one step. A ground walk script is single-pass, so (like
        // the live loop) re-invoke it once its thread ends, keeping the legs cycling
        // across the whole bake window instead of freezing after the first pass.
        auto stepVm = [&](float dt) {
            tmp.vm->tick(dt);
            if (canMove && !canFly && tmp.vm->threadCount() == 0) {
                if (hasWalk) tmp.vm->start("walk") || tmp.vm->start("walk_legs");
                else tmp.vm->start("Create");   // no-walk mover: keep its ambient loop going
            }
        };
        SDL_Texture* atlas = atlasFor(slot);
        // A page-allocation failure is transient (VRAM pressure) -- un-reserve the
        // key so this type rebakes after the backoff, unlike the permanent
        // no-COB/no-model reservations above.
        if (sprPages_.empty() && !newSprPage()) { sprites_.erase(key); return; }
        SpriteSet ss;
        ss.frames = animated ? kSprFrames : 1;
        std::vector<Tri> scratch;
        SDL_Texture* prev = SDL_GetRenderTarget(ren_);
        const float S = kImpScale, kPi = 3.14159265f;
        // Estimate the real locomotion cycle length so the baked frames span one
        // actual cycle (not a guessed 0.9s). TA locomotion scripts play once then
        // hold, so the cycle = how long the pose keeps changing; if it never
        // settles (a truly looping script) or settles instantly, fall back to 0.9s.
        auto sig = [&] {
            double s = 0;
            for (const auto& pc : tmp.vm->pieces())
                s += std::sin(pc.rot[0]) + std::sin(pc.rot[1]) + std::sin(pc.rot[2])
                   + pc.move[0] + pc.move[1] + pc.move[2];
            return float(s);
        };
        float period = 0.9f;
        if (animated) {
            initAnim();
            const float dt = 1.0f / 60.0f;
            float prev = sig();
            int stable = 0;
            for (float t = dt; t < 2.5f; t += dt) {
                tmp.vm->tick(dt);   // no re-invoke: let one walk pass settle = the cycle
                float s = sig();
                if (std::fabs(s - prev) < 1e-4f) {
                    if (++stable >= 6 && t - 6 * dt > 0.25f) { period = t - 6 * dt; break; }
                } else stable = 0;
                prev = s;
            }
            period = std::clamp(period, 0.3f, 2.0f);
        }
        bool atlasFull = false, pageAllocFailed = false;
        SDL_Texture* target = sprPages_.back().tex;
        // Capture the current VM pose at facing fi into a packed cell of `target`.
        auto capture = [&](int fi, SDL_Rect& outR, SDL_FRect& outB) {
            float heading = float(fi) / kSprFacings * 2.0f * kPi;
            // Every mover, flyers included, faces -heading: with the piece X/Y
            // negation the fly pose no longer needs a per-flyer 180 (retail has no
            // flyer facing branch, root matrix 0x4ee620 identical for all units).
            float facing = (canFly || canMove) ? -heading : 0.0f;
            scratch.clear();
            collect(scratch, atlas, vt->second.model.root, Xform{}, &tmp, facing, 0, false);
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
            if (scratch.empty()) { minX = minY = 0; maxX = maxY = 1; }
            const int pad = 2;
            int w = std::clamp(int(std::ceil((maxX - minX) * S)) + 2 * pad, 2, 400);
            int h = std::clamp(int(std::ceil((maxY - minY) * S)) + 2 * pad, 2, 400);
            SprPage& pg = sprPages_.back();   // the in-progress bake target owns the cursor
            if (pg.curX + w > sprAtlasDim_) { pg.curX = 0; pg.curY += pg.shelfH + 1; pg.shelfH = 0; }
            if (pg.curY + h > sprAtlasDim_) { atlasFull = true; return; }
            int rx = pg.curX, ry = pg.curY;
            for (auto& t : scratch) {
                SDL_Vertex v[3];
                for (int i = 0; i < 3; ++i) {
                    v[i] = t.v[i];
                    v[i].position.x = (t.v[i].position.x - minX) * S + float(rx + pad);
                    v[i].position.y = (t.v[i].position.y - minY) * S + float(ry + pad);
                }
                if (t.tex) SDL_RenderGeometry(ren_, t.tex, v, 3, nullptr, 0);
            }
            outR = SDL_Rect{rx, ry, w, h};
            outB = SDL_FRect{minX - pad / S, minY - pad / S, w / S, h / S};
            pg.curX += w + 1;
            pg.shelfH = std::max(pg.shelfH, h);
        };
        // Bake all frames into the current page; if it overflows, start a fresh
        // page and re-bake from the top (at most one retry -- a type that can't fit
        // an empty page is left not-ready and just uses its full model).
        for (int attempt = 0; attempt < 2; ++attempt) {
            initAnim();
            target = sprPages_.back().tex;
            SDL_SetRenderTarget(ren_, target);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            atlasFull = false;
            for (int k = 0; k < ss.frames && !atlasFull; ++k) {
                if (k > 0) for (int s = 0; s < 4; ++s) stepVm(period / ss.frames / 4);
                for (int fi = 0; fi < kSprFacings && !atlasFull; ++fi)
                    capture(fi, ss.rect[fi][k], ss.bbox[fi][k]);
            }
            if (!atlasFull) { ss.page = target; break; }
            if (attempt == 0 && !newSprPage()) { pageAllocFailed = true; break; }
        }
        SDL_SetRenderTarget(ren_, prev);
        // Transient VRAM failure: un-reserve so the type rebakes after the backoff.
        if (pageAllocFailed) { sprites_.erase(key); return; }
        if (atlasFull || !ss.page) return;   // doesn't fit a fresh page -> full model
        ss.period = period;
        ss.ready = true;
        sprites_[key] = ss;
    }

    void GameView::buildUnitGeom(const UnitR& u, UnitGeom& g, std::vector<Tri>& scratch) {
        g.verts.clear();
        g.runs.clear();
        g.canFly = u.type && u.type->canFly;
        if (u.underConstruction && !u.buildBegun) return;   // ghost drawn serially
        auto ut = unitType_.find(u.id);   // defensive: a throw here would abort
        if (ut == unitType_.end()) return;
        auto vt = visuals_.find(ut->second);
        if (vt == visuals_.end()) return;
        const Anim* anim = nullptr;
        auto at = anims_.find(u.id);
        if (at != anims_.end()) anim = &at->second;

        float zm = mapView_.zoom();
        int slot = colorSlot_[u.player & 7];
        // Interpolated pose so the unit glides between 30Hz sim ticks (lift computed at the
        // interpolated spot so it stays seated on the terrain as it moves).
        float ix, iz, ih; interpPose(u, ix, iz, ih);
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
        // Sprite sheet: draw a moving/idle unit as one animated quad from the baked
        // locomotion cycle. Attack/death poses keep the full 3D model (rare).
        if (spritesEnabled_) {
            auto sit = sprites_.find(std::make_pair(vt->first, slot));
            // A grounded/idle unit shows a static frame (no flap); it only cycles
            // the animation while airborne (flyers) or moving (ground). Attack/death
            // poses keep the full 3D model.
            bool grounded = (u.type && u.type->canFly) && !(anim && anim->airborne);
            bool special = anim && (anim->dying || anim->firing);
            if (sit != sprites_.end() && sit->second.ready && !special) {
                const SpriteSet& ss = sit->second;
                int fi = facingIndex((u.type && u.type->canMove) ? ih : 0.0f,
                                     kSprFacings);
                int frame = 0;
                if (ss.frames > 1 && !grounded) {
                    bool moving = (u.type && u.type->canFly) || u.moving();
                    if (moving) {
                        float rate = float(ss.frames) / std::max(0.05f, ss.period);
                        frame = (int(animClock_ * rate) + u.id) % ss.frames;
                    }
                }
                const SDL_Rect& r = ss.rect[fi][frame];
                const SDL_FRect& bb = ss.bbox[fi][frame];
                if (r.w > 2 && r.h > 2) {   // else falls to full model
                    float alt = g.canFly ? (anim ? anim->altitude : u.type->cruiseAlt) : 0.0f;
                    float qx = ax + bb.x * zm;
                    float qy = ay + bb.y * zm;   // ay already carries the altitude lift
                    float inv = 1.0f / float(sprAtlasDim_);
                    pushQuadUV(g.verts, qx, qy, bb.w * zm, bb.h * zm,
                               float(r.x) * inv, float(r.y) * inv,
                               float(r.x + r.w) * inv, float(r.y + r.h) * inv,
                               SDL_Color{255, 255, 255, 255});
                    g.runs.push_back({ss.page, 6});
                    g.ax = ax; g.ay = ay; g.alt = alt;
                    g.occY = wallOcclusionY(u.x, u.z);
                    return;
                }
            }
        }
        // Level of detail: only when really zoomed out (zoom below the gate), draw a
        // unit small enough on screen (< lodPx_ tall) as a cached impostor billboard
        // instead of its model. At normal/close zoom every unit keeps full 3D.
        if (lodEnabled_ && zm < kLodZoomGate) {
            auto hit = modelH_.find(vt->first);
            auto iit = impostors_.find(std::make_pair(vt->first, slot));
            if (hit != modelH_.end() && iit != impostors_.end() && iit->second.ready
                && hit->second * zm < lodPx_) {
                const Impostor& imp = iit->second;
                int f = facingIndex((u.type && u.type->canMove) ? ih : 0.0f, kFacings);
                const SDL_Rect& r = imp.rect[f];
                const SDL_FRect& bb = imp.bbox[f];
                float alt = g.canFly ? (anim ? anim->altitude : u.type->cruiseAlt) : 0.0f;
                float qx = ax + bb.x * zm;
                float qy = ay + bb.y * zm;   // ay already carries the altitude lift
                float inv = 1.0f / float(impAtlasDim_);
                pushQuadUV(g.verts, qx, qy, bb.w * zm, bb.h * zm,
                           float(r.x) * inv, float(r.y) * inv,
                           float(r.x + r.w) * inv, float(r.y + r.h) * inv,
                           SDL_Color{255, 255, 255, 255});
                g.runs.push_back({impAtlas_, 6});
                g.ax = ax; g.ay = ay; g.alt = alt;
                g.occY = wallOcclusionY(u.x, u.z);
                return;
            }
        }

        scratch.clear();
        Xform base;
        // (Altitude is applied to the screen anchor above, not here: a model-space
        // translate did not agree with the impostor's screen-space one, so a flyer
        // visibly jumped height as it crossed the LOD threshold.)
        // Bank and pitch the whole model (bankscale/pitchscale). Composed on the
        // BASE, before the piece tree, so the animation's own piece rotations ride
        // on top of the attitude rather than fighting it. Piece X and Y are negated
        // at the script boundary for the mirrored basis; this is a body attitude, not
        // a script rotation, so it goes in directly.
        if (anim && (anim->bank != 0.0f || anim->pitch != 0.0f)) {
            float att[3] = {anim->pitch, 0.0f, anim->bank};
            float alt = base.t[1];
            base = Xform{}.then(0.0f, alt, 0.0f, att);
        }
        // Flyers face -heading exactly like ground movers (no flyer facing branch).
        float facing = (u.type && (u.type->canMove || u.type->canFly)) ? -ih : 0.0f;
        // Disco emote: a dancing monarch spins, bobs and hue-cycles. Local wall-time
        // (animClock_) drives the smooth motion; world_.discoActive() (a synced sim
        // timer) gates it. Pure client-side eye-candy -- nothing here is hashed.
        bool disco = dancing(u);
        float discoBob = 0.0f, discoMix = 0.0f;
        SDL_Color discoCol{};
        if (disco) {
            float t = animClock_;
            facing += t * 6.2831853f;                                // ~1 rev/sec
            discoBob = std::fabs(std::sin(t * 8.0f)) * 11.0f * zm;    // bounce, px
            discoCol = discoHue(t * 0.8f);                           // body tint hue
            discoMix = 0.5f;
        }
        // Headbang emote (Shift+H): no spin -- a sharp downward nod synced to the metal
        // beat (~152 BPM), the body whipping side to side and flashing red on each bang.
        if (headbanging(u)) {
            float ph = animClock_ * 2.533f * 6.2831853f;             // ~152 bangs/min
            float bang = std::pow(std::max(0.0f, std::sin(ph)), 2.0f);
            discoBob = -bang * 16.0f * zm;                           // dip DOWN (nod)
            facing += std::sin(ph) * 0.55f;                          // hair-whip
            discoCol = SDL_Color{210, 40, 40, 255};                 // deep metal red
            discoMix = bang * 0.5f;                                  // flash on the bang
            disco = true;                                            // reuse the tint path
        }
        // Statue corpses: a petrified body renders stone-gray, a frozen one
        // ice-blue (the isstone/isfrozen statue defs), through the same vertex
        // tint mix the emotes use.
        if (u.corpsePhase && u.corpseFeat >= 0 &&
            size_t(u.corpseFeat) < world_.featureTypes().size()) {
            const auto& cf = world_.featureTypes()[size_t(u.corpseFeat)];
            if (cf.isStone) {
                discoCol = SDL_Color{145, 145, 150, 255};
                discoMix = 0.65f; disco = true;
            } else if (cf.isFrozen) {
                discoCol = SDL_Color{160, 200, 255, 255};
                discoMix = 0.55f; disco = true;
            }
        }
        bool mirror = false;
        SDL_Texture* atlas = (slot >= 0 && size_t(slot) < atlasTex_.size())
                                 ? atlasTex_[size_t(slot)] : nullptr;
        collect(scratch, atlas, vt->second.model.root, base, anim, facing, u.player, mirror);
        std::stable_sort(scratch.begin(), scratch.end(),
                  [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
        g.ax = ax; g.ay = ay;
        g.alt = anim ? anim->altitude : 0.0f;
        g.occY = wallOcclusionY(u.x, u.z);
        // "Materialising" = the summon fade-in/shimmer: either a site still conjuring
        // (progress = HP fraction) or a unit just summoned from a building (progress =
        // its viewer-only birth ramp). Both fade alpha in and glow while p < 1.
        float birthP = birthProgress(u.id);
        bool conjuring = u.type && (u.underConstruction || birthP < 1.0f);
        float p = !u.type ? 1.0f
                  : u.underConstruction ? std::clamp(u.hp / u.type->maxHp, 0.0f, 1.0f)
                                        : birthP;
        Uint8 alpha = Uint8(p * 255.0f);
        const bool spectral = u.type && u.type->ghost && !conjuring;
        float vetGold = (!conjuring && u.veteran >= 4)
                            ? float(std::min(u.veteran, 10) - 3) / 7.0f * 0.5f : 0.0f;
        SDL_Texture* cur = nullptr;
        int runStart = 0;
        for (auto& t : scratch) {
            if (t.tex != cur) {
                if (int(g.verts.size()) > runStart)
                    g.runs.push_back({cur, int(g.verts.size()) - runStart});
                cur = t.tex;
                runStart = int(g.verts.size());
            }
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
                    float pulse = 0.5f + 0.5f * std::sin(animClock_ * 7.0f +
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
                // FBI ghost: a spectral body is see-through. Retail switches the
                // renderer to its translucent blend mode for these; we scale vertex
                // alpha instead, which lands in the same place through our painter.
                // Its exact fraction comes from a per-palette .alp table we do not copy.
                if (spectral) v.color.a = Uint8(int(v.color.a) * 55 / 100);
                g.verts.push_back(v);
            }
        }
        if (int(g.verts.size()) > runStart)
            g.runs.push_back({cur, int(g.verts.size()) - runStart});
    }

    void GameView::drawUnit(const UnitR& u) {
        // A placed-but-not-yet-started site shows as a faint ghost until the
        // builder arrives and it begins conjuring for real.
        if (u.underConstruction && !u.buildBegun) {
            if (u.type) drawGhostAt(u.type, u.x, u.z);
            return;
        }
        auto vt = visuals_.find(unitType_.at(u.id));
        if (vt == visuals_.end()) return;
        const Anim* anim = nullptr;
        auto at = anims_.find(u.id);
        if (at != anims_.end()) anim = &at->second;

        // The model projection (collect + sort + screen transform + colour) was
        // done for every visible unit in parallel on the worker pool this frame;
        // here we just look up the result and submit its draw calls.
        int gslot = geomSlot(u.id);
        if (gslot < 0) return;
        UnitGeom& g = geomPool_[size_t(gslot)];
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
        if (occluded) {
            SDL_GetRendererOutputSize(ren_, &outW, &outH);
            hadClip = SDL_RenderIsClipEnabled(ren_);
            if (hadClip) SDL_RenderGetClipRect(ren_, &prevClip);
            int line = std::clamp(int(occY), 0, outH);
            SDL_Rect top{0, 0, outW, line};   // only pixels above the wall top show
            SDL_RenderSetClipRect(ren_, &top);
        }
        // No shadow here. Pass 1 batches EVERY casting unit, specials included,
        // into the coverage mask and composites it once. Drawing a second one
        // here would land on top of the composited mask and double-darken it --
        // exactly the stacking the mask exists to prevent.
        // Disco dance floor: a pulsing, hue-cycling glow disc under a dancing monarch.
        if (dancing(u)) {
            float t = animClock_;
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
        if (headbanging(u)) {
            float ph = animClock_ * 2.533f * 6.2831853f;
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
        bool conjuring = u.type && (u.underConstruction || birthProgress(u.id) < 1.0f);
        int off = 0;
        for (const auto& r : g.runs) {
            // A null run texture (an atlas page that failed to allocate under
            // VRAM pressure -- corpse meshes load mid-game) must be SKIPPED:
            // SDL_RenderGeometry(nullptr,..) paints the raw vertex colours as a
            // solid white shape. Invisible-until-retry beats a white flash.
            if (r.first)
                SDL_RenderGeometry(ren_, r.first, g.verts.data() + off, r.second,
                                   nullptr, 0);
            off += r.second;
        }

        // Build/reclaim nano-sparkle. Retail sparkles BOTH ends -- the worker unit AND
        // its target -- but only once the job has really STARTED: a placed site shows as
        // a ghost until buildBegun (above), and a reclaim sparkles only once the builder
        // is in range (not while it walks/flies over).
        auto sideLower = [&] {
            std::string s = u.type ? u.type->side : std::string();
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            return s;
        };
        auto uFootW = [&] { return std::max(u.type->footX, 1) * 16.0f * zm; };
        auto uFootH = [&] { return std::max(u.type->footZ, 1) * 16.0f * zm; };

        // A conjuring/summoning SITE (this unit) sparkles over itself.
        if (conjuring)
            sprinkleBuildFx(sideLower(), ax, ay, uFootW(), uFootH());

        // A builder actively conjuring a site sparkles over ITSELF too (the worker end).
        if (u.type && u.buildSiteId != 0) {
            const auto* site = frameUnitP(u.buildSiteId);
            if (site && site->buildBegun)
                sprinkleBuildFx(sideLower(), ax, ay, uFootW(), uFootH());
        }

        // A reclaimer IN RANGE (the reclaim has really started -- range test mirrors
        // World::tickReclaim): sparkle the reclaimer AND the feature it is chewing on.
        if (u.type && u.reclaimId != 0) {
            const auto* feat = world_.feature(u.reclaimId);
            if (feat && feat->alive) {
                float dxr = feat->x - u.x, dzr = feat->z - u.z;
                float reach = 24.0f + 8.0f * float(std::max(feat->fx, feat->fz)) +
                              (u.type->buildDist > 0 ? u.type->buildDist : 0.0f);
                if (dxr * dxr + dzr * dzr <= reach * reach) {
                    sprinkleBuildFx(sideLower(), ax, ay, uFootW(), uFootH());   // the reclaimer
                    float fsx = (feat->x - mapView_.offX()) * zm - terrainLiftX(feat->x, feat->z) * zm;
                    float fsy = (feat->z - mapView_.offY()) * zm - terrainLift(feat->x, feat->z) * zm;
                    sprinkleBuildFx(sideLower(), fsx, fsy,
                                    std::max(feat->fx, 1) * 16.0f * zm,
                                    std::max(feat->fz, 1) * 16.0f * zm);         // the feature
                }
            }
        }

        // Occluded: re-draw the hidden lower part as a faint, flat player-coloured
        // silhouette through the wall, so a unit behind cover is never fully lost.
        if (occluded) {
            int line = std::clamp(int(occY), 0, outH);
            SDL_Rect bot{0, line, outW, std::max(0, outH - line)};
            SDL_RenderSetClipRect(ren_, &bot);
            SDL_Color tc = playerColor(u.player);
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            // The silhouette is flat, untextured and single-colour, so the whole
            // model collapses to ONE draw call: re-tint every vertex and submit.
            triBatch_.clear();
            for (const SDL_Vertex& sv : g.verts) {
                SDL_Vertex v = sv;
                v.color = SDL_Color{tc.r, tc.g, tc.b, 70};
                triBatch_.push_back(v);
            }
            if (!triBatch_.empty())
                SDL_RenderGeometry(ren_, nullptr, triBatch_.data(),
                                   int(triBatch_.size()), nullptr, 0);
            if (hadClip) SDL_RenderSetClipRect(ren_, &prevClip);
            else SDL_RenderSetClipRect(ren_, nullptr);
        }
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
            const auto* pal = featurePalette("aramon");
            if (pal) try {
                for (auto& sq : tak::gaf::load(vread("anims/shadows.gaf"), *pal, -1,
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
                if (std::filesystem::path(path).extension() != ".tdf") continue;
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

    const tak::gaf::Palette* GameView::featurePalette(std::string world) {
        std::transform(world.begin(), world.end(), world.begin(), ::tolower);
        auto it = featurePals_.find(world);
        if (it != featurePals_.end()) return &it->second;
        const std::string cands[] = {world + "_features.pcx", world + ".pcx",
                                     std::string("aramon_features.pcx")};
        for (const std::string& cand : cands) {
            try {
                std::string pp = "palettes/" + cand;
                return &featurePals_
                            .emplace(world, tak::gaf::Palette::fromBytes(vread(pp), pp))
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

    GameView::FeatArt* GameView::featureArtFor(const tak::tdf::Node& def,
                                               const char* seqKey, const char* shadKey) {
        std::string file = def.valueOr("filename", "");
        std::string seq = def.valueOr(seqKey, "");
        std::string seqShad = def.valueOr(shadKey, "");
        if (seq.empty()) return nullptr;   // e.g. a def without seqnameburn
        std::string key = file + "|" + seq;
        auto it = featureArt_.find(key);
        if (it != featureArt_.end()) return it->second.tex ? &it->second : nullptr;
        FeatArt a{};
        const auto* pal = featurePalette(def.valueOr("world", "aramon"));
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
                for (auto& sq : tak::gaf::load(vread("anims/" + f + ".gaf"), *pal, -1,
                                               "anims/" + f + ".gaf")) {
                    if (sq.frames.empty() || sq.frames[0].width == 0) continue;
                    auto& fr = sq.frames[0];
                    if (ieq(sq.name, seq)) {
                        bool animate = def.numberOr("animating", 0) != 0 ||
                                       def.numberOr("animatable", 0) != 0;
                        size_t nf = animate ? sq.frames.size() : 1;
                        // Keep EVERY frame at its own size/anchor: wave sequences
                        // author their motion through per-frame w/h/xoff/yoff (the
                        // foam sweeps by re-anchoring each tightly-cropped frame),
                        // so dropping non-uniform frames froze them.
                        int acc = 0;
                        for (size_t fi = 0; fi < nf; ++fi) {
                            auto& ff = sq.frames[fi];
                            if (ff.width == 0 || ff.height == 0) continue;
                            SDL_Texture* t = gpuvram::create(
                                ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                ff.width, ff.height);
                            if (!t) continue;   // VRAM-pressure alloc fail: drop the frame
                            premulUpload(t, ff.rgba, ff.width);
                            if (bilinear_) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
                            a.frames.push_back(t);
                            a.fgeom.push_back({ff.width, ff.height, ff.xoff, ff.yoff});
                            acc += std::max(1, ff.delayTicks);
                            a.tickEnd.push_back(acc);
                        }
                        a.totalTicks = acc;
                        if (!a.frames.empty()) {
                            a.tex = a.frames[0];
                            a.w = a.fgeom[0].w; a.h = a.fgeom[0].h;
                            a.xoff = a.fgeom[0].xoff; a.yoff = a.fgeom[0].yoff;
                        }
                    } else if (!seqShad.empty() && ieq(sq.name, seqShad)) {
                        // Shadow: silhouette drawn as translucent black.
                        std::vector<uint8_t> px = fr.rgba;
                        for (size_t i = 0; i + 3 < px.size(); i += 4) {
                            px[i] = px[i + 1] = px[i + 2] = 0;
                            px[i + 3] = px[i + 3] ? 90 : 0;
                        }
                        a.shadow = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                     SDL_TEXTUREACCESS_STATIC, fr.width,
                                                     fr.height);
                        premulUpload(a.shadow, px, fr.width);
                        if (bilinear_) SDL_SetTextureScaleMode(a.shadow, SDL_ScaleModeLinear);
                        a.sw = fr.width; a.sh = fr.height;
                        a.sxoff = fr.xoff; a.syoff = fr.yoff;
                    }
                }
            } catch (const std::exception&) {}
        }
        featureArt_[key] = a;
        return featureArt_[key].tex ? &featureArt_[key] : nullptr;
    }

    bool GameView::addFeature(const std::string& rawName, float x, float z, bool blockNav) {
        loadFeatureDefs();
        std::string key = rawName;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        auto di = featureDefs_.find(key);
        if (di == featureDefs_.end()) return false;
        FeatArt* a = featureArtFor(di->second);
        if (!a) return false;
        FeatureInst inst;
        inst.tex = a->tex;
        inst.frames = &a->frames;
        inst.art = a;   // per-frame geometry + GAF tick timeline (stable: map node)
        inst.shadow = a->shadow;
        inst.w = a->w; inst.h = a->h; inst.xoff = a->xoff; inst.yoff = a->yoff;
        inst.sw = a->sw; inst.sh = a->sh; inst.sxoff = a->sxoff; inst.syoff = a->syoff;
        inst.x = x;
        inst.z = z;
        inst.name = key;
        inst.simId = (int(z) / 16) * mapView_.map().width + int(x) / 16;
        featInstIds_.insert(inst.simId);
        // Mana deposits ("Sacred Stone", category=Mana) are the spots you build
        // lodestones ON, so they must stay buildable (walkable) — never block
        // the nav grid for them, or canPlace rejects the deposit itself.
        std::string cat = di->second.valueOr("category", "");
        std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
        inst.mana = (cat == "mana");
        inst.tree = (cat == "trees");
        // The buildable spot is the animated Sacred Stone centre; the static
        // Standing Stones sharing the category are just ruins around it.
        inst.glowy = inst.mana && di->second.numberOr("animating", 0) != 0;
        features_.push_back(inst);
        // Retail nav-blocking: ordinary obstacle features AND the static Standing
        // Stones (blocking=1) block; only the glowy Sacred Stone centre stays
        // walkable, so a lodestone can build on it. (Standing Stones default to
        // blocking; the centre has no blocking field, so it never blocks.)
        bool blocks = !inst.glowy && (!inst.mana || di->second.numberOr("blocking", 0) != 0);
        if (blockNav && blocks) {
            int fx = int(di->second.numberOr("footprintx", 1));
            int fz = int(di->second.numberOr("footprintz", 1));
            world_.nav().block(int(x) / 16 - fx / 2, int(z) / 16 - fz / 2, fx, fz, true);
        }
        return true;
    }

    // Burn-out art swap: the sim swapped this feature to its featureburnt stage
    // in place (same cell/id); re-point the instance at the burnt feature's art.
    void GameView::swapFeatureArt(FeatureInst& fi, const std::string& name) {
        fi.name = name;
        fi.burnArt = nullptr;
        fi.tree = false;   // a burnt stump/smudge doesn't sway
        auto di = featureDefs_.find(name);
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
        // Takes simMutex_: this runs on the RENDER thread but reads live world_
        // state the worker mutates. gameview.h documents the invariant -- the
        // render side reads the published snapshot, and any remaining live read
        // locks -- and this was the one place breaking it. Not a benign torn
        // int either: featureAt() hands back a pointer INTO the feature vector,
        // and area reclaim removes features, so the worker can reallocate that
        // vector under this loop.
        std::unique_lock<std::mutex> lk(simMutex_, std::defer_lock);
        if (useSimThread_) lk.lock();
        if (world_.featureTypes().empty()) return;
        for (auto& fi : features_) {
            const auto* sf = world_.featureAt(fi.x, fi.z);
            if (!sf || sf->type < 0) continue;
            if (fi.simType == -2) fi.simType = sf->type;       // first sight
            else if (sf->type != fi.simType) {                  // burnt-stage swap
                fi.simType = sf->type;
                swapFeatureArt(fi, world_.featureTypes()[size_t(sf->type)].name);
            }
            bool b = sf->alive && sf->burn != 0;
            if (b && !fi.burnVis) {                             // ignition edge
                auto di = featureDefs_.find(fi.name);
                if (di != featureDefs_.end())
                    fi.burnArt = featureArtFor(di->second, "seqnameburn",
                                               "seqnameburnshad");
                static const bool kBurnLog = tak::devEnv("TAK_BURNLOG") != nullptr;
                if (kBurnLog)
                    std::fprintf(stderr, "burn-vis %s art=%d\n", fi.name.c_str(),
                                 fi.burnArt && fi.burnArt->tex ? 1 : 0);
                // Flame overlays: retail plays seqnamefrontflame/backflame from a
                // shared flame registry; our looping "flame" effect stands in,
                // sized to cover the 5s sim burn.
                spawnEffectAnim("flame", fi.x, fi.z, 0.0f, 0.0f, 7);
                spawnEffectAnim("flame", fi.x - 8, fi.z + 6, 0.3f, 0.0f, 7);
            }
            fi.burnVis = b ? 1 : 0;
            if (b && animClock_ >= fi.lastSmoke + 0.35f &&
                (noFog_ || cellVisibleR(fi.x, fi.z))) {
                fi.lastSmoke = animClock_ + float(salt_++ % 20) * 0.01f;
                spawnBurst(fi.x, fi.z, 2, 90, 80, 80, 12, 2.6f, 1, float(fi.h) * 0.4f);
                spawnBurst(fi.x, fi.z, 1, 240, 140, 40, 14, 1.8f, 0, 6);
            }
        }
        // Features the SIM created mid-game (corpses) get a visual instance on
        // first sight. No nav blocking here -- the sim owns corpse blocking.
        for (const auto& sf : world_.features()) {
            if (!sf.alive || sf.type < 0 || featInstIds_.count(sf.id)) continue;
            addFeature(world_.featureTypes()[size_t(sf.type)].name, sf.x, sf.z, false);
            featInstIds_.insert(sf.id);   // even on art failure: don't retry every frame
        }
    }

    void GameView::loadFeatures() {
        features_.clear();   // full rebuild -- safe to call again on a map change
        featInstIds_.clear();
        const auto& names = mapView_.map().featureNames;
        if (names.empty()) return;
        loadFeatureDefs();
        const auto& map = mapView_.map();
        int placed = 0;
        for (int cz = 0; cz < map.height; ++cz)
            for (int cx = 0; cx < map.width; ++cx) {
                uint16_t v = map.features[size_t(cz) * map.width + cx];
                if (v >= names.size()) continue;
                if (addFeature(names[v], float(cx) * 16 + 8, float(cz) * 16 + 8, true))
                    ++placed;
            }
        std::printf("features: %d placed\n", placed);
        // Register the Sacred Stone deposits so lodestones can only build on
        // them (and the AI knows where to put them). The buildable spot is the
        // GLOWING centre (animated Sacred Stone) -- NOT the ring of static
        // Standing Stones around it, which share category=mana but are just
        // ruins; including them pulled the spot off-centre. One deposit can have
        // a couple of adjacent glowy stones, so still cluster (union-find, 60px
        // link) and keep ONE spot per cluster. Fallback: a deposit with no glowy
        // centre at all (odd data) uses its category=mana features instead.
        std::vector<std::pair<float, float>> raw;
        for (const auto& f : features_)
            if (f.glowy) raw.push_back({f.x, f.z});
        if (raw.empty())
            for (const auto& f : features_)
                if (f.mana) raw.push_back({f.x, f.z});
        std::vector<int> par(raw.size());
        for (size_t i = 0; i < par.size(); ++i) par[i] = int(i);
        std::function<int(int)> find = [&](int a) {
            while (par[size_t(a)] != a) { par[size_t(a)] = par[size_t(par[size_t(a)])]; a = par[size_t(a)]; }
            return a;
        };
        const float link2 = 60.0f * 60.0f;
        for (size_t i = 0; i < raw.size(); ++i)
            for (size_t j = i + 1; j < raw.size(); ++j) {
                float dx = raw[i].first - raw[j].first, dz = raw[i].second - raw[j].second;
                if (dx * dx + dz * dz < link2) par[size_t(find(int(i)))] = find(int(j));
            }
        std::map<int, std::pair<std::pair<double, double>, int>> acc;   // root -> (sum, count)
        for (size_t i = 0; i < raw.size(); ++i) {
            auto& a = acc[find(int(i))];
            a.first.first += raw[i].first; a.first.second += raw[i].second; ++a.second;
        }
        manaSpots_.clear();
        for (auto& [root, a] : acc)
            manaSpots_.push_back({float(a.first.first / a.second),
                                  float(a.first.second / a.second)});
        world_.setManaSpots(manaSpots_);
        // Standing Stones block nav, but a large one can reach the glowy centre;
        // keep every deposit buildable by carving the 2x2 lodestone footprint
        // clear at each spot (canPlace tests exactly these cells).
        for (const auto& [sx, sz] : manaSpots_)
            world_.nav().block(int(sx) / 16 - 1, int(sz) / 16 - 1, 2, 2, false);
        std::printf("mana deposits: %zu (from %zu features)\n",
                    manaSpots_.size(), raw.size());
        addShorelineWaves();
    }

    void GameView::addShorelineWaves() {
        // Shipped maps hand-place their wave features as sparse OFFSHORE breakers
        // (Athri Cay: 34 sprites on a 480x480 map, 4-15 cells off the coast in
        // strings 10-18 cells apart along SOME stretches -- never a continuous
        // surf rim; some maps have zero). Generated maps have no author, so
        // scatter them retail-style here. Display only: addFeature(...,
        // blockNav=false) is a render instance with no nav/mana/sim effect.
        if (!tak::mapgen::isGeneratedMapId(mapPath_)) return;   // authored maps ship their own
        const auto& map = mapView_.map();
        const int W = map.width, H = map.height, sea = map.seaLevel;
        if (W <= 0 || H <= 0 || map.heights.size() < size_t(W) * H) return;
        // World prefix (AraWave/TarWave/VerWave/ZonWave/CreWave) from the map's
        // feature names (case-insensitive: Creon TDFs mix "CreTree"/"CRERock").
        std::string wp = "Ara";
        for (const auto& n : map.featureNames) {
            std::string p = n.substr(0, 3);
            std::transform(p.begin(), p.end(), p.begin(), ::tolower);
            if (p == "tar") { wp = "Tar"; break; }
            if (p == "ver") { wp = "Ver"; break; }
            if (p == "zon") { wp = "Zon"; break; }
            if (p == "ara") { wp = "Ara"; break; }
            if (p == "cre") { wp = "Cre"; break; }
        }
        auto land = [&](int x, int z) {
            return x >= 0 && z >= 0 && x < W && z < H && map.heights[size_t(z) * W + x] >= sea;
        };
        // Multi-source BFS: distance (in cells) from every water cell to land.
        std::vector<uint8_t> dist(size_t(W) * H, 255);
        std::vector<int> q; q.reserve(size_t(W) * H / 4);
        for (int z = 0; z < H; ++z)
            for (int x = 0; x < W; ++x)
                if (land(x, z)) { dist[size_t(z) * W + x] = 0; q.push_back(z * W + x); }
        for (size_t head = 0; head < q.size(); ++head) {
            int c = q[head], cx = c % W, cz = c / W;
            int d = dist[size_t(c)];
            if (d >= 15) continue;
            static const int dx4[] = {1, -1, 0, 0}, dz4[] = {0, 0, 1, -1};
            for (int k = 0; k < 4; ++k) {
                int nx = cx + dx4[k], nz = cz + dz4[k];
                if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
                size_t ni = size_t(nz) * W + nx;
                if (dist[ni] != 255) continue;
                dist[ni] = uint8_t(d + 1);
                q.push_back(int(ni));
            }
        }
        // One candidate per 12x12 bucket, hash-gated to ~45% so the breakers form
        // intermittent strings (not every stretch of coast gets any) -- matching
        // the shipped density/spacing. Stable per map (hash on bucket + map dims).
        auto hash = [&](uint64_t v) {
            v ^= uint64_t(W) << 32 ^ uint64_t(H) ^ (uint64_t(sea) << 20);
            v = (v ^ (v >> 30)) * 0xbf58476d1ce4e5b9ULL;
            v = (v ^ (v >> 27)) * 0x94d049bb133111ebULL;
            return v ^ (v >> 31);
        };
        int placed = 0;
        for (int bz = 0; bz < H; bz += 12)
            for (int bx = 0; bx < W; bx += 12) {
                if (hash(uint64_t(bz) * 100003 + bx) % 100 >= 45) continue;
                // Best cell in the bucket: offshore band 5..14, nearest to 9 out.
                int cx = -1, cz = -1, best = 99;
                for (int z = bz; z < std::min(bz + 12, H); ++z)
                    for (int x = bx; x < std::min(bx + 12, W); ++x) {
                        int d = dist[size_t(z) * W + x];
                        if (land(x, z) || d < 5 || d > 14) continue;
                        if (std::abs(d - 9) < best) { best = std::abs(d - 9); cx = x; cz = z; }
                    }
                if (cx < 0) continue;
                // Face the crest toward the nearest shore: steepest-descent walk
                // down the distance field to land, then classify the direction.
                int wx = cx, wz = cz;
                for (int step = 0; step < 20 && !land(wx, wz); ++step) {
                    int bd = dist[size_t(wz) * W + wx], nx = wx, nz = wz;
                    static const int dx4[] = {1, -1, 0, 0}, dz4[] = {0, 0, 1, -1};
                    for (int k = 0; k < 4; ++k) {
                        int tx = wx + dx4[k], tz = wz + dz4[k];
                        if (tx < 0 || tz < 0 || tx >= W || tz >= H) continue;
                        if (dist[size_t(tz) * W + tx] < bd) { bd = dist[size_t(tz) * W + tx]; nx = tx; nz = tz; }
                    }
                    if (nx == wx && nz == wz) break;
                    wx = nx; wz = nz;
                }
                int ddx = wx - cx, ddz = wz - cz;
                // Variant by land direction (foam draws toward the land; anchors
                // calibrated from the GAF): cardinals N->13 S->01 E->07 W->03,
                // diagonals NW->04 NE->06 SE->16 SW->10.
                int v;
                bool diag = std::abs(ddx) * 2 > std::abs(ddz) && std::abs(ddz) * 2 > std::abs(ddx);
                if (diag) v = ddx < 0 ? (ddz < 0 ? 4 : 10) : (ddz < 0 ? 6 : 16);
                else if (std::abs(ddx) >= std::abs(ddz)) v = ddx >= 0 ? 7 : 3;
                else v = ddz >= 0 ? 1 : 13;
                char nm[24];
                std::snprintf(nm, sizeof nm, "%sWave%02d", wp.c_str(), v);
                if (addFeature(nm, float(cx) * 16 + 8, float(cz) * 16 + 8, false)) ++placed;
            }
        std::printf("offshore waves: %d placed (%sWave)\n", placed, wp.c_str());
    }

    bool GameView::buildIconClick(float mx, float my, bool lmb, bool rmb) {
        if (!lmb && !rmb) return false;
        for (const auto& [r, bt] : iconRects_) {
            if (mx < r.x || mx > r.x + r.w || my < r.y || my > r.y + r.h) continue;
            playClickTone();
            const auto* b = selectedBuilder();
            if (!b || !bt) return true;   // consume the click even if it can't act
            uint16_t mod = SDL_GetModState();
            bool ctrl = (mod & KMOD_CTRL) != 0, shift = (mod & KMOD_SHIFT) != 0;
            // Ctrl+left-click = infinite production (RepeatTrain), FIRST so it also
            // works for a mobile conjurer (a beast tamer): the sim spawns each new
            // unit beside the producer and re-queues, no manual placement needed.
            if (lmb && ctrl && !shift) {
                tak::net::Command c;
                c.kind = tak::net::Cmd::RepeatTrain;
                c.unitId = b->id;
                std::snprintf(c.type, sizeof c.type, "%s", bt->id.c_str());
                issue(c);
                return true;
            }
            if (isStructure(bt) || !isStructure(b->type)) {
                if (lmb) placing_ = bt;   // manual placement (buildings / mobile conjurers)
                return true;
            }
            tak::net::Command c;
            c.unitId = b->id;
            std::snprintf(c.type, sizeof c.type, "%s", bt->id.c_str());
            c.kind = lmb ? tak::net::Cmd::Train : tak::net::Cmd::Unqueue;
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
            auto img = tak::jpeg::load(vread("anims/buildpic/" + typeId + ".jpg"));
            tex = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                    SDL_TEXTUREACCESS_STATIC, img.width, img.height);
            SDL_UpdateTexture(tex, nullptr, img.rgba.data(), img.width * 4);
        } catch (const std::exception&) {}
        icons_[typeId] = tex;
        return tex;
    }

    SDL_Texture* GameView::modelIconTex(const std::string& id, int slot, bool canMove) {
        AaScaleReset _sr(ren_);
        auto keyp = std::make_pair(id, slot);
        if (auto it = modelIcons_.find(keyp); it != modelIcons_.end()) return it->second;
        if (gpuAllocBlocked()) return nullptr;   // retry after the alloc backoff lapses
        modelIcons_[keyp] = nullptr;   // cache the attempt (success or failure) up front
        if (!ghostModel(id)) return nullptr;
        auto vt = visuals_.find(id);
        if (vt == visuals_.end()) return nullptr;
        SDL_Texture* atlas = atlasFor(slot);
        std::vector<Tri> scratch;
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
                int d = h - (heightRef_ < 0 ? 0 : heightRef_);
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
            if (!alliedToLocal(u.player) && !cellVisibleR(u.x, u.z) && !noFog_) continue;
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

    void GameView::drawFog() {
        if (noFog_) return;
        const auto& vis = frameVisibility();
        if (vis.empty()) return;
        int w = frameVisW(), h = frameVisH();
        if (!fogTex_) {
            fogTex_ = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
            SDL_SetTextureBlendMode(fogTex_, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(fogTex_, SDL_ScaleModeLinear);
        }
        // The fog CONTENT only changes when the sim recomputes visibility (4Hz);
        // frames render far more often (up to 240Hz), so rewrite + re-upload the
        // streaming texture only when the vis generation actually advanced.
        if (fogTexGen_ != frameVisGeneration()) {
            fogTexGen_ = frameVisGeneration();
            void* px = nullptr;
            int pitch = 0;
            if (SDL_LockTexture(fogTex_, nullptr, &px, &pitch) == 0) {
                for (int z = 0; z < h; ++z) {
                    uint32_t* row = reinterpret_cast<uint32_t*>(
                        static_cast<uint8_t*>(px) + size_t(z) * size_t(pitch));
                    for (int x = 0; x < w; ++x) {
                        uint8_t v = vis[size_t(z) * w + x];
                        uint8_t a = v == 2 ? 0 : (v == 1 ? 110 : 235);
                        row[x] = uint32_t(a) << 24;   // black with alpha (RGBA32 LE)
                    }
                }
                SDL_UnlockTexture(fogTex_);
            }
        }
        float zm = mapView_.zoom();
        // Lift the fog to sit on the terrain relief, exactly like units do, so the
        // cleared area follows a unit up a hill instead of staying at ground level.
        // Draw it as a grid of quads lifted per-cell (the terrain art itself is a
        // flat mosaic, but units are drawn lifted onto it -- the fog must match).
        heightAbove(0.0f, 0.0f);   // ensure heightRef_/scales are initialised
        float maxLy = float(255 - std::max(heightRef_, 0)) * kHeightScale_;
        float maxLx = float(255 - std::max(heightRef_, 0)) * kHeightScaleX_;
        float ox = mapView_.offX(), oy = mapView_.offY();
        int gx0 = std::clamp(int(ox / 16) - 1, 0, w);
        int gx1 = std::clamp(int((ox + winW_ / zm + maxLx) / 16) + 2, 0, w);
        int gz0 = std::clamp(int(oy / 16) - 1, 0, h);
        int gz1 = std::clamp(int((oy + winH_ / zm + maxLy) / 16) + 2, 0, h);
        // One height sample per grid CORNER, shared by all four adjacent quads.
        // vert() used to pay two independent bilinear samples per corner PER QUAD
        // (terrainLift + terrainLiftX each re-sampling), 8 samples per cell -- at a
        // 7680-wide window that alone measured ~2ms/frame; the shared corner grid
        // is ~8x fewer samples, each yielding both lifts.
        int cw = std::max(0, gx1 - gx0 + 1), ch = std::max(0, gz1 - gz0 + 1);
        fogLift_.assign(size_t(cw) * size_t(ch), 0.0f);
        for (int gz = gz0; gz <= gz1 && ch > 0; ++gz)
            for (int gx = gx0; gx <= gx1; ++gx)
                fogLift_[size_t(gz - gz0) * cw + size_t(gx - gx0)] =
                    heightAbove(float(gx) * 16.0f, float(gz) * 16.0f);
        auto vert = [&](int gx, int gz) {
            float wx = float(gx) * 16.0f, wz = float(gz) * 16.0f;
            float ha = fogLift_[size_t(gz - gz0) * cw + size_t(gx - gx0)];
            SDL_Vertex v;
            v.position = {(wx - ox) * zm - ha * kHeightScaleX_ * zm,
                          (wz - oy) * zm - ha * kHeightScale_ * zm};
            v.tex_coord = {float(gx) / float(w), float(gz) / float(h)};
            v.color = {255, 255, 255, 255};
            return v;
        };
        // Skip cells whose whole 3x3 neighbourhood is fully visible: their quad is
        // invisible (alpha 0) and, because the texture is linear-filtered, only a
        // fogged NEIGHBOUR can bleed alpha across the edge. Late-game this culls
        // most of the vertex stream.
        auto fogged = [&](int x, int z) {
            if (x < 0 || z < 0 || x >= w || z >= h) return true;   // map edge: keep
            return vis[size_t(z) * w + x] != 2;
        };
        fogVerts_.clear();
        for (int gz = gz0; gz < gz1; ++gz)
            for (int gx = gx0; gx < gx1; ++gx) {
                bool any = false;
                for (int dz = -1; dz <= 1 && !any; ++dz)
                    for (int dx = -1; dx <= 1 && !any; ++dx)
                        any = fogged(gx + dx, gz + dz);
                if (!any) continue;
                SDL_Vertex a = vert(gx, gz), b = vert(gx + 1, gz),
                           c = vert(gx + 1, gz + 1), d = vert(gx, gz + 1);
                fogVerts_.push_back(a); fogVerts_.push_back(b); fogVerts_.push_back(c);
                fogVerts_.push_back(a); fogVerts_.push_back(c); fogVerts_.push_back(d);
            }
        if (!fogVerts_.empty())
            SDL_RenderGeometry(ren_, fogTex_, fogVerts_.data(), int(fogVerts_.size()),
                               nullptr, 0);
    }

    void GameView::drawGhost() {
        float zm = mapView_.zoom();
        // Resolve the elevated cell drawn under the cursor. Draw a translucent ghost
        // of the thing being placed, lifted onto the relief exactly like the finished
        // unit/building -- a plain ghost where it can go, red-washed where it can't.
        float wx, wz;
        pickWorld(mouseX_, mouseY_, wx, wz);
        bool ok = canPlaceLocked(placing_, wx, wz);
        // A site blocked only by clearable doodads isn't a refusal any more -- the
        // click sends the builder to clear it first -- so don't red-wash it. The
        // notice tells the player what the click will actually do.
        std::vector<int> clearFeats;
        if (!ok && clearableAt(placing_, wx, wz, clearFeats) && !clearFeats.empty())
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
        const auto* pal = featurePalette("aramon");   // ignored for truecolor TAF
        // "file:sequence" targets a specific GAF sequence (e.g. "flames:flame large");
        // a bare name uses the file of that name and its like-named (or first) sequence.
        std::string file = animName, seqWant = animName;
        if (auto c = animName.find(':'); c != std::string::npos) {
            file = animName.substr(0, c);
            seqWant = animName.substr(c + 1);
        }
        for (const std::string suf : {"_4444.taf", "_1555.taf", ".taf", ".gaf"}) {
            if (!ea.frames.empty()) break;
            try {
                std::string ap = "anims/" + file + suf;
                auto seqs = tak::gaf::load(vread(ap), pal ? *pal : tak::gaf::Palette{}, -1, ap);
                const tak::gaf::Sequence* seq = nullptr;
                for (auto& s : seqs) {
                    if (s.frames.empty()) continue;
                    if (!seq) seq = &s;
                    std::string sn = s.name;
                    std::transform(sn.begin(), sn.end(), sn.begin(), ::tolower);
                    if (sn == seqWant) { seq = &s; break; }
                }
                if (!seq) continue;
                for (auto& fr : seq->frames) {
                    if (fr.width == 0 || fr.height == 0) continue;
                    SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                       SDL_TEXTUREACCESS_STATIC,
                                                       fr.width, fr.height);
                    SDL_UpdateTexture(t, nullptr, fr.rgba.data(), fr.width * 4);
                    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);   // fiery glow
                    ea.frames.push_back({t, fr.width, fr.height, fr.xoff, fr.yoff});
                }
            } catch (const std::exception&) {}
        }
        auto& stored = effectAnims_[animName];
        stored = std::move(ea);
        return stored.frames.empty() ? nullptr : &stored;
    }

    void GameView::updateEffects(float dt) {
        for (auto& b : beams_) b.age += dt;
        std::erase_if(beams_, [](const BeamFx& b) { return b.age > b.life; });
        for (auto& e : effects_) e.age += dt;
        std::erase_if(effects_, [](const EffectInst& e) {
            if (!e.anim || e.anim->frames.empty()) return true;
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

    void GameView::drawRings() {
        float zm = mapView_.zoom();
        for (const auto& r : rings_) {
            float local = r.age - r.delay;
            if (local < 0 || !r.anim || r.anim->frames.empty()) continue;
            if (!cellVisibleR(r.x, r.z)) continue;
            float t = std::clamp(local / std::max(r.dur, 1e-3f), 0.0f, 1.0f);
            float radius = r.maxR * t;
            int nf = int(r.anim->frames.size());
            const EFrame& f = r.anim->frames[size_t(std::clamp(int(t * nf), 0, nf - 1))];
            for (int k = 0; k < r.sprites; ++k) {
                float a = 6.2831853f * float(k) / float(r.sprites);
                float wx = r.x + std::cos(a) * radius, wz = r.z + std::sin(a) * radius;
                float sx = (wx - mapView_.offX()) * zm - f.ax * zm - terrainLiftX(r.x, r.z) * zm;
                float sy = (wz - mapView_.offY()) * zm - f.ay * zm - terrainLift(r.x, r.z) * zm;
                SDL_FRect dst{sx, sy, f.w * zm, f.h * zm};
                SDL_RenderCopyF(ren_, f.tex, nullptr, &dst);
            }
        }
    }

    void GameView::drawEffects() {
        drawRings();
        float zm = mapView_.zoom();
        for (const auto& e : effects_) {
            if (!e.anim || e.anim->frames.empty()) continue;
            float local = e.age - e.delay;
            if (local < 0) continue;                // still waiting to start
            if (!cellVisibleR(e.x, e.z)) continue;
            float per = effLoopLen(e);
            float within = local - std::floor(local / per) * per;   // into this loop
            int nf = int(e.anim->frames.size());
            int fi = std::clamp(int(within / per * float(nf)), 0, nf - 1);
            const EFrame& f = e.anim->frames[size_t(fi)];
            float sx = (e.x - mapView_.offX()) * zm - f.ax * zm - terrainLiftX(e.x, e.z) * zm;
            float sy = (e.z - mapView_.offY()) * zm - f.ay * zm - terrainLift(e.x, e.z) * zm
                       - e.alt * zm;
            SDL_FRect dst{sx, sy, f.w * zm, f.h * zm};
            SDL_RenderCopyF(ren_, f.tex, nullptr, &dst);
        }
    }

    void GameView::drawUnitFx() {
        float zm = mapView_.zoom();
        const float kLinger = 0.8f;   // seconds after the last emit to keep drawing
        for (auto& [id, a] : anims_) {
            bool fire = a.fireFx && a.fireT < kLinger;
            bool smk = a.smokeFx && a.smokeT < kLinger;
            if (!fire && !smk) continue;
            const auto* u = frameUnitP(id);
            if (!u || !u->type) continue;
            if (!noFog_ && !cellVisibleR(u->x, u->z)) continue;
            auto draw = [&](const EffectAnim* ea, float lift, float fps) {
                int nf = int(ea->frames.size());
                int fi = int(animClock_ * fps + float(id) * 0.37f) % nf;
                if (fi < 0) fi += nf;
                const EFrame& f = ea->frames[size_t(fi)];
                float sx = (u->x - mapView_.offX()) * zm - f.ax * zm - terrainLiftX(u->x, u->z) * zm;
                float sy = (u->z - mapView_.offY()) * zm - f.ay * zm - terrainLift(u->x, u->z) * zm
                           - lift * zm;
                SDL_FRect dst{sx, sy, f.w * zm, f.h * zm};
                SDL_RenderCopyF(ren_, f.tex, nullptr, &dst);
            };
            if (smk) draw(a.smokeFx, a.smokeLift, 12.0f);
            if (fire) draw(a.fireFx, a.fireLift, 18.0f);   // flame over its smoke
        }
    }

    void GameView::updateParticles(float dt) {
        static const bool kLog = tak::devEnv("TAK_FXLOG") != nullptr;
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
            if (p.kind == 1) { p.x += windDX; p.z += windDZ; }
            p.alt += p.valt * dt;
            p.valt -= (p.kind == 1 ? 6.0f : 90.0f) * dt;   // smoke floats, sparks fall
            p.vx *= 0.92f; p.vz *= 0.92f;
        }
        std::erase_if(particles_, [](const Particle& p) { return p.life <= 0; });
    }

    void GameView::drawParticles() {
        // One batched geometry call instead of a state change + FillRect per
        // particle (battles keep hundreds live -- that pattern broke the render
        // batch hundreds of times a frame). Off-screen particles cost nothing.
        float zm = mapView_.zoom();
        partBatch_.clear();
        for (const auto& p : particles_) {
            float sx = (p.x - mapView_.offX()) * zm;
            float sy = (p.z - mapView_.offY()) * zm - p.alt * zm;
            if (sx < -40 || sx > float(winW_) + 40 || sy < -80 || sy > float(winH_) + 40)
                continue;   // cheap screen cull before the fog + lift lookups
            if (!cellVisibleR(p.x, p.z)) continue;
            float t = std::clamp(p.life / std::max(p.maxLife, 1e-3f), 0.0f, 1.0f);
            sx -= terrainLiftX(p.x, p.z) * zm;
            sy -= terrainLift(p.x, p.z) * zm;
            float r = p.size * zm * (p.kind == 1 ? (1.4f - t) : t);
            Uint8 a = Uint8(std::clamp(t * 255.0f, 0.0f, 255.0f));
            pushQuad(partBatch_, sx - r, sy - r, 2 * r, 2 * r, SDL_Color{p.r, p.g, p.b, a});
        }
        if (!partBatch_.empty()) {
            SDL_SetRenderDrawBlendMode(ren_, SDL_BLENDMODE_BLEND);
            SDL_RenderGeometry(ren_, nullptr, partBatch_.data(),
                               int(partBatch_.size()), nullptr, 0);
        }
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
    // One deliberately dropped: retail drew the whole overlay only while SHIFT was
    // physically held (0x4fcc46 polls GetAsyncKeyState(VK_SHIFT)). Here the line is
    // simply on for the current selection, which is what makes a queue visible at
    // the moment you build it.
    //
    // Ownership is gated even though retail did not bother: our click-select has no
    // owner filter, and the render snapshot carries every unit's orders, so drawing
    // trails for anything but our own units would hand the player a readout of
    // enemy intent.
    void GameView::drawOrderTrails(int mvw, int winH) {
        if (selection_.empty() || !cursors_.ok()) return;
        const float zm = mapView_.zoom();
        const float kSpacing = 48.0f;          // world units, straight off retail
        // At low zoom the beads would overlap into a smear (48 world units is only
        // 12 screen px at 0.25x), so stop drawing rather than draw mush -- the same
        // call the waypoint rings already make.
        if (kSpacing * zm < 10.0f) return;
        const int scale = std::clamp(int(zm + 0.5f), 1, 3);
        const size_t frames = std::max<size_t>(1, cursors_.frameCount(tak::CursorId::PathIcon));
        // Retail shows the order line only while SHIFT is physically held -- the
        // whole overlay pass is behind a GetAsyncKeyState(VK_SHIFT) test at
        // 0x4fcc46. The beads are the part that reads as clutter when a big
        // selection is standing still, so they take the gate; the end markers stay
        // on, since they are what makes a selection's orders legible at a glance.
        const bool shiftHeld = (SDL_GetModState() & KMOD_SHIFT) != 0;
        const uint32_t tick = front().gameTick;
        int drawn = 0;
        for (int selId : selection_) {
            const UnitR* up = frameUnitP(selId);
            if (!up || !up->alive() || !up->type) continue;
            if (up->player != localPlayer_) continue;      // never leak enemy intent
            if (up->orders.empty()) continue;
            // Retail beads only its few "focus" units but puts an END MARKER on
            // every selected unit's orders (the dots flag is per unit; the marker
            // is not), so the caps differ on purpose.
            const bool beads = drawn < kTrailUnits && shiftHeld;
            ++drawn;
            float px = up->x, pz = up->z;                  // running position
            for (const auto& o : up->orders) {
                // Only the ends of the player's own orders are line vertices; the
                // A* waypoints between them are the navigator's business, exactly
                // as retail's order list held goals and not path nodes.
                if (!o.goal) continue;
                const float qx = o.x, qz = o.z;
                const bool line = o.targetId == 0;         // attack orders: marker only
                if (beads && line) {
                    float dx = qx - px, dz = qz - pz;
                    float len = std::sqrt(dx * dx + dz * dz);
                    if (len >= 1.0f) {
                        float ux = dx / len, uz = dz / len;
                        float phase = float((tick - o.issuedTick) % 30u) * kSpacing / 30.0f;
                        int bead = 0;
                        for (float t = phase; t < len && bead < kTrailBeads; t += kSpacing, ++bead) {
                            float wx = px + ux * t, wz = pz + uz * t;
                            float sx = (wx - mapView_.offX()) * zm - terrainLiftX(wx, wz) * zm;
                            float sy = (wz - mapView_.offY()) * zm - terrainLift(wx, wz) * zm;
                            if (sx < -16 || sx > float(mvw) + 16 || sy < -16 || sy > float(winH) + 16)
                                continue;
                            cursors_.drawFrame(ren_, tak::CursorId::PathIcon,
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
                tak::CursorId marker = tak::CursorId::Move;
                bool haveMarker = true;
                if (o.buildType)            haveMarker = false;   // the site ghost says it
                else if (o.reclaimFeat)     marker = tak::CursorId::Reclaim;
                else if (o.repairTarget)    marker = tak::CursorId::Repair;
                else if (o.load)            marker = tak::CursorId::Load;
                else if (o.unload)          marker = tak::CursorId::Unload;
                else if (o.guard)           marker = tak::CursorId::Defend;
                else if (o.targetId)        marker = tak::CursorId::Attack;
                else if (o.patrol)          marker = tak::CursorId::Patrol;
                if (haveMarker) {
                    // Draw the marker where the PLAYER CLICKED, not where the
                    // unit will end up. order() snaps a destination the unit
                    // cannot stand on -- a click on a mountain -- to the nearest
                    // cell it fits in, and without this the marker visibly jumps
                    // off the cursor to the spot the engine picked. The bead
                    // trail still runs to the real destination, since that is
                    // the route the unit walks.
                    const float kx = o.clickX != 0.0f ? o.clickX : qx;
                    const float kz = o.clickZ != 0.0f ? o.clickZ : qz;
                    float mx = (kx - mapView_.offX()) * zm - terrainLiftX(kx, kz) * zm;
                    float my = (kz - mapView_.offY()) * zm - terrainLift(kx, kz) * zm;
                    if (mx > -40 && mx < float(mvw) + 40 && my > -40 && my < float(winH) + 40) {
                        size_t n = std::max<size_t>(1, cursors_.frameCount(marker));
                        cursors_.drawFrame(ren_, marker, size_t(tick / 3) % n,
                                           int(mx), int(my), scale);
                    }
                }
                px = qx; pz = qz;                          // advance either way
            }
        }
    }

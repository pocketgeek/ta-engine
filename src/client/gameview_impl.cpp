#include "client/gameview.h"

#include "util/strcase.h"
#include "client/runtimesettings.h"

// Out-of-line GameView method definitions (core concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    std::vector<std::pair<float, float>> GameView::parseStartPositions() const {
        return ta::sim::parseStartPositions(vfs_, mapPath_);
    }

    void GameView::discoSound() {
        for (int p = 0; p < 8 && p < world_.numPlayers(); ++p) {
            bool on = frameDiscoActive(p);
            if (on) {
                // Centroid of this player's (visible) dancing monarchs.
                float cx = 0, cz = 0; int n = 0;
                for (const UnitR* _up : front().live) {
                    const UnitR& u = *_up;
                    if (u.player != p || !u.alive() || !isMonarchType(u.type)) continue;
                    if (!alliedToLocal(p) && !noFog_ && !cellVisibleR(u.x, u.z)) continue;
                    cx += u.x; cz += u.z; ++n;
                }
                if (n) {
                    cx /= n; cz /= n;
                    if (!discoWas_[p]) sounds_.discoAt(cx, cz);               // start the track
                    else sounds_.repositionWorld("disco", cx, cz);           // keep it panned
                }
            }
            discoWas_[p] = on;
        }
    }

    void GameView::headbangSound() {
        for (int p = 0; p < 8 && p < world_.numPlayers(); ++p) {
            bool on = frameHeadbangActive(p);
            if (on) {
                float cx = 0, cz = 0; int n = 0;
                for (const UnitR* _up : front().live) {
                    const UnitR& u = *_up;
                    if (u.player != p || !u.alive() || !isMonarchType(u.type)) continue;
                    if (!alliedToLocal(p) && !noFog_ && !cellVisibleR(u.x, u.z)) continue;
                    cx += u.x; cz += u.z; ++n;
                }
                if (n) {
                    cx /= n; cz /= n;
                    if (!headbangWas_[p]) sounds_.metalAt(cx, cz);
                    else sounds_.repositionWorld("metal", cx, cz);
                }
            }
            headbangWas_[p] = on;
        }
    }

    void GameView::manageMusic() {
        int want = inLobbyPhase() ? 1 : 2;
        if (want == musicMode_) return;
        musicMode_ = want;
        // The menu-launched front-end owns the lobby BGM (so it plays continuously
        // from the menu into the lobby); suppress ours there, but still switch to
        // faction music once the game proper begins.
        if (want == 1) { if (!externalLobbyMusic_) sounds_.startMusic(vfs_, {15}); }
        else if (mp_ && mp_->isSpectator()) sounds_.startMusic(vfs_, allFactionMusicTracks());
        else sounds_.startMusic(vfs_, factionMusicTracks(side_));
    }

    void GameView::applySettings(const ta::Settings& s) {
        // Push the display options that render code reads from globals (smooth art,
        // cursor factor, movie deblocking). This is GameView's one "settings changed"
        // hook, so wiring it here reaches the Esc-menu Options and its DEFAULTS button
        // without either of them having to know the globals exist.
        ta::applyRuntimeSettings(s);
        sounds_.setMasterVolume(s.masterVol);
        sounds_.setMusicVolume(s.bgmVol);
        sounds_.setSfxVolume(s.sfxVol);
        for (int i = 0; i < sounds_.channelCount(); ++i) sounds_.setChannelGain(i, s.chanGain[i]);
        mapView_.setZoomSpeed(s.mouseZoomSpeed);
        edgeScrollSpeed_ = s.edgeScrollSpeed;
        edgeScrollOn_ = s.edgeScroll;
        uiScale_ = s.uiScale;
        buildBarAlign_ = std::clamp(s.buildBarAlign, 0, 2);   // Options: build-menu row
        buildBarScale_ = std::clamp(s.buildBarScale, 0.75f, 4.0f);
        // Bilinear filtering (retail video option): smooth the terrain and the
        // standalone feature/shadow sprites. The packed model-texture atlas stays
        // NEAREST regardless (linear sampling would bleed neighbouring sprites),
        // and fog/minimap are always linear by design.
        bilinear_ = s.bilinear;
        healthBars_ = std::clamp(s.healthBars, 0, 2);
        statsPanel_ = s.statsPanel;                       // Options: minimap-strip readout
        hotkeys_.load(s.hotkeys);                         // Options: rebindable hotkeys
        mapView_.setBilinear(s.bilinear);
        SDL_ScaleMode fm = s.bilinear ? SDL_ScaleModeLinear : SDL_ScaleModeNearest;
        for (auto& [id, a] : featureArt_) {
            for (SDL_Texture* t : a.frames) if (t) SDL_SetTextureScaleMode(t, fm);
            if (a.shadow) SDL_SetTextureScaleMode(a.shadow, fm);
        }
    }

    void GameView::openOptions() {
        if (!settings_) return;
        options_ = std::make_unique<ta::OptionsScreen>(ren_, *settings_,
            [this, fsWas = settings_->fullscreen, vsWas = settings_->vsync]() mutable {
                applySettings(*settings_);
                // Only touch the window/renderer when that display setting actually changed
                // since Options opened -- re-issuing it on every tweak re-commits the
                // (Wayland) surface and later clicks miss/die. See the menu Options callback.
                if (settings_->fullscreen != fsWas) {
                    if (SDL_Window* w = SDL_RenderGetWindow(ren_))
                        SDL_SetWindowFullscreen(w, settings_->fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
                    fsWas = settings_->fullscreen;
                }
                if (settings_->vsync != vsWas) {
                    SDL_RenderSetVSync(ren_, settings_->vsync ? 1 : 0);
                    vsWas = settings_->vsync;
                }
            },
            [this] { saveSettings(*settings_); },
            sounds_.channelCount(),
            [this] { openHotkeys(); },
            [this] { sounds_.reopenDevice(); });   // live output-device switch
    }

    void GameView::writeResume(uint32_t gid, uint64_t tok) const {
        if (mpResumePath_.empty()) return;
        if (FILE* f = std::fopen(mpResumePath_.c_str(), "w")) {
            std::fprintf(f, "%u %llu\n", gid, (unsigned long long)tok);
            std::fclose(f);
        }
    }

    bool GameView::readResume(uint32_t& gid, uint64_t& tok) const {
        if (mpResumePath_.empty()) return false;
        FILE* f = std::fopen(mpResumePath_.c_str(), "r");
        if (!f) return false;
        unsigned long long t = 0;
        bool ok = std::fscanf(f, "%u %llu", &gid, &t) == 2;
        std::fclose(f);
        tok = t;
        return ok;
    }

    bool GameView::haveReclaimer() {
        for (int id : selection_)
            if (const auto* u = frameUnitP(id))
                if (u->alive() && u->type && u->type->isBuilder && u->type->canMove &&
                    u->type->canReclaim && u->player == localPlayer_)
                    return true;
        return false;
    }

    int GameView::firstReclaimer() {
        for (int id : selection_)
            if (const auto* u = frameUnitP(id))
                if (u->alive() && u->type && u->type->isBuilder && u->type->canMove &&
                    u->type->canReclaim && u->player == localPlayer_)
                    return id;
        return 0;
    }

    void GameView::issueReclaimBox(float x0, float z0, float x1, float z1, bool queue) {
        int builderId = firstReclaimer();
        const auto* b = frameUnitP(builderId);
        if (!b) return;
        float minx = std::min(x0, x1), maxx = std::max(x0, x1);
        float minz = std::min(z0, z1), maxz = std::max(z0, z1);
        std::vector<std::pair<float, int>> targets;
        {   // Live read under the lock: one-shot on the drag release, and the worker can
            // reallocate this vector (corpse push_back) under the scan.
            std::unique_lock<std::mutex> lk(simMutex_, std::defer_lock);
            if (useSimThread_) lk.lock();
            for (const auto& f : world_.features()) {
                if (!f.alive || f.x < minx || f.x > maxx || f.z < minz || f.z > maxz) continue;
                float dx = f.x - b->x, dz = f.z - b->z;
                targets.push_back({dx * dx + dz * dz, f.id});
            }
        }
        // Corpses, statues and building rubble in the box too (negative id =
        // dead-unit record -- see World::reclaim).
        for (const UnitR* _cp : front().live) {
            const UnitR& cu = *_cp;
            if (cu.alive() || !cu.corpsePhase || cu.corpseFeat < 0 || !cu.type) continue;
            if (size_t(cu.corpseFeat) >= world_.featureTypes().size() ||
                !world_.featureTypes()[size_t(cu.corpseFeat)].reclaimable)
                continue;
            if (cu.x < minx || cu.x > maxx || cu.z < minz || cu.z > maxz) continue;
            float dx = cu.x - b->x, dz = cu.z - b->z;
            targets.push_back({dx * dx + dz * dz, -cu.id});
        }
        std::sort(targets.begin(), targets.end());
        bool first = true;
        for (auto& [d, fid] : targets) {
            ta::net::Command c;
            c.kind = ta::net::Cmd::Reclaim;
            c.unitId = builderId;
            c.targetId = fid;
            c.queue = uint8_t((first && !queue) ? 0 : 1);   // first clears, rest append
            issue(c);
            first = false;
        }
        if (!targets.empty()) {
            notice_ = "RECLAIM " + std::to_string(targets.size());
            noticeTimer_ = 2;
            voice(builderId, "move");
        }
    }

    std::pair<int, size_t> GameView::keytestPickOwnUnit() const {
        int pick = -1, any = -1;
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (!u.alive() || u.player != 0 || !u.type) continue;
            any = u.id;
            if (u.type->isBuilder) pick = u.id;
        }
        return { pick >= 0 ? pick : any, front().live.size() };
    }

    void GameView::issue(ta::net::Command c) {
        if (replayMode_ || spectating_) return;   // watch-only: can't order units
        c.player = uint8_t(localPlayer_);
        if (mp_) outbox_.push_back(c);
        else apply(c);
    }

    void GameView::startReplay(ta::sim::MatchConfig cfg,
                     std::vector<ta::net::Bundle> bundles,
                     const std::string& mission) {
        // Read through this view's own VFS (we own it), not a caller pointer.
        cfg.vfs = &vfs_;
        cfg.mapPath = mapPath_;
        std::vector<std::pair<float, float>> spots;
        if (!mission.empty()) {
            // A CAMPAIGN recording rebuilds the mission -- its placements and its
            // in-sim script -- exactly as the game did. Replaying it through
            // setupMatch produced a plain skirmish on the mission's map: none of the
            // placed units, none of the scripting, so the recorded commands landed in
            // a world that had nothing to do with the one they came from. The
            // recorded mission stem is new in format 6; before it, the loader could
            // not have known.
            missionStem_ = mission;
            int human = 0;
            ta::sim::MissionSetup ms;
            if (ta::sim::setupMission(world_, registry_, vfs_, mission, human, &ms))
                spots = ms.slotPos;
            else
                std::fprintf(stderr, "replay: mission '%s' not in this data\n",
                             mission.c_str());
        } else {
            spots = ta::sim::setupMatch(world_, registry_, cfg);
        }
        replayBundles_ = std::move(bundles);
        replayMode_ = true;
        noFog_ = true;             // a spectator sees the whole map
        localPlayer_ = 0;
        world_.setVisPlayer(-1);   // no fog needed (noFog_) -> skip the O(units) fog pass
        side_ = "ara";
        loadPanel(side_);
        loadGui(side_);
        if (!spots.empty())
            mapView_.setOffset(spots[0].first - 640 / 0.9f, spots[0].second - 400 / 0.9f);
    }

    void GameView::replayStep(float dt) {
        // The early return used to sit HERE, which quietly broke the last snapshot:
        // captureFrame only publishes the buffer, while the registration that makes a
        // unit drawable happens in cosmeticStep (it walks front().live and calls
        // registerUnit). Stop calling cosmeticStep and any unit that FIRST appears in
        // the final snapshot is never registered -- so a replay consumed in one frame
        // still renders an empty map, which is exactly the bug this was meant to fix.
        // Publishing and registering are two steps and both have to keep running.
        //
        // Pausing is the same story: a paused replay still has to draw what it froze on.
        if (!paused_ && replayTick_ < replayBundles_.size()) {
            replayAdvance(dt);
        }
        // ...but with the SIM'S time, not the wall clock. Two things follow from that:
        //
        //   paused  -> zero. The pass still has to RUN (registration lives in it), but
        //              real elapsed time would keep particles moving, expire effects and
        //              rings and drift flyer altitude in a scene that is supposed to be
        //              frozen. Zero registers without animating.
        //   playing -> dt * speedMult(), because replayAdvance advances the sim by
        //              exactly that. Unscaled dt made effects outlive combat by the
        //              speed factor -- ten times longer at 10x playback, and expiring
        //              too fast when slowed.
        cosmeticStep(paused_ ? 0.0f : dt * speedMult());
    }

    void GameView::replayAdvance(float dt) {
        replayAccum_ += dt * speedMult();
        int guard = 0;
        bool advanced = false;
        while (replayAccum_ >= 1.0f / 30.0f && replayTick_ < replayBundles_.size() && guard < 64) {
            const auto& bd = replayBundles_[replayTick_];
            for (const auto& c : bd.cmds) apply(c);
            for (const auto& e : bd.events) applyEvent(e);
            world_.tick(1.0f / 30.0f);
            ++replayTick_;
            // COMPARE against what the original game computed at this tick. Recording
            // the checkpoints was only half of it -- playback printed the count and
            // checked nothing, so a replay that diverged still ran happily to the end
            // and looked like a faithful reproduction. Report the FIRST divergence and
            // then stop reporting: after the first, every later tick differs too and
            // the rest is noise.
            if (replayCheckAt_ < replayChecks_.size()) {
                const auto& ck = replayChecks_[replayCheckAt_];
                if (ck.tick + 1 == uint32_t(replayTick_)) {
                    ++replayCheckAt_;
                    const uint64_t mine = world_.stateHash();
                    if (mine != ck.hash && !replayDiverged_) {
                        replayDiverged_ = true;
                        std::fprintf(stderr,
                            "replay: DIVERGED at tick %u -- recorded %016llx, replayed "
                            "%016llx\n", ck.tick, (unsigned long long)ck.hash,
                            (unsigned long long)mine);
                        postNotice("REPLAY DIVERGED FROM THE RECORDING", 8);
                    }
                } else if (ck.tick + 1 < uint32_t(replayTick_)) {
                    ++replayCheckAt_;   // checkpoint we stepped past; keep in step
                }
            }
            replayAccum_ -= 1.0f / 30.0f;
            ++guard;
            advanced = true;
        }
        // PUBLISH what we just simulated, exactly as update() does for a live game
        // (simStep ends with captureFrame, then cosmeticStep runs). replayStep did
        // neither, so front() was never filled: the render reads front().live, which
        // stayed empty, and playback drew the terrain with none of the recorded
        // armies on it. Effects follow the same snapshot, so cosmeticStep comes after.
        if (advanced) captureFrame();
    }

    size_t GameView::aliveUnits() const {
        size_t n = 0; for (auto& u : world_.units()) if (u.alive() && u.type) ++n; return n;
    }

    // Build the end-of-game statistics table from the last render frame. Rows follow
    // slot order so the table reads the same for everyone in a multiplayer game; a
    // player still standing when the game ended is timed at the full match length.
    ta::ResultStats GameView::resultStats() const {
        ta::ResultStats st;
        const Frame& f = front();
        st.matchSec = int(f.gameTick / 30);
        static const char* kSides[5] = {"ara", "tar", "ver", "zon", "cre"};
        for (int i = 0; i < 5; ++i) if (side_ == kSides[i]) st.faction = i;
        for (int p = 0; p < f.numPlayers && p < int(f.players.size()); ++p) {
            const PlayerR& pr = f.players[size_t(p)];
            ta::ResultRow row;
            row.name = !playerName_[p & 7].empty() ? playerName_[p & 7]
                                                   : "PLAYER " + std::to_string(p + 1);
            row.colorSlot = colorSlot_[p & 7];
            row.built = pr.built;
            row.kills = pr.kills;
            row.losses = pr.losses;
            row.defeated = pr.defeated;
            row.isLocal = (p == localPlayer_) && !spectating_;
            row.timeSec = pr.defeatedAt >= 0 ? int(pr.defeatedAt) : st.matchSec;
            st.rows.push_back(std::move(row));
        }
        return st;
    }

    void GameView::navyDemo() {
        struct S { const char* t; float x, z; int player; };
        const S fleet[] = {
            {"verflag", 1150, 1250, 0}, {"verman", 1080, 1150, 0},
            {"verman", 1220, 1130, 0},  {"verharp", 1020, 1260, 0},
            {"vertre", 1100, 1360, 0},
            {"npcbotl", 1750, 1500, 1}, {"npcbotl", 1830, 1600, 1},
            {"monpiran", 1700, 1400, 1}, {"monpiran", 1780, 1420, 1},
            {"monpiran", 1650, 1500, 1},
        };
        std::vector<int> a, b;
        for (const auto& sp : fleet) {
            int id = spawn(sp.t, sp.x, sp.z, sp.player == 0 ? 1.57f : -1.57f, sp.player);
            if (id >= 0) (sp.player == 0 ? a : b).push_back(id);
        }
        for (size_t i = 0; i < a.size(); ++i)
            world_.attack(a[i], b[i % b.size()], false);
        for (size_t i = 0; i < b.size(); ++i)
            world_.attack(b[i], a[i % a.size()], false);
        mapView_.setOffset(1500 - 640 / 0.9f, 1380 - 400 / 0.9f);
    }

    void GameView::creonDemo() {
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        const char* squad[] = {"cregod",  "creiron", "creauto", "creauto",
                               "crebeas", "cregatl", "creshoc", "credrag"};
        int i = 0;
        for (const char* t : squad) {
            spawn(t, cx - 100 + float(i % 4) * 60, cz - 40 + float(i / 4) * 70, 1.57f, 0);
            ++i;
        }
        mapView_.setOffset(cx - 640 / 0.9f, cz - 400 / 0.9f);
    }

    void GameView::missionTest() {
        // Plant 4 Watch Towers + escorts inside mission06's forest-edge
        // region (cells 147,119-269,210) to exercise the victory script.
        for (int i = 0; i < 4; ++i)
            spawn("verat", 3300 + float(i % 2) * 60, 2600 + float(i / 2) * 60, 0, 0);
        spawn("versword", 3260, 2700, 0, 0);
        mapView_.setOffset(3300 - 640 / 0.9f, 2620 - 400 / 0.9f);
    }

    void GameView::testBuild() {
        const auto* keep = world_.unit(keepId_);
        if (!keep) return;
        const auto* lode = registry_.find("aralode");
        // Probe outward from the keep for the first legal site.
        for (float r = 90; r < 400; r += 24) {
            for (float a = 0; a < 6.28f; a += 0.5f) {
                float x = keep->x + std::cos(a) * r, z = keep->z + std::sin(a) * r;
                if (world_.canPlace(lode, x, z)) {
                    int id = world_.startBuild(builderId_, lode, x, z);
                    std::printf("testbuild: site id %d at %.0f,%.0f\n", id, x, z);
                    return;
                }
            }
        }
        std::printf("testbuild: no site found\n");
    }

    void GameView::lookAt(float x, float z) {
        mapView_.setOffset(x - 640 / mapView_.zoom(), z - 400 / mapView_.zoom());
    }

    void GameView::soundTest() {
        int id = spawn("araarch", 900, 1000, 0, 0);
        selection_ = {id};
        for (int i = 0; i < 8; i++) voice(id, "move");
    }

    void GameView::faceTest() {
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        // araarch (correct, +h) vs zonhand, both walking east, arrows on.
        float ddx[4]={0,400,0,-400}, ddz[4]={-400,0,400,0};
        for (int i=0;i<4;i++){
            int id=spawn("araarch", cx+ddx[i]*0.15f, cz+ddz[i]*0.15f, 0, 0);
            world_.order(id, cx+ddx[i], cz+ddz[i], false);
        }
        mapView_.setOffset(cx - 640 / mapView_.zoom(), cz - 400 / mapView_.zoom());
    }

    void GameView::fireTest() {
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        int a = spawn("araarch", cx - 40, cz, 1.57f, 0);
        int e = spawn("tararch", cx + 200, cz, -1.57f, 1);
        world_.attack(a, e, false);
        // Aim-pipeline check: a Veruna watch tower (5-TURN AimWeapon) with an
        // enemy off-axis to its north-east -- the turret must visibly swing to
        // face it (auto-acquire), proving the AimWeapon heading sign.
        int tw = spawn("vertower", cx - 40, cz + 220, 0.0f, 0);
        int zm = spawn("tarzom", cx + 160, cz + 100, -1.57f, 1);
        world_.attack(tw, zm, false);   // explicit order: aim runs even without LOS
        // Feature-burning check: park a victim right beside a flamable feature
        // and have a dragon breathe on it -- the splash must ignite the tree
        // (spread + burnt swap then follow on their own).
        float tx = cx + 60, tz = cz - 60;
        for (const auto& ft : world_.features())
            if (ft.alive && ft.type >= 0 &&
                world_.featureTypes()[size_t(ft.type)].flamable) {
                float ddx = ft.x - cx, ddz = ft.z - cz;
                if (ddx * ddx + ddz * ddz < 400 * 400) { tx = ft.x + 20; tz = ft.z; break; }
            }
        int dr = spawn("tardrag", tx - 260, tz - 40, 1.57f, 1);
        int ar = spawn("araarch", tx, tz, -1.57f, 0);
        world_.attack(dr, ar, false);
        // Statue-death check: the Basilisk's gaze petrifies -- the victim must
        int bs = spawn("zonbasil", tx - 120, tz + 90, 1.57f, 1);
        int vic = spawn("arabow", tx + 40, tz + 90, -1.57f, 0);
        world_.attack(bs, vic, false);
        // Ordered corpse-reclaim check: a builder is sent (negative target id)
        // to consume a fresh corpse -- the body must vanish when it arrives.
        int rcv = spawn("arasword", cx - 200, cz + 120, 0.0f, 0);
        if (auto* rd = world_.unit(rcv)) rd->hp = 0;   // dies this tick, normal corpse
        int rcb = spawn("arabuild", cx - 250, cz + 160, 1.57f, 0);
        world_.reclaim(rcb, -rcv, false);
        // Animate check: an idle necromancer beside the (soon) archer corpse
        // must channel and raise a Ghoul from it. Hold-fire stance so it never
        // auto-acquires (the channel needs it order-free).
        int nec = spawn("tarpries", tx + 30, tz - 40, -1.57f, 1);
        if (auto* np = world_.unit(nec)) np->stance = 2;
        // Watch the burn, not the tower: centre the camera on the target tree.
        mapView_.setOffset(tx - 640 / mapView_.zoom(), tz - 400 / mapView_.zoom());
    }

    void GameView::lodeTest() {
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        spawn(lodeUnit.empty()?"zonlode":lodeUnit, cx, cz, 3.14159f, 0);
        mapView_.setOffset(cx - 640 / mapView_.zoom(), cz - 400 / mapView_.zoom());
    }

    void GameView::guardTest() {
        // Squad guards the first unit; the first unit marches east alone.
        int leader = -1;
        for (auto& u : world_.units())
            if (u.alive() && u.player == 0 && u.type && u.type->canMove) {
                if (leader < 0) leader = u.id;
                else world_.guard(u.id, leader, false);
            }
        if (leader >= 0) world_.order(leader, 1500, 950, false);
    }

    void GameView::marchTo(float dx, float dz) {
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        for (auto& u : world_.units())
            if (u.player == localPlayer_ && u.type && u.type->canMove) {
                ta::net::Command c;
                c.kind = ta::net::Cmd::AttackMove;
                c.unitId = u.id;
                c.x = cx + dx;
                c.z = cz + dz;
                issue(c);
            }
    }

    void GameView::setPlayerColor(int player, int slot) {
        if (player >= 0 && player < 8 && slot >= 0) colorSlot_[player] = slot;
    }

    void GameView::simStep(float dt) {
        int64_t _sim0 = int64_t(SDL_GetPerformanceCounter());
        // Advance the sim in sub-steps capped at 1/30s so fast speeds (or a laggy
        // frame) can't move a unit far enough to tunnel a wall; effects/AI below use
        // the full scaled dt (they only interpolate, so a big step is harmless).
        for (float rem = dt, guard = 0; rem > 1e-5f && guard < 16; ++guard) {
            float step = std::min(rem, 1.0f / 30.0f);
            world_.tick(step);
            rem -= step;
        }
        profSimTicks_ += int64_t(SDL_GetPerformanceCounter()) - _sim0;
        // Scenario (.crt) "Display" actions: surface the sim runner's messages as
        // HUD notices for the viewing player. Drained on the sim thread (same as
        // scenario step, so no race on its queue); postNotice defers to main.
        if (auto* sc = world_.scenario())
            for (auto& m : sc->drainMessages()) postNotice(m.text, 8);
        if (missionVm_) {
            missionVm_->tick(dt);
            // Engine sweep: armed regions fire TriggerHit per player unit
            // inside. One-shot story triggers disarm themselves in-script;
            // viccheck re-arms its counting region every loop.
            trigTimer_ -= dt;
            if (trigTimer_ <= 0) {
                trigTimer_ = 0.3f;
                for (auto& [rid, r] : regions_) {
                    if (!r.armed) continue;
                    for (auto& u : world_.units()) {
                        if (!u.alive() || u.embarked() || u.player != 0) continue;
                        int cx = int(u.x) / 16, cz = int(u.z) / 16;
                        bool inside = r.rect
                            ? (cx >= r.a && cz >= r.b && cx <= r.c && cz <= r.d)
                            : ((cx - r.a) * (cx - r.a) + (cz - r.b) * (cz - r.b) <=
                               r.c * r.c);
                        if (inside && missionVm_->threadCount() < 200)
                            missionVm_->start("TriggerHit", {rid, u.id, 0});
                    }
                }
            }
            if (trace_) {
                static float dbg = 0;
                dbg += dt;
                if (dbg > 2) {
                    dbg = 0;
                    std::printf("MSTAT s0=%d threads=%zu pcs:",
                                missionVm_->getStatic(0), missionVm_->threadCount());
                    std::map<uint32_t, int> hist;
                    for (auto pc : missionVm_->threadPcs()) ++hist[pc];
                    for (auto& [pc, n] : hist) std::printf(" %u x%d", pc, n);
                    std::printf("\n");
                }
            }
            for (auto& u : world_.units()) {
                if (u.player != 0) continue;
                if (u.justBuilt) missionVm_->start("UnitCreated", {u.justBuilt, 0});
                bool wasBuilding = building_.count(u.id) != 0;
                if (u.underConstruction) building_.insert(u.id);
                else if (wasBuilding) {
                    building_.erase(u.id);
                    missionVm_->start("UnitCreated", {u.id, 0});
                }
                if (u.alive()) missionAliveP0_.insert(u.id);
            }
            // Death edge: a tracked player-0 unit that is now dead (or removed) fires the
            // mission "UnitDestroyed" hook. Done here on the SIM thread (deterministic, all
            // peers agree) instead of render-side in cosmeticStep, where it raced the sim
            // thread under Stage B and diverged from the referee.
            for (auto it = missionAliveP0_.begin(); it != missionAliveP0_.end();) {
                const auto* mu = world_.unit(*it);
                if (!mu || !mu->alive()) {
                    missionVm_->start("UnitDestroyed", {*it});
                    it = missionAliveP0_.erase(it);
                } else ++it;
            }
        }
        if (amphib_) {
            auto* t = world_.unit(transportId_);
            if (t && t->alive()) {
                if (amphibPhase_ == 0 && int(t->cargo.size()) >= amphibSquad_) {
                    world_.unloadAt(transportId_, amphibSeaX_, amphibSeaZ_);
                    amphibPhase_ = 1;
                } else if (amphibPhase_ == 1 && t->cargo.empty()) {
                    for (auto& u : world_.units())
                        if (u.alive() && !u.embarked() && u.player == 0 && u.type &&
                            u.type->canMove && !u.type->canTransport)
                            world_.order(u.id, amphibLandX_, amphibLandZ_, false);
                    amphibPhase_ = 2;
                }
            }
        }
        // Victory check: last team standing. The sim computes winningTeam() and
        // per-player defeated flags each tick (deterministic across peers); the
        // viewer just maps that to this player's win/lose banner. Only armed once
        // at least two teams have fielded units (staged demos may field one).
        if (outcome_ == 0 && !missionStem_.empty()) {
            // Campaign mission: the in-sim god script + the .ota victory/defeat
            // conditions decide the result (last-team-standing doesn't apply -- the
            // objective may be an escort, a timer, a kill-target, etc.).
            outcome_ = world_.missionOutcome();
        } else if (outcome_ == 0) {
            int teamsSeen = 0;
            for (int t = 0; t < world_.numPlayers(); ++t) {
                bool any = false;
                for (auto& u : world_.units())
                    if (u.alive() && u.type && world_.player(u.player).team == t) { any = true; break; }
                if (any) sawTeam_[t] = true;
                if (sawTeam_[t]) ++teamsSeen;
            }
            int win = world_.winningTeam();
            if (teamsSeen >= 2 && win >= 0) {
                outcome_ = (win == world_.player(localPlayer_).team) ? 1 : -1;
            } else if (world_.player(localPlayer_).defeated && teamsSeen >= 2) {
                // My whole team may still be alive via allies; only lose when the
                // sim says my team is gone, but a solo (FFA) defeat ends my game.
                bool teamAlive = false;
                for (int p = 0; p < world_.numPlayers(); ++p)
                    if (world_.player(p).team == world_.player(localPlayer_).team &&
                        !world_.player(p).defeated) { teamAlive = true; break; }
                if (!teamAlive) outcome_ = -1;
            }
        }
        // The result just landed: ask for this player's replay to be written. REQUEST,
        // not write -- simStep runs on the sim worker, while the net client appends to
        // its recorded bundles and hashes from the main thread. Serializing those
        // vectors from here would read them while they grow. The main thread picks
        // this up (see cosmeticStep) and does the work.
        if (outcome_ != 0) replayWanted_.store(true, std::memory_order_relaxed);
        captureFrame();   // snapshot post-tick unit state for the render (poses + read fields)
    }

    void GameView::captureFrame() {
        // Pick a spare buffer to write: never the published one (the render may pin it on
        // its next beginFrame) and never the one the render is currently pinned on. With
        // three buffers there is always exactly one such spare.
        int w;
        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            for (w = 0; w < 3; ++w) if (w != published_ && w != reading_) break;
        }
        // On the graveyard: world_.units() is append-only and nothing ever clears
        // Unit::type, so this scan copies every unit EVER SPAWNED, not just the living
        // ones, forever. That is real and it is unbounded -- but measured, it does not
        // matter. A/B over 150s at 3803 spawned / 3216 living (15% graveyard), skipping
        // every unit past its visual life (dead and deadFor >= max(corpseUntil, 4)):
        //
        //     with the graveyard   capture = 0.253 ms/tick
        //     skipping it          capture = 0.252 ms/tick
        //
        // ~1.7ns per skipped record, because a dead unit's orders/buildQueue/cargo
        // vectors are EMPTY -- corpses are the cheapest records here, not the dearest.
        // Projected, 20k accumulated dead costs ~34us/tick, 0.1% of the 30Hz budget.
        // Memory is the more real cost: sizeof(UnitR) is 232 bytes across 3 buffers,
        // ~700 bytes per unit ever spawned (2.6 MB here, ~35 MB at 50k spawns).
        // Not optimised deliberately: the skip would change what frameUnitP() reports
        // for a dead unit, which is a real semantic risk, bought for nothing.
        Frame& fb = frameBuf_[w];                // write buffer
        const Frame& pf = frameBuf_[published_]; // previously-published frame (last tick's poses)
        fb.gen = ++captureCounter_;     // records written this pass get gen==fb.gen (=> live this tick)
        fb.live.clear();
        size_t need = world_.units().size() + 1;
        if (fb.units.size() < need) fb.units.resize(need);
        for (const auto& u : world_.units()) {
            if (u.id < 0 || size_t(u.id) >= fb.units.size()) continue;
            UnitR& s = fb.units[size_t(u.id)];
            if (!u.type) { s.seeded = false; s.type = nullptr; continue; }
            // Render-read fields, captured for ALL units (alive + dead-recent: the death
            // animation and the deadFor>=4 cull both need a live value).
            s.gen = fb.gen;
            fb.live.push_back(&s);   // compact live list (mirrors world_.units())
            s.id = u.id; s.type = u.type; s.player = u.player;
            s.hp = u.hp; s.deadFor = u.deadFor;
            s.inTransport = u.inTransport; s.squad = u.squad; s.stance = u.stance;
            s.weaponSlot = u.weaponSlot;
            s.underConstruction = u.underConstruction; s.buildBegun = u.buildBegun;
            s.cloaked = u.cloaked; s.cloakOn = u.cloakOn; s.active = u.active;
            s.paralyzedFor = u.paralyzedFor;
            s.selfDestructT = u.selfDestructT;
            s.buildSiteId = u.buildSiteId; s.reclaimId = u.reclaimId; s.repairId = u.repairId;
            s.buildProgress = u.buildProgress;
            s.buildQueue = u.buildQueue; s.orders = u.orders;
            s.cargo = u.cargo; s.repeatType = u.repeatType;
            s.moving_ = u.moving(); s.walking_ = u.walking();
            s.corpsePhase = !u.alive() && u.deadFor < u.corpseUntil &&
                            u.deadFor >= (u.corpseStatue >= 0 ? 0.0f : 4.0f);
            s.deathType = u.deathType;
            s.severity = u.severity;
            s.corpseFeat = world_.corpseTypeOf(u.type);
            s.speed = u.speed; s.justFired = u.justFired; s.justBuilt = u.justBuilt;
            s.disco = world_.discoActive(u.player);
            s.headbang = world_.headbangActive(u.player);
            s.alliedToLocal = alliedToLocal(u.player);
            // Pose: prev comes from the previously-published frame's curr for this SAME unit
            // (id live last tick + matching type). Interpolate alive units between ticks;
            // a dead unit holds its death pose. A big jump (teleport / id reuse) seeds fresh.
            const UnitR* prev = (size_t(u.id) < pf.units.size() &&
                                 pf.units[size_t(u.id)].gen == pf.gen &&
                                 pf.units[size_t(u.id)].type == u.type)
                                    ? &pf.units[size_t(u.id)] : nullptr;
            if (u.alive() && prev &&
                std::abs(u.x - prev->x) <= 200.0f && std::abs(u.z - prev->z) <= 200.0f) {
                s.px = prev->x; s.pz = prev->z; s.ph = prev->heading;   // interpolate from last tick
                s.x = u.x; s.z = u.z; s.heading = u.heading;
                s.seeded = true;
            } else {
                s.x = u.x; s.z = u.z; s.heading = u.heading;
                s.px = s.x; s.pz = s.z; s.ph = s.heading;
                s.seeded = u.alive();   // dead holds its pose (never interpolated)
            }
        }
        // Player table snapshot for the HUD/scoreboard.
        fb.numPlayers = world_.numPlayers();
        for (int p = 0; p < fb.numPlayers && p < int(fb.players.size()); ++p) {
            const auto& pl = world_.player(p);
            PlayerR& r = fb.players[size_t(p)];
            r.metal = pl.metal; r.energy = pl.energy;
            r.kills = pl.kills; r.unitCount = pl.unitCount;
            r.built = pl.built; r.losses = pl.losses; r.defeatedAt = pl.defeatedAt;
            r.team = pl.team; r.defeated = pl.defeated;
            r.discoLeft = pl.discoLeft; r.headbangLeft = pl.headbangLeft;
        }
        fb.projectiles = world_.projectiles();   // sim push_back/erase each tick -> must copy
        fb.storms = world_.storms();             // ditto: the viewer draws these
        fb.hits = world_.hits();                  // weapon impacts this tick (cleared next tick)
        // ...and queue them for the render as well, so impacts survive a skipped
        // snapshot (see hitQueue_). Oldest out first when the renderer is so far
        // behind that the queue fills.
        if (!fb.hits.empty()) {
            std::lock_guard<std::mutex> hq(hitQueueMutex_);
            for (const auto& h : fb.hits) {
                if (hitQueue_.size() >= kMaxPendingHits) hitQueue_.pop_front();
                hitQueue_.push_back(h);
            }
        }
        fb.shakeReq = world_.shakeRequest();       // copied under the worker's lock
        fb.soundReq = world_.soundRequest();       // ditto (carries a std::string)
        fb.winningTeam = world_.winningTeam();
        fb.gameTick = world_.tickCount();
        // Fog snapshot: copy world_.vis_ into this buffer only when THIS buffer's fog is stale
        // (fog recomputes ~4Hz, so at most ~2 copies per change -- one per buffer). A spectator
        // (noFog_) leaves vis_ empty, so the copy is a no-op and cellVisibleR reveals all.
        if (fb.visGen != world_.visGeneration() || fb.vis.size() != world_.visibility().size()) {
            fb.vis = world_.visibility();
            fb.visW = world_.visW(); fb.visH = world_.visH();
            fb.visGen = world_.visGeneration();
        }
        fb.tickMs = SDL_GetTicks64();
        fb.tickDurMs = (1000.0f / 30.0f) / std::max(0.1f, animSpeed());
        // Publish: the render's next beginFrame() will pin this buffer as front().
        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            published_ = w;
        }
    }

    void GameView::interpPose(const UnitR& s, float& x, float& z, float& heading) const {
        x = s.x; z = s.z; heading = s.heading;
        if (!settings_ || !settings_->smoothMotion || !s.seeded) return;
        float a = interpAlpha_;
        x = s.px + (s.x - s.px) * a;
        z = s.pz + (s.z - s.pz) * a;
        float dh = s.heading - s.ph;
        while (dh >  3.14159265f) dh -= 6.28318531f;   // shortest-path turn
        while (dh < -3.14159265f) dh += 6.28318531f;
        heading = s.ph + dh * a;
    }

    const UnitR& GameView::frameUnit(int id) const {
        static const UnitR kEmpty{};
        const Frame& f = front();
        return (id >= 0 && size_t(id) < f.units.size()) ? f.units[size_t(id)] : kEmpty;
    }

    const UnitR* GameView::frameUnitP(int id) const {
        const Frame& f = front();
        if (id < 0 || size_t(id) >= f.units.size()) return nullptr;
        const UnitR& r = f.units[size_t(id)];
        return (r.gen == f.gen && r.type) ? &r : nullptr;
    }

    const PlayerR& GameView::framePlayer(int p) const {
        static const PlayerR kEmpty{};
        const Frame& f = front();
        return (p >= 0 && p < int(f.players.size())) ? f.players[size_t(p)] : kEmpty;
    }

    bool GameView::cellVisibleR(float x, float z) const {
        const Frame& f = front();
        if (f.vis.empty()) return true;
        int cx = int(x) / 16, cz = int(z) / 16;
        if (cx < 0 || cz < 0 || cx >= f.visW || cz >= f.visH) return false;
        return f.vis[size_t(cz) * f.visW + cx] == 2;
    }

    int GameView::frameQueuedCount(int builderId, const ta::sim::UnitType* type) const {
        const UnitR* b = frameUnitP(builderId);
        if (!b || !type) return 0;
        int n = 0;
        for (const auto* q : b->buildQueue) if (q == type) ++n;
        return n;
    }

    void GameView::cosmeticStep(float dt) {
        // Reads the render SNAPSHOT (front()/frameHits()), never live world_, so it is safe
        // on the main thread while the sim worker ticks. One-tick EVENTS (impacts, justFired)
        // are gated on newTick_ so they fire once per published tick even if cosmeticStep is
        // called more than once against the same pinned snapshot.
        // Deferred replay write, requested by the sim thread when the result landed.
        // Done HERE because this runs on the main thread, which is the only one that
        // may read the net client's recorded bundles while it is still connected.
        if (replayWanted_.exchange(false, std::memory_order_relaxed)) saveNetReplay();
        const bool newTick_ = (front().gen != lastCosmeticGen_);
        lastCosmeticGen_ = front().gen;
        // Weapon impacts this tick: play each weapon's soundhitclass, picking the
        // material-specific variant from the struck unit's bodytype (flesh/armor/..).
        // A storm announces itself ONCE when it is cast and leaves its dissipation
        // art where it dies. The sim only hands us the LIVE storms each tick, so
        // track ids: an id we have not seen is a new cast, an id that vanished has
        // just blown itself out.
        if (newTick_) {
            std::unordered_map<int, StormTrack> live;
            for (const auto& st : front().storms) {
                auto prev = stormsSeen_.find(st.id);
                if (prev == stormsSeen_.end() && st.w && !st.w->soundHit.empty())
                    sounds_.playWorld(st.w->soundHit, st.x, st.z);
                live.emplace(st.id, StormTrack{st.x, st.z, st.w});
            }
            for (const auto& [id, tr] : stormsSeen_)
                if (!live.count(id) && tr.w && !tr.w->wanderEnd.empty())
                    spawnEffectAnim(tr.w->wanderEnd, tr.x, tr.z);
            stormsSeen_ = std::move(live);
        }
        // A mission script asking for a camera shake: fire on the sequence edge.
        if (newTick_) {
            const auto& sr = front().shakeReq;     // snapshot, not live world_
            if (sr.seq != shakeSeqSeen_) { shakeSeqSeen_ = sr.seq; triggerShake(sr.mag, sr.dur); }
            // Scripted mission VO. The name is a bare wav ("monsara1.wav"); play it
            // unpositioned, as narration rather than a world sound. A few of the
            // named lines are simply absent from this install -- skip those.
            const auto& qr = front().soundReq;     // snapshot, not live world_
            if (qr.seq != soundSeqSeen_) {
                soundSeqSeen_ = qr.seq;
                std::string n = qr.name;
                std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                if (auto dot = n.rfind(".wav"); dot != std::string::npos) n.erase(dot);
                if (!n.empty() && sounds_.has(n)) sounds_.play(n);
            }
        }
        // Drained, not gated on newTick_: the whole point is to pick up impacts
        // from ticks whose snapshots the render never saw.
        std::vector<ta::sim::World::HitFx> hitsToShow;
        {
            std::lock_guard<std::mutex> hq(hitQueueMutex_);
            hitsToShow.assign(hitQueue_.begin(), hitQueue_.end());
            hitQueue_.clear();
        }
        for (const auto& h : hitsToShow) {
            // Instant-hit weapons (FBI type = Line of Sight) spawn no projectile, so
            // nothing was ever drawn for them -- the Aramon King's Thunder, the Creon
            // tasers, the Zhon lightning and a dozen more fired completely INVISIBLY.
            // Draw the shot itself: a bolt struck from the firer to the victim, in
            // the weapon's own inner/middle/outer colours where it declares them.
            // Fire breath is a Line-of-Sight weapon too, but retail's flame class
            // draws an emitter, not a bolt -- and we already draw its flame. Excluding
            // it here stops a drake's breath coming with a spurious lightning streak.
            if (h.weapon && h.weapon->beam && !h.weapon->melee &&
                h.weapon->fx != ta::sim::WeaponFx::Fire &&
                (noFog_ || cellVisibleR(h.x, h.z))) {
                BeamFx b;
                b.x1 = h.fromX; b.z1 = h.fromZ;
                b.x2 = h.x;     b.z2 = h.z;
                b.alt1 = unitAltById(h.weapon ? 0 : 0) * 0.0f;   // set below
                b.lightning = h.weapon->fx == ta::sim::WeaponFx::Lightning;
                for (int i = 0; i < 3; ++i) {
                    b.inner[i] = h.weapon->inner[i];
                    b.middle[i] = h.weapon->middle[i];
                    b.outer[i] = h.weapon->outer[i];
                }
                b.alt1 = flyerAltAt(b.x1, b.z1) * 0.8f;
                b.alt2 = flyerAltAt(b.x2, b.z2) * 0.8f;
                // Lifetime = flight time of the virtual shot. Clamped so a zero or
                // silly weaponvelocity can't leave a bolt on screen for a minute.
                float bdx = b.x2 - b.x1, bdz = b.z2 - b.z1;
                float bdist = std::sqrt(bdx * bdx + bdz * bdz);
                b.life = h.weapon->projVel > 0.0f
                             ? std::clamp(bdist / h.weapon->projVel, 0.05f, 1.5f)
                             : 0.16f;
                beams_.push_back(b);
            }
            // A wandering storm grinds EVERY TICK, so routing its hits through the
            // ordinary impact path meant 30 generic dust bursts and 30 hit sounds a
            // second, each one a dozen-plus particles. The storm's own animation and
            // its one-shot cast sound carry it instead.
            if (h.weapon && h.weapon->kind == ta::sim::Weapon::Kind::Wandering) continue;
            if (h.weapon && !h.weapon->soundHit.empty()) {
                const std::string& body = h.target ? h.target->bodyType : std::string("default");
                const std::string* wav = soundClasses_.pick(h.weapon->soundHit, body, salt_++);
                if (!wav) wav = soundClasses_.pick(h.weapon->soundHit, "default", salt_++);
                if (wav) sounds_.playWorld(*wav, h.x, h.z);
            }
            // Impact visual: play the weapon's real GAF/TAF explosion effect
            // (water variant over water); fall back to procedural particles when
            // the class or its art is unavailable.
            if (h.weapon) {
                const std::string& cls = (world_.isWater(h.x, h.z) &&
                                          !h.weapon->waterExplosionClass.empty())
                                             ? h.weapon->waterExplosionClass
                                             : h.weapon->explosionClass;
                // Lift the blast onto an airborne target (shooting down a flyer).
                float tAlt = flyerAltAt(h.x, h.z) * 0.8f;
                if (!spawnEffect(cls, h.x, h.z, tAlt)) spawnImpact(*h.weapon, h.x, h.z, tAlt);
            }
            // Weapon area-effect: expanding shockwave rings (radiusart, staggered
            // by ringdelay) and ground fire (firestarter) at the impact.
            if (h.weapon) {
                float maxR = std::max(h.weapon->aoe * 0.5f, 48.0f);
                for (int i = 0; i < h.weapon->ringCount && i < 3; ++i)
                    if (!h.weapon->radiusArt[i].empty())
                        spawnRing(h.weapon->radiusArt[i], h.x, h.z,
                                  float(i) * h.weapon->ringDelay, h.weapon->ringDur,
                                  h.weapon->spriteCount, maxR);
                if (h.weapon->fireStarter && !world_.isWater(h.x, h.z))
                    spawnEffectAnim("flame", h.x, h.z, 0.0f, 0.0f, 5);   // fire lingers
                // Camera shake for heavy impacts you can actually see.
                if (h.weapon->shakeMag > 0 && cellVisibleR(h.x, h.z))
                    triggerShake(h.weapon->shakeMag, h.weapon->shakeDur);
            }
            if (h.target && h.target->bodyType == "flesh") {
                // Spray blood from the victim's SweetSpot (its body centre) rather
                // than the ground hit point -- retail asks the COB for that piece
                // (SweetSpot -> out-param local 0) and homes hit effects to it.
                float bx = h.x, bz = h.z, ba = flyerAltAt(h.x, h.z) * 0.8f;
                if (h.victimId && (noFog_ || cellVisibleR(h.x, h.z)))
                    if (auto vi = anims_.find(h.victimId); vi != anims_.end() &&
                        vi->second.vm && vi->second.pieceNames)
                        if (const UnitR* v = frameUnitP(h.victimId); v && v->type) {
                            vi->second.vm->call("SweetSpot", {0});
                            const auto& ll = vi->second.vm->lastLocals();
                            int pc = ll.empty() ? -1 : ll[0];
                            if (pc >= 0 && pc < int(vi->second.pieceNames->size())) {
                                float sx, sz, sa;
                                if (pieceWorldFx(*v, vi->second,
                                                 (*vi->second.pieceNames)[size_t(pc)], sx, sz, sa)) {
                                    bx = sx; bz = sz; ba = sa;
                                }
                            }
                        }
                spawnBurst(bx, bz, 5, h.target->blood[0], h.target->blood[1],
                           h.target->blood[2], 26, 1.8f, 0, ba);
            }
            // (mission ScreenShake is handled once per tick, below the hit loop)
            // Damage flinch (retail HitByWeapon callin): args are
            // (damageType, cos*400, sin*400, damage) -- the scripts themselves
            // gate on damage>10 and skip damageType 4 (paralyze), so status
            // weapons map there and everything else passes 0. Direction is the
            // bearing from the victim to the attacker, in rendered-facing space
            // (same convention as the aim driver).
            if (h.victimId)
                if (auto fi = anims_.find(h.victimId);
                    fi != anims_.end() && fi->second.hasFlinch && fi->second.vm)
                    if (const UnitR* v = frameUnitP(h.victimId);
                        v && v->alive() && v->type) {
                        int dtype = (h.weapon && h.weapon->status !=
                                     ta::sim::Weapon::Status::None) ? 4 : 0;
                        // isStructure(), not canMove: drawUnit locks a structure's
                        // yaw to 0, and the Barracks declares canmove=1 with no
                        // velocity (the CLAUDE.md gotcha), so canMove said "rotates"
                        // for a body rendered fixed -- putting every bearing 180 deg
                        // out on a building that spawns at heading pi.
                        bool rot = !v->type->isStructure();
                        float ang = std::atan2(h.fromX - v->x, h.fromZ - v->z) -
                                    (rot ? v->heading : 0.0f);
                        fi->second.vm->start("HitByWeapon",
                            {dtype, int32_t(std::cos(ang) * 400.0f),
                             int32_t(std::sin(ang) * 400.0f), int32_t(h.damage)});
                    }
        }
        // Sim-driven feature fire: burn-anim playback, smoke, burnt-art swaps.
        syncBurningFeatures();
        // Ambient wind: a slow random walk; each shift bumps windGen_ and the
        // per-unit loop below re-sends WindChange to flags/sails as they differ.
        loadMapWind();
        if (animClock_ >= windNext_) {
            // Retail's wind tick (KINGDOMS.icd 0x525170): the next shift lands in
            // 90..360 ticks (3..12s at 30Hz), the speed is re-rolled anywhere in the
            // map's range, and the direction random-walks +-0x2000 (+-45 degrees) --
            // but only when the new speed is non-zero. Ours re-oriented on a 20-40s
            // timer over a hardcoded 50..300, so shipped flags and sails turned about
            // four times too slowly and never reached a gale.
            windNext_ = animClock_ + 3.0f + float(salt_++ % 10);
            windSpeed_ = windMin_ + (windMax_ - windMin_) * (float(salt_++ % 1024) / 1024.0f);
            if (windSpeed_ > 0.0f)
                windHeading_ += (float(salt_++ % 256) / 256.0f - 0.5f) * (6.2831853f / 4.0f);
            ++windGen_;
        }
        // (No world_.clearHits() here: World::tick already clears hits_ at the start of the
        //  next tick, so the viewer-side clear was redundant -- and dropping it keeps the
        //  render path from mutating sim state, a prerequisite for the sim-thread decouple.
        //  See docs/sim-render-decouple-plan.md.)
        updateParticles(dt);
        updateEffects(dt);
        updateRings(dt);
        if (ta::devEnv("TA_STUCKSTAT")) {   // crowd-jam diagnostic
            static float acc = 0; acc += dt;
            if (acc >= 2.0f) {
                acc = 0;
                int moving = 0, stalled = 0, ordered = 0;
                for (const UnitR* _up : front().live) {
                    const UnitR& u = *_up;
                    // isStructure() again: canMove would count the Barracks as a
                    // ground mover that never moves, i.e. permanently "stalled".
                    if (!u.alive() || !u.type || u.type->isStructure() || u.type->canFly ||
                        u.orders.empty() || u.orders.front().targetId != 0) continue;
                    ordered++;
                    if (u.speed > 3.0f) moving++; else stalled++;
                }
                std::printf("stuckstat t=%.0f ordered=%d moving=%d stalled=%d\n",
                            animClock_, ordered, moving, stalled);
                std::fflush(stdout);
            }
        }
        // Register newly-seen units for rendering (load the model + COB once per type,
        // then build a per-unit animation VM and run its Create script). BUDGETED: a huge
        // simultaneous spawn -- the stress test, a mass transport unload, or a big army
        // revealed by fog -- would otherwise build thousands of VMs and run their Create
        // scripts in ONE frame, a multi-hundred-millisecond hitch that "settles" only once
        // every unit is registered. Cap new registrations per frame so the cost spreads
        // over ~a second; the render path already skips units not yet in unitType_, so the
        // stragglers just pop in a few frames later. Purely cosmetic -- the sim already has
        // them (they move/fight); this only gates when the client starts drawing them.
        int regBudget = kRegistrationsPerFrame;
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (u.type && u.alive() && !unitType_.count(u.id)) {
                registerUnit(u.id, u.type);
                if (--regBudget <= 0) break;
            }
        }
        // Kick off the summon fade-in/shimmer for anything just conjured from a
        // building (the producer flags justBuilt for that one tick); then age the
        // active effects and drop finished or dead ones. Cosmetic, viewer-only.
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (u.justBuilt && !birthFx_.count(u.justBuilt)) birthFx_[u.justBuilt] = 0.0f;
        }
        for (auto it = birthFx_.begin(); it != birthFx_.end();) {
            const auto* bu = frameUnitP(it->first);
            it->second += dt;
            if (it->second >= kBirthFxDur || !bu || !bu->alive()) it = birthFx_.erase(it);
            else ++it;
        }
        if (briefTimer_ > 0) briefTimer_ -= dt;
        animClock_ += dt;

        // (Corpses: the sim keeps the dead-unit record for the corpse window and
        // the paint pass draws it wearing the corpse mesh -- see
        // maybeSwapCorpseModel. The old path here tried to spawn a cosmetic
        // FEATURE, which never worked: corpse defs are object= models with no
        // sprite art, so addFeature always bailed before drawing anything.)
        if (noticeTimer_ > 0) noticeTimer_ -= dt;

        // (T-tracking / edge-scroll / shake now run per-frame in cameraFrame, so
        // the camera stays smooth when the net sim stalls.)
        if (follow_ && !front().live.empty()) {
            // Track moving friendly units; fall back to everyone.
            float cx = 0, cz = 0;
            int n = 0;
            for (const UnitR* _up : front().live) {
                const UnitR& u = *_up;
                if (u.alive() && u.player == 0 && u.type && u.type->canMove &&
                    u.moving()) { cx += u.x; cz += u.z; ++n; }
            }
            if (!n)
                for (const UnitR* _up : front().live) {
                    const UnitR& u = *_up;
                    if (u.alive()) { cx += u.x; cz += u.z; ++n; }
                }
            if (n)
                mapView_.setOffset(cx / float(n) - 640 / mapView_.zoom(),
                                   cz / float(n) - 400 / mapView_.zoom());
        }
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (!u.alive() && u.corpsePhase) maybeSwapCorpseModel(u);
            auto it = anims_.find(u.id);
            if (u.justFired && newTick_ && u.type) {
                using Fx = ta::sim::WeaponFx;
                const auto& w = u.type->weapon;
                // Generic firing sounds are a stand-in for units whose COB carries no
                // PLAY_SOUND of its own; units with script audio (attack swooshes,
                // spell cracks) now play those instead -- doubling both was wrong.
                bool scripted = it != anims_.end() && it->second.cobSounds;
                if (scripted) { /* the attack script provides the sound */ }
                else if (w.melee)
                    sounds_.playWorld("ahitfl0" + std::to_string(1 + (salt_++ % 3)), u.x, u.z);
                else if (w.fx == Fx::Fire)
                    sounds_.playWorld(sounds_.has("firedrag") ? "firedrag" : "fireflsh", u.x, u.z);
                else if (w.fx == Fx::Lightning)
                    sounds_.playWorld("lightng" + std::to_string(1 + (salt_++ % 3)), u.x, u.z);
                else
                    sounds_.playWorld("bow2", u.x, u.z);
                // Muzzle world point. Retail asks the COB which piece a weapon fires
                // from -- QueryWeapon writes the emit-piece index to its out-param
                // local 0 (e.g. the tower's emitCan, the drake's emitjim mouth anchor)
                // -- and spawns the flash/emit there. We resolve that piece to a world
                // position via the model tree; without QueryWeapon (or on failure) we
                // fall back to a point just ahead of the unit along its facing.
                float fx = u.x + std::sin(u.heading) * 11.0f;
                float fz = u.z + std::cos(u.heading) * 11.0f;
                float falt = unitAltById(u.id) * 0.8f;   // flyer-effect lift (as death/unitScreen)
                // Muzzle/beam visuals are culled off-screen anyway, and the QueryWeapon
                // COB query + model-tree walk aren't free -- skip them for fogged/off-
                // screen shooters so a big off-screen battle doesn't tax the main thread.
                bool vis = noFog_ || cellVisibleR(u.x, u.z);
                if (vis && !w.melee && it != anims_.end() && it->second.hasQueryWeapon &&
                    it->second.vm && it->second.pieceNames) {
                    auto& fa = it->second;
                    fa.vm->call("QueryWeapon", {0, 0});   // local0=piece OUT (seed 0), local1=weaponNum
                    const auto& ll = fa.vm->lastLocals();
                    int piece = ll.empty() ? -1 : ll[0];
                    if (piece >= 0 && piece < int(fa.pieceNames->size())) {
                        float wx, wz, wa;
                        if (pieceWorldFx(u, fa, (*fa.pieceNames)[size_t(piece)], wx, wz, wa)) {
                            fx = wx; fz = wz; falt = wa;
                        }
                    }
                }
                // Muzzle flash: a quick bright puff at the weapon (skip melee swings).
                if (vis && !w.melee) {
                    Uint8 mr = 255, mg = 235, mb = 150;   // arrow/generic = warm
                    if (w.fx == Fx::Lightning) { mr = 200; mg = 225; mb = 255; }
                    else if (w.fx == Fx::Fire) { mr = 255; mg = 150; mb = 60; }
                    spawnBurst(fx, fz, 4, mr, mg, mb, 14, 1.4f, 0, falt);
                }
                // Fire Breath (a Line-of-Sight beam): a sustained flame STREAM from
                // the dragon's mouth (the QueryWeapon emit piece) to its target, not a
                // single traveling puff. The sim already applied the beam's damage this
                // tick (see World::fire); this is the emitter-only visual. Retail emits
                // for `emittime`; we lay a run of flame sprites along the sightline with
                // growing start delay so the flame reads as sweeping outward, plus a hot
                // gout at the mouth. Re-fires each reload (~3s), a breath pulse.
                if (vis && w.beam && w.fx == Fx::Fire) {
                    float ex = u.x + std::sin(u.heading) * w.range;
                    float ez = u.z + std::cos(u.heading) * w.range;
                    float talt = 0.0f;
                    int tgt = (!u.orders.empty() && u.orders.front().targetId)
                                  ? u.orders.front().targetId : 0;
                    if (const UnitR* t = tgt ? frameUnitP(tgt) : nullptr) {
                        ex = t->x; ez = t->z; talt = unitAltById(t->id);
                    }
                    float dx = ex - fx, dz = ez - fz;   // from the muzzle to the target
                    float dl = std::max(std::sqrt(dx * dx + dz * dz), 1.0f);
                    dx /= dl; dz /= dl;
                    int steps = std::clamp(int(dl / 22.0f), 4, 24);
                    float sweep = std::max(w.emitTime, 0.6f);   // stream reach-out time
                    for (int s = 0; s < steps; ++s) {
                        float f = steps > 1 ? s / float(steps - 1) : 0.0f;
                        float d = f * dl;   // from the muzzle (0) out to the target (dl)
                        float wob = ((s * 811 + int(salt_) * 7) % 9 - 4) * 2.4f;
                        float px = fx + dx * d - dz * wob;
                        float pz = fz + dz * d + dx * wob;
                        float alt = falt + (talt - falt) * f;   // descend to the target's height
                        spawnEffectAnim("flame", px, pz, sweep * f * 0.5f, 0.0f, 1, alt);
                    }
                    spawnBurst(fx, fz, 8, 255, 210, 90, 20, 2.0f, 0, falt);
                    salt_++;
                }
                // Play the unit's own firing animation while standing. Flyers
                // keep their continuous flight threads running, so don't reset.
                if (it != anims_.end() && !u.walking()) {
                    auto& fa = it->second;
                    // Reset only MANUALLY-driven walk-cycle units: for ships and
                    // MeleeControl-driven walkers the reset would permanently kill
                    // the Create ambients (gait driver, wakes, veteran swaps) that
                    // nothing restarts -- the Ark froze after its first AA shot
                    // because of exactly this.
                    if (!fa.flying && fa.hasWalk && !fa.hasMelee) {
                        fa.vm->reset();
                        fa.vm->setStatic(0, 0);
                    }
                    // Multi-weapon units branch FireWeapon on arg0 = the weapon index
                    // (araat: attack_1 vs attack_2, i.e. which archer looses). The sim
                    // doesn't tell us which weapon fired, so alternate so both crews fire
                    // over time. Single-weapon units keep the argless call (arg0=0).
                    int nw = u.type ? int(u.type->weapons.size()) : 1;
                    if (nw > 1) {
                        // No "MeleeAttack" in the chain: ZERO of the 204 shipped
                        // COBs define it, so that call could only ever fail. The
                        // melee animation comes from MeleeControl, which Create
                        // starts as a thread and which calls attack1 itself.
                        fa.vm->start("FireWeapon", {fa.fireSlot}) || fa.vm->start("attack1") ||
                            fa.vm->start("fire");
                        fa.fireSlot = (fa.fireSlot + 1) % nw;
                    } else {
                        fa.vm->start("FireWeapon") || fa.vm->start("attack1") ||
                            fa.vm->start("fire");
                    }
                    fa.walking = false;
                    fa.firing = true;
                }
            }
            if (it == anims_.end()) continue;
            auto& a = it->second;
            if (!u.alive()) {
                if (!a.dying) {
                    a.dying = true;
                    a.vm->reset();
                    a.vm->setStatic(0, 0);
                    // Retail order (icd 0x512610): Killed(severity, corpseOut,
                    // deathType) runs first -- deathType 3 (explosion kill)
                    // EXPLODEs every piece there; no shipped script reads the
                    // severity, and the corpse decision is the SIM's
                    // (corpseUntil), so the out-param is ignored. Then
                    // Dying(deathType): the death cry (92 units PLAY_SOUND
                    // there, zero in `death`), the fall-over via CALL death,
                    // the final EXPLODEs.
                    int32_t dtype = u.deathType;
                    if (dtype >= 14) {
                        // Petrified/frozen: retail skips Killed AND Dying
                        // (severity forced 0) -- the victim simply freezes in
                        // its current pose and stands as the statue. The reset
                        // above already halted every thread; nothing plays.
                    } else {
                        a.vm->start("Killed", {int32_t(u.severity), 0, dtype});
                        a.vm->start("Dying", {dtype}) || a.vm->start("death");
                    }
                    const std::string& id = u.type->id;
                    if (a.cobSounds) { /* the Dying script plays its own death cry */ }
                    else if (sounds_.has(id + "die1")) sounds_.playWorld(id + "die1", u.x, u.z);
                    else if (sounds_.has(id + "die2")) sounds_.playWorld(id + "die2", u.x, u.z);
                    // Death effect: a real GAF explosion sized to the unit (bigger
                    // footprint => bigger blast), plus blood particles for flesh.
                    int foot = std::max(u.type->footX, u.type->footZ);
                    const char* deathCls = foot >= 3 ? "large explosion"
                                         : foot == 2 ? "medium explosion"
                                                     : "small explosion";
                    float dAlt = unitAltById(u.id) * 0.8f;   // a flyer explodes mid-air
                    spawnEffect(deathCls, u.x, u.z, dAlt);
                    if (u.type->bodyType == "flesh") {
                        spawnEffect("blood explosion", u.x, u.z, dAlt);
                        spawnBurst(u.x, u.z, 14, u.type->blood[0], u.type->blood[1],
                                   u.type->blood[2], 40, 2.2f, 0, dAlt);
                    } else
                        spawnBurst(u.x, u.z, 10, 110, 100, 90, 30, 2.4f, 1, dAlt);
                    // (The mission "UnitDestroyed" hook now fires deterministically in
                    // simStep on the sim thread -- see the death-edge detection there --
                    // rather than here off the render-side death animation.)
                }
                continue;   // VM advanced in the parallel pass below
            }
            if (a.flying) {
                // Walk the ground datum toward the sector value rather than
                // snapping to it. The dilated datum is a step function -- it jumps
                // the moment the unit crosses a 128-unit sector boundary -- so
                // reading it directly makes a flyer hop vertically at every
                // boundary. Retail's flyer mover never does that: its vertical
                // servo moves Y at most max(1, speed/4) world units per 30Hz tick
                // toward the commanded altitude, which turns those steps into a
                // climb. Same rate here, converted to per-second.
                float wantGround = flyerGround(u.x, u.z);
                if (!a.groundInit) { a.groundY = wantGround; a.groundInit = true; }
                else {
                    // Retail's step is max(1, speed/4) world units per 30Hz TICK,
                    // and its speed is per-tick too. Ours is px/SECOND, so the
                    // conversion is max(1, (speed/30)/4) * 30 == max(30, speed/4)
                    // units per second. Getting that wrong by the 30x makes the
                    // servo cross the entire relief in a twentieth of a second,
                    // i.e. it smooths nothing and the steps come straight back.
                    float rate = std::max(30.0f, std::abs(u.speed) / 4.0f);
                    float step = rate * dt;
                    a.groundY += std::clamp(wantGround - a.groundY, -step, step);
                }
                // Take off when moving, settle back to the ground when idle.
                float cruise = u.type ? u.type->cruiseAlt : 0.0f;
                // Flyers cruise while doing anything — moving, or hovering to
                // conjure — and touch down when idle, playing the `land` script
                // for a proper folded-wing landed pose (not the wings-spread
                // rest "T-pose").
                // Also stay airborne for any builder work -- reclaiming/clearing and
                // repairing, not just conjuring -- so a flyer hovers over the job (and
                // touches down only when truly idle) exactly as it does while building.
                bool busy = u.walking() || !u.orders.empty() ||
                            u.buildSiteId != 0 || u.hasQueuedBuild() ||
                            u.reclaimId != 0 || u.hasQueuedWork() || u.repairId != 0;
                float target = busy ? cruise : 0.0f;
                float step = std::max(cruise, 1.0f) / 0.7f * dt;   // ~0.7s to cruise
                a.altitude += std::clamp(target - a.altitude, -step, step);

                // Run the flight animation whenever she is airborne (always, in
                // practice, since idle only settles to a low hover).
                bool air = a.altitude > std::max(cruise, 1.0f) * 0.3f;
                if (a.hasFlightSM) {
                    // Drake VTOL state machine: retail's engine only ever calls
                    // BeginFlight (takeoff) and BeginLanding (descent), plus
                    // setSFXoccupy to report active/occupied. The Create-started
                    // FlightControl loop then plays launch->fly->soar itself, gated
                    // on airborne(static7) and active(static5); RestoreWatcher eases
                    // back to rest on halt. No reset() -- the loops must keep running.
                    if (air != a.airborne) {
                        a.airborne = air;
                        if (air) {
                            a.vm->start("setSFXoccupy", {5});   // active -> flap
                            a.vm->start("BeginFlight");         // takeoff: launch then fly
                        } else {
                            a.vm->start("BeginLanding");        // descent -> land thread
                            a.vm->start("setSFXoccupy", {0});   // inactive -> settle/restore
                        }
                    }
                } else if (a.flyAmbient) {
                    // Ambient airship: its Create-started MotionControl / rotor loops run
                    // continuously (never reset). Feed it only the moving/occupy signal so
                    // MotionControl picks fly vs restore: setSFXoccupy(creaeri) / MoveRate
                    // (verball, tarship). Gate on airborne so it animates while aloft.
                    if (air != a.airborne) {
                        a.airborne = air;
                        int r = air ? 5 : 0;
                        a.vm->start("setSFXoccupy", {r});
                        a.vm->start("MoveRate", {r});
                    }
                } else if (air) {
                    // The flyer `fly` script gates its whole body/wing animation on
                    // a static whose index differs per unit (zonhunt=8, zongod and
                    // zonharp=7); a.flyGate is read from the bytecode. Setting the
                    // wrong index leaves `fly` inert — the unit sits in its wings-
                    // spread rest pose (a T-pose) and never picks up the fly-pose
                    // body turn, so it reads static and backward.
                    if (!a.airborne) {
                        a.airborne = true;
                        a.vm->reset();
                        a.vm->setStatic(a.flyGate, 1);   // gate that "fly" animates on
                        a.vm->start("fly");
                    } else if (a.vm->threadCount() == 0) {
                        // Keep the airborne pose looping. While conjuring, loop `build`
                        // (retail's BuildControl loops it) instead of `fly`, else the
                        // one-shot build plays once and the flyer freezes mid-air for
                        // the rest of the job.
                        a.vm->start(a.building ? "build" : "fly");
                    }
                } else if (a.airborne) {
                    a.airborne = false;
                    a.vm->reset();
                    a.vm->setStatic(a.flyGate, 0);
                    a.vm->start("land") || a.vm->start("restore_x");   // landed pose
                }
            } else if (a.hasMelee) {
                // Retail-driven mover: its Create ambients ARE the gait driver --
                // MoveWatcher polls GET 29 (speed) into the walk static and
                // MeleeControl CALLs walk / walk_water / walk_road and the restore
                // poses itself. Nothing to do per tick, and nothing here may reset
                // the VM (that would kill the ambients permanently -- retail never
                // resets a living unit's VM).
            } else if (a.hasWalk) {
                bool m = u.walking();
                if (m != a.walking) {
                    a.walking = m;
                    a.vm->reset();
                    a.vm->setStatic(a.moveGate, m ? 1 : 0);   // per-COB gate (vermage=3, most=0)
                    if (m) { a.vm->start("walk") || a.vm->start("walk_legs"); }
                    else { a.vm->start("restore_x") || a.vm->start("restore_legs"); }
                    a.firing = false;
                } else if (m && a.vm->threadCount() == 0) {
                    // The walk script is single-pass; the engine re-invokes it
                    // each cycle while the unit keeps moving.
                    a.vm->start("walk") || a.vm->start("walk_legs");
                }
            } else if (u.type->maxVel > 0) {
                // No-walk movers (ships, wheeled vehicles): retail drives these via
                // the MoveRate engine callin. The Ark's MotionControl ambient (from
                // Create) polls the static that MoveRate sets and runs the rowing /
                // sail / rudder loops -- without the callin the hull glides with
                // frozen oars. NO reset here: the Create ambients must keep running.
                bool m = u.walking();
                if (m != a.walking) {
                    a.walking = m;
                    a.vm->start("MoveRate", {m ? 100 : 0});
                }
            }
            // Turret aim (retail AimWeapon pipeline, display-only). The engaged
            // target -- ordered attack or auto-acquire -- always sits at
            // orders.front (auto-acquire INSERTS one), so the snapshot already
            // has it. Retail convention (vertower disasm): arg0 = 32768 + signed
            // relative heading in COB angle units (the script subtracts 32768
            // and TURNs its y-axis to it), arg1 = pitch (x-axis TURN, we pass 0
            // on our flat battlefield), arg2 is echoed back via SET 22 (aim-
            // ready token; we don't gate sim fire on it -- hash safety).
            // TargetCleared(token) on loss runs the restore path. Sign verified
            // visually (vertower firetest): script space takes PLUS the sim-
            // relative bearing -- the piece-Y render negation and the mirrored
            // model basis cancel.
            if (a.hasAim) {
                int tgt = 0;
                if (!u.orders.empty()) {
                    const auto& o = u.orders.front();
                    if (o.targetId && !o.load && !o.guard) tgt = o.targetId;
                }
                const UnitR* t = tgt ? frameUnitP(tgt) : nullptr;
                if (t && t->alive() && !u.underConstruction) {
                    if (tgt != a.aimTarget || animClock_ >= a.aimNext) {
                        a.aimTarget = tgt;
                        a.aimNext = animClock_ + 0.33f;   // retail re-aims each cycle
                        constexpr float kTau = 6.2831853f;
                        // Structures render yaw-locked (facing 0 in drawUnit) even
                        // though the sim turns their heading toward the target --
                        // aim against the RENDERED facing, not the sim heading.
                        bool rotates = !u.type->isStructure();   // see HitByWeapon above
                        float rel = std::atan2(t->x - u.x, t->z - u.z) -
                                    (rotates ? u.heading : 0.0f);
                        while (rel > kTau / 2) rel -= kTau;
                        while (rel < -kTau / 2) rel += kTau;
                        int32_t h16 = 32768 + int32_t(rel * (65536.0f / kTau));
                        // Multi-weapon turrets (araat: two independent bows) branch
                        // AimWeapon on arg2 = the weapon index -- aim EACH so both crews
                        // track the target; a single AimWeapon left the other bow static.
                        // Single-weapon units keep the historical arg2=1 (they ignore it).
                        int nw = int(u.type->weapons.size());
                        bool ok;
                        if (nw > 1)
                            for (int ws = 0; ws < nw && ws < 3; ++ws)
                                ok = a.vm->start("AimWeapon", {h16, 0, ws});
                        else
                            ok = a.vm->start("AimWeapon", {h16, 0, 1});
                        static const bool kAimLog = ta::devEnv("TA_AIMLOG") != nullptr;
                        if (kAimLog)
                            std::fprintf(stderr, "aim u%d tgt%d rel=%.2f h16=%d ok=%d\n",
                                         u.id, tgt, rel, h16, int(ok));
                    }
                } else if (a.aimTarget) {
                    a.aimTarget = 0;
                    int nw = int(u.type->weapons.size());
                    if (nw > 1)
                        for (int ws = 0; ws < nw && ws < 3; ++ws)
                            a.vm->start("TargetCleared", {ws});
                    else
                        a.vm->start("TargetCleared", {1});
                }
            }
            // Wind delivery (retail WindChange(speed, heading)): the script TURNs
            // flag/sail pieces straight to arg1, so pass the wind bearing in
            // rendered-facing space (structures: absolute; movers: minus heading).
            if (a.hasWind && a.windStamp != windGen_) {
                a.windStamp = windGen_;
                constexpr float kTau = 6.2831853f;
                bool rotates = !u.type->isStructure();   // see HitByWeapon above
                float w = windHeading_ - (rotates ? u.heading : 0.0f);
                while (w > kTau / 2) w -= kTau;
                while (w < -kTau / 2) w += kTau;
                a.vm->start("WindChange", {int32_t(windSpeed_),
                                           int32_t(w * (65536.0f / kTau))});
            }
            // Builders: the conjure/build animation while actively working a site
            // (constructing, repairing, or reclaiming). Retail drives this via the COB
            // StartBuilding/StopBuilding hooks -- StartBuilding raises the script's own
            // "am building" gate, StopBuilding clears it. This now covers FLYING builders
            // too (arafly/zonhunt via their FlightControl, tarpries via a direct `build`),
            // which used to just flap in place while conjuring.
            if (u.type->isBuilder && !isStructure(u.type)) {
                // One-shot per JOB, not a loop: the retail build scripts are a single
                // pose performance (zonhand's whip swing + PLAY_SOUND crack, ~4s of
                // keyframes, then RETURN) after which the builder HOLDS the final
                // working stance until StopBuilding. Re-invoking on thread death
                // (the walk pattern) replayed the whole performance -- and its sound
                // -- endlessly. Retrigger only for a NEW job (a queued neighbouring
                // site started without walking in between).
                int workId = u.buildSiteId ? u.buildSiteId
                           : u.repairId    ? u.repairId
                           : u.reclaimId   ? u.reclaimId : 0;
                bool working = !u.walking() && workId != 0;
                if (working != a.building || (working && workId != a.workId)) {
                    a.building = working;
                    a.workId = working ? workId : 0;
                    if (!a.flying) {
                        // Ground builder.
                        if (working) a.vm->start("StartBuilding") || a.vm->start("startbuild");
                        else {
                            a.vm->start("StopBuilding");
                            a.vm->start("restore_x") || a.vm->start("RestoreAfterDelay");
                        }
                    } else if (a.hasFlightSM) {
                        // SM flyer (arafly/zonhunt): StartBuilding sets the build static
                        // and the Create-run FlightControl plays `build`; no reset.
                        a.vm->start(working ? "StartBuilding" : "StopBuilding");
                    } else {
                        // Generic flyer builder (tarpries): no FlightControl loop, so the
                        // flight re-kick above loops `build` while a.building (matching
                        // retail's BuildControl). Just flip the state + set the COB gate.
                        a.vm->start(working ? "StartBuilding" : "StopBuilding");
                    }
                }
            }
            // Buildings: yard/production anims. Detect via isStructure (maxVel<=0),
            // NOT !canMove -- the Keep/Castle/Hell carry canmove=1 in their FBI, so
            // the old test skipped every factory and none of them ever animated
            // while training.
            if (u.type && isStructure(u.type)) {
                bool busy = !u.buildQueue.empty();
                if (busy != a.producing) {
                    a.producing = busy;
                    // Do NOT reset() the VM here. A building's Create script starts
                    // persistent ambient threads (flag wave, SmokeControl,
                    // DamageFlameControl); reset() clears every thread, so after a
                    // factory produced even once its flags/smoke stopped for the rest
                    // of the game -- the same regression the ship/MeleeControl movers
                    // already guard against. The production hook is a self-contained
                    // pose loop that coexists with those ambients.
                    //
                    // Name order matters. `startbuild` is the Aramon (and most-faction)
                    // spelling; `start_building` is the Veruna factory (vercastl)
                    // spelling. OpenYard/CloseYard are deliberately NOT in the chain:
                    // they are value-only stubs (they set the pathfinding yard flags
                    // 18/19 but MOVE no piece), so on a unit that defines both they
                    // return true and MASK the real door animation. Activate is the
                    // last-resort producer hook.
                    if (busy) {
                        a.vm->start("startbuild") || a.vm->start("start_building") ||
                            a.vm->start("Activate");
                    } else {
                        a.vm->start("stopbuild") || a.vm->start("stop_building") ||
                            a.vm->start("Deactivate");
                    }
                }
            }
            // On/off structures: the doors/power state swing via the COB Activate/
            // Deactivate scripts (RequestState->Go->[open + hide doors + OpenYard], and
            // the reverse). OpenYard/CloseYard alone move NO piece, so they are not the
            // hook. A GATE auto-opens on friendly footprint occupancy (retail 0x40a020):
            // it is purely cosmetic for us -- the gate's passage cells ('c' in the
            // yardmap) are never blocked, so units pass regardless -- so we drive it
            // CLIENT-side (no hashed state) from the render frame rather than the sim.
            // Non-gate onoffables follow the sim's u.active (the O/ToggleGate order).
            if (u.type && u.type->onOffable && a.hasActivate) {
                if (a.hasGateDoors) {
                    // Rescan a few times a second (not the O(units) sweep every tick).
                    if (animClock_ >= a.gateNext) {
                        a.gateNext = animClock_ + 0.2f;
                        float halfW = u.type->footX * 8.0f + 24.0f;   // half-extent + margin
                        float halfD = u.type->footZ * 8.0f + 24.0f;
                        bool wantOpen = false;
                        for (const UnitR* op : front().live) {
                            // isStructure (maxVel<=0), NOT !canMove: walls set canmove=1
                            // with no velocity, and a wall abuts every gate -- the canMove
                            // test would latch the gate open forever (CLAUDE.md gotcha).
                            if (op == &u || !op->type || isStructure(op->type)) continue;
                            if (op->player != u.player) continue;
                            if (std::abs(op->x - u.x) < halfW && std::abs(op->z - u.z) < halfD) {
                                wantOpen = true;
                                break;
                            }
                        }
                        if (wantOpen != a.active) {
                            a.active = wantOpen;
                            a.vm->start(wantOpen ? "Activate" : "Deactivate");
                        }
                    }
                } else if (u.active != a.active) {
                    a.active = u.active;
                    a.vm->start(u.active ? "Activate" : "Deactivate");
                }
            }
            // Cloak pose. StartCloaking/StopCloaking are real engine entry points
            // (they appear in the icd's call-script-by-name sites alongside Create
            // and Activate), and the two units that define them -- araspy and
            // npcheket -- fold their pieces with MOVE_NOWs in the `cloak` script
            // that StartCloaking starts. We were never calling either, so a
            // cloaking spy just went transparent in its walking pose.
            if (a.hasCloakAnim && u.cloaked != a.cloaked) {
                a.cloaked = u.cloaked;
                a.vm->start(u.cloaked ? "StartCloaking" : "StopCloaking");
            }
            // (The VM itself is advanced in the parallel pass below.)
        }

        // (The per-unit animation VMs are advanced in animFrame(), once per RENDER frame
        // rather than per sim tick, so the heavy parallel pass doesn't clump onto the
        // 1-in-8 net frame that also runs the sim tick, and animation runs at display rate.
        // The STATE transitions above -- which script/gait, script starts, sounds -- stay
        // here on the sim tick.)
    }

    void GameView::animFrame(float realDt) {
        if (paused_) return;
        // A game can also stop dead without anyone touching the pause key: when bundles
        // stop arriving a net game simply produces no ticks. Everything ELSE already
        // holds still when that happens -- unit positions clamp to the newest pose
        // (interpAlpha_), and cosmeticStep runs only for ticks actually drained -- but
        // the animation VM is deliberately decoupled from the tick rate so that the walk
        // cycle stays smooth between 30Hz ticks, and that decoupling meant it kept
        // playing through a stall: legs walking on the spot, wheels turning, rotors
        // spinning, while the world stood frozen. Freeze it on the same event, so a
        // stalled game reads as stopped rather than broken.
        //
        // Measured from when the SIM CLOCK last moved, NOT from the age of the newest
        // snapshot. front() is pinned for the whole frame, so the snapshot is always at
        // least one frame old, and an age test would call a healthy client on a slow
        // machine permanently stalled at any frame rate below the threshold.
        // Only once the game is actually running. Before the first tick lands -- loading,
        // the lobby, the pre-game hold -- gameTick sits at 0 and never "advances", which
        // the gate below would read as a stall and freeze on, then visibly un-freeze the
        // moment the first tick arrived. There is nothing to stall yet at tick 0.
        const uint64_t nowMs = SDL_GetTicks64();
        if (front().gameTick == 0) { animAdvanceMs_ = nowMs; animLastTick_ = 0; }
        else if (front().gameTick != animLastTick_ || animAdvanceMs_ == 0) {
            animLastTick_ = front().gameTick;
            animAdvanceMs_ = nowMs;
        }
        // Scaled by the tick interval rather than a flat millisecond figure: game speed
        // changes how long a tick takes, and a fixed threshold would read a deliberately
        // slowed game as a permanent stall. The floor keeps ordinary jitter from
        // flickering the animation off and on.
        const float stallMs = std::max(250.0f, front().tickDurMs * 6.0f);
        if (float(nowMs - animAdvanceMs_) > stallMs) return;
        float dt = realDt * animSpeed();
        vmTick_.clear();
        for (auto& [id, a] : anims_) {
            if (a.vm) vmTick_.push_back(a.vm.get());
            a.fireT += dt; a.smokeT += dt;   // age since last emit (fire fades if it stops)
            // Flyer attitude. A retail flyer rolls into its turn and pitches into a
            // climb or dive, scaled per unit by bankscale/pitchscale; ours flew
            // perfectly level through everything. Derived from how the unit is
            // ACTUALLY moving (heading change and altitude change per second) and
            // eased, so it leans in and levels out instead of snapping.
            if (!a.flying || dt <= 0.0f) continue;
            const UnitR* fu = frameUnitP(id);
            if (!fu || !fu->type) continue;
            if (fu->type->bankScale <= 0 && fu->type->pitchScale <= 0) continue;
            if (!a.attitudeInit) {
                a.attitudeInit = true;
                a.prevHeading = fu->heading;
                a.prevAlt = a.altitude;
            }
            constexpr float kTau = 6.2831853f;
            float dh = fu->heading - a.prevHeading;
            while (dh > kTau / 2) dh -= kTau;
            while (dh < -kTau / 2) dh += kTau;
            a.prevHeading = fu->heading;
            float dAlt = a.altitude - a.prevAlt;
            a.prevAlt = a.altitude;
            // Targets: turn rate (rad/s) and climb rate, each scaled and capped so a
            // hard turn banks hard but never rolls past a believable limit.
            float wantBank = std::clamp(dh / dt * fu->type->bankScale * 0.35f, -0.9f, 0.9f);
            float wantPitch = std::clamp(dAlt / dt * fu->type->pitchScale * 0.010f, -0.5f, 0.5f);
            float ease = std::clamp(dt * 4.0f, 0.0f, 1.0f);
            a.bank += (wantBank - a.bank) * ease;
            a.pitch += (wantPitch - a.pitch) * ease;
        }
        pool_.parallelFor(vmTick_.size(), [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) vmTick_[i]->tick(dt);
        }, /*minParallel=*/1500);   // ~0.2us/VM: pool dispatch only pays off at scale
        // Drain emit-sfx the VMs stashed (fire/smoke from FireControl-style loops),
        // now serially on the main thread, into the world-space effect system.
        for (auto& [id, a] : anims_) {
            if (a.pendingSfx.empty() && a.pendingSnd.empty() &&
                a.pendingExplode.empty()) continue;
            const auto* u = frameUnitP(id);
            if (u && u->type && (noFog_ || cellVisibleR(u->x, u->z))) {
                for (auto& [piece, sfx] : a.pendingSfx) emitSfx(*u, a, piece, sfx);
                for (auto& [piece, fl] : a.pendingExplode) explodePiece(*u, a, piece, fl);
                // COB PLAY_SOUND: resolve the name-table index to a wav stem. This is
                // how retail plays per-unit action sounds -- the Beast Handler's whip
                // crack when a conjure starts, attack swooshes, death cries.
                for (int32_t si : a.pendingSnd) {
                    std::string nm = a.vm->file().name(uint32_t(si));
                    std::transform(nm.begin(), nm.end(), nm.begin(), ::tolower);
                    if (!nm.empty() && sounds_.has(nm)) sounds_.playWorld(nm, u->x, u->z);
                }
            }
            a.pendingSfx.clear();
            a.pendingSnd.clear();
            a.pendingExplode.clear();
        }
    }

    float GameView::animSpeed() const {
        if (isNet() && mp_) return std::max(1, int(mp_->gameSpeed())) / 10.0f;
        return speedMult();
    }

    void GameView::update(float dt) {
        if (paused_) return;   // freeze the sim; input/render/camera keep running
        // Game speed: scale game time (sim, effects, AI, animation all follow dt).
        dt *= speedMult();
        simStep(dt);
        cosmeticStep(dt);
    }

    void GameView::prepare(int winW, int winH) {
        mapView_.ensureChunks(mapViewW(winW), winH);
        if (!miniTex_) buildMinimap();
        // Listener = the camera view (the map viewport), so positional sounds pan by
        // where the source sits on SCREEN, not by its absolute map position. This
        // runs every frame for SP, net, AND spectator -- update() is skipped on the
        // net/spectator path, so the listener can't live there.
        float zm = std::max(mapView_.zoom(), 1e-3f);
        float halfW = (mapViewW(winW) / 2.0f) / zm, halfH = (winH / 2.0f) / zm;
        sounds_.setListener(mapView_.offX() + halfW, mapView_.offY() + halfH, halfW, halfH);
    }

    void GameView::takeProf(double& projMs, double& submitMs, double& shadowMs,
                            double& simMs, uint64_t& unitsDrawn, uint64_t& shadowVerts) {
        // Deltas since the last call -- the counters themselves are monotonic so that the
        // per-frame TA_SPIKES logger can take its own independent deltas off them.
        projMs   = profProjMs_   - profProjPrev_;    profProjPrev_   = profProjMs_;
        submitMs = profSubmitMs_ - profSubmitPrev_;  profSubmitPrev_ = profSubmitMs_;
        shadowMs = profShadowMs_ - profShadowPrev_;  profShadowPrev_ = profShadowMs_;
        unitsDrawn  = profUnits_       - profUnitsPrev_;       profUnitsPrev_       = profUnits_;
        shadowVerts = profShadowVerts_ - profShadowVertsPrev_; profShadowVertsPrev_ = profShadowVerts_;
        // profSimTicks_ is the sim WORKER's counter, exchanged (not delta'd): the spike
        // logger never reads it, so it has only the one consumer.
        simMs = double(profSimTicks_.exchange(0)) * 1000.0 / double(SDL_GetPerformanceFrequency());
    }

    void GameView::advance(float seconds) {
        float printed = 0;
        for (float t = 0; t < seconds; t += 1.0f / 30.0f) {
            update(1.0f / 30.0f);
            if (trace_ && t >= printed) {
                printed += 0.5f;
                for (auto& u : world_.units())
                    if (u.player == 0 && u.alive())
                        std::printf("TRACE %.1f %d %.1f %.1f\n", t, u.id, u.x, u.z);
            }
        }
    }

    int GameView::minWindowWidth() const {
        int n = int(registry_.maxBuildMenu());
        int rowW = n > 0 ? (n - 1) * 66 + 60 : 0;
        return rowW + 24 + panelW();
    }

    void GameView::emitSfx(const UnitR& u, Anim& a, int piece, int32_t sfx) {
        const char* anim = sfxAnimFor(sfx);
        if (!anim) return;
        const EffectAnim* ea = effectFor(anim);
        if (!ea || ea->frames.empty()) return;
        // Refresh this unit's persistent flame/smoke; drawUnitFx cycles it smoothly.
        float lift = pieceLift(u, a, piece);
        if (anim[0] == 's') { a.smokeFx = ea; a.smokeT = 0; a.smokeLift = lift; }
        else                { a.fireFx = ea;  a.fireT = 0;  a.fireLift = lift; }
    }

    void GameView::explodePiece(const UnitR& u, Anim& a, int piece, int32_t flags) {
        float dAlt = pieceLift(u, a, piece) + unitAltById(u.id);
        // Debris chunks at retail velocities (icd 0x50dd20: vx,vz = (20-rand(40))<<12,
        // vy = rand(10)<<14, 16.16 px/tick => +-37.5 / up-to-75 px/s, life 900ms),
        // suppressed by BITMAPONLY (0x20 -- the VM already kept the piece visible).
        if (!(flags & 0x20)) {
            bool flesh = u.type->bodyType == "flesh";
            for (int i = 0; i < 3; ++i) {
                Particle p;
                p.x = u.x; p.z = u.z; p.alt = 4 + dAlt;
                p.vx = 37.5f - float(salt_++ % 75);
                p.vz = 37.5f - float(salt_++ % 75);
                p.valt = float(salt_++ % 75);
                p.maxLife = p.life = 0.9f;
                p.size = 2.5f + float(salt_++ % 100) / 40.0f;   // chunky
                if (flesh && i < 2) { p.r = u.type->blood[0]; p.g = u.type->blood[1];
                                      p.b = u.type->blood[2]; }
                else { p.r = 115; p.g = 100; p.b = 80; }        // wood/stone chunk
                p.kind = 0;
                particles_.push_back(p);
            }
        }
        // TA flag extras: SMOKE (8), FIRE (16), BITMAP1..NUKE (0x100..0x2000) pick
        // an explosion-class one-shot at the piece.
        if (flags & 8)  spawnBurst(u.x, u.z, 3, 90, 80, 80, 14, 2.6f, 1, dAlt);
        if (flags & 16) spawnBurst(u.x, u.z, 4, 240, 130, 40, 26, 2.2f, 0, dAlt);
        if (flags & 0x3F00)
            spawnEffect(flags >= 0x1000 ? "medium explosion" : "small explosion",
                        u.x, u.z, dAlt);
    }

    float GameView::pieceLift(const UnitR& u, const Anim& a, int piece) {
        if (!a.pieceNames || piece < 0 || size_t(piece) >= a.pieceNames->size() || !u.type) return 0.0f;
        auto vt = visuals_.find(u.type->id);
        if (vt == visuals_.end()) return 0.0f;
        float out = 0.0f;
        findPieceY(vt->second.model.root, (*a.pieceNames)[size_t(piece)], 0.0f, out);
        return std::max(0.0f, out);
    }

    // Corpse mesh: once the death anim finishes, the body draws as the FBI
    // corpse feature's `object=` model (arasword_dead etc.) through the normal
    // unit pipeline -- same swap trick as the veteran meshes. The old VM's piece
    // names don't match the corpse skeleton, so it renders in rest pose.
    void GameView::maybeSwapCorpseModel(const UnitR& u) {
        if (!u.type) return;
        int ct = u.corpseFeat;   // corpse OR statue def, resolved by the sim
        if (ct < 0) return;
        const std::string& obj = world_.featureTypes()[size_t(ct)].object;
        if (obj.empty()) return;
        auto it = unitType_.find(u.id);
        if (it == unitType_.end() || it->second == obj) return;
        if (!visuals_.count(obj)) {
            try {
                visuals_[obj] = {ta::tdo::load(vread("objects3d/" + obj + ".3do")), {}};
            } catch (const std::exception&) { return; }   // no corpse mesh: keep pose
        }
        it->second = obj;
    }

    void GameView::registerUnit(int id, const ta::sim::UnitType* type) {
        const std::string& typeId = type->id;
        if (!visuals_.count(typeId)) {
            try {
                Visual v{ta::tdo::load(vread("objects3d/" + typeId + ".3do")), {}};
                buildPieceMeta(v.model.root, v.meta);   // fixed for the model's life
                visuals_[typeId] = std::move(v);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "no model for %s: %s\n", typeId.c_str(), e.what());
                return;
            }
        }
        Anim a;
        try {
            // Per-type COB cache: parse the script once and share the immutable
            // bytecode across every unit of the type (each Vm previously owned a
            // full copy -- 60-140KB of code words per unit in a big army -- and
            // every spawn re-read + re-parsed the file).
            auto ci = cobCache_.find(typeId);
            if (ci == cobCache_.end()) {
                std::string cobPath = "scripts/" + typeId + ".cob";
                CobCache cc;
                cc.file = std::make_shared<const ta::cob::File>(
                    ta::cob::load(vread(cobPath), cobPath));
                for (const auto& p : cc.file->pieces) {
                    std::string n = p;
                    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                    cc.pieceNames.push_back(n);
                }
                if (!cc.file->names.empty())
                    for (uint32_t w : cc.file->code)
                        if (w == 0x10072000) { cc.hasSounds = true; break; }
                cc.moveGate = walkGateOf(*cc.file);
                cc.hasWalk = hasWalkCycle(*cc.file);
                cc.hasMelee = cc.file->scriptIndex("MoveWatcher") >= 0 ||
                              cc.file->scriptIndex("MeleeControl") >= 0 ||
                              cc.file->scriptIndex("DemonControl") >= 0;   // tarcan (Rictus)
                cc.hasAim = cc.file->scriptIndex("AimWeapon") >= 0;
                cc.hasCloakAnim = cc.file->scriptIndex("StartCloaking") >= 0;
                cc.hasFlinch = cc.file->scriptIndex("HitByWeapon") >= 0;
                cc.hasWind = cc.file->scriptIndex("WindChange") >= 0;
                cc.hasFlightSM = cc.file->scriptIndex("BeginFlight") >= 0;
                cc.hasActivate = cc.file->scriptIndex("Activate") >= 0;
                cc.hasQueryWeapon = cc.file->scriptIndex("QueryWeapon") >= 0;
                cc.hasFly = cc.file->scriptIndex("fly") >= 0;
                cc.hasMotionControl = cc.file->scriptIndex("MotionControl") >= 0;
                cc.hasOpen = cc.file->scriptIndex("open") >= 0;
                ci = cobCache_.emplace(typeId, std::move(cc)).first;
            }
            a.pieceNames = &ci->second.pieceNames;
            a.cobSounds = ci->second.hasSounds;
            a.moveGate = ci->second.moveGate;
            a.hasWalk = ci->second.hasWalk;
            a.hasMelee = ci->second.hasMelee;
            a.hasCloakAnim = ci->second.hasCloakAnim;
            a.hasAim = ci->second.hasAim;
            a.hasFlinch = ci->second.hasFlinch;
            a.hasWind = ci->second.hasWind;
            a.hasFlightSM = ci->second.hasFlightSM;
            a.hasActivate = ci->second.hasActivate;
            a.hasGateDoors = type && type->onOffable && ci->second.hasOpen;
            a.hasQueryWeapon = ci->second.hasQueryWeapon;
            // Airships with no fly/land state machine (creaeri rotors, verball/tarship
            // MotionControl): driven entirely by their Create ambients. The generic
            // flyGate+reset+fly/land path freezes them (no `fly`/`land` to drive, and
            // reset() would kill the ambients).
            a.flyAmbient = type && type->canFly && !ci->second.hasFlightSM &&
                           (!ci->second.hasFly || ci->second.hasMotionControl);
            // Seed the door/active latch. A gate starts CLOSED (Create leaves its
            // doors shut) so proximity opens it; other onoffable units mirror the
            // sim's initial u.active (= activateWhenBuilt) so a unit built inactive
            // doesn't fire a spurious Deactivate the first frame it's seen.
            a.active = a.hasGateDoors ? false : (type ? type->activateWhenBuilt : true);
            a.vm = std::make_unique<ta::cob::Vm>(ci->second.file);
            // TA COB unit-state queries answered from the sim.
            int unitId = id;
            a.vm->onGet = [this, unitId](int32_t valId,
                                         const std::vector<int32_t>&) -> int32_t {
                // Ticked on the worker pool (animFrame) -- read the pinned render snapshot,
                // never live world_, which the sim thread mutates concurrently under Stage B.
                const auto* su = frameUnitP(unitId);
                if (!su || !su->type) return 0;
                // TAK's GET_UNIT_VALUE numbering (icd getter table @0x50d394) --
                // NOT TA-1997's. The old TA-numbered cases here answered ids no
                // shipped TAK script asks for, while id 4 (HEALTH%) returned 0 --
                // making every building COB's SmokeControl/DamageFlameControl
                // believe it was at 0 HP and belch damage smoke from spawn.
                switch (valId) {
                    case 1:  return su->buildQueue.empty() ? 0 : 1;   // ACTIVATION
                    case 4:  return int32_t(su->hp / su->type->maxHp * 100);  // HEALTH %
                    case 6:  return su->moving() ? 1 : 0;             // BUSY
                    case 9:  {                                        // UNIT_XZ
                        int32_t x = int32_t(su->x) & 0xFFFF;
                        int32_t z = int32_t(su->z) & 0xFFFF;
                        return (x << 16) | z;
                    }
                    case 17: return su->underConstruction              // BUILD_PERCENT_LEFT (the
                                 ? int32_t(100 - su->hp / su->type->maxHp * 100)  // Create wait-
                                 : 0;                                  // loops on it, so ambient
                                                                       // anims/emit-sfx hold off
                                                                       // until the building is up)
                    case 27:                                           // HEADING (16-bit angle)
                        return int32_t(su->heading * (65536.0f / 6.2831853f)) & 0xFFFF;
                    case 28: {                                         // STANDING ON WATER
                        // (MeleeControl picks walk_water; WakeControl gates wakes.)
                        const auto& mp = mapView_.map();               // const after load
                        int cx = std::clamp(int(su->x) / 16, 0, mp.width - 1);
                        int cz = std::clamp(int(su->z) / 16, 0, mp.height - 1);
                        return mp.heights[size_t(cz) * mp.width + cx] < mp.seaLevel ? 1 : 0;
                    }
                    case 34: {                                         // STANDING ON ROAD
                        // (MeleeControl picks walk_road; 92 units ask.) Roads are
                        // 0xFFFB cells in the map's feature plane; retail sets the
                        // bit only when the WHOLE footprint is on road (icd 0x509760).
                        const auto& mp = mapView_.map();               // const after load
                        if (mp.features.empty()) return 0;
                        int fx = std::max(1, int(su->type->footX));
                        int fz = std::max(1, int(su->type->footZ));
                        int x0 = int(su->x) / 16 - fx / 2, z0 = int(su->z) / 16 - fz / 2;
                        if (x0 < 0 || z0 < 0 || x0 + fx > mp.width || z0 + fz > mp.height)
                            return 0;
                        for (int dz = 0; dz < fz; ++dz)
                            for (int dx = 0; dx < fx; ++dx)
                                if (mp.features[size_t(z0 + dz) * mp.width +
                                                size_t(x0 + dx)] != 0xFFFB)
                                    return 0;
                        return 1;
                    }
                    case 29:                                           // CURRENT_SPEED (% of max:
                        return su->type->maxVel > 0                    // ship MotionControl picks
                                   ? int32_t(std::clamp(               // slowrow/row/fastrow at
                                         su->speed / su->type->maxVel * 100.0f,  // 25/75)
                                         0.0f, 100.0f))
                                   : 0;
                    // VETERAN LEVEL: Kingdoms-only, and always 0 here. Retail TA's
                    // StatusControl scripts read it but nothing in TA sets it.
                    case 32: return 0;
                    // YARD_OPEN. Returning 0 here is NOT benign, which is what
                    // the old comment claimed -- it hangs the build yard. The
                    // castle/factory protocol (aracastl OpenYard) is a blocking
                    // handshake:
                    //
                    //     SET 18,1            request the yard open
                    //   loop:
                    //     GET 18; NOT; JUMP_IF_FALSE done    leave only on NONZERO
                    //     SET 19,1            BUGGER_OFF: shove units out of the yard
                    //     SLEEP 1500; SET 18,1; JUMP loop
                    //
                    // so 0 means "refused" and the script retries every 1.5s for
                    // ever. And because Go CALLs startbuild then OpenYard, that
                    // thread never returns -- the state machine never reaches
                    // Stop/stopbuild, so the yard doors open and never close.
                    // Measured with the COB VM on aracastl: one thread stuck for
                    // ever above the ambient loops with 0, none with 1.
                    //
                    // We grant it unconditionally because our sim has no notion
                    // of a unit blocking a yard; retail refuses only while one
                    // does, and we never set BUGGER_OFF in motion anyway.
                    case 18: return 1;                                 // YARD_OPEN
                    case 33: {                                         // TURN RATE (signed)
                        // SuperDynamicWheelSpinner (9 wheeled units) reads this,
                        // tests it against +910 and -910, and uses the sign to
                        // spin the wheels on the two sides at different rates --
                        // the differential of a vehicle in a turn. 910 is 5
                        // degrees in 16-bit angle units (65536/360*5 = 910.2),
                        // which is what fixes the scale: angle units per tick.
                        if (!su->seeded) return 0;
                        float d = su->heading - su->ph;
                        while (d >  3.14159265f) d -= 6.28318531f;   // shortest way round
                        while (d < -3.14159265f) d += 6.28318531f;
                        return int32_t(d * (65536.0f / 6.28318531f));
                    }
                    case 46: {                                         // HAS TARGET
                        // HolsterControl (verbers, vercrus) only ever tests this
                        // against 0: zero puts the weapon away, non-zero draws
                        // it. Retail's is (unit+0x130 >> 20) & 3, a 2-bit state;
                        // since nothing reads the other bits, "am I attacking
                        // something" answers every shipped use.
                        for (const auto& o : su->orders)
                            if (o.targetId && !o.load && !o.guard) return 1;
                        return 0;
                    }
                    // 30 is still unanswered: ONE use, in lifbird's FlightControl,
                    // where it is compared against -50 and 0 takes the hover
                    // branch -- which is the right look for a bird at rest. See
                    // docs/retail-engine.md.
                    default: return 0;
                }
            };
            // Flyers deploy their wings and start flapping at spawn via their
            // flight scripts; without these they sit in the landed rest pose
            // (which also reads as facing the wrong way).
            if (type && type->canFly) {
                a.flying = true;   // starts grounded; the update loop flies her
                if (a.hasFlightSM || a.flyAmbient) {
                    // Drake / dragon / gryphon / harpy / roc / wisp (BeginFlight VTOL
                    // state machine), and the ambient airships (creaeri / verball /
                    // tarship). Their Create HIDEs the veteran-detail LOD pieces
                    // (*_5/*_10 -- else every rookie renders them doubled over the base
                    // mesh) and starts the ambient loops (FlightControl / MotionControl /
                    // StatusControl / rotor+smoke). Drive them by their engine edges
                    // below; DON'T use flyGate/fly/land and NEVER reset() (that kills the
                    // loops and freezes the model).
                    a.vm->start("Create");
                } else {
                    a.flyGate = flyGateOf(*a.vm);
                    // Start in the folded landed pose, not the wings-spread rest
                    // pose, so a flyer that spawns idle and never takes off (e.g. the
                    // Monarch at game start) doesn't sit in a T-pose.
                    a.vm->start("land");
                }
            } else if (isStructure(type) || !a.hasWalk || a.hasMelee) {
                // Buildings: run the COB constructor so ambient loops start (e.g. the
                // Keep's Create kicks off its flag/smoke scripts, the Sacred Fire's
                // its FireControl flicker). Detect via isStructure (maxVel<=0), NOT
                // !canMove -- the Keep and friends set canmove=1 with no velocity, so
                // the old !canMove test skipped them and they never animated.
                // Also mobile units with NO walk cycle (ships' oars, wheeled war-machines'
                // wheels/props): their motion is a Create ambient loop, not a walk script,
                // so start it here and let it run (the walk state machine leaves them be).
                a.vm->start("Create");
            }
            if (a.hasAim && type->weapon.reload > 0)
                // Seed RestoreAfterDelay's timer (retail SetMaxReloadTime, ms):
                // unseeded, the turret snaps back to rest the moment an aim ends.
                a.vm->start("SetMaxReloadTime",
                            {int32_t(type->weapon.reload * 1000.0f)});
        } catch (const std::exception&) { /* unit stays unanimated */ }
        // Flag units whose model uses an animated glow texture (lodestone/mana/crystal)
        // so the glow only cycles once built -- held static while still conjuring.
        if (auto vt = visuals_.find(typeId); vt != visuals_.end())
            for (const auto& tn : vt->second.model.textures()) {
                std::string t = tn;
                std::transform(t.begin(), t.end(), t.begin(), ::tolower);
                if (animatedTex_.count(t)) { a.usesGlow = true; break; }
            }
        if (a.vm) {
            Anim& st = anims_[id] = std::move(a);
            // The VM is ticked on the worker pool, so emit-sfx only stashes into this
            // unit's own buffer (std::map nodes are pointer-stable); the main thread
            // drains it into effects_ after the parallel tick.
            st.vm->onEmitSfx = [buf = &st.pendingSfx](int piece, int32_t sfx) {
                buf->push_back({piece, sfx});
            };
            st.vm->onPlaySound = [buf = &st.pendingSnd](int32_t idx) {
                buf->push_back(idx);
            };
            st.vm->onExplode = [buf = &st.pendingExplode](int piece, int32_t flags) {
                buf->push_back({piece, flags});
            };
        }
        unitType_[id] = typeId;
    }

    int GameView::spawn(const std::string& typeId, float x, float z, float heading, int player) {
        const auto* type = registry_.find(typeId);
        if (!type) return -1;
        int id = world_.spawn(type, x, z, heading, player);
        // registerUnit mutates the client render maps (visuals_/cobCache_/anims_/unitType_),
        // which the render thread + the animFrame VM pool read/iterate. It must run ONLY on
        // the main thread. When spawn() is reached from the SIM WORKER (summonGod / mission
        // reinforcements / mapCommand, all inside simStep), skip it: the main thread's
        // cosmeticStep lazily registers every live snapshot unit, so it is redundant there.
        if (std::this_thread::get_id() == mainThreadId_) {
            registerUnit(id, type);
            if (!unitType_.count(id)) return -1;
        }
        return id;
    }

    void GameView::loadTextures() {
        // TA indexes every unit texture against the ONE game palette. Kingdoms
        // instead gave each faction its own bank with its own
        // palettes/<side>_textures.pcx, and this used to bail outright when
        // ara_textures.pcx was missing -- which is every TA install, so it loaded
        // ZERO textures and every unit rendered flat grey with no error anywhere.
        //
        // The per-side banks are still read where they exist (a Kingdoms-shaped
        // override), but they are now an override on top of the game palette
        // rather than a precondition.
        std::map<std::string, ta::gaf::Palette> pals;
        for (const char* side : {"ara", "tar", "ver", "zon", "aid", "cre"}) {
            std::string pp = std::string("palettes/") + side + "_textures.pcx";
            try { if (vfs_.has(pp)) pals[side] = ta::gaf::Palette::fromBytes(vread(pp), pp); }
            catch (const std::exception&) {}
        }
        ta::gaf::Palette base{};
        bool haveBase = false;
        for (const char* p : {"palettes/palette.pal", "palettes/guipal.pal"}) {
            try { base = ta::gaf::Palette::fromBytes(vread(p), p); haveBase = true; break; }
            catch (const std::exception&) {}
        }
        if (!haveBase && pals.count("ara")) { base = pals.at("ara"); haveBase = true; }
        if (!haveBase) return;   // nothing to colour textures with
        for (const std::string& path : vfs_.list("textures")) {
            if (!ta::iendsWith(path, ".gaf")) continue;
            std::string stem = std::filesystem::path(path).stem().string();
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            const auto* pal = &base;
            auto pit = pals.find(stem.substr(0, 3));
            if (pit != pals.end()) pal = &pit->second;
            try {
                for (auto& seq : ta::gaf::load(vread(path), *pal, 5, path)) {
                    if (seq.frames.empty()) continue;
                    std::string name = seq.name;
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    if (textures_.count(name)) continue;
                    // 10-frame sequences are EITHER per-player colours (insignia --
                    // frames span distinct hues) OR an animated glow (lodestone/mana/
                    // sacred-fire crystal -- frames share a hue, a moving sparkle).
                    // Keep all 10 for both; classify by hue spread so the glow cycles
                    // over time (animatedTex_) while insignia stay picked-by-player.
                    size_t n = seq.frames.size() == 10 ? 10 : 1;
                    // The animated glows (lodestone/mana/sacred-fire/crystal crystals)
                    // are the mana/lodestone/crystal-named textures whose 10 frames
                    // pulse ONE hue over time. The "*logo*" textures (incl. the
                    // lodestone side-panel logos) are per-PLAYER-colour -- their 10
                    // frames are the 10 player colours, picked by slot, NOT animated.
                    if (n == 10 && name.find("logo") == std::string::npos) {
                        for (const char* g :
                             {"lode", "mana", "sacred", "crystal", "lightning", "stone"})
                            if (name.find(g) != std::string::npos) {
                                animatedTex_.insert(name);
                                break;
                            }
                    }
                    // Sample the actual 10 player-colour RGBs once, from a logo/
                    // insignia texture (mostly pure player colour), so the HUD and
                    // minimap can match whatever colour a player renders in.
                    if (!sampledColors_ && n == 10 && !animatedTex_.count(name) &&
                        name.find("logo") != std::string::npos) {
                        for (size_t i = 0; i < 10; ++i) {
                            const auto& f = seq.frames[i];
                            // Saturation-weighted average: the pure player-colour
                            // pixels dominate, grey shading/outlines contribute little.
                            double r = 0, g = 0, b = 0, wsum = 0;
                            for (size_t k = 0; k + 3 < f.rgba.size(); k += 4) {
                                if (f.rgba[k + 3] < 128) continue;
                                int R = f.rgba[k], G = f.rgba[k + 1], B = f.rgba[k + 2];
                                int mx = std::max({R, G, B}), mn = std::min({R, G, B});
                                if (mx < 45) continue;                 // skip outlines
                                double w = double(mx - mn) + 4.0;      // ~saturation
                                w *= w;                                // emphasise colour
                                r += R * w; g += G * w; b += B * w; wsum += w;
                            }
                            if (wsum > 0) {
                                // Brighten a touch so a swatch reads clearly.
                                auto up = [](double v) {
                                    return Uint8(std::min(255.0, v * 1.25));
                                };
                                playerColors_[i] = {up(r / wsum), up(g / wsum),
                                                    up(b / wsum), 255};
                            }
                        }
                        sampledColors_ = true;
                    }
                    std::vector<SDL_Texture*> frames;
                    for (size_t i = 0; i < n; ++i) {
                        auto& f = seq.frames[i];
                        if (f.width == 0 || f.height == 0) break;
                        SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                           SDL_TEXTUREACCESS_STATIC,
                                                           f.width, f.height);
                        SDL_UpdateTexture(t, nullptr, f.rgba.data(), f.width * 4);
                        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                        frames.push_back(t);
                    }
                    if (!frames.empty()) textures_[name] = std::move(frames);
                }
            } catch (const std::exception&) {}
        }
    }

    const ta::cob::PieceState* GameView::pieceFor(const Anim* a, const std::string& objName) const {
        if (!a || !a->vm || !a->pieceNames) return nullptr;
        std::string n = objName;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        for (size_t i = 0; i < a->pieceNames->size(); ++i)
            if ((*a->pieceNames)[i] == n) return &a->vm->pieces()[i];
        return nullptr;
    }



    bool GameView::dancing(const UnitR& u) const {
        return isMonarchType(u.type) && u.disco;
    }

    bool GameView::headbanging(const UnitR& u) const {
        return isMonarchType(u.type) && u.headbang;
    }

    void GameView::sprinkleBuildFx(const std::string& sideLower, float cx, float cy, float fpw, float fph) {
        auto fit = buildFx_.find(sideLower);
        if (fit == buildFx_.end() || fit->second.empty()) return;
        auto& frames = fit->second;
        int fw, fh;
        SDL_QueryTexture(frames[0], nullptr, nullptr, &fw, &fh);
        const float zm = mapView_.zoom();
        constexpr float kBuildFxScale = 2.0f;   // bigger than 1:1 so the sparkle reads
        float tw = float(fw) * zm * kBuildFxScale, th = float(fh) * zm * kBuildFxScale;
        fpw = std::max(fpw * 1.35f, tw);         // overflow the footprint; >=1 sparkle
        fph = std::max(fph * 1.35f, th);
        int nx = std::clamp(int(fpw / tw + 0.5f), 1, 5);
        int nz = std::clamp(int(fph / th + 0.5f), 1, 5);
        float x0 = cx - fpw * 0.5f, y0 = cy - fph * 0.6f;
        int base = int(animClock_ * 12);
        for (int gz = 0; gz < nz; ++gz)
            for (int gx = 0; gx < nx; ++gx) {
                SDL_Texture* fxt = frames[size_t(base + gx * 3 + gz * 5) % frames.size()];
                SDL_FRect d{x0 + (gx + 0.5f) * fpw / nx - tw * 0.5f,
                            y0 + (gz + 0.5f) * fph / nz - th * 0.5f, tw, th};
                SDL_RenderCopyF(ren_, fxt, nullptr, &d);
            }
    }

    ta::tdf::Node GameView::vtdf(const std::string& p) const {
        auto b = vfs_.read(p);
        return ta::tdf::parseText(std::string(b.begin(), b.end()), p);
    }

    std::string GameView::mapSibling(const char* ext) const {
        std::filesystem::path p = mapPath_; p.replace_extension(ext);
        return p.generic_string();
    }

    void GameView::remountPolicy(uint8_t p) {
        auto pol = ta::hpi::OverridePolicy(p <= 2 ? p : 2);
        if (pol == policy_ || installRoot_.empty()) return;
        policy_ = pol;
        vfs_ = ta::hpi::mountRetailRoot(installRoot_, pol);
        registry_ = ta::sim::TypeRegistry{};
        ta::sim::setupRegistry(registry_, vfs_);
    }

    float GameView::birthProgress(int id) const {
        auto it = birthFx_.find(id);
        return it == birthFx_.end() ? 1.0f
                                    : std::clamp(it->second / kBirthFxDur, 0.0f, 1.0f);
    }

    float GameView::speedMult() const {
        return mp_ ? 1.0f : std::pow(10.0f, float(gameSpeed_) / 10.0f);
    }

    SDL_Color GameView::playerColor(int player) const {
        int s = (player >= 0 && player < 8) ? colorSlot_[player] : 0;
        return playerColors_[(s >= 0 && s < 10) ? s : 0];
    }

    // Raw bilinear heightmap value at a world point -- NOT shifted by the ground
    // reference and NOT clamped at zero, so water cells report their true (low)
    // height. heightAbove() below is this minus the reference, floored at 0; only
    // the waterline sink needs the unclamped value.
    // The map's wind range, from the .ota [GlobalHeader]. Lazy and keyed on the map
    // path, so it follows a lobby map swap, a mission and a replay alike. A generated
    // map has no .ota, which leaves retail's own defaults standing -- exactly what
    // retail does for the 31 shipped maps that omit the keys.
    void GameView::loadMapWind() {
        if (windFrom_ == mapPath_) return;
        windFrom_ = mapPath_;
        windMin_ = 100.0f;
        windMax_ = 2000.0f;
        std::string ota = mapSibling(".ota");
        if (!vhas(ota)) return;
        try {
            auto b = vread(ota);
            ta::tdf::Node r = ta::tdf::parseText(std::string(b.begin(), b.end()), ota);
            if (const ta::tdf::Node* gh = r.child("globalheader")) {
                windMin_ = float(gh->numberOr("minwindspeed", 100));
                windMax_ = float(gh->numberOr("maxwindspeed", 2000));
                if (windMax_ < windMin_) std::swap(windMin_, windMax_);
            }
        } catch (const std::exception&) {}
    }

    float GameView::rawHeight(float wx, float wz) {
        const auto& m = mapView_.map();
        if (m.heights.empty() || m.width <= 0) return 0.0f;
        float gx = (wx - 8.0f) / 16.0f, gz = (wz - 8.0f) / 16.0f;
        int x0 = std::clamp(int(std::floor(gx)), 0, m.width - 1);
        int z0 = std::clamp(int(std::floor(gz)), 0, m.height - 1);
        int x1 = std::min(x0 + 1, m.width - 1), z1 = std::min(z0 + 1, m.height - 1);
        float fx = std::clamp(gx - float(x0), 0.0f, 1.0f);
        float fz = std::clamp(gz - float(z0), 0.0f, 1.0f);
        auto H = [&](int x, int z) { return float(m.heights[size_t(z) * m.width + x]); };
        return H(x0, z0) * (1 - fx) * (1 - fz) + H(x1, z0) * fx * (1 - fz) +
               H(x0, z1) * (1 - fx) * fz + H(x1, z1) * fx * fz;
    }

    // Retail anchors a wading or floating unit at max(terrainHeight, waterLevel -
    // waterline) (KINGDOMS.icd 0x51b220 for canhover, 0x4dad78 for floater), so the
    // model sinks up to `waterline` height units and no further once the seabed
    // rises to meet it. Our sim has no Y, so this is a pure screen-Y offset, added
    // where terrainLift is subtracted.
    float GameView::waterSink(const ta::sim::UnitType* t, float wx, float wz) {
        if (!t || t->waterline <= 0 || !(t->canHover || t->floater)) return 0.0f;
        const auto& m = mapView_.map();
        if (m.heights.empty()) return 0.0f;
        float depth = float(m.seaLevel) - rawHeight(wx, wz);
        return std::clamp(depth, 0.0f, float(t->waterline)) * kHeightScale_;
    }

    float GameView::heightAbove(float wx, float wz) {
        const auto& m = mapView_.map();
        if (m.heights.empty() || m.width <= 0) return 0.0f;
        // 1-entry memo: nearly every render pass asks for terrainLiftX(x,z) and
        // terrainLift(x,z) back-to-back for the SAME point, so the second call reuses
        // this 4-sample bilinear instead of redoing it. Keyed on the map identity so a
        // map change can't return a stale height.
        if (&m == hMemoMap_ && wx == hMemoX_ && wz == hMemoZ_) return hMemoV_;
        if (heightRef_ < 0) {
            long hist[256] = {0};
            for (uint8_t v : m.heights) hist[v]++;
            int best = 0;
            for (int i = 1; i < 256; ++i) if (hist[i] > hist[best]) best = i;
            heightRef_ = best;
            if (const char* e = ta::devEnv("TA_HSCALE")) kHeightScale_ = std::stof(e);
            if (const char* e = ta::devEnv("TA_HSCALEX")) kHeightScaleX_ = std::stof(e);
            if (ta::devEnv("TA_HDEBUG")) showHDebug_ = true;
        }
        float gx = (wx - 8.0f) / 16.0f, gz = (wz - 8.0f) / 16.0f;
        int x0 = std::clamp(int(std::floor(gx)), 0, m.width - 1);
        int z0 = std::clamp(int(std::floor(gz)), 0, m.height - 1);
        int x1 = std::min(x0 + 1, m.width - 1), z1 = std::min(z0 + 1, m.height - 1);
        float fx = std::clamp(gx - float(x0), 0.0f, 1.0f);
        float fz = std::clamp(gz - float(z0), 0.0f, 1.0f);
        auto H = [&](int x, int z) { return float(m.heights[size_t(z) * m.width + x]); };
        float h = H(x0, z0) * (1 - fx) * (1 - fz) + H(x1, z0) * fx * (1 - fz) +
                  H(x0, z1) * (1 - fx) * fz + H(x1, z1) * fx * fz;
        float v = std::max(0.0f, h - float(heightRef_));
        hMemoMap_ = &m; hMemoX_ = wx; hMemoZ_ = wz; hMemoV_ = v;
        return v;
    }

    // Retail's flyer datum, built once per map (icd 0x50e740). One byte per
    // 128x128-world-unit sector = the MAX terrain height over its 8x8 block of
    // 16-unit cells, floored at the water datum; then a 3x3 max dilation, first
    // along X then along Z, so the value a flyer reads is the highest ground
    // within 384x384 world units of it. Cheap: a few hundred bytes, computed on
    // the first query after a map change.
    void GameView::buildFlyerGround() {
        const auto& m = mapView_.map();
        flyGroundMap_ = &m;
        flyGround_.clear();
        flyGroundW_ = flyGroundH_ = 0;
        if (m.heights.empty() || m.width <= 0 || m.height <= 0) return;
        heightAbove(0, 0);                       // force heightRef_ to be resolved
        const int kCells = 8;                    // 8 cells of 16 units = one 128-unit sector
        flyGroundW_ = (m.width + kCells - 1) / kCells;
        flyGroundH_ = (m.height + kCells - 1) / kCells;
        std::vector<uint8_t> maxv(size_t(flyGroundW_) * size_t(flyGroundH_), 0);
        for (int z = 0; z < m.height; ++z)
            for (int x = 0; x < m.width; ++x) {
                int h = int(m.heights[size_t(z) * m.width + x]) - heightRef_;
                if (h < 0) h = 0;                // below the water datum reads as flat
                uint8_t& dst = maxv[size_t(z / kCells) * size_t(flyGroundW_) + size_t(x / kCells)];
                if (h > int(dst)) dst = uint8_t(h > 255 ? 255 : h);
            }
        // 3x3 max dilation, separably: along X, then along Z in place.
        std::vector<uint8_t> tmp = maxv;
        for (int z = 0; z < flyGroundH_; ++z)
            for (int x = 0; x < flyGroundW_; ++x) {
                uint8_t v = maxv[size_t(z) * size_t(flyGroundW_) + size_t(x)];
                if (x > 0) v = std::max(v, maxv[size_t(z) * size_t(flyGroundW_) + size_t(x - 1)]);
                if (x + 1 < flyGroundW_) v = std::max(v, maxv[size_t(z) * size_t(flyGroundW_) + size_t(x + 1)]);
                tmp[size_t(z) * size_t(flyGroundW_) + size_t(x)] = v;
            }
        flyGround_.assign(tmp.begin(), tmp.end());
        for (int z = 0; z < flyGroundH_; ++z)
            for (int x = 0; x < flyGroundW_; ++x) {
                uint8_t v = tmp[size_t(z) * size_t(flyGroundW_) + size_t(x)];
                if (z > 0) v = std::max(v, tmp[size_t(z - 1) * size_t(flyGroundW_) + size_t(x)]);
                if (z + 1 < flyGroundH_) v = std::max(v, tmp[size_t(z + 1) * size_t(flyGroundW_) + size_t(x)]);
                flyGround_[size_t(z) * size_t(flyGroundW_) + size_t(x)] = v;
            }
    }

    float GameView::flyerGround(float wx, float wz) {
        const auto& m = mapView_.map();
        if (&m != flyGroundMap_) buildFlyerGround();
        if (flyGround_.empty()) return 0.0f;
        int sx = std::clamp(int(wx / 128.0f), 0, flyGroundW_ - 1);
        int sz = std::clamp(int(wz / 128.0f), 0, flyGroundH_ - 1);
        return float(flyGround_[size_t(sz) * size_t(flyGroundW_) + size_t(sx)]);
    }

    float GameView::unitAltById(int id) const {
        auto it = anims_.find(id);
        return it != anims_.end() ? it->second.altitude : 0.0f;
    }

    float GameView::flyerAltAt(float x, float z) const {
        float best = 24.0f * 24.0f, alt = 0.0f;
        for (const UnitR* _up : front().live) { const UnitR& u = *_up;
            if (!u.alive() || !u.type || !u.type->canFly) continue;
            float dx = u.x - x, dz = u.z - z, d = dx * dx + dz * dz;
            if (d < best) { best = d; alt = unitAltById(u.id); }
        }
        return alt;
    }

    SDL_FPoint GameView::unitScreen(const UnitR& u) {
        float zm = mapView_.zoom();
        float ix, iz, ih; interpPose(u, ix, iz, ih);   // match the gliding model position
        // uLiftX/uLiftY, NOT terrainLift: a flyer rides the coarse dilated datum,
        // and picking has to use the same one the body is drawn with or you click
        // where the unit is not. The waterline sink moves the drawn body too, so
        // this follows that as well -- else a wading god can't be clicked where you
        // see it.
        return {(ix - mapView_.offX()) * zm - uLiftX(u) * zm,
                (iz - mapView_.offY()) * zm - uLiftY(u) * zm
                    + waterSink(u.type, ix, iz) * zm - altLift(u) * zm - 12.0f * zm};
    }

    const SDL_FRect& GameView::unitHitBox(const ta::sim::UnitType* type) {
        auto it = hitBoxes_.find(type->id);
        if (it != hitBoxes_.end()) return it->second;
        // Fallback (model not loaded): a small box just above the anchor.
        SDL_FRect box{-12.0f, -28.0f, 24.0f, 30.0f};
        auto vt = visuals_.find(type->id);
        if (vt != visuals_.end()) {
            std::vector<Tri> scratch;
            // A STRUCTURE is drawn at a fixed facing -- and never rotates, even with
            // the canmove=1/no-velocity FBI quirk -- so its bounds are just its
            // projected vertices.
            //
            // A MOVER has to be covered from every side. That used to mean sampling
            // sixteen headings and unioning the results, which is both approximate
            // (an extreme falling between two samples is missed) and sixteen full
            // projections. It is unnecessary: rotating about Y sends each vertex
            // (x,z) round a circle of radius hypot(x,z) and leaves y untouched, so
            // ONE walk that records those radii gives the union over ALL headings
            // exactly -- x spans +-maxR, and y spans the per-vertex
            // -(y*kProjY) -+ r*kProjZ. collect() gathers it as it goes, so there is
            // still a single transform implementation rather than a copy of it here.
            if (isStructure(type)) {
                float minX = 1e9f, minY = 1e9f, maxX = -1e9f, maxY = -1e9f;
                bool any = false;
                collect(scratch, nullptr, vt->second.model.root, Xform{}, nullptr,
                        0.0f, 0, false);
                for (const auto& t : scratch)
                    for (int i = 0; i < 3; ++i) {
                        minX = std::min(minX, t.v[i].position.x);
                        minY = std::min(minY, t.v[i].position.y);
                        maxX = std::max(maxX, t.v[i].position.x);
                        maxY = std::max(maxY, t.v[i].position.y);
                        any = true;
                    }
                if (any) box = SDL_FRect{minX, minY, maxX - minX, maxY - minY};
            } else {
                RadialExtent ext;
                collect(scratch, nullptr, vt->second.model.root, Xform{}, nullptr,
                        0.0f, 0, false, true, false, &ext);
                if (ext.any)
                    box = SDL_FRect{-ext.maxR, ext.minY, 2.0f * ext.maxR,
                                    ext.maxY - ext.minY};
            }
        }
        return hitBoxes_.emplace(type->id, box).first->second;
    }


    const GameView::RingBox& GameView::unitRingBox(const ta::sim::UnitType* type) {
        auto it = ringBoxes_.find(type->id);
        if (it != ringBoxes_.end()) return it->second;
        RingBox rb;   // defaults cover a model that never loaded
        auto vt = visuals_.find(type->id);
        if (vt != visuals_.end()) {
            float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            bool any = false;
            // Walk the rest pose accumulating MODEL-space bounds. Ground plates are
            // skipped for the same reason collect() refuses to draw them: they are
            // invisible spread-out polygons that would blow the ring out to nothing
            // like the unit's size.
            static const float kNoRot[3] = {0, 0, 0};   // rest pose: no COB rotation
            auto walk = [&](auto&& self, const ta::tdo::Object& o, const Xform& parent,
                            bool isRoot) -> void {
                    Xform xf = parent.then(o.x, o.y, o.z, kNoRot);
                    std::string on = o.name;
                    std::transform(on.begin(), on.end(), on.begin(), ::tolower);
                    auto ends = [&](const char* suf) {
                        size_t n = std::strlen(suf);
                        return on.size() >= n && on.compare(on.size() - n, n, suf) == 0;
                    };
                    bool plate = isRoot || ends("gp") || ends("null") || ends("off") ||
                                 on.find("ground") != std::string::npos ||
                                 on.find("gpoly") != std::string::npos ||
                                 on.find("gpoint") != std::string::npos;
                    if (!plate)
                        for (const auto& p : o.primitives)
                            for (uint16_t vi : p.indices) {
                                size_t v = size_t(vi) * 3;
                                if (v + 2 >= o.vertices.size()) continue;
                                float w[3];
                                xf.apply(o.vertices[v], o.vertices[v + 1], o.vertices[v + 2], w);
                                for (int k = 0; k < 3; ++k) {
                                    lo[k] = std::min(lo[k], w[k]);
                                    hi[k] = std::max(hi[k], w[k]);
                                }
                                any = true;
                            }
                    for (const auto& c : o.children) self(self, c, xf, false);
                };
            walk(walk, vt->second.model.root, Xform{}, true);
            if (any) {
                rb.halfX = std::max(1.0f, (hi[0] - lo[0]) * 0.5f);
                rb.halfZ = std::max(1.0f, (hi[2] - lo[2]) * 0.5f);
                rb.midY = (lo[1] + hi[1]) * 0.5f;
            }
        }
        return ringBoxes_.emplace(type->id, rb).first->second;
    }

    void GameView::pickWorld(float sx, float sy, float& wx, float& wz) {
        float zm = mapView_.zoom();
        // Flat (no-lift) world position of the click.
        float cwx = mapView_.offX() + sx / zm;
        float cwz = mapView_.offY() + sy / zm;
        wx = cwx; wz = cwz;
        const auto& m = mapView_.map();
        if (m.heights.empty()) return;
        heightAbove(cwx, cwz);   // init ref/scales
        // A surface cell of height h renders at flat position (wx - h*scaleX,
        // wz - h*scaleY), so a point that projects to this click satisfies
        // (wx, wz) = (cwx + h*scaleX, cwz + h*scaleY) with h = heightAbove(wx, wz).
        // March h outward (both axes move together along the tilt) and take the
        // front-most self-consistent surface -- the largest h where the candidate's
        // actual height drops through the assumed h. Handles the X/Z coupling so a
        // click on a wall top resolves to that top, not the ground behind it.
        float maxH = std::max(0.0f, float(255 - (heightRef_ < 0 ? 0 : heightRef_)));
        float bestH = 0.0f;
        float prevDiff = heightAbove(cwx, cwz);   // actualH - 0 at h = 0
        const float step = 1.0f;
        for (float h = step; h <= maxH + 2; h += step) {
            float ax = cwx + h * kHeightScaleX_;
            float az = cwz + h * kHeightScale_;
            float diff = heightAbove(ax, az) - h;
            // Front-most self-consistent surface: interpolate the zero-crossing of
            // (actualHeight - h) so we land ON the surface, not past its far edge.
            if (prevDiff > 0 && diff <= 0)
                bestH = (h - step) + step * prevDiff / (prevDiff - diff);
            prevDiff = diff;
        }
        wx = std::clamp(cwx + bestH * kHeightScaleX_, 0.0f, float(m.width * 16 - 1));
        wz = std::clamp(cwz + bestH * kHeightScale_, 0.0f, float(m.height * 16 - 1));
    }

    float GameView::wallOcclusionY(float wx, float wz) {
        const auto& m = mapView_.map();
        if (m.heights.empty()) return 1e9f;
        terrainLift(wx, wz);   // ensure heightRef_/kHeightScale_ are initialised
        float zm = mapView_.zoom();
        int cx = std::clamp(int(wx) / 16, 0, m.width - 1);
        int cz0 = std::clamp(int(wz) / 16, 0, m.height - 1);
        int hUnit = m.heights[size_t(cz0) * m.width + cx];
        float best = 1e9f;
        for (int dz = 1; dz <= kOccScan_; ++dz) {
            int cz = cz0 + dz;
            if (cz >= m.height) break;
            int h = m.heights[size_t(cz) * m.width + cx];
            if (h <= hUnit + 24) continue;   // not a wall relative to this unit
            // The wall's painted top projects north of its heightmap footprint by
            // ~height*kHeightScale_ (the art leans it up-and-back) -- the same scale
            // the unit lift and picking use, so occlusion, seating and clicks agree.
            float proj = std::max(0.0f, float(h - heightRef_) * kHeightScale_);
            float wy = (float(cz) * 16 + 8 - mapView_.offY()) * zm - proj * zm;
            if (wy < best) best = wy;
        }
        return best;
    }

    void GameView::simWorkerLoop() {
        for (;;) {
            SimJob job;
            {
                std::unique_lock<std::mutex> lk(inboxMutex_);
                inboxCv_.wait(lk, [&]{ return simQuit_.load() || !simInbox_.empty(); });
                // Cancelled: drop whatever is queued. The tick in flight (if any)
                // already finished above, so the world is left on a tick boundary.
                if (simCancel_.load()) { simInbox_.clear(); return; }
                if (simInbox_.empty()) return;   // quit signalled and nothing left to process
                job = std::move(simInbox_.front());
                simInbox_.pop_front();
            }
            uint64_t hash = 0;
            {
                std::lock_guard<std::mutex> lk(simMutex_);
                for (const auto& c : job.bundle.cmds) apply(c);
                for (const auto& e : job.bundle.events) applyEvent(e);
                simStep(1.0f / 30.0f);   // world_.tick + captureFrame (publishes a snapshot)
                if (job.wantHash) hash = reportedHash(job.spectator, job.tick);
            }
            if (job.wantHash) {
                std::lock_guard<std::mutex> lk(outboxMutex_);
                simOutbox_.push_back({job.tick, hash});
            }
            simProcessedTick_.store(job.tick, std::memory_order_relaxed);   // for backlog/ack tracking
        }
    }

    void GameView::startSimThread() {
        if (useSimThread_) return;
        useSimThread_ = true;
        simQuit_ = false;
        simThread_ = std::thread([this]{ simWorkerLoop(); });
    }

    void GameView::stopSimThread(bool drain) {
        if (!useSimThread_) return;
        {
            std::lock_guard<std::mutex> lk(inboxMutex_);
            if (!drain) { simCancel_ = true; simInbox_.clear(); }
            simQuit_ = true;
        }
        inboxCv_.notify_one();
        if (simThread_.joinable()) simThread_.join();
        useSimThread_ = false;
    }

    void GameView::beginFrame() {
        std::lock_guard<std::mutex> lk(frameMutex_);
        renderReadIdx_ = published_;
        reading_ = published_;
    }

    void GameView::endFrame() {
        std::lock_guard<std::mutex> lk(frameMutex_);
        reading_ = -1;
    }

    void GameView::finishTerrain() {
        mapView_.finishChunks();
        for (int i = 0; i < 1000 && !miniTex_; ++i) {   // adopt once the crunch lands
            buildMinimap();
            if (!miniTex_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    std::vector<int> GameView::factionMusicTracks(const std::string& side) {
        std::string want = side;   // "ara" -> match "ARAMON" etc.
        std::transform(want.begin(), want.end(), want.begin(), ::toupper);
        const std::string prefixes[] = {
            std::string("ARA=ARAMON"), "TAR=TAROS", "VER=VERUNA", "ZON=ZHON",
            "CRE=CREON"};
        std::string full;
        for (const auto& m : prefixes)
            if (m.substr(0, 3) == want) full = m.substr(4);
        std::vector<int> out;
        try {
            auto sd = vtdf("gamedata/sidedata.tdf");
            for (const auto& key : sd.childOrder) {
                const auto& sec = sd.children.at(key);
                std::string nm = sec.valueOr("name", "");
                std::transform(nm.begin(), nm.end(), nm.begin(), ::toupper);
                if (nm != full) continue;
                std::istringstream ts(sec.valueOr("musictracks", ""));
                int t;
                while (ts >> t) out.push_back(t);
                if (!out.empty()) return out;
            }
        } catch (const std::exception&) {}
        return out;
    }

    std::vector<int> GameView::allFactionMusicTracks() {
        std::vector<int> out;
        try {
            auto sd = vtdf("gamedata/sidedata.tdf");
            for (const auto& key : sd.childOrder) {
                std::istringstream ts(sd.children.at(key).valueOr("musictracks", ""));
                int t;
                while (ts >> t) out.push_back(t);
            }
        } catch (const std::exception&) {}
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;   // empty -> startMusic falls back to all 20
    }

    void GameView::loadBuildFx() {
        static const std::pair<const char*, const char*> maps[] = {
            {"ara", "aramonbuild"}, {"tar", "tarosbuild"},
            {"ver", "verunabuild"}, {"zon", "zhonbuild"}, {"cre", "zhonbuild"},
        };
        for (auto& [side, file] : maps) {
            try {
                // TAF frames are raw ARGB; the palette arg is ignored for them.
                auto pal = ta::gaf::Palette::fromBytes(vread("palettes/ara_textures.pcx"),
                                                        "palettes/ara_textures.pcx");
                std::string tp = "anims/" + std::string(file) + "_4444.taf";
                auto seqs = ta::gaf::load(vread(tp), pal, -1, tp);
                if (seqs.empty()) continue;
                auto& frames = buildFx_[side];
                for (auto& fr : seqs[0].frames) {
                    if (fr.width == 0) continue;
                    SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                       SDL_TEXTUREACCESS_STATIC,
                                                       fr.width, fr.height);
                    SDL_UpdateTexture(t, nullptr, fr.rgba.data(), fr.width * 4);
                    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
                    frames.push_back(t);
                }
            } catch (const std::exception&) {}
        }
    }

    void GameView::issuePerUnit(ta::net::Cmd kind, int value) {
        for (int id : selection_) {
            const auto* u = frameUnitP(id);
            if (!u || u->player != localPlayer_) continue;
            ta::net::Command c;
            c.kind = kind;
            c.unitId = id;
            c.targetId = value;
            issue(c);
        }
    }

    // Retail's own mission labels, from translate/unitmissions.tdf -- the table
    // keyed by the UNITMISSIONCODE_* strings the engine's command table carries.
    // Several of these used to be reasonable-sounding paraphrases that retail
    // simply does not use: it says "Clearing" and not RECLAIMING, "Loading" and
    // not BOARDING, and a half-built unit is an "Intangible Mass".
    const char* GameView::unitStatusText(const UnitR* u) const {
        if (!u || !u->type) return "";
        // UNITMISSIONCODE_SELFDESTRUCT. First, because it is what the unit is
        // doing -- it has stopped taking orders and is walking out on you.
        if (u->selfDestructT >= 0) return "LEAVING YOUR COMMAND";
        if (u->paralyzedFor > 0) return "PARALYZED";
        if (u->underConstruction) return "INTANGIBLE MASS";
        if (!u->active) return "INACTIVE";
        if (u->repairId != 0) return "REPAIRING";
        if (u->reclaimId != 0) return "CLEARING";
        if (u->buildSiteId != 0 || !u->buildQueue.empty()) return "CONJURING";
        if (!u->orders.empty()) {
            const auto& o = u->orders.front();
            if (o.guard) return "GUARDING";
            if (o.patrol) return "PATROLLING";
            if (o.load) return "LOADING";
            if (o.unload) return "UNLOADING";
            if (o.reclaimFeat) return "CLEARING AREA";
            if (o.targetId != 0) return "ATTACKING";
            if (o.attackMove) return "SEEKING TO ATTACK";
            return "MOVING";
        }
        if (u->cloaked) return "CLOAKING";
        return "STANDBY";   // retail's idle label
    }

    bool GameView::handleKey(SDL_Keycode key, uint16_t mod) {
        bool ctrl = (mod & KMOD_CTRL) != 0;
        bool shift = (mod & KMOD_SHIFT) != 0;
        bool alt = (mod & KMOD_ALT) != 0;

        // Pause toggle works without a selection.
        if (key == SDLK_PAUSE) {
            paused_ = !paused_;
            // In a net game (which single-player also is -- it runs a private local
            // server) the sim is driven by delivered ticks, not by update(), so a
            // local flag froze only the animation while the game carried on. Ask the
            // SERVER to stop issuing ticks; every peer then freezes together.
            if (isNet() && mp_) mp_->setPause(paused_);
            return true;
        }
        // +/- (and keypad +/-) step game speed over -10..+10 (0 = normal).
        if (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS ||
            key == SDLK_MINUS || key == SDLK_KP_MINUS) {
            bool up = (key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS);
            if (mp_) {   // lockstep: only the host may re-cadence, and only if unlocked
                if (!mp_->room().opts.speedUnlock) {
                    notice_ = "SPEED LOCKED (host can unlock in the lobby)";
                    noticeTimer_ = 2; return true;
                }
                if (mp_->room().hostId != mp_->myClientId()) {
                    notice_ = "ONLY THE HOST CAN CHANGE SPEED";
                    noticeTimer_ = 2; return true;
                }
                auto o = mp_->room().opts;
                int ns = std::clamp(int(mp_->gameSpeed()) + (up ? 5 : -5), 5, 40);
                if (ns != o.speed) { o.speed = uint8_t(ns); mp_->setGameOptions(o); }
                char nb[24]; std::snprintf(nb, sizeof nb, "GAME SPEED %.1fx", ns / 10.0f);
                notice_ = nb; noticeTimer_ = 2;
                return true;
            }
            gameSpeed_ = std::clamp(gameSpeed_ + (up ? 1 : -1), -10, 10);
            notice_ = "GAME SPEED " + std::string(gameSpeed_ > 0 ? "+" : "") +
                      std::to_string(gameSpeed_);
            noticeTimer_ = 2;
            return true;
        }

        // The number row 1-9,0 addresses control squads (0 = squad 10). CTRL+N assigns a
        // GROUP, ALT+N a FORMATION; SHIFT appends instead of replacing. A plain digit
        // recalls squad N. Squads are lockstep sim state (Cmd::SetSquad); a unit is in one
        // squad at a time, and a number is a group XOR a formation (assigning replaces).
        // Structural (the whole number row), not rebindable.
        int digit = -1;
        if (key >= SDLK_0 && key <= SDLK_9) digit = int(key - SDLK_0);
        if (digit >= 0 && !spectating_) {
            int num = digit == 0 ? 10 : digit;
            if (ctrl || alt) assignSquad(num, alt ? -1 : +1, shift);
            else             recallSquad(num);
            return true;
        }

        // Everything below is a REBINDABLE action: resolve the pressed chord to an
        // Act via the user's hotkey config (src/client/hotkeys) and dispatch.
        switch (hotkeys_.match(int32_t(key), mod)) {
            case ta::Act::ToggleCounts: showCounts_ = !showCounts_; return true;
            case ta::Act::UnitInfo: toggleUnitInfo(); return true;
            case ta::Act::FullScreenRadar:
                // A VIEW action, so it belongs here and not in the order switch
                // below -- that one returns early when nothing is selected, and
                // the full-screen map is exactly what you want with an empty
                // selection. Toggle only: retail's handler ignores its argument
                // list entirely and there is no dismiss-on-click or -on-Escape
                // path, so TAB again is the only way out there too.
                fsRadar_ = !fsRadar_;
                pendingCmd_ = 0;
                return true;
            case ta::Act::SelfDestruct: {   // self-destruct the selected unit(s)
                // Through the command path (Cmd::Destroy), not a direct hp write --
                // a local mutation would silently desync a networked game.
                // Cmd::Destroy TOGGLES a 5s countdown; if any selected unit is
                // already counting down, this press cancels instead of arming.
                int n = 0;
                bool anyArmed = false;
                for (int id : selection_)
                    if (auto* su = frameUnitP(id))
                        if (su->alive() && su->player == localPlayer_) {
                            if (su->selfDestructT >= 0.0f) anyArmed = true;
                            ta::net::Command c;
                            c.kind = ta::net::Cmd::Destroy;
                            c.unitId = id;
                            issue(c);
                            ++n;
                        }
                if (n) {
                    notice_ = anyArmed ? "SELF-DESTRUCT CANCELLED" : "SELF-DESTRUCT IN 5...";
                    noticeTimer_ = 2;
                }
                return true;
            }
            case ta::Act::Disco:
            case ta::Act::Headbang: {
                // One emote at a time -- don't even send the command while a disco or
                // headbang is already running (the sim enforces this too). The busy
                // notice reflects what you're ACTUALLY doing, not the key you pressed.
                bool emoting = frameDiscoActive(localPlayer_) || frameHeadbangActive(localPlayer_);
                if (emoting) {
                    notice_ = frameDiscoActive(localPlayer_) ? "ALREADY GROOVING" : "ALREADY ROCKING";
                    noticeTimer_ = 2; return true;
                }
                bool disco = hotkeys_.match(int32_t(key), mod) == ta::Act::Disco;
                ta::net::Command c;
                c.kind = disco ? ta::net::Cmd::Disco : ta::net::Cmd::Headbang;
                issue(c);   // routed through lockstep so every peer sees the dance
                notice_ = disco ? "DISCO TIME" : "HEADBANG!!";
                noticeTimer_ = 2;
                return true;
            }
            // Selection commands (no armed order, no selection prerequisite).
            case ta::Act::SelectAll:
                selectOwned([](const UnitR&){ return true; });
                return true;
            case ta::Act::SelectSameType: {   // all of the currently-selected type
                const auto* first = selection_.empty() ? nullptr : frameUnitP(selection_.front());
                const auto* t = first ? first->type : nullptr;
                if (t) selectOwned([t](const UnitR& u){ return u.type == t; });
                return true;
            }
            case ta::Act::SelectOnScreen:
                selectOwned([this](const UnitR& u){ return onScreen(u); });
                return true;
            case ta::Act::SelectMonarch: {   // select the Monarch and track it
                const auto* m = playerMonarchId_ >= 0 ? frameUnitP(playerMonarchId_) : nullptr;
                if (m && m->alive()) {
                    selection_ = {playerMonarchId_};
                    trackSel_ = true;
                    centerOnSelection();
                    voice(playerMonarchId_, "select");
                } else {
                    notice_ = "NO MONARCH"; noticeTimer_ = 2;
                }
                return true;
            }
            // Category selects: all owned units matching a predicate (workers, army,
            // navy, casters, flyers, ...). Any-weapon checks scan the weapons list.
            case ta::Act::SelectBuilders:
                selectOwned([](const UnitR& u){ return u.type->isBuilder; });
                return true;
            case ta::Act::SelectFactory:
                selectOwned([](const UnitR& u){ return u.type->isBuilder && u.type->isStructure(); });
                return true;
            case ta::Act::SelectMelee:
                selectOwned([](const UnitR& u){
                    if (!u.type->canMove) return false;
                    for (const auto& w : u.type->weapons) if (w.melee) return true;
                    return false;
                });
                return true;
            case ta::Act::SelectMagic:   // no casters in TA: selects nothing
                selectOwned([](const UnitR&){ return false; });
                return true;
            case ta::Act::SelectBoats:
                selectOwned([](const UnitR& u){
                    return u.type->domain == ta::sim::UnitType::Domain::Water;
                });
                return true;
            case ta::Act::SelectBallistic:
                selectOwned([](const UnitR& u){
                    for (const auto& w : u.type->weapons) if (w.ballistic) return true;
                    return false;
                });
                return true;
            case ta::Act::SelectTroops:   // mobile armed, no navy, not the Monarch
                selectOwned([](const UnitR& u){
                    if (!u.type->canMove || u.type->commander) return false;
                    if (u.type->domain == ta::sim::UnitType::Domain::Water) return false;
                    for (const auto& w : u.type->weapons) if (w.damage > 0) return true;
                    return false;
                });
                return true;
            case ta::Act::SelectArmed:    // anything with a weapon except the Monarch
                selectOwned([](const UnitR& u){
                    if (u.type->commander) return false;
                    for (const auto& w : u.type->weapons) if (w.damage > 0) return true;
                    return false;
                });
                return true;
            case ta::Act::SelectOnScreenType: {   // on-screen units of the selected type
                const auto* first = selection_.empty() ? nullptr : frameUnitP(selection_.front());
                const auto* t = first ? first->type : nullptr;
                if (t) selectOwned([this, t](const UnitR& u){ return u.type == t && onScreen(u); });
                return true;
            }
            case ta::Act::SelectFlying:
                selectOwned([](const UnitR& u){ return u.type->canFly; });
                return true;
            default: break;
        }
        if (ctrl) return false;   // other CTRL combos fall through to the map view

        // Order commands need at least one selected unit.
        if (selection_.empty()) return false;
        switch (hotkeys_.match(int32_t(key), mod)) {
            case ta::Act::FightMove: pendingCmd_ = 'f'; return true;
            case ta::Act::Move:      pendingCmd_ = 'm'; return true;
            case ta::Act::Attack:    pendingCmd_ = 'a'; return true;
            case ta::Act::Patrol:    pendingCmd_ = 'p'; return true;
            case ta::Act::Guard:     pendingCmd_ = 'g'; return true;
            case ta::Act::Heal:      pendingCmd_ = 'r'; return true;   // repair a damaged friendly
            case ta::Act::Load:      pendingCmd_ = 'l'; return true;   // click a unit to carry
            case ta::Act::Unload:    pendingCmd_ = 'u'; return true;   // click the drop destination
            case ta::Act::ClearOrders:                       // clear the whole order queue
                for (int id : selection_) {
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Stop;
                    c.unitId = id;
                    issue(c);
                }
                pendingCmd_ = 0;
                return true;
            case ta::Act::CycleWeapon:                       // cycle active weapon
                if (const auto* u = multiWeaponSel()) {
                    int n = int(u->type->weapons.size());
                    selectWeapon((u->weaponSlot + 1) % n);
                }
                pendingCmd_ = 0;
                return true;
            case ta::Act::Stop:                              // stop (immediate)
                for (int id : selection_) {
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Stop;
                    c.unitId = id;
                    issue(c);
                }
                pendingCmd_ = 0;
                return true;
            case ta::Act::ToggleCloak: {   // cloak on if any selected cloaker is off
                bool anyOn = false, anyCloaker = false;
                for (int id : selection_)
                    if (const auto* u = frameUnitP(id); u && u->type && u->type->canCloak) {
                        anyCloaker = true;
                        if (u->cloakOn) anyOn = true;
                    }
                if (anyCloaker) issuePerUnit(ta::net::Cmd::Cloak, anyOn ? 0 : 1);
                pendingCmd_ = 0;
                return true;
            }
            case ta::Act::ToggleGate: {   // open/close: toggle active on gates (onoffable)
                bool anyActive = false, anyGate = false;
                for (int id : selection_)
                    if (const auto* u = frameUnitP(id); u && u->type && u->type->onOffable) {
                        anyGate = true;
                        if (u->active) anyActive = true;
                    }
                if (anyGate) issuePerUnit(ta::net::Cmd::SetActive, anyActive ? 0 : 1);
                pendingCmd_ = 0;
                return true;
            }
            case ta::Act::TrackSelection:                    // track/untrack selection
                trackSel_ = !trackSel_;
                if (trackSel_) centerOnSelection();
                pendingCmd_ = 0;
                return true;
            case ta::Act::NextUnit: cycleNextUnit(); return true;
            default: return false;
        }
    }

    void GameView::issueSquad(int unitId, int val) {   // val: 0 none, +N group N, -N formation N
        ta::net::Command c;
        c.kind = ta::net::Cmd::SetSquad;
        c.unitId = unitId;
        c.targetId = val;
        issue(c);
    }

    void GameView::assignSquad(int num, int sign, bool append) {
        if (spectating_ || num < 1 || num > 10 || selection_.empty()) return;
        int want = sign * num;
        std::unordered_set<int> sel(selection_.begin(), selection_.end());
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (u.alive() && u.player == localPlayer_ && std::abs(int(u.squad)) == num &&
                sel.find(u.id) == sel.end())
                issueSquad(u.id, append ? want : 0);   // append: retype it; else evict it
        }
        for (int id : selection_)
            if (const auto* u = frameUnitP(id); u && u->alive() && u->player == localPlayer_)
                issueSquad(id, want);
    }

    void GameView::recallSquad(int num) {
        if (spectating_) return;
        selection_.clear();
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (u.alive() && u.player == localPlayer_ && u.type &&
                std::abs(int(u.squad)) == num && !u.type->isBuilder)
                selection_.push_back(u.id);
        }
        if (!selection_.empty()) { centerOn(selection_.front()); voice(selection_.front(), "select"); }
    }

    void GameView::clearSquad() {
        if (spectating_) return;
        for (int id : selection_)
            if (const auto* u = frameUnitP(id);
                u && u->alive() && u->player == localPlayer_ && u->squad)
                issueSquad(id, 0);
    }

    bool GameView::onScreen(const UnitR& u) const {
        float sx = (u.x - mapView_.offX()) * mapView_.zoom();
        float sy = (u.z - mapView_.offY()) * mapView_.zoom();
        return sx >= 0 && sy >= 0 && sx <= float(winW_) && sy <= float(winH_) - barH();
    }

    bool GameView::lbHot(const SDL_FRect& r) const {
        float mx = mouseX_ / lobbyScale_ - lobbyOffX_, my = mouseY_ / lobbyScale_ - lobbyOffY_;
        return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
    }

    void GameView::lbField(float x, float y, float w, const std::string& label,
                 const std::string& value, int id) {
        blockText(label, x, y, 1.6f, {150, 155, 170, 255});
        SDL_FRect r{x, y + 14, w, 22};
        bool active = (lbField_ == id);
        SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255);
        SDL_RenderFillRectF(ren_, &r);
        SDL_SetRenderDrawColor(ren_, active ? 200 : 90, active ? 210 : 100, active ? 130 : 130, 255);
        SDL_RenderDrawRectF(ren_, &r);
        std::string shown = value + (active ? "_" : "");
        blockText(shown, x + 6, y + 20, 1.8f, {225, 230, 240, 255});
        lobbyHots_.push_back({r, [this, id] { lbField_ = id; SDL_StartTextInput(); }});
    }

    std::string GameView::readOta(const std::string& tntPath) const {
        std::filesystem::path ota = tntPath; ota.replace_extension(".ota");
        try { auto d = vfs_.read(ota.generic_string()); return std::string(d.begin(), d.end()); }
        catch (...) { return {}; }
    }

    std::string* GameView::lbFieldBuf() {
        switch (lbField_) {
            case 1: return &createName_; case 2: return &createPass_;
            case 3: return &joinPass_; case 4: return &chatDraft_;
            default: return nullptr;
        }
    }

    std::vector<std::pair<float, float>> GameView::buildLinePositions(
        float x0, float z0, float x1, float z1) const {
        std::vector<std::pair<float, float>> out;
        if (!placing_) return out;
        // Full footprint width plus a one-cell gap: edge-to-edge spacing lets integer
        // cell rounding of the un-aligned drag tip adjacent sites into a shared
        // footprint cell, which makes canPlace reject every other one.
        float sp = (std::max({placing_->footX, placing_->footZ, 1}) + 1) * 16.0f;
        float dx = x1 - x0, dz = z1 - z0, len = std::sqrt(dx * dx + dz * dz);
        int n = int(len / sp);
        float ux = len > 1e-3f ? dx / len : 0, uz = len > 1e-3f ? dz / len : 0;
        for (int i = 0; i <= n; ++i)
            out.push_back({x0 + ux * sp * i, z0 + uz * sp * i});
        return out;
    }

    int GameView::rosterIndexOf(int unitId) {
        auto* u = frameUnitP(unitId);
        if (!u || !u->type) return -1;
        for (size_t i = 0; i < missionRoster_.size(); ++i)
            if (missionRoster_[i] == u->type->id) return int(i);
        return -1;
    }

    int32_t GameView::mapCommand(int sub, const std::vector<int32_t>& a) {
        switch (sub) {
            case 0:   // define (and arm) region: rect or circle, cells
                if (a.size() == 5) regions_[a[0]] = {a[1], a[2], a[3], a[4], true, true};
                else if (a.size() == 4)
                    regions_[a[0]] = {a[1], a[2], a[3], 0, false, true};
                return 0;
            case 1:   // disarm region (one-shot triggers disarm themselves)
                if (!a.empty()) {
                    auto it = regions_.find(a[0]);
                    if (it != regions_.end()) it->second.armed = false;
                }
                return 0;
            case 2: {   // nearest unit of player a[0] to cell (a[1],a[2])
                if (a.size() < 3) return 0;
                int player = std::clamp(a[0] - 1, 0, 3);
                float wx = float(a[1]) * 16 + 8, wz = float(a[2]) * 16 + 8;
                int best = 0;
                float bestD = 1e18f;
                for (auto& u : world_.units()) {
                    if (!u.alive() || u.player != player) continue;
                    float dx = u.x - wx, dz = u.z - wz;
                    if (dx * dx + dz * dz < bestD) { bestD = dx * dx + dz * dz; best = u.id; }
                }
                return best;
            }
            case 4: {   // HEURISTIC: spawn a reinforcement for player a[0]
                if (a.size() < 3) return 0;
                int player = std::clamp(a[0] - 1, 0, 3);
                auto& pool = reinfPool_[player];
                if (pool.empty()) return 0;
                const std::string& type = pool[size_t(reinfIdx_++) % pool.size()];
                float wx = float(a[1]) * 16 + 8, wz = float(a[2]) * 16 + 8;
                int id = spawn(type, wx + float(reinfIdx_ % 3) * 18,
                               wz + float(reinfIdx_ % 2) * 18, 3.14159f, player);
                if (id >= 0 && player == 0 && hudFont_.ok()) postNotice("REINFORCEMENTS!", 6);
                if (trace_) std::printf("SPAWN4 %s player%d at %d,%d -> id %d\n",
                                        type.c_str(), player, a[1], a[2], id);
                return id;
            }
            case 3: case 5: {   // HEURISTIC: activate spawned unit - join force
                if (a.empty()) return 0;
                const auto* u = frameUnitP(a[0]);
                if (!u) return 0;
                float bx = 0, bz = 0;
                int n = 0;
                for (auto& o : world_.units())
                    if (o.alive() && o.player == u->player && o.id != u->id && o.type &&
                        o.type->canMove) { bx += o.x; bz += o.z; ++n; }
                if (n) world_.attackMove(a[0], bx / float(n), bz / float(n), false);
                return 0;
            }
            case 8: case 9: case 12: case 13: case 14:
                if (a.empty()) return missionTowerIdx_;   // type-constant heuristic
                return 0;
            default:
                return 0;
        }
    }

    void GameView::voice(int unitId, const std::string& event) {
        // Retail command feedback: one WEIGHTED draw from the sound class's
        // event pool -- the symbolic "_NN-note" entry (weight 100) vs the voice
        // lines (weight 1 each), so units bong ~99% of the time and speak
        // occasionally. The note names no file: it is the faction click tone
        // (sounds/tone<side>.wav, what click.hpi packs replace) pitch-shifted
        // to the named chromatic semitone (A=1..G#=12) -- a different pitch
        // per command type. Units without a class fall back to the plain tone
        // on command events.
        const auto* u = frameUnitP(unitId);
        const std::string* wav = nullptr;
        if (u && u->type && !u->type->soundClass.empty())
            wav = soundClasses_.pick(u->type->soundClass, event, salt_++);
        auto isTone = [](const std::string& w) {
            return w.size() >= 4 &&
                   std::tolower((unsigned char)w[0]) == 't' &&
                   std::tolower((unsigned char)w[1]) == 'o' &&
                   std::tolower((unsigned char)w[2]) == 'n' &&
                   std::tolower((unsigned char)w[3]) == 'e';
        };
        if (wav && !wav->empty() && (*wav)[0] == '_') {
            int semi = std::atoi(wav->c_str() + 1);
            playClickTone(semi);
        } else if (wav && isTone(*wav)) {
            // Some classes name the faction tone directly (ARAKING's every
            // event is just TONEARA): route it through the boosted tone path,
            // or it plays half-volume and quiet click.hpi replacements vanish.
            playClickTone();
        } else if (wav) {
            sounds_.playWorld(*wav, u->x, u->z);
        } else if (event != "select") {
            playClickTone();
        }
    }

    void GameView::loadExplosionClasses() {
        if (explosionsLoaded_) return;
        explosionsLoaded_ = true;
        try {
            auto root = vtdf("gamedata/explosions/explosions.tdf");
            for (const auto& cls : root.childOrder) {
                const auto& node = root.children.at(cls);
                auto& list = explosionClasses_[cls];   // cls is already lowercased
                for (const auto& vn : node.childOrder) {
                    const auto& v = node.children.at(vn);
                    std::string a = v.valueOr("anim", v.valueOr("gaf", ""));
                    if (!a.empty()) {
                        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                        list.push_back(a);
                    }
                }
            }
        } catch (const std::exception&) {}
    }

    void GameView::triggerShake(float mag, float dur) {
        if (mag <= 0 || dur <= 0) return;
        // Let a stronger/longer quake override a fading one.
        if (mag * dur >= shakeMag_ * shakeTime_) {
            shakeMag_ = mag; shakeDur_ = dur; shakeTime_ = dur;
        }
    }


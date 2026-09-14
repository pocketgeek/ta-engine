#include <ctime>
#include "client/gameview.h"

// Out-of-line GameView method definitions (net concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    uint8_t GameView::mpCapacity() const {
        // A generated map's capacity is the player count baked into its "~gen1~" id.
        // The lobby sets mpMapId_; the headless "game" flow only has mapPath_ -- honour
        // either, since sim::parseStartPositions can't parse a synthetic id.
        const std::string& gid = tak::mapgen::isGeneratedMapId(mpMapId_) ? mpMapId_ : mapPath_;
        if (tak::mapgen::isGeneratedMapId(gid))
            return uint8_t(std::clamp<int>(tak::mapgen::decodeMapId(gid).players, 2, tak::net::kMaxSlots));
        int n = int(parseStartPositions().size());
        return uint8_t(std::clamp(n < 2 ? 2 : n, 2, tak::net::kMaxSlots));
    }

    void GameView::setMpClient(tak::net::MpClient* mp) {
        mp_ = mp;
    }

    void GameView::saveNetReplay() {
        if (replaySaved_ || !mp_ || !mp_->recording() || mp_->replayLog().empty()) return;
        replaySaved_ = true;
        // Beside settings.ini: settingsPath() is <prefdir>/settings.ini, so trim the
        // file name off rather than rebuilding the platform path by hand.
        std::string dir = tak::settingsPath();
        const size_t cut = dir.find_last_of("/\\");
        if (cut == std::string::npos) return;
        dir.erase(cut + 1);
        const std::string path = saveReplayFile(dir, *mp_, uint64_t(std::time(nullptr)),
                                                gameDataHash());
        if (!path.empty()) {
            std::fprintf(stderr, "replay: wrote %s (%zu ticks)\n",
                         path.c_str(), mp_->replayLog().size());
            postNotice("REPLAY SAVED", 5);
        } else {
            std::fprintf(stderr, "replay: could not write into %s\n", dir.c_str());
        }
    }

    void GameView::startMpGame(const tak::net::RoomView& room, uint32_t seed) {
        replaySaved_ = false;   // a fresh game gets a fresh recording
        int maxSlot = 0;
        for (int i = 0; i < tak::net::kMaxSlots; ++i)
            if (room.slots[i].type == 1 || room.slots[i].type == 2) maxSlot = i;
        // Retail's loading screen covers the whole start: everything below blocks the
        // render loop for a second or more, and the wait on the other players after it.
        loadScreen_ = std::make_unique<tak::LoadScreen>(
            ren_, vfs_, room.mission.empty() ? room.mapId : room.mission, settings_);
        {
            std::array<std::string, 8> names{};
            for (int i = 0; i <= maxSlot && i < 8; ++i) {
                if (room.slots[i].type != 1 && room.slots[i].type != 2) continue;
                names[size_t(i)] = !room.slots[i].name.empty()
                                       ? room.slots[i].name
                                       : (room.slots[i].type == 2 ? "COMPUTER" : "PLAYER");
                // An AI slot has nothing to load and never reports in, so its bar is
                // full from the start -- only humans are worth waiting on.
                if (room.slots[i].type == 2) loadScreen_->setSlotDone(i);
            }
            loadScreen_->setPlayers(names, room.mySlot);
        }
        loadScreen_->step("MOUNTING GAME DATA", 5);
        // Build the world through the SHARED setup so the server's referee sim and
        // every client produce a bit-identical world (and hash). Client-only bits
        // (colours, camera, local player, panel) stay here.
        // The game's balance AND override tier are the ROOM's (lobby choices), which
        // may differ from how this client was launched. Remount to the room's tier
        // (so gameplay overrides -- or their absence -- match the referee) and
        // rebuild the registry, so our world and hash agree with the server's
        // referee. remountPolicy already rebuilds the registry for the current
        // crusades setting; handle a crusades-only change separately.
        remountPolicy(room.opts.overridePolicy);
        if ((room.opts.crusades != 0) != crusades_) {
            crusades_ = room.opts.crusades != 0;
            registry_ = tak::sim::TypeRegistry{};
            tak::sim::setupRegistry(registry_, vfs_, crusades_);
        }
        // Campaign mission: build the SAME world the referee did. setupMission is
        // deterministic (terrain + placed units + the in-sim god script), so our sim,
        // the referee, and every peer stay byte-identical -- the mission runs in
        // lockstep with no relayed actions. See docs/campaign-design.md.
        if (!room.mission.empty()) {
            mapPath_ = "missions/" + room.mission + ".tnt";
            resetMinimap();   // its thread reads the map being swapped
            loadScreen_->step("LOADING TERRAIN", 30);
            mapView_.reload(vfs_, mapPath_);
            int human = 0;
            tak::sim::MissionSetup ms;
            tak::sim::setupMission(world_, registry_, vfs_, room.mission, human, &ms);
            missionFullVision_ = ms.fullVision;
            missionPreMapped_ = ms.preMapped;
            loadFeatures();
            if (loadScreen_) loadScreen_->step("LOADING CURSORS", 35);
            warmCursors();   // cursor art + hardware reconstruction, off the frame path
            // Per-mission unit restriction: missions/<stem>.tdf lists the unit ids this
            // mission allows; the human's conjure menu is filtered to it (UI only).
            missionAllowed_.clear();
            if (std::string tdfp = "missions/" + room.mission + ".tdf"; vfs_.has(tdfp)) {
                std::vector<uint8_t> tb = vfs_.read(tdfp);
                tak::tdf::Node root = tak::tdf::parseText(std::string(tb.begin(), tb.end()), tdfp);
                for (const auto& [name, node] : root.children) { (void)node; missionAllowed_.push_back(name); }
            }
            missionObjectives_ = tak::loadObjectives(vfs_, room.mission);   // in-game panel
            showObjectives_ = true;
            // The player commands the mission's human player; for the common case its
            // index equals our room slot (TODO: seat the client at `human` otherwise).
            localPlayer_ = human;
            world_.setVisPlayer(localPlayer_);
            for (auto& u : world_.units())
                if (u.player == localPlayer_ && u.type) { playerMonarchId_ = u.id; builderId_ = u.id; break; }
            if (!missionAllowed_.empty())
                std::fprintf(stderr, "mission %s: conjure menu restricted to %zu allowed unit types\n",
                             room.mission.c_str(), missionAllowed_.size());
            const char* sides[5] = {"ara", "tar", "ver", "zon", "cre"};
            if (room.mySlot >= 0) side_ = sides[room.slots[room.mySlot].faction % 5];
            loadScreen_->step("LOADING INTERFACE", 90);
            loadPanel(side_);
            loadGui(side_);
            loadScreen_->step("WAITING FOR PLAYERS", 100);
            for (auto& u : world_.units())
                if (u.player == localPlayer_ && u.type) {
                    mapView_.setOffset(u.x - 640 / 0.9f, u.z - 400 / 0.9f);
                    break;
                }
            return;
        }
        // Adopt the ROOM's map (the host's / lobby selection), which may differ from
        // this session's launch map. Point mapPath_ at it AND reload the render terrain
        // (mapView_), so the rendered map, the local sim, and the referee all agree.
        // Without this, picking a non-default map drew the launch map's terrain under a
        // different map's sim -- phantom water, a monarch out in it, and misaligned fog.
        if (std::string rp = tak::hpi::findMap(vfs_, room.mapId); !rp.empty()) mapPath_ = rp;
        resetMinimap();   // its thread reads the map being swapped
        loadScreen_->step("LOADING TERRAIN", 30);
        mapView_.reload(vfs_, mapPath_);
        loadScreen_->step("BUILDING THE WORLD", 65);
        tak::sim::MatchConfig cfg;
        cfg.vfs = &vfs_;
        cfg.mapPath = mapPath_;
        cfg.gods = room.opts.gods != 0;
        cfg.unitCap = room.opts.unitCap;
        cfg.monarchExpendable = room.opts.monarchExpendable != 0;
        cfg.stressTest = room.opts.stressTest != 0;
        cfg.benchmark = room.opts.benchmark;
        cfg.randomStarts = room.opts.randomStarts != 0;
        cfg.startSeed = seed;   // same seed the referee shuffles with
        if (room.opts.benchmark) benchmarkLevel_ = room.opts.benchmark;   // for the results label
        cfg.slots.resize(size_t(maxSlot + 1));
        for (int i = 0; i <= maxSlot; ++i) {
            const auto& s = room.slots[i];
            // Mirror the referee's per-slot income multiplier (Absurd AI = 2x) from the
            // shared aiLevel, so hashed mana stays identical to the server (lockstep).
            float mm = s.type == 2
                ? tak::ai::incomeMultFor(tak::ai::difficultyFromLevel(s.aiLevel)) : 1.0f;
            cfg.slots[size_t(i)] = {s.type == 1 || s.type == 2, s.faction % 5, s.team, mm};
            colorSlot_[i & 7] = s.color % 10;
            playerAi_[i & 7] = (s.type == 2);
            playerName_[i & 7] = !s.name.empty()
                                     ? s.name   // human name, or the AI's random name
                                     : s.type == 2 ? ("AI " + std::to_string(i + 1))
                                                   : ("Player " + std::to_string(i + 1));
        }
        auto spots = tak::sim::setupMatch(world_, registry_, cfg);
        // Rebuild the rendered feature sprites (features_) from the map we actually
        // loaded -- they were built once in the ctor from the launch map, so on a
        // different chosen map the trees/houses you SEE would be the launch map's,
        // while the reclaimable features in the sim (world_.features()) are this map's.
        // That mismatch made right-drag reclaim miss (the box covered the wrong stuff).
        // setupMatch is authoritative for the sim; loadFeatures only re-adds the same
        // map's mana/nav idempotently (setTerrain already rebuilt the nav).
        loadFeatures();
        warmCursors();   // cursor art + hardware reconstruction, off the frame path
        // client-only presentation
        localPlayer_ = room.mySlot < 0 ? 0 : room.mySlot;
        world_.setVisPlayer(localPlayer_);
        world_.setFogExplored(room.opts.fogExplored == 1 || missionPreMapped_);
        // A mission overrides the room's fog rule with its own: lineofsight=0 plays
        // revealed, mapping=1 starts the terrain explored. Display-only either way.
        if (missionFullVision_) noFog_ = true;
        if (room.opts.fogExplored == 2) noFog_ = true;       // FULL VISION: no fog at all --
                                                             // same spectator path, so the
                                                             // wasted-visibility work is skipped too
        if (benchmarkMode_) benchmarkBaseline();             // t=0 baseline for the perf samples
        for (auto& u : world_.units())
            if (u.player == localPlayer_ && u.type) { playerMonarchId_ = u.id; builderId_ = u.id; break; }
        const char* sides[5] = {"ara", "tar", "ver", "zon", "cre"};
        side_ = sides[room.slots[localPlayer_].faction % 5];
        loadScreen_->step("LOADING INTERFACE", 90);
        loadPanel(side_);
        loadGui(side_);
        loadScreen_->step("WAITING FOR PLAYERS", 100);
        if (!spots.empty())
            mapView_.setOffset(spots[0].first - 640 / 0.9f, spots[0].second - 400 / 0.9f);
    }

    bool GameView::mpStep() {
        if (!mp_->poll()) { netError_ = mp_->error().empty() ? "disconnected" : mp_->error(); return false; }
        if (mp_->desynced()) { netError_ = mp_->desyncReason(); return false; }
        // Send at the rate the server DRAINS (kCmdCapPerTick per sim tick), metered
        // by elapsed ticks rather than by frames. Two earlier versions of this were
        // both frame-coupled: flushing the whole outbox every render step offered
        // far more than the server accepts, and flushing one batch per step still
        // tied throughput to the frame rate in the other direction -- at 10fps that
        // is 640 commands/sec against a server willing to take 1920.
        //
        // Credit saturates at kCmdQueueCap, exactly the queue the server keeps, so
        // an honest client can never have more outstanding than the server can hold:
        // overflow stops being reachable without the loss being the client's own
        // doing. See cmdSendCredit.
        if (netTick_ != lastSendTick_) {
            cmdCredit_ = tak::net::cmdSendCredit(cmdCredit_, netTick_ - lastSendTick_);
            lastSendTick_ = netTick_;
        }
        // Rate AND window: the credit above says how fast, cmdInFlight_ says how
        // much may be outstanding. Without the window the rate limiter alone let a
        // stalled uplink accumulate 1024 unacknowledged commands against a 512
        // queue, and the server dropped the difference when they all landed.
        const int sendable = tak::net::cmdSendWindow(cmdCredit_, cmdInFlight_);
        if (!outbox_.empty() && sendable > 0 && !cmdCatchUp_) {
            const size_t n = std::min(outbox_.size(), size_t(sendable));
            cmdCredit_ -= int(n);
            cmdInFlight_ += int(n);
            if (n == outbox_.size()) { mp_->sendCommands(outbox_); outbox_.clear(); }
            else {
                mp_->sendCommands({outbox_.begin(), outbox_.begin() + n});
                outbox_.erase(outbox_.begin(), outbox_.begin() + n);
            }
        }
        // Decide once whether to run the sim on its own worker thread. On for interactive
        // net games (the whole point -- keeps world_.tick off the render thread); off for the
        // headless harness/replay (inline, byte-identical + deterministic) unless
        // TAK_SIM_THREAD forces it on to VERIFY the threaded sim against the referee.
        if (!simThreadDecided_) {
            simThreadDecided_ = true;
            wantSimThread_ = simThreadMode_;
            if (wantSimThread_) startSimThread();
        }
        // Simulate every delivered tick, but cap per frame so a big catch-up
        // (rejoin replay) stays responsive rather than freezing for seconds.
        tak::net::Bundle bd;
        int drained = 0;
        auto simTick = [&] {
            // Our own commands coming back in this bundle are the server's
            // acknowledgement that it took them (lockstep relays every tick to
            // every peer, sender included). That is what retires in-flight credit.
            //
            // NOT during a rejoin replay. Catch-up feeds the whole bundle log back,
            // including this player's commands from BEFORE the disconnect -- history,
            // not acknowledgement. Counting those retired commands we had only just
            // sent, reopening the send window past what the server can hold. We send
            // nothing while catching up anyway (see cmdCatchUp_), so there is nothing
            // legitimate to retire here.
            if (!cmdCatchUp_)
                if (int ms = mp_->room().mySlot; ms >= 0) {
                    for (const auto& c : bd.cmds)
                        if (int(c.player) == ms && cmdInFlight_ > 0) --cmdInFlight_;
                }
            if (useSimThread_) {
                // Hand this tick's bundle to the sim worker (FIFO == lockstep tick order).
                // world_ is simulated there; the state hash comes back via simOutbox_ and is
                // sent to the server below. The main thread stays free for the render.
                SimJob job;
                job.bundle = bd;
                job.tick = netTick_;
                job.wantHash = (netTick_ % uint32_t(tak::net::kHashPeriod) == 0);
                job.spectator = mp_->isSpectator();
                { std::lock_guard<std::mutex> lk(inboxMutex_); simInbox_.push_back(std::move(job)); }
                inboxCv_.notify_one();
            } else {
                for (const auto& c : bd.cmds) apply(c);
                for (const auto& e : bd.events) applyEvent(e);
                // Only the SIM half per bundle (speedMult() is 1 in net games): the
                // cosmetic half runs once after the drain, so a rejoin/spectate
                // catch-up replays lockstep state without replaying 512 ticks of
                // sounds, effects and VM dispatch per frame.
                simStep(1.0f / 30.0f);
                // Report our processed tick every kHashPeriod. A seated player sends its
                // state hash (desync check + flow-control ack); a spectator sends one too
                // -- purely as a progress ACK so the server can pace an all-AI watch game
                // to what the spectator can sustain (its hash is never desync-checked, since
                // an all-AI room has no seated players to form a consensus). A spectator's
                // hash is a progress ACK only, so skip the O(units) stateHash for it.
                if (netTick_ % uint32_t(tak::net::kHashPeriod) == 0)
                    mp_->sendHash(netTick_, reportedHash(mp_->isSpectator(), netTick_));
            }
            ++netTick_;
            ++drained;
            // During a heavy catch-up (a spectator fast-forwarding a big backlog at high
            // game speed), service the connection every so often -- answer the server's
            // keepalive pings and keep draining our recv buffer. Otherwise a spectator
            // busy simulating for >15s stops ponging and the server drops it ("peer
            // closed"). poll() is cheap (recv + ping/pong + flush); we ignore its result
            // here, the next top-of-mpStep poll() surfaces any real error.
            if ((drained & 63) == 0) mp_->poll();
        };
        if (netDelay_ == -2) {   // one-time init from the env
            // The adaptive jitter buffer is ON by default: it only ever reduces
            // stalls and is pure pacing (byte-identical sim). TAK_NET_DELAY overrides
            // -- "0" disables it (drain every bundle immediately), a positive integer
            // pins a fixed reserve depth, "auto" (or unset) self-sizes to the link.
            const char* e = tak::devEnv("TAK_NET_DELAY");
            if (!e || std::string(e) == "auto") { netAuto_ = true; netDelay_ = 3; mp_->enableRttProbe(); }
            else netDelay_ = std::max(0, std::atoi(e));
        }
        if (netAuto_) {
            // Size the buffer to cover the measured bundle-arrival jitter, with an
            // RTT-scaled floor, clamped. Recomputed each frame so it tracks the link.
            int kJit = int(std::ceil(mp_->arrivalJitterMs() / (1000.0f / 30.0f)));
            int kRtt = int(std::ceil(mp_->rttMs() / 60.0f));   // gentle RTT floor
            netDelay_ = std::clamp(2 + std::max(kJit, kRtt), 2, 16);
        }
        if (netDelay_ <= 0) {
            // Default: drain to the newest delivered bundle every frame.
            while (outcome_ == 0 && drained < 512 && mp_->takeBundle(netTick_, bd)) simTick();
            // Stall metric: 0 ticks played this frame while future bundles ARE
            // buffered means the one we need is late (head-of-line block) -- a stall.
            ++netBenchFrames_;
            if (drained == 0 && mp_->bufferedBundles() > 0) ++netBenchStalls_;
        } else {
            // Jitter buffer: fill an initial reserve of netDelay_ bundles, then pace
            // the sim on the wall clock at ~30 Hz, staying that many bundles behind
            // the newest received. A late bundle is covered from the reserve; only a
            // gap deeper than the reserve stalls. Same bundles, same order -> the sim
            // and every hash are byte-identical, so this is a pure pacing change.
            uint64_t now = SDL_GetTicks64();
            float dt = netStepMs_ ? std::min(0.25f, float(now - netStepMs_) / 1000.0f) : 0.0f;
            netStepMs_ = now;
            int buffered = int(mp_->bufferedBundles());
            if (!netBufReady_) {
                if (buffered < netDelay_) return true;   // still filling the reserve
                netBufReady_ = true; netAccum_ = 0;
            }
            // Adaptive playout: run the sim clock slightly fast/slow to servo the
            // buffer depth to the target (netDelay_). Too deep -> play a touch faster
            // (drain toward target, shed latency); too shallow -> slower (rebuild the
            // reserve). Holds the added latency tight at ~netDelay_ ticks.
            float err = float(buffered - netDelay_);
            // Base playout tracks the game speed (server ticks that much faster/slower).
            float sp = std::max(1, int(mp_->gameSpeed())) / 10.0f;
            float rate = 30.0f * sp * std::clamp(1.0f + 0.06f * err, 0.7f, 1.3f);
            netAccum_ += dt * rate;            // accumulates fractional TICKS now
            int budget = int(netAccum_);
            netAccum_ -= float(budget);
            // A deep backlog (rejoin replay, or the client fell behind) is NOT jitter
            // -- fast-forward it back down to the target reserve instead of pacing.
            if (buffered > netDelay_ + 60) budget = 512;
            budget = std::min(budget, 512);
            while (outcome_ == 0 && drained < budget && mp_->takeBundle(netTick_, bd)) simTick();
            // Stall metric: the wall clock wanted more ticks than we could play
            // because the next bundle isn't buffered yet (jitter exceeded the
            // reserve). One count per starved frame.
            ++netBenchFrames_;
            if (drained < budget && !mp_->haveBundle(netTick_)) ++netBenchStalls_;
        }
        // Caught up: the replayed log is spent and what arrives now is live. Reset the
        // window rather than trust a count accumulated across the reconnect -- the
        // server cleared this connection's queue when we left.
        //
        // AFTER both drain paths, not inside one. There are two -- the immediate
        // drain (netDelay_ <= 0) and the jitter-buffered one -- and the default is
        // the buffered one, so a check placed in the other branch never ran at all
        // and a rejoined player stayed locked out of issuing orders for the whole
        // game.
        //
        // The gate is the SERVER'S boundary, not our buffer depth. Two local
        // heuristics failed here: "buffered == 0" essentially never fires because
        // the adaptive buffer keeps a reserve in hand, and "down to the reserve"
        // cannot tell the end of history from a GAP BETWEEN REPLAY CHUNKS -- the
        // replay is streamed in socket-paced chunks, so the buffer legitimately
        // runs dry mid-history and the gate opened early, putting the false-
        // acknowledgement bug straight back.
        if (cmdCatchUp_ && netTick_ >= cmdReplayEnd_) {
            cmdCatchUp_ = false;
            if (tak::devEnv("TAK_NETLOG"))
                std::fprintf(stderr, "catch-up complete at tick %u\n", netTick_);
            cmdInFlight_ = 0;
            cmdCredit_ = tak::net::kCmdCapPerTick;
        }
        // Send the worker's finished per-tick state hashes to the server (lockstep desync
        // check + flow-control ack). Drained here on the main thread -- mp_ has a single owner.
        if (useSimThread_) {
            std::deque<HashJob> done;
            { std::lock_guard<std::mutex> lk(outboxMutex_); done.swap(simOutbox_); }
            for (const auto& h : done) mp_->sendHash(h.tick, h.hash);
            drainPendingNotice();   // apply any HUD notice the worker posted (god/mission/scenario)
        }
        // Cosmetics once per frame, covering the game time actually played.
        if (drained > 0) cosmeticStep(float(drained) / 30.0f);
        // Spectator progress heartbeat: keep the server's flow-control ack FRESH even
        // when we're caught up and not draining. The per-tick hash send only fires while
        // draining (at 30-tick boundaries), so a caught-up spectator would stop acking;
        // the server -- pacing an all-AI game to that now-stale ack -- would then hold
        // forever (ACTUAL 0.0x) and eventually time us out ("recv failed"). One ack per
        // ~0.4s wall keeps the server's view of our position current.
        if (mp_->isSpectator()) {
            uint64_t nowHb = SDL_GetTicks64();
            if (nowHb - lastSpecAckMs_ > 400) {
                lastSpecAckMs_ = nowHb;
                // A spectator's hash is a pure progress ACK -- the server never
                // desync-checks it (an all-AI room has no seated consensus). So send a
                // trivial value, NOT world_.stateHash(): folding thousands of units into an
                // FNV every 0.4s on the render thread was a periodic hitch that scaled with
                // the battle. The TICK is what the server's flow control reads -- and it must
                // be the tick actually PROCESSED (the worker's, when threaded), not the push
                // position netTick_, or the server thinks we're further along than we are and
                // over-delivers (growing the sim backlog).
                uint32_t ackTick = useSimThread_ ? simProcessedTick_.load(std::memory_order_relaxed)
                                                 : (netTick_ ? netTick_ - 1 : 0);
                mp_->sendHash(ackTick, 0);
            }
        }
        // Measure the ACTUAL game speed: how fast our sim really advances (ticks/sec
        // over a ~0.5s window, /30 = a speed multiplier). At the requested speed this
        // tracks it; when a client can't keep up (or the server paces to the slowest)
        // it reads lower. Shown on the F4 board next to the requested speed.
        {
            uint64_t na = SDL_GetTicks64();
            if (actualSpeedT0_ == 0) { actualSpeedT0_ = na; actualSpeedTick0_ = netTick_; }
            else if (na - actualSpeedT0_ >= 500) {
                float ips = float(int64_t(netTick_) - int64_t(actualSpeedTick0_)) * 1000.0f
                            / float(na - actualSpeedT0_);
                actualSpeed_ = ips / 30.0f;
                actualSpeedT0_ = na; actualSpeedTick0_ = netTick_;
            }
        }
        // "Machine too slow" guard: if the backlog stays deep for a sustained
        // stretch, this client can't process ticks as fast as they arrive and
        // will never catch up -- fail clearly instead of falling ever further
        // behind (or reconnect-looping). With the sim worker, the main thread drains
        // mp_'s buffer FAST (into simInbox_), so bufferedBundles() no longer reflects a
        // slow client -- the backlog is (pushed - processed). Count both.
        uint64_t now = SDL_GetTicks64();
        long long simBacklog = useSimThread_
            ? std::max<long long>(0, (long long)netTick_ - 1 - (long long)simProcessedTick_.load(std::memory_order_relaxed))
            : 0;
        if (mp_->bufferedBundles() + simBacklog > 900) {
            if (!mpSlowSinceMs_) mpSlowSinceMs_ = now;
            else if (now - mpSlowSinceMs_ > 5000) {
                if (mp_->isSpectator()) {
                    // A spectator is a WATCHER, not a lockstep participant -- it holds
                    // nobody up. If it can't sustain the game speed (e.g. a big battle
                    // at 4x) it just LAGS behind the live game instead of disconnecting;
                    // the keepalive poll in the drain keeps the connection healthy. Show
                    // a transient hint and carry on. (Lowering the speed catches it up.)
                    notice_ = "SPEED TOO HIGH -- SPECTATOR LAGGING";
                    noticeTimer_ = 2;
                    mpSlowSinceMs_ = now;   // re-arm; don't spam
                } else {
                    netError_ = "this machine can't keep up with the game speed";
                    return false;
                }
            }
        } else {
            mpSlowSinceMs_ = 0;
        }
        return true;
    }

    bool GameView::mpAutoStep(int autoMode, const std::string& mapId, bool crusades) {
        using S = tak::net::MpClient::State;
        if (!mp_->poll()) { netError_ = mp_->error(); return false; }
        S st = mp_->state();
        if (st == S::Done) { if (netError_.empty()) netError_ = mp_->error(); return false; }
        if (st == S::Lobby && (autoMode == 1 || autoMode == 4 || autoMode == 7 || autoMode == 8)) {
            tak::net::GameOptions o; o.crusades = crusades ? 1 : 0;
            o.overridePolicy = uint8_t(policy_);   // room tier = this host's launch tier
            // TAK_SPEED: set the game speed in tenths (10 = 1x) for headless timing
            // tests -- re-cadences the server without touching the (deterministic) sim.
            if (const char* sp = tak::devEnv("TAK_SPEED")) o.speed = uint8_t(std::clamp(std::atoi(sp), 1, 40));
            if (tak::devEnv("TAK_STRESS")) o.stressTest = 1;   // headless: spawn ~95% cap per AI
            if (const char* be = tak::devEnv("TAK_BENCH")) {   // headless: benchmark run
                int lv = std::atoi(be);                        // TAK_BENCH=<level 1..6>, default High
                benchmarkLevel_ = (lv >= 1 && lv <= tak::sim::kBenchLevels) ? lv : 3;
                benchmarkMode_ = true;                         // (forces watch + 8 AI + cap 8 + Ulasem below)
            }
            if (const char* uc = tak::devEnv("TAK_UNITCAP")) o.unitCap = uint16_t(std::atoi(uc));
            // The remaining room options, as headless knobs. Without these a harness
            // could not reach the code they gate -- and GODS in particular is where a
            // desync has already hidden once: summoning used to run only on the client,
            // so the referee's world ran a unit short from the first god onward. A
            // desync hunt that cannot turn gods on cannot find that class of bug.
            if (tak::devEnv("TAK_GODS")) o.gods = 1;
            if (tak::devEnv("TAK_RANDOM_STARTS")) o.randomStarts = 1;
            if (tak::devEnv("TAK_MONARCH_EXPENDABLE")) o.monarchExpendable = 1;
            if (tak::devEnv("TAK_FORFEIT_SELFDESTRUCT")) o.forfeitSelfDestruct = 1;
            // TAK_FOG=0|1|2 forces the room's fog rule (not explored / explored /
            // full vision) so the setting can be tested end to end without driving
            // the lobby by hand.
            if (const char* fg = tak::devEnv("TAK_FOG"))
                o.fogExplored = uint8_t(std::clamp(std::atoi(fg), 0, 2));
            // TAK_MP_WATCH: host creates the game as a spectator (no slot) so every
            // slot can be an AI -- an all-AI game to watch.
            if (benchmarkMode_) o.benchmark = uint8_t(benchmarkLevel_);   // menu Benchmark: intensity level
            // Benchmark is an all-AI WATCH run (host takes no slot) on Ulasem Arena, forced
            // to 8 slots regardless of the map's start-position count (setupMatch synthesises
            // the extra starts), private (not in the browser).
            bool watch = (autoMode == 1 && tak::devEnv("TAK_MP_WATCH")) || benchmarkMode_;
            // Mode 7 is interactive SINGLE-PLAYER: a private game (hidden from the
            // browser) with one server-run AI opponent.
            // Mode 8 is a single-player CAMPAIGN mission: a private game whose world is
            // built from the mission bundle (server + every peer run setupMission).
            bool priv = autoMode == 7 || autoMode == 8 || benchmarkMode_;
            std::string mission = autoMode == 8 ? missionStem_ : std::string();
            uint8_t cap = (autoMode == 8 || benchmarkMode_) ? tak::net::kMaxSlots : mpCapacity();
            // Exact stem match (findMap): the map file is "ulasem arena.tnt".
            std::string createMap = benchmarkMode_ ? std::string("Ulasem Arena") : mapId;
            mp_->createGame(benchmarkMode_ ? "Benchmark" : (priv ? "Single Player" : "headless"),
                            "", createMap, o, cap, watch, priv, mission);
        } else if (st == S::Lobby && autoMode == 5) {
            // Rejoin: read the resume ticket the original session saved and
            // reconnect to the held slot.
            uint32_t gid = 0; uint64_t tok = 0;
            if (readResume(gid, tok) && gid) { mp_->rejoin(gid, tok); mpReadied_ = true; }
        } else if (st == S::Lobby && autoMode == 6) {
            // Spectate: poll the list and watch the first running game.
            if (SDL_GetTicks64() - mpListMs_ > 300) { mp_->listGames(); mpListMs_ = SDL_GetTicks64(); }
            for (const auto& g : mp_->games())
                if (g.running) { mp_->spectate(g.id, ""); break; }
        } else if (st == S::Lobby && (autoMode == 2 || autoMode == 3)) {
            // Poll the game list; join the first, or (mode 3) create if none appear.
            if (SDL_GetTicks64() - mpListMs_ > 300) { mp_->listGames(); mpListMs_ = SDL_GetTicks64(); }
            if (!mp_->games().empty()) mp_->joinGame(mp_->games().front().id, "");
            else if (autoMode == 3 && mpListMs_ && SDL_GetTicks64() - mpFirstListMs_ > 800) {
                tak::net::GameOptions o; o.crusades = crusades ? 1 : 0;
            o.overridePolicy = uint8_t(policy_);   // room tier = this host's launch tier
                mp_->createGame(mapId, "", mapId, o, mpCapacity());
            }
            if (!mpFirstListMs_) mpFirstListMs_ = SDL_GetTicks64();
        } else if (st == S::InRoom && autoMode && !mpReadied_) {
            const auto& r = mp_->room();
            // Host-spectator (TAK_MP_WATCH): seat AIs in the LOW slots (0..N-1) and
            // don't seat self -- an all-AI game the host just watches.
            if (r.mySlot < 0 && (benchmarkMode_ || (autoMode == 1 && tak::devEnv("TAK_MP_WATCH")))) {
                const char* ai = tak::devEnv("TAK_MP_AIS");
                // Benchmark: always a full 8-faction FFA (each AI its own team -> they fight,
                // which is the point of the load test). FIXED factions by slot (k%5):
                // AI1 Aramon, AI2 Taros, AI3 Veruna, AI4 Zhon, AI5 Creon, AI6 Aramon,
                // AI7 Taros, AI8 Veruna -- never random.
                int nAi = benchmarkMode_ ? int(tak::net::kMaxSlots)
                                         : std::clamp(ai ? std::atoi(ai) : 2, 2, int(tak::net::kMaxSlots));
                for (int k = 0; k < nAi; ++k)
                    mp_->setSlot(k, 2, uint8_t(k % 5), uint8_t(k), uint8_t(k), 1, aiLevelEnv());
                mpReadied_ = true;
            } else if (autoMode == 8 && r.mySlot >= 0) {
                // Campaign mission: seat the human ready and start; the mission's own
                // script drives the enemies (no skirmish AI slots).
                mp_->setSlot(r.mySlot, 1, facIdx(side_), uint8_t(r.mySlot),
                             uint8_t(r.mySlot), 1);
                mp_->startGame();
                mpStarted_ = true;
                mpReadied_ = true;
            } else if (autoMode == 7 && r.mySlot >= 0) {
                // Single-player: seat self UNREADY and hand off to the interactive
                // Room, where the player adds one or more AI opponents, then readies
                // up and starts.
                mp_->setSlot(r.mySlot, 1, facIdx(side_), uint8_t(r.mySlot),
                             uint8_t(r.mySlot), 0);
                // Headless test hook: TAK_SP_AIS=N seats N AI opponents and starts
                // immediately (the interactive path leaves this to the player).
                if (const char* na = tak::devEnv("TAK_SP_AIS")) {
                    // The auto-start hook must ready-up the host (interactive SP now
                    // seats unready, which would otherwise block startGame()).
                    mp_->setSlot(r.mySlot, 1, facIdx(side_), uint8_t(r.mySlot),
                                 uint8_t(r.mySlot), 1);
                    int n = std::clamp(std::atoi(na), 1, int(tak::net::kMaxSlots) - 1);
                    for (int k = 0; k < n && k + 1 < int(tak::net::kMaxSlots); ++k) {
                        int slot = k + 1;
                        mp_->setSlot(slot, 2, uint8_t((facIdx(aiSide_) + k) % 5),
                                     uint8_t(slot), uint8_t(slot), 1, aiLevelEnv());
                    }
                    mp_->startGame();
                    mpStarted_ = true;
                }
                mpReadied_ = true;
            } else if (r.mySlot >= 0) {
                mp_->setSlot(r.mySlot, 1, uint8_t(r.mySlot % 5), uint8_t(r.mySlot),
                             uint8_t(r.mySlot), 1);
                // autoMode 4 (AI-game host): also seat one AI opponent in slot 1.
                if (autoMode == 4 && r.mySlot == 0)
                    mp_->setSlot(1, 2, 1, 1, 1, 1, aiLevelEnv());   // AI, tar, colour 1, team 1
                // Host stress harness: TAK_MP_AIS=N seats N server AIs in the TOP
                // slots, leaving the low slots for human joiners.
                if (autoMode == 1 && r.mySlot == 0)
                    if (const char* ai = tak::devEnv("TAK_MP_AIS")) {
                        int nAi = std::clamp(std::atoi(ai), 0, tak::net::kMaxSlots - 1);
                        for (int k = 0; k < nAi; ++k) {
                            int slot = tak::net::kMaxSlots - 1 - k;
                            mp_->setSlot(slot, 2, uint8_t(slot % 5), uint8_t(slot),
                                         uint8_t(slot), 1, aiLevelEnv());   // AI, distinct colour/team
                        }
                    }
                mpReadied_ = true;
            }
        } else if (st == S::InRoom && (autoMode == 1 || autoMode == 3 || autoMode == 4) &&
                   !mpStarted_ &&
                   mp_->room().hostId == mp_->myClientId()) {
            // (mode 7 single-player does NOT auto-start: the player adds AIs and
            //  clicks START in the Room.)
            int ready = 0;
            for (int i = 0; i < tak::net::kMaxSlots; ++i) {
                const auto& s = mp_->room().slots[i];
                if ((s.type == 1 && s.ready) || s.type == 2) ++ready;   // human-ready or AI
            }
            // Default: start with any 2 ready. TAK_MP_WAIT=N holds for a full lobby.
            static const int wantReady = [] {
                const char* w = tak::devEnv("TAK_MP_WAIT"); return w ? std::atoi(w) : 2;
            }();
            if (ready >= wantReady) { mp_->startGame(); mpStarted_ = true; }
        } else if (mp_->isRejoin()) {
            // Rejoin OR spectate (checked BEFORE the state branches -- the replayed
            // bundles may already have flipped the state to InGame): reset the sim
            // and replay from tick 0. The server has queued the whole bundle log
            // after the GameStarting; mpStep drains it, fast-forwarding to now.
            bool spec = mp_->isSpectator();
            mp_->clearRejoin();
            world_.resetForReplay();
            netTick_ = 0; outcome_ = 0; netError_.clear();
            // Replaying history from tick 0: hold our own orders and stop counting
            // acknowledgements until the log is spent (see cmdCatchUp_).
            // The SERVER says where history ends; we do not guess (see
            // Client::replayTicks and the gate below).
            cmdReplayEnd_ = mp_->replayTicks();
            cmdCatchUp_ = cmdReplayEnd_ > 0;
            cmdInFlight_ = 0;
            outbox_.clear();   // pre-disconnect orders are moot; the server dropped them
            startMpGame(mp_->startRoom(), mp_->startSeed());
            if (spec) {
                spectating_ = true;   // watch-only: no fog, no control, no resume
                noFog_ = true;
                world_.setVisPlayer(-1);   // sees everything -> skip the wasted O(units) fog pass
                showCounts_ = true;   // the F4 scoreboard is on by default while spectating
                gameStartMs_ = SDL_GetTicks64();
            } else {
                mp_->reportLoaded(gameDataHash());
                writeResume(mp_->gameId(), mp_->resumeToken());
            }
            mpSetupDone_ = true;
            std::fprintf(stderr, spec ? "spectating -- replaying to catch up...\n"
                                      : "rejoined -- replaying to catch up...\n");
            mpStep();   // drain the replay this frame
        } else if (mp_->starting() && !mpSetupDone_) {
            startMpGame(mp_->startRoom(), mp_->startSeed());
            if (mp_->isSpectator()) {
                // Host-spectator (create-as-spectator): watch-only, no fog, no slot,
                // no resume ticket, and nothing to report loaded.
                spectating_ = true;
                noFog_ = true;
                world_.setVisPlayer(-1);   // sees everything -> skip the wasted O(units) fog pass
                showCounts_ = true;   // F4 scoreboard on by default while spectating
                gameStartMs_ = SDL_GetTicks64();
            } else {
                mp_->reportLoaded(gameDataHash());
                writeResume(mp_->gameId(), mp_->resumeToken());   // reconnect ticket
            }
            mpSetupDone_ = true;
        } else if (st == S::InGame) {
            return mpStep();
        }
        return true;
    }

    void GameView::amphibDemo() {
        amphib_ = true;
        const auto* shipType = registry_.find("vertrans");
        const auto& ground = world_.nav();
        const auto& water = world_.navFor(shipType);
        float cx = float(mapView_.map().width) * 8, cz = float(mapView_.map().height) * 8;

        // Walk outward from the island center along a direction: last land
        // cell with deep water a bit beyond = a beach; return both spots.
        auto findBeach = [&](float ax, float az, float* bx, float* bz, float* wx2,
                             float* wz2) {
            for (float r = 0; r < 4000; r += 16) {
                int gx = int(cx + ax * r) / 16, gz = int(cz + az * r) / 16;
                if (!ground.walkable(gx, gz)) {
                    for (float rw = r + 48; rw < r + 400; rw += 16) {
                        int wxc = int(cx + ax * rw) / 16, wzc = int(cz + az * rw) / 16;
                        if (water.walkable(wxc, wzc)) {
                            *bx = cx + ax * (r - 32);
                            *bz = cz + az * (r - 32);
                            *wx2 = cx + ax * rw;
                            *wz2 = cz + az * rw;
                            return true;
                        }
                    }
                }
            }
            return false;
        };
        float bax, baz, wax, waz, bbx, bbz, wbx, wbz;
        if (!findBeach(-0.9f, 0.44f, &bax, &baz, &wax, &waz) ||
            !findBeach(0.44f, -0.9f, &bbx, &bbz, &wbx, &wbz)) {
            std::printf("amphib: no beaches found\n");
            return;
        }
        std::printf("amphib: embark beach (%.0f,%.0f) landing (%.0f,%.0f)\n", bax, baz,
                    bbx, bbz);
        amphibLandX_ = bbx;
        amphibLandZ_ = bbz;
        amphibSeaX_ = wbx;
        amphibSeaZ_ = wbz;

        transportId_ = spawn("vertrans", wax, waz, 0, 0);
        const char* squad[] = {"araarch", "araarch", "arasword", "arasword"};
        int i = 0;
        for (const char* t : squad) {
            int id = spawn(t, bax + float(i % 2) * 24 - 12, baz + float(i / 2) * 24 - 12,
                           0, 0);
            if (id >= 0) {
                world_.loadInto(id, transportId_);
                ++amphibSquad_;
            }
            ++i;
        }
    }

    const std::vector<uint8_t>* GameView::kingdomPalette(const std::string& kingdom) {
        if (kingdom.empty()) return nullptr;
        auto it = kingdomPals_.find(kingdom);
        if (it == kingdomPals_.end()) {
            std::vector<uint8_t> rgba;
            try {
                auto pal = tak::gaf::Palette::fromBytes(vfs_.read("palettes/" + kingdom + ".pcx"),
                                                        kingdom + ".pcx");
                rgba.assign(&pal.rgba[0][0], &pal.rgba[0][0] + 256 * 4);
            } catch (...) { rgba.clear(); }
            it = kingdomPals_.emplace(kingdom, std::move(rgba)).first;
        }
        return it->second.empty() ? nullptr : &it->second;
    }

    void GameView::clampMapScroll() {
        mapScroll_ = std::clamp(mapScroll_, 0, std::max(0, mapTotalRows_ - mapVisRows_));
    }


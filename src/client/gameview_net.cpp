#include <ctime>
#include "sim/sides.h"
#include "tnt/ota.h"
#include "client/gameview.h"
#include "net/crypto.h"
#include "net/matchconfig.h"
#include "campaign/campaign.h"
#include "util/u8path.h"
#include <filesystem>

// Out-of-line GameView method definitions (net concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    uint8_t GameView::mpCapacity(bool mapScenario) const { return capacityFor(mpMapId_, mapPath_, mapScenario); }

    uint8_t GameView::capacityFor(const std::string& mapId, const std::string& path, bool mapScenario) const {
        // A generated map's capacity is the player count baked into its "~gen1~" id.
        // The lobby sets mpMapId_; the headless "game" flow only has mapPath_ -- honour
        // either, since sim::parseStartPositions can't parse a synthetic id.
        const std::string& gid = ta::mapgen::isGeneratedMapId(mapId) ? mapId : path;
        if (ta::mapgen::isGeneratedMapId(gid))
            return uint8_t(std::clamp<int>(ta::mapgen::decodeMapId(gid).players, 2, ta::net::kMaxSlots));
        const int n = int(ta::sim::parseStartPositions(vfs_, path).size());
        // Capacity is available seats, not a minimum player count. A room that plays
        // its map's scenario opens every seat that scenario needs (its placement owners
        // and the rule banks that do something), which can lie past the start positions;
        // the verified lobby may still start a solo scenario without filling the rest.
        auto scenarioPath=path;
        ta::hpi::Vfs scenarioData(&vfs_);
        if(const auto cached=vfs_.cachedMap(scenarioPath)) {
            scenarioPath=cached->first;scenarioData.setMapFiles(cached->second);
        }
        return seatsFor(n, ta::sim::inspectScenario(scenarioData,scenarioPath), mapScenario);
    }

    uint8_t GameView::seatsFor(int starts, const ta::sim::ScenarioInfo& s, bool scenarioOn) {
        int n = starts;
        if(s.active(scenarioOn))
            for(int i=0;i<ta::net::kMaxSlots;++i)
                if(s.requiredSeats()>>i & 1)n=std::max(n,i+1);
        return uint8_t(std::clamp(n < 2 ? 2 : n, 2, ta::net::kMaxSlots));
    }

    ta::sim::ScenarioInfo GameView::mapScenarioInfo(const std::string& path) const {
        try {
            auto mapPath=path;
            ta::hpi::Vfs view(&vfs_);
            if(const auto cached=vfs_.cachedMap(path)) {
                mapPath=cached->first;view.setMapFiles(cached->second);
            }
            return ta::sim::inspectScenario(view,mapPath,&registry_);
        } catch(const std::exception&) {return {};}   // an unreadable MapCache archive
    }

    void GameView::setMpClient(ta::net::MpClient* mp) {
        if (mp_ && mp_ != mp) mp_->setRecordingRestartHook(nullptr);
        mp_ = mp;
        // A new game on this connection: finish the old game's file while its bundles
        // are still in the log (the hook runs before the log is cleared). The new game
        // opens its own once it is set up (startMpGame).
        if (mp_) mp_->setRecordingRestartHook([this] {
            if ((replayRec_ || replayRecRetryMs_) &&
                (replayRecGame_ != mp_->room().id || replayRecSeed_ != mp_->startSeed()))
                finishReplayRecording();
        });
    }

    // Watch-only view of a live net game (spectator join or create-as-spectator):
    // no fog, no control, the F4 scoreboard on. A frame captured before this may
    // still hold the seat's plane -- noFog_, not an empty plane, is what tells the
    // overlays the spectator sees everything (and stops captureFog's work).
    void GameView::beginSpectating() {
        spectating_ = true;
        noFog_ = true;
        showCounts_ = true;
        gameStartMs_ = SDL_GetTicks64();
    }

    // The whole game is kept in memory (mp_->replayLog()), so a recording that cannot
    // start now -- the folder unwritable or the disk full at this moment -- is tried
    // again every few seconds (pumpReplayRecording) and a last time when the game
    // ends (finishReplayRecording); once it opens it still begins at tick 0. Only the
    // first failure is logged.
    void GameView::tryOpenReplayRecording(bool first) {
        const uint64_t now = SDL_GetTicks64();
        replayRecRetryMs_ = now + 5000;
        const std::string dir = ta::replayDir();
        if (dir.empty()) {
            if (first) std::fprintf(stderr, "replay: no replay folder yet; will keep trying\n");
            return;
        }
        auto rec = std::make_unique<ta::net::ReplayWriter>();
        if (!rec->open(dir, replayRecHeader_, replayRecGame_, uint64_t(std::time(nullptr)))) {
            if (first)
                std::fprintf(stderr, "replay: cannot record yet (%s); will keep trying\n", rec->error().c_str());
            return;
        }
        std::fprintf(stderr, "replay: recording to %s\n", rec->partPath().c_str());
        replayRec_ = std::move(rec);
        replayRecRetryMs_ = 0;
        replayFlushMs_ = replaySyncMs_ = now;
    }

    // Main thread, every mpStep. Bundles are handed to the OS about once a second, so
    // a crash or a kill loses at most that much; they reach the disk itself every ten
    // seconds (a power cut), and at once when the result lands.
    void GameView::pumpReplayRecording() {
        if (!mp_) return;
        if (!replayRec_ && replayRecRetryMs_ && SDL_GetTicks64() >= replayRecRetryMs_) tryOpenReplayRecording(false);
        if (!replayRec_) return;
        replayRec_->append(mp_->replayLog(), mp_->hashLog());
        const uint64_t now = SDL_GetTicks64();
        const bool result = replayResultSync_.exchange(false, std::memory_order_relaxed);
        if (result || replayRec_->buffered() >= (64u << 10) ||
            (replayRec_->buffered() && now - replayFlushMs_ >= 1000)) {
            // A failed write (disk full) keeps its bytes and is retried at the next
            // flush and at finish(); it is logged once per run of failures.
            const bool wasFailing = replayRec_->failing();
            if (!replayRec_->flush() && !wasFailing)
                std::fprintf(stderr, "replay: %s; will keep trying\n", replayRec_->error().c_str());
            else if (wasFailing && !replayRec_->failing())
                std::fprintf(stderr, "replay: writing %s again\n", replayRec_->partPath().c_str());
            replayFlushMs_ = now;
        }
        if (result || now - replaySyncMs_ >= 10000) {
            replayRec_->syncAsync();
            replaySyncMs_ = now;
        }
        if (result) {
            // What is on disk now is the game up to its result. Recording goes on:
            // whatever is played or watched after it belongs to the replay too. A
            // watcher's goes on only while the game does: with no human seated the
            // server ends it on the result's own tick (Msg::GameOver), and the recording
            // ends there. The line says so when the server already has.
            if (mp_->gameOver())
                std::fprintf(stderr, "replay: result at tick %u, %u ticks recorded; the server ended the game after %u ticks, "
                             "and the recording ends there\n", netTick_, replayRec_->bundles(), mp_->gameOverTicks());
            else
                std::fprintf(stderr, "replay: result at tick %u, %u ticks recorded; recording continues%s\n",
                             netTick_, replayRec_->bundles(), spectating_ ? " while the game does" : "");
            if (!replayRec_->failing()) postNotice("REPLAY SAVED", 5);
        }
    }

    // ownLog: mp_'s log is still the recorded game's (the destructor, the restart
    // hook). beginGameRecording passes false: by then the log is the next game's.
    void GameView::finishReplayRecording(bool ownLog) {
        ownLog = ownLog && mp_;
        if (!replayRec_ && replayRecRetryMs_ && ownLog) tryOpenReplayRecording(false);   // a last try
        replayRecRetryMs_ = 0;   // this game's recording is over, opened or not
        if (!replayRec_) return;
        if (ownLog) replayRec_->append(mp_->replayLog(), mp_->hashLog());
        const uint32_t ticks = replayRec_->bundles();
        const std::string path = replayRec_->finish();
        if (!path.empty())
            std::fprintf(stderr, "replay: wrote %s (%u ticks)\n", path.c_str(), ticks);
        else if (ticks)
            std::fprintf(stderr, "replay: could not finish %s (%s); it is completed at the next start\n",
                         replayRec_->partPath().c_str(), replayRec_->error().c_str());
        replayRec_.reset();
    }

    void GameView::resetDiplomacy(const ta::net::RoomView& room) {
        // Chat and voice start with your allies only: the occupied slots on your team
        // (you included). A spectator has no team and keeps everyone.
        uint8_t allies=0xff;
        if (room.mySlot>=0 && room.mySlot<ta::net::kMaxSlots && room.mySlot<8) {
            allies=0;
            for (int i=0;i<ta::net::kMaxSlots && i<8;++i)
                if ((room.slots[i].type==1 || room.slots[i].type==2) && room.slots[i].team==room.slots[room.mySlot].team)
                    allies|=uint8_t(1u<<i);
        }
        chatRecipients_=allies;voiceRecipients_=allies;requestedManaSharing_.reset();
        // Spectators: off (the Diplomacy SPECTATORS row ticks them in). A spectator never
        // chats or talks in a running game at all.
        chatSpectators_=voiceSpectators_=false;
        voiceVolume_.fill(100);voiceVolumeDrag_=-1;humanSlots_=0;
        for (int i=0;i<ta::net::kMaxSlots && i<8;++i)
            if (room.slots[i].type==1) humanSlots_|=uint8_t(1u<<i);
        giveUnitsMenu_=false;giveUnitsHots_.clear();
    }

    void GameView::startMpGame(const ta::net::RoomView& room, uint32_t seed) {
        {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            missionSoundQueue_.clear();
            unitSoundQueue_.clear();
        }
        resetDiplomacy(room);
        radarCode_ = false;   // power codes start off in every match
        buildLineGap_ = 0;    // so does the tight shift-drag line spacing
        buildLineWheel_ = 0;
        buildLineWheelMs_ = 0;
        buildLineTrail_ = {};
        poster_.reset();
        // A room field (SAY) focused when the host started is let go: the game
        // has no such field, and its text input would otherwise hold push to talk off.
        if (lbField_) { lbField_ = 0; SDL_StopTextInput(); }
        resultParticipants_.reset();
        if (room.mission.empty()) {
            resultParticipants_=0;
            for (int i=0;i<ta::net::kMaxSlots;++i)
                if (room.slots[i].type==1 || room.slots[i].type==2)
                    *resultParticipants_|=uint8_t(1u<<i);
        }
        int maxSlot = 0;
        for (int i = 0; i < ta::net::kMaxSlots; ++i)
            if (room.slots[i].type == 1 || room.slots[i].type == 2) maxSlot = i;
        // Retail's loading screen covers the whole start: everything below blocks the
        // render loop for a second or more, and the wait on the other players after it.
        loadScreen_ = std::make_unique<ta::LoadScreen>(
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
        logSimThreads(simThreadMode_);   // what this game's simulation runs on (latchSimThreads)
        // Build the world through the SHARED setup so the server's referee sim and
        // every client produce a bit-identical world (and hash). Client-only bits
        // (colours, camera, local player, panel) stay here.
        // The game mods are the ROOM's (lobby choices), which may differ from how
        // this client was launched. Remount with the room's game mods (so gameplay
        // data -- or its absence -- matches the referee); remountMods rebuilds the
        // registry, so our world and hash agree with the server's referee.
#ifndef NDEBUG
        previewLifetimeProbe(false);   // TA_PREVIEW_LIFETIME_TEST=gen|big
#endif
        resetMinimap();
        mapView_.quiesce();
        // The lobby's previews read vfs_ on their own threads too: a guest can still have
        // the full-window map open, and generated maps' rolls (superseded ones included)
        // may still be running. READY and START do not wait for them; this does.
        stopPreviewReaders();
        remountMods();
        vfs_.setMapFiles(mp_ && mp_->mapPackage() && room.mission.empty() &&
            !ta::mapgen::isGeneratedMapId(room.mapId) ? mp_->mapPackage()->files : room.mission.empty()
                ? std::make_shared<const ta::hpi::Vfs::Files>() : nullptr);
        featureDefs_.clear(); featurePals_.clear();
        for (auto& [name, art] : featureArt_) art.destroyTextures();
        featureArt_.clear();
        // Campaign mission: build the SAME world the referee did. setupMission is
        // deterministic (terrain + placed units + the in-sim god script), so our sim,
        // the referee, and every peer stay byte-identical -- the mission runs in
        // lockstep with no relayed actions. See docs/campaign-design.md.
        if (!room.mission.empty()) {
            mapPath_ = ta::sim::missionMapPath(vfs_, room.mission);
            resetMinimap();   // its thread reads the map being swapped
            loadScreen_->step("LOADING TERRAIN", 30);
            mapView_.reload(vfs_, mapPath_);
            int human = 0;
            ta::sim::MissionSetup ms;
            // (Debug) TA_FORCE_WIN / TA_FORCE_LOSE: the outcome forced by the mission's
            // own scenario rules; the local server this client started reads the same.
            const auto forced = ta::forcedOutcomeMission(vfs_, room.mission);
            ta::sim::setupMission(writableWorld(), registry_, forced ? *forced : vfs_, room.mission, human, &ms);
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
                ta::tdf::Node root = ta::tdf::parseText(std::string(tb.begin(), tb.end()), tdfp);
                for (const auto& [name, node] : root.children) { (void)node; missionAllowed_.push_back(name); }
            }
            missionObjectives_ = ta::loadObjectives(vfs_, room.mission);   // in-game panel
            showObjectives_ = true;
            // The player commands the mission's human player: always the .ota's
            // Player1, world slot 0, as in retail (setupMission). A campaign room
            // seats its host at slot 0 and refuses seat moves, so the server's
            // command stamping (the sender's seat) and this view name the same player.
            localPlayer_ = human;
            for (auto& u : world_.units())
                if (u.player == localPlayer_ && u.type) { playerMonarchId_ = u.id; builderId_ = u.id; break; }
            if (!missionAllowed_.empty())
                std::fprintf(stderr, "mission %s: conjure menu restricted to %zu allowed unit types\n",
                             room.mission.c_str(), missionAllowed_.size());
            if (room.mySlot >= 0) side_ = ta::sides::lowerPrefix(room.slots[room.mySlot].faction);
            loadScreen_->step("LOADING INTERFACE", 90);
            loadPanel(side_);
            loadGui(side_);
            loadScreen_->step("WAITING FOR PLAYERS", 100);
            startAtMonarch();
            beginGameRecording();
            return;
        }
        // Adopt the ROOM's map (the host's / lobby selection), which may differ from
        // this session's launch map. Point mapPath_ at it AND reload the render terrain
        // (mapView_), so the rendered map, the local sim, and the referee all agree.
        // Without this, picking a non-default map drew the launch map's terrain under a
        // different map's sim -- phantom water, a monarch out in it, and misaligned fog.
        mapPath_ = mp_ && mp_->mapPackage() && !ta::mapgen::isGeneratedMapId(room.mapId)
            ? mp_->mapPackage()->mapPath : ta::hpi::findMap(vfs_, room.mapId);
        if (mapPath_.empty()) throw std::runtime_error("selected map is missing: " + room.mapId);
        resetMinimap();   // its thread reads the map being swapped
        loadScreen_->step("LOADING TERRAIN", 30);
        mapView_.reload(vfs_, mapPath_);
        loadScreen_->step("BUILDING THE WORLD", 65);
        // The referee's own builder (net/matchconfig.h) -- the same seed, slot table,
        // Absurd AI income and player count -- so hashed state matches (lockstep).
        ta::sim::MatchConfig cfg = ta::net::matchConfigFor(room.opts, room.slots, seed);
        cfg.vfs = &vfs_;
        cfg.mapPath = mapPath_;
        cfg.scenarioViewPlayer = localPlayer_;
        cfg.scenarioTrace = scenarioTrace_;
        if (room.opts.benchmark) benchmarkLevel_ = room.opts.benchmark;   // for the results label
        for (int i = 0; i <= maxSlot; ++i) {
            const auto& s = room.slots[i];
            colorSlot_[i & 7] = s.color % 10;
            playerAi_[i & 7] = (s.type == 2);
            playerName_[i & 7] = !s.name.empty()
                                     ? s.name   // human name, or the AI's random name
                                     : s.type == 2 ? ("AI " + std::to_string(i + 1))
                                                   : ("Player " + std::to_string(i + 1));
        }
        auto spots = ta::sim::setupMatch(writableWorld(), registry_, cfg);
        if (world_.hasScenarioOutcomes())   // the map's own scenario (MAP SCENARIO or a Test Map copy)
            std::fprintf(stderr, "map scenario: %zu units at the start\n", world_.units().size());
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
        // FOG OF WAR: EXPLORED (and a mission's mapping=1) reach the display through the
        // World's room vision (mapRevealedFor). A mission overrides the room's rule with
        // its own: lineofsight=0 plays revealed.
        if (missionFullVision_) noFog_ = true;
        if (room.opts.fogExplored == 2) noFog_ = true;       // FULL VISION: no fog at all --
                                                             // the spectator path, so
                                                             // captureFog does no work
        if (benchmarkMode_) benchmarkBaseline();             // t=0 baseline for the perf samples
        for (auto& u : world_.units())
            if (u.player == localPlayer_ && u.type) { playerMonarchId_ = u.id; builderId_ = u.id; break; }
        side_ = ta::sides::lowerPrefix(room.slots[localPlayer_].faction);
        loadScreen_->step("LOADING INTERFACE", 90);
        loadPanel(side_);
        loadGui(side_);
        loadScreen_->step("WAITING FOR PLAYERS", 100);
        startAtMonarch();
        beginGameRecording();
    }

    // The last step of startMpGame, on BOTH of its paths (a campaign mission returns
    // early). Record from here: the header needs the data this game runs on (the
    // room's tier, mounted above) and nothing has been simulated yet. A rejoin or a
    // spectator gets the backlog first, so its recording starts from tick 0 too.
    void GameView::beginGameRecording() {
        gameDataHash_ = gameDataHash();   // reported at Loaded and recorded in the replay header
        if (!mp_ || !mp_->recording() || replayMode_) return;
        // The Benchmark is a performance run, not a game: it is never recorded.
        if (benchmarkMode_ || mp_->room().opts.benchmark) return;
        if ((replayRec_ || replayRecRetryMs_) && replayRecGame_ == mp_->room().id &&
            replayRecSeed_ == mp_->startSeed())
            return;   // a rejoin of the game already being recorded: keep its file
        finishReplayRecording(/*ownLog=*/false);   // normally done already, by the restart hook
        // Everything the file needs about this game, taken now: the restart hook can
        // run (and try a last open) after the next game's GameStarting replaced it.
        replayRecHeader_ = clientReplayHeader(*mp_, gameDataHash_);
        replayRecGame_ = mp_->room().id;
        replayRecSeed_ = mp_->startSeed();
        replayResultSync_.store(false, std::memory_order_relaxed);
        tryOpenReplayRecording(/*first=*/true);
    }

    bool GameView::showInitialCampaignBriefing(const std::string& stem) {
        if (stem.empty()) return true;
        // Headless synchronization runs do not need a modal render; screenshot
        // runs still exercise the loaded-world briefing and its first-tick gate.
        const char* driver=SDL_GetCurrentVideoDriver();
        if (driver && std::strcmp(driver,"dummy")==0 && !ta::devEnv("TA_SHOT_BRIEFING")) return true;
#ifndef NDEBUG
        const bool verifyBriefing=ta::devEnv("TA_SHOT_BRIEFING")!=nullptr;
        const auto briefingHash=verifyBriefing ? world_.stateHash() : 0;
#endif
        // mpAutoStep can run mid-frame with a render target, scale, viewport or clip
        // bound. A modal must present to the actual window, then restore the caller's
        // render state.
        struct RenderState {
            SDL_Renderer* renderer;
            SDL_Texture* target;
            float scaleX,scaleY;
            SDL_Rect viewport,clip;
            SDL_bool clipped;
            explicit RenderState(SDL_Renderer* r):renderer(r),target(SDL_GetRenderTarget(r)),clipped(SDL_RenderIsClipEnabled(r)) {
                SDL_RenderGetScale(r,&scaleX,&scaleY);
                SDL_RenderGetViewport(r,&viewport);SDL_RenderGetClipRect(r,&clip);
                SDL_SetRenderTarget(r,nullptr);SDL_RenderSetScale(r,1,1);
                SDL_RenderSetViewport(r,nullptr);SDL_RenderSetClipRect(r,nullptr);
            }
            ~RenderState() {
                SDL_SetRenderTarget(renderer,target);SDL_RenderSetScale(renderer,scaleX,scaleY);
                SDL_RenderSetViewport(renderer,&viewport);SDL_RenderSetClipRect(renderer,clipped?&clip:nullptr);
            }
        } renderState(ren_);
        std::string title=stem;
        int chapterNumber=0;
        for (const auto& campaign:ta::loadCampaigns(vfs_))
            if (const auto chapter=ta::campaignChapter(campaign,stem)) {
                title=chapter->title;chapterNumber=chapter->index+1;break;
            }

        // No Loaded acknowledgement has been sent: the referee cannot advance
        // its first tick. Poll only the connection while the player reads, so a
        // long briefing cannot trip keepalive or silently consume mission time.
        auto loading=std::move(loadScreen_);
        int width=0,height=0;
        SDL_GetRendererOutputSize(ren_,&width,&height);
        prepare(width,height);   // apply the new map's initial camera before capture
        finishTerrain();
        captureFrame();beginFrame();cosmeticStep(0);
        const bool objectives=showObjectives_;
        showObjectives_=false;   // the modal itself owns the objective text
        draw(width,height);
        showObjectives_=objectives;
        std::unique_ptr<SDL_Texture,decltype(&gpuvram::destroy)> background(nullptr,&gpuvram::destroy);
        if (width>0 && height>0) {
            std::vector<uint8_t> pixels(size_t(width)*height*4);
            if (SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_ABGR8888,pixels.data(),width*4)==0) {
                background.reset(gpuvram::create(ren_,SDL_PIXELFORMAT_ABGR8888,SDL_TEXTUREACCESS_STATIC,width,height));
                if (background) SDL_UpdateTexture(background.get(),nullptr,pixels.data(),width*4);
            }
        }
        const bool dismissed=ta::BriefingScreen::run(ren_,vfs_,stem,title,settings_,nullptr,
            background.get(),[this]{return mp_->poll();},chapterNumber,alwaysOnNote());
        briefingPaused_=true;
        loadScreen_=std::move(loading);
#ifndef NDEBUG
        if (verifyBriefing) {
            if (world_.tickCount()!=0 || world_.stateHash()!=briefingHash)
                throw std::runtime_error("initial briefing changed authoritative mission state");
            std::fprintf(stderr,"PASS: loaded campaign briefing preserves tick-zero state\n");
        }
#endif
        return dismissed;
    }

    bool GameView::mpStep() {
        // A watcher has no result of its own to leave on: its game ends when the server
        // says so (Msg::GameOver -- the match is decided with no human seated, or every
        // player has gone) and the last tick it names has been played and shown.
        if (spectating_ && !watchOver_ && mp_->gameOver() && netTick_ >= mp_->gameOverTicks() &&
            front().gameTick >= mp_->gameOverTicks()) {
            watchOver_ = true;
            std::fprintf(stderr, "watch: the server ended the game after %u ticks (%s)\n", mp_->gameOverTicks(),
                         mp_->gameOverTeam() >= 0 ? ("team " + std::to_string(mp_->gameOverTeam() + 1) + " wins").c_str()
                                                  : "no winner");
        }
        if ((outcome_ != 0 && !spectating_ && !replayMode_ && missionStem_.empty()) || watchOver_) {
            const uint64_t now = SDL_GetTicks64();
            if (!resultStartedMs_) resultStartedMs_ = now;
            if (now - resultStartedMs_ >= 3000) menuRequested_ = true;
        } else if (outcome_ != 0 && !spectating_ && !replayMode_ && ta::sim::isTaMission(vfs_, missionStem_)) {
            // A TA campaign mission ends itself: the outcome lands at the end of the
            // condition manager's countdown, which sets the game-over flag, and the
            // front end goes straight on to the end-of-game processing (glamour, ENDMSN;
            // docs/retail-campaign.md 3.4 and 4.1). Kingdoms missions wait for Esc.
            menuRequested_ = true;
        }

        pumpReplayRecording();
        if (reconnectUntilMs_) {
            if (!reconnectStep()) return false;
            if (reconnectUntilMs_) return true;   // still away: nothing to send or simulate
        } else if (!mp_->poll()) {
            if (beginReconnect()) return true;
            netError_ = lanHostGone() ? "the host closed the game" : mp_->error().empty() ? "disconnected" : mp_->error();
            return false;
        }
        if (mp_->desynced()) { netError_ = mp_->desyncReason(); return false; }
#ifndef NDEBUG
        // TA_DROP_LINK_AT_TICK=N: lose the connection at tick N (no Bye, no Leave), once,
        // so a harness can watch the reconnect inside the disconnect wait.
        if (const char* e = ta::devEnv("TA_DROP_LINK_AT_TICK"); e && !linkDropped_ && netTick_ >= uint32_t(std::atoi(e))) {
            linkDropped_ = true;
            std::fprintf(stderr, "drop-link: closing the connection at tick %u\n", netTick_);
            mp_->dropLink();
            return true;
        }
        // Regression probe: an eliminated, non-acking client must not stall survivors.
        if (outcome_ < 0 && ta::devFlag("TA_DEFEAT_STALL")) return true;
        // TA_STALL_AT=<tick>:<ms>: freeze this client's main thread once for <ms> when the played
        // tick reaches <tick> -- an alt-tab, a menu or a hitch -- so a harness can check that the
        // client's own stall is not read back as network jitter (tools/speed_stall_check.sh).
        if (const char* e = ta::devEnv("TA_STALL_AT"); e && !stallDone_ && netTick_ >= uint32_t(std::atoi(e))) {
            stallDone_ = true;
            const char* c = std::strchr(e, ':');
            const int ms = c ? std::max(0, std::atoi(c + 1)) : 0;
            std::fprintf(stderr, "SPEEDLOG stall %llu tick %u ms %d\n", (unsigned long long)SDL_GetTicks64(), netTick_, ms);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        speedProbe();
#endif
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
            cmdCredit_ = ta::net::cmdSendCredit(cmdCredit_, netTick_ - lastSendTick_);
            lastSendTick_ = netTick_;
        }
        // Rate AND window: the credit above says how fast, cmdInFlight_ says how
        // much may be outstanding. Without the window the rate limiter alone let a
        // stalled uplink accumulate 1024 unacknowledged commands against a 512
        // queue, and the server dropped the difference when they all landed.
        //
        // The outbox is ONE FIFO of commands and explicit orders (Cmd::Order, protocol
        // 267). A run of commands goes out as one PlayerCommands message under the
        // rate and window above; an order goes out whole as its transaction
        // (MpClient::sendOrder) under its own window (orderSendWindow), never behind
        // the command credit. Whatever cannot go yet waits, and so does everything
        // behind it: a Stop issued after a big Move never overtakes it.
        // Input the server dropped never comes back in a bundle: the server says so
        // (Msg::InputDropped) and that releases it from the windows, so a drop can
        // never hold the outbox shut.
        for (const auto& d : mp_->takeInputDropped()) {
            cmdInFlight_ -= std::min(cmdInFlight_, int(std::min<uint32_t>(d.commands, uint32_t(INT32_MAX))));
            if (d.serial) orderWindow_.retire(d.serial);
            if (ta::devEnv("TA_NETLOG"))
                std::fprintf(stderr, "server dropped %u commands, order %u\n", d.commands, d.serial);
        }
        if (outboxHead_ < outbox_.size() && !cmdCatchUp_) {
            int sendable = ta::net::cmdSendWindow(cmdCredit_, cmdInFlight_);
            size_t at = outboxHead_;
            while (at < outbox_.size()) {
                size_t end = at;
                while (end < outbox_.size() && outbox_[end].kind != ta::net::Cmd::Order &&
                       int(end - at) < sendable) ++end;
                if (end > at) {
                    const int n = int(end - at);
                    cmdCredit_ -= n;
                    cmdInFlight_ += n;
                    sendable -= n;
                    mp_->sendCommands({outbox_.begin() + ptrdiff_t(at), outbox_.begin() + ptrdiff_t(end)});
                    at = end;
                    continue;
                }
                auto& item = outbox_[at];
                if (item.kind != ta::net::Cmd::Order || !item.order) break;   // out of command credit
                const size_t bytes = ta::net::orderRecordBytes(*item.order);
                if (!orderWindow_.fits(bytes)) break;
                // The serial is the one current when the order LEAVES, not when it was clicked: an
                // order clicked during a rejoin's catch-up waits here, and nextOrderSerial_ only
                // passes the orders that activated before the drop as that history is replayed.
                // flushClick made this order (non-const) and nothing else holds it until it is
                // sent, so the serial is set in place.
                std::const_pointer_cast<ta::net::SelectionOrder>(item.order)->serial = nextOrderSerial_++;
                mp_->sendOrder(*item.order);
                orderWindow_.sent(item.order->serial, bytes);
                ++at;
            }
            outboxHead_ = at;
            if (outboxHead_ == outbox_.size()) {  // fully drained
                outbox_.clear();
                outboxHead_ = 0;
            } else if (outboxHead_ > outbox_.size() / 2 && outboxHead_ > 64) {
                // Amortised compaction: each command moves at most once.
                outbox_.erase(outbox_.begin(), outbox_.begin() + ptrdiff_t(outboxHead_));
                outboxHead_ = 0;
            }
        }
        // Decide once whether to run the sim on its own worker thread. On for interactive
        // net games (the whole point -- keeps world_.tick off the render thread); off for the
        // headless harness/replay (inline, byte-identical + deterministic) unless
        // TA_SIM_THREAD forces it on to VERIFY the threaded sim against the referee.
        if (!simThreadDecided_) {
            simThreadDecided_ = true;
            wantSimThread_ = simThreadMode_;
#ifndef NDEBUG
            if (const char* e = ta::devEnv("TA_SIM_SLOW_US")) simSlowUs_ = std::max(0, std::atoi(e));
#endif
            if (wantSimThread_) startSimThread();
        }
        // Simulate every delivered tick, but cap per frame so a big catch-up
        // (rejoin replay) stays responsive rather than freezing for seconds.
        ta::net::Bundle bd;
        int drained = 0;
#ifndef NDEBUG
        int paceBuffered = int(mp_->bufferedBundles());
        float paceRate = 0;
        bool paceFF = false;
#endif
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
            if (int ms = mp_->room().mySlot; ms >= 0)
                for (const auto& c : bd.cmds)   // serials continue above every own order that activated
                    if (int(c.player) == ms && c.kind == ta::net::Cmd::Order && c.order)
                        nextOrderSerial_ = std::max(nextOrderSerial_, c.order->serial + 1);
            if (!cmdCatchUp_)
                if (int ms = mp_->room().mySlot; ms >= 0) {
                    for (const auto& c : bd.cmds) {
                        if (int(c.player) != ms) continue;
                        if (c.kind == ta::net::Cmd::Order) {
                            if (c.order) orderWindow_.retire(c.order->serial);
                        } else if (cmdInFlight_ > 0) --cmdInFlight_;
                    }
#ifndef NDEBUG
                    // TA_AUTOPLAY_FORMATION delivery probe: own commands applied this tick (stderr only).
                    if (formPending_ > 0) {
                        int own = 0;   // unit commands: an explicit order counts each member
                        for (const auto& c : bd.cmds)
                            if (int(c.player) == ms)
                                own += c.kind == ta::net::Cmd::Order && c.order ? int(c.order->members.size()) : 1;
                        if (own && !formLanded_) formFirstTick_ = netTick_;
                        formLanded_ += own;
                        if (formLanded_ >= formPending_) {
                            std::fprintf(stderr, "FORMATION_DELIVERY issue=%u n=%d landed=%d first=%u last=%u\n",
                                         formIssueTick_, formPending_, formLanded_, formFirstTick_, netTick_);
                            formPending_ = 0;
                        }
                    }
#endif
                }
            for (const auto& e : bd.events) noteEvent(e);   // display only (the sim applies it below)
            if (useSimThread_) {
                // Hand this tick's bundle to the sim worker (FIFO == lockstep tick order).
                // world_ is simulated there; the state hash comes back via simOutbox_ and is
                // sent to the server below. The main thread stays free for the render.
                SimJob job;
                job.bundle = bd;
                job.tick = netTick_;
                job.wantHash = (netTick_ % uint32_t(ta::net::kHashPeriod) == 0);
                job.spectator = mp_->isSpectator();
                job.wantPos = job.wantHash && mp_->recording() && !job.spectator;
                { std::lock_guard<std::mutex> lk(inboxMutex_); simInbox_.push_back(std::move(job)); }
                inboxCv_.notify_one();
            } else {
#ifndef NDEBUG
                PaceSpan jobSpan(paceJobWallNs_, &paceJobCpuNs_);
#endif
                for (const auto& c : bd.cmds) apply(c);
                for (const auto& e : bd.events) applyEvent(e);
                // Only the SIM half per bundle (speedMult() is 1 in net games): the
                // cosmetic half runs once after the drain, so a rejoin/spectate
                // catch-up replays lockstep state without replaying 512 ticks of
                // sounds, effects and VM dispatch per frame.
                simStep(1.0f / 30.0f);
#ifndef NDEBUG
                if (simSlowUs_ > 0) std::this_thread::sleep_for(std::chrono::microseconds(simSlowUs_));
#endif
                // Report our processed tick every kHashPeriod. A seated player sends its
                // state hash (desync check + flow-control ack); a spectator sends one too
                // -- purely as a progress ACK so the server can pace an all-AI watch game
                // to what the spectator can sustain (its hash is never desync-checked, since
                // an all-AI room has no seated players to form a consensus). A spectator's
                // hash is a progress ACK only, so skip the O(units) stateHash for it.
                if (netTick_ % uint32_t(ta::net::kHashPeriod) == 0)
                    mp_->sendHash(netTick_, reportedHash(mp_->isSpectator(), netTick_),
                                  mp_->recording() && !mp_->isSpectator() ? world_.posDigest() : 0);
            }
            ++netTick_;
            ++drained;
            // During a heavy catch-up (a spectator fast-forwarding a big backlog at high
            // game speed), service the connection every so often -- answer the server's
            // keepalive pings and keep draining our recv buffer. Otherwise a spectator
            // busy simulating for >15s stops ponging and the server drops it ("peer
            // closed"). poll() is cheap (recv + ping/pong + flush); we ignore its result
            // here, the next top-of-mpStep poll() surfaces any real error.
            //
            // An INLINE sim (the headless harness) also polls on the wall clock. There every
            // tick is simulated right here, and a stress game under CPU load runs 200-500 ms
            // per tick, so 64 ticks between polls left the socket unread and our StateHash
            // reports unsent for 15-30 s: the server dropped the seat for silence. Every
            // kInlinePollMs keeps the server hearing from us while we simulate. Pure pacing:
            // bundles are still taken in tick order, so the sim and its hashes are unchanged.
            constexpr uint64_t kInlinePollMs = 250;
            if (useSimThread_) {
                if ((drained & 63) == 0) mp_->poll();
            } else if ((drained & 63) == 0 || SDL_GetTicks64() - netPollMs_ >= kInlinePollMs) {
                mp_->poll();
                netPollMs_ = SDL_GetTicks64();
            }
        };
        // Another bundle may be played: below the --time stop tick and, for the harness, not
        // past the tick the result landed (setNetStopAtOutcome).
        const auto mayPlay = [&] {
            return netTick_ < netStopTick_ && !(netStopAtOutcome_ && outcome_.load() != 0);
        };
        if (netDelay_ == -2) {   // one-time init from the env
            // The adaptive jitter buffer is ON by default: it only ever reduces
            // stalls and is pure pacing (byte-identical sim). TA_NET_DELAY overrides
            // -- "0" disables it (drain every bundle immediately), a positive integer
            // pins a fixed reserve depth, "auto" (or unset) self-sizes to the link.
            const char* e = ta::devEnv("TA_NET_DELAY");
            if (!e || std::string(e) == "auto") { netAuto_ = true; netDelay_ = 3; mp_->enableRttProbe(); }
            else netDelay_ = std::max(0, std::atoi(e));
        }
        // WE S3: a seated player's sim worker is fed only while its inbox holds kSimInboxCap ticks
        // or fewer at the start of the frame. The rest wait in mp_, where the jitter servo, the
        // 900-tick guard and the server's flow control (Msg::Progress below) all see them, so a
        // client that cannot sustain the speed slows the game to what it can play instead of
        // queueing 120-150 ticks of command latency in the inbox. Rejoin catch-up and spectators
        // keep the 512-tick fast-forward: they replay history the server is not waiting on.
        constexpr size_t kSimInboxCap = 2;
        if (netAuto_) {
            // Size the buffer to cover the measured bundle-arrival jitter, with an RTT-scaled
            // floor and a frame term, never past what the server's flow control lets the buffer
            // reach (ta::net::jitterTargetTicks, net/pacing.h). Recomputed each frame so it
            // tracks the link and the game speed.
            const uint64_t nowF = SDL_GetTicks64();
            if (netFrameAt_) {
                const float f = std::min(250.0f, float(nowF - netFrameAt_));
                netFrameMs_ += 0.1f * (f - netFrameMs_);
            }
            netFrameAt_ = nowF;
            netDelay_ = ta::net::jitterTargetTicks(mp_->arrivalJitterMs(), mp_->rttMs(), netFrameMs_,
                                                    std::max(1, int(mp_->gameSpeed())),
                                                    useSimThread_ ? int(kSimInboxCap) : 0);
        }
        const bool capInbox = useSimThread_ && !cmdCatchUp_ && !mp_->isSpectator();
        bool inboxFull = false;
        if (capInbox) {
            std::lock_guard<std::mutex> lk(inboxMutex_);
            inboxFull = simInbox_.size() > kSimInboxCap;
        }
        if (netDelay_ <= 0) {
            // Default: drain to the newest delivered bundle every frame.
            while (!inboxFull && drained < 512 && mayPlay() && mp_->takeBundle(netTick_, bd)) simTick();
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
            const bool fastForward = buffered > netDelay_ + 60 && !capInbox;
            if (fastForward) budget = 512;
            if (inboxFull) budget = 0;
#ifndef NDEBUG
            paceBuffered = buffered; paceRate = rate; paceFF = fastForward;
#endif
            budget = std::min(budget, 512);
            while (drained < budget && mayPlay() && mp_->takeBundle(netTick_, bd)) simTick();
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
            if (ta::devEnv("TA_NETLOG"))
                std::fprintf(stderr, "catch-up complete at tick %u\n", netTick_);
            cmdInFlight_ = 0;
            cmdCredit_ = ta::net::kCmdCapPerTick;
            orderWindow_.clear();
        }
        // Send the worker's finished per-tick state hashes to the server (lockstep desync
        // check + flow-control ack). Drained here on the main thread -- mp_ has a single owner.
        if (useSimThread_) {
            std::deque<HashJob> done;
            { std::lock_guard<std::mutex> lk(outboxMutex_); done.swap(simOutbox_); }
            for (const auto& h : done) mp_->sendHash(h.tick, h.hash, h.pos);
            drainPendingNotice();   // apply any HUD notice the worker posted (god/mission/scenario)
        }
        // WE S3 flow control: report the tick actually PROCESSED (the worker's, never the push
        // position) every kProgressPeriod ticks, so the server runs flowLeadTicks() past it rather
        // than 120 ticks past a hash sent every 30. Seated players and spectators alike (a spectator
        // paces an all-AI room).
        if (netTick_ > 0) {
            const uint32_t pushed = netTick_ - 1;
            const uint32_t done = useSimThread_ ? std::min(simProcessedTick_.load(std::memory_order_relaxed), pushed)
                                                : pushed;
            if (!progressSent_ || done < progressSentTick_ ||
                done - progressSentTick_ >= uint32_t(ta::net::kProgressPeriod)) {
                mp_->sendProgress(done);
                progressSent_ = true;
                progressSentTick_ = done;
            }
        }
#ifndef NDEBUG
        if (!paceInit_) { paceInit_ = true; pace_.open(ta::devEnv("TA_PACELOG")); }
        if (pace_.on()) {
            // What the player saw this frame: the worker's finished ticks with the sim thread on, the
            // drained count inline. The wall clock is read here, so a long sim stall shows in wallMs.
            const uint64_t nowMs = SDL_GetTicks64();
            ta::PaceLog::Frame fr;
            fr.wallMs = paceLastMs_ ? double(nowMs - paceLastMs_) : 0.0;
            paceLastMs_ = nowMs;
            if (useSimThread_) {
                const uint32_t done = simProcessedTick_.load(std::memory_order_relaxed);
                fr.ticks = int(done - paceLastTick_);
                paceLastTick_ = done;
                std::lock_guard<std::mutex> lk(inboxMutex_);
                fr.inbox = int(simInbox_.size());
                fr.tick = done;
            } else {
                fr.ticks = drained;
                fr.tick = netTick_;
            }
            fr.buffered = paceBuffered; fr.rate = paceRate; fr.fastForward = paceFF;
            fr.backlog = int(mp_->bufferedBundles());
            if (useSimThread_ && netTick_ > 0)
                fr.backlog += int(std::max<long long>(0, (long long)netTick_ - 1 -
                                                         (long long)simProcessedTick_.load(std::memory_order_relaxed)));
            // What the frame shows: the pinned snapshot, interpolated the way the render does.
            fr.shown = front().gameTick;
            if (present_.on) {
                // The presentation clock's own record of this frame (presentFrame).
                fr.td = present_.td; fr.dtd = present_.dtd; fr.nominal = present_.nominal;
                fr.freeze = present_.freeze; fr.gate = present_.gate; fr.snap = present_.snap;
                fr.lagTarget = float(present_.lagTarget);
            } else {
                const double rawAlpha = front().tickDurMs > 0.0f
                    ? double(nowMs - front().tickMs) / double(front().tickDurMs) : 1.0;
                fr.td = double(fr.shown) - 1.0 + std::clamp(rawAlpha, 0.0, 1.0);
                fr.freeze = rawAlpha >= 1.0;   // the newest pose is held: nothing newer to interpolate toward
                // Display time advanced this frame against the frame's nominal step (its wall time at
                // the game speed), timed on the performance counter: millisecond ticks would quantise a
                // 4.17 ms frame by 24%.
                const uint64_t nowC = SDL_GetPerformanceCounter();
                if (paceLastCounter_ && paceLastTd_ >= 0) {
                    const double dtS = double(nowC - paceLastCounter_) / double(SDL_GetPerformanceFrequency());
                    fr.nominal = paused_ ? 0.0 : dtS * 30.0 * double(animSpeed());
                    fr.dtd = fr.td - paceLastTd_;
                }
                paceLastCounter_ = nowC;
                paceLastTd_ = fr.td;
            }
            fr.lockWaitUs = double(paceLockWaitNs_) / 1000.0;
            paceLockWaitNs_ = 0;
            const uint64_t now[5] = {paceWorldNs_.load(), paceCaptureNs_.load(), paceHashNs_.load(),
                                     paceJobWallNs_.load(), paceJobCpuNs_.load()};
            double* out[5] = {&fr.worldMs, &fr.captureMs, &fr.hashMs, &fr.jobWallMs, &fr.jobCpuMs};
            for (size_t i = 0; i < 5; ++i) {
                *out[i] = double(now[i] - paceSeenNs_[i]) / 1e6;
                paceSeenNs_[i] = now[i];
            }
            pace_.frame(fr);
        }
#endif
        // Cosmetics once per frame, covering the game time actually DISPLAYED: the presentation
        // clock's advance (particles, timers and the tick-stamped event queues then follow what is
        // on screen), or without it the ticks played.
        if (present_.on) { if (present_.dtd > 0) cosmeticStep(float(present_.dtd) / 30.0f); }
        else if (drained > 0) cosmeticStep(float(drained) / 30.0f);
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
            // The window opens at the game's first tick: one that spanned the loading
            // read near zero and the average then climbed from it. Until a window has
            // closed the row shows the requested speed.
            if (actualSpeedT0_ == 0 || netTick_ == 0) {
                actualSpeedT0_ = netTick_ ? na : 0; actualSpeedTick0_ = netTick_;
                if (netTick_ && actualSpeed_ <= 0.0f) {
                    actualSpeedReq_ = std::max(1, int(mp_->gameSpeed())) / 10.0f;
                    actualSpeed_ = actualSpeedReq_;
                }
            }
            else if (na - actualSpeedT0_ >= 500) {
                float ips = float(int64_t(netTick_) - int64_t(actualSpeedTick0_)) * 1000.0f
                            / float(na - actualSpeedT0_);
                // CAPPED AT THE REQUESTED SPEED. The playout controller deliberately
                // runs up to 1.3x to drain a deep buffer (see the clamp on `rate`
                // above), and at the start of a game or benchmark the buffer fills
                // ahead of playout, so it pins to exactly that -- which is why a fresh
                // game read "1.3x" before it settled. The sim was not going faster than
                // asked; the buffer was catching up. Reporting the transient made the
                // readout look wrong at the one moment everybody watches it.
                //
                // So the ceiling is the requested speed, and the meaning is kept simple:
                // at the requested multiplier the sim is healthy, below it the client
                // (or the server, pacing to the slowest) cannot sustain the pace. That
                // is the only question this row answers, and undershoot -- the half that
                // matters -- is untouched.
                const float reqSp = std::max(1, int(mp_->gameSpeed())) / 10.0f;
                const float sample = std::min(ips / 30.0f, reqSp);
                // SMOOTHED, because the raw sample is mostly quantisation noise. A ~0.5s
                // window holds about 15 ticks at 1x, so one tick landing either side of
                // the boundary moves the reading ~7% -- the number changed constantly
                // while telling the reader nothing. An exponential average over roughly
                // the last two seconds still drops promptly when a client genuinely
                // cannot keep up, which is the only thing this readout is for.
                //
                // Seeded at the requested speed rather than ramping up from zero, so the
                // display starts where the game was asked to run and only drops if it
                // cannot keep up.
                // A CHANGE OF REQUESTED SPEED IS A DISCONTINUITY, NOT NOISE. Smoothing
                // across it is wrong in both directions, and badly so downwards: after
                // a host drops 4x to 1x the stored average is still ~4, and capping only
                // the incoming sample leaves the row reading 3.2x for several seconds
                // while playback is already sustaining exactly 1x. Restart the average
                // at the new speed instead. (Found by review.)
                if (reqSp != actualSpeedReq_) { actualSpeed_ = reqSp; actualSpeedReq_ = reqSp; }
                actualSpeed_ = actualSpeed_ + (sample - actualSpeed_) * 0.25f;
                // Belt and braces: the average can only ever sit at or below the speed
                // that was asked for, whatever route it took to get here.
                actualSpeed_ = std::min(actualSpeed_, reqSp);
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

#ifndef NDEBUG
// TA_SPEED_AT=<tick>:<speed>[,<tick>:<speed>...] -- the host changes the game speed (tenths)
// in-game when the played tick reaches <tick>, exactly as the +/- keys do. Each change, and one
// line per wall second, is printed as SPEEDLOG (wall ms, played tick, requested speed) so a
// harness can measure the real playout rate across a change. Debug-only, pacing only.
void GameView::speedProbe() {
    static const char* spec = ta::devEnv("TA_SPEED_AT");
    if (!spec || !mp_ || mp_->state() != ta::net::MpClient::State::InGame) return;
    if (speedAt_.empty() && !speedAtParsed_) {
        speedAtParsed_ = true;
        std::string s = spec;
        for (size_t p = 0; p < s.size();) {
            size_t e = s.find(',', p); if (e == std::string::npos) e = s.size();
            const std::string item = s.substr(p, e - p);
            const size_t c = item.find(':');
            if (c != std::string::npos)
                speedAt_.push_back({uint32_t(std::atoi(item.c_str())), std::atoi(item.c_str() + c + 1)});
            p = e + 1;
        }
    }
    const uint64_t now = SDL_GetTicks64();
    if (!speedLogMs_) speedLogMs_ = now;
    if (speedAtNext_ < speedAt_.size() && netTick_ >= speedAt_[speedAtNext_].first) {
        auto o = mp_->room().opts;
        o.speed = uint8_t(std::clamp(speedAt_[speedAtNext_].second, 1, ta::net::kMaxDevGameSpeed));
        mp_->setGameOptions(o);
        std::fprintf(stderr, "SPEEDLOG set %llu tick %u speed %d\n", (unsigned long long)now, netTick_, int(o.speed));
        ++speedAtNext_;
    }
    if (now - speedLogMs_ >= 1000) {
        speedLogMs_ += 1000;
        std::fprintf(stderr, "SPEEDLOG wall %llu tick %u speed %d buffered %d delay %d\n", (unsigned long long)now,
                     netTick_, int(mp_->gameSpeed()), int(mp_->bufferedBundles()), netDelay_);
    }
}

// TA_BUILDLINE_NET: the local player's monarch lays three build lines of its smallest
// building through placeBuildLine (the code a shift-drag release runs) -- horizontal and
// diagonal at the tight default, vertical at gap 2 -- so the Build/Move orders travel the
// real path through the server and come back in tick bundles. Every site must then stand
// finished: BUILDLINE_NET prints sites/built once all are, or at the deadline.
void GameView::buildlineNetProbe() {
    if (!ta::devFlag("TA_BUILDLINE_NET") || netLinesDone_ || spectating_ || localPlayer_ < 0) return;
    if (netLines_.empty()) {
        if (netTick_ < 90) return;
        int monarch = -1;
        const ta::sim::UnitType* small = nullptr;
        std::vector<std::vector<std::pair<float, float>>> plans;
        float mx = 0, mz = 0;
        {
            const auto live = liveWorld();
            for (const auto& u : live->units())
                if (u.alive() && int(u.player) == localPlayer_ && u.type && u.type->commander && u.type->isBuilder) {
                    monarch = u.id; mx = u.x.toFloat(); mz = u.z.toFloat(); break;
                }
            if (monarch < 0) { netLinesDone_ = true; std::fprintf(stderr, "BUILDLINE_NET no monarch\n"); return; }
            for (const auto& id : registry_.buildable(live->unit(monarch)->type->id)) {
                const auto* t = registry_.find(id);
                if (t && t->isStructure() && !t->onMana && t->yardMap.find_first_of("wWCY") == std::string::npos &&
                    (!small || t->footX * t->footZ < small->footX * small->footZ)) small = t;
            }
            if (!small) { netLinesDone_ = true; std::fprintf(stderr, "BUILDLINE_NET no building\n"); return; }
            const float drags[3][3] = {{128, 0, 0}, {112, 112, 0}, {0, 160, 2}};
            for (const auto& d : drags) {
                bool found = false;
                for (int r = 4; r < 40 && !found; ++r)
                    for (int i = -r; i <= r && !found; ++i)
                        for (int j = -r; j <= r && !found; ++j) {
                            if (std::max(std::abs(i), std::abs(j)) != r) continue;
                            const float x0 = mx + float(i) * 32, z0 = mz + float(j) * 32;
                            bool open = true;
                            for (float z = z0 - 96; open && z <= z0 + d[1] + 96; z += 32)
                                for (float x = x0 - 96; open && x <= x0 + d[0] + 96; x += 32)
                                    if (!live->canPlace(small, x, z, localPlayer_)) open = false;
                            for (const auto& l : plans) for (const auto& [px, pz] : l)
                                if (std::abs(px - x0) < 300 && std::abs(pz - z0) < 300) open = false;
                            if (open) { plans.push_back({{x0, z0}, {x0 + d[0], z0 + d[1]}}); found = true; }
                        }
            }
        }
        if (plans.size() != 3) { netLinesDone_ = true; std::fprintf(stderr, "BUILDLINE_NET no open ground\n"); return; }
        const float gaps[3] = {0, 0, 2};
        for (size_t k = 0; k < plans.size(); ++k) {
            ClickScope click(*this);
            selection_ = {monarch}; placing_ = small; buildLineGap_ = int(gaps[k]);
            const auto [x0, z0] = plans[k][0];
            const auto [x1, z1] = plans[k][1];
            netLines_.push_back({small, buildLinePositions(x0, z0, x1, z1)});
            placeBuildLine(x0, z0, x1, z1);
        }
        placing_ = nullptr; buildLineGap_ = 0;
        netLinesTick_ = netTick_;
        size_t n = 0; for (const auto& l : netLines_) n += l.sites.size();
        std::fprintf(stderr, "BUILDLINE_NET queued %zu sites of %s (%dx%d) at tick %u\n", n, small->id.c_str(),
                     small->footX, small->footZ, netTick_);
        return;
    }
    if ((netTick_ - netLinesTick_) % 150 != 0) return;
    size_t sites = 0, built = 0;
    {
        const auto live = liveWorld();
        for (const auto& l : netLines_)
            for (const auto& [x, z] : l.sites) {
                ++sites;
                const int ox = ta::sim::footprintOrigin(x, l.type->footX), oz = ta::sim::footprintOrigin(z, l.type->footZ);
                for (const auto& u : live->units())
                    if (live->finishedHere(u) && u.type == l.type && int(u.player) == localPlayer_ &&
                        ta::sim::footprintOrigin(u.x, l.type->footX) == ox && ta::sim::footprintOrigin(u.z, l.type->footZ) == oz) {
                        ++built; break;
                    }
            }
    }
    if (built == sites || netTick_ - netLinesTick_ >= 30u * 600u) {
        std::fprintf(stderr, "BUILDLINE_NET sites=%zu built=%zu after %u ticks\n", sites, built, netTick_ - netLinesTick_);
        netLinesDone_ = true;
    }
}

// See gameview.h. Issues ordinary Move / AttackMove orders through GameView::issue,
// so they travel the real path -- outbox -> sendCommands -> server -> bundle -> every
// peer -- rather than being applied locally, which would desync by construction.
void GameView::autoplayStep() {
    ClickScope click(*this);   // one click: a Legion net game sends it as explicit orders
    static const int rate = [] {
        const char* e = ta::devEnv("TA_AUTOPLAY");
        return e ? std::clamp(std::atoi(e), 0, 60) : 0;
    }();
    // TA_AUTOPLAY_FORMATION: the human seat plays like a player who forms armies -- every 600 ticks it puts
    // all its mobile units in formation 1 (Alt+1) and sends them to fight at the enemy's base, one command per
    // unit through GameView::issue, so the client's rate credit spreads a big selection over the same ticks a
    // real click does. Without it the --mpai human never forms a group and the AI never does, so no Legion group
    // code runs at all in that harness.
    static const bool formation = ta::devFlag("TA_AUTOPLAY_FORMATION");
    if ((!rate && !formation) || spectating_ || replayMode_ || localPlayer_ < 0) return;
    if (netTick_ < autoplayNext_) return;
    if (formation) {
        autoplayNext_ = netTick_ + 600u;
        std::vector<int> mine;
        float tx = -1, tz = -1;
        {
            const auto live = liveWorld();
            for (const auto& u : live->units()) {
                if (!u.alive() || !u.type) continue;
                if (int(u.player) == localPlayer_) {
                    // The monarch stays home: sending it into the enemy base ends the game, and with it the run.
                    if (u.type->maxVel > ta::sim::Fixed() && !u.type->commander && !u.embarked() && !u.underConstruction)
                        mine.push_back(u.id);
                } else if (!live->allied(localPlayer_, int(u.player)) && (tx < 0 || (u.type->commander && !u.type->canFly))) {
                    tx = u.x.toFloat(); tz = u.z.toFloat();   // the first enemy seen, or better, an enemy monarch
                }
            }
        }
        if (mine.empty() || tx < 0) return;
        for (int id : mine) {
            ta::net::Command c;
            c.kind = ta::net::Cmd::SetSquad;
            c.unitId = id;
            c.targetId = -1;     // formation 1
            issue(c);
        }
        for (int id : mine) {
            ta::net::Command c;
            c.kind = ta::net::Cmd::AttackMove;
            c.unitId = id;
            c.x = tx; c.z = tz;  // one shared point: the Legion group's click
            c.queue = 0;
            issue(c);
        }
        // Delivery probe: the issue tick and the commands to watch for (an unfinished earlier order's
        // count is reported as it stood).
        if (formPending_ > 0)
            std::fprintf(stderr, "FORMATION_DELIVERY issue=%u n=%d landed=%d first=%u last=-1 (superseded)\n",
                         formIssueTick_, formPending_, formLanded_, formFirstTick_);
        formIssueTick_ = netTick_;
        formPending_ = int(mine.size()) * 2;
        formLanded_ = 0;
        formFirstTick_ = 0;
        return;
    }

    // Seed once, from the slot and the game seed: same game + same slot -> same order
    // stream, so a run reproduces. Different slots diverge, which is the point --
    // eight players pulling in different directions is what interleaves commands.
    if (autoplayRng_ == 0)
        autoplayRng_ = uint32_t(localPlayer_ + 1) * 2654435761u ^
                       ((mp_ ? mp_->startSeed() : 0u) * 2246822519u) ^ 0x9e3779b9u;
    auto rnd = [&] {
        autoplayRng_ = autoplayRng_ * 1664525u + 1013904223u;
        return autoplayRng_ >> 8;
    };

    // Schedule the NEXT order off the tick clock, never the wall clock.
    autoplayNext_ = netTick_ + std::max(1u, 300u / uint32_t(rate));

    // Collect this player's mobile units. Stable order (units_ is append-only and
    // identical on every peer), so indexing into it is reproducible.
    //
    // UNDER simMutex_ (liveWorld): with TA_SIM_THREAD the worker mutates world_ while this
    // runs on the main thread, and a spawn that reallocates units_ invalidates the iteration
    // outright -- not a torn read, a crash.
    std::vector<int> mine;
    {
        const auto live = liveWorld();
        for (const auto& u : live->units())
            if (u.alive() && u.type && int(u.player) == localPlayer_ && u.type->maxVel > ta::sim::Fixed() &&
                !u.embarked() && !u.underConstruction &&
                // A builder at work is left to it (autoplayHelpStep's jobs would never finish),
                // and so is a flyer on guard: VTOL_Follow's help, repair and orbit are what
                // its guard is for, and a random move would end it.
                !(u.type->isBuilder && (u.buildSiteId || u.repairId || u.productionSiteId ||
                                        (!u.orders.empty() && u.orders.front().buildType))) &&
                !(u.type->canFly && !u.orders.empty() && u.orders.back().guard))
                mine.push_back(u.id);
    }
    if (mine.empty()) { autoplayHelpStep(rnd); return; }

    // Order a handful at a time: one unit per order is too thin to collide with
    // another client's commands in the same tick, which is the thing being tested.
    const int batch = 1 + int(rnd() % 4);
    const float w = float(mapView_.map().width) * 16.0f;
    const float h = float(mapView_.map().height) * 16.0f;
    for (int i = 0; i < batch; ++i) {
        ta::net::Command c;
        c.unitId = mine[rnd() % mine.size()];
        c.x = float(rnd() % uint32_t(w > 32 ? w - 32 : 32)) + 16.0f;
        c.z = float(rnd() % uint32_t(h > 32 ? h - 32 : 32)) + 16.0f;
        // Mostly plain moves, some attack-moves: attack-move drives acquisition and
        // combat, which is where the sim has the most state to disagree about.
        c.kind = (rnd() % 4 == 0) ? ta::net::Cmd::AttackMove : ta::net::Cmd::Move;
        c.queue = 0;
        issue(c);
    }
    autoplayHelpStep(rnd);
}

// TA_AUTOPLAY's production and HelpBuild orders: now and then
// the seat has a builder put down a factory (or anything on its menu) near
// itself, trains at its factories, puts a conjurer on a repeat queue, has a
// builder guard a busy unit of its own (guard-help: repair, HelpBuild on a
// site or on a factory's / conjurer's output), has a FLYING builder guard one
// or a damaged unit (VTOL_Follow's help and repair: its approach, orbit and
// cadence; its idle orbit when there is nothing to do) and has a builder
// assist an unfinished unit -- so autoplay desync sweeps run construction, shared
// production progress and guard-help between clients and the referee. (A
// seat with flying builders from the start: TA_START_FLYERS on every peer,
// TA_AUTOPLAY_SIDE ara|tar|zon for the seat.) All
// through issue(); the sim judges every order, so an ineligible one (a
// blocked spot, an off-menu type) is refused on every peer alike.
void GameView::autoplayHelpStep(const std::function<uint32_t()>& rnd) {
    struct Mine { int id; const ta::sim::UnitType* type; float x, z; };
    std::vector<Mine> factories, builders, flyers, orbiting;
    std::vector<int> busy, unfinished, damaged;
    int flyGuards = 0, flyHelping = 0, flyRepairing = 0;
    {
        const auto live = liveWorld();
        for (const auto& u : live->units()) {
            if (!u.alive() || !u.type || int(u.player) != localPlayer_ || u.embarked()) continue;
            if (u.underConstruction) { unfinished.push_back(u.id); continue; }
            if (u.hp < ta::sim::Fixed::fromFloat(u.type->maxHp)) damaged.push_back(u.id);
            if (u.type->canFly && !u.orders.empty() && u.orders.back().guard) {
                ++flyGuards;
                flyHelping += u.buildSiteId != 0;
                flyRepairing += u.repairId != 0;
            }
            if (!u.type->isBuilder) continue;
            const Mine m{u.id, u.type, u.x.toFloat(), u.z.toFloat()};
            const bool working = u.buildSiteId || u.repairId || u.productionSiteId ||
                                 (!u.orders.empty() && (u.orders.front().buildType || u.orders.front().guard));
            // Only an idle builder takes a new job: the jobs get to finish.
            if (u.type->isStructure()) factories.push_back(m);
            else if (!working) {
                builders.push_back(m);
                if (u.type->canFly) flyers.push_back(m);
            } else if (u.type->canFly && !u.buildSiteId && !u.repairId && !u.productionSiteId &&
                       !u.orders.empty() && u.orders.front().guard) {
                orbiting.push_back(m);   // a flying guard with nothing to do: free to guard elsewhere
            }
            if (u.buildSiteId || u.repairId || u.productionSiteId) busy.push_back(u.id);
        }
    }
    const auto menu = [&](const Mine& m, bool structures) {
        std::vector<const ta::sim::UnitType*> out;
        for (const auto& name : registry_.buildable(m.type->id))
            if (const auto* t = registry_.find(name); t && t->isStructure() == structures) out.push_back(t);
        return out;
    };
    // Coverage probe (stderr only): the seat's flying guards and what they do.
    if (flyGuards && netTick_ >= autoplayFlyReport_) {
        autoplayFlyReport_ = netTick_ + 300u;
        std::fprintf(stderr, "AUTOPLAY_FLYGUARD tick=%u guards=%d helping=%d repairing=%d\n", netTick_,
                     flyGuards, flyHelping, flyRepairing);
    }
    ta::net::Command c;
    c.queue = 0;
    switch (rnd() % 8) {
    case 0: {   // a builder puts a structure down near itself
        if (builders.empty()) return;
        const Mine& b = builders[rnd() % builders.size()];
        auto types = menu(b, true);
        // Mostly something that builds in turn: a factory gives the guards work.
        std::vector<const ta::sim::UnitType*> producers;
        for (const auto* t : types) if (t->isBuilder) producers.push_back(t);
        if (!producers.empty() && rnd() % 4) types = producers;
        if (types.empty()) return;
        c.kind = ta::net::Cmd::Build; c.unitId = b.id;
        c.x = b.x + float(int(rnd() % 321) - 160); c.z = b.z + 96.0f + float(rnd() % 160);
        std::snprintf(c.type, sizeof c.type, "%s", types[rnd() % types.size()]->id.c_str());
        break;
    }
    case 1: {   // a factory trains one mobile unit
        if (factories.empty()) return;
        const Mine& f = factories[rnd() % factories.size()];
        const auto types = menu(f, false);
        if (types.empty()) return;
        c.kind = ta::net::Cmd::Train; c.unitId = f.id; c.targetId = 1 + int(rnd() % 3);
        std::snprintf(c.type, sizeof c.type, "%s", types[rnd() % types.size()]->id.c_str());
        break;
    }
    case 2: {   // a conjurer on a repeat queue (or off it again: the same command toggles)
        if (builders.empty()) return;
        const Mine& b = builders[rnd() % builders.size()];
        const auto types = menu(b, false);
        if (types.empty() || b.type->commander) return;   // the monarch keeps building
        c.kind = ta::net::Cmd::RepeatTrain; c.unitId = b.id;
        std::snprintf(c.type, sizeof c.type, "%s", types[rnd() % types.size()]->id.c_str());
        break;
    }
    case 3:
    case 4:     // a builder guards a busy unit: it helps / repairs what that unit works on
        if (builders.empty() || busy.empty()) return;
        c.kind = ta::net::Cmd::Guard; c.unitId = builders[rnd() % builders.size()].id;
        c.targetId = busy[rnd() % busy.size()];
        if (c.unitId == c.targetId) return;
        break;
    case 6:     // a flying builder guards a busy unit (flyers are few: give them their own turn)
        if (flyers.empty() || busy.empty()) return;
        c.kind = ta::net::Cmd::Guard; c.unitId = flyers[rnd() % flyers.size()].id;
        c.targetId = busy[rnd() % busy.size()];
        if (c.unitId == c.targetId) return;
        break;
    case 7: {   // an idle (or idly orbiting) flying builder guards a damaged unit, else a busy one
        const std::vector<Mine>& pick = !flyers.empty() ? flyers : orbiting;
        if (pick.empty()) return;
        c.kind = ta::net::Cmd::Guard; c.unitId = pick[rnd() % pick.size()].id;
        const std::vector<int>& pool = !damaged.empty() ? damaged : busy;
        if (pool.empty()) return;
        c.targetId = pool[rnd() % pool.size()];
        if (c.unitId == c.targetId) return;
        break;
    }
    case 5:     // a builder assists an unfinished unit (a site, or a factory's output)
        if (builders.empty() || unfinished.empty()) return;
        c.kind = ta::net::Cmd::Assist; c.unitId = builders[rnd() % builders.size()].id;
        c.targetId = unfinished[rnd() % unfinished.size()];
        break;
    default:
        return;
    }
    // Coverage probe (stderr only): what the seat asked for, at which tick.
    std::fprintf(stderr, "AUTOPLAY_HELP tick=%u kind=%d unit=%d target=%d type=%s\n", netTick_, int(c.kind),
                 c.unitId, c.targetId, c.type);
    issue(c);
}
#endif

    // The disconnect wait, the client's side. The server holds a seat it
    // stopped hearing from for what is left of its kDropWaitMs budget; a player whose link dropped reconnects
    // inside that window and carries on from the first bundle it lacks, its world
    // intact. Only a seated player in a game still being played, and only with another
    // human seated -- alone in the room (single-player), the server ends the game when
    // the link goes, so there is nothing to come back to.
    bool GameView::beginReconnect() {
        if (!mp_ || !mp_->canReconnect() || spectating_ || replayMode_ || outcome_ != 0) return false;
        if (lanHostGone()) return false;   // a LOCAL SERVER went with its host: nothing to reconnect to
        const int mine = mp_->room().mySlot;
        bool others = false;
        for (int i = 0; i < ta::net::kMaxSlots; ++i)
            others |= i != mine && mp_->startSlots()[i].type == 1;
        if (!others) return false;
        const uint64_t now = SDL_GetTicks64();
        // What the server will really wait for us (the budget is per game), but long
        // enough for one attempt, so a spent budget still hears why from the server.
        reconnectUntilMs_ = now + std::max<uint64_t>(mp_->reconnectWindowMs(), 2000);
        reconnectNextMs_ = now;
        std::fprintf(stderr, "connection lost (%s) -- reconnecting\n", mp_->error().c_str());
        return true;
    }

    bool GameView::reconnectStep() {
        using S = ta::net::MpClient::State;
        const uint64_t now = SDL_GetTicks64();
        if (mp_->state() != S::Done) mp_->poll();
        uint32_t from = 0, end = 0;
        if (mp_->takeReseated(from, end)) {
            reconnectUntilMs_ = 0;
            // What was in flight when the link went never comes back: the server dropped
            // it with the old connection. The bundles from `from` to `end` are history
            // (the ticks played before the game stopped for us): hold our orders and
            // acknowledgements through them, as a spectator's catch-up does.
            cmdInFlight_ = 0;
            orderWindow_.clear();
            cmdReplayEnd_ = end;
            cmdCatchUp_ = netTick_ < end;
            netError_.clear();
            notice_ = "RECONNECTED"; noticeTimer_ = 3.0f;
            std::fprintf(stderr, "reconnected -- catching up from tick %u (log at %u)\n", from, end);
            return true;
        }
        if (mp_->state() == S::Done) {
            if (!mp_->canReconnect() || now >= reconnectUntilMs_) {
                reconnectUntilMs_ = 0;
                netError_ = lanHostGone() ? "the host closed the game" : mp_->error().empty() ? "disconnected" : mp_->error();
                return false;
            }
            if (now >= reconnectNextMs_) { reconnectNextMs_ = now + 2000; mp_->reconnect(); }
        }
        return true;
    }

    // Display side of a bundle's lifecycle events (main thread; the sim applies them).
    void GameView::noteEvent(const ta::net::Event& e) {
        if (e.kind != ta::net::Event::Kind::AiTakeover || e.player >= 8) return;
        playerAi_[e.player] = true;
        if (replayInTransit_) return;   // a jump passes it by
        const std::string& name = playerName_[e.player];
        notice_ = (name.empty() ? "PLAYER " + std::to_string(e.player + 1) : name) +
                  " STOPPED RESPONDING: AN AI (NORMAL) TAKES OVER";
        noticeTimer_ = 6.0f;
        std::fprintf(stderr, "player %d: AI takeover at tick %u\n", int(e.player), netTick_);
    }

    bool GameView::mpAutoStep(int autoMode, const std::string& mapId) {
        using S = ta::net::MpClient::State;
        lobbyAutoMode_ = autoMode;
        if (autoMode) rememberRoom_ = false;   // a harness room writes no remembered settings
        // Reconnecting after the link dropped mid-game: mpStep owns it (a reconnect
        // passes through Lobby, which must not read as the lobby branches below).
        if (reconnectUntilMs_) return mpStep();
        if (!mp_->poll()) {
            // The link is gone: so are the browser's window and every password typed for a room.
            browserStep();
            roomPasswordStep();
            if (beginReconnect()) return true;
            netError_ = lanHostGone() ? "the host closed the game" : mp_->error();
            return false;
        }
        S st = mp_->state();
        // A door's or CREATE's room: in it, or refused (the lobby says why), or gone with the link.
        if (st == S::InRoom || st == S::Done) roomAutoCreate_ = roomCreateSent_ = false;
        else if (roomCreateSent_ && !mp_->joinError().empty()) roomAutoCreate_ = roomCreateSent_ = false;
        browserStep();   // the game browser's own request, and the window that waits on it
        roomPasswordStep();   // the password typed for a room: its own while in it, then wiped
        if (st == S::Done) {
            if (beginReconnect()) return true;
            if (netError_.empty()) netError_ = mp_->error();
            return false;
        }
        if (st == S::InGame) powerScriptStep();   // TA_POWER_SCRIPT (debug builds)
        // A START the server refused ends a harness run, with the reason as its error. Only
        // the auto-start paths set mpStarted_; a player in the room reads the refusal there.
        if (st == S::InRoom && mpStarted_ && !mp_->startError().empty()) {
            netError_ = "start refused: " + mp_->startError(); return false;
        }
        // A createGame whose map cannot build sends nothing, so the lobby never advances: end a
        // harness run on it (and never retry -- each try rebuilds the map). Interactive auto
        // modes (menu Benchmark, CLI single-player) ignore the result and keep the banner.
        if (autoMode && st == S::Lobby && !mp_->createError().empty()) {
            if (netError_.empty()) netError_ = "create game failed: " + mp_->createError();
            return false;
        }
        // So does a CreateGame the server refused (its game limit): the connection stays up in
        // the lobby, where a hosting mode would ask again every frame.
        if ((autoMode == 1 || autoMode == 4 || autoMode == 7 || autoMode == 8) && st == S::Lobby &&
            !mp_->joinError().empty()) {
            if (netError_.empty()) netError_ = "create game refused: " + mp_->joinError();
            return false;
        }
        // TA_ROOM_SCRIPT (debug builds): room actions, after this mode's own seating step.
        if (!roomScriptStep(autoMode)) return false;
        if (st == S::Lobby && autoMode == 0 && roomAutoCreate_ && !roomCreateSent_) {
            // PLAY THE MACHINE, a LOCAL SERVER host, --play-map: the room opens straight away,
            // from the remembered settings (headless too, so a menu pick reaches the room).
            createRoomFromPrefs();
        } else if (st == S::Lobby && (autoMode == 1 || autoMode == 4 || autoMode == 7 || autoMode == 8)) {
            if (const char* be = ta::devEnv("TA_BENCH")) {   // headless: benchmark run
                int lv = std::atoi(be);                        // TA_BENCH=<level 1..6>, default High
                benchmarkLevel_ = (lv >= 1 && lv <= ta::sim::kBenchLevels) ? lv : 3;
                benchmarkMode_ = true;                         // (forces watch + 8 AI + cap 8 + Painted Desert below)
            }
            // The Benchmark runs with no mod packs at all (a menu run already applied this).
            if (benchmarkMode_) ta::applyBenchmarkMods(mods_, mp_);
            ta::net::GameOptions o;
            o.gameMods = mods_.game ? 1 : 0;   // debug --mods override (the lobby always sends 1)
            // TA_SPEED: set the game speed in tenths (10 = 1x) for headless timing
            // tests -- re-cadences the server without touching the (deterministic) sim.
            if (const char* sp = ta::devEnv("TA_SPEED")) o.speed = uint8_t(std::clamp(std::atoi(sp), 1, ta::net::kMaxDevGameSpeed));
            // The menu Benchmark runs at its SPEED pick: NORMAL, +10 (2x, retail's fastest,
            // the default) or +100 (11x, a benchmark room only), for 60 real seconds either
            // way; the results show where the sim falls short.
            else if (benchmarkMode_) o.speed = uint8_t(10 + benchmarkSpeed_);
            // TA_SPEED_AT schedules in-game speed changes (see mpStep), so the room must
            // allow them.
            if (ta::devEnv("TA_SPEED_AT")) o.speedUnlock = 1;
            o.stressTest = ta::devFlag("TA_STRESS") ? ta::net::kStressStaged : 0;   // headless: spawn ~95% cap per AI
            if (const char* uc = ta::devEnv("TA_UNITCAP")) o.unitCap = uint16_t(std::atoi(uc));
            // The remaining room options, as headless knobs. Without these a harness
            // could not reach the code they gate -- and GODS in particular is where a
            // desync has already hidden once: summoning used to run only on the client,
            // so the referee's world ran a unit short from the first god onward. A
            // desync hunt that cannot turn gods on cannot find that class of bug.
            // TA_RANDOM_STARTS=2 picks BY SLOT; any other set value but 0 RANDOM.
            const char* starts = ta::devEnv("TA_RANDOM_STARTS");
            o.randomStarts = starts && std::atoi(starts) == ta::net::kStartsBySlot ? ta::net::kStartsBySlot
                           : ta::devFlag("TA_RANDOM_STARTS") ? ta::net::kStartsRandom : ta::net::kStartsFixed;
            o.monarchExpendable = ta::devFlag("TA_MONARCH_EXPENDABLE") ? 1 : 0;
            o.forfeitSelfDestruct = ta::devFlag("TA_FORFEIT_SELFDESTRUCT") ? 1 : 0;
            o.doubleSight = ta::devFlag("TA_DOUBLE_SIGHT") ? 1 : 0;
            // The menu Benchmark's pick; every other auto launch is Retail. TA_LEGION
            // forces Legion for any of them (a TA_BENCH run has no pick of its own).
            o.pathfindingMode = ta::devFlag("TA_LEGION") ? ta::sim::PathfindingMode::Legion :
                benchmarkPathfinding_;
            // TA_FOG=0|1|2 forces the room's fog rule (not explored / explored /
            // full vision) so the setting can be tested end to end without driving
            // the lobby by hand.
            if (const char* fg = ta::devEnv("TA_FOG"))
                o.fogExplored = uint8_t(std::clamp(std::atoi(fg), 0, 2));
            // TA_NO_SPECTATORS=1: the room refuses watchers (ALLOW SPECTATORS off), so
            // a --mpspectate client can be shown the server's refusal.
            o.allowSpectators = ta::devFlag("TA_NO_SPECTATORS") ? 0 : 1;
            // TA_POWER_CODES=1: the room plays with POWER CODES on (the server clears it
            // for a mission), so a harness can type codes (TA_POWER_SCRIPT).
            o.powerCodes = ta::devFlag("TA_POWER_CODES") ? 1 : 0;
            // TA_MP_WATCH: host creates the game as a spectator (no slot) so every
            // slot can be an AI -- an all-AI game to watch.
            if (benchmarkMode_) o.benchmark = uint8_t(benchmarkLevel_);   // menu Benchmark: intensity level
            // Benchmark is an all-AI WATCH run (host takes no slot) on Painted Desert, forced
            // to 8 slots regardless of the map's start-position count (setupMatch synthesises
            // the extra starts), private (not in the browser).
            bool watch = (autoMode == 1 && ta::devEnv("TA_MP_WATCH")) || benchmarkMode_;
            // Mode 7 is interactive SINGLE-PLAYER: a private game (hidden from the
            // browser) with one server-run AI opponent.
            // Mode 8 is a single-player CAMPAIGN mission: a private game whose world is
            // built from the mission bundle (server + every peer run setupMission).
            bool priv = autoMode == 7 || autoMode == 8 || benchmarkMode_;
            std::string mission = autoMode == 8 ? missionStem_ : std::string();
            // TA_MAP_SCENARIO=1: create the room with MAP SCENARIO on (the server refuses START when
            // the map has no scenario or a needed seat is empty -- a harness can test both).
            o.mapScenario = autoMode != 8 && ta::devFlag("TA_MAP_SCENARIO") ? 1 : 0;
            if (autoMode == 7 && skirmish_) {   // TA SKIRMISH options
                const auto& sk = *skirmish_;
                o.monarchExpendable = sk.commanderEnds ? 0 : 1;
                o.randomStarts = sk.fixedStarts ? ta::net::kStartsFixed : ta::net::kStartsRandom;
                o.fogExplored = sk.lineOfSight == 0 ? 2 : (sk.mapped ? 1 : 0);
                for (const auto& seat : sk.seats)
                    if (seat.controller == 1) {
                        o.startMetal100 = uint8_t(std::clamp(seat.metal / 100, 1, 255));
                        o.startEnergy100 = uint8_t(std::clamp(seat.energy / 100, 1, 255));
                    }
            }
            uint8_t cap = (autoMode == 8 || benchmarkMode_) ? ta::net::kMaxSlots : mpCapacity(o.mapScenario != 0);
            // Exact stem match (findMap): the map file is "painted desert.tnt" (a 19x19 map
            // with eight start positions).
            std::string createMap = benchmarkMode_ ? std::string("Painted Desert") : mapId;
            mp_->createGame(benchmarkMode_ ? "Benchmark" : (priv ? "Single Player" : "headless"),
                            "", createMap, o, cap, watch, priv, mission);
        } else if (st == S::Lobby && autoMode == 6) {
            // Spectate: poll the list and watch the first running game. Ask once
            // per refusal, not every frame: a room with ALLOW SPECTATORS off says no,
            // and the browser keeps showing why.
            if (SDL_GetTicks64() - mpListMs_ > 300) { mp_->listGames(); mpListMs_ = SDL_GetTicks64(); }
            if (mp_->joinError().empty())
                for (const auto& g : mp_->games())
                    if (g.running) { mp_->spectate(g.id, ""); break; }
        } else if (st == S::Lobby && (autoMode == 2 || autoMode == 3)) {
            // Poll the game list; join the first, or (mode 3) create if none appear.
            if (SDL_GetTicks64() - mpListMs_ > 300) { mp_->listGames(); mpListMs_ = SDL_GetTicks64(); }
            if (!mp_->games().empty()) mp_->joinGame(mp_->games().front().id, "");
            else if (autoMode == 3 && mpListMs_ && SDL_GetTicks64() - mpFirstListMs_ > 800) {
                ta::net::GameOptions o;
            o.gameMods = mods_.game ? 1 : 0;   // debug --mods override (the lobby always sends 1)
                o.mapScenario = ta::devFlag("TA_MAP_SCENARIO") ? 1 : 0;   // as for modes 1/4/7
                o.powerCodes = ta::devFlag("TA_POWER_CODES") ? 1 : 0;
                mp_->createGame(mapId, "", mapId, o, mpCapacity(o.mapScenario != 0));
            }
            if (!mpFirstListMs_) mpFirstListMs_ = SDL_GetTicks64();
        } else if (st == S::InRoom && autoMode && !mpReadied_) {
            const auto& r = mp_->room();
            // Host-spectator (TA_MP_WATCH): seat AIs in the LOW slots (0..N-1) and
            // don't seat self -- an all-AI game the host just watches.
            if (r.mySlot < 0 && (benchmarkMode_ || (autoMode == 1 && ta::devEnv("TA_MP_WATCH")))) {
                const char* ai = ta::devEnv("TA_MP_AIS");
                // Benchmark: always a full 8-faction FFA (each AI its own team -> they fight,
                // which is the point of the load test). FIXED factions by slot (k%5):
                // AI1 Aramon, AI2 Taros, AI3 Veruna, AI4 Zhon, AI5 Creon, AI6 Aramon,
                // AI7 Taros, AI8 Veruna -- never random.
                int nAi = benchmarkMode_ ? int(ta::net::kMaxSlots)
                                         : std::clamp(ai ? std::atoi(ai) : 2, 2, int(ta::net::kMaxSlots));
                for (int k = 0; k < nAi; ++k)
                    mp_->setSlot(k, 2, uint8_t(k % ta::sides::count()), uint8_t(k), uint8_t(k), 1, aiLevelEnv(k));
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
                if (skirmish_) {
                    // TA SKIRMISH rows: the human takes its own seat, the computers
                    // the next ones, in row order; ally groups become teams.
                    const auto& sk = *skirmish_;
                    int slot = 0;
                    auto team = [&](const ta::SkirmishSeat& s, int sl) { return uint8_t(s.ally < 5 ? 10 + s.ally : sl); };
                    for (const auto& seat : sk.seats)
                        if (seat.controller == 1) {
                            mp_->setSlot(r.mySlot, 1, uint8_t(seat.side), uint8_t(seat.color),
                                         team(seat, r.mySlot), 1);
                        }
                    for (const auto& seat : sk.seats) {
                        if (seat.controller != 2) continue;
                        while (slot == r.mySlot) ++slot;
                        if (slot >= int(ta::net::kMaxSlots)) break;
                        // Difficulty Easy/Medium/Hard: the computer level (src/ai).
                        mp_->setSlot(slot, 2, uint8_t(seat.side), uint8_t(seat.color), team(seat, slot), 1,
                                     uint8_t(skirmishAiLevel(sk.difficulty)));
                        ++slot;
                    }
                    mp_->startGame();
                    mpStarted_ = true;
                }
                // Headless test hook: TA_SP_AIS=N seats N AI opponents and starts
                // immediately (the interactive path leaves this to the player).
                else if (const char* na = ta::devEnv("TA_SP_AIS")) {
                    // The auto-start hook must ready-up the host (interactive SP now
                    // seats unready, which would otherwise block startGame()).
                    mp_->setSlot(r.mySlot, 1, facIdx(side_), uint8_t(r.mySlot),
                                 uint8_t(r.mySlot), 1);
                    int n = std::clamp(std::atoi(na), 1, int(ta::net::kMaxSlots) - 1);
                    for (int k = 0; k < n && k + 1 < int(ta::net::kMaxSlots); ++k) {
                        int slot = k + 1;
                        mp_->setSlot(slot, 2, uint8_t((facIdx(aiSide_) + k) % ta::sides::count()),
                                     uint8_t(slot), uint8_t(slot), 1, aiLevelEnv(k));
                    }
                    mp_->startGame();
                    mpStarted_ = true;
                }
                mpReadied_ = true;
            } else if (r.mySlot >= 0) {
                // TA_AUTOPLAY_SIDE=ara|tar|ver|zon|cre: the seat's faction (default by
                // slot), e.g. a side with flying builders for TA_START_FLYERS.
                const char* autoSide = ta::devEnv("TA_AUTOPLAY_SIDE");
                mp_->setSlot(r.mySlot, 1, autoSide ? facIdx(autoSide) : uint8_t(r.mySlot % ta::sides::count()),
                             uint8_t(r.mySlot), uint8_t(r.mySlot), 1);
                // autoMode 4 (AI-game host): also seat one AI opponent in slot 1.
                if (autoMode == 4 && r.mySlot == 0)
                    mp_->setSlot(1, 2, 1, 1, 1, 1, aiLevelEnv());   // AI, tar, colour 1, team 1
                // Host stress harness: TA_MP_AIS=N seats N server AIs in the TOP
                // slots, leaving the low slots for human joiners.
                if (autoMode == 1 && r.mySlot == 0)
                    if (const char* ai = ta::devEnv("TA_MP_AIS")) {
                        int nAi = std::clamp(std::atoi(ai), 0, ta::net::kMaxSlots - 1);
                        for (int k = 0; k < nAi; ++k) {
                            int slot = ta::net::kMaxSlots - 1 - k;
                            mp_->setSlot(slot, 2, uint8_t(slot % ta::sides::count()), uint8_t(slot),
                                         uint8_t(slot), 1, aiLevelEnv(k));   // AI, distinct colour/team
                        }
                    }
                mpReadied_ = true;
            }
        } else if (st == S::InRoom && (autoMode == 2 || autoMode == 3) && !mpStarted_ &&
                   mp_->room().hostId != mp_->myClientId() && mp_->room().mySlot >= 0 &&
                   mp_->room().slots[mp_->room().mySlot].type == 1 && !mp_->room().slots[mp_->room().mySlot].ready) {
            // A harness guest whose READY the host's change cleared (or that never counted:
            // sent on a room it had not seen yet) readies again, once per room revision, with
            // the seat it chose above (a table still in flight may show the server's defaults).
            static std::pair<uint32_t, uint32_t> readiedAt{0, 0};   // (room, roomRev)
            const auto& r = mp_->room();
            if (readiedAt != std::pair<uint32_t, uint32_t>{r.id, r.roomRev}) {
                readiedAt = {r.id, r.roomRev};
                const char* autoSide = ta::devEnv("TA_AUTOPLAY_SIDE");
                mp_->setSlot(r.mySlot, 1, autoSide ? facIdx(autoSide) : uint8_t(r.mySlot % ta::sides::count()),
                             uint8_t(r.mySlot), uint8_t(r.mySlot), 1);
            }
        } else if (st == S::InRoom && (autoMode == 1 || autoMode == 3 || autoMode == 4) &&
                   !mpStarted_ &&
                   mp_->room().hostId == mp_->myClientId()) {
            // (mode 7 single-player does NOT auto-start: the player adds AIs and
            //  clicks START in the Room.)
            int ready = 0;
            for (int i = 0; i < ta::net::kMaxSlots; ++i) {
                const auto& s = mp_->room().slots[i];
                if ((s.type == 1 && s.ready) || s.type == 2) ++ready;   // human-ready or AI
            }
            // Default: start with any 2 ready. TA_MP_WAIT=N holds for a full lobby.
            static const int wantReady = [] {
                const char* w = ta::devEnv("TA_MP_WAIT"); return w ? std::atoi(w) : 2;
            }();
            if (ready >= wantReady && mp_->room().mapsReady) { mp_->startGame(); mpStarted_ = true; }
        } else if (mp_->isCatchUp()) {
            // Spectate (checked BEFORE the state branches -- the replayed bundles may
            // already have flipped the state to InGame): reset the sim and replay from
            // tick 0. The server has queued the whole bundle log after the
            // GameStarting; mpStep drains it, fast-forwarding to now.
            mp_->clearCatchUp();
            // Unit ids restart at 1: snapshots from here on carry a new world epoch,
            // so render caches never take a new unit for the old one with its id.
            worldEpoch_.fetch_add(1, std::memory_order_release);
            writableWorld().resetForReplay();
            netTick_ = 0; outcome_ = 0; outcomeTick_ = 0; resultStartedMs_ = 0; watchOver_ = false; netError_.clear();
            // Replaying history from tick 0: hold our own orders and stop counting
            // acknowledgements until the log is spent (see cmdCatchUp_).
            // The SERVER says where history ends; we do not guess (see
            // Client::replayTicks and the gate below).
            cmdReplayEnd_ = mp_->replayTicks();
            cmdCatchUp_ = cmdReplayEnd_ > 0;
            cmdInFlight_ = 0;
            orderWindow_.clear();
            outbox_.clear();   // pre-disconnect orders are moot; the server dropped them
            outboxHead_ = 0;
            startMpGame(mp_->startRoom(), mp_->startSeed());
            beginSpectating();   // watch-only: no fog, no control
            mpSetupDone_ = true;
            std::fprintf(stderr, "spectating -- replaying to catch up...\n");
            mpStep();   // drain the replay this frame
        } else if (mp_->starting() && !mpSetupDone_) {
            startMpGame(mp_->startRoom(), mp_->startSeed());
            if (!mp_->isSpectator() && !showInitialCampaignBriefing(mp_->startRoom().mission)) return false;
            if (mp_->isSpectator()) {
                // Host-spectator (create-as-spectator): watch-only, no fog, no slot,
                // no resume ticket, and nothing to report loaded.
                beginSpectating();
            } else {
                mp_->reportLoaded(gameDataHash_);
            }
            mpSetupDone_ = true;
        } else if (st == S::InGame || (st == S::Starting && mpSetupDone_ && mp_->gameOver())) {
            // (Starting: a watched game the server ended before its first tick sends no
            // bundle to start on; mpStep still ends the watch.)
#ifndef NDEBUG
            autoplayStep();   // TA_AUTOPLAY: headless humans that actually give orders
            buildlineNetProbe();
            // Exercise defeat through the ordinary synchronized Ctrl+D command.
            if (ta::devFlag("TA_DEFEAT_TEST") && !debugDefeatSent_ && netTick_ >= 60) {
                for (const auto* u : front().live) {
                    if (u->player != localPlayer_ || !u->type || !u->type->commander) continue;
                    ta::net::Command c; c.kind = ta::net::Cmd::Destroy; c.unitId = u->id;
                    issue(c); debugDefeatSent_ = true; break;
                }
            }
#endif
            return mpStep();
        }
        return true;
    }

namespace {
// TA_ROOM_SCRIPT's opt: rules, by the name a step gives them, and the room's value of each.
struct RoomScriptRule { const char* name; ta::RoomField field; };
constexpr RoomScriptRule kRoomScriptRules[] = {
    {"doubleSight", ta::RoomField::DoubleSight},
    {"unitCap", ta::RoomField::UnitCap}, {"monarchExpendable", ta::RoomField::Monarch},
    {"speedUnlock", ta::RoomField::SpeedUnlock}, {"fog", ta::RoomField::Fog},
    {"randomStarts", ta::RoomField::StartLocations}, {"pathfinding", ta::RoomField::Pathfinding},
    {"powerCodes", ta::RoomField::PowerCodes}, {"allowSpectators", ta::RoomField::AllowSpectators}};
int roomRuleValue(const ta::net::GameOptions& o, ta::RoomField field) {
    switch (field) {
    case ta::RoomField::DoubleSight: return o.doubleSight;
    case ta::RoomField::UnitCap: return o.unitCap;
    case ta::RoomField::Monarch: return o.monarchExpendable;
    case ta::RoomField::SpeedUnlock: return o.speedUnlock;
    case ta::RoomField::Fog: return o.fogExplored;
    case ta::RoomField::StartLocations: return o.randomStarts;
    case ta::RoomField::Pathfinding: return int(o.pathfindingMode);
    case ta::RoomField::PowerCodes: return o.powerCodes;
    case ta::RoomField::AllowSpectators: return o.allowSpectators;
    default: return -1;
    }
}
}  // namespace

    void GameView::createRoomFromPrefs() {
        if (!mp_ || mp_->state() != ta::net::MpClient::State::Lobby) { roomActError_ = "not in the game list"; return; }
        if (roomCreateSent_) { roomActError_ = "a game is already being opened"; return; }
        roomOpenError_.clear();
        // The browser's CREATE makes a multiplayer room; the doors named theirs.
        if (roomKind_ == RoomKind::None) roomKind_ = singlePlayer_ ? RoomKind::SinglePlayer : RoomKind::Multiplayer;
        rememberRoom_ = lobbyAutoMode_ == 0 && roomForcedMap_.empty();
        const bool sp = roomKind_ == RoomKind::SinglePlayer;
        // The rules: the kind's remembered set, each value one the room's cells can show. A
        // private single-player room is never listed, so ALLOW SPECTATORS stays at its default
        // there. MAP SCENARIO starts off (a Test Map copy plays its scenario by its opt-in).
        const ta::Settings::RoomPrefs p = settings_ ? settings_->prefsFor(sp) : ta::Settings::RoomPrefs{};
        ta::net::GameOptions o;
        o.doubleSight = p.doubleSight ? 1 : 0;
        o.pathfindingMode = ta::sim::validPathfindingMode(uint8_t(p.pathfindingMode)) ? p.pathfindingMode
                                                                                      : ta::sim::PathfindingMode::Retail;
        o.speedUnlock = p.speedUnlock ? 1 : 0;
        o.unitCap = p.unitCap == 250 || p.unitCap == 500 || p.unitCap == 1000 ? uint16_t(p.unitCap) : uint16_t(2000);
        o.monarchExpendable = p.monarchExpendable ? 1 : 0;
        o.powerCodes = p.powerCodes ? 1 : 0;
        o.fogExplored = uint8_t(std::clamp(p.fog, 0, 2));
        o.randomStarts = uint8_t(std::clamp(p.randomStarts, 0, 2));
        o.allowSpectators = sp || p.allowSpectators ? 1 : 0;
        // Game mods are on in every room: the host ticks mods/game packs in the room, and they
        // and the always-enabled ones go to every player (the server turns them off for missions).
        o.gameMods = 1;
        o.mapScenario = 0;
        roomChosen_ = o; roomChosenSet_ = true;
        mp_->setGameModsAllowed(mods_.game);
        mp_->setGameModPacks(p.gamePacks);
        // The map: the --play-map map and nothing else; otherwise the remembered recipe or map,
        // then the launch's map, then the generator's defaults (buildable from retail data).
        struct Try { std::string id, path; bool remembered; };
        std::vector<Try> tries;
        std::string why;   // the remembered map was passed over: why
        if (!roomForcedMap_.empty()) {
            tries.push_back({roomForcedMap_, ta::hpi::findMap(vfs_, roomForcedMap_), false});
        } else {
            if (p.generated) {
                bool made = ta::mapgen::isGeneratedMapId(p.generator);
                if (made) try { ta::mapgen::parseMapId(p.generator); } catch (const std::exception&) { made = false; }
                if (made) tries.push_back({p.generator, p.generator, true});
                else why = "its random map cannot be made";
            }
            if (!p.map.empty()) {
                if (std::string path = ta::hpi::findMap(vfs_, p.map); !path.empty()) tries.push_back({p.map, path, true});
                else if (why.empty() && !p.generated) why = "it is not installed";
            }
            if (!roomDefaultMap_.empty())
                if (std::string path = ta::hpi::findMap(vfs_, roomDefaultMap_); !path.empty())
                    tries.push_back({roomDefaultMap_, path, false});
            const std::string recipe = ta::mapgen::encodeMapId(ta::mapgen::Params{});
            tries.push_back({recipe, recipe, false});
        }
        std::string opened, failed;
        for (const Try& t : tries) {
            try {
                if (t.path.empty()) throw std::runtime_error("map not installed: " + t.id);
                mp_->createGame(sp ? "Single Player" : roomStartName_, roomStartPass_, t.id, o, capacityFor(t.id, t.path, false), false, sp);
                failed = mp_->createError();
            } catch (const std::exception& e) { failed = e.what(); }
            if (failed.empty()) { opened = t.id; break; }
            std::fprintf(stderr, "room: cannot open a game on '%s': %s\n", t.id.c_str(), failed.c_str());
            if (t.remembered && why.empty()) why = failed;
        }
        // The password is sent, or never to be: the room shows it to its host (roomPasswordStep).
        ta::crypto::wipe(roomPassAsked_);
        if (!opened.empty()) roomPassAsked_ = roomStartPass_;
        ta::crypto::wipe(roomStartPass_);
        if (opened.empty()) {   // a --play-map map that cannot be built here: the lobby says why
            roomActError_ = roomOpenError_ = failed;
            roomAutoCreate_ = false;
            return;
        }
        roomCreateSent_ = true; roomRequestMs_ = SDL_GetTicks64();
        if (!why.empty()) {
            std::string note = "THE REMEMBERED MAP COULD NOT BE USED (" + why + "): " + mapTitle(opened) + " WAS CHOSEN";
            for (char& c : note) c = char(std::toupper(static_cast<unsigned char>(c)));
            roomOpenNote_ = std::move(note);
        }
    }

    bool GameView::roomScriptStep(int autoMode) {
        using S = ta::net::MpClient::State;
        if (!roomScriptRead_) {
            roomScriptRead_ = true;
            const char* script = ta::devEnv("TA_ROOM_SCRIPT");
            const std::string all = script ? script : "";
            for (size_t at = 0; at < all.size();) {
                const size_t end = std::min(all.find(';', at), all.size());
                const std::string item = all.substr(at, end - at);
                at = end + 1;
                if (item.empty()) continue;
                const size_t colon = item.find(':');
                roomScript_.push_back({item, item.substr(0, colon),
                                       colon == std::string::npos ? std::string() : item.substr(colon + 1)});
            }
        }
        if (roomScriptAt_ >= roomScript_.size()) return true;
        const RoomScriptStep step = roomScript_[roomScriptAt_];
        const S st = mp_->state();
        const uint64_t now = SDL_GetTicks64();
        const auto fail = [&](const std::string& why) {
            netError_ = "room script: " + step.text + ": " + why;
            std::fprintf(stderr, "%s\n", netError_.c_str());
            roomScriptAt_ = roomScript_.size();
            quitRequested_ = true;   // an interactive client ends too
            return false;
        };
        // create, join and window act in the browser; every other step in the room, once this
        // mode has seated its player (showpass, shot, press and browser: there, or in the
        // browser). The step's limit runs from when it can act.
        const bool browserVerb = step.verb == "create" || step.verb == "create!" || step.verb == "join" ||
                                 step.verb == "join!" || step.verb == "window";
        if (!roomScriptActed_) {
            const bool inRoom = st == S::InRoom && (autoMode == 0 || mpReadied_);
            const bool inBrowser = st == S::Lobby && !singlePlayer_ && !lanHost_ && !mp_->lanServer();
            const bool either = step.verb == "shot" || step.verb == "showpass" || step.verb == "browser" ||
                                step.verb == "press";
            const bool due = browserVerb ? inBrowser : inRoom || (either && inBrowser);
            if (!due) { roomScriptDueMs_ = 0; return true; }
        }
        if (!roomScriptDueMs_) roomScriptDueMs_ = now;
        // Act once through the member the button calls; a room action that did nothing says why.
        const auto act = [&](auto&& action) {
            roomActError_.clear();
            action();
            roomScriptActed_ = true;
            return roomActError_;
        };
        const auto slotArg = [&](int& slot) {
            slot = std::atoi(step.arg.c_str());
            return !step.arg.empty() && slot >= 0 && slot < ta::net::kMaxSlots;
        };
        const auto& room = mp_->room();
        std::string refused;   // the action did nothing: why
        bool done = false;
        int slot = -1;
        // A browser window's answer: its own line, or the browser's (a game with no password).
        const auto browserAnswer = [&] {
            if (!browserWin_.error.empty()) return browserWin_.error;
            return browserWin_.kind == BrowserWindow::Kind::None ? browserLine() : std::string();
        };
        if (browserVerb) {
            // create[:<name>[:<password>]]: CREATE, its window's fields as given (a name left
            // out stays the default), its CREATE. join:<game name>[:<password>]: that game's
            // row (a live game: its WATCH) and, for a locked one, the password window's field
            // and its JOIN; a window a refusal left open on that game is sent again. With "!"
            // the request must be refused, and the step ends on the answer shown.
            // window:create[:...] and window:join:<game name>[:...] stop short of the window's
            // own button: it stays open as filled in (for a picture).
            const bool fillOnly = step.verb == "window";
            std::string what = step.verb, arg = step.arg;
            if (fillOnly) {
                const size_t cut = arg.find(':');
                what = arg.substr(0, cut);
                arg = cut == std::string::npos ? std::string() : arg.substr(cut + 1);
                if (what != "create" && what != "join") return fail("no window '" + what + "'");
            }
            const bool wantRefusal = what.back() == '!';
            const size_t colon = arg.find(':');
            const std::string name = arg.substr(0, colon);
            const std::string password = colon == std::string::npos ? std::string() : arg.substr(colon + 1);
            if (!roomScriptActed_ && what.rfind("create", 0) == 0) {
                refused = act([&] {
                    openCreateWindow();
                    if (!roomActError_.empty()) return;
                    if (!name.empty()) browserWin_.name = name;
                    browserWin_.password = password;
                    if (!fillOnly) browserWindowCommit();
                });
            } else if (!roomScriptActed_) {
                const auto& games = mp_->games();
                const auto game = std::find_if(games.begin(), games.end(),
                                               [&](const ta::net::GameInfo& g) { return g.name == name; });
                if (game == games.end()) {   // not listed yet: ask again (nothing may be drawing the list)
                    if (now - mpListMs_ > 300) { mp_->listGames(); mpListMs_ = now; }
                } else {
                    refused = act([&] {
                        const bool watch = game->running != 0;
                        const auto kind = watch ? BrowserWindow::Kind::Watch : BrowserWindow::Kind::Join;
                        if (fillOnly && !game->passworded) { roomActError_ = "a game without a password has no window"; return; }
                        if (browserWin_.kind != kind || browserWin_.gameId != game->id) {
                            closeBrowserWindow();
                            browserGameClick(game->id, watch);
                        }
                        if (!roomActError_.empty() || browserWin_.kind == BrowserWindow::Kind::None) return;
                        browserWin_.password = password;
                        if (!fillOnly) browserWindowCommit();
                    });
                }
            }
            if (fillOnly) {
                done = roomScriptActed_ && refused.empty();
            } else if (wantRefusal && !refused.empty()) {   // refused here: nothing went to the server
                std::fprintf(stderr, "room script: %s: refused: %s\n", step.text.c_str(), refused.c_str());
                refused.clear();
                done = true;
            } else if (roomScriptActed_ && refused.empty()) {
                const std::string answer = browserAnswer();
                if (wantRefusal && st != S::Lobby) return fail("the server accepted it");
                if (!wantRefusal && !answer.empty()) return fail(answer);
                if (wantRefusal && !answer.empty()) std::fprintf(stderr, "room script: %s: refused: %s\n", step.text.c_str(), answer.c_str());
                // In the room: with its first LobbyState, so a step that leaves it finds it whole.
                done = wantRefusal ? !answer.empty() : st == S::InRoom ? room.id != 0 : st != S::Lobby;
            }
        } else if (step.verb == "showpass") {
            // The SHOW / HIDE beside the room's password, or beside an open window's field.
            const bool show = std::atoi(step.arg.c_str()) != 0;
            if (st == S::InRoom) {
                if (!roomScriptActed_) refused = act([&] { roomShowPassword(show); });
                done = roomPassShown_ == show;
            } else {
                if (!roomScriptActed_)
                    refused = act([&] {
                        if (browserWin_.kind == BrowserWindow::Kind::None) roomActError_ = "no window is open";
                        else browserWin_.showPassword = show;
                    });
                done = browserWin_.showPassword == show;
            }
        } else if (step.verb == "ai") {
            if (!slotArg(slot)) return fail("no slot '" + step.arg + "'");
            if (!roomScriptActed_) refused = act([&] { roomSetSlotType(slot, 2); });
            done = room.slots[slot].type == 2;
        } else if (step.verb == "map" || step.verb == "map!") {
            // map!: the change must be refused (here, or by the server).
            const bool wantRefusal = step.verb == "map!";
            if (!roomScriptActed_) {
                refused = act([&] {
                    openMapPicker();
                    if (roomActError_.empty()) pickerSelect(step.arg);
                    if (roomActError_.empty()) pickerPick();
                });
                roomScriptSerial_ = picker_.pickSerial;   // 0: PICK had nothing to send
                if (wantRefusal && !refused.empty()) {
                    std::fprintf(stderr, "room script: %s: refused here: %s\n", step.text.c_str(), refused.c_str());
                    refused.clear();
                    done = true;
                }
            }
            // Then the server's answer to that PICK, and the new map verified.
            const auto& answer = mp_->roomMapAnswer();
            using A = ta::net::MpClient::RoomMapAnswer;
            if (!done && refused.empty() && !roomScriptSerial_) {
                if (wantRefusal) return fail("nothing was sent to refuse");
                done = room.mapsReady;
            } else if (!done && refused.empty() && answer.serial == roomScriptSerial_) {
                if (answer.kind == A::TimedOut) return fail("the server did not answer");
                if (answer.kind == A::Refused && !wantRefusal) return fail("refused: " + answer.why);
                if (answer.kind == A::Accepted && wantRefusal) return fail("the server accepted it");
                done = answer.kind == A::Refused || room.mapsReady;
            }
        } else if (step.verb == "opt") {
            const size_t eq = step.arg.find('=');
            const std::string name = step.arg.substr(0, eq);
            const auto rule = std::find_if(std::begin(kRoomScriptRules), std::end(kRoomScriptRules),
                                           [&](const RoomScriptRule& r) { return name == r.name; });
            if (eq == std::string::npos || rule == std::end(kRoomScriptRules)) return fail("no rule '" + name + "'");
            const int value = std::atoi(step.arg.c_str() + eq + 1);
            if (!roomScriptActed_) refused = act([&] { roomRuleCommit(rule->field, value); });
            done = roomRuleValue(room.opts, rule->field) == value;
        } else if (step.verb == "scenario") {
            const bool on = std::atoi(step.arg.c_str()) != 0;
            // A click toggles the cell, so only a room in the other state is clicked. One that
            // would remove or move players arms the cell first: the second click confirms.
            if (!roomScriptActed_ && (room.opts.mapScenario != 0) != on)
                refused = act([&] {
                    roomScenarioClick();
                    if (roomActError_.empty() && scenarioArmUntil_ > SDL_GetTicks64()) roomScenarioClick();
                });
            done = (room.opts.mapScenario != 0) == on;
        } else if (step.verb == "watch") {
            if (!roomScriptActed_) refused = act([&] { roomWatch(); });
            done = mp_->isSpectator();
        } else if (step.verb == "sit") {
            if (!slotArg(slot)) return fail("no slot '" + step.arg + "'");
            if (!roomScriptActed_) refused = act([&] { roomSitHere(slot); });
            done = room.mySlot == slot;
        } else if (step.verb == "wait") {
            // wait:humans=N (seated human players), wait:ready=N (of them, READY).
            const size_t eq = step.arg.find('=');
            const std::string what = step.arg.substr(0, eq);
            if (eq == std::string::npos || (what != "humans" && what != "ready")) return fail("no count '" + what + "'");
            int count = 0;
            for (const auto& s : room.slots) count += s.type == 1 && (what == "humans" || s.ready);
            roomScriptActed_ = true;
            done = count >= std::atoi(step.arg.c_str() + eq + 1);
        } else if (step.verb == "ready") {
            if (!roomScriptActed_) refused = act([&] { roomSetReady(true); });
            done = room.mySlot >= 0 && room.slots[room.mySlot].ready;
        } else if (step.verb == "leave") {
            // LEAVE, as the LOCAL SERVER prompt's YES (it asks nothing more). The room is left at
            // once: a single-player or LOCAL SERVER session ends with this frame.
            if (!roomScriptActed_) refused = act([&] { roomLeave(); });
            done = mp_->state() != S::InRoom;
        } else if (step.verb == "browser") {
            // browser:<ms>: the game list, and nothing else, for that long. After a leave: a
            // room that opens again fails the script (it is due in a room for that).
            if (st != S::Lobby) return fail("in a room (game " + std::to_string(room.id) + ")");
            roomScriptActed_ = true;
            done = now - roomScriptDueMs_ >= uint64_t(std::max(0, std::atoi(step.arg.c_str())));
        } else if (step.verb == "start" || step.verb == "start!") {
            // START once it is enabled, as a player would press it. start!: the server must
            // refuse it, and the step ends on the reason the room shows (printed).
            const bool wantRefusal = step.verb == "start!";
            if (!roomScriptActed_ && roomCanStart(roomFrame(int(kLobbyW)))) {
                refused = act([&] { roomStart(); });
                if (!wantRefusal) mpStarted_ = true;   // a refusal ends the run (above), as for the auto-start modes
            }
            const bool started = st == S::Starting || st == S::InGame;
            if (roomScriptActed_ && wantRefusal && started) return fail("the server started the game");
            if (roomScriptActed_ && !mp_->startError().empty()) {
                if (!wantRefusal) return fail("start refused: " + mp_->startError());
                std::fprintf(stderr, "room script: %s: refused: CANNOT START: %s\n", step.text.c_str(), mp_->startError().c_str());
                done = true;
            } else done = roomScriptActed_ && !wantRefusal && started;
        } else if (step.verb == "press") {
            // press:<x>:<y>[:<clicks>[:<ms>]]: the left button pressed and released at (x, y)
            // of the lobby's 960x540, through input() as SDL delivers it, <ms> after the step
            // came due. <clicks> is the press's click count (2: the second press of a double
            // click). The one step that goes the pointer's way: what it hits is the test's.
            float px = 0, py = 0;
            int clicks = 1, after = 0;
            if (std::sscanf(step.arg.c_str(), "%f:%f:%d:%d", &px, &py, &clicks, &after) < 2 || clicks < 1 || clicks > 255)
                return fail("no point '" + step.arg + "'");
            if (now - roomScriptDueMs_ >= uint64_t(std::max(after, 0))) {
                int w = 0, h = 0;
                SDL_GetRendererOutputSize(ren_, &w, &h);
                SDL_Event e{};
                e.button.button = SDL_BUTTON_LEFT; e.button.clicks = Uint8(clicks);
                e.button.x = int(std::lround((px + lobbyOffX_) * lobbyScale_));
                e.button.y = int(std::lround((py + lobbyOffY_) * lobbyScale_));
                for (const Uint32 type : {Uint32(SDL_MOUSEBUTTONDOWN), Uint32(SDL_MOUSEBUTTONUP)}) {
                    e.type = type; e.button.state = type == SDL_MOUSEBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
                    input(e, w, h);
                }
                roomScriptActed_ = done = true;
            }
        } else if (step.verb == "shot") {
            if (!roomScriptActed_) { roomShotPath_ = step.arg; roomScriptActed_ = true; }
            std::error_code ec;
            done = roomShotPath_.empty() && std::filesystem::exists(ta::u8path(step.arg), ec);
        } else {
            return fail("unknown step");
        }
        if (!refused.empty()) return fail(refused);
        if (done) {
            std::fprintf(stderr, "room script: %s: done\n", step.text.c_str());
            ++roomScriptAt_;
            roomScriptActed_ = false; roomScriptSerial_ = 0; roomScriptDueMs_ = 0;
            if (roomScriptAt_ == roomScript_.size()) std::fprintf(stderr, "room script: finished\n");
            return true;
        }
        // (create! and join! outlast the window's own wait for an answer.)
        const uint64_t limit = step.verb == "wait" ? 30000 : browserVerb && step.verb.back() == '!' ? 20000 : 10000;
        if (now - roomScriptDueMs_ > limit) return fail("not done after " + std::to_string(limit / 1000) + " s");
        return true;
    }

    // TA_ROOM_SCRIPT shot: the whole window as the room drew it, out of the lobby's
    // viewport and scale for the read (put back for anything drawn after).
    void GameView::roomShotCapture() {
        SDL_RenderSetViewport(ren_, nullptr);
        SDL_RenderSetScale(ren_, 1.0f, 1.0f);
        int w = 0, h = 0;
        SDL_GetRendererOutputSize(ren_, &w, &h);
        std::vector<uint8_t> px(size_t(std::max(w, 0)) * size_t(std::max(h, 0)) * 4);
        const SDL_Rect area{0, 0, w, h};
        if (w > 0 && h > 0 && SDL_RenderReadPixels(ren_, &area, SDL_PIXELFORMAT_RGBA32, px.data(), w * 4) == 0) {
            ta::png::write(roomShotPath_, w, h, px);
            std::fprintf(stderr, "room script: wrote %s\n", roomShotPath_.c_str());
        }
        roomShotPath_.clear();
        const SDL_Rect vp{int(lobbyOffX_), int(lobbyOffY_), int(kLobbyW), int(kLobbyH)};
        SDL_RenderSetScale(ren_, lobbyScale_, lobbyScale_);
        SDL_RenderSetViewport(ren_, &vp);
    }

    const std::vector<uint8_t>* GameView::kingdomPalette(const std::string& kingdom) {
        if (kingdom.empty()) return nullptr;
        auto it = kingdomPals_.find(kingdom);
        if (it == kingdomPals_.end()) {
            std::vector<uint8_t> rgba;
            try {
                auto pal = ta::gaf::Palette::fromBytes(vfs_.read("palettes/" + kingdom + ".pcx"),
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

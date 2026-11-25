#include "campaign/campaign.h"
#include "sim/sides.h"
#include "util/virtualpath.h"
#include "client/retailaim.h"
#include "client/tabeamdraw.h"
#include "gaf/nimbus.h"
#include "client/gameview.h"
#include "client/buildline.h"
#include "client/reclaimpick.h"
#include "client/gamespeed.h"
#include "client/guiart.h"
#include "client/shadowmask.h"
#include "client/retailfeatureclock.h"
#include "client/retailbuilderanimation.h"
#include "client/retailmovementcallbacks.h"
#include "client/gaitpace.h"
#include "client/retaildeathsfx.h"
#include "client/runtimesettings.h"
#include "client/chatbox.h"
#include "client/unitstatus.h"
#include "client/displayget.h"
#include "net/crypto.h"
#include "sim/retailmotion.h"
#include <cmath>
#include <cstdlib>
#include "util/u8path.h"

// Out-of-line GameView method definitions (core concern), split from the
// class body in gameview.h so editing a body recompiles only this translation
// unit. Trivial getters, ctors, static, template, constexpr and default-arg
// methods stay inline in the header. Grouping is by name heuristic.

    GameView::GameView(SDL_Renderer* ren, ta::hpi::Vfs vfs, const std::string& mapPath,
             const std::string& installRoot, ta::hpi::ModUse mods,
             bool demo, bool scenario,
             bool bare, const std::string& side, const std::string& aiSide)
        // (side_ initialized below before loadPanel uses it; vfs_ must precede
        //  mapView_ in the member list so the Compositor can borrow it)
        : ren_(ren), vfs_(std::move(vfs)), mapView_(ren, vfs_, mapPath),
          installRoot_(installRoot), mods_(mods),
          mapPath_(mapPath), side_(side), aiSide_(aiSide) {
        if(vfs_.has(mapSibling(".ota"))) {
            const auto ota=vtdf(mapSibling(".ota"));
            if(const auto* header=ota.child("globalheader")) {
                debrisLavaWorld_=header->numberOr("lavaworld",0)!=0;
                debrisNoSeaTrigger_=header->numberOr("nosealeveltrigger",0)!=0;
            }
        }
        // Unit registry: MOVEINFO + units + canbuild.
        // The VFS merges base + Iron Plague + community data into one namespace,
        // precedence resolved by the retail newest-date rule.
        ta::sim::setupRegistry(registry_, vfs_);
        loadTextures();
        mapView_.setZoom(0.9f);
        loadInterfaceFonts();
        loadOrderButtons();
        sounds_.init(vfs_);
        soundClasses_.load(vfs_);   // music is started per-state by manageMusic()
        loadPanel(side_);
        loadGui(side_);

#ifndef NDEBUG
        if (ta::devFlag("TA_GUI_SMOOTH_TEST")) guiSmoothTest();
        if (!bare && !scenario && ta::devFlag("TA_PATROL_PERF")) {
            setupPatrolPerf();
            return;
        }
        if (scenario) {
            setupScenario();
            return;
        }
#endif
        writableWorld().setTerrain(mapView_.map().heights, mapView_.map().width,
                          mapView_.map().height, mapView_.map().seaLevel,
                          &mapView_.map().features);
        ta::sim::registerMapFeatures(writableWorld(), mapView_.map(), vfs_, &registry_);
        loadFeatures();
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;

        // Each side starts with only its Monarch, dropped on the map's real
        // start positions (from the .ota). Pick the two furthest-apart spots
        // so the player and the AI begin on opposite sides.
        auto starts = parseStartPositions();
        float px = cx - 260, pz = cz + 30;   // fallbacks near map center
        float ax = cx + 300, az = cz + 30;
        if (starts.size() >= 2) {
            size_t bi = 0, bj = 1;
            float bestD = -1;
            for (size_t i = 0; i < starts.size(); ++i)
                for (size_t j = i + 1; j < starts.size(); ++j) {
                    float dx = starts[i].first - starts[j].first;
                    float dz = starts[i].second - starts[j].second;
                    if (dx * dx + dz * dz > bestD) { bestD = dx * dx + dz * dz; bi = i; bj = j; }
                }
            px = starts[bi].first;  pz = starts[bi].second;
            ax = starts[bj].first;  az = starts[bj].second;
        } else if (starts.size() == 1) {
            px = starts[0].first; pz = starts[0].second;
        }
        // Camera opens on the player's Monarch.
        mapView_.setOffset(px - 640 / 0.9f, pz - terrainLift(px, pz) - 400 / 0.9f);
#ifndef NDEBUG
        // Local dev/test match layouts (TA_FFA, the 2-monarch start, demo armies):
        // gameview_devharness.cpp. A release build is always `bare` -- the match is built
        // from the server's GameStarting -- so none of that is compiled in.
        if (setupLocalMatch(demo, bare, side, aiSide, starts, cx, cz, px, pz, ax, az)) return;
#else
        (void)demo; (void)scenario; (void)bare; (void)side; (void)aiSide; (void)ax; (void)az;
#endif

        startAtMonarch();

        for (auto& u : world_.units()) {
            if (!u.type || u.type->canMove) continue;
            ta::sim::blockFootprint(writableWorld().nav(), *u.type, u.x.toFloat(), u.z.toFloat(), true);
        }
    }

    GameView::~GameView() {
#ifndef NDEBUG
        previewLifetimeProbe(true);   // TA_PREVIEW_LIFETIME_TEST=teardown
#endif
        stopPreviewReaders();   // no preview may read vfs_ past here
        stopSimThread(/*drain=*/false);
        // The session is over, however it ended (quit, return to menu, results,
        // disconnect, desync, kick, SIGTERM): finish the replay with everything
        // received up to now. The net client outlives the view (main.cpp).
        finishReplayRecording();
        if (mp_) mp_->setRecordingRestartHook(nullptr);
        resetMinimap();
        destroyGpuTextures();
        if (gaitLog_) std::fclose(gaitLog_);
        // No game password typed in this session outlives it.
        for (std::string* typed : {&roomPass_, &roomPassAsked_, &roomStartPass_, &joinPass_, &browserWin_.password})
            ta::crypto::wipe(*typed);
    }

    std::vector<std::pair<float, float>> GameView::parseStartPositions() const {
        return ta::sim::parseStartPositions(vfs_, mapPath_);
    }

    void GameView::discoSound() {
        for (int p = 0; p < 8 && p < worldTables().numPlayers(); ++p) {
            bool on = frameDiscoActive(p);
            if (on) {
                // Centroid of this player's visible dancing units.
                float cx = 0, cz = 0; int n = 0;
                for (const UnitR* _up : front().live) {
                    const UnitR& u = *_up;
                    if (u.player != p || !u.alive() || !dancing(u)) continue;
                    if (!unitShownR(u)) continue;
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
        for (int p = 0; p < 8 && p < worldTables().numPlayers(); ++p) {
            bool on = frameHeadbangActive(p);
            if (on) {
                float cx = 0, cz = 0; int n = 0;
                for (const UnitR* _up : front().live) {
                    const UnitR& u = *_up;
                    if (u.player != p || !u.alive() || !headbanging(u)) continue;
                    if (!unitShownR(u)) continue;
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

    // Music: retail plays no CD music in the front end or the lobby, and in a game it
    // plays by battle status (client/battlemusic.h, docs/retail-music.md).
    void GameView::manageMusic() {
        const bool inGame = !inLobbyPhase();
        const uint32_t now = uint32_t(SDL_GetTicks64() * 30 / 1000);   // retail clock: 1/30 s
        auto apply = [&](const ta::battlemusic::Action& a) {
            if (a.kind == ta::battlemusic::Action::Kind::Play) { sounds_.setMusicFade(1.0f); sounds_.playTrack(vfs_, a.track); }
            else if (a.kind == ta::battlemusic::Action::Kind::Stop) sounds_.stopMusic();
            else sounds_.setMusicFade(a.gain);
        };
        if (!inGame) {
            if (musicMode_ == 2) apply(battleMusic_.stop());
            musicMode_ = 1;
            return;
        }
        const bool spectator = mp_ && mp_->isSpectator();
        const int me = localPlayer_;
        const bool havePlayer = !spectator && me >= 0 && me < frameNumPlayers();
        if (musicMode_ != 2) {
            musicMode_ = 2;
            if (havePlayer) {
                musicKillsSeen_ = framePlayer(me).kills;
                musicEventsSeen_ = framePlayer(me).combatEvents;
            }
            apply(battleMusic_.startGame(now));
            return;
        }
        int units = 0;
        if (havePlayer) {   // only the local player's fights count (0x494e70)
            const auto& p = framePlayer(me);
            for (uint32_t e = musicEventsSeen_; e != p.combatEvents; ++e) battleMusic_.onDamage();
            for (int k = musicKillsSeen_; k < p.kills; ++k) battleMusic_.onKill();
            musicEventsSeen_ = p.combatEvents;
            musicKillsSeen_ = p.kills;
            units = p.unitCount;
        }
        apply(battleMusic_.tick(now, units, sounds_.musicEnded()));
    }

    void GameView::applySettings(const ta::Settings& s) {
        ta::music::setCustom(s.customMusicTracks);   // CUSTOM MUSIC: the next track plays it
        // Push the display options that render code reads from globals (smooth art,
        // cursor factor, movie deblocking). This is GameView's one "settings changed"
        // hook, so wiring it here reaches the Esc-menu Options and its DEFAULTS button
        // without either of them having to know the globals exist.
        ta::applyRuntimeSettings(s);
        if (smoothArt_ != s.smoothArt) {
            smoothArt_ = s.smoothArt;
            reloadInterfaceArt(s);
        }
        sounds_.setMasterVolume(s.masterVol);
        sounds_.setMusicVolume(s.bgmVol);
        sounds_.setSfxVolume(s.sfxVol);
        sounds_.setVoiceLimit(s.soundVoices);
        sounds_.setEvenOut(s.evenSoundLevels);
        unitSpeech_ = std::clamp(s.unitSpeech, 0, 10);       // Options: UNIT SPEECH
        unitChatText_ = std::clamp(s.unitChatText, 0, 10);   // Options: UNIT CHAT TEXT
        for (int i = 0; i < sounds_.channelCount(); ++i) sounds_.setChannelGain(i, s.chanGain[i]);
        mapView_.setZoomSpeed(s.mouseZoomSpeed);
        edgeScrollSpeed_ = s.edgeScrollSpeed;
        edgeScrollOn_ = s.edgeScroll;
        mouseZoomOn_ = s.mouseZoom;   // off: prepare() holds retail's 1:1 view
        uiScale_ = s.uiScale;
        screenShake_ = s.screenShake;                     // Options: camera shake
        buildBarAlign_ = std::clamp(s.buildBarAlign, 0, 2);   // Options: build-menu row
        buildBarScale_ = std::clamp(s.buildBarScale, 0.5f, 4.0f);
        idleBuilders_ = std::clamp(s.idleBuilders, 0, 3);   // Options: IDLE BUILDERS
        idleBuildersScale_ = std::clamp(s.idleBuildersScale, 0.5f, 4.0f);
        chatScale_ = std::clamp(s.chatScale, ta::chatbox::kMinScale, ta::chatbox::kMaxScale);   // Options: chat box
        chatAlign_ = std::clamp(s.chatAlign, 0, 1);
        setTextureFilter(s.textureFilter);
        healthBars_ = std::clamp(s.healthBars, 0, 2);
        statsPanel_ = s.statsPanel;                       // Options: minimap-strip readout
        tacticalDotsOpt_ = s.tacticalDots && s.mouseZoom; // Options: zoomed-out unit dots (none at retail 1:1)
        tacticalDotsZoom_ = s.tacticalDotsZoom;
        hiddenUnitsMode_ = std::clamp(s.hiddenUnits, 0, ta::hiddenunits::kModeCount - 1);   // Options: units behind scenery
        hiddenFadeOpacity_ = ta::hiddenunits::fadeOpacity(s.hiddenFadeOpacity);
        hotkeys_.load(s.hotkeys);                         // Options: rebindable hotkeys
    }

    void GameView::latchSimThreads(int choice, bool localServer, bool pin) {
        // k threads: the thread that ticks the World (the sim worker, or this one for a
        // replay and the headless harness) plus k - 1 SimPool workers. Nothing has
        // ticked yet, and the World builds its pool at its first pooled pass, on the
        // thread that ticks it.
        using ta::sim::SimPool;
        simThreads_ = std::max(0, choice);
        const ta::sim::CpuTopology cpus = ta::sim::detectCpus();
        // A debug build's TA_SIM_WORKERS overrides AUTO, and only AUTO: this simulation
        // runs it, and so does the local server's (started with auto, it reads it too).
        simSession_ = SimPool::session(simThreads_, cpus, localServer, &simDebugAuto_);
        writableWorld().setSimWorkers(int(simSession_.game) - 1);
        // Pinned: the game's CPUs of the hand-out, a slot each (simWorkerLoop takes the
        // first, the World's pool the rest).
        simPin_ = pin && cpus.canPin;
        simCpus_ = SimPool::handOut(cpus.order, simSession_, localServer, simPin_).game;
        if (simPin_) writableWorld().setSimCpus(simCpus_);
    }

    void GameView::logSimThreads(bool ownThread) const {
        // sim threads: 4 (auto: 8, half; local server 4) pinned to cpus 0,2,4,6 / 4 (of 8,
        // half; local server 4) / 8 (auto) / 8: the count this simulation runs, where it
        // came from, and the CPUs its threads are pinned to (none: nothing more). The main
        // thread takes no CPU: where it ticks the World, the first is left out.
        std::vector<int> pinned = simCpus_;
        if (!ownThread && !pinned.empty()) pinned.erase(pinned.begin());
        const std::string cpus = ta::sim::pinnedText(ta::sim::cpuListText(pinned),
                                                      simSession_.game - unsigned(pinned.size()));
        char from[96] = "";
        if (simDebugAuto_ && simSession_.server)
            std::snprintf(from, sizeof from, " (auto: TA_SIM_WORKERS; local server %u)", simSession_.server);
        else if (simDebugAuto_) std::snprintf(from, sizeof from, " (auto: TA_SIM_WORKERS)");
        else if (simSession_.server)
            std::snprintf(from, sizeof from, " (%s %u, half; local server %u)", simThreads_ > 0 ? "of" : "auto:",
                          simSession_.total, simSession_.server);
        else if (simThreads_ == 0) std::snprintf(from, sizeof from, " (auto)");
        std::fprintf(stderr, "sim threads: %u%s%s\n", simSession_.game, from, cpus.c_str());
    }

    // The AUDIO page (Esc menu > AUDIO): sound device, volumes, speakers, CUSTOM MUSIC.
    void GameView::openAudioOptions() {
        if (!settings_) return;
        options_ = std::make_unique<ta::OptionsScreen>(ren_, *settings_, [this] { applySettings(*settings_); },
            [this] { saveSettings(*settings_); }, ta::music::sideTracks(vfs_), sounds_.channelCount(),
            [this] { sounds_.reopenDevice(); });
    }

    void GameView::openOptions(ta::OptionsPage page) {
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
            [this] { sounds_.reopenDevice(); },
            // GAME DATA FOLDER: shown, never changeable here -- this view's whole session
            // (and in multiplayer, its agreed gameplay data) sits on the mounted install.
            ta::DataFolderRow{installRoot_,
                replayMode_ ? "Change it from Settings > Game on the main menu, not while watching a replay."
                : inLobbyPhase() ? "Change it from Settings > Game on the main menu, not in a lobby."
                                 : "Change it from Settings > Game on the main menu, not during a game.", {}},
            page);
    }

    namespace {
    // World::reclaim's caster test, for one of the local player's units.
    bool canReclaimAsLocal(const UnitR& u, int localPlayer) {
        return u.alive() && u.type && u.type->isBuilder && u.type->canMove &&
               u.type->canReclaim && u.player == localPlayer;
    }
    }

    bool GameView::haveReclaimer() {
        for (int id : selection_)
            if (const auto* u = frameUnitP(id))
                if (canReclaimAsLocal(*u, localPlayer_)) return true;
        return false;
    }

    // World::reviveKind on the render snapshot: what a RESURRECT order from
    // one of our units on that body would be (ta::sim::reviveKindFor), 0 for
    // none. The sim checks it again when it applies the command.
    uint8_t GameView::reviveKindOn(const UnitR& caster, const UnitR& body) const {
        if (!caster.alive() || caster.embarked() || caster.underConstruction || !caster.type ||
            caster.player != localPlayer_ || !caster.type->canMove || caster.type->isStructure())
            return 0;
        const auto& feats = worldTables().featureTypes();
        if (body.alive() || !body.corpsePhase || body.corpseFading || body.corpseFeat < 0 ||
            size_t(body.corpseFeat) >= feats.size())
            return 0;
        return ta::sim::reviveKindFor(*caster.type, feats[size_t(body.corpseFeat)]);
    }

    // TotalA.exe raises a feature as well (RESURRECT 0x404db0 finds it at the
    // point): a wreck or heap whose name names a unit (FeatType::raises).
    uint8_t GameView::reviveKindOnFeature(const UnitR& caster, int featureId) const {
        if (!caster.alive() || caster.embarked() || caster.underConstruction || !caster.type ||
            caster.player != localPlayer_ || !caster.type->canMove || caster.type->isStructure())
            return 0;
        const auto& feats = worldTables().featureTypes();
        for (const auto& f : features_)
            if (f.hasSim && f.simId == featureId && f.aliveVis && f.reclaimable &&
                f.simType >= 0 && size_t(f.simType) < feats.size())
                return ta::sim::reviveKindFor(*caster.type, feats[size_t(f.simType)]);
        return 0;
    }

    // The body under a world point that a selected unit can raise, with the
    // reclaim pick circle (nearest wins); 0 = none.
    int GameView::raiseTargetAt(float wx, float wz) const {
        ta::client::ReclaimPick pick{wx, wz};
        const auto& live = front().live;
        for (uint32_t index : bodyCandidates(wx, wz)) {
            const UnitR* body = live[index];
            if (body->alive() || !body->corpsePhase || !canPickPoint(body->x, body->z)) continue;
            for (int id : selection_)
                if (const auto* caster = frameUnitP(id); caster && reviveKindOn(*caster, *body)) {
                    pick.offer(body->id, body->x, body->z, body->corpseFootX, body->corpseFootZ);
                    break;
                }
        }
        return pick.id ? *pick.id : 0;
    }

    // Every selected unit that can reclaim, in selection order. Retail's order code
    // (5214b0) resolves each selected unit on its own through 4de530 (case 0xc): a
    // type without the reclaim bit (+0x264 0x800) gets no mission and its queue is
    // left alone, every reclaimer gets the same box or point (no claims, no split).
    std::vector<int> GameView::reclaimers() {
        std::vector<int> out;
        for (int id : selection_)
            if (const auto* u = frameUnitP(id))
                if (canReclaimAsLocal(*u, localPlayer_)) out.push_back(id);
        return out;
    }

    void GameView::issueReclaimBox(float x0, float z0, float x1, float z1, uint8_t queue,
                                   const std::vector<int>& only) {
        ClickScope click(*this);
        const auto ids = only.empty() ? reclaimers() : only;
        if (ids.empty()) return;
        for (int id : ids) {   // each its own RECLAIMAREA (4d78a0), Shift queues per unit
            ta::net::Command c;
            c.kind = ta::net::Cmd::ReclaimArea;
            c.unitId = id;
            c.x = x0; c.z = z0; c.x2 = x1; c.z2 = z1;
            c.queue = uint8_t(queue);
            issue(c);
        }
        notice_ = "CLEAR AREA";
        noticeTimer_ = 2;
        voice(ackUnit(ids), "move");
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
        if (watchOnly()) return;   // the backstop: a command from a watcher disconnects it
        c.player = uint8_t(localPlayer_);
        if (c.kind == ta::net::Cmd::Move || c.kind == ta::net::Cmd::AttackMove || c.kind == ta::net::Cmd::Patrol)
            noteSpotFlash(c.unitId);
        if (click_) { click_->cmds.push_back(std::move(c)); return; }
        if (mp_) outbox_.push_back(c);
        else apply(c);
    }

    // The outermost ClickScope closed: a Legion net game sends the click as
    // explicit orders, everything else command by command, as issued.
    void GameView::flushClick() {
        if (!click_) return;
        ClickBuffer click = std::move(*click_);
        click_.reset();
        if (click.cmds.empty()) return;
        if (!mp_) { for (const auto& c : click.cmds) apply(c); return; }
        if (!ta::sim::isLegionPathfinding(worldTables().pathfindingMode())) {
            outbox_.insert(outbox_.end(), click.cmds.begin(), click.cmds.end());
            return;
        }
        // Serials are given when an order is sent (mpStep), so a click held through a
        // rejoin's catch-up still gets one above every order that activated before.
        uint32_t provisional = nextOrderSerial_;
        for (auto& item : ta::net::packClick(click.cmds, click.shape, provisional))
            outbox_.push_back(std::move(item));
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
            // a world that had nothing to do with the one they came from.
            missionStem_ = mission;
            int human = 0;
            ta::sim::MissionSetup ms;
            // (Debug) a recording of a TA_FORCE_WIN / TA_FORCE_LOSE game replays under the same.
            const auto forced = ta::forcedOutcomeMission(vfs_, mission);
            if (ta::sim::setupMission(writableWorld(), registry_, forced ? *forced : vfs_, mission, human, &ms))
                spots = ms.slotPos;
            else
                std::fprintf(stderr, "replay: mission '%s' not in this data\n",
                             mission.c_str());
        } else {
            spots = ta::sim::setupMatch(writableWorld(), registry_, cfg);
        }
        logSimThreads(false);   // what this playback's simulation runs on (the main thread ticks it)
        replayBundles_ = std::move(bundles);
        replayMode_ = true;
        replayKeyBudget_ = ta::replaybar::keyframeBudget(uint64_t(SDL_GetSystemRAM()) << 20);
        noFog_ = true;             // a spectator sees the whole map
        localPlayer_ = 0;
        side_ = "arm";
        loadPanel(side_);
        loadGui(side_);
        if (!spots.empty())
            mapView_.setOffset(spots[0].first - 640 / 0.9f,
                               spots[0].second - terrainLift(spots[0].first, spots[0].second) - 400 / 0.9f);
    }

    void GameView::replayStep(float dt) {
        namespace rb = ta::replaybar;
        if (replayKeys_.empty() && replayTick_ == 0) replayCaptureKeyframe();   // what rewinds reach
        if (replayHeld_ && !(SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK)) replayHeld_ = 0;
        if (replayHeld_ && SDL_GetTicks64() >= replayHeldNextMs_) {   // << >> repeat while held
            replayHoldNudge(replayHeld_);
            replayHeldNextMs_ = SDL_GetTicks64() + 200;
        }
#ifndef NDEBUG
        if (replaySeekStraight_.erase(replayTick_)) replaySeekPrint();
        if (!replaySeekTo_ && !replaySeekScript_.empty()) {
            replayJumpTo(replaySeekScript_.front());
            replaySeekScript_.pop_front();
        }
#endif
        // The early return used to sit HERE, which quietly broke the last snapshot:
        // captureFrame only publishes the buffer, while the registration that makes a
        // unit drawable happens in cosmeticStep (it walks front().live and calls
        // registerUnit). Stop calling cosmeticStep and any unit that FIRST appears in
        // the final snapshot is never registered -- so a replay consumed in one frame
        // still renders an empty map, which is exactly the bug this was meant to fix.
        // Publishing and registering are two steps and both have to keep running.
        //
        // Pausing is the same story: a paused replay still has to draw what it froze on.
        // A jump runs paused or not, and playback then carries on as it was.
        const bool jumping = replaySeekTo_.has_value();
        if (!replayHalted_ && ((!paused_ && replayTick_ < replayBundles_.size()) || jumping || replayPublish_))
            replayAdvance(dt);
        else replayRealRate_ = 0;
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
        //   very fast -> no sound at all, and display time held at 8x: nothing can be
        //              followed at that rate, and the effects and animation are what
        //              would otherwise cap it. Ticks are not skipped.
        //
        //   in transit -> no sound and no display time either, up to and including the
        //              frame that shows the landing, which is what cosmeticStep decides
        //              on. It drops whatever the jump queued, so nothing comes out after.
        const float speed = speedMult();
        sounds_.setEffectsMuted(replayInTransit_ || speed > rb::kFullEffectsSpeed);
        cosmeticStep(paused_ || replayInTransit_ ? 0.0f : dt * std::min(speed, rb::kFullEffectsSpeed));
    }

    bool GameView::replayResult() const {
        if (!replayMode_ || !missionStem_.empty() || replayBundles_.empty() || replaySeekTo_ ||
            replayTick_ < replayBundles_.size())
            return false;
        const Frame& f = front();
        if (f.winningTeam < 0 || f.gameTick < replayBundles_.size()) return false;
        int teams = 0;
        for (int p = 0; p < f.numPlayers && p < int(f.players.size()); ++p) {
            if (f.players[size_t(p)].built <= 0) continue;
            bool first = true;
            for (int q = 0; q < p && first; ++q)
                first = f.players[size_t(q)].built <= 0 || f.players[size_t(q)].team != f.players[size_t(p)].team;
            teams += first;
        }
        return teams >= 2;
    }

    std::vector<uint32_t> GameView::replayKeyTicks() const {
        std::vector<uint32_t> ticks;
        ticks.reserve(replayKeys_.size());
        for (const auto& key : replayKeys_) ticks.push_back(key.tick);
        return ticks;
    }

    void GameView::replayTickOnce() {
#ifndef NDEBUG
        if (situation_.armed()) situation_.observe(uint32_t(replayTick_), world_, replayBundles_);
#endif
        const auto& bd = replayBundles_[replayTick_];
        for (const auto& c : bd.cmds) apply(c);
        for (const auto& e : bd.events) { noteEvent(e); applyEvent(e); }
        writableWorld().tick(1.0f / 30.0f);
        captureTransportEffects();
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
                // Keeps checking the position digest after the first hash divergence:
                // that is what separates a hash-layout change from changed play.
                if (replayTracker_.observe(ck, mine, [&] { return world_.posDigest(); })) {
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
        if (replayTick_ % ta::replaybar::kKeyframeTicks == 0) replayCaptureKeyframe();
#ifndef NDEBUG
        if (replaySeekStraight_.erase(replayTick_)) replaySeekPrint();
#endif
    }

    void GameView::replayAdvance(float dt) {
        namespace rb = ta::replaybar;
        const float speed = speedMult();
        const bool jumping = replaySeekTo_.has_value();
        if (!paused_ && !jumping) replayAccum_ += dt * speed;
        // On time, not on a tick count: how much fits in a frame is what the game's
        // size decides. Never a tick, a checkpoint or a keyframe is skipped.
        const auto start = std::chrono::steady_clock::now();
        const auto budget = std::chrono::duration<double, std::milli>(rb::frameBudgetMs(speed, jumping));
        bool advanced = false;
        size_t ticks = 0;
        while (replayTick_ < replayBundles_.size()) {
            if (jumping ? replayTick_ >= *replaySeekTo_ : replayAccum_ < 1.0f / 30.0f) break;
            if (advanced && std::chrono::steady_clock::now() - start >= budget) break;
            replayTickOnce();
            if (!jumping) replayAccum_ -= 1.0f / 30.0f;
            advanced = true;
            ++ticks;
        }
        if (jumping && replayTick_ >= *replaySeekTo_) {
            replaySeekTo_.reset();
            replayAccum_ = 0;
            worldEpoch_.fetch_add(1, std::memory_order_release);   // the landing is its own frame
            replayDisplayStale_ = true;
            replayPublish_ = true;
#ifndef NDEBUG
            if (replaySeekPrinting_) replaySeekPrint();
#endif
        }
        // What did not fit is dropped, so nothing keeps racing after the speed comes down.
        replayAccum_ = std::min(replayAccum_, std::max(1.0f / 30.0f, dt * speed));
        // The rate the speed label reports when it falls short of the request.
        if (paused_ || jumping) { replayRealRate_ = 0; replayRateSec_ = 0; replayRateTicks_ = 0; }
        else {
            replayRateTicks_ += ticks;
            replayRateSec_ += dt;
            if (replayRateSec_ >= 0.5f) {
                replayRealRate_ = float(replayRateTicks_) / 30.0f / replayRateSec_;
                replayRateSec_ = 0;
                replayRateTicks_ = 0;
            }
        }
        // PUBLISH what we just simulated, exactly as update() does for a live game
        // (simStep ends with captureFrame, then cosmeticStep runs). replayStep did
        // neither, so front() was never filled: the render reads front().live, which
        // stayed empty, and playback drew the terrain with none of the recorded
        // armies on it. Effects follow the same snapshot, so cosmeticStep comes after.
        if (advanced || replayPublish_) captureFrame();
        replayPublish_ = false;
    }

    void GameView::replayCaptureKeyframe() {
        namespace rb = ta::replaybar;
        const uint32_t tick = uint32_t(replayTick_);
        const auto at = std::lower_bound(replayKeys_.begin(), replayKeys_.end(), tick,
                                         [](const ReplayKeyframe& k, uint32_t t) { return k.tick < t; });
        if (at != replayKeys_.end() && at->tick == tick) return;   // laid on an earlier pass
        // Room for one like the last, made before the copy is taken: a copy never runs the
        // keyframes over the budget by more than it has outgrown the last. With no room to
        // make, this capture is passed over: nothing is copied to find out (that copy would
        // sit on top of a full budget) and nothing is given up. The size asked for is the
        // last copy's until a jump brings that of the keyframe it restored, so a game that
        // outgrew the budget is asked about again wherever it is rewound to.
        if (!replayThinKeys(replayKeyLast_)) return;
        try {
            ReplayKeyframe key;
            key.tick = tick;
            key.hash = world_.stateHash();
            std::copy(std::begin(playerAi_), std::end(playerAi_), key.playerAi.begin());
            {
                ta::client::AllocCount counted;
                key.world = world_.keyframe();
                key.bytes = counted.bytes();
            }
            key.bytes += key.world->copyOnWriteBytes();
            replayKeyLast_ = key.bytes;
            replayKeyBytes_ += key.bytes;
            const auto place = std::lower_bound(replayKeys_.begin(), replayKeys_.end(), tick,
                                                [](const ReplayKeyframe& k, uint32_t t) { return k.tick < t; });
            replayKeys_.insert(place, std::move(key));
            // This one can be the bigger. If even alone beside tick 0 it does not fit, it goes again.
            if (!replayThinKeys(0) && replayKeys_.size() > 1) {
                const auto mine = std::lower_bound(replayKeys_.begin() + 1, replayKeys_.end(), tick,
                                                   [](const ReplayKeyframe& k, uint32_t t) { return k.tick < t; });
                if (mine != replayKeys_.end() && mine->tick == tick) {
                    replayKeyBytes_ -= mine->bytes;
                    replayKeys_.erase(mine);
                }
            }
        } catch (const std::bad_alloc&) {
            // The machine has less to give than the budget took for granted: on with half the
            // budget, thinned to it as at any other time, so what is kept stays spread over
            // what was played. Back to tick 0 alone when even that cannot be done.
            std::fprintf(stderr, "replay: out of memory at tick %u: keyframe budget halved\n", tick);
            replayKeyBudget_ /= 2;
            replayKeyBytes_ = 0;   // of those held: the one that failed may be counted and not laid
            for (const auto& key : replayKeys_) replayKeyBytes_ += key.bytes;
            bool thinned = false;
            try { thinned = replayThinKeys(0); } catch (const std::bad_alloc&) {}
            if (!thinned) {
                if (replayKeys_.size() > 1) replayKeys_.erase(replayKeys_.begin() + 1, replayKeys_.end());
                replayKeyBytes_ = replayKeys_.empty() ? 0 : replayKeys_.front().bytes;
            }
        }
    }

    bool GameView::replayThinKeys(uint64_t incoming) {
        namespace rb = ta::replaybar;
        std::vector<uint64_t> bytes;
        bytes.reserve(replayKeys_.size());
        for (const auto& key : replayKeys_) bytes.push_back(key.bytes);
        rb::ThinPlan plan = rb::thinPlan(replayKeyTicks(), bytes, replayKeyBudget_, incoming,
                                         uint32_t(replayTick_), uint32_t(replayTick_));
        if (!plan.fits) return false;
        std::sort(plan.drop.begin(), plan.drop.end(), std::greater<size_t>());   // erase from the back
        for (const size_t drop : plan.drop) {
            replayKeyBytes_ -= replayKeys_[drop].bytes;
            replayKeys_.erase(replayKeys_.begin() + std::ptrdiff_t(drop));
        }
        return true;
    }

    void GameView::replayRestore(size_t keyframe) {
        const ReplayKeyframe& key = replayKeys_[keyframe];
        try {
            for (int attempt = 0;; ++attempt) {
                writableWorld().restoreFrom(*key.world);
                if (world_.stateHash() == key.hash) break;
                if (attempt) {
                    std::fprintf(stderr, "replay: DIVERGED restoring tick %u -- kept %016llx, restored "
                                 "%016llx\n", key.tick, (unsigned long long)key.hash,
                                 (unsigned long long)world_.stateHash());
                    replayKeyMismatch_ = true;
                    postNotice("REPLAY DIVERGED FROM THE RECORDING", 8);
                    break;
                }
            }
        } catch (const std::bad_alloc&) {
            // The world is half written and cannot be played on; the last frame stays up.
            std::fprintf(stderr, "replay: out of memory restoring tick %u\n", key.tick);
            replayHalted_ = true;
            replaySeekTo_.reset();
            replayInTransit_ = false;
            postNotice("NOT ENOUGH MEMORY TO REWIND", 8);
            return;
        }
        replayTick_ = key.tick;
        replayAccum_ = 0;
        replayKeyLast_ = key.bytes;   // the world is this size again: what the next copy will take
        replayCheckAt_ = ta::replaybar::checkAfter(replayChecks_, replayTick_);
        std::copy(key.playerAi.begin(), key.playerAi.end(), std::begin(playerAi_));
        worldEpoch_.fetch_add(1, std::memory_order_release);
        replayDisplayStale_ = true;
        replayPublish_ = true;
    }

    void GameView::replayJumpTo(size_t tick) {
        namespace rb = ta::replaybar;
        if (!replayMode_ || replayHalted_) return;
        tick = std::min(tick, replayBundles_.size());
        if (replayKeys_.empty()) {
            if (replayTick_ == 0) replayCaptureKeyframe();
            if (replayKeys_.empty()) return;
        }
        if (!replayInTransit_) replayJumpOwner_ = hudPlayer();
        replayInTransit_ = true;
        replaySeekTo_ = tick;
        const size_t keyframe = rb::keyframeAt(replayKeyTicks(), uint32_t(tick));
        if (tick < replayTick_ || replayKeys_[keyframe].tick > replayTick_) replayRestore(keyframe);
    }

    void GameView::replayNudge(int64_t ticks) {
        namespace rb = ta::replaybar;
        const size_t from = replaySeekTo_.value_or(replayTick_);
        const size_t want = rb::nudgeTarget(replayTick_, from, ticks, replayBundles_.size());
        replayJumpTo(ticks < 0 ? rb::snapRewind(replayKeyTicks(), uint32_t(want), uint32_t(from)) : want);
    }

    void GameView::replayHoldNudge(int direction) {
        // The next step waits for the last landing, so a hold never runs ahead of what the
        // game can simulate and keeps showing the units: every restore redraws them all.
        if (!replayInTransit_) replayNudge(int64_t(direction) * ta::replaybar::kStepTicks);
    }

    void GameView::replayTogglePause() {
        paused_ = !paused_;
        if (paused_ && replaySeekTo_) replaySeekTo_ = replayTick_;   // a jump stops where it stands
    }

    void GameView::replayDropQueued() {
        std::lock_guard<std::mutex> lock(hitQueueMutex_);
        hitQueue_.clear();missionSoundQueue_.clear();unitSoundQueue_.clear();
        transportEffectQueue_.clear();smokeTickQueue_.clear();
        flightAnimationQueue_.clear();weaponAnimationQueue_.clear();
        pendingHurt_.clear();
    }

    void GameView::replayResetDisplay() {
        // Everything the display built up belongs to another moment of the game. Unit ids
        // keep their meaning within a recording, so the selection, groups and tracking
        // stay; what each unit looked like does not.
        replayDropQueued();
        shakeTime_ = 0;
        anims_.clear();unitType_.clear();birthFx_.clear();
        beams_.clear();effects_.clear();explosionGlows_.clear();rings_.clear();particles_.clear();
        detachedPieces_.clear();bloodStains_.clear();nimbusEffects_.clear();
        pointParticles_.clear();damageFlames_.clear();smokeSprites_.clear();featureSmokeSprites_.clear();
        smokeOwners_.clear();featureSmokeOwners_.clear();
        effectVisibility_.reset();
        // Features the game added (corpses) go, and the map's own re-sync from the world.
        for (size_t i = featuresAtLoad_; i < features_.size(); ++i) featInstIds_.erase(features_[i].simId);
        if (features_.size() > featuresAtLoad_) features_.erase(features_.begin() + std::ptrdiff_t(featuresAtLoad_), features_.end());
        featureIndexDirty_ = true;
        featSimState_.clear();lastFeatGen_ = UINT32_MAX;
        registerEveryUnit_ = true;
    }

#ifndef NDEBUG
    void GameView::setReplaySeekScript(const std::string& ticks, bool straight) {
        std::stringstream list(ticks);
        std::string item;
        while (std::getline(list, item, ','))
            if (!item.empty()) {
                const size_t tick = size_t(std::strtoull(item.c_str(), nullptr, 10));
                if (straight) replaySeekStraight_.insert(tick);
                else replaySeekScript_.push_back(tick);
            }
        replaySeekPrinting_ = !straight;
    }

    void GameView::replaySeekPrint() const {
        std::fprintf(stderr, "replay seek: tick=%zu hash=%016llx\n", replayTick_,
                     (unsigned long long)world_.stateHash());
    }
#endif

    size_t GameView::aliveUnits() const {
        size_t n = 0; for (auto& u : world_.units()) if (u.alive() && u.type) ++n; return n;
    }

    // Build the end-of-game statistics table from the last render frame. Rows follow
    // slot order so the table reads the same for everyone in a multiplayer game; a
    // player still standing when the game ended is timed at the full match length.
    //
    // A watcher is on nobody's side, so its plate is the winner's: the winning team's
    // rows are the highlighted ones, the plate is the side of its first player still
    // standing (else of its first player), a line names the winner, and the match
    // length is the tick the result landed (a game its players played on past the
    // result is not timed at the moment the watcher left). With no winner the band
    // reads "Game Over". The plate is that winner's whatever its side: one that is
    // not in gamedata/sidedata.tdf gets the shared plate (kPlateFallback), said in
    // the log -- never the side_ the watcher's interface opened with, which is only
    // the first seat's.
    ta::ResultStats GameView::resultStats() const {
        ta::ResultStats st;
        const Frame& f = front();
        st.matchSec = int(f.gameTick / 30);
        if (spectating_ && outcomeTick_ != 0) st.matchSec = std::min(st.matchSec, int(outcomeTick_ / 30));
        if (!spectating_ && ta::sides::find(side_) >= 0) st.faction = ta::sides::find(side_);
        const int winner = spectating_ ? f.winningTeam : -1;
        int winners = 0;
        const PlayerR* plate = nullptr;   // the winner whose side the plate is
        std::string winnerName;
        for (int p = 0; p < f.numPlayers && p < int(f.players.size()); ++p) {
            if (resultParticipants_ && !(*resultParticipants_ & (1u<<p))) continue;
            const PlayerR& pr = f.players[size_t(p)];
            ta::ResultRow row;
            row.name = !playerName_[p & 7].empty() ? playerName_[p & 7]
                                                   : "PLAYER " + std::to_string(p + 1);
            row.colorSlot = colorSlot_[p & 7];
            row.side = pr.side;
            row.built = pr.built;
            // TotalA.exe 0x41dd86..0x41de10: the totals _ftol'd, the Score from the
            // kills and the clock at the end (econ::plateScore).
            auto ftol = [](double v) { return int32_t(uint32_t(int64_t(v))); };
            row.energyProduced = ftol(pr.produced[0]); row.metalProduced = ftol(pr.produced[1]);
            row.energyWasted = ftol(pr.wasted[0]); row.metalWasted = ftol(pr.wasted[1]);
            row.score = ta::sim::econ::plateScore(f.gameTick, int32_t(int16_t(pr.kills)), world_.scoreKillMul(),
                                                  world_.scoreTimeMul());
            row.kills = pr.kills;
            row.losses = pr.losses;
            row.defeated = pr.defeated;
            row.isLocal = (p == localPlayer_) && !spectating_;
            row.timeSec = pr.defeatedAt >= 0 ? int(pr.defeatedAt) : st.matchSec;
            if (winner >= 0 && pr.team == winner) {
                row.isLocal = true;
                if (!winners++) winnerName = row.name;
                // The plate: the first winner still standing, else the first winner.
                if (!plate || (plate->defeated && !pr.defeated)) plate = &pr;
            }
            st.rows.push_back(std::move(row));
        }
        if (spectating_) {
            if (winner < 0) st.bandWord = "Game Over";
            else st.watchLine = (winners == 1 ? winnerName : "Team " + std::to_string(winner + 1)) + " wins";
        }
        if (plate) {
            st.faction = ta::factionIndex(plate->side);
            if (st.faction < 0) {
                std::fprintf(stderr, "result: the winner's side '%s' is not in sidedata.tdf: showing the shared plate\n",
                             plate->side.c_str());
                st.faction = ta::kPlateFallback;
            }
        }
        return st;
    }

    void GameView::startAtMonarch() {
        const ta::sim::Unit* target=nullptr;
        for (const auto& u : world_.units()) {
            if (u.player!=localPlayer_ || !u.alive() || !u.type) continue;
            if (!target || u.type->commander) target=&u;
            if (u.type->commander) break;
        }
        if (target) {
            playerMonarchId_=target->id;builderId_=target->id;
            initialCamera_=std::pair{target->x.toFloat(),target->z.toFloat()};
        }
    }

    void GameView::lookAt(float x, float z) {
        initialCamera_.reset();
        follow_ = false;   // an explicit view (--follow Z --look X Z = a fixed camera at zoom Z)
        mapView_.setOffset(x - 640 / mapView_.zoom(), z - 400 / mapView_.zoom());
    }

    void GameView::setPlayerColor(int player, int slot) {
        if (player >= 0 && player < 8 && slot >= 0) colorSlot_[player] = slot;
    }

    void GameView::simStep(float dt) {
        int64_t _sim0 = int64_t(SDL_GetPerformanceCounter());
        // Advance the sim in sub-steps capped at 1/30s so fast speeds (or a laggy
        // frame) can't move a unit far enough to tunnel a wall; effects/AI below use
        // the full scaled dt (they only interpolate, so a big step is harmless).
        {
#ifndef NDEBUG
        PaceSpan worldSpan(paceWorldNs_);
#endif
        for (float rem = dt, guard = 0; rem > 1e-5f && guard < 16; ++guard) {
            float step = std::min(rem, 1.0f / 30.0f);
            writableWorld().tick(step);
            captureTransportEffects();
            rem -= step;
        }
        }
        profSimTicks_ += int64_t(SDL_GetPerformanceCounter()) - _sim0;
        // Scenario (.crt) "Display" actions: surface the sim runner's messages as
        // HUD notices for the viewing player. Drained on the sim thread (same as
        // scenario step, so no race on its queue); postNotice defers to main.
        if (auto* sc = writableWorld().scenario())
            for (auto& m : sc->drainMessages()) postNotice(m.text, 8);
#ifndef NDEBUG
        if (amphib_) {
            auto* t = world_.unit(transportId_);
            if (t && t->alive()) {
                if (amphibPhase_ == 0 && int(t->cargo.size()) >= amphibSquad_) {
                    writableWorld().unloadAt(transportId_, amphibSeaX_, amphibSeaZ_);
                    amphibPhase_ = 1;
                } else if (amphibPhase_ == 1 && t->cargo.empty()) {
                    for (auto& u : world_.units())
                        if (u.alive() && !u.embarked() && u.player == 0 && u.type &&
                            u.type->canMove && !u.type->canTransport)
                            writableWorld().order(u.id, amphibLandX_, amphibLandZ_, false);
                    amphibPhase_ = 2;
                }
            }
        }
#endif
        // Victory check: last team standing. The sim computes winningTeam() and
        // per-player defeated flags each tick (deterministic across peers); the
        // viewer just maps that to this player's win/lose banner. Only armed once
        // at least two teams have fielded units (staged demos may field one).
        const int previousOutcome = outcome_.load();
        if (outcome_ == 0 && !missionStem_.empty()) {
            // Campaign mission: the in-sim god script + the .ota victory/defeat
            // conditions decide the result (last-team-standing doesn't apply -- the
            // objective may be an escort, a timer, a kill-target, etc.).
            outcome_ = world_.missionOutcome();
        } else if (outcome_ == 0 && world_.hasScenarioOutcomes()) {
            outcome_ = world_.scenarioOutcome(spectating_ ? -1 : localPlayer_);
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
            } else if (!spectating_ && world_.player(localPlayer_).defeated && teamsSeen >= 2) {
                // A defeated player exits even when an allied player is still fighting.
                outcome_ = -1;
            }
        }
        // The result just landed: ask for the replay so far to be made durable. REQUEST,
        // not write -- simStep runs on the sim worker, while the net client appends to
        // its recorded bundles and hashes from the main thread. The main thread picks
        // this up (pumpReplayRecording). The recording itself goes on.
        if (previousOutcome == 0 && outcome_ != 0) {
            outcomeTick_.store(world_.tickCount(), std::memory_order_relaxed);
            replayResultSync_.store(true, std::memory_order_relaxed);
        }
#ifndef NDEBUG
        PaceSpan captureSpan(paceCaptureNs_);
#endif
        captureFrame();   // snapshot post-tick unit state for the render (poses + read fields)
    }

    void GameView::captureEffectVisibility() {
        if (noFog_) return;
        const auto& map=mapView_.map();
        if (!effectVisibility_ || effectVisibilityPlayer_!=localPlayer_ ||
            world_.tickCount()<=effectVisibilityTick_) {
            effectVisibilityPlayer_=localPlayer_;
            effectVisibility_=std::make_unique<ta::RetailEffectVisibility>(map.width/2,map.height/2,
                ta::sim::retailExplorationHeights(map.width,map.height,uint8_t(map.seaLevel),
                    [&](int x,int z) {return map.heights[size_t(z)*map.width+x];}));
        }
        effectVisibility_->expire(world_.tickCount());
        for (const auto& unit:world_.units()) {
            // Transport cargo keeps a live footprint (4f6a60 has no attachment test).
            // A passive neutral's sight is not the local player's (World::viewShareMask).
            const bool eligible=unit.type && unit.alive() && !unit.underConstruction &&
                                sharesSightToLocal(unit.player);
            effectVisibility_->track(unit.id,unit.player,localPlayer_,eligible,unit.alive(),
                                     unit.sightFootprint,world_.tickCount());
        }
        effectVisibilityTick_=world_.tickCount();
    }

    void GameView::captureTransportEffects() {
        captureEffectVisibility();
        SmokeTick smoke;
        smoke.tick=world_.tickCount();smoke.windX=world_.wind().x;smoke.windZ=world_.wind().z;
        if(smoke.tick<=smokeCaptureTick_) {smokeOwners_.clear();featureSmokeOwners_.clear();}
        smokeCaptureTick_=smoke.tick;
        for(auto it=smokeOwners_.begin();it!=smokeOwners_.end();) {
            const auto* owner=world_.unit(*it);
            if(!owner || ta::retailAttachedSfxOwnerRemoved(owner->alive(),owner->deadFor,
                    owner->corpseAnimationTicks())) {
                smoke.removedOwners.push_back(*it);it=smokeOwners_.erase(it);
            } else ++it;
        }
        for(auto it=featureSmokeOwners_.begin();it!=featureSmokeOwners_.end();) {
            const auto* feature=world_.feature(it->first);
            if(!feature || !feature->alive || !feature->burn ||
               feature->burnStarted!=it->second.burnStarted ||
               feature->burnSequence!=it->second.activationSequence) {
                smoke.removedFeatures.push_back(it->first);it=featureSmokeOwners_.erase(it);
            } else ++it;
        }
        for(const auto& feature:world_.features()) {
            const auto bodyAge=ta::retailFeatureSmokeAge(smoke.tick,feature.burnStarted);
            if(!feature.alive || !feature.burn || feature.type<0 || !bodyAge)continue;
            const auto& map=mapView_.map();
            const auto x=feature.x.v,z=feature.z.v;
            const int height=map.heights.empty() ? 0 : ta::sim::retailTerrainHeight(
                x,z,map.width,map.height,[&](int cx,int cz) {
                    return map.heights[size_t(cz)*map.width+size_t(cx)];
                });
            smoke.features.push_back({feature.id,world_.featureTypes()[size_t(feature.type)].name,
                {x,std::bit_cast<int32_t>(uint32_t(height)<<16),z},
                *bodyAge,feature.burnSequence,smoke.tick%3==0});
            featureSmokeOwners_[feature.id]={feature.burnStarted,feature.burnSequence};
        }
        ta::retailOrderFeatureSmokeNewestFirst(smoke.features);
        for(const auto& event:world_.scriptEmissions()) {
            if(!(event.code>=2 && event.code<=5) && event.code!=257 && event.code!=258 && event.code!=265 &&
               (event.code<260 || event.code>264))continue;
            const auto* owner=world_.unit(event.unitId);
            if(event.code>=2 && event.code<=5 && owner && !owner->type->canFly)continue;
            const bool transient=event.code==263 || event.code==264;
            if(owner && (transient || !ta::retailAttachedSfxOwnerRemoved(owner->alive(),
                    owner->deadFor,owner->corpseAnimationTicks())) &&
               localSeesLive(*owner)) {
                smoke.emissions.push_back(event);
                if(!transient)smokeOwners_.insert(event.unitId);
            }
        }
        // Which units hand a packet to the weapon or flight queue this tick (WE S5d). The
        // per-unit read is a pass over every Unit, so it runs on capturePool_. Both queues
        // drop a packet that carries nothing new (push: no callback and no shot; capture:
        // the flyer's last recorded state), so only the changed ones are flagged. The
        // flight test reads the queue's last states, which only the capture below writes;
        // the render's drain() touches the pending list alone, so it may run meanwhile.
        transportFlags_.resize(world_.units().size());
        forEachUnitSlot([&](size_t i) {
            const auto& unit=world_.units()[i];
            const bool flyer=unit.alive() && unit.type && unit.type->canFly &&
                flightAnimationQueue_.changed(unit.id,unit.flightGroundMode==2,
                    unit.flightBeginCallbackSerial,unit.flightLandingCallbackSerial);
            transportFlags_[i]=uint8_t((unit.weaponAnimations.count || unit.firedWeapons ? 1 : 0) |
                (flyer ? 2 : 0));
        });
        {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            smokeTickQueue_.push_back(std::move(smoke));
            // Capture every simulation step, including steps whose render
            // snapshot will be superseded before the next display frame.
            // Only units with something to hand over are visited, in slot order (the
            // queues' order is unchanged); the scan that finds them is the parallel part.
            const auto& units=world_.units();
            for (size_t i=0;i<units.size();++i) {
                if (!transportFlags_[i]) continue;
                const auto& unit=units[i];
                if (transportFlags_[i]&1)
                    weaponAnimationQueue_.push(world_.tickCount(),unit.id,unit.weaponAnimations,
                        unit.firedWeapons,unit.x.toFloat(),unit.z.toFloat());
                if (transportFlags_[i]&2)
                    flightAnimationQueue_.capture(world_.tickCount(),unit.id,unit.flightGroundMode==2,
                        unit.flightBeginCallbackSerial,unit.flightLandingCallbackSerial);
            }
        }
        if (!world_.transportEffects().empty()) {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            for (const auto& event:world_.transportEffects()) {
                if (transportEffectQueue_.size() >= kMaxPendingHits) transportEffectQueue_.pop_front();
                transportEffectQueue_.push_back(event);
            }
        }
        if (!world_.scriptSounds().empty()) {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            for (const auto& sound:world_.scriptSounds()) {
                const auto* script=sound.type ? sound.type->script() : nullptr;
                if (!script) continue;
                if (unitSoundQueue_.size() >= kMaxPendingHits) unitSoundQueue_.pop_front();
                unitSoundQueue_.push_back({sound.tick,sound.unitId,sound.player,sound.x.toFloat(),
                    sound.z.toFloat(),sound.flags,script->name(uint32_t(sound.name)),
                    {sound.x.v,sound.y.v,sound.z.v},sound.hidden,sound.type});
            }
        }
    }

    // One unit's render record (WE S5b: runs on capturePool_, so it may touch only its own
    // slot of fb.units / captureSlots_ and const reads of world_ and pf). The ID table, the
    // live list and the conjure-site lookup are left to captureFrame's serial tail:
    // World::unit may repair its ID table on a miss, so it is not a safe concurrent read.
    void GameView::captureUnit(Frame& fb, const Frame& pf, size_t slot) {
        const auto& u=world_.units()[slot];
        UnitR& s=fb.units[slot];
        if(s.id!=u.id)s=UnitR{};
        const int conjureSiteId=u.buildSiteId ? u.buildSiteId : u.productionSiteId;
        captureSlots_[slot]={u.id,conjureSiteId,u.type!=nullptr,false,u.type && u.type->canFly};
        if (!u.type) { s.seeded = false; s.type = nullptr; return; }
        s.conjuring=false;   // the serial tail sets it for the few units that have a site
        // Render-read fields, captured for ALL units (alive + dead-recent: the death
        // animation and the deadFor>=4 cull both need a live value).
        s.gen = fb.gen;
        s.id = u.id; s.type = u.type; s.player = u.player;
        if (s.scenarioName != u.scenarioName) s.scenarioName = u.scenarioName;
        s.hp = u.hp.toFloat(); s.mana = 0; s.veteran = u.veteran; s.kills = u.kills;
        s.deadFor = u.deadFor < 0 ? -1.0f : float(u.deadFor) / 30.0f;   // ticks -> seconds
        s.inTransport = u.inTransport; s.padPiece = u.padPiece; s.squad = u.squad; s.stance = u.stance; s.outputStance = u.outputStance;
        s.standingOrder = u.standingOrder;
        s.weaponSlot = u.weaponSlot;
        for(size_t w=0;w<s.weaponReloads.size();++w)
            s.weaponReloads[w]=u.reloads[w];
        s.underConstruction = u.underConstruction; s.buildBegun = u.buildBegun;
        s.selectable = u.selectable;
        s.replacementModel=nullptr;s.replacementOpacity=-1;
        s.cloaked = u.cloaked; s.cloakOn = u.cloakOn; s.active = u.active;
        // The sim counts these in TICKS now (retail's representation); the HUD
        // wants seconds, so the conversion happens here, at the render boundary.
        s.paralyzedFor = float(u.paralyzedFor) / 30.0f;
        s.selfDestructT = ta::client::selfDestructSeconds(u.selfDestructT);
        s.hasConstructionEmitter=bool(u.constructionEmitter || u.cosmeticConstructionEmitter);
        s.constructionEmissions=u.constructionEmissions;
        if (u.constructionEmitter) s.constructionParticles=u.constructionEmitter->particles;
        else if (u.cosmeticConstructionEmitter) s.constructionParticles=u.cosmeticConstructionEmitter->particles;
        else s.constructionParticles.clear();
        s.buildSiteId = u.buildSiteId; s.productionSiteId = u.productionSiteId;
        s.reclaimId = u.reclaimId; s.repairId = u.repairId;
        s.yardOpen = world_.scriptYardOpen(u.id);
        s.scriptHealthPercent = int32_t(int16_t(u.hp.floorInt()))*100/std::max(u.maximumHp(),1);
        s.constructionPercentLeft = u.retailSite
            ? int(ta::sim::retailConstructionPercent(u.retailSite->progress.remaining))
            : int(u.underConstruction);
        s.moverMoving = u.speed.v != 0;
        s.buildRemaining = u.underConstruction ? 1.0f - u.constructionFraction() : 0.0f;
        s.buildProgress = (float(u.buildProgress) + float(u.buildFraction) / 65536.0f) / 30.0f;   // ticks -> seconds
        // Orders/rally are read for the VIEWED player's units only (the order
        // trails, queued-build ghosts and the HUD all gate on hudPlayer()); a watcher can
        // put any player on show, so it copies them all, as the buildQueue below does.
        // Copying them for every unit was the bulk of the capture cost at 10k units. Queues are tiny and only factories hold
        // one, so an ally's stay visible for the HUD -- and every player's for a spectator
        // or replay viewer, whose info panel shows any selected factory's queue.
        if (u.player==localPlayer_ || (watchOnly() && !benchmarkMode_)) {
            s.captureOrders(u);
            // The unit's own spot (destination markers, drawUnitSpots): one navigator
            // lookup, and only for a unit with orders, read like legionSwapTick below.
            ta::sim::Fixed spotX, spotZ;
            s.hasSpot = !s.orders.empty() && u.alive() && world_.legionDestination(u.id, spotX, spotZ);
            if (s.hasSpot) { s.spotX = spotX.toFloat(); s.spotZ = spotZ.toFloat(); }
        }
        else s.clearOrders();
        if (u.player==localPlayer_ || alliedToLocal(u.player) || spectating_ || replayMode_) s.buildQueue = u.buildQueue;
        else s.buildQueue.clear();
        s.cargo = u.cargo; s.repeatType = u.repeatType;
        s.captureMovement(u);
        s.corpseAnimationTicks=u.corpseAnimationTicks();
        s.corpsePhase = !u.alive() && u.deadFor < u.corpseUntil &&
                        u.deadFor >= u.corpseAnimationTicks();
        s.deathType = u.deathType;
        s.severity = u.severity;
        s.corpseFeat = world_.corpseTypeOf(u.type);
        s.corpseReclaimable=false;
        s.corpseFading=u.corpseFading;
        // A water grave sinks to its sea bed (4959c0); the body is drawn at
        // its feature's height.
        if (const auto* sink=u.alive() ? nullptr : world_.corpseSink(u.id)) {
            s.corpseSinking=true;s.corpseSinkY=float(sink->y)/65536.0f;
            // The record's own fade (+0x2f) is drawn: the feature draw hands
            // +0x2f and the alpha to the model instance (4fd844..4fd85f), and
            // the model draw blends the body at ftol(alpha * 255.0f) (4ed04a).
            s.corpseSinkAlpha=sink->fading ? ta::sim::retailFadeByte(sink->alpha) : uint8_t(255);
        } else {s.corpseSinking=false;s.corpseSinkAlpha=255;}
        // A decayed body's last 60 updates (+0x40 counting, 495af6..): retail
        // lowers the model's root by 0x2000 (1/8 px) each update, 7.5 px in
        // all, then takes it off the plane. Drawn as that sink plus a fade,
        // so it does not pop at the end; land and water alike (water graves
        // decay too here, World::updateCorpseWindow).
        const int32_t corpseLeft=u.corpseUntil-u.deadFor;
        const bool decaying=!u.alive() && !u.corpseFading && u.corpseUntil!=INT32_MAX && corpseLeft<60;
        s.corpseDecayAlpha=decaying ? uint8_t(std::max(0,corpseLeft)*255/60) : uint8_t(255);
        s.corpseDecaySink=decaying ? float(60-std::max(0,corpseLeft))*0.125f : 0.0f;
        if (!u.alive() && s.corpseFeat>=0 && size_t(s.corpseFeat)<world_.featureTypes().size()) {
            const auto& corpse=world_.featureTypes()[size_t(s.corpseFeat)];
            s.corpseReclaimable=corpse.reclaimable && !u.corpseFading;
            s.corpseCellX=ta::sim::footprintOrigin(u.x,u.type->footX)+u.type->corpseAdjX;
            s.corpseCellZ=ta::sim::footprintOrigin(u.z,u.type->footZ)+u.type->corpseAdjZ;
            s.corpseFootX=corpse.fx;s.corpseFootZ=corpse.fz;
        }

        // UnitR::speed is documented px/s and consumers (the flyer altitude servo,
        // the MotionControl percentage) rely on that; the sim keeps px/TICK now.
        s.justBuilt = u.justBuilt;
        s.disco = u.alive() && world_.discoTarget(u.player, u.id);
        s.headbang = u.alive() && world_.headbangTarget(u.player, u.id);
        s.sightShared = sharesSightToLocal(u.player);
        // The local view's sight of another side's unit, on the plane captureFog
        // just built (a dying or dead unit is tested where it lies).
        s.losSeen = s.mapSeen = false;
        if (!noFog_ && !s.sightShared && !fb.fog.empty()) {
            namespace rf = ta::retailfog;
            const bool hidden = u.cloaked || u.embarked();
            const int32_t y = (u.type->canFly ? u.flightY : u.groundY).v;
            s.losSeen = rf::unitInView(fb.fog, fb.fogW, fb.fogH, hidden, u.x.v, y, u.z.v,
                                       u.type->footX, u.type->footZ, u.type->modelTop);
            s.mapSeen = !hidden && (rf::pointInView(fb.fog, fb.fogW, fb.fogH, u.x.v, y, u.z.v) ||
                                    fogRadar_.covers(u.x.v, u.z.v));
        }
        // Pose: prev comes from the previously-published frame's curr for this SAME unit
        // (id live last tick + matching type). Interpolate alive units between ticks;
        // a dead unit holds its death pose. A big jump (teleport / id reuse) seeds fresh.
        // Another epoch's frame is another world: a jump's last frame shows where a unit
        // will be, not where it was.
        const UnitR* prev=fb.worldEpoch==pf.worldEpoch ? pf.unit(u.id) : nullptr;
        if(prev && prev->type!=u.type)prev=nullptr;
        s.headingWord=u.heading.v;
        s.captureOccupancy(u,world_.mapSea(),prev ? prev->animationOccupancy : 0);
        // Legion walkers pace their gait by the distance actually covered (gaitpace.h).
        // Flyers keep their flight scripts; Retail keeps retail's full-pace cycle.
        s.gaitRate=1;
        if (prev && !gaitPaceOff_ && u.alive() && !u.type->canFly && !u.type->isStructure() &&
            ta::sim::isLegionPathfinding(world_.pathfindingMode()) &&
            std::abs(u.x.toFloat()-prev->x)<=200.0f && std::abs(u.z.toFloat()-prev->z)<=200.0f) {
            const auto maximum=u.baseSpeed*(u.groundTerrainFlags&0x800 ? u.type->roadMult :
                u.groundTerrainFlags&0x1000 ? u.type->waterMult : ta::sim::Fixed::fromInt(1));
            s.gaitRate=ta::legionGaitRate(prev->gaitRate,
                int64_t(s.worldPosition[0])-prev->worldPosition[0],
                int64_t(s.worldPosition[2])-prev->worldPosition[2],
                uint32_t(world_.tickCount()-pf.gameTick),maximum.v,s.animationSpeedPercent()>0);
        }
        s.captureMoveRate(u,prev ? std::bit_cast<int16_t>(uint16_t(u.heading.v-prev->headingWord)) : 0);
        const auto multiplier=u.groundTerrainFlags&0x800 ? u.type->roadMult :
            u.groundTerrainFlags&0x1000 ? u.type->waterMult : ta::sim::Fixed::fromInt(1);
        s.turnSpeedPercent=prev ? ta::sim::retailTurnAnimationPercent(
            std::bit_cast<int16_t>(uint16_t(u.heading.v-prev->headingWord)),
            uint16_t(u.type->turnRate),uint16_t(u.type->turnInPlaceRate),
            multiplier.v,u.embarked()) : 0;
        if (u.alive() && prev &&
            std::abs(u.x.toFloat() - prev->x) <= 200.0f && std::abs(u.z.toFloat() - prev->z) <= 200.0f) {
            s.px = prev->x; s.pz = prev->z; s.ph = prev->heading;   // UnitR already holds radians
            s.x = u.x.toFloat(); s.z = u.z.toFloat(); s.heading = ta::sim::radiansFromBam(u.heading);   // render boundary: radians
            s.seeded = true;
            s.turnReqBam = u.turnReqBam;   // sim's requested turn this tick (TurnDirection)
        } else {
            s.x = u.x.toFloat(); s.z = u.z.toFloat(); s.heading = ta::sim::radiansFromBam(u.heading);   // render boundary: radians
            s.px = s.x; s.pz = s.z; s.ph = s.heading;
            s.turnReqBam = u.turnReqBam;
            s.seeded = u.alive();   // dead holds its pose (never interpolated)
        }
        // Legion allies swap places in one tick; the display plays it out over a few (swapshow.h).
        s.swap = {}; s.pox = s.poz = 0;
        if (s.seeded && prev && !u.type->canFly && !u.type->isStructure() &&
            ta::sim::isLegionPathfinding(world_.pathfindingMode())) {
            const uint32_t ticks = uint32_t(world_.tickCount() - pf.gameTick);
            const uint32_t swapped = world_.legionSwapTick(u.id);   // stamped after the counter advanced
            s.swap = ta::swapShowStep(prev->swap, s.x - s.px, s.z - s.pz, ticks,
                                       swapped != UINT32_MAX && swapped > pf.gameTick);
            s.pox = prev->swap.ox; s.poz = prev->swap.oz;
        }
        captureSlots_[slot].x = s.x; captureSlots_[slot].z = s.z;   // Frame::unitGrid
        captureSlots_[slot].corpseFoot = std::max(s.corpseFootX, s.corpseFootZ);   // Frame::maxCorpseFoot
        captureSlots_[slot].step = std::max({std::fabs(s.x - s.px - s.pox), std::fabs(s.z - s.pz - s.poz),
                                             std::fabs(s.swap.ox), std::fabs(s.swap.oz)});   // Frame::maxInterpStep
        captureSlots_[slot].buildGhosts = std::any_of(s.orders.begin(), s.orders.end(),
            [](const RenderOrder& o) { return bool(o.buildType); });   // Frame::buildGhostUnits
    }

    void GameView::captureFrame() {
        // Pick the oldest free ring slot: never the published one (the render may pin it on
        // its next frame), the one the render is pinned on, or the one the presentation clock
        // showed last frame. With twelve slots at least nine are free.
        int w = -1;
        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            for (int i = 0; i < kFrameRing; ++i)
                if (i != published_ && i != reading_ && i != shown_ &&
                    (w < 0 || frameBuf_[i].gen < frameBuf_[w].gen)) w = i;
            writing_ = w;
        }
        // Compact snapshots follow current records, not every lifetime unit ID.
        // The previous published buffer supplies same-ID interpolation poses.
        Frame& fb = frameBuf_[w];                // write buffer
        const Frame& pf = frameBuf_[published_]; // previously-published frame (last tick's poses)
        fb.gen = ++captureCounter_;     // records written this pass get gen==fb.gen (=> live this tick)
        fb.worldEpoch = worldEpoch_.load(std::memory_order_acquire);
        captureFog(fb);   // the local display plane, before the units are tested against it
        fb.live.clear();
        const size_t count=world_.units().size();
        fb.units.resize(count);
        captureSlots_.resize(count);
        // The per-unit copy is memory-latency bound (first touch of each Unit), so it
        // splits across capturePool_ near-linearly (forEachUnitSlot).
        forEachUnitSlot([&](size_t i) {captureUnit(fb, pf, i);});
        // Serial tail, in slot order (the same order as before): the ID table, the compact
        // live list, and the conjure flag for the few units that have a site.
        captureGridX_.clear(); captureGridZ_.clear(); fb.buildGhostUnits.clear(); fb.flyerUnits.clear();
        fb.maxCorpseFoot = 0; fb.maxInterpStep = 0;
        for (size_t i=0; i<count; ++i) {
            const CaptureSlot& c=captureSlots_[i];
            if(fb.unitSlots.size()<=size_t(c.id))fb.unitSlots.resize(size_t(c.id)+1,-1);
            fb.unitSlots[size_t(c.id)]=int32_t(i);
            if (!c.typed) continue;
            UnitR& s=fb.units[i];
            fb.live.push_back(&s);   // compact live list (mirrors world_.units())
            if (c.buildGhosts) fb.buildGhostUnits.push_back(uint32_t(captureGridX_.size()));
            if (c.flyer) fb.flyerUnits.push_back(uint32_t(captureGridX_.size()));
            fb.maxCorpseFoot = std::max(fb.maxCorpseFoot, c.corpseFoot);
            fb.maxInterpStep = std::max(fb.maxInterpStep, c.step);
            captureGridX_.push_back(c.x); captureGridZ_.push_back(c.z);
            // Zero means no site (captureUnit already cleared the flag). Only the few
            // units with one are touched here: a pool thread just wrote every record,
            // so a store to each would pull all of them back across cores.
            if (!c.site) continue;
            const auto* conjureSite=world_.unit(c.site);
            s.conjuring=conjureSite && conjureSite->underConstruction && conjureSite->buildBegun;
        }
        {
            // The renderer's unit broad phase (workstream L): built here, once per snapshot,
            // from the positions just copied (compact, still in this thread's cache).
            const auto gridStart=std::chrono::steady_clock::now();
            fb.unitGrid.build(captureGridX_, captureGridZ_, captureGridCells_);
            fb.unitGridBuildNs=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now()-gridStart).count());
        }
        if (gaitLog_) {
            for (const UnitR* r:fb.live) {
                if (!r->alive() || !r->type || r->type->canFly || r->type->isStructure()) continue;
                const UnitR* p=pf.unit(r->id);
                if (!p || p->type!=r->type) continue;
                const auto* u=world_.unit(r->id);
                const auto maximum=u->baseSpeed*(u->groundTerrainFlags&0x800 ? u->type->roadMult :
                    u->groundTerrainFlags&0x1000 ? u->type->waterMult : ta::sim::Fixed::fromInt(1));
                const double dx=double(r->worldPosition[0]-p->worldPosition[0])/65536.0;
                const double dz=double(r->worldPosition[2]-p->worldPosition[2])/65536.0;
                std::fprintf(gaitLog_,"%u,%d,%s,%.1f,%.1f,%.4f,%.4f,%d,%d,%.4f\n",world_.tickCount(),r->id,
                    r->type->id.c_str(),r->x,r->z,std::sqrt(dx*dx+dz*dz),maximum.toFloat(),
                    r->animationSpeedPercent(),int(r->movementRefused),r->gaitRate);
            }
            std::fflush(gaitLog_);
        }
        // Player table snapshot for the HUD/scoreboard.
        fb.numPlayers = world_.numPlayers();
        for (int p = 0; p < fb.numPlayers && p < int(fb.players.size()); ++p) {
            const auto& pl = world_.player(p);
            PlayerR& r = fb.players[size_t(p)];
            r.captureEconomy(pl);
            {
                r.side = pf.players[size_t(p)].side;
                if (r.side.empty()) for (const auto& unit : world_.units())
                    if (unit.player == p && unit.type && !unit.type->side.empty()) {
                        r.side = unit.type->side;
                        break;
                    }
            }
            r.manaShareMask = pl.manaShareMask;
            r.kills = pl.kills; r.unitCount = pl.unitCount; r.combatEvents = pl.combatEvents;
            r.built = pl.built; r.losses = pl.losses; r.score = pl.score;
            // sim keeps these in TICKS now; the scoreboard wants seconds.
            r.defeatedAt = pl.defeatedAt < 0 ? -1.0f : float(pl.defeatedAt) / 30.0f;
            r.team = pl.team; r.defeated = pl.defeated;
            r.emoteSequence = pl.emoteSequence;
            r.emoteDisco = pl.emoteDisco;
        }
        fb.flames=world_.flames();
        fb.projectiles = world_.projectiles();   // sim push_back/erase each tick -> must copy
        fb.hits = world_.hits();                  // weapon impacts this tick (cleared next tick)
        // ...and queue them for the render as well, so impacts survive a skipped
        // snapshot (see hitQueue_). Oldest out first when the renderer is so far
        // behind that the queue fills.
        if (!world_.recordHurt().empty()) {
            std::lock_guard<std::mutex> hq(hitQueueMutex_);
            for (int id : world_.recordHurt())
                if (pendingHurt_.size() < 256) pendingHurt_.push_back({world_.tickCount(), id});
        }
        if (!fb.hits.empty()) {
            std::lock_guard<std::mutex> hq(hitQueueMutex_);
            for (const auto& h : fb.hits) {
                for (int id : h.hurtUnits)
                    if (pendingHurt_.size() < 256) pendingHurt_.push_back({world_.tickCount(), id});
                if (hitQueue_.size() >= kMaxPendingHits) hitQueue_.pop_front();
                hitQueue_.push_back({world_.tickCount(), h});
            }
        }
        fb.shakeReq = world_.shakeRequest();       // copied under the worker's lock
        {
            auto sounds=writableWorld().takeSoundRequests();
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            for(auto& sound:sounds) {
                if(missionSoundQueue_.size()>=256)missionSoundQueue_.pop_front();
                missionSoundQueue_.push_back(std::move(sound));
            }
        }
        fb.winningTeam = world_.winningTeam();
        fb.gameTick = world_.tickCount();
        fb.benchEnded = world_.benchmarkEnded();
        fb.scenarioClock = writableWorld().scenario() && writableWorld().scenario()->showClock();
        fb.wind = world_.wind();
        if (effectVisibility_ && !noFog_) {
            const auto counts=effectVisibility_->counts();
            fb.effectVisibility.assign(counts.begin(),counts.end());
            fb.effectVisW=mapView_.map().width/2;fb.effectVisH=mapView_.map().height/2;
        } else fb.effectVisibility.clear();
        fb.tickMs = SDL_GetTicks64();
        fb.tickDurMs = (1000.0f / 30.0f) / std::max(0.1f, animSpeed());
        fb.pubCounter = SDL_GetPerformanceCounter();
        // Publish: the render's next frame may pin this slot as front().
        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            published_ = w;
            writing_ = -1;
        }
    }

    void GameView::interpPose(const UnitR& s, float& x, float& z, float& heading) const {
        x = s.x + s.swap.ox; z = s.z + s.swap.oz; heading = s.heading;
        if (settings_ && settings_->smoothMotion && s.seeded) {
            float a = interpAlpha_;
            x = s.px + s.pox + (x - s.px - s.pox) * a;
            z = s.pz + s.poz + (z - s.pz - s.poz) * a;
            float dh = s.heading - s.ph;
            while (dh >  3.14159265f) dh -= 6.28318531f;   // shortest-path turn
            while (dh < -3.14159265f) dh += 6.28318531f;
            heading = s.ph + dh * a;
        }
        // A flyer on a repair pad rides its pad piece: every mover tick the
        // attached branch (0x43d6d0) copies the piece's position (0x43e060, the
        // sim's unitPiecePoint) and sets its own orientation to the pad's plus
        // that piece's own turn angles (0x43d78e -> 0x43e180: unit +0x64/+0x66/
        // +0x68 = the carrier's + the piece's). The heading is the visible part
        // (a building has no pitch or roll; pad pieces turn about y).
        if (s.padPiece >= 0 && s.inTransport) {
            const UnitR* pad = frameUnitP(s.inTransport);
            if (pad && pad->padPiece < 0) {
                float px, pz, ph;
                interpPose(*pad, px, pz, ph);
                float turn = 0;
                if (const auto at = anims_.find(pad->id); at != anims_.end() && at->second.vm) {
                    const auto& pieces = at->second.vm->pieces();
                    if (size_t(s.padPiece) < pieces.size()) turn = pieces[size_t(s.padPiece)].rot[1];
                }
                // The piece turn enters the model as -rot (scriptRot) under the
                // body's -heading, so the rider's heading is the pad's plus rot.
                heading = ph + turn;
            }
        }
    }

    const UnitR& GameView::frameUnit(int id) const {
        static const UnitR kEmpty{};
        const auto* u=front().unit(id);
        return u ? *u : kEmpty;
    }

    const UnitR* GameView::frameUnitP(int id) const {
        return front().unit(id);
    }

    const PlayerR& GameView::framePlayer(int p) const {
        static const PlayerR kEmpty{};
        const Frame& f = front();
        return (p >= 0 && p < int(f.players.size())) ? f.players[size_t(p)] : kEmpty;
    }

    void GameView::playUnitSound(const std::string& name, int32_t flags, float x, float z, bool visible, bool selected) {
        if (!ta::retailUnitSoundAudible(flags,visible,selected)) return;
        const int priority=ta::retailSoundPriority(flags);
        if (priority==7) sounds_.play(name,1.f,1.f,priority,(flags&32)!=0);
        else sounds_.playWorld(name,x,z,priority);
    }

    float GameView::groundProjectedZ(float x, float z) const {
        const auto& m = mapView_.map();
        int height = 0;
        if (!m.heights.empty() && m.width > 0 && m.height > 0)
            height = m.heights[size_t(std::clamp(int(z) / 16, 0, m.height - 1)) * size_t(m.width) +
                               size_t(std::clamp(int(x) / 16, 0, m.width - 1))];
        return std::floor(z) - float(height >> 1);
    }

    uint8_t GameView::fogCellR(float x, float z) const {
        // No fog (spectator, replay, FULL VISION, a lineofsight=0 mission) sees every
        // cell, whatever plane a frame captured before the view stopped fogging.
        if (noFog_) return ta::retailfog::kInView;
        return projectedFogCellR(x, groundProjectedZ(x, z));
    }

    uint8_t GameView::projectedFogCellR(float x, float projectedZ) const {
        if (noFog_) return ta::retailfog::kInView;
        const Frame& f = front();
        if (f.fog.empty()) return ta::retailfog::kInView;   // no terrain: nothing to fog
        const int cx = int(std::floor(x)) >> 5, cz = int(std::floor(projectedZ)) >> 5;
        if (uint32_t(cx) >= uint32_t(f.fogW) || uint32_t(cz) >= uint32_t(f.fogH))
            return ta::retailfog::kUnexplored;
        return f.fog[size_t(cz) * size_t(f.fogW) + size_t(cx)];
    }

    bool GameView::localSeesLive(const ta::sim::Unit& u) const {
        // The players whose sight the local plane holds (World::viewShareMask):
        // an allied passive neutral's units and eyes count only through them.
        if (noFog_ || sharesSightToLocal(u.player)) return true;
        for (int p = 0; p < world_.numPlayers(); ++p)
            if (sharesSightToLocal(p) && world_.playerSees(p, u)) return true;
        return false;
    }

    // The local display plane for the frame being captured (sim thread, after the
    // tick): the World's view counts of the local player and its allies, its
    // Mapping bits, the room's and the power codes' vision rules. Also collects
    // the local player's radar for the minimap dots.
    void GameView::captureFog(Frame& fb) {
        namespace rf = ta::retailfog;
        fogRadar_.clear();
        if (noFog_ || world_.viewPlaneW() <= 0 || world_.viewPlaneH() <= 0) {
            fb.fog.clear(); fb.fogW = fb.fogH = 0;
            return;
        }
        const int w = world_.viewPlaneW(), h = world_.viewPlaneH();
        rf::PlaneSource source;
        source.width = w; source.height = h; source.player = localPlayer_;
        source.mapRevealed = world_.mapRevealedFor(localPlayer_);
        source.losOff = world_.roomLosOff() || world_.powerLosOff(localPlayer_);
        // The planes the local player shares (World::viewShareMask: every ally
        // but a passive neutral), so the fog is the plane its units pick from.
        source.viewers = world_.viewShareMask(localPlayer_);
        source.viewCounts = world_.viewCounts();
        source.explored = world_.navigationExploration();
        if (world_.viewCounts().empty()) {
            // No pass yet (before tick 0): stamp the local side's footprints the way
            // World::updateViewCounts will, into one plane of our own.
            const size_t cells = size_t(w) * size_t(h);
            const auto& map = mapView_.map();
            if (fogPreviewHeights_.size() != cells && map.width == 2 * w && map.height == 2 * h)
                fogPreviewHeights_ = ta::sim::retailExplorationHeights(map.width, map.height,
                    uint8_t(world_.mapSea()), [&](int x, int z) { return map.heights[size_t(z) * map.width + x]; });
            fogPreviewCounts_.assign(cells, 0);
            fogPreviewExplored_.assign(cells, 0);
            if (fogPreviewHeights_.size() == cells)
                for (const auto& u : world_.units()) {
                    if (!u.alive() || !u.type || u.underConstruction || !sharesSightToLocal(u.player)) continue;
                    ta::sim::RetailSightFootprint sight;
                    sight.distance = int16_t(world_.sightDistance(*u.type));
                    sight.sightHeight = u.type->sightHeight;
                    ta::sim::retailUpdateSight(sight, u.x, u.type->canFly ? u.flightY : u.groundY, u.z,
                        uint8_t(world_.mapSea()), false, w, h, 0,
                        [&](int x, int z) { return fogPreviewHeights_[size_t(z) * w + x]; },
                        [&](int x, int z, int delta, uint16_t) {
                            fogPreviewCounts_[size_t(z) * w + x] = uint16_t(fogPreviewCounts_[size_t(z) * w + x] + delta);
                            if (delta > 0) fogPreviewExplored_[size_t(z) * w + x] = 1;
                        });
                }
            source.viewCounts = fogPreviewCounts_;
            source.explored = fogPreviewExplored_;
            source.viewers = 1; source.player = 0;
        }
        // Only the squares the World marked since the last capture can differ, unless
        // the rules or the planes themselves changed (or there is no World pass yet).
        const auto& dirty = world_.viewDirty();
        const int dw = world_.viewDirtyW(), shift = ta::sim::World::kViewDirtyShift;
        const FogKey key{source.viewCounts.data(), source.explored.data(), w, h, source.viewers,
                         source.player, source.mapRevealed, source.losOff};
        bool changed = false;
        if (key != fogKey_ || fogPlane_.size() != size_t(w) * size_t(h) || dw <= 0 || debugFogOverride_ ||
            world_.viewCounts().empty()) {
            changed = rf::buildPlane(source, fogPlane_) || key != fogKey_;
        } else {
            for (size_t b = 0; b < dirty.size(); ++b)
                if (dirty[b]) {
                    const int bx = int(b % size_t(dw)), bz = int(b / size_t(dw));
                    changed = rf::buildPlane(source, fogPlane_, bx << shift, bz << shift,
                                             (bx + 1) << shift, (bz + 1) << shift) || changed;
                }
#ifndef NDEBUG
            static const bool verify = ta::devFlag("TA_FOG_VERIFY");
            if (verify) {   // the squares must have caught every change
                std::vector<uint8_t> whole;
                rf::buildPlane(source, whole);
                if (whole != fogPlane_)
                    std::fprintf(stderr, "TA_FOG_VERIFY tick %u: the incremental plane differs\n", world_.tickCount());
            }
#endif
        }
        fogKey_ = key;
        if (!world_.viewCounts().empty()) writableWorld().clearViewDirty();
        if (changed) ++fogPlaneGen_;
        if (debugFogOverride_) { debugFogOverride_(fogPlane_, w, h); ++fogPlaneGen_; }
        if (fb.fogGen != fogPlaneGen_ || fb.fog.size() != fogPlane_.size()) {
            fb.fog = fogPlane_;
            fb.fogGen = fogPlaneGen_;
        }
        fb.fogW = w; fb.fogH = h;
        // 0x4f9970: the local player's own units with a radar distance, built
        // (the same units 4f6a40 stamps sight for).
        for (const auto& u : world_.units())
            if (u.player == localPlayer_ && u.alive() && u.type && !u.underConstruction)
                fogRadar_.add(u.x.v, u.z.v, world_.radarDistance(*u.type));
        fogRadar_.finish();
    }

    bool GameView::effectVisibleR(const std::array<int32_t,3>& position) const {
        if (noFog_) return true;
        const auto& frame=front();
        if (frame.effectVisibility.empty()) return false;
        return ta::RetailEffectVisibility::visible(frame.effectVisibility,
            frame.effectVisW,frame.effectVisH,position);
    }

    bool GameView::unitSoundVisibleR(int player,bool hidden,const ta::sim::UnitType* type,
                                     const std::array<int32_t,3>& position) const {
        if (noFog_) return true;
        const auto& frame=front();
        if (frame.effectVisibility.empty() || !type) return false;
        return ta::RetailEffectVisibility::unitSoundSeen(frame.effectVisibility,frame.effectVisW,
            frame.effectVisH,player==localPlayer_,hidden,position,type->footX,type->footZ,type->modelTop);
    }

    int GameView::frameQueuedCount(int builderId, const ta::sim::UnitType* type) const {
        const UnitR* b = frameUnitP(builderId);
        if (!b || !type) return 0;
        int n = 0;
        for (const auto* q : b->buildQueue) if (q == type) ++n;
        return n;
    }

    // The self-destruct notice reads the longest countdown left among the
    // units the press armed, as the sim runs it down (UnitR::selfDestructT).
    void GameView::updateSelfDestructNotice(float dt) {
        if (selfDestructWatch_.empty()) return;
        float left = -1.0f;
        for (int id : selfDestructWatch_)
            if (const auto* su = frameUnitP(id); su && su->alive() && su->selfDestructT >= 0.0f)
                left = std::max(left, su->selfDestructT);
        const bool ours = notice_.rfind("SELF-DESTRUCT IN ", 0) == 0;
        if (left < 0.0f) {
            // Gone, cancelled, or not armed yet: stop once it was seen or the
            // order had its time to arrive; the last notice then fades.
            selfDestructArmWait_ -= dt;
            if (selfDestructSeen_ || selfDestructArmWait_ <= 0.0f) selfDestructWatch_.clear();
            else if (ours) noticeTimer_ = std::max(noticeTimer_, 0.5f);
            return;
        }
        selfDestructSeen_ = true;
        if (!ours) { selfDestructWatch_.clear(); return; }   // another notice took over
        notice_ = ta::client::selfDestructNotice(left);
        noticeTimer_ = std::max(noticeTimer_, 0.5f);
    }

    void GameView::cosmeticStep(float dt) {
        // Reads the render SNAPSHOT (front()), never live world_, so it is safe
        // on the main thread while the sim worker ticks. Transient combat events
        // arrive through queues so skipped snapshots cannot discard them.
        // The frame on show can still be from before a replay jump: a future that is gone,
        // and nothing may be built from it (a flyer's anim seeded with serials ahead of
        // its unit would count them back up through the whole uint32_t range).
        if (replayMode_ && front().worldEpoch != worldEpoch_.load(std::memory_order_acquire)) return;
        const bool jumpPass = replayInTransit_;
        if (replayDisplayStale_) {
            replayResetDisplay();
            replayDisplayStale_ = false;
            if (!replaySeekTo_) replayInTransit_ = false;   // this frame is the landing
        } else if (jumpPass) {
            replayDropQueued();
        }
        const bool newTick_ = (front().gen != lastCosmeticGen_);
        lastCosmeticGen_ = front().gen;
        // Weapon impacts this tick: play each weapon's soundhitclass, picking the
        // material-specific variant from the struck unit's bodytype (flesh/armor/..).
        // A mission script asking for a camera shake: fire on the sequence edge.
        if (newTick_) {
            const auto& sr = front().shakeReq;     // snapshot, not live world_
            if (sr.seq != shakeSeqSeen_) {
                shakeSeqSeen_ = sr.seq;
                if (!jumpPass) triggerShake(sr.mag, sr.dur);
            }
        }
        std::vector<ta::sim::World::SoundReq> missionSounds;
        {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            while(!missionSoundQueue_.empty() &&
                  int32_t(front().gameTick-missionSoundQueue_.front().tick)>=0) {
                missionSounds.push_back(std::move(missionSoundQueue_.front()));
                missionSoundQueue_.pop_front();
            }
        }
        for(const auto& request:missionSounds) {
            auto name=request.name;
            std::transform(name.begin(),name.end(),name.begin(),::tolower);
            if(name.ends_with(".wav"))name.resize(name.size()-4);
            // Native mission host 4d3580: always global, non-looping, authored
            // low-three-bit priority. SoundBank enforces the 0/1 free-voice gate.
            if(!name.empty())sounds_.play(name,1.f,1.f,ta::retailSoundPriority(request.flags),false);
        }
        // Death sounds as the simulation's dying VMs played them (World::ScriptSound),
        // so every peer, replay and spectator hears the cry the shared game stream chose.
        // The native unit sound host 50def0 gates each one at its own position.
        std::vector<QueuedUnitSound> unitSounds;
        {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            while(!unitSoundQueue_.empty() &&
                  int32_t(front().gameTick-unitSoundQueue_.front().tick)>=0) {
                unitSounds.push_back(std::move(unitSoundQueue_.front()));
                unitSoundQueue_.pop_front();
            }
        }
        for(const auto& sound:unitSounds) {
            if(sound.name.empty() || !ta::unitSoundLive(sound.tick,cmdReplayEnd_)) continue;
            const bool visible=unitSoundVisibleR(sound.player,sound.hidden,sound.type,sound.position);
            // Selecting a unit changes nothing a watcher hears.
            const bool selected=!watchOnly() && std::find(selection_.begin(),selection_.end(),sound.unitId)!=selection_.end();
            playUnitSound(sound.name,sound.flags,sound.x,sound.z,visible,selected);
        }
        // Drained, not gated on newTick_: the whole point is to pick up impacts
        // from ticks whose snapshots the render never saw.
        // Released by the displayed tick, like the other tick-stamped queues below.
        std::vector<ta::sim::World::HitFx> hitsToShow;
        std::vector<std::pair<uint32_t, int>> hurt;
        {
            std::lock_guard<std::mutex> hq(hitQueueMutex_);
            hurt.swap(pendingHurt_);
            while (!hitQueue_.empty() && int32_t(front().gameTick - hitQueue_.front().tick) >= 0) {
                hitsToShow.push_back(std::move(hitQueue_.front().hit));
                hitQueue_.pop_front();
            }
        }
        // TotalA.exe's under-attack alert (client/monarchalert.h): a local unit hit while
        // off screen. The hit unit's own sound class speaks it (0x47fd70: the class of
        // [unit+0x92]+0x20e, slot "underattack"; warning1 in every shipped class), with
        // no position, at speech priority, when UNIT SPEECH allows it -- which starts the
        // 20 s cooldown -- and "<unit>: Under Attack" goes into the message area when
        // UNIT CHAT TEXT allows it (0x47ff69 -> 0x463ca0). The queue holds one request
        // of an event at a time (0x47fb0a), so one hit list asks once.
        for (const auto& [tick, id] : hurt) {
            const UnitR* u = frameUnitP(id);
            if (!u || !u->type ||   // a fatal hit too: 0x406f80 runs before the death
                !monarchAlert_.admit(u->player, localPlayer_, watchOnly(), onScreen(*u), tick)) continue;
            if (ta::MonarchAlert::speaks(unitSpeech_)) {
                const std::string* wav = u->type->soundClass.empty()
                                             ? nullptr : soundClasses_.pick(u->type->soundClass, "underattack", salt_++);
                if (wav) {   // 0x47fe03: a class with no variant for the slot says nothing
                    sounds_.play(*wav, 1.f, 1.f, 7, false);
                    monarchAlert_.spoken(tick);
                }
            }
            if (ta::MonarchAlert::shows(unitChatText_))
                gameChat_.push_back({std::string(), ta::MonarchAlert::line(u->type->name), 0, true});
            break;
        }
        // Total Annihilation firing sounds: each shot launched since the last frame
        // plays its weapon's soundstart (a WAV stem) where it left the barrel.
        {
            const Frame& sf = front();
            for (const auto& shot : sf.projectiles) {
                const ta::sim::Weapon* w = shot.wsrc;
                if (!w || w->soundStart.empty() || shot.start <= lastFireSoundTick_) continue;
                if (!noFog_ && !cellVisibleR(shot.x.toFloat(), shot.z.toFloat())) continue;
                if (sounds_.has(w->soundStart)) sounds_.playWorld(w->soundStart, shot.x.toFloat(), shot.z.toFloat());
            }
            lastFireSoundTick_ = std::max(lastFireSoundTick_, sf.gameTick);
        }
        for (const auto& h : hitsToShow) {
            // A Line-of-Sight weapon with no weaponvelocity (none shipped) still
            // lands at once and spawns no projectile: draw a bolt struck from the
            // firer to the victim, in the weapon's own inner/middle/outer colours.
            // Every shipped Line-of-Sight class flies a real shot instead (ordinary,
            // lightning, flame, and the mind-control, stone and freeze balls), so
            // the effect lands when that shot arrives. Fire breath draws its own
            // emitter, not a bolt.
            if (h.weapon && h.weapon->beam && h.weapon->projVel <= 0.0f && !h.weapon->straight &&
                !h.weapon->lightning && h.weapon->flameKind<0 &&
                h.weapon->fx != ta::sim::WeaponFx::Fire &&
                (noFog_ || cellVisibleR(h.x, h.z))) {
                BeamFx b;
                b.model = h.weapon->shotModel;
                b.sprite = h.weapon->weaponArt;
                b.player = h.fromPlayer;
                b.x1 = h.fromX; b.z1 = h.fromZ;
                b.x2 = h.x;     b.z2 = h.z;
                if(h.fromPosition) {
                    b.x1=float((*h.fromPosition)[0]>>16);b.z1=float((*h.fromPosition)[2]>>16);
                }
                if(h.position) {
                    b.x2=float((*h.position)[0]>>16);b.z2=float((*h.position)[2]>>16);
                }
                b.lightning = h.weapon->fx == ta::sim::WeaponFx::Lightning;
                for (int i = 0; i < 3; ++i) {
                    b.inner[i] = h.weapon->inner[i];
                    b.middle[i] = h.weapon->middle[i];
                    b.outer[i] = h.weapon->outer[i];
                }
                auto lift = [&](const auto& position,float x,float z) {
                    return position ? float((*position)[1] >> 16) * 0.5f -
                        terrainLift(x,z) - 12.f : 0.f;
                };
                b.alt1 = lift(h.fromPosition,b.x1,b.z1);
                b.alt2 = lift(h.position,b.x2,b.z2);
                b.fromPosition=h.fromPosition;b.position=h.position;
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

            // A status spell that lands (charm, stone, freeze) adds a silent event
            // carrying only its explosion, on the victim (HitFx::sound/explosion).
            if (h.weapon && h.sound && !h.weapon->soundHit.empty()) {
                const std::string& body = h.target ? h.target->bodyType : std::string("default");
                const std::string* wav = soundClasses_.pick(h.weapon->soundHit, body, salt_++);
                if (!wav) wav = soundClasses_.pick(h.weapon->soundHit, "default", salt_++);
                if (wav) sounds_.playWorld(*wav, h.x, h.z);
                // soundhit names a WAV directly; soundhitclass names a table.
                else if (sounds_.has(h.weapon->soundHit))
                    sounds_.playWorld(h.weapon->soundHit, h.x, h.z);
            }
            // Impact visual: play the weapon's real GAF/TAF explosion effect
            // (water variant over water). Missing art does not request extra particles.
            if (h.weapon && h.explosion) {
                spawnWeaponImpact(*h.weapon, h.x, h.z, 0, h.position, h.victimId != 0);
            }
            // Weapon area-effect: expanding shockwave rings (radiusart, staggered
            // by ringdelay) and ground fire (firestarter) at the impact.
            if (h.weapon && h.sound) {
                float maxR = std::max(h.weapon->aoe * 0.5f, 48.0f);
                for (int i = 0; i < h.weapon->ringCount && i < 3; ++i)
                    if (!h.weapon->radiusArt[i].empty())
                        spawnRing(h.weapon->radiusArt[i], h.x, h.z,
                                  float(i) * h.weapon->ringDelay, h.weapon->ringDur,
                                  h.weapon->spriteCount, maxR);
                if (h.weapon->fireStarter && !worldTables().isWater(h.x, h.z))
                    spawnEffectAnim("flame", h.x, h.z, 0.0f, 0.0f, 5);   // fire lingers
                // Camera shake for heavy impacts you can actually see.
                if (h.weapon->shakeMag > 0 && cellVisibleR(h.x, h.z))
                    triggerShake(h.weapon->shakeMag, h.weapon->shakeDur);
            }
            if (h.target && h.target->bodyType == "flesh") {
                // Spray blood from the victim's SweetSpot (its body centre) rather
                // than the ground hit point -- retail asks the COB for that piece
                // (SweetSpot -> out-param local 0) and homes hit effects to it:
                // 4dd2a0's centre of the piece's drawn bounds, as the sim aims.
                float bx = h.x, bz = h.z;
                float ba = h.position ? float((*h.position)[1] >> 16) * 0.5f -
                    terrainLift(bx,bz) : 0.f;
                if (h.victimId && (noFog_ || cellVisibleR(h.x, h.z)))
                    if (auto vi = anims_.find(h.victimId); vi != anims_.end() &&
                        vi->second.vm && vi->second.pieceNames)
                        if (const UnitR* v = frameUnitP(h.victimId); v && v->type) {
                            vi->second.vm->call("SweetSpot", {0});
                            const auto& ll = vi->second.vm->lastLocals();
                            int pc = ll.empty() ? -1 : ll[0];
                            const auto cache = cobCache_.find(v->type->id);
                            if (pc >= 0 && cache != cobCache_.end() &&
                                size_t(pc) < cache->second.pieceVertices.size()) {
                                const auto offset = ta::sim::retailPieceDrawnCenter(
                                    cache->second.modelPieces, vi->second.vm->retailPieces(), pc,
                                    cache->second.pieceVertices[size_t(pc)],
                                    uint16_t(v->headingWord + 32768), v->bodyPitch, v->bodyRoll);
                                // A world offset: X/Z on the ground, Y as scriptEffectOrigin lifts it.
                                bx = v->x + float(offset[0]) / 65536.f;
                                bz = v->z + float(offset[2]) / 65536.f;
                                ba = (float(offset[1]) / 65536.f + unitAltById(v->id)) * kProjY;
                            }
                        }
                spawnBurst(bx, bz, 5, h.target->blood[0], h.target->blood[1],
                           h.target->blood[2], 26, 1.8f, 0, ba);
            }
            // (mission ScreenShake is handled once per tick, below the hit loop)
        }
        // Sim-driven feature fire: burn-anim playback, smoke, burnt-art swaps.
        syncBurningFeatures();
        // Wind is simulation state; consume the published snapshot so skipped
        // render frames and animation rates cannot change its random stream.
        windGen_ = front().wind.generation;
        windSpeed_ = float(front().wind.speed);
        windHeading_ = float(front().wind.heading) * (6.2831853f / 65536.0f);
        // (No world_.clearHits() here: World::tick already clears hits_ at the start of the
        //  next tick, so the viewer-side clear was redundant -- and dropping it keeps the
        //  render path from mutating sim state, a prerequisite for the sim-thread decouple.)
        if (!jumpPass) nanoStep();
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
        int regBudget = registerEveryUnit_ ? INT_MAX : kRegistrationsPerFrame;
        // Cursor picking (perf follow-up C) also learns here whether every typed record of
        // this snapshot is registered (a budget break leaves that unknown).
        bool unregistered = false;
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            // A rebuilt display brings the dead back too: a body is a dead unit's record.
            if (u.type && (u.alive() || registerEveryUnit_) && !unitType_.count(u.id)) {
                if (u.replacementModel && !visuals_.count(u.replacementModel->id))
                    loadVisual(u.replacementModel->id);
                registerUnit(u.id, u.type);
                if (!unitType_.registered(u.id)) unregistered = true;   // no model
                // One that finished dying before this moment does not die again.
                if (auto a = anims_.find(u.id); !u.alive() && a != anims_.end() &&
                    u.deadFor >= float(u.corpseAnimationTicks) / 30.0f) {
                    a->second.dying = true;
                    a->second.vm->reset();
                    a->second.ownerVmStopRequested = true;
                }
                if (--regBudget <= 0) { unregistered = true; break; }
            } else if (u.type && !u.alive() && !unitType_.registered(u.id)) {
                unregistered = true;   // died before it was registered
            }
        }
        pickRegistered_ = {front().gen, front().unitSlots.size(), !unregistered};
        registerEveryUnit_ = false;
        consumeSmokeTicks();
        for(auto it=pointParticles_.begin();it!=pointParticles_.end();) {
            const auto* owner=frameUnitP(it->first);
            if(!owner || !owner->alive())it=pointParticles_.erase(it);else ++it;
        }
        // Drain mission-time events even when whole render snapshots were skipped.
        std::deque<ta::sim::World::TransportFx> transportEffects;
        std::unordered_map<int,std::vector<ta::RetailWeaponAnimation>> weaponAnimations;
        std::unordered_map<int,std::vector<ta::WeaponAnimationQueue::Shot>> weaponShots;
        std::unordered_map<int,std::vector<ta::RetailFlightAnimationQueue::Snapshot>> flightSnapshots;
        {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            flightAnimationQueue_.drain(front().gameTick,[&](const auto& snapshot) {
                flightSnapshots[snapshot.unit].push_back(snapshot);
            });
            weaponAnimationQueue_.drain(front().gameTick,[&](int unit,const auto& callback) {
                weaponAnimations[unit].push_back(callback);
            },[&](int unit,const auto& shot) {
                weaponShots[unit].push_back(shot);
            });
            while (!transportEffectQueue_.empty() &&
                   int32_t(front().gameTick-transportEffectQueue_.front().tick)>=0) {
                transportEffects.push_back(transportEffectQueue_.front());
                transportEffectQueue_.pop_front();
            }
        }
        for (const auto& event:transportEffects) {
            const auto spawn=[&](const char* name,const std::array<int32_t,3>& point) {
                spawnEffectAnim(name,float(point[0])/65536.0f,float(point[2])/65536.0f,
                    0,0,1,0,true,point,event.tick);
            };
            spawn("mindspin",event.passenger);
            spawn("transportfx:transswirl",event.carrier);
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
        animClock_ += dt;

        // (Corpses: the sim keeps the dead-unit record for the corpse window and
        // the paint pass draws it wearing the corpse mesh -- see
        // maybeSwapCorpseModel. The old path here tried to spawn a cosmetic
        // FEATURE, which never worked: corpse defs are object= models with no
        // sprite art, so addFeature always bailed before drawing anything.)
        if (noticeTimer_ > 0) noticeTimer_ -= dt;
        updateSelfDestructNotice(dt);
        if (buildLineGapShown_ > 0) buildLineGapShown_ -= dt;

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
            if (u.alive()) maybeSwapVeteranModel(u);
            else if (u.corpsePhase) maybeSwapCorpseModel(u);
            auto it = anims_.find(u.id);
            if (const auto shots=weaponShots.find(u.id);u.type && shots!=weaponShots.end())
            for (const auto& shot:shots->second)
            for (int slot=0;slot<int(u.type->weapons.size()) && slot<32;++slot) {
                if (!(shot.weapons & (uint32_t(1)<<slot))) continue;
                const auto& w = u.type->weapons[size_t(slot)];
                // These native initializers start one faction nimbus on the
                // caster, restarting any prior instance (52c540 serves the
                // ordinary and status Line-of-Sight classes, and lightning
                // through 52cbf0).
                if (w.nimbus && (w.straight || w.lightning ||
                                 w.losStatus != ta::sim::Weapon::LosStatus::None) && w.projVel > 0) {
                    if (!factionNimbusLoaded_) {
                        factionNimbus_ = ta::gaf::factionNimbus(vfs_);
                        factionNimbusLoaded_ = true;
                    }
                    std::string side = u.type->side;
                    std::transform(side.begin(), side.end(), side.begin(), ::tolower);
                    const auto name = factionNimbus_.find(side);
                    if (name != factionNimbus_.end() && !name->second.empty())
                        if (const auto* art = effectFor(name->second))
                            nimbusEffects_[u.id] = {art, shot.tick};
                }
                // FireWeapon/Attack COB callbacks own firing audio, including
                // deliberately silent branches. Native 530140 dispatches the
                // callback; it does not infer sounds from the weapon's visual kind.
                // Projectile creation already resolves the authoritative muzzle.
                // Script emissions and authored weapon art supply firing effects;
                // a generic puff here adds a flash even to ordinary arrows.
            }
            if (it == anims_.end()) continue;
            auto& a = it->second;
            // Before the death branch: a body killed while slowed must die at full pace
            // (a dead unit's gaitRate is 1), or its gait thread and the pieces it holds
            // stay gated through Dying and the corpse keeps a half-played stride.
            a.vm->setGaitRate(u.gaitRate);
            if (!u.alive()) {
                if (!a.dying) {
                    a.dying = true;
                    // Destroyed cargo remains hidden. Native 512860 propagates
                    // a zero-severity/type-8 death packet to attached passengers,
                    // bypassing their ordinary Killed/Dying presentation.
                    // Explicit retirement (a scripted removal) is not a combat
                    // death and has no death script.
                    // Combat severity survives retirement, so a skipped render snapshot
                    // must not swallow the authored death sound and detached effects.
                    if (u.embarked() || (u.deadFor>=float(ta::sim::World::kRetiredTicks)/30.0f &&
                                         !u.severity)) {
                        a.vm->reset();a.ownerVmStopRequested=true;continue;
                    }
                    // Retail order (icd 0x512610): Killed(severity, corpseOut,
                    // deathType) runs first -- deathType 3 (explosion kill)
                    // EXPLODEs every piece there; no shipped script reads the
                    // severity, and the corpse decision is the SIM's
                    // (corpseUntil), so the out-param is ignored. Then
                    // Dying(deathType): the death cry (92 units PLAY_SOUND
                    // there, zero in `death`), the fall-over via CALL death,
                    // the final EXPLODEs.
                    int32_t dtype = u.deathType;
                    if (!u.severity) {
                        // Petrified/frozen: retail skips Killed AND Dying
                        // (severity forced 0) -- the victim simply freezes in
                        // its current pose and stands as the statue. reset()
                        // stops threads but leaves in-flight piece turns/moves
                        // active, so also stop display VM advancement.
                        a.vm->reset();a.ownerVmStopRequested=true;
                    } else {
                        // Keep the existing pose/statics and let the authored
                        // callbacks stop their own threads, as in the simulation.
                        // TotalA.exe runs Killed whenever the severity is
                        // positive (0x4865c3), a self-destruct included.
                        a.vm->call("Killed", {int32_t(u.severity), 0, dtype});
                        if (!a.vm->start("Dying", {dtype})) a.ownerVmStopRequested=true;
                    }
                    // Death audio belongs to Killed/Dying, including deliberately
                    // silent branches. Do not invent a cry from a filename. The
                    // simulation runs these callbacks too, drawing RAND from the
                    // shared game stream: unitSoundQueue_ plays its sounds, and
                    // onPlaySound drops this VM's from here on.
                    // Killed/Dying own death pieces and emitted effects. Units
                    // with EXPLODEAS also produce their authored weapon impact
                    // through the simulation. Do not add a generic body blast.
                    // (The mission "UnitDestroyed" hook now fires deterministically in
                    // simStep on the sim thread -- see the death-edge detection there --
                    // rather than here off the render-side death animation.)
                }
                continue;   // VM advanced in the parallel pass below
            }
            if (a.flying) {
                // Flight height already follows the retail controller in the sim.
                // The old independent 0.7s ascent and terrain servo could draw a
                // conjurer far north of her actual position, especially near hills.
                const float datum=flyerGround(u.x,u.z);
                const auto [ground,altitude]=u.flightRenderHeight(datum);
                a.groundY=ground;a.groundInit=true;a.altitude=altitude;
                flyerAltitudeSeen_=std::max(flyerAltitudeSeen_,altitude);   // cursor picking's flyer bound
                // Occupancy is authoritative too: a hovering conjurer stays active
                // even when stationary; a landed flyer plays its landing pose.
                const bool air=u.flightGroundMode==2;
                if (a.hasFlightSM) {
                    // Drake VTOL state machine: retail's engine only ever calls
                    // BeginFlight (takeoff) and BeginLanding (descent), plus
                    // setSFXoccupy(5) to report ACTIVE (static 6). Two Create-started
                    // threads then drive the pose, both gated on static 6:
                    //   FlightControl -- plays launch->fly->soar while active and NOT
                    //     (aiming static 5 / building static 9); when aiming or building
                    //     it switches to `attack` for the airborne body.
                    //   RestoreWatcher -- while active AND building (static 10, set by
                    //     Go via StartBuilding->RequestState), loops `build`, the conjure
                    //     arm gesture; else eases back to rest.
                    // Both freeze if static 6 goes clear -- so the flyer must stay ACTIVE
                    // through hovering builder work, as reported by flightGroundMode. Traced
                    // with tools/re/emuphase.py + the cob VM; pinned by conjure_test,
                    // which drives this exact sequence through the real Vm. No reset()
                    // -- the loops must keep running.
                    const auto startFlightCallback=[&](auto call) {
                        switch (call) {
                            case ta::RetailFlightAnimationCall::BeginFlight:
                                a.vm->start("BeginFlight");
                                break;
                            case ta::RetailFlightAnimationCall::BeginLanding:
                                a.vm->start("BeginLanding");
                                break;
                            case ta::RetailFlightAnimationCall::EndTransport:
                                a.vm->start("EndTransport");
                                break;
                        }
                    };
                    if(const auto snapshots=flightSnapshots.find(u.id);snapshots!=flightSnapshots.end())
                        for(const auto& snapshot:snapshots->second)
                            ta::updateRetailFlightAnimation(a.flightAnimation,snapshot.airborne,
                                snapshot.beginFlightSerial,snapshot.landingSerial,
                                u.type->canTransport,startFlightCallback);
                    // Also initialize newly registered units and retain the
                    // existing mode fallback when no transition packet is due.
                    ta::updateRetailFlightAnimation(a.flightAnimation,air,
                        u.flightBeginCallbackSerial,u.flightLandingCallbackSerial,
                        u.type->canTransport,startFlightCallback);
                }
                // Other flyers (Priest and ambient birds) run their own
                // Create-started controllers, polling speed/vertical motion.
                // Resetting them to a hand-picked fly/land script kills those
                // controllers and the Priest's concurrent build animation.
            }
            // Every shipped walker has a Create-owned movement controller.
            // GET 29/28/34 drives its gait without resetting concurrent scripts.
            // Native 4dc800's common tail receives TurnDirection during the
            // mover, then MoveRate (4db350), then setSFXoccupy (4dc600). All
            // three can signal COB threads, so keep that order on coincident
            // transitions for both ground and flight movers.
            // TurnDirection uses the REQUESTED turn (want - heading, unclamped
            // BAM), converted with retail's truncating /182. Retail notifies only
            // when its turn state changes sign, including the stop transition.
            const int turnDegrees=u.turnReqBam/182;
            const uint32_t previousMoveRate=a.moveRate;
            ta::updateRetailMovementAnimationCallbacks(a.hasTurnDir,turnDegrees,a.turnSign,
                u.animationMoveRate,a.moveRate,u.animationOccupancy,a.occupancy,
                [&](int degrees) { a.vm->start("TurnDirection",{degrees}); },
                // TotalA.exe 0x43da70: StartMoving/StopMoving + MoveRate1..3.
                [&](uint32_t rate) {
                    ta::sim::retailMovementCallins(previousMoveRate,rate,
                        [&](const char* name) { a.vm->start(name); });
                },
                [&](uint32_t surface) { a.vm->start("setSFXoccupy",{int32_t(surface)}); });
            // TotalA.exe 0x437910: when the wind changes, a wind generator's script gets
            // SetDirection(wind heading, game+0x37ed8) then SetSpeed(wind speed << 4,
            // game+0x37eda) -- its post turns into the wind and its blades spin.
            if (u.type && u.type->windGenerator > 0 && a.windGen != front().wind.generation) {
                a.windGen = front().wind.generation;
                a.vm->start("SetDirection", {int32_t(front().wind.heading)});
                a.vm->start("SetSpeed", {int32_t(front().wind.speed) << 4});
            }
            // The simulation owns readiness and callback order. Do not recompute
            // aim from interpolated render poses or a separate display handshake.
            if (const auto callbacks=weaponAnimations.find(u.id);callbacks!=weaponAnimations.end())
            for (const auto& event:callbacks->second) {
                switch(event.kind) {
                // TotalA.exe's call-ins by FBI slot (docs/retail-weapon-scripts.md
                // section 1); the events carry that slot.
                case ta::RetailWeaponAnimation::Aim: {
                    static constexpr const char* kAim[3]={"AimPrimary","AimSecondary","AimTertiary"};
                    if(event.slot<3)a.vm->start(kAim[event.slot],{event.heading,event.pitch});
                    break;
                }
                case ta::RetailWeaponAnimation::Fire: {
                    static constexpr const char* kFire[3]={"FirePrimary","FireSecondary","FireTertiary"};
                    if(event.slot<3)a.vm->start(kFire[event.slot],{});
                    // 0x49cbe6: RockUnit(-800 cos h, -800 sin h), h the shot's
                    // heading relative to the body.
                    a.vm->start("RockUnit",{-ta::sim::retailScaledCosine(event.heading,800),
                                            -ta::sim::retailScaledSine(event.heading,800)});
                    a.firing=true;
                    break;
                }
                case ta::RetailWeaponAnimation::Clear:
                    a.vm->start("TargetCleared",{event.slot});
                    break;
                case ta::RetailWeaponAnimation::Switch:
                    a.vm->start("SwitchWeapon",{event.slot});
                    break;
                case ta::RetailWeaponAnimation::Hit:
                    a.vm->start("HitByWeapon",{event.slot,
                        ta::sim::retailScaledCosine(event.heading,400),
                        ta::sim::retailScaledSine(event.heading,400),event.damage});
                    break;
                }
            }
            // Native 4d4520 passes wind bearing minus unit bearing in the
            // retail heading convention, without normalizing that difference.
            if (u.type->receivesWind && a.hasWind && a.windStamp != windGen_) {
                a.windStamp = windGen_;
                const auto bodyHeading=ta::sim::portHeadingToRetail(
                    ta::sim::bamFromRadians(u.heading));
                const int32_t relativeHeading=int32_t(front().wind.heading)-int32_t(bodyHeading);
                a.vm->start("WindChange", {front().wind.speed,relativeHeading});
            }
            // Builders: the conjure/build animation while actively working a site
            // (constructing, repairing, or reclaiming). Retail drives this via the COB
            // StartBuilding/StopBuilding hooks -- StartBuilding raises the script's own
            // "am building" gate, StopBuilding clears it. This now covers FLYING builders
            // too (arafly/zonhunt via their FlightControl, tarpries via a direct `build`),
            // which used to just flap in place while conjuring.
            if (u.type->isBuilder && !isStructure(u.type)) {
                // Each queued output gets its own site id. A non-empty queue
                // alone is not work: while capped there is no active conjure
                // site, and consecutive products must retrigger one-shot poses.
                ta::updateRetailBuilderAnimation(a.building,a.workId,
                    u.buildSiteId,u.repairId,u.reclaimId,!u.buildQueue.empty(),
                    u.productionSiteId,u.conjuring,u.walking(),u.type->canFly,
                    [&](bool working) {
                        if (!a.flying) {
                            // Ground builder.
                            // TotalA.exe 0x4385a7: StartBuilding gets one argument, the
                            // heading to the work target relative to the body (low 16
                            // bits); scripts turn the torso/arms by it (armcom:
                            // TURN torso y-axis to heading).
                            if (working) {
                                std::vector<int32_t> args;
                                float tx = 0, tz = 0;
                                bool have = false;
                                if (const int t = u.buildSiteId ? u.buildSiteId : u.repairId; t)
                                    if (const UnitR* target = frameUnitP(t)) { tx = target->x; tz = target->z; have = true; }
                                if (!have && u.reclaimId)
                                    for (const auto& f : features_)
                                        if (f.simId == u.reclaimId) { tx = f.x; tz = f.z; have = true; break; }
                                if (have) {
                                    const uint16_t direction = uint16_t(ta::sim::retailDirection(
                                        ta::sim::Fixed::fromFloat(u.x - tx), ta::sim::Fixed::fromFloat(u.z - tz)).v);
                                    args.push_back(int32_t(uint16_t(direction - uint16_t(u.headingWord + 32768))));
                                }
                                a.vm->start("StartBuilding", args) || a.vm->start("startbuild");
                            }
                            // The script's own controller restores the pose after
                            // StopBuilding. Starting restore_x here races that
                            // controller and bypasses its attack/movement guards.
                            else a.vm->start("StopBuilding");
                        } else if (a.hasFlightSM) {
                            // SM flyer (arafly/zonhunt monarch): StartBuilding sets unit
                            // value 5 and fires RequestState->Go, raising statics 9/10;
                            // RestoreWatcher (a Create thread) then loops `build` -- NOT
                            // FlightControl, which plays `attack` for the body. Verified by
                            // driving the real Vm: arms conjure, body holds the attack pose.
                            // No reset.
                            a.vm->start(working ? "StartBuilding" : "StopBuilding");
                        } else {
                            // Generic flyer builder (tarpries): no FlightControl loop, so the
                            // flight re-kick above loops `build` while a.building (matching
                            // retail's BuildControl). Just flip the state + set the COB gate.
                            a.vm->start(working ? "StartBuilding" : "StopBuilding");
                        }
                    });
            }
            // Buildings: yard/production anims. Detect via isStructure (maxVel <= ta::sim::Fixed()),
            // NOT !canMove -- the Keep/Castle/Hell carry canmove=1 in their FBI, so
            // the old test skipped every factory and none of them ever animated
            // while training.
            if (u.type && isStructure(u.type)) {
                // Use exported state-machine entry points rather than internal
                // door routines; keep Create-owned ambient threads running.
                ta::updateRetailFactoryAnimation(a.producing,a.workId,u.underConstruction,
                    !u.buildQueue.empty(),u.productionSiteId,[&](auto call) {
                        using Call=ta::RetailFactoryAnimationCall;
                        switch(call) {
                        case Call::Activate: a.vm->start("Activate");break;
                        case Call::Deactivate: a.vm->start("Deactivate");break;
                        case Call::StartBuilding: a.vm->start("StartBuilding",{0,0});break;
                        case Call::StopBuilding: a.vm->start("StopBuilding");break;
                        }
                    });
            }
            // The simulation owns activation, including manual gate commands and
            // automatic AI gate occupancy checks. Mirror its edge so door poses
            // agree with the authoritative yard handshake below.
            if (u.type && (u.type->onOffable || u.type->activateWhenBuilt) && a.hasActivate && u.active != a.active) {
                a.active = u.active;
                a.vm->start(u.active ? "Activate" : "Deactivate");
            }
            // Cloak pose. StartCloaking/StopCloaking are real engine entry points
            // (they appear in the icd's call-script-by-name sites alongside Create
            // and Activate), and the two units that define them -- araspy and
            // npcheket -- fold their pieces with MOVE_NOWs in the `cloak` script
            // that StartCloaking starts. We were never calling either, so a
            // cloaking spy just went transparent in its walking pose.
            // Native callbacks follow the requested mode. Enemy proximity or
            // insufficient mana can reveal a unit without restoring its pose.
            if (a.hasCloakAnim && u.cloakOn != a.cloakOn) {
                a.cloakOn = u.cloakOn;
                a.vm->start(u.cloakOn ? "StartCloaking" : "StopCloaking");
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
        if (paused_) { animRateLive_ = false; return; }
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
        if (float(nowMs - animAdvanceMs_) > stallMs) { animRateLive_ = false; return; }
        // Short of a stall, a world can still run SLOWER than its speed asks: the Benchmark
        // overloads it on purpose, a big battle outruns a slow machine, a slow player holds
        // the room back. Its units then move slower than asked, and the walk cycles, wheels
        // and rotors must slow with them, or legs run ahead of the bodies they carry. So the
        // animation follows the ticks actually advanced per second (smoothed over ~0.5 s),
        // never faster than the speed. The ticks are timed on the frame's own realDt -- the
        // clock the animation runs on -- so a harness stepping frames at a fixed dt stays
        // reproducible. Before the game runs (tick 0: loading, tools) it plays at the speed.
        // Display only: the units' own scripts run in the World.
        const double nominalRate = 30.0 * double(std::max(0.1f, animSpeed()));
        const uint32_t tickNow = front().gameTick;
        if (tickNow == 0 || !animRateLive_ || tickNow < animRateTick_) {
            animRate_ = nominalRate;
        } else if (realDt > 0) {
            const double dts = double(realDt);
            animRate_ += (double(tickNow - animRateTick_) / dts - animRate_) * std::min(1.0, dts / 0.5);
        }
        animRateTick_ = tickNow;
        animRateLive_ = true;
        const float pace = tickNow == 0 ? 1.0f : float(std::clamp(animRate_ / nominalRate, 0.02, 1.0));
        float dt = realDt * animSpeed() * pace;
        vmTick_.clear();explosionVmTick_.clear();
        for (auto& [id, a] : anims_) {
            if(a.vm && ta::retailOwnerVmMayAdvance(a.ownerVmStopRequested)) {
                if(a.vm->mayReachExplosion(a.explosionReachability))explosionVmTick_.emplace_back(id,a.vm.get());
                else vmTick_.push_back(a.vm.get());
            }
            a.fireT += dt; a.smokeT += dt;   // age since last emit (fire fades if it stops)

        }
        pool_.parallelFor(vmTick_.size(), [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) vmTick_[i]->tick(dt);
        }, /*minParallel=*/1500);   // ~0.2us/VM: pool dispatch only pays off at scale
        std::sort(explosionVmTick_.begin(),explosionVmTick_.end(),
            [](const auto& a,const auto& b){return a.first<b.first;});
        for(const auto& [id,vm]:explosionVmTick_)vm->tick(dt);
#ifndef NDEBUG
        // TA_PIECELOG=<unit id>: that unit's display-VM piece poses, about twice a second.
        if (const char* logId = ta::devEnv("TA_PIECELOG")) {
            static float logT = 0; logT += dt;
            if (logT >= 0.5f) {
                logT = 0;
                if (auto it = anims_.find(std::atoi(logId)); it != anims_.end() && it->second.vm) {
                    const auto& vm = *it->second.vm;
                    std::string line;
                    for (size_t p = 0; p < vm.pieces().size(); ++p) {
                        const auto& s = vm.pieces()[p];
                        if (!s.visible || s.rot[0] || s.rot[1] || s.rot[2] || s.move[0] || s.move[1] || s.move[2]) {
                            char buf[160];
                            std::snprintf(buf, sizeof buf, " %s%s[r %.2f %.2f %.2f m %.1f %.1f %.1f]",
                                vm.file().pieces[p].c_str(), s.visible ? "" : "(hid)",
                                s.rot[0], s.rot[1], s.rot[2], s.move[0], s.move[1], s.move[2]);
                            line += buf;
                        }
                    }
                    std::fprintf(stderr, "PIECELOG tick=%u threads=%zu%s\n", front().gameTick,
                                 vm.threadPcs().size(), line.c_str());
                }
            }
        }
#endif
        // Drain emit-sfx the VMs stashed (fire/smoke from FireControl-style loops),
        // now serially on the main thread, into the world-space effect system.
        for (auto& [id, a] : anims_) {
            if (a.pendingPoints.empty() && a.pendingSfx.empty() && a.pendingSnd.empty()) continue;
            const auto* u = frameUnitP(id);
            if (u && u->type && unitShownR(*u)) {
                for(auto& event:a.pendingPoints) {
                    event.unitId=u->id;event.player=u->player;event.position=u->worldPosition;
                    event.heading=uint16_t(u->headingWord+32768);event.pitch=u->bodyPitch;event.roll=u->bodyRoll;
                    emitPoint(event);
                }
                for (auto& [piece, sfx] : a.pendingSfx) emitSfx(*u, a, piece, sfx);
            }
            if (u && u->type && !a.pendingSnd.empty()) {
                const bool visible=unitSoundVisibleR(u->player,u->cloaked || u->embarked(),u->type,u->worldPosition);
                const bool selected=!watchOnly() && std::find(selection_.begin(),selection_.end(),id)!=selection_.end();
                for (const auto& [index,flags]:a.pendingSnd)
                    playUnitSound(a.vm->file().name(uint32_t(index)),flags,u->x,u->z,visible,selected);
            }
            a.pendingPoints.clear();
            a.pendingSfx.clear();
            a.pendingSnd.clear();
        }
    }

    float GameView::animSpeed() const {
        if (isNet() && mp_) return std::max(1, int(mp_->gameSpeed())) / 10.0f;
        if (replayMode_) return std::min(speedMult(), ta::replaybar::kFullEffectsSpeed);
        return speedMult();
    }

    void GameView::update(float dt) {
        if (paused_) return;   // freeze the sim; input/render/camera keep running
        // Game speed: scale game time (sim, effects, AI, animation all follow dt).
        dt *= speedMult();
#ifndef NDEBUG
        if (patrolPerfAccum_ >= 0.0f) {
            patrolPerfAccum_ += dt;
            for (int steps = 0; patrolPerfAccum_ >= 1.0f / 30.0f && steps < 64; ++steps) {
                simStep(1.0f / 30.0f);
                patrolPerfAccum_ -= 1.0f / 30.0f;
                if (world_.tickCount() % 300 == 0) {
                    int alive = 0, moving = 0, displaced = 0;
                    for (const auto& u : world_.units()) {
                        alive += u.alive();
                        moving += u.alive() && u.speed.v > 0;
                        displaced += u.alive() && (u.x != u.homeX || u.z != u.homeZ);
                    }
                    std::printf("PATROL_PERF tick=%u alive=%d moving=%d displaced=%d\n",
                                world_.tickCount(), alive, moving, displaced);
                    std::fflush(stdout);
                }
            }
            cosmeticStep(dt);
            return;
        }
#endif
        simStep(dt);
        cosmeticStep(dt);
    }

    // Retail draws the map at native 1:1 scale: zoom there, keeping the centre of the world
    // viewport rather than zooming toward the pointer (Shift+Z, and MOUSE ZOOM turned off).
    void GameView::retailZoom(int winW, int winH, bool notice) {
        const float cx=float(mapViewW(winW))*0.5f;
        const float cy=float(mapViewH(winH))*0.5f;
        const float oldZoom=std::max(mapView_.zoom(),1e-3f);
        const float wx=mapView_.offX()+cx/oldZoom;
        const float wz=mapView_.offY()+cy/oldZoom;
        mapView_.setZoom(1.0f);
        mapView_.clampZoom(mapViewW(winW),mapViewH(winH));   // a map smaller than the view: fill it
        const float z=mapView_.zoom();
        mapView_.setOffset(wx-cx/z,wz-cy/z);
        if(notice){notice_="RETAIL ZOOM";noticeTimer_=2;}
    }

    void GameView::prepare(int winW, int winH) {
        if (watchOnly() && !inLobbyPhase())   // the interface of the player on show
            if (const int shown = hudPlayer(); shown >= 0) showSideArt(framePlayer(shown).side);
        if (initialCamera_ && !inLobbyPhase() && !spectating_ && !replayMode_) {
            const auto [x,z]=*initialCamera_;
            float initialZoom=mouseZoomOn_ ? 1.25f : 1.0f;   // MOUSE ZOOM off: retail's 1:1
            // Reproducible render profiling through the normal network path.
            // devEnv is disabled in release; ordinary starting views stay fixed.
            if (const char* zoom=ta::devEnv("TA_PROFILE_ZOOM"))
                initialZoom=std::clamp(float(std::atof(zoom)),0.1f,4.0f);
            mapView_.setZoom(initialZoom);
            mapView_.setOffset(x-terrainLiftX(x,z)-float(mapViewW(winW))*0.5f/mapView_.zoom(),
                               z-terrainLift(x,z)-12.0f-float(winH-barH())*0.5f/mapView_.zoom());
            initialCamera_.reset();
        }

        // MOUSE ZOOM off: retail's 1:1 view, or just enough zoom to fill the window on a map
        // smaller than it -- held every frame, so a resized window keeps the rule. (The
        // benchmark's and the poster's own cameras are left alone.)
        if (!mouseZoomOn_ && !benchmarkMode_ && !poster_ && winW > 0 && winH > 0) {
            const float want = std::max(1.0f, mapView_.minZoom(mapViewW(winW), mapViewH(winH)));
            if (std::abs(mapView_.zoom() - want) > 1e-4f) retailZoom(winW, winH, false);
        }
        mapView_.ensureChunks(mapViewW(winW), mapViewH(winH));
        updateWorldFilter();   // after the camera settles, before anything draws
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
            if (ta::devFlag("TA_CONJURE_TEST") ||
                ta::devFlag("TA_PROJECTILE_CAPTURE") || ta::devFlag("TA_NIMBUS_CAPTURE")) {
                beginFrame();
                cosmeticStep(1.0f / 30.0f);
                animFrame(1.0f / 30.0f);
                endFrame();
            }
            if (ta::devFlag("TA_NIMBUS_CAPTURE")) {
                for (const auto& [id, effect] : nimbusEffects_)
                    if (front().gameTick - effect.started >= uint32_t(std::max(1,
                            std::atoi(ta::devEnv("TA_NIMBUS_CAPTURE"))))) {
                        std::printf("nimbus capture: tick=%u caster=%d age=%u\n", front().gameTick,
                            id, front().gameTick - effect.started);
                        paused_ = true;
                        return;
                    }
            }
            if(ta::devFlag("TA_PROJECTILE_CAPTURE")) {
                const uint32_t captureAge=uint32_t(std::max(1,std::atoi(ta::devEnv("TA_PROJECTILE_CAPTURE"))));
                for (const auto& shot:world_.flames())
                    if (int32_t(world_.tickCount()-shot.start)>=int32_t(captureAge) && !shot.particles.empty()) {
                        std::printf("flame capture: tick=%u age=%u kind=%d particles=%zu\n",
                            world_.tickCount(),world_.tickCount()-shot.start,
                            shot.weapon ? int(shot.weapon->flameKind) : -1,shot.particles.size());
                        paused_=true;return;
                    }
                for(const auto& shot:world_.projectiles())if(shot.lightningEffect &&
                    shot.age>=std::max(1,std::atoi(ta::devEnv("TA_PROJECTILE_CAPTURE"))) &&
                    !shot.lightningEffect->particles.empty()) {
                    std::printf("projectile capture: tick=%u age=%d pixels=%zu nimbus=%zu\n",world_.tickCount(),shot.age,
                        shot.lightningEffect->pixels.size(), nimbusEffects_.size());
                    paused_=true; // Keep the selected tick while screenshot terrain uploads finish.
                    return;
                }
                // A mind-control, stone or freeze ball in flight.
                for(const auto& shot:world_.projectiles())
                    if(shot.straight && shot.life>0 && shot.wsrc &&
                       shot.wsrc->losStatus!=ta::sim::Weapon::LosStatus::None && shot.age>=int32_t(captureAge)) {
                        std::printf("status shot capture: tick=%u age=%d at=%.1f,%.1f\n",world_.tickCount(),shot.age,
                            ta::sim::Fixed::raw(shot.position[0]).toFloat(),ta::sim::Fixed::raw(shot.position[2]).toFloat());
                        paused_=true;
                        return;
                    }
            }
            if (trace_ && t >= printed) {
                printed += 0.5f;
                for (auto& u : world_.units())
                    if (u.player == 0 && u.alive()) {
                        std::printf("TRACE %.1f %d %.1f %.1f %.1f\n", t, u.id,
                                    u.x.toFloat(),
                                    (u.type && u.type->canFly ? u.flightY : u.groundY).toFloat(),
                                    u.z.toFloat());
                        if (ta::devFlag("TA_CONJURE_TEST")) {
                            std::printf("BUILDTRACE tick=%u site=%d orders=%zu events=%x\n",
                                        world_.tickCount(),u.buildSiteId,u.orders.size(),u.missionEvents);
                            for (const auto& o:u.orders)
                                std::printf("  stage=%u pending=%x wait=%x goal=%.0f,%.0f build=%s\n",
                                            unsigned(o.mission.stage),o.mission.pending,o.mission.waitMask,
                                            o.x.toFloat(),o.z.toFloat(),o.buildType?o.buildType->id.c_str():"-");
                            if (const char* name=ta::devEnv("TA_CONJURE_TARGET"))
                                if (const auto* type=registry_.find(name))
                                    std::printf("  target footprint=%dx%d\n",type->footX,type->footZ);
                        }
                    }
            }
        }
    }

    int GameView::minWindowWidth() const {
        int n = int(registry_.maxBuildMenu());
        int rowW = n > 0 ? (n - 1) * 66 + 60 : 0;
        return rowW + 24 + panelW();
    }

    void GameView::loadDamageFlameClasses() {
        if(damageFlameClassesLoaded_)return;
        damageFlameClassesLoaded_=true;
        for(const auto& path:vfs_.list("gamedata/damageflames")) {
            if(!path.ends_with(".tdf"))continue;
            const auto definitions=vtdf(path);
            for(const auto& [name,node]:definitions.orderedChildren()) {
                const auto key=ta::hpi::MountSet::key(std::string(name));
                const int kind=key=="smallflame" ? 0 : key=="mediumflame" ? 1 : key=="largeflame" ? 2 : -1;
                if(kind<0)continue;
                const auto bank=node->valueOr("gaf","");
                const auto sequence=node->valueOr("anim","");
                if(bank.empty() || sequence.empty())continue;
                if(const auto* art=effectFor(bank+":"+sequence);art && !art->frames.empty())
                    damageFlameClasses_[size_t(kind)].push_back(art);
            }
        }
    }

    void GameView::consumeSmokeTicks() {
        std::deque<SmokeTick> ticks;
        {
            std::lock_guard<std::mutex> lock(hitQueueMutex_);
            while(!smokeTickQueue_.empty() && int32_t(front().gameTick-smokeTickQueue_.front().tick)>=0) {
                ticks.push_back(std::move(smokeTickQueue_.front()));smokeTickQueue_.pop_front();
            }
        }
        const auto random=[&] {return ta::sim::retailCrtRandom(smokeRandom_);};
        for(const auto& tick:ticks) {
            if(tick.tick<=smokeTick_) {smokeSprites_.clear();featureSmokeSprites_.clear();damageFlames_.clear();pointParticles_.clear();smokeRandom_=1;}
            smokeTick_=tick.tick;
            for(int owner:tick.removedOwners) {
                smokeSprites_.erase(owner);damageFlames_.erase(owner);pointParticles_.erase(owner);
            }
            for(int id:tick.removedFeatures)featureSmokeSprites_.erase(id);
            const auto updateFeatureSmoke=[&](const FeatureSmokeEmission& event) {
                const auto found=featureSmokeSprites_.find(event.id);
                if(found==featureSmokeSprites_.end())return;
                auto& sprites=found->second;
                std::erase_if(sprites,[&](auto& sprite) {
                    return !sprite.particle.tick(tick.windX,tick.windZ,8155,random);
                });
                if(sprites.empty())featureSmokeSprites_.erase(found);
            };
            const auto emitFeatureSmoke=[&](const FeatureSmokeEmission& event) {
                const auto definition=featureDefs_.find(event.type);
                if(definition==featureDefs_.end())return;
                const auto* body=featureArtFor(definition->second,"seqnameburn","seqnameburnshad");
                if(!body || body->fgeom.empty() || body->totalTicks<=0)return;
                const int age=int(event.age%uint32_t(body->totalTicks));
                const size_t index=size_t(std::upper_bound(body->tickEnd.begin(),body->tickEnd.end(),age)-body->tickEnd.begin());
                if(index>=body->fgeom.size())return;
                const auto& frame=body->fgeom[index];
                const int dx=frame.w/4+int(uint64_t(random())*unsigned(frame.w/2)/32768)-frame.xoff;
                const int dy=2*(frame.yoff-int(uint64_t(random())*unsigned(frame.h/2)/32768)-frame.h/4);
                auto& sprites=featureSmokeSprites_[event.id];
                if(sprites.size()>=10)return;
                const auto* art=effectFor("bigsmoke");
                if(!art || art->frames.size()<3)return;
                ta::RetailSmokeParticle particle;particle.position=event.position;
                particle.position[0]=std::bit_cast<int32_t>(uint32_t(particle.position[0])+(uint32_t(dx)<<16));
                particle.position[1]=std::bit_cast<int32_t>(uint32_t(particle.position[1])+(uint32_t(dy)<<16));
                particle.frameLimit=2+uint32_t(uint64_t(random())*(art->frames.size()-3)/32768);
                sprites.push_back({particle,art});
            };
            ta::retailStepFeatureSmoke(tick.features,updateFeatureSmoke,emitFeatureSmoke);
            const auto& terrain=mapView_.map();
            for(auto it=pointParticles_.begin();it!=pointParticles_.end();) {
                std::erase_if(it->second,[&](auto& particle) {
                    return !particle.tick(int(uint8_t(terrain.seaLevel)),[&](const auto& position) {
                        return terrain.heights.empty() ? -1 : ta::sim::retailTerrainHeight(
                            position[0],position[2],terrain.width,terrain.height,[&](int x,int z) {
                                return terrain.heights[size_t(z)*size_t(terrain.width)+size_t(x)];
                            });
                    });
                });
                if(it->second.empty())it=pointParticles_.erase(it);else ++it;
            }
            for(auto it=damageFlames_.begin();it!=damageFlames_.end();) {
                std::erase_if(it->second,[](auto& sprite) {
                    if(--sprite.life<=0)return true;
                    sprite.clock.tick(sprite.art->durations,sprite.art->loop);
                    return !sprite.clock.active;
                });
                if(it->second.empty())it=damageFlames_.erase(it);else ++it;
            }
            for(auto it=smokeSprites_.begin();it!=smokeSprites_.end();) {
                std::erase_if(it->second,[&](auto& sprite) {
                    return !sprite.particle.tick(tick.windX,tick.windZ,8155,random);
                });
                if(it->second.empty())it=smokeSprites_.erase(it);else ++it;
            }
            for(const auto& event:tick.emissions) {
                if(event.code>=2 && event.code<=5) {
                    emitPoint(event);
                    continue;
                }

                if(event.code==263 || event.code==264) {
                    const auto& point=event.position;
                    spawnEffectAnim(event.code==263 ? "deathmagic:purpledeath" : "pillaroflight",
                        float(point[0])/65536.f,float(point[2])/65536.f,0,0,1,0,true,point,event.tick);
                    continue;
                }
                if(event.code>=260 && event.code<=262) {
                    loadDamageFlameClasses();
                    auto& sprites=damageFlames_[event.unitId];
                    const auto& variants=damageFlameClasses_[size_t(event.code-260)];
                    if(sprites.size()>=40 || variants.empty())continue;
                    const auto* art=variants[size_t(uint64_t(random())*variants.size()/32768)];
                    DamageFlameSprite sprite; sprite.position=event.position;sprite.art=art;
                    sprite.clock.start(art->durations);sprites.push_back(sprite);
                    continue;
                }
                auto& sprites=smokeSprites_[event.unitId];
                if(sprites.size()>=10)continue;
                const char* name=event.code==265 ? "steam" : event.code==258 ? "smoke:smoke01" : "bigsmoke";
                const auto* art=effectFor(name);
                if(!art || art->frames.size()<3)continue;
                ta::RetailSmokeParticle particle;
                particle.position=event.position;particle.small=event.code==258;particle.steam=event.code==265;
                particle.frameLimit=2+uint32_t(uint64_t(random())*(art->frames.size()-3)/32768);
                sprites.push_back({particle,art});
            }
        }
    }

    void GameView::emitPoint(const ta::sim::World::ScriptEmission& event) {
        auto& particles=pointParticles_[event.unitId];
        if(particles.size()>=100 || event.pose.empty())return;
        std::array<std::array<int32_t,3>,2> endpoints;
        for(size_t i=0;i<2;++i) {
            const auto point=modelEmissionPoint(event.pose,event.vertices[i],
                event.heading,event.pitch,event.roll);
            for(size_t axis=0;axis<3;++axis) {
                const auto offset=modelEmissionFixed(point[axis]);
                endpoints[i][axis]=std::bit_cast<int32_t>(uint32_t(event.position[axis])+
                    (axis==2 ? 0u-uint32_t(offset) : uint32_t(offset)));
            }
        }
        if(event.code>=4)std::swap(endpoints[0],endpoints[1]);
        particles.push_back(ta::RetailPointParticle::emit(endpoints[0],endpoints[1],
            event.code%2 ? 8:16,[&] {return ta::sim::retailCrtRandom(smokeRandom_);}));
    }

    void GameView::emitSfx(const UnitR& /*u*/, Anim& a, int piece, int32_t sfx) {
        // These emissions come from the simulation instruction stream, not the
        // independently paced display VM.
        if(sfx==257 || sfx==258 || sfx==265 || (sfx>=260 && sfx<=262))return;
        const char* anim = sfxAnimFor(sfx);
        if (!anim) return;
        const EffectAnim* ea = effectFor(anim);
        if (!ea || ea->frames.empty()) return;
        // Refresh this unit's persistent flame/smoke; drawFxItem (FxKind::UnitFx) cycles it smoothly.
        if (anim[0] == 's') { a.smokeFx = ea; a.smokeT = 0; a.smokePiece = piece; }
        else                { a.fireFx = ea;  a.fireT = 0;  a.firePiece = piece; }
    }

    bool GameView::explodePiece(const UnitR& u, Anim& a, int piece, int32_t flags) {
        bool accepted=false;
        float x, z, dAlt;
        scriptEffectOrigin(u, a, piece, x, z, dAlt);
        if(!(flags&0x20) && a.pieceNames && piece>=0 &&
           size_t(piece)<a.pieceNames->size()) {
            const auto visualName=unitType_.find(u.id);
            // An immediate corpse handoff may already have swapped the body
            // model. EXPLODE still detaches pieces from the original live model.
            const auto visual=u.corpsePhase && u.type
                ? visuals_.find(u.veteran>=10 && !u.type->veteranModel.empty()
                    ? u.type->veteranModel : u.type->id)
                : (visualName==unitType_.end()?visuals_.end():visuals_.find(visualName->second));
            if(visual!=visuals_.end()) {
                const auto& wanted=(*a.pieceNames)[size_t(piece)];
                const auto find=[&](auto&& self,const ta::tdo::Object& object)->const ta::tdo::Object* {
                    if(ta::hpi::MountSet::key(object.name)==wanted)return &object;
                    for(const auto& child:object.children)if(const auto* found=self(self,child))return found;
                    return nullptr;
                };
                if(const auto* source=find(find,visual->second.model.root)) {
                    ta::RetailDebrisMotion motion;
                    ta::RetailDebrisMotion::launch(uint32_t(flags),[&](unsigned bound) {
                        return unsigned(uint64_t(ta::sim::retailCrtRandom(smokeRandom_))*bound/32768u);
                    },motion);
                    if(detachedPieces_.size()<100) {
                        DetachedPiece debris;
                        debris.model=ta::retailDebrisModel(*source,(flags&64)!=0);
                        debris.names=*a.pieceNames;
                        debris.poses.assign(a.capturedPose.begin(),a.capturedPose.end());
                        debris.motion=motion;debris.motion.position=u.worldPosition;
                        if(size_t(piece)<debris.poses.size())for(unsigned axis=0;axis<3;++axis)
                            debris.motion.rotation[axis]=uint16_t(int32_t(std::lround(
                                debris.poses[size_t(piece)].rot[axis]*65536.f/6.28318530717959f)));
                        debris.tick=front().gameTick;debris.player=u.player;
                        if(a.effectQueryVm && u.type) {
                            a.effectQueryVm->call("QueryBlood",{-1});
                            const auto& output=a.effectQueryVm->lastLocals();
                            if(!output.empty() && output[0]!=-1)
                                for(unsigned i=0;i<100;++i)
                                    debris.blood.push_back(ta::RetailBloodParticle::emit(
                                        u.worldPosition,u.type->bloodColors,
                                        [&]{return ta::sim::retailCrtRandom(smokeRandom_);}));
                        }
                        detachedPieces_.push_back(std::move(debris));
                        accepted=true;
                    }
                }
            }
        }
        // Retail retains the legacy SMOKE/FIRE bits in the debris descriptor,
        // but they create neither immediate bursts nor attached emitters here.
        // The native attached blood emitter is gated separately by its script.
        loadExplosionClasses();
        // Native 50dd20 requests each set class bit independently. The first
        // nine entries in explosions.tdf are the script's numeric class table.
        for(size_t cls=0;cls<9 && cls<explosionClassOrder_.size();++cls)
            if(uint32_t(flags)&(0x100u<<cls)) {
                spawnEffect(explosionClassOrder_[cls],x,z,dAlt);
                if(cls<4)explosionGlows_.push_back({x,z,dAlt,front().gameTick,unsigned(std::min(cls,size_t(2)))});
            }
        return accepted;
    }

    // Corpse mesh: once the death anim finishes, the body draws as the FBI
    // corpse feature's `object=` model (arasword_dead etc.) through the normal
    // unit pipeline -- same swap trick as the veteran meshes. The old VM's piece
    // names don't match the corpse skeleton, so it renders in rest pose.
    void GameView::maybeSwapCorpseModel(const UnitR& u) {
        if (!u.type) return;
        int ct = u.corpseFeat;   // corpse OR statue def, resolved by the sim
        if (ct < 0) return;
        const std::string& obj = worldTables().featureTypes()[size_t(ct)].object;
        if (obj.empty()) return;
        auto it = unitType_.find(u.id);
        if (it == unitType_.end() || it->second == obj) return;
        if (!visuals_.count(obj)) {
            try {
                // Static ruins can put their entire mesh in the root object.
                // Only its selection polygon is a ground reference plate.
                loadVisual(obj, false);
            } catch (const std::exception&) { return; }   // no corpse mesh: keep pose
        }
        unitType_.set(u.id, obj);   // the corpse mesh from now on (registration unchanged)
    }

    void GameView::maybeSwapVeteranModel(const UnitR& u) {
        if (!u.type || u.veteran < 10 || u.type->veteranModel.empty()) return;
        const std::string& vm = u.type->veteranModel;
        auto it = unitType_.find(u.id);
        if (it == unitType_.end() || it->second == vm) return;   // not drawn yet / done
        if (!visuals_.count(vm)) {
            try {
                loadVisual(vm);   // with its PieceMeta tree -- see loadVisual
            } catch (const std::exception&) { return; }   // no promoted mesh: keep base
        }
        unitType_.set(u.id, vm);   // draw the promoted mesh from now on
        if(auto anim=anims_.find(u.id);anim!=anims_.end())
            configureExplosionHierarchy(anim->second,visuals_.at(vm).model.root);
    }

    void GameView::configureExplosionHierarchy(Anim& a,const ta::tdo::Object& root) {
        if(!a.vm)return;
        std::vector<std::vector<int>> descendants(a.vm->file().pieces.size());
        const auto visit=[&](auto&& self,const ta::tdo::Object& object,std::vector<int> ancestors)->void {
            const int index= [&] {
                const auto name=ta::hpi::MountSet::key(object.name);
                for(size_t i=0;i<a.vm->file().pieces.size();++i)
                    if(ta::hpi::MountSet::key(a.vm->file().pieces[i])==name)return int(i);
                return -1;
            }();
            if(index>=0) {
                for(int parent:ancestors)descendants[size_t(parent)].push_back(index);
                ancestors.push_back(index);
            }
            for(const auto& child:object.children)self(self,child,ancestors);
        };
        visit(visit,root,{});
        a.vm->setExplosionDescendants(std::move(descendants));
    }

    void GameView::registerUnit(int id, const ta::sim::UnitType* type) {
        const std::string& typeId = type->id;
        if (!visuals_.count(typeId)) {
            try {
                loadVisual(typeId);
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
                cc.explosionReachability=ta::cob::explosionReachability(*cc.file);
                cc.gaitCode=ta::cob::gaitCode(*cc.file);
                for (const auto& p : cc.file->pieces) {
                    std::string n = p;
                    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                    cc.pieceNames.push_back(n);
                }
                cc.pieceVertices.resize(cc.pieceNames.size());
                auto appendModel=[&](auto&& self,const ta::tdo::Object& object,int parent)->void {
                    auto name=object.name;
                    std::transform(name.begin(),name.end(),name.begin(),::tolower);
                    auto found=std::find(cc.pieceNames.begin(),cc.pieceNames.end(),name);
                    const int piece=found==cc.pieceNames.end() ? -1 : int(found-cc.pieceNames.begin());
                    const int index=int(cc.modelPieces.size());
                    const bool first=piece<0 || std::none_of(cc.modelPieces.begin(),cc.modelPieces.end(),
                        [&](const auto& model) {return model.scriptPiece==piece;});
                    cc.modelPieces.push_back({object.offsetRaw,parent,piece});
                    // The first object named by the piece, as the sim (and the
                    // posed chain retailPieceDrawnCenter walks) uses it.
                    if(piece>=0 && first) {
                        auto& vertices=cc.pieceVertices[size_t(piece)];
                        vertices=object.verticesRaw;
                        // Mirrored authored vertices, as the model loader installs
                        // them: emissions read the first two, and SweetSpot (4dd2a0)
                        // bounds them once posed (retailPieceDrawnCenter).
                        for(auto& vertex:vertices) for(int axis:{0,2})
                            vertex[size_t(axis)]=std::bit_cast<int32_t>(0u-uint32_t(vertex[size_t(axis)]));
                    }
                    for(const auto& child:object.children)self(self,child,index);
                };
                appendModel(appendModel,visuals_.at(typeId).model.root,-1);
                cc.hasAim = cc.file->scriptIndex("AimPrimary") >= 0 || cc.file->scriptIndex("AimSecondary") >= 0 ||
                            cc.file->scriptIndex("AimTertiary") >= 0;
                cc.hasCloakAnim = cc.file->scriptIndex("StartCloaking") >= 0;
                cc.hasFlinch = cc.file->scriptIndex("HitByWeapon") >= 0;
                cc.hasWind = cc.file->scriptIndex("WindChange") >= 0;
                cc.hasFlightSM = cc.file->scriptIndex("BeginFlight") >= 0;
                cc.hasActivate = cc.file->scriptIndex("Activate") >= 0;
                cc.hasTurnDir = cc.file->scriptIndex("TurnDirection") >= 0;
                ci = cobCache_.emplace(typeId, std::move(cc)).first;
            }
            a.pieceNames = &ci->second.pieceNames;
            a.hasCloakAnim = ci->second.hasCloakAnim;
            a.hasAim = ci->second.hasAim;
            a.hasFlinch = ci->second.hasFlinch;
            a.hasWind = ci->second.hasWind;
            a.hasFlightSM = ci->second.hasFlightSM;
            a.hasActivate = ci->second.hasActivate;
            a.hasTurnDir = ci->second.hasTurnDir;
            // Every unit starts inactive (511990); the mirror below replays an
            // active snapshot's Activate, a gate's that became active before it
            // was visible and an activatewhenbuilt unit's (512301 / 429a8d) too.
            a.active = false;
            a.vm = std::make_unique<ta::cob::Vm>(ci->second.file);
            a.vm->enableRetailAnimation();
            {
                // Every display VM used to start from the same RAND seed, so
                // bodies of a type rolled one display-only sequence (idle loops,
                // smoke). Death sounds are the simulation's now
                // (unitSoundQueue_); this VM is never hashed, and a per-unit
                // stream keeps its remaining rolls apart.
                uint64_t h=uint64_t(uint32_t(id))+0x9e3779b97f4a7c15ull;
                h=(h^(h>>30))*0xbf58476d1ce4e5b9ull;h=(h^(h>>27))*0x94d049bb133111ebull;
                a.vm->seedRandom(uint32_t(h^(h>>31)));
            }
            a.explosionReachability=ci->second.explosionReachability;
            a.vm->setGaitCode(ci->second.gaitCode);
            configureExplosionHierarchy(a,visuals_.at(typeId).model.root);
            // TA COB unit-state queries answered from the sim.
            int unitId = id;
            a.vm->onGet = [this, unitId](int32_t valId,
                                         const std::vector<int32_t>&) -> int32_t {
                // Ticked on the worker pool (animFrame) -- read the pinned render snapshot,
                // never live world_, which the sim thread mutates concurrently under Stage B.
                const auto* su = frameUnitP(unitId);
                if (!su || !su->type) return 0;
                // TotalA.exe's ids 1..20 only (0x480770; client/displayget.h).
                return ta::client::displayScriptGet(*su, valId);
            };
            a.flying = type && type->canFly;
        } catch (const std::exception&) { /* unit stays unanimated */ }
        if (a.vm) {
            Anim& st = anims_[id] = std::move(a);
            // The VM is ticked on the worker pool, so emit-sfx only stashes into this
            // unit's own buffer (std::map nodes are pointer-stable); the main thread
            // drains it into effects_ after the parallel tick.
            st.vm->onEmitSfx = [buf = &st.pendingSfx,
                    points=&st.pendingPoints,vm=st.vm.get(),cache=&cobCache_.at(typeId),
                    flying=st.flying](int piece, int32_t sfx) {
                // Attached death flames now come from the same authoritative
                // instruction stream as live emissions, exactly once per tick.
                if(sfx>=260 && sfx<=262)return;
                if(!flying && sfx>=2 && sfx<=5) {
                    if(piece<0 || size_t(piece)>=cache->pieceVertices.size() ||
                       cache->pieceVertices[size_t(piece)].size()<2)return;
                    ta::sim::World::ScriptEmission event;event.code=sfx;event.piece=piece;
                    std::copy_n(cache->pieceVertices[size_t(piece)].begin(),2,event.vertices.begin());
                    int index=-1;
                    for(size_t i=0;i<cache->modelPieces.size();++i)
                        if(cache->modelPieces[i].scriptPiece==piece) {index=int(i);break;}
                    const auto states=vm->retailPieces();
                    for(;index>=0;index=cache->modelPieces[size_t(index)].parent) {
                        const auto& node=cache->modelPieces[size_t(index)];
                        ta::cob::EmissionPose pose;pose.offset=node.offset;
                        if(node.scriptPiece>=0 && size_t(node.scriptPiece)<states.size())
                            for(size_t axis=0;axis<3;++axis) {
                                pose.move[axis]=states[size_t(node.scriptPiece)].move[axis];
                                pose.turn[axis]=uint16_t(states[size_t(node.scriptPiece)].turn[axis]);
                            }
                        event.pose.push_back(pose);
                    }
                    std::reverse(event.pose.begin(),event.pose.end());
                    points->push_back(std::move(event));return;
                }
                buf->push_back({piece, sfx});
            };
            st.vm->onPlaySound = [state = &st](int32_t idx,int32_t flags) {
                // Once dying (Killed, Dying and every thread still running), the
                // sounds are the simulation's: its VM rolled them from the shared
                // game stream (unitSoundQueue_). This VM's local RAND would differ.
                if (!state->dying) state->pendingSnd.emplace_back(idx,flags);
            };
            st.vm->onSetUnitValue = [state=&st](int32_t valueId,int32_t) {
                if(ta::retailOwnerVmStopsOnSetUnitValue(valueId)) {
                    state->ownerVmStopRequested=true;
                }
            };
            st.vm->onExplode = [this,id, state=&st, vm=st.vm.get()](int piece, int32_t flags) {
                const auto* unit=frameUnitP(id);
                if(!unit || !unit->type || !unitShownR(*unit))return false;
                std::vector<ta::cob::PieceState> pose;
                const auto states=vm->retailPieces();
                pose.resize(states.size());
                constexpr float angle=2*3.14159265358979f/65536.0f;
                for(size_t i=0;i<states.size();++i) {
                    pose[i].visible=states[i].visible;
                    pose[i].cached=states[i].cached;
                    pose[i].shaded=states[i].shaded;
                    pose[i].rendered=states[i].rendered;
                    for(size_t axis=0;axis<3;++axis) {
                        pose[i].move[axis]=float(states[i].move[axis])/65536.0f;
                        pose[i].rot[axis]=float(states[i].turn[axis])*angle;
                    }
                }
                Anim snapshot;snapshot.pieceNames=state->pieceNames;
                snapshot.capturedPose=pose;snapshot.effectQueryVm=vm;
                return explodePiece(*unit,snapshot,piece,flags);
            };
            unitType_.set(id,typeId);
            // Install effect sinks before immediate retail notifications run.
            st.vm->start("Create");
            st.vm->start("SetMaxReloadTime",{type->maxWeaponReloadMs});
            // TotalA.exe 0x437840, run at the unit's creation: an extractor's script gets
            // SetSpeed(sum over its footprint of (cell metal + 1)) (0x4378eb..0x437902),
            // the value its arms spin from. Footprint as the settle reads it (taeconomy.cpp).
            if (type->extractsMetal > 0)
                if (const auto* unit = world_.unit(id)) {   // the frame may not hold it yet
                    const int cx = int(unit->x.toFloat()) / 16 - type->footX / 2;
                    const int cz = int(unit->z.toFloat()) / 16 - type->footZ / 2;
                    int32_t sum = 0;
                    for (int dz = 0; dz < type->footZ; ++dz)
                        for (int dx = 0; dx < type->footX; ++dx) sum += world_.cellMetal(cx + dx, cz + dz) + 1;
                    st.vm->start("SetSpeed", {int32_t(int16_t(sum))});
                }
            // A wind generator hears of the wind at its next change (0x437910 runs on
            // the change flag only), not at its creation.
            st.windGen = front().wind.generation;
        }
        unitType_.set(id, typeId);
    }

    int GameView::spawn(const std::string& typeId, float x, float z, float heading, int player) {
        const auto* type = registry_.find(typeId);
        if (!type) return -1;
        int id = writableWorld().spawn(type, x, z, heading, player);
        // registerUnit mutates the client render maps (visuals_/cobCache_/anims_/unitType_),
        // which the render thread + the animFrame VM pool read/iterate. It must run ONLY on
        // the main thread. When spawn() is reached from the SIM WORKER (summonGod / mission
        // scripted reinforcements, all inside simStep), skip it: the main thread's
        // cosmeticStep lazily registers every live snapshot unit, so it is redundant there.
        if (std::this_thread::get_id() == mainThreadId_) {
            registerUnit(id, type);
            if (!unitType_.count(id)) return -1;
        }
        return id;
    }

    void GameView::loadTextures() {
        assetGeneration_.bump();   // the model texture registry changes below
        // Faction texture banks use their own palettes (palettes/<side>_textures.pcx).
        // The VFS merges base + Iron Plague (cre) texture GAFs into one namespace.
        // Total Annihilation draws every model texture through the one game palette
        // (palettes/palette.pal) with its shade table (palettes/palette.shd).
        std::map<std::string, ta::gaf::Palette> pals;
        try { pals["ta"] = ta::gaf::Palette::fromBytes(vread("palettes/palette.pal"), "palettes/palette.pal"); }
        catch (const std::exception&) {}
        if (!pals.count("ta")) return;   // no palette available
        // The nanoframe's shimmer ramp (TotalA.exe 0x458e56: palette 160..175).
        for (int i = 0; i < 16; ++i) {
            const auto* e = pals.at("ta").rgba[160 + i];
            nanoPalette_[size_t(i)] = SDL_Color{e[0], e[1], e[2], 255};
        }
        // A coloured primitive's index in the nanoframe bitmap (read by the workers).
        for (int i = 0; i < 256; ++i) {
            const auto* e = pals.at("ta").rgba[i];
            nanoGamePalette_[size_t(i)] = uint32_t(e[0]) << 24 | uint32_t(e[1]) << 16 | uint32_t(e[2]) << 8 | 0xffu;
        }
        nanoGamePaletteSet_ = true;
        try {
            const auto gui = ta::gaf::Palette::fromBytes(vread("palettes/guipal.pal"), "palettes/guipal.pal");
            taGuiColourMap_ = ta::taGuiColourMap(gui.rgba, pals.at("ta").rgba);
        } catch (const std::exception&) {
            for (int i = 0; i < 256; ++i) taGuiColourMap_[size_t(i)] = uint8_t(i);
        }
        // Palette lighting banks (workstream M): one palette + shade table per faction
        // bank, shared by every texture of the bank.
        std::map<std::string, uint8_t> bankIndex;
        // TotalA.exe 0x42a440 loads every textures/*.gaf EXCEPT logos.gaf into the
        // texture table; 0x4318c0 loads textures/logos.gaf separately. The 3DO
        // primitive lookup (0x42a182..0x42a298) searches the table first and
        // logos.gaf only when no other bank has the name, so logos.gaf goes last
        // here (first registration wins below).
        std::vector<std::string> texturePaths = vfs_.list("textures");
        const auto isLogos = [](const std::string& path) {
            std::string stem = ta::vpath::stem(path);
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            return stem == "logos";
        };
        std::stable_partition(texturePaths.begin(), texturePaths.end(),
                              [&](const std::string& path) { return !isLogos(path); });
        for (const std::string& path : texturePaths) {
            if (ta::vpath::extension(path) != ".gaf") continue;
            std::string stem = ta::vpath::stem(path);
            std::transform(stem.begin(), stem.end(), stem.begin(), ::tolower);
            const bool logosBank = stem == "logos";
            const auto* pal = &pals.at("ta");
            auto pit = pals.find("ta");
            try {
                const auto bytes=vread(path);
                // Decode through an index-identity palette as well: palette colours
                // are not unique, so reconstructing indices from RGB loses authored
                // shade mappings. This also preserves composite-frame transparency.
                ta::gaf::Palette indexPalette{};
                for(int i=0;i<256;++i) {
                    indexPalette.rgba[i][0]=uint8_t(i);
                    indexPalette.rgba[i][3]=255;
                }
                auto indexed=ta::gaf::load(bytes,indexPalette,5,path);
                const std::string bank="ta";
                auto bankSlot=bankIndex.find(bank);
                if(bankSlot==bankIndex.end()) {
                    // A bank without its shade table gets no palette lighting (as before).
                    const std::string shadePath="palettes/palette.shd";
                    std::vector<uint8_t> shades;
                    if(vfs_.has(shadePath)) shades=vread(shadePath);
                    uint8_t slot=0xff;
                    if(shades.size()==ta::render::palette::kShadeTableBytes &&
                       paletteBanks_.size()<size_t(ta::render::palette::kMaxBanks)) {
                        ta::render::palette::Bank entry;
                        std::memcpy(entry.palette,pal->rgba,sizeof entry.palette);
                        entry.shades=std::move(shades);
                        slot=uint8_t(paletteBanks_.size());
                        paletteBanks_.push_back(std::move(entry));
                    }
                    bankSlot=bankIndex.emplace(bank,slot).first;
                }
                size_t sequenceIndex=0;
                for (auto& seq : ta::gaf::load(bytes, *pal, 5, path)) {
                    auto& indexSequence=indexed.at(sequenceIndex++);
                    if (seq.frames.empty()) continue;
                    std::string name = seq.name;
                    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                    if (textures_.count(name)) continue;
                    if(bankSlot->second!=0xff && std::all_of(seq.frames.begin(),seq.frames.end(),
                        [](const auto& frame){return frame.encoding==0 || frame.encoding==1;})) {
                        PaletteTextureSource source;
                        source.bank=bankSlot->second;
                        for(const auto& frame:indexSequence.frames) {
                            // Index-identity decode: R = palette index, A = coverage.
                            ta::render::palette::Frame la;
                            la.width=frame.width;la.height=frame.height;
                            la.la.resize(size_t(frame.width)*size_t(frame.height)*2);
                            for(size_t pixel=0;pixel*4+3<frame.rgba.size() && pixel*2+1<la.la.size();++pixel) {
                                la.la[pixel*2]=frame.rgba[pixel*4];
                                la.la[pixel*2+1]=frame.rgba[pixel*4+3];
                            }
                            source.frames.push_back(std::move(la));
                        }
                        paletteTextureSources_.emplace(name,std::move(source));
                    }
                    // TotalA.exe 0x42a1e7..0x42a288: a texture found in logos.gaf
                    // with exactly 10 frames is a player-colour texture (primitive
                    // flag 4: one frame per lobby colour, never animated) -- that is
                    // every 10-frame sequence there (colorslt, Solid1a, ArmLogo...,
                    // not only names containing "logo"). Any other multi-frame
                    // texture (flag 2 without 4) joins the "Animplay Pointers" list
                    // and animates (glow, flashing01/02, CorPlat, AlienGlo, onoff01).
                    const size_t n = seq.frames.size();
                    const bool playerColours = logosBank && n == 10;
                    if (n > 1 && !playerColours) {
                        animatedTex_.insert(name);
                        auto& animation = modelTextureAnimations_[name];
                        animation.loop = seq.loopFlag != 0;
                        for (const auto& frame : seq.frames)
                            animation.durations.push_back(frame.retailDelayTicks);
                    }
                    // Sample the actual 10 player-colour RGBs once, from a logo/
                    // insignia texture (mostly pure player colour), so the HUD and
                    // minimap can match whatever colour a player renders in.
                    if (!sampledColors_ && playerColours &&
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
                    std::vector<ShadowMaskFrame> masks(n);
                    bool hasMask=false;
                    for (size_t i = 0; i < n; ++i) {
                        auto& f = seq.frames[i];
                        if (f.width == 0 || f.height == 0) break;
                        SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                           SDL_TEXTUREACCESS_STATIC,
                                                           f.width, f.height);
                        SDL_UpdateTexture(t, nullptr, f.rgba.data(), f.width * 4);
                        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                        frames.push_back(t);
                        bool cutout=false;
                        for (size_t k=3;k<f.rgba.size();k+=4)
                            if (f.rgba[k]!=255) { cutout=true; break; }
                        if (cutout) {
                            // MOD leaves white texels untouched; opaque texels
                            // multiply the ground by the usual shadow level.
                            masks[i]=addShadowMask(f.rgba,f.width,f.height);
                            hasMask |= masks[i].texture!=nullptr;
                        }
                    }
                    if (hasMask) shadowMasks_[name]=std::move(masks);
                    if (!frames.empty()) textures_[name] = std::move(frames);
                    // A unit whose animated textures are held (TotalA.exe 0x458845:
                    // a structure not activated, a mobile unit not moving) draws
                    // the sequence's first frame (0x458635..0x458646, 0x4b7f30(seq,0)).
                    // It gets its own single-frame entry, so the atlas keeps both.
                    if (animatedTex_.count(name) && seq.frames[0].width && seq.frames[0].height) {
                        const auto& f = seq.frames[0];
                        const std::string still = name + kStillTextureSuffix;
                        SDL_Texture* t = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32,
                                                           SDL_TEXTUREACCESS_STATIC, f.width, f.height);
                        if (t) {
                            SDL_UpdateTexture(t, nullptr, f.rgba.data(), f.width * 4);
                            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
                            textures_[still] = {t};
                        }
                        if (auto source = paletteTextureSources_.find(name);
                            source != paletteTextureSources_.end() && !source->second.frames.empty()) {
                            PaletteTextureSource first;
                            first.bank = source->second.bank;
                            first.frames.push_back(source->second.frames[0]);
                            paletteTextureSources_.emplace(still, std::move(first));
                        }
                    }
                }
            } catch (const std::exception&) {}
        }
    }

    GameView::ShadowMaskFrame GameView::addShadowMask(std::span<const uint8_t> rgba,
                                                      int width,int height) {
        SDL_RendererInfo info{};SDL_GetRendererInfo(ren_,&info);
        const auto makeCoverage=[&](int w,int h) -> SDL_Texture* {
            if(!((info.flags&SDL_RENDERER_ACCELERATED) || ta::devFlag("TA_SHADOW_ATLAS_SOFTWARE")) ||
               !SDL_RenderTargetSupported(ren_))return nullptr;
            constexpr size_t limit=16u*1024u*1024u;
            size_t used=0;
            for(const auto& page:shadowMaskPages_)
                if(page.coverage)used+=size_t(page.width)*page.height*4;
            const size_t bytes=size_t(w)*h*4;
            if(bytes>limit || used>limit-bytes || gpuvram::blocked() || !gpuvram::wouldFit(bytes))return nullptr;
            auto* texture=gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);
            if(texture)SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND);
            return texture;
        };
        const auto standalone=[&]() -> ShadowMaskFrame {
            auto* texture=gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STATIC,width,height);
            if(!texture)return {};
            const auto pixels=ta::shadowMaskPixels(rgba,kShadowLevel);
            if(SDL_UpdateTexture(texture,nullptr,pixels.data(),width*4)!=0) {
                gpuvram::destroy(texture);return {};
            }
            SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_MOD);
            // A full page cannot receive later atlas frames. Ownership remains
            // common to both paths so GPU reset destroys every texture once.
            auto* coverage=makeCoverage(width,height);
            if(coverage) {
                const auto alpha=ta::shadowCoveragePixels(rgba);
                if(SDL_UpdateTexture(coverage,nullptr,alpha.data(),width*4)!=0) {
                    gpuvram::destroy(coverage);coverage=nullptr;
                } else shadowCoverageTextures_[texture]=coverage;
            }
            shadowMaskPages_.push_back({texture,width,height,width,height,0,coverage});
            return {texture,{0,0,1,1}};
        };
        // Keep the software rasterizer's existing standalone sampling path;
        // its batching/sampling behavior differs from the accelerated backend.
        if(!(info.flags&SDL_RENDERER_ACCELERATED))return standalone();
        const int paddedWidth=width+2,paddedHeight=height+2;
        ShadowMaskPage* page=nullptr;
        for(auto& candidate:shadowMaskPages_) {
            if(candidate.x+paddedWidth>candidate.width) {
                candidate.x=0;candidate.y+=candidate.rowHeight;candidate.rowHeight=0;
            }
            if(candidate.y+paddedHeight<=candidate.height && paddedWidth<=candidate.width) {
                page=&candidate;break;
            }
        }
        if(!page) {
            const int maxWidth=info.max_texture_width>0 ? info.max_texture_width : 1024;
            const int maxHeight=info.max_texture_height>0 ? info.max_texture_height : 1024;
            if(paddedWidth>maxWidth || paddedHeight>maxHeight)return standalone();
            const int pageWidth=std::max(paddedWidth,std::min(1024,maxWidth));
            const int pageHeight=std::max(paddedHeight,std::min(1024,maxHeight));
            auto* texture=gpuvram::create(ren_,SDL_PIXELFORMAT_RGBA32,
                SDL_TEXTUREACCESS_STATIC,pageWidth,pageHeight);
            if(!texture)return standalone();
            SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_MOD);
            auto* coverage=makeCoverage(pageWidth,pageHeight);
            if(coverage)shadowCoverageTextures_[texture]=coverage;
            shadowMaskPages_.push_back({texture,pageWidth,pageHeight,0,0,0,coverage});
            page=&shadowMaskPages_.back();
        }
        // Replicate a one-texel border: UV endpoints retain the standalone
        // texture's clamp behavior instead of sampling its atlas neighbor.
        const auto pixels=ta::paddedShadowMaskPixels(rgba,width,height,kShadowLevel);
        const SDL_Rect rect{page->x,page->y,paddedWidth,paddedHeight};
        if(SDL_UpdateTexture(page->texture,&rect,pixels.data(),paddedWidth*4)!=0)return standalone();
        if(page->coverage) {
            const auto alpha=ta::paddedShadowCoveragePixels(rgba,width,height);
            if(SDL_UpdateTexture(page->coverage,&rect,alpha.data(),paddedWidth*4)!=0) {
                shadowCoverageTextures_.erase(page->texture);
                gpuvram::destroy(page->coverage);page->coverage=nullptr;
            }
        }
        ShadowMaskFrame frame{page->texture,{float(page->x+1)/page->width,
            float(page->y+1)/page->height,float(width)/page->width,float(height)/page->height}};
        page->x+=paddedWidth;page->rowHeight=std::max(page->rowHeight,paddedHeight);
        return frame;
    }

    const ta::cob::PieceState* GameView::pieceFor(const Anim* a, const std::string& objName) const {
        if (!a || !a->pieceNames) return nullptr;
        const auto poses=!a->capturedPose.empty() ? a->capturedPose :
            a->vm ? std::span<const ta::cob::PieceState>(a->vm->pieces()) :
                    std::span<const ta::cob::PieceState>{};
        std::string n = objName;
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        for (size_t i = 0; i < a->pieceNames->size(); ++i)
            if ((*a->pieceNames)[i] == n && i<poses.size()) return &poses[i];
        return nullptr;
    }



    bool GameView::dancing(const UnitR& u) const {
        return u.disco && u.type && !u.type->isStructure() && frameDiscoActive(u.player);
    }

    bool GameView::headbanging(const UnitR& u) const {
        return u.headbang && u.type && !u.type->isStructure() && frameHeadbangActive(u.player);
    }

    ta::tdf::Node GameView::vtdf(const std::string& p) const {
        auto b = vfs_.read(p);
        return ta::tdf::parseText(std::string(b.begin(), b.end()), p);
    }

    std::string GameView::mapSibling(const char* ext) const {
        return ta::vpath::replaceExtension(mapPath_,ext);
    }

    void GameView::remountMods() {
        if (installRoot_.empty()) return;
        // Cosmetic mods are this player's own (never sent, never hashed); game mods
        // come from the room's verified package (multiplayer and single player alike).
        // The always-enabled cosmetic packs (mods/cosmetic/always_enabled/) load in
        // every game, campaigns included (--mods none|game leaves them out with the
        // cosmetic packs).
        const auto cosmetic=mods_.cosmetic && settings_?settings_->cosmeticModPacks:std::vector<std::string>{};
        std::vector<std::string> skipped;
        if(mp_) vfs_=mp_->modVfs(cosmetic,mods_.cosmetic,&skipped);
        else {
            // Debug local harnesses (no server): this launch's own always-enabled and
            // ticked game packs (the single-player set), as a room it hosted would apply;
            // none in a mission.
            const auto root=ta::u8path(installRoot_);
            const bool gamePacks=mods_.game && missionStem_.empty();
            auto game=gamePacks && settings_?settings_->spRoom.gamePacks:std::vector<std::string>{};
            const auto have=ta::hpi::modPacks(root,ta::hpi::ModKind::Game);
            std::erase_if(game,[&](const auto& n){return !std::binary_search(have.begin(),have.end(),n);});
            if(gamePacks)for(const auto& name:ta::hpi::alwaysEnabledPacks(root,ta::hpi::ModKind::Game))game.push_back(name);
            std::sort(game.begin(),game.end());game.erase(std::unique(game.begin(),game.end()),game.end());
            if(game.size()>64)game.resize(64);
            vfs_=ta::hpi::mountPlayVfs(root,ta::hpi::modFiles(root,ta::hpi::ModKind::Game,game),
                missionStem_.empty()?cosmetic:std::vector<std::string>{},mods_.cosmetic,&skipped);
        }
        // A cosmetic mod left out (it cannot load, or its anims/ art would change
        // gameplay timing) is named in the game's chat rather than stopping it.
        alwaysOnLoaded_.clear();
        if(mods_.cosmetic)for(const auto& name:ta::hpi::alwaysEnabledPacks(ta::u8path(installRoot_),ta::hpi::ModKind::Cosmetic))
            if(std::none_of(skipped.begin(),skipped.end(),[&](const std::string& why){return why.starts_with(name+" (");}))
                alwaysOnLoaded_.push_back(name);
        for(const auto& why:skipped) {
            std::fprintf(stderr,"mods: cosmetic mod left out: %s\n",why.c_str());
            gameChat_.push_back({"SYSTEM","COSMETIC MOD LEFT OUT: "+why,0,true});
        }
        // Lobby choices are applied before match setup; discard art/audio cached
        // while the lobby still used the previous data set.
        destroyGpuTextures();anims_.clear();cobCache_.clear();visuals_.clear();
        atlasRect_.clear();atlasLaidOut_=false;modelTextureAnimations_.clear();animatedTex_.clear();
        hitBoxes_.clear();ringBoxes_.clear();kingdomPals_.clear();pickBoundsValid_=false;pickRegistered_={};
        featureDefs_.clear();featurePals_.clear();cursorsInit_=false;
        loadTextures();loadOrderButtons();
        loadInterfaceFonts();
        sounds_.reload(vfs_);soundClasses_.load(vfs_);musicMode_=0;
        registry_ = ta::sim::TypeRegistry{};
        ta::sim::setupRegistry(registry_, vfs_);
    }

    float GameView::birthProgress(int id) const {
        auto it = birthFx_.find(id);
        return it == birthFx_.end() ? 1.0f
                                    : std::clamp(it->second / kBirthFxDur, 0.0f, 1.0f);
    }

    // Retail's banner for speed level d: "GAME SPEED NORMAL" or "GAME SPEED +5".
    static std::string speedNotice(int d) {
        return "GAME SPEED " + ta::speedLevelText(d) + (d > 10 ? " (DEBUG)" : "");
    }

    float GameView::speedMult() const {
        if (replayMode_) return ta::replaybar::speedAt(replaySpeed_);
        return mp_ ? 1.0f : float(gameSpeed_ + 10) / 10.0f;   // retail: linear, (d+10)/10
    }

    SDL_Color GameView::playerColor(int player) const {
        int s = playerColorSlot(player);
        return playerColors_[(s >= 0 && s < 10) ? s : 0];
    }

    // Terrain height at a world point with retail's sampler (0x511170, the one
    // the sim's unit Y uses): height k belongs to the cell CORNER at 16k, and each
    // interpolation truncates. Clamped to the map so the edge rows hold instead of
    // reading the sampler's -1 sentinel.
    float GameView::rawHeight(float wx, float wz) {
        const auto& m = mapView_.map();
        return ta::client::lift::terrainHeight(m.heights.data(), m.width, m.height, wx, wz);
    }

    // The screen-Y offset (down positive) of a wading or floating body from the
    // terrain lift. Retail stands a canhover unit at max(terrain, sea - waterline)
    // and a floater at sea - waterline (0x51b220 / 0x51b2fa), so over deep water
    // the body rides near the surface, not on the seabed.
    float GameView::waterSink(const ta::sim::UnitType* t, float wx, float wz) {
        if (!t || !(t->canHover || t->floater)) return 0.0f;
        const auto& m = mapView_.map();
        if (m.heights.empty()) return 0.0f;
        const float terrain = rawHeight(wx, wz);
        const float body = ta::client::lift::bodyHeight(terrain, m.seaLevel, t->waterline,
                                                         t->canHover, t->floater);
        return std::floor(terrain * kHeightScale_) - std::floor(body * kHeightScale_);
    }

    float GameView::heightAbove(float wx, float wz) {
        const auto& m = mapView_.map();
        if (m.heights.empty() || m.width <= 0) return 0.0f;
        // Geometry workers call this concurrently. The debug scales are read on
        // the render thread before dispatch; sample without shared memo state.
        if (!liftReady_) {
            liftReady_ = true;
            long hist[256] = {0};
            for (uint8_t v : m.heights) hist[v]++;
            for (int i = 1; i < 256; ++i) if (hist[i] > hist[modalHeight_]) modalHeight_ = i;
            if (const char* e = ta::devEnv("TA_HSCALE")) kHeightScale_ = std::stof(e);
            if (const char* e = ta::devEnv("TA_HSCALEX")) kHeightScaleX_ = std::stof(e);
            if (ta::devEnv("TA_HDEBUG")) showHDebug_ = true;
        }
        return rawHeight(wx, wz);
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
                const int kCells = 8;                    // 8 cells of 16 units = one 128-unit sector
        flyGroundW_ = (m.width + kCells - 1) / kCells;
        flyGroundH_ = (m.height + kCells - 1) / kCells;
        std::vector<uint8_t> maxv(size_t(flyGroundW_) * size_t(flyGroundH_), 0);
        for (int z = 0; z < m.height; ++z)
            for (int x = 0; x < m.width; ++x) {
                const int h = std::max<int>(m.heights[size_t(z) * m.width + x], m.seaLevel);
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

    SDL_FPoint GameView::unitScreen(const UnitR& u) {
        float zm = mapView_.zoom();
        float ix, iz, ih; interpPose(u, ix, iz, ih);   // match the gliding model position
        // uLiftX/uLiftY, NOT terrainLift: a flyer rides the coarse dilated datum,
        // and picking has to use the same one the body is drawn with or you click
        // where the unit is not. uLiftY already carries the waterline sink the drawn
        // body has, so a wading god or a ship is clicked where you see it.
        return {(ix - mapView_.offX()) * zm - uLiftX(u) * zm,
                (iz - mapView_.offY()) * zm - uLiftY(u) * zm - altLift(u) * zm - 12.0f * zm};
    }

    const SDL_FRect& GameView::unitHitBox(const ta::sim::UnitType* type) {
        auto it = hitBoxes_.find(type->id);
        if (it != hitBoxes_.end()) return it->second;
        SDL_FRect box = kFallbackHitBox;   // model not loaded
        auto vt = visuals_.find(type->id);
        if (vt != visuals_.end()) {
            std::vector<Tri> scratch;
            // Buildings also carry a birth heading. Cache bounds that cover
            // every heading, as for movers, so rotated bodies remain selectable.
            RadialExtent ext;
            collect(scratch, nullptr, vt->second.model.root, Xform{}, nullptr,
                    0.0f, 0, false, true, false, &ext);
            if (ext.any)
                box = SDL_FRect{-ext.maxR, ext.minY, 2.0f * ext.maxR, ext.maxY-ext.minY};
        }
        return hitBoxes_.emplace(type->id, box).first->second;
    }


    const GameView::RingBox& GameView::unitRingBox(const ta::sim::UnitType* type) {
        auto it = ringBoxes_.find(type->id);
        if (it != ringBoxes_.end()) return it->second;
        RingBox rb;   // defaults cover a model that never loaded
        auto vt = visuals_.find(type->id);
        if (vt != visuals_.end()) rb = selectionRingBox(vt->second.model.root);
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
        //
        // Over water the surface clicked is the WATER, not the seabed: a ship or a
        // wading god rides at sea - waterline (waterSink), so inverting through the
        // seabed would send it to a point drawn (sea - seabed)/2 px north of the
        // cursor. Flooring the sampled height at sea level puts the order where the
        // ship will be drawn, to within waterline/2. A land unit wading the shallows
        // is then off by (sea - seabed)/2, small where it can walk. (Inferred:
        // retail's own click inversion over water has not been traced in the icd.)
        const float sea = float(int(uint8_t(m.seaLevel)));
        const auto surfaceH = [&](float x, float z) { return std::max(heightAbove(x, z), sea); };
        const float maxH = 255.0f;
        float bestH = 0.0f;
        float prevDiff = surfaceH(cwx, cwz);   // actualH - 0 at h = 0
        const float step = 1.0f;
        for (float h = step; h <= maxH + 2; h += step) {
            float ax = cwx + h * kHeightScaleX_;
            float az = cwz + h * kHeightScale_;
            float diff = surfaceH(ax, az) - h;
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
        terrainLift(wx, wz);   // ensure kHeightScale_ is initialised
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
            float proj = float(h) * kHeightScale_;
            // Height k belongs to the cell's north-west CORNER at 16k (retail's
            // sampler, terrainlift.h), so the wall's top edge is at z = 16cz.
            float wy = (float(cz) * 16 - mapView_.offY()) * zm - proj * zm;
            if (wy < best) best = wy;
        }
        return best;
    }

    void GameView::simWorkerLoop() {
        // PIN SIMULATION THREADS: this thread is the simulation's first (latchSimThreads).
        if (simPin_) ta::sim::placeThisThread(simCpus_.empty() ? -1 : simCpus_.front());
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
            uint64_t hash = 0, pos = 0;
            {
#ifndef NDEBUG
                PaceSpan jobSpan(paceJobWallNs_, &paceJobCpuNs_);
#endif
                std::lock_guard<std::mutex> lk(simMutex_);
                for (const auto& c : job.bundle.cmds) apply(c);
                for (const auto& e : job.bundle.events) applyEvent(e);
                simStep(1.0f / 30.0f);   // world_.tick + captureFrame (publishes a snapshot)
                if (job.wantHash) hash = reportedHash(job.spectator, job.tick);
                if (job.wantPos) pos = world_.posDigest();
            }
            if (job.wantHash) {
                std::lock_guard<std::mutex> lk(outboxMutex_);
                simOutbox_.push_back({job.tick, hash, pos});
            }
#ifndef NDEBUG
            if (simSlowUs_ > 0) std::this_thread::sleep_for(std::chrono::microseconds(simSlowUs_));
#endif
            simProcessedTick_.store(job.tick, std::memory_order_relaxed);   // for backlog/ack tracking
        }
    }

    void GameView::startSimThread() {
        if (useSimThread_) return;
        // A thread starts on its creator's CPUs: started by the pinned sim worker at its
        // first big capture, the capture pool's threads would all share that one CPU.
        // Pinned sessions start them here instead, on this thread's.
        if (simPin_ && !capturePool_) capturePool_ = std::make_unique<ThreadPool>(kCaptureThreads);
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
        shown_ = -1;
        pinFrameLocked(published_);
    }

    void GameView::pinFrameLocked(int slot) {
        renderReadIdx_ = slot;
        reading_ = slot;
        placePreviewBusy_ = false;   // one failed try_lock per frame at most (canPlacePreview)
        if (!gameStartMs_ && front().gameTick > 0) gameStartMs_ = SDL_GetTicks64();
        if (replayInTransit_) return;   // a frame on the way says nothing about the landing
        // A previously selected enemy must stop exposing its live state on leaving sight.
        // An own unit a mission script takes over drops out (51e01f: the selected
        // bit goes with the selectable one).
        std::erase_if(selection_, [this](int id) {
            const auto* u = frameUnitP(id);
            return !u || !u->alive() || !canPickUnit(*u) ||
                   (!watchOnly() && u->player == localPlayer_ && !u->selectable);
        });
        keepOneWatchOwner();   // a capture or a gift can split a watcher's selection
    }

    int GameView::hudPlayer() const {
        if (!watchOnly()) return localPlayer_;
        if (selection_.empty()) return -1;
        const auto* u = frameUnitP(selection_.front());
        if (u && u->type) return int(u->player);
        return replayInTransit_ ? replayJumpOwner_ : -1;   // not born yet on this frame of a jump
    }

    void GameView::keepOneWatchOwner() {
        if (!watchOnly() || selection_.empty()) return;
        const int owner = hudPlayer();
        std::erase_if(selection_, [&](int id) {
            const auto* u = frameUnitP(id);
            return !u || int(u->player) != owner;
        });
    }

    void GameView::endFrame() {
        std::lock_guard<std::mutex> lk(frameMutex_);
        reading_ = -1;
    }

    bool GameView::presentClockWanted() const {
#ifndef NDEBUG
        // TA_PRESENT=0: the pre-S4 presentation (newest snapshot, wall-clock alpha), for A/B runs.
        if (const char* e = ta::devEnv("TA_PRESENT"); e && e[0] == '0') return false;
#endif
        return isNet() && !replayMode_;
    }

    int GameView::ringSlotLocked(uint32_t tick) const {
        int best = -1;
        for (int i = 0; i < kFrameRing; ++i)
            if (i != writing_ && frameBuf_[i].gen && frameBuf_[i].gameTick == tick &&
                (best < 0 || frameBuf_[i].gen > frameBuf_[best].gen)) best = i;
        return best;
    }

    void GameView::presentFrame() {
        if (!presentClockWanted()) {
            present_ = {};
            beginFrame();
            return;
        }
        auto& c = present_;
        c.on = true;
        c.freeze = c.gate = c.snap = false;
        c.dtd = c.nominal = 0;
        std::lock_guard<std::mutex> lk(frameMutex_);
        const uint32_t newest = frameBuf_[published_].gameTick;
        const uint64_t now = SDL_GetPerformanceCounter();
        const double freq = double(SDL_GetPerformanceFrequency());
        const double dt = c.last ? std::min(0.25, double(now - c.last) / freq) : 0.0;
        c.last = now;
        const double nominalRate = 30.0 * double(std::max(0.1f, animSpeed()));
        if (c.rate <= 0) c.rate = nominalRate;
        // Before the first tick (loading, the pre-game hold) and after the game is over or
        // reset: show the newest snapshot, as beginFrame does.
        if (newest == 0 || newest < c.seen) {
            c = {};
            shown_ = -1;
            pinFrameLocked(published_);
            return;
        }
        // The rate the display time advances at: the game speed, or the measured publish rate when
        // the game runs slower than that (a client- or referee-bound game slows down smoothly
        // instead of freezing between snapshots).
        if (dt > 0 && c.started) c.rate += (double(newest - c.seen) / dt - c.rate) * std::min(1.0, dt / 0.25);
        const double base = std::clamp(c.rate, 0.02 * nominalRate, nominalRate);
        // Publish gaps of the ticks that appeared since the last frame (slots still in the ring),
        // in ticks at that rate: how far the display time moves while no new snapshot arrives.
        if (newest > c.seen) {
            const uint32_t from = std::max(c.seen + 1, newest > uint32_t(kFrameRing) ? newest - uint32_t(kFrameRing) + 1 : 1u);
            for (uint32_t t = from; t <= newest; ++t) {
                const int a = ringSlotLocked(t - 1), b = ringSlotLocked(t);
                if (a < 0 || b < 0 || frameBuf_[b].pubCounter <= frameBuf_[a].pubCounter) continue;
                c.gaps[c.gapNext] = float(double(frameBuf_[b].pubCounter - frameBuf_[a].pubCounter) / freq * base);
                c.gapNext = (c.gapNext + 1) % c.gaps.size();
                c.gapCount = std::min(c.gapCount + 1, c.gaps.size());
            }
        }
        c.seen = newest;
        if (c.gapCount) {
            std::array<float, 128> sorted = c.gaps;
            const size_t n = c.gapCount, k = std::min(n - 1, n * 98 / 100);
            std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(k), sorted.begin() + std::ptrdiff_t(n));
            c.lagTarget = std::clamp(double(sorted[k]) + 0.25, 1.0, 4.0);
        }
        // Start gate: hold the first tick until the first ticks are published, so a slow spawn
        // tick is part of the game-start transition instead of the first thing that moves.
        const uint32_t gateTicks = std::max(3u, uint32_t(std::lround(double(kPresentGate) * nominalRate / 240.0)));
        if (!c.started) { c.started = true; c.gateTick = newest; c.td = newest; }
        if (!c.gateOpen) {
            if (newest < c.gateTick + gateTicks) {
                c.gate = true;
                c.td = c.gateTick;
                const int slot = ringSlotLocked(c.gateTick);
                shown_ = slot >= 0 ? slot : published_;
                pinFrameLocked(shown_);
                interpAlpha_ = 1.0f;
                return;
            }
            c.gateOpen = true;
            c.snap = true;
            c.td = std::max(double(c.gateTick), double(newest) - c.lagTarget);
            c.lagAfter = double(newest) - c.td;
        } else {
            // Advance at that rate, servoed so the lag behind the newest snapshot settles at
            // lagTarget over ~0.25 s.
            const double factor = std::clamp(1.0 + (c.lagAfter - c.lagTarget) / (base * 0.25), 0.85, 1.25);
            const double before = c.td;
            c.nominal = paused_ ? 0.0 : dt * nominalRate;
            c.td += dt * base * factor;
            if (c.td >= double(newest)) { c.td = double(newest); c.freeze = true; }
            // Far behind (a catch-up, a long stall): jump. A smaller overrun of the ring is caught
            // below, when the wanted tick is no longer held.
            if (double(newest) - c.td > double(2 * kFrameRing)) { c.td = double(newest) - c.lagTarget; c.snap = true; }
            c.dtd = c.td - before;
            c.lagAfter += (double(newest) - c.td - c.lagAfter) * std::min(1.0, dt / 0.1);
        }
        uint32_t tick = std::max<uint32_t>(1, uint32_t(std::ceil(c.td - 1e-9)));
        int slot = ringSlotLocked(tick);
        if (slot < 0) {
            // The tick was overwritten (the writer outran the ring): show the oldest tick still held
            // that is not older than the one wanted, or the newest.
            const double before = c.td;
            uint32_t found = newest;
            for (int i = 0; i < kFrameRing; ++i)
                if (i != writing_ && frameBuf_[i].gen && frameBuf_[i].gameTick >= tick && frameBuf_[i].gameTick < found)
                    found = frameBuf_[i].gameTick;
            tick = found;
            slot = ringSlotLocked(tick);
            if (slot < 0) { slot = published_; tick = newest; }
            c.td = std::max(c.td, double(tick) - 1.0);
            c.dtd += c.td - before;
            c.snap = true;
        }
        shown_ = slot;
        pinFrameLocked(slot);
        interpAlpha_ = float(std::clamp(c.td - (double(tick) - 1.0), 0.0, 1.0));
    }

    void GameView::finishTerrain() {
        mapView_.finishChunks();
        for (int i = 0; i < 1000 && !miniTex_; ++i) {   // adopt once the crunch lands
            buildMinimap();
            if (!miniTex_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void GameView::issuePerUnit(ta::net::Cmd kind, int value) {
        ClickScope click(*this);   // one click: a Legion net game sends it as explicit orders
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

    // The status line (unitstatus.h).
    const char* GameView::unitStatusText(const UnitR* u) const {
        return u ? ta::client::unitStatusLabel(*u) : "";
    }

    bool GameView::handleKey(SDL_Keycode key, uint16_t mod) {
        ClickScope click(*this);   // one click: a Legion net game sends it as explicit orders
        bool ctrl = (mod & KMOD_CTRL) != 0;
        bool shift = (mod & KMOD_SHIFT) != 0;
        bool alt = (mod & KMOD_ALT) != 0;

        if (replayMode_ && replayKey(key, mod)) return true;
        const bool speedKey = key == SDLK_EQUALS || key == SDLK_PLUS || key == SDLK_KP_PLUS ||
                              key == SDLK_MINUS || key == SDLK_KP_MINUS;
        if (spectating_ && (key == SDLK_PAUSE || speedKey) &&
            !ta::client::watchMayPace(singlePlayer_, mp_ && mp_->room().hostId == mp_->myClientId()))
            return true;
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
        if (speedKey) {
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
                // Retail steps one level (0.1x) at a time, 0.1x .. 2x. Debug builds
                // with TA_FAST_SPEED=1 carry on past 2x in 0.5x steps up to 8x
                // (a debug test/loopback server accepts it; others clamp to 2x).
                int maxSpeed = ta::net::kMaxGameSpeed;
                if (ta::devFlag("TA_FAST_SPEED")) maxSpeed = ta::net::kMaxDevGameSpeed;
                const int cur = std::min(int(mp_->gameSpeed()), maxSpeed);
                const int stepBy = (up ? cur >= ta::net::kMaxGameSpeed : cur > ta::net::kMaxGameSpeed) ? 5 : 1;
                int ns = std::clamp(cur + (up ? stepBy : -stepBy), ta::net::kMinGameSpeed, maxSpeed);
                if (ns != o.speed) { o.speed = uint8_t(ns); mp_->setGameOptions(o); }
                notice_ = speedNotice(ns - 10); noticeTimer_ = 2;
                return true;
            }
            gameSpeed_ = std::clamp(gameSpeed_ + (up ? 1 : -1), -9, 10);
            notice_ = speedNotice(gameSpeed_);
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
        if (digit >= 0) {
            int num = digit == 0 ? 10 : digit;
            if (ctrl || alt) assignSquad(num, alt ? -1 : +1, shift);
            else             recallSquad(num);
            return true;
        }

        // Everything below is a REBINDABLE action: resolve the pressed chord to an
        // Act via the user's hotkey config (src/client/hotkeys) and dispatch.
        // A watcher's order key does nothing at all, not even a notice.
        if (watchOnly() && !ta::client::watchAllows(hotkeys_.match(int32_t(key), mod))) return true;
        switch (hotkeys_.match(int32_t(key), mod)) {
            case ta::Act::PushToTalk: return true;   // held for voice chat (voiceKey), no action here
            case ta::Act::GiveUnits:
                if (!watchOnly() && missionStem_.empty()) {
                    giveUnitsMenu_ = true;
                    giveUnitsHots_.clear();
                    voiceVolumeDrag_ = -1;
                    dragging_ = false;
                }
                return true;
            case ta::Act::ToggleCounts: showCounts_ = !showCounts_; return true;
            case ta::Act::MakePoster: if (powerCodesOn()) startPoster(); return true;   // +MakePoster
            case ta::Act::UnitInfo: toggleUnitInfo(); return true;
            case ta::Act::RetailZoom: retailZoom(winW_, winH_, true); return true;
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
                // Cmd::Destroy TOGGLES one unit's countdown; if any selected
                // unit is already counting down, this press cancels those and
                // arms none (selfDestructPress).
                std::vector<std::pair<int, bool>> owned;
                for (int id : selection_)
                    if (auto* su = frameUnitP(id))
                        if (su->alive() && su->player == localPlayer_)
                            owned.emplace_back(id, su->selfDestructT >= 0.0f);
                const auto send = ta::client::selfDestructPress(owned);
                bool anyArmed = false;
                for (const auto& o : owned) anyArmed |= o.second;
                for (int id : send) {
                    ta::net::Command c;
                    c.kind = ta::net::Cmd::Destroy;
                    c.unitId = id;
                    issue(c);
                }
                if (!send.empty()) {
                    selfDestructWatch_.clear();
                    if (anyArmed) notice_ = "SELF-DESTRUCT CANCELLED";
                    else {
                        // Until the order reaches the sim, the countdown each
                        // unit's type will start (selfdestructcountdown).
                        int seconds = 0;
                        for (int id : send)
                            if (const auto* su = frameUnitP(id); su && su->type)
                                seconds = std::max(seconds, su->type->selfDestructCountdown);
                        notice_ = ta::client::selfDestructNotice(float(seconds));
                        selfDestructWatch_ = send;
                        selfDestructArmWait_ = kSelfDestructArmWait;
                        selfDestructSeen_ = false;
                    }
                    noticeTimer_ = 2;
                }
                return true;
            }
            case ta::Act::Disco:
            case ta::Act::Headbang: {
                // One emote at a time -- don't even send the command while a disco or
                // headbang is already running on the real-time clock. The busy
                // notice reflects what you're ACTUALLY doing, not the key you pressed.
                bool emoting = frameDiscoActive(localPlayer_) || frameHeadbangActive(localPlayer_);
                if (emoting) {
                    notice_ = frameDiscoActive(localPlayer_) ? "ALREADY GROOVING" : "ALREADY ROCKING";
                    noticeTimer_ = 2; return true;
                }
                bool disco = hotkeys_.match(int32_t(key), mod) == ta::Act::Disco;
                ta::net::Command c;
                c.kind = disco ? ta::net::Cmd::Disco : ta::net::Cmd::Headbang;
                if (selection_.empty()) issue(c); // living monarch fallback
                else for (int id : selection_) {
                    const auto* u = frameUnitP(id);
                    if (!u || !u->alive() || u->player != localPlayer_ || !u->type || u->type->isStructure()) continue;
                    c.unitId = id;
                    issue(c); // capture selection through lockstep
                    c.queue = 1; // further targets belong to the same emote trigger
                }
                notice_ = disco ? "DISCO TIME" : "HEADBANG!!";
                noticeTimer_ = 2;
                return true;
            }
            // Selection commands (no armed order, no selection prerequisite).
            case ta::Act::SelectAll:
                selectOwned([](const UnitR&){ return true; });
                return true;
            case ta::Act::SelectSameType: {   // expand every type in the original selection
                std::set<const ta::sim::UnitType*> types;
                for (int id : selection_)
                    if (const auto* u = frameUnitP(id); u && u->type)
                        types.insert(u->type);
                // Snapshot the types first: selectOwned replaces selection_.
                if (!types.empty())
                    selectOwned([&types](const UnitR& u){ return types.contains(u.type); });
                return true;
            }
            case ta::Act::SelectOnScreen:
                selectOwned([this](const UnitR& u){ return onScreen(u); });
                return true;
            case ta::Act::SelectMonarch: {   // select the Monarch and track it
                int monarch = playerMonarchId_;
                if (watchOnly()) {   // the monarch of the player on show
                    const int owner = hudPlayer();
                    if (owner < 0) return true;
                    monarch = -1;
                    for (const UnitR* u : front().live)
                        if (u->alive() && u->type && int(u->player) == owner && u->type->commander) {
                            monarch = u->id;
                            break;
                        }
                }
                const auto* m = monarch >= 0 ? frameUnitP(monarch) : nullptr;
                if (m && m->alive()) {
                    selection_ = {monarch};
                    trackSel_ = true;
                    centerOnSelection();
                    voice(monarch, "select");
                } else {
                    notice_ = "NO COMMANDER"; noticeTimer_ = 2;
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

    void GameView::assignWatchSquad(int num, bool append) {
        auto& squad = watchSquads_[size_t(num - 1)];
        const auto* first = squad.empty() ? nullptr : frameUnitP(squad.front());
        if (!append || (first && int(first->player) != hudPlayer())) squad.clear();
        for (int id : selection_) {
            for (auto& other : watchSquads_) std::erase(other, id);
            squad.push_back(id);
        }
    }

    void GameView::assignSquad(int num, int sign, bool append) {
        if (num < 1 || num > 10 || selection_.empty()) return;
        if (watchOnly()) { assignWatchSquad(num, append); return; }
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
        const bool follow = lastRecalledSquad_ == num;
        selection_.clear();
        if (watchOnly()) {
            for (int id : watchSquads_[size_t(num - 1)])
                if (const auto* u = frameUnitP(id); u && u->alive() && canPickUnit(*u))
                    selection_.push_back(id);
            keepOneWatchOwner();
        } else for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (u.alive() && u.player == localPlayer_ && u.type &&
                std::abs(int(u.squad)) == num && !u.type->isBuilder)
                selection_.push_back(u.id);
        }
        lastRecalledSquad_ = selection_.empty() ? 0 : num;
        trackSel_ = follow && !selection_.empty();
        if (trackSel_) centerOnSelection();
        if (!selection_.empty()) voice(selection_.front(), "select");
    }

    void GameView::clearSquad() {
        if (watchOnly()) {
            for (int id : selection_) for (auto& squad : watchSquads_) std::erase(squad, id);
            return;
        }
        for (int id : selection_)
            if (const auto* u = frameUnitP(id);
                u && u->alive() && u->player == localPlayer_ && u->squad)
                issueSquad(id, 0);
    }

    bool GameView::onScreen(const UnitR& u) const {
        float sx = (u.x - mapView_.offX()) * mapView_.zoom();
        float sy = (u.z - mapView_.offY()) * mapView_.zoom();
        return sx >= 0 && sy >= 0 && sx <= float(winW_) && sy <= float(mapViewH(winH_));
    }

    bool GameView::lbHot(const SDL_FRect& r) const {
        if (lobbyModal_) return false;   // the room under the CHANGE MAP panel or the LAN leave prompt, the game list under a window
        float mx = mouseX_ / lobbyScale_ - lobbyOffX_, my = mouseY_ / lobbyScale_ - lobbyOffY_;
        return mx >= r.x && mx <= r.x + r.w && my >= r.y && my <= r.y + r.h;
    }

    void GameView::lbField(float x, float y, float w, const std::string& label,
                 const std::string& value, int id, bool enabled, FieldText text) {
        // MODERN's descenders: the label rises by part of them to stay off the box, and
        // the value is a size smaller so ascenders and descenders both sit inside it.
        blockText(label, x, y - ta::uiTextDescent(1.6f) * 0.6f, 1.6f, {150, 155, 170, 255});
        SDL_FRect r{x, y + 14, w, 22};
        // (A shown password is MODERN's in either UI FONT, at MODERN's size and place.)
        std::optional<ExactText> exact;
        if (text == FieldText::Shown) exact.emplace();
        const float vpx = ta::modernUiText() ? 1.6f : 1.8f;
        bool active = enabled && (lbField_ == id);
        SDL_SetRenderDrawColor(ren_, 24, 26, 34, 255);
        SDL_RenderFillRectF(ren_, &r);
        if (enabled) SDL_SetRenderDrawColor(ren_, active ? 200 : 90, active ? 210 : 100, active ? 130 : 130, 255);
        else SDL_SetRenderDrawColor(ren_, 56, 60, 74, 255);
        SDL_RenderDrawRectF(ren_, &r);
        const SDL_Color ink = enabled ? SDL_Color{225, 230, 240, 255} : SDL_Color{110, 115, 125, 255};
        const std::string caret = active ? "_" : "";
        const float ty = ta::modernUiText() ? r.y + (r.h - 7 * vpx - ta::uiTextDescent(vpx)) / 2 : y + 20;
        if (text == FieldText::Hidden) {
            // A password: one pip per character, as many as the box holds, then the caret.
            // (The pips sit on the box's middle: they have no descenders to leave room for.)
            const size_t fits = size_t(std::max(0.0f, (w - 12 - blockWidth(caret, vpx)) / (6 * vpx)));
            const float pips = lbPips(x + 6, r.y + (r.h - 7 * vpx) / 2, std::min(value.size(), fits), vpx, ink);
            blockText(caret, x + 6 + pips, ty, vpx, ink);
        } else {
            // The END of a long value shows (where typing happens), and the text sits in
            // the box with its descenders (MODERN) clear of the border.
            blockText(ta::uiTextFitStart(value + caret, vpx, w - 12), x + 6, ty, vpx, ink);
        }
        if (enabled) lobbyHots_.push_back({r, [this, id] { lbFocus(id); }});
    }

    float GameView::lbPips(float x, float y, size_t count, float px, SDL_Color colour) {
        SDL_SetRenderDrawColor(ren_, colour.r, colour.g, colour.b, colour.a);
        for (size_t i = 0; i < count; ++i) {
            const SDL_FRect pip{x + float(i) * 6 * px + px * 0.5f, y + 2 * px, 3 * px, 3 * px};
            SDL_RenderFillRectF(ren_, &pip);
        }
        return float(count) * 6 * px;
    }

    std::string GameView::readOta(const std::string& tntPath) const {
        const auto ota = ta::vpath::replaceExtension(tntPath,".ota");
        try { auto d = vfs_.read(ota); return std::string(d.begin(), d.end()); }
        catch (...) { return {}; }
    }

    std::string* GameView::lbFieldBuf() {
        switch (lbField_) {
            case 4: return &chatDraft_;
            case 5: return &genParams_.name;
            case 6: return &mapFilter_;
            case 7: return &cosmeticModFilter_;
            case 8: return &gameModFilter_;
            case 11: return &browserWin_.name;
            case 12: return &browserWin_.password;
            default: return nullptr;
        }
    }

    std::vector<std::pair<float, float>> GameView::buildLinePositions(
        float x0, float z0, float x1, float z1) const {
        if (!placing_) return {};
        // Snapped to the cells canPlace tests, so gap 0 really is edge to edge: the
        // old un-snapped spacing needed a spare cell to stop rounding pushing two
        // neighbours into one shared cell.
        return ta::client::buildLineSites(placing_->footX, placing_->footZ, buildLineGap_, x0, z0, x1, z1);
    }

    void GameView::voice(int unitId, const std::string& event) {
        if (watchOnly()) return;   // no acknowledgement of any kind
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
        explosionClasses_.clear();explosionClassOrder_.clear();
        try {
            auto root = vtdf("gamedata/explosions/explosions.tdf");
            for (const auto& cls : root.childOrder) {
                explosionClassOrder_.push_back(cls);
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

    void GameView::spawnWeaponImpact(const ta::sim::Weapon& weapon,
                                     float x, float z, float alt,
                                     std::optional<std::array<int32_t,3>> position, bool struckUnit) {
        // Native 529c10: only an environmental water hit selects the water
        // class. A direct hit uses the ordinary class even above water; a -1
        // (missing) selected class requests no visual, not a generic substitute.
        const std::string& cls = (worldTables().isWater(x, z) && !struckUnit)
                                     ? weapon.waterExplosionClass
                                     : weapon.explosionClass;
        if (spawnEffect(cls, x, z, alt) && position) effects_.back().worldPosition = position;
    }

    void GameView::triggerShake(float mag, float dur) {
        if (!screenShake_ || mag <= 0 || dur <= 0) return;
        // Let a stronger/longer quake override a fading one.
        if (mag * dur >= shakeMag_ * shakeTime_) {
            shakeMag_ = mag; shakeDur_ = dur; shakeTime_ = dur;
        }
    }

    void GameView::buildPieceMeta(const ta::tdo::Object& o, PieceMeta& m, bool isRoot) {
        // One record per texture animation (first use decides the order, which is the
        // order of the frame words in the local geometry key); a frame feeds the shadow
        // if any primitive using it has shadow masks.
        const auto addAnimationFrame=[](PieceMeta& meta,const size_t* frame,bool masked) {
            const auto at=std::find(meta.animationFrames.begin(),meta.animationFrames.end(),frame);
            if(at==meta.animationFrames.end()) {
                meta.animationFrames.push_back(frame);meta.animationFrameMasks.push_back(masked);
            } else meta.animationFrameMasks[size_t(at-meta.animationFrames.begin())]|=uint8_t(masked);
        };
        pieceMetaFor(o, isRoot, m);
        m.primAtlas.reserve(m.primTex.size());
        for (const auto& name : m.primTex) {
            auto it = atlasRect_.find(name);
            m.primAtlas.push_back(it == atlasRect_.end() ? nullptr : &it->second);
            auto mask=shadowMasks_.find(name);
            m.primShadowMasks.push_back(mask==shadowMasks_.end() ? nullptr : &mask->second);
            m.primAnimated.push_back(animatedTex_.count(name)!=0);
            m.animated |= m.primAnimated.back();
            if(m.primAnimated.back())
                addAnimationFrame(m,&modelTextureAnimations_.at(name).frame,m.primShadowMasks.back()!=nullptr);
        }
        m.children.resize(o.children.size());
        for (size_t i = 0; i < o.children.size(); ++i) {
            buildPieceMeta(o.children[i], m.children[i], false);
            m.animated |= m.children[i].animated;
            for(size_t f=0;f<m.children[i].animationFrames.size();++f)
                addAnimationFrame(m,m.children[i].animationFrames[f],m.children[i].animationFrameMasks[f]!=0);
        }
    }

    void GameView::buildStillMeta(const PieceMeta& meta, PieceMeta& still) {
        still=meta;
        still.animated=false;still.animationFrames.clear();still.animationFrameMasks.clear();
        for (size_t i=0;i<still.primTex.size();++i) {
            if (i>=still.primAnimated.size() || !still.primAnimated[i]) continue;
            const std::string name=still.primTex[i]+kStillTextureSuffix;
            const auto rect=atlasRect_.find(name);
            if (rect==atlasRect_.end()) continue;   // keep the animated face
            still.primTex[i]=name;
            if (i<still.primAtlas.size()) still.primAtlas[i]=&rect->second;
            if (i<still.primShadowMasks.size()) still.primShadowMasks[i]=nullptr;
            still.primAnimated[i]=false;
        }
        const auto addFrame=[&](const size_t* frame,uint8_t masked) {
            still.animated=true;
            const auto at=std::find(still.animationFrames.begin(),still.animationFrames.end(),frame);
            if (at==still.animationFrames.end()) {
                still.animationFrames.push_back(frame);still.animationFrameMasks.push_back(masked);
            } else still.animationFrameMasks[size_t(at-still.animationFrames.begin())]|=masked;
        };
        // Faces left animated (no still entry) keep their own clock words.
        for (size_t i=0;i<still.primTex.size();++i)
            if (i<still.primAnimated.size() && still.primAnimated[i])
                addFrame(&modelTextureAnimations_.at(still.primTex[i]).frame,
                         uint8_t(i<still.primShadowMasks.size() && still.primShadowMasks[i]));
        for (size_t i=0;i<meta.children.size();++i) {
            buildStillMeta(meta.children[i],still.children[i]);
            for (size_t f=0;f<still.children[i].animationFrames.size();++f)
                addFrame(still.children[i].animationFrames[f],still.children[i].animationFrameMasks[f]);
        }
    }

    bool GameView::pieceWorldFx(const UnitR& u, const Anim& a, const std::string& pieceName,
                      float& outX, float& outZ, float& outAlt) {
        auto vt = visuals_.find(u.type ? u.type->id : std::string());
        if (vt == visuals_.end() || !u.type) return false;
        float m[3];
        if (!pieceModelOrigin(vt->second.model.root, &a, modelBodyTransform(u.bodyPitch,u.bodyRoll), pieceName, m)) return false;
        // Match the body rotation, including a building's birth heading.
        float facing = -u.heading;
        float cy = std::cos(facing), sy = std::sin(facing);
        float rx = m[0] * cy + m[2] * sy;
        float rz = -m[0] * sy + m[2] * cy;
        // Match the model's projected altitude and local depth at its screen anchor.
        float ry = (m[1] + unitAltById(u.id)) * kProjY + rz * kProjZ;
        outX = u.x + rx;
        outZ = u.z;
        outAlt = ry;
        return true;
    }

    bool GameView::spawnEffect(const std::string& cls, float x, float z, float alt) {
        if (cls.empty()) return false;
        loadExplosionClasses();
        auto it = explosionClasses_.find(cls);
        if (it == explosionClasses_.end() || it->second.empty()) return false;
        // Native 492fd0 scales one CRT draw into the class variant list.
        const size_t variant = uint64_t(ta::sim::retailCrtRandom(smokeRandom_)) *
                               it->second.size() / 32768u;
        const std::string& anim = it->second[variant];
        const EffectAnim* ea = effectFor(anim);
        if (!ea) return false;
        // Native explosion classes advance their authored clock once per sim tick.
        effects_.push_back({ea, x, z, 0.0f, 0.0f, 0.0f, 1, alt,
                            true, front().gameTick});
        static const bool kLog = ta::devEnv("TA_FXLOG") != nullptr;
        if (kLog) std::fprintf(stderr, "t=%.2f effect '%s' anim '%s' (%zu frames)\n",
                               animClock_, cls.c_str(), anim.c_str(), ea->frames.size());
        return true;
    }

    // TotalA.exe's nanolathe spray. Each game tick a builder's work step succeeds
    // (build 0x41ba60 -> 0x402a1c, repair 0x41bd10 -> 0x40252e, ...), retail asks
    // QueryNanoPiece for the emitting piece (0x43e400: unit position + that
    // piece's world offset) and calls 0x4720d0(point, targetBox, list 6). The
    // target box is the target's model bounds; the particle object (vtable
    // 0x4fd5b8, init 0x473b50) keeps only its middle: min + 4/11..7/11 of each
    // axis. Its emit (0x473d50) makes 5 particles (i = 0..4): from a random point
    // of the source box to a random point of the target box, travelling
    // distance/4 ticks (4 px per tick: 0x473ebd..0x473ed5 takes the high word of
    // (d<<16)/0x40000) in a straight line, coloured palette index 0xa1 + i%7
    // (161..165, the nano greens). Feature reclaim (0x404cc4 -> 0x472200) runs
    // the other way, box -> nano piece, two emits per tick.
    void GameView::nanoStep() {
        const uint32_t now = front().gameTick;
        if (now < nanoTick_ || nanoTick_ == 0) nanoTick_ = now;
        const uint32_t ticks = std::min<uint32_t>(now - nanoTick_, 4);
        nanoTick_ = now;
        if (!ticks) return;
        using Box = NanoBox;
        const auto spray = [&](const UnitR& source, Anim& a, const Box& box, bool reclaim) {
            nanoSpray(source, a, box, reclaim, ticks);
        };
        for (auto& [id, a] : anims_) {
            const UnitR* u = frameUnitP(id);
            if (!u || !u->type || !a.vm || !a.pieceNames || u->underConstruction || u->deadFor >= 0) continue;
            int targetId = u->productionSiteId ? u->productionSiteId
                         : u->buildSiteId ? u->buildSiteId : u->repairId;
            const bool reclaim = !targetId && u->reclaimId;
            if (!targetId && !reclaim) { a.nanoTarget = 0; a.nanoLevel = -1; continue; }
            Box box{};
            if (!reclaim) {
                const UnitR* t = frameUnitP(targetId);
                if (!t || !t->type) continue;
                // Spray only on a tick the work advanced (retail emits after a
                // successful build/repair step, not while stalled).
                // A TA site gains hp as it is built (buildProgress is the
                // Kingdoms clock and may stay 0), a repair only hp.
                const float level = t->buildProgress * 1e6f + t->hp;
                const bool advanced = a.nanoTarget == targetId && level > a.nanoLevel;
                a.nanoTarget = targetId; a.nanoLevel = level;
                if (!advanced) continue;
                box = {t->x, t->z, unitAltById(t->id) * kProjY + terrainLift(t->x, t->z),
                       float(t->type->footX) * 16.0f, float(t->type->footZ) * 16.0f,
                       std::max(8.0f, float(t->type->modelTop) / 65536.0f)};
            } else {
                if (!a.building) continue;
                const FeatureInst* f = nullptr;
                for (const auto& fi : features_) if (fi.simId == u->reclaimId) { f = &fi; break; }
                if (!f) continue;
                box = {f->x, f->z, terrainLift(f->x, f->z), 16.0f, 16.0f, 16.0f};
            }
            if (!unitShownR(*u) || (!noFog_ && !cellVisibleR(u->x, u->z) && !cellVisibleR(box.x, box.z))) continue;
            spray(*u, a, box, reclaim);
        }
        // A repair pad's spray: SelfRepair (0x402430) runs on the flyer, and each
        // tick its repair step from the pad succeeds (0x41bd10(pad, flyer) at
        // 0x402518) it sprays exactly as a builder's repair does: the pad's
        // QueryNanoPiece point (0x43e400(pad) at 0x40252e) to the flyer's model
        // box (its position + type bounds, 0x402533..0x4025a5), 0x4720d0(.., 6).
        for (const UnitR& f : front().units) {
            if (f.padPiece < 0 || !f.inTransport || !f.type || f.deadFor >= 0) {
                if (f.padPiece < 0) padRepairHp_.erase(f.id);
                continue;
            }
            const auto last = padRepairHp_.find(f.id);
            const bool advanced = last != padRepairHp_.end() && f.hp > last->second;
            padRepairHp_[f.id] = f.hp;
            if (!advanced) continue;
            const UnitR* pad = frameUnitP(f.inTransport);
            if (!pad || !pad->type || pad->deadFor >= 0) continue;
            const auto at = anims_.find(pad->id);
            if (at == anims_.end() || !at->second.vm || !at->second.pieceNames) continue;
            const Box box{f.x, f.z, unitAltById(f.id) * kProjY + terrainLift(f.x, f.z),
                          float(f.type->footX) * 16.0f, float(f.type->footZ) * 16.0f,
                          std::max(8.0f, float(f.type->modelTop) / 65536.0f)};
            if (!unitShownR(*pad) || (!noFog_ && !cellVisibleR(pad->x, pad->z))) continue;
            spray(*pad, at->second, box, false);
        }
        for (auto it = padRepairHp_.begin(); it != padRepairHp_.end();)
            it = frameUnitP(it->first) ? std::next(it) : padRepairHp_.erase(it);
    }

    void GameView::nanoSpray(const UnitR& source, Anim& a, const NanoBox& box, bool reclaim, uint32_t ticks) {
        [[maybe_unused]] const uint32_t now = front().gameTick;
        const auto* pal = featurePalette("");
        auto colour = [&](int i) {
            SDL_Color c{75, 171, 43, 255};
            if (pal) { const auto* e = pal->rgba[161 + i % 7]; c = {e[0], e[1], e[2], 255}; }
            return c;
        };
        auto rnd = [&] { salt_ = salt_ * 1103515245u + 12345u; return float((salt_ >> 16) & 0x7fff) / 32768.0f; };
        const UnitR* u = &source;
        [[maybe_unused]] const int id = source.id;
        {
            const int32_t piece = (a.vm->call("QueryNanoPiece", {0}), a.vm->lastLocals().empty()
                                   ? -1 : a.vm->lastLocals()[0]);
            float nx, nz, nalt;
            scriptEffectOrigin(*u, a, piece, nx, nz, nalt);
            nalt += terrainLift(u->x, u->z);   // absolute projected lift
            const int emits = reclaim ? 2 : 1;
#ifndef NDEBUG
            if (ta::devEnv("TA_NANOLOG"))
                std::fprintf(stderr, "NANO tick=%u unit=%d piece=%d src=%.1f,%.1f,%.1f box=%.1f,%.1f,%.1f ticks=%u live=%zu\n",
                             now, id, piece, nx, nz, nalt, box.x, box.z, box.alt, ticks, particles_.size());
#endif
            for (uint32_t tick = 0; tick < ticks; ++tick)
            for (int e = 0; e < emits; ++e)
            for (int i = 0; i < 5; ++i) {
                // A point in the middle 3/11 of the box on every axis.
                const float bx = box.x + box.w * (4.0f / 11.0f - 0.5f + 3.0f / 11.0f * rnd());
                const float bz = box.z + box.d * (4.0f / 11.0f - 0.5f + 3.0f / 11.0f * rnd());
                const float balt = box.alt + box.h * (4.0f + 3.0f * rnd()) / 11.0f * kProjY;
                float x0 = nx, z0 = nz, l0 = nalt, x1 = bx, z1 = bz, l1 = balt;
                if (reclaim) { std::swap(x0, x1); std::swap(z0, z1); std::swap(l0, l1); }
                const float dx = x1 - x0, dz = z1 - z0, dy = (l1 - l0) / kProjY;
                const int steps = int(std::sqrt(dx * dx + dz * dz + dy * dy) / 4.0f);
                if (steps <= 0) continue;
                Particle p;
                p.x = x0; p.z = z0;
                p.alt = l0 - terrainLift(x0, z0);   // the draw adds the lift under the dot
                const float perSec = 30.0f / float(steps);
                p.vx = dx * perSec; p.vz = dz * perSec;
                // Keep the drawn height on the straight line despite the terrain
                // lift changing under the moving dot.
                p.valt = ((l1 - terrainLift(x1, z1)) - p.alt) * perSec;
                p.maxLife = p.life = float(steps) / 30.0f;
                p.size = 1.0f;
                const SDL_Color c = colour(i);
                p.r = c.r; p.g = c.g; p.b = c.b; p.kind = 2;
                particles_.push_back(p);
            }
        }
    }

    void GameView::spawnBurst(float x, float z, int n, Uint8 r, Uint8 g, Uint8 b,
                    float spread, float sizeMax, int kind, float baseAlt) {
        for (int i = 0; i < n; ++i) {
            Particle p;
            p.x = x; p.z = z;
            float ang = float(salt_++ % 628) / 100.0f;
            float sp = spread * (0.3f + float(salt_++ % 100) / 100.0f);
            p.vx = std::sin(ang) * sp;
            p.vz = std::cos(ang) * sp;
            p.alt = 4 + baseAlt;
            p.valt = kind == 1 ? 18.0f : (30.0f + float(salt_++ % 40));
            p.maxLife = p.life = 0.35f + float(salt_++ % 50) / 100.0f;
            p.size = 1.5f + float(salt_++ % 100) / 100.0f * sizeMax;
            p.r = r; p.g = g; p.b = b; p.kind = kind;
            particles_.push_back(p);
        }
    }

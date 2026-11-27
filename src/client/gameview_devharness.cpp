// GameView local dev/test harness: the standalone (server-less) scenario/demo/FFA
// match setup and the TA_* / --testbuild / --firetest style fixtures that spawn
// units, issue orders and fast-forward the LOCAL sim. DEBUG builds only -- the
// whole file compiles to nothing under NDEBUG, and the callers (the ctor, and the
// localHarness block in main.cpp) are #ifndef NDEBUG too, so a release taclient
// carries none of it. Shared helpers the harness leans on (startAtCommander, lookAt,
// collect, spawn) stay in gameview_impl.cpp / gameview.h.
#include "sim/sides.h"
#include "util/virtualpath.h"
#include "client/gameview.h"
#include "client/tabeamdraw.h"
#include "client/gamespeed.h"
#include "client/renderdriver.h"
#include "client/retailaim.h"
#include "client/shadowmask.h"
#include "client/retailfeatureclock.h"
#include "client/retailbuilderanimation.h"
#include "client/retailmovementcallbacks.h"
#include "client/retaildeathsfx.h"
#include "client/runtimesettings.h"
#include "client/buildline.h"
#include "client/cursortiming.h"
#include "client/vfsreadgate.h"
#include "client/taguiview.h"
#include "sim/retailmotion.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>

#ifndef NDEBUG

    // The local harness's kits, keyed by sidedata.tdf nameprefix (ta::sides::lowerPrefix):
    // the side's commander, its first factory, a metal extractor, a construction kbot
    // and a mixed kbot squad.
    const GameView::FactionKit& GameView::kit(const std::string& side) {
        static const std::map<std::string, FactionKit> kits = {
            {"arm", {"armcom", "armlab", "armmex", "armck",
                     {"armpw", "armpw", "armrock", "armham"}}},
            {"cor", {"corcom", "corlab", "cormex", "corck",
                     {"corak", "corak", "corstorm", "corthud"}}},
        };
        std::string key = side;
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (key == "core") key = "cor";
        auto it = kits.find(key);
        return it != kits.end() ? it->second : kits.at("arm");
    }

    void GameView::debugFogDisc(float x, float z, float radius) {
        const float px=x,pz=groundProjectedZ(x,z);
        debugFogOverride_=[px,pz,radius](std::vector<uint8_t>& plane,int w,int h) {
            for (int cz=0;cz<h;++cz) for (int cx=0;cx<w;++cx) {
                const float dx=float(cx)*32+16-px,dz=float(cz)*32+16-pz;
                plane[size_t(cz)*size_t(w)+size_t(cx)]=dx*dx+dz*dz<radius*radius ?
                    ta::retailfog::kInView : ta::retailfog::kExplored;
            }
        };
    }

    void GameView::debugWatchPoke() {
        static const char* const poke = ta::devEnv("TA_WATCHONLY_POKE");
        static bool done = false;
        if (!poke || done || !spectating_ || netTick_ < uint32_t(std::atoi(poke)) || front().live.empty()) return;
        done = true;
        for (const UnitR* u : front().live) if (u->alive() && u->type) { selection_ = {u->id}; break; }
        constexpr int W = 1024, H = 768;
        for (const auto& def : ta::hotkeyDefs()) {
            const auto chord = hotkeys_.chord(def.act);
            if (!chord.bound() || def.act == ta::Act::UnitInfo) continue;
            uint16_t mod = 0;
            if (chord.mod & ta::HK_CTRL) mod |= KMOD_CTRL;
            if (chord.mod & ta::HK_SHIFT) mod |= KMOD_SHIFT;
            if (chord.mod & ta::HK_ALT) mod |= KMOD_ALT;
            handleKey(chord.key, mod);
            selection_.resize(std::min<size_t>(selection_.size(), 1));
        }
        handleKey(SDLK_1, KMOD_CTRL);
        handleKey(SDLK_1, 0);
        SDL_Event e{};
        e.type = SDL_MOUSEBUTTONDOWN;e.button.button = SDL_BUTTON_RIGHT;e.button.x = W / 2;e.button.y = H / 2;
        input(e, W, H);
        e.type = SDL_MOUSEBUTTONUP;
        input(e, W, H);
        std::fprintf(stderr, "watch poke at tick %u: %zu commands queued\n", netTick_, outbox_.size());
    }

    // TA_PREVIEW_LIFETIME_TEST=gen|big|teardown: the lobby's map previews against a change
    // of the data set they read. A read gate on vfs_ holds every preview worker at a read --
    // three generated-map rolls (the first two superseded) for gen, the full-window view's
    // terrain workers for big, all of them for teardown -- and from two seconds on lets
    // them go one at a time, the last held first and the first held last of all, so a wait
    // for anything less than every reader (the newest roll alone, say) leaves one held. The
    // data set must outlive them: startMpGame remounts it (gen, big), the destructor
    // destroys it (teardown). tools/preview_lifetime_test.py runs all three.
    void GameView::previewLifetimeProbe(bool teardown) {
        static const char* const mode = ta::devEnv("TA_PREVIEW_LIFETIME_TEST");
        if (!mode || (std::strcmp(mode, "teardown") == 0) != teardown) return;
        const bool gen = teardown || std::strcmp(mode, "gen") == 0;
        const bool big = teardown || std::strcmp(mode, "big") == 0;
        auto fail = [](const std::string& why) {
            std::fprintf(stderr, "FAIL: preview lifetime (%s): %s\n", mode, why.c_str());
            std::fflush(stderr);
            std::_Exit(1);   // a released reader would read freed memory
        };
        auto waitHeld = [&](ta::ReadGate& gate, size_t n) {
            if (!gate.waitHeld(n, std::chrono::seconds(60)))
                fail("only " + std::to_string(gate.held()) + " of " + std::to_string(n) +
                     " preview readers reached a read");
        };
        // The game's own terrain and minimap workers go quiet first, so every read the gate
        // holds is a preview's.
        resetMinimap();
        mapView_.quiesce();
        auto gate = std::make_shared<ta::ReadGate>(vfs_, mainThreadId_);
        const auto start = std::chrono::steady_clock::now();
        gate->onGone([start, fail](const ta::ReadGate::Gone& at) {
            const auto ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            if (at.held || !at.released)
                fail("the data set went " + std::to_string(ms) + " ms into the hold, with " +
                     std::to_string(at.held) + " preview reads still in it");
            std::fprintf(stderr, "PASS: preview lifetime (%s): the data set went %lld ms after the hold "
                         "began, once its readers had left (%zu reads through it after the release)\n",
                         mode, ms, at.lateReads);
        });
        gate->arm();
        if (big) {
            // A generated map: its view reads vfs_ itself (an installed map's room view
            // reads the verified package's own files). Opened as from the CHANGE MAP panel's
            // RANDOM tab, and drawn once, as the lobby draws it, so the chunk worker has work too.
            ta::mapgen::Params params;
            params.seed = 4;
            const std::string id = ta::mapgen::encodeMapId(params);
            const MapPicker panel = picker_;
            picker_.open = picker_.random = true;
            picker_.gen = {id, id};
            mapPreviewFor_ = id;
            openBigMap();
            picker_ = panel;
            if (!bigMap_) fail("the full-window view did not open");
            bigMap_->draw({0, 0, 640, 480});
            // A Kingdoms map's view decoded shared terrain sections off the data set on its
            // section worker. A TA map carries its own tiles, so the view's workers bake from
            // memory and may read nothing: hold whatever they do read, and the view itself
            // (a Vfs over vfs_) must still be gone or re-pointed before the data set goes.
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (gen) {
            const size_t before = gate->held();
            for (uint64_t seed = 1; seed <= 3; ++seed) {   // each supersedes the one before
                ta::mapgen::Params params;
                params.seed = seed;
                buildMapPreview(ta::mapgen::encodeMapId(params));
                waitHeld(*gate, before + seed);   // held in the order they were asked for
            }
        }
        std::fprintf(stderr, "preview lifetime (%s): %zu preview reads held (%zu rolls listed, full-window view %s)\n",
                     mode, gate->held(), genJobs_.size(), bigMap_ ? "open" : "closed");
        if (gate->held() == 0) { gate->release(); return; }   // nothing read (big, TA tiles): nothing to hold
        std::thread([gate] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            while (gate->held() > 1 && gate->releaseLatest())
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            gate->release();
        }).detach();
    }

    void GameView::guiSmoothTest() {
        const auto hash = world_.stateHash();
        const auto initial = smoothArt_;
        // The HUD face's GLYPH ART is what is checked below: UI FONT = CLASSIC draws it
        // (MODERN draws the TrueType face over the same metrics).
        const auto initialFont = ta::uiFontStyle();
        ta::Settings s; s.hardwareCursor = false; s.smoothArt = false; s.uiFont = 1;
        applySettings(s);
        cursorsInit_ = true; cursors_.load(ren_, vfs_, &s);
        const auto bytes = gpuvram::bytes(), count = gpuvram::count();
        const auto width = hudFont_.width("Smoothing test"), height = hudFont_.height();
        auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        // TA's sidebar is a .gui page (TaGuiView, gameview_tahud.cpp) drawn from intgaf
        // art that SMOOTH ART does not touch; the toggle must still keep layout and
        // state and leak nothing.
        for (int i = 0; i < 3; ++i) for (bool on : {true, false}) {
            s.smoothArt = on; applySettings(s);
            check(hudFont_.width("Smoothing test") == width && hudFont_.height() == height,
                  "live smoothing changed interface layout");
            check(world_.stateHash() == hash, "interface smoothing changed simulation state");
            if (!on) check(gpuvram::bytes() == bytes && gpuvram::count() == count,
                           "HUD smoothing leaks textures");
        }
        // The squad marker's drawSolid(): every fully covered pixel is exactly the
        // requested colour, and (unsmoothed) it covers the same pixels as the tinted
        // draw(). The tinted draw is checked to be MULTI-coloured, so the test would
        // catch the marker going back to the shaded glyph art.
        const SDL_Color green{70, 240, 90, 255};
        const auto render = [&](bool solid, float scale) {
            const int tw = 96, th = 48;
            SDL_Texture* rt = gpuvram::create(ren_, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, tw, th);
            check(rt, "solid-text render target");
            SDL_Texture* prev = SDL_GetRenderTarget(ren_);
            SDL_SetRenderTarget(ren_, rt);
            SDL_SetRenderDrawColor(ren_, 0, 0, 0, 0); SDL_RenderClear(ren_);
            if (solid) hudFont_.drawSolid(ren_, "2F0", 4, 30, scale, green);
            else hudFont_.draw(ren_, "2F0", 4, 30, scale, green);
            std::vector<uint8_t> px(size_t(tw) * th * 4);
            SDL_RenderReadPixels(ren_, nullptr, SDL_PIXELFORMAT_RGBA32, px.data(), tw * 4);
            SDL_SetRenderTarget(ren_, prev);
            gpuvram::destroy(rt);
            return px;
        };
        // The HUD's glyph-art face is Kingdoms' fonts/bodfontbody.gaf; a data set without
        // it (retail TA) has no glyph art to check.
        const bool glyphArt = hudFont_.ok();
        for (bool on : {false, true}) {
            if (!glyphArt) break;
            s.smoothArt = on; applySettings(s);
            for (float scale : {1.0f, 1.7f}) {
                const auto solid = render(true, scale), tinted = render(false, scale);
                size_t opaque = 0, sameCover = 0, covered = 0, offColour = 0;
                std::set<uint32_t> tintedColours;
                for (size_t p = 0; p < solid.size(); p += 4) {
                    if (solid[p + 3] == 255) {
                        ++opaque;
                        if (solid[p] != green.r || solid[p + 1] != green.g || solid[p + 2] != green.b) ++offColour;
                    }
                    if (tinted[p + 3] == 255)
                        tintedColours.insert(uint32_t(tinted[p]) << 16 | uint32_t(tinted[p + 1]) << 8 | tinted[p + 2]);
                    if (solid[p + 3] || tinted[p + 3]) { ++covered; sameCover += (solid[p + 3] > 0) == (tinted[p + 3] > 0); }
                }
                check(opaque > 40, "solid text drew nothing");
                check(offColour == 0, "solid text is not one flat colour");
                check(tintedColours.size() > 4, "tinted glyph art is unexpectedly flat (fixture lost its contrast)");
                if (!on && scale == 1.0f) check(sameCover == covered, "solid text changed the glyph shapes");
            }
        }
        s.smoothArt = initial; applySettings(s);
        ta::setUiFontStyle(initialFont);
        std::puts("PASS: HUD smoothing toggles live, keeps layout/state and releases textures");
        std::puts(glyphArt ? "PASS: solid HUD text is one flat colour in the glyph shapes"
                           : "PASS: no HUD glyph-art face in this data set (fonts/bodfontbody.gaf): solid text not checked");
    }

    void GameView::setupScenario() {
        writableWorld().setTerrain(mapView_.map().heights, mapView_.map().width,
                          mapView_.map().height, mapView_.map().seaLevel,
                          &mapView_.map().features);
        ta::sim::registerMapFeatures(writableWorld(), mapView_.map(), vfs_, &registry_);
        std::string crtPath = mapSibling(".crt");
        ta::crt::Scenario scen = vhas(crtPath) ? ta::crt::parse(vread(crtPath))
                                                : ta::crt::Scenario{};
        std::printf("scenario: %zu placements, %zu regions\n",
                    scen.units.size(), scen.regions.size());
        float cx = 0, cz = 0;
        int n = 0;
        for (const auto& u : scen.units) {
            std::string id = u.objectName;
            std::transform(id.begin(), id.end(), id.begin(), ::tolower);
            int player = std::clamp(u.player, 0, 3);
            float wx = float(u.x) * 16.0f + 8.0f, wz = float(u.z) * 16.0f + 8.0f;
            // .crt facing is degrees (shipped maps use 180 = due south).
            float heading = float(u.angle) * 3.14159265f / 180.0f;
            int uid = spawn(id, wx, wz, heading, player);
            if (uid >= 0) {
                if (ta::sim::Unit* su = writableWorld().unit(uid)) {   // apply the .crt stats
                    if (su->type)
                        su->hp = ta::sim::Fixed::fromFloat(su->type->maxHp * float(std::clamp(u.health, 0, 100)) / 100.0f);
                    writableWorld().setKills(*su, u.kills);   // TA veterancy (an old rank file's rank x 5)
                }
                if (player == 0) { cx += wx; cz += wz; ++n; }
            }
        }
        if (n) mapView_.setOffset(cx / float(n) - 640 / 0.9f,
                                  cz / float(n) - 400 / 0.9f);
        loadFeatures();

        // The .crt's per-player trigger rules run in the SIM (lockstep +
        // hashed) via ScenarioScript, replacing the old client-side heuristic:
        // conditions/actions (spawn, victory/defeat, flags, timers, messages)
        // are evaluated in World::tick and folded into stateHash. Display
        // actions surface as HUD notices (drained in simStep).
        if (!scen.players.empty() || !scen.regions.empty())
            writableWorld().setScenario(std::make_unique<ta::sim::ScenarioScript>(
                scen, registry_, localPlayer_, world_.numPlayers(),
                mapView_.map().width, mapView_.map().height));
    }

    // The local (server-less) match layouts: TA_FFA=N, then the 2-player commander
    // start (+ the demo armies). Returns true when it fully set the match up (FFA),
    // false to fall through to the ctor's startAtCommander tail. Dev path only: it does
    // not call setupMatch, so it can drift from the real match setup (TA_FFA resolves
    // godType by hand below for exactly that reason).
    bool GameView::setupLocalMatch(bool demo, bool bare, const std::string& side,
                                   const std::string& aiSide,
                                   const std::vector<std::pair<float, float>>& starts,
                                   float cx, float cz, float px, float pz, float ax, float az) {
        // Dev harness: TA_FFA=N or TA_FFA=N,t0.t1.t2... sets up an N-player
        // game (each on its own team unless a team list is given), one commander +
        // a small army per player at N start positions, all AI-driven. Verifies
        // the 10-player / team / shared-vision / win-condition paths before the
        // real lobby exists. (multiplayer M1)
        if (const char* ff = ta::devEnv("TA_FFA")) {
            int n = std::atoi(ff);
            n = std::clamp(n, 2, ta::sim::kMaxPlayers);
            std::vector<int> teams(size_t(n), 0);
            for (int i = 0; i < n; ++i) teams[size_t(i)] = i;   // default: FFA
            if (const char* comma = std::strchr(ff, ',')) {     // optional team list
                std::string ts(comma + 1);
                int i = 0;
                for (size_t p = 0; p < ts.size() && i < n; ) {
                    size_t dot = ts.find('.', p);
                    teams[size_t(i++)] = std::atoi(ts.substr(p, dot - p).c_str());
                    if (dot == std::string::npos) break;
                    p = dot + 1;
                }
            }
            writableWorld().setPlayerCount(n);
            for (int i = 0; i < n; ++i) writableWorld().setTeam(i, teams[size_t(i)]);
            // Spread players across the N most mutually-distant start positions
            // (fall back to a ring around centre when the map has too few).
            std::vector<std::pair<float, float>> spots = starts;
            while (int(spots.size()) < n) {
                float ang = float(spots.size()) / float(n) * 6.2831853f;
                spots.push_back({cx + std::cos(ang) * 300, cz + std::sin(ang) * 300});
            }
            for (int i = 0; i < n; ++i) {
                std::string fkSide = ta::sides::lowerPrefix(i % ta::sides::count());
                const FactionKit& fk = kit(fkSide);
                float mx = spots[size_t(i)].first, mz = spots[size_t(i)].second;
                int mon = spawn(fk.commander, mx, mz, 0, i);
                if (i == 0) { playerCommanderId_ = mon; builderId_ = mon; }
                for (int s = 0; s < 4; ++s)
                    spawn(fk.squad[s % 4], mx + (s % 2) * 26 - 13, mz - 50 + (s / 2) * 26, 0, i);
                writableWorld().setStartResources(i, 1000, 1000);
            }
            ffaPlayers_ = n;
            for (auto& u : world_.units()) {
                if (!u.type || u.type->canMove) continue;
                ta::sim::blockFootprint(writableWorld().nav(), *u.type, u.x.toFloat(), u.z.toFloat(), true);
            }
            return true;
        }
        if (!bare) {
        const FactionKit& pk = kit(side);
        const FactionKit& ak = kit(aiSide);
        // The commanders face one another.
        float pFace = std::atan2(ax - px, az - pz);
        float aFace = std::atan2(px - ax, pz - az);
        playerCommanderId_ = spawn(pk.commander, px, pz, pFace, 0);
        builderId_ = playerCommanderId_;
        aiCommanderId_ = spawn(ak.commander, ax, az, aFace, 1);
        // A skirmish's opening economy (TA's default 1000 metal / 1000 energy).
        writableWorld().setStartResources(0, 1000, 1000);
        writableWorld().setStartResources(1, 1000, 1000);
        if (demo) {
            // Showcase: skip the slow build-up and pit two ready armies at the
            // start positions against each other.
            std::vector<int> playerA, playerB;
            // TA_SHOT_SQUADS=1: a still life for the under-unit squad marker -- the
            // first three troops in control group 1, the rest in formation 2, the
            // commander in group 10 ("0"), spread out with no keeps and no attack orders
            // so the labels neither overlap nor walk away.
            const bool squadShot = ta::devFlag("TA_SHOT_SQUADS");
            const float sp = squadShot ? 2.5f : 1.0f;
            for (int i = 0; i < 6; ++i) {
                int a = spawn(pk.squad[i % 4], px + float(i % 2) * 26 * sp + (sp - 1) * 40,
                              pz - 60 * sp + float(i / 2) * 30 * sp, pFace, 0);
                int b = spawn(ak.squad[i % 4], ax + float(i % 2) * 26,
                              az - 60 + float(i / 2) * 30, aFace, 1);
                if (a >= 0) playerA.push_back(a);
                if (b >= 0) playerB.push_back(b);
            }
            if (pk.keep[0] && !squadShot) keepId_ = spawn(pk.keep, px, pz + 60, pFace, 0);
            if (ak.keep[0] && !squadShot) aiKeepId_ = spawn(ak.keep, ax, az + 60, aFace, 1);
            if (squadShot) {
                for (size_t k = 0; k < playerA.size(); ++k)
                    writableWorld().unit(playerA[k])->squad = k < 3 ? 1 : -2;
                if (auto* m = writableWorld().unit(playerCommanderId_)) m->squad = 10;
                std::fprintf(stderr, "squad shot: player army at %.0f %.0f\n", px, pz);
            }
            if (!squadShot && !playerA.empty() && !playerB.empty()) {
                for (size_t k = 0; k < playerA.size(); ++k)
                    writableWorld().attack(playerA[k], playerB[k % playerB.size()], false);
                for (size_t k = 0; k < playerB.size(); ++k)
                    writableWorld().attack(playerB[k], playerA[k % playerA.size()], false);
            }
            // Showcase: pre-select the player's factory so the command panel shows its
            // build menu and the info bar shows the portrait (a fuller HUD for
            // screenshots). Harmless otherwise -- demo is a dev/showcase mode.
            if (keepId_ >= 0) selectOnly(keepId_);
            else if (!playerA.empty() && !squadShot) selectOnly(playerA.front());
        }
        }
        return false;
    }

    void GameView::setupPatrolPerf() {
        // Exactly 16,000 living units: eight commanders plus 1,999 troops per
        // player. This is a local developer fixture, not a lobby cap option.
        ta::sim::MatchConfig cfg;
        cfg.vfs = &vfs_;
        cfg.mapPath = mapPath_;
        cfg.stressTest = true;
        cfg.stressImmediate = true;   // counts and orders the whole army below
        cfg.unitCap = (1999 * 100 + 94) / 95;
        for (int p = 0; p < 8; ++p)
            cfg.slots.push_back({true, p % std::max(1, ta::sides::count()), 0, 2, true, false});
        const auto spots = ta::sim::setupMatch(writableWorld(), registry_, cfg);
        if (world_.units().size() != 16000)
            throw std::runtime_error("patrol performance map must fit exactly 16000 units");
        writableWorld().setUnitCap(2000);
        for (const auto& u : world_.units())
            if (u.type && !u.type->isStructure())
                writableWorld().patrol(u.id, float(writableWorld().nav().width() * 16) - u.x.toFloat(),
                              u.z.toFloat());
        noFog_ = true;
        edgeScrollOn_ = false;
        patrolPerfAccum_ = 0.0f;
        float zoom = 0.9f;
        if (const char* value = ta::devEnv("TA_PATROL_PERF_ZOOM"))
            zoom = std::clamp(float(std::atof(value)), 0.1f, 3.0f);
        mapView_.setZoom(zoom);
        if (!spots.empty()) lookAt(spots[0].first, spots[0].second);
        std::printf("PATROL_PERF initial=%zu (local, no AI/combat/network)\n",
                    world_.units().size());
        std::fflush(stdout);
    }

    void GameView::navyDemo() {
        struct S { const char* t; float x, z; int player; };
        const S fleet[] = {
            {"armroy", 1150, 1250, 0}, {"armpt", 1080, 1150, 0},
            {"armpt", 1220, 1130, 0},  {"armsub", 1020, 1260, 0},
            {"armroy", 1100, 1360, 0},
            {"corroy", 1750, 1500, 1}, {"corroy", 1830, 1600, 1},
            {"corpt", 1700, 1400, 1}, {"corpt", 1780, 1420, 1},
            {"corsub", 1650, 1500, 1},
        };
        std::vector<int> a, b;
        for (const auto& sp : fleet) {
            int id = spawn(sp.t, sp.x, sp.z, sp.player == 0 ? 1.57f : -1.57f, sp.player);
            if (id >= 0) (sp.player == 0 ? a : b).push_back(id);
        }
        for (size_t i = 0; i < a.size(); ++i)
            writableWorld().attack(a[i], b[i % b.size()], false);
        for (size_t i = 0; i < b.size(); ++i)
            writableWorld().attack(b[i], a[i % a.size()], false);
        mapView_.setOffset(1500 - 640 / 0.9f, 1380 - 400 / 0.9f);
    }

    void GameView::testBuild() {
        // Formation flyers over the front (Legion, for --shot captures mid-move and on arrival with --time):
        // an Alt+1 formation of 30 Peewees and 8 Freedom Fighters right-clicked TA_FLYER_FRONT px east of the map's
        // centre (negative: west). TA_FLYER_FRONT_RETAIL=1 keeps Retail pathfinding for a comparison;
        // TA_FLYER_FRONT_GROUND=N spawns N Peewees instead of 30 (1: a squad narrower than its flyers).
        if (const char* front=ta::devEnv("TA_FLYER_FRONT")) {
            writableWorld().setPathfindingMode(ta::devFlag("TA_FLYER_FRONT_RETAIL") ? ta::sim::PathfindingMode::Retail
                                                                                       : ta::sim::PathfindingMode::Legion);
            writableWorld().setPathService(true);   // as a real match's setup (sim/matchsetup.cpp): Legion plans through it
            const float x0=float(mapView_.map().blocksX)*16.0f,z0=float(mapView_.map().blocksY)*16.0f,east=float(std::atof(front));
            const float side=east<0?1.f:-1.f;   // the block starts on the far side of the centre from its goal
            const char* flyer=ta::devEnv("TA_FLYER_FRONT_TYPE");
            selection_.clear();
            const char* ground=ta::devEnv("TA_FLYER_FRONT_GROUND");
            for(int i=0,n=ground?std::max(std::atoi(ground),1):30;i<n;++i)selection_.push_back(spawn("armpw",x0+side*(300+float(i/6)*40),z0-100+float(i%6)*40,0,localPlayer_));
            for(int i=0;i<8;++i)selection_.push_back(spawn(flyer?flyer:"armfig",x0+side*(500+float(i/4)*60),z0-90+float(i%4)*60,0,localPlayer_));
            std::erase_if(selection_,[](int id){return id<0;});
            for(const int id:selection_)issueSquad(id,-1);
            captureFrame();beginFrame();
            rightClickOrder(x0+east,z0,false);
            return;
        }
        // Info-bar portrait strip still life (--testbuild --shot): TA_SELPORTRAITS=
        // "armpw:5,armrock:3,..." spawns that many of each type at the map centre and
        // selects them all, in that order (the first type leads the strip).
        if (const char* list=ta::devEnv("TA_SELPORTRAITS")) {
            const float x0=float(mapView_.map().blocksX)*16.0f,z0=float(mapView_.map().blocksY)*16.0f;
            selection_.clear();
            std::string spec(list);
            int k=0;
            for (size_t p=0;p<spec.size();) {
                size_t comma=spec.find(',',p);
                std::string one=spec.substr(p,comma==std::string::npos?std::string::npos:comma-p);
                size_t colon=one.find(':');
                const std::string type=one.substr(0,colon);
                const int n=colon==std::string::npos?1:std::max(1,std::atoi(one.c_str()+colon+1));
                for (int i=0;i<n;++i,++k)
                    selection_.push_back(spawn(type,x0-300+float(k%16)*40,z0-120+float(k/16)*40,0,localPlayer_));
                if (comma==std::string::npos) break;
                p=comma+1;
            }
            std::erase_if(selection_,[](int id){return id<0;});
            // TA_SELPORTRAITS_HURT: every other unit at a partial HP, so the bar's group
            // totals differ from any one unit's.
            // TA_SELPORTRAITS_SQUAD=N (-N formation): put them all in that squad.
            const bool hurt=ta::devFlag("TA_SELPORTRAITS_HURT");
            const char* sq=ta::devEnv("TA_SELPORTRAITS_SQUAD");
            for (size_t i=0;i<selection_.size();++i) {
                auto* su=writableWorld().unit(selection_[i]);
                if (!su || !su->type) continue;
                if (hurt && i%2==0) su->hp=ta::sim::Fixed::fromFloat(float(su->type->maxHp)*(0.25f+0.1f*float(i%5)));
                if (sq) su->squad=int8_t(std::atoi(sq));
            }
            // TA_SELPORTRAITS_ORDERS: the order overlay's still life (shoot it with
            // TA_SHOT_SHIFT=1). Cloakers cloak on a full pool, the first unit that
            // cannot cloak attacks an enemy A.K. to the south-east, and a reclaimer is
            // given an area clear round the tree to the north-east.
            if (ta::devFlag("TA_SELPORTRAITS_ORDERS")) {
                const int enemy=spawn("corak",x0+100,z0+120,0,localPlayer_==0?1:0);
                bool attacker=false,clearer=false;
                for (int id:selection_) {
                    auto* su=writableWorld().unit(id);
                    if (!su || !su->type) continue;
                    if (su->type->canCloak) {su->cloakOn=true;continue;}
                    if (!attacker && enemy>=0) {writableWorld().attack(id,enemy,false);attacker=true;continue;}
                    if (!clearer && su->type->canReclaim) {
                        writableWorld().reclaimArea(id,x0-140,z0-180,x0+20,z0-20,false);clearer=true;
                    }
                }
            }
            // TA_SELPORTRAITS_CONJURE=type: the front unit (a builder) lays that site at
            // the first legal spot near it, a third built, so the bar shows a build target.
            if (const char* ct=ta::devEnv("TA_SELPORTRAITS_CONJURE"); ct && !selection_.empty())
                if (const auto* tt=registry_.find(ct); tt)
                    if (const auto* b=world_.unit(selection_.front()))
                        for (float r=90,bx=b->x.toFloat(),bz=b->z.toFloat();r<400;r+=24)
                            for (float a=0;a<6.28f;a+=0.5f)
                                if (const float x=bx+std::cos(a)*r,z=bz+std::sin(a)*r;world_.canPlace(tt,x,z))
                                    if (const int site=writableWorld().startBuild(selection_.front(),tt,x,z)) {
                                        writableWorld().unit(site)->hp=ta::sim::Fixed::fromFloat(float(tt->maxHp)/3);
                                        r=400;break;
                                    }
            noFog_=true;edgeScrollOn_=false;
            mapView_.setOffset(x0-640/mapView_.zoom(),z0-400/mapView_.zoom());
            captureFrame();   // publish them, or the next frame pin prunes the selection
            std::fprintf(stderr,"selportraits: %zu units selected\n",selection_.size());
            return;
        }
        if (ta::devFlag("TA_RETAIL_FOG_TEST")) {
            // The display fog is retail's single sight plane (World::viewCounts) seen
            // through LosOn: on flat ground an enemy at 0.9x our unit's sight is drawn
            // and at 1.1x is not; from a hill the cone reaches 1.5x; radar puts an
            // enemy on the minimap but never in the main view. Fixtures are found on
            // the loaded map. TA_RETAIL_FOG_CAPTURE=<prefix> writes
            // <prefix>-flat/-hill/-radar.png (the whole frame, minimap included).
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("retail fog: "+what);
                std::fprintf(stderr,"PASS: retail fog: %s\n",what.c_str());
            };
            constexpr int W=1280,H=960;
            noFog_=false;edgeScrollOn_=false;tacticalDotsOpt_=false;healthBars_=0;
            fogStill_=true;
            writableWorld().setPlayerCount(2);
            // A Peewee sees 280 px (armpw.fbi), far enough that its bounding box and the
            // 32 px cells do not keep an enemy in view past 1.1x; the radar tower is armrad.
            const auto* sword=registry_.find("armpw");
            const auto* keep=registry_.find("armrad");
            check(sword && keep && keep->radar>0,"fixture types (a Peewee, a radar tower)");
            const auto& m=mapView_.map();
            const int sea=world_.mapSea();
            const auto h16=[&](int x,int z) {
                return int(m.heights[size_t(std::clamp(z,0,m.height-1))*size_t(m.width)+size_t(std::clamp(x,0,m.width-1))]);
            };
            const float S=float(world_.sightDistance(*sword)),R=float(world_.radarDistance(*keep));
            const auto heights=ta::sim::retailExplorationHeights(m.width,m.height,uint8_t(sea),h16);
            const int pw=m.width/2,ph=m.height/2;
            // Would retail's cone (4c6800) admit a target unit standing on (tx,tz)
            // for a swordsman on (x,z)? Its LosOn corners, as World::playerSees.
            const auto admits=[&](float x,float z,float tx,float tz) {
                ta::sim::RetailSightFootprint f;
                f.x=int16_t(int(x)/32);f.z=int16_t(int(z)/32);f.distance=int16_t(S);f.sightHeight=sword->sightHeight;
                f.eyeHeight=std::max(h16(int(x)/16,int(z)/16),sea+1)+sword->sightHeight;
                const int radius=ta::sim::retailSightRadius(f);
                const float slope=ta::sim::retailSightSlope(f);
                auto at=[&](int cx,int cz){return heights[size_t(cz)*size_t(pw)+size_t(cx)];};
                const int y=h16(int(tx)/16,int(tz)/16),top=y+(sword->modelTop>>16);
                for (const auto& [cx,cy,cz]:{std::array<float,3>{tx-8*sword->footX,float(top),tz-8*sword->footZ},
                        {tx+8*sword->footX,float(top),tz-8*sword->footZ},{tx+8*sword->footX,float(y),tz+8*sword->footZ},
                        {tx-8*sword->footX,float(y),tz+8*sword->footZ}}) {
                    const int px=int(cx)>>5,pz=(int(cz)-(int(cy)>>1))>>5;
                    if (px<0 || pz<0 || px>=pw || pz>=ph) continue;
                    if (ta::sim::retailSightAdmits(f,px,pz,radius*radius,slope,at)) return true;
                }
                return false;
            };
            // Flat: one height over 1.4 sight distances either way along x.
            float fx=-1,fz=-1,hx=-1,hz=-1,hdx=0;
            const int reach=int(1.4f*S/16)+2;
            for (int z=m.height/2,step=0;fx<0 && step<m.height;++step,z=m.height/2+((step&1)?1:-1)*(step/2)*2) {
                if (z<8 || z>=m.height-8) continue;
                for (int x=reach+4;x<m.width-reach-4 && fx<0;x+=3) {
                    const int h0=h16(x,z);
                    if (h0<=sea+2) continue;
                    bool flat=true;
                    for (int dz=-3;dz<=3 && flat;++dz) for (int dx=-reach;dx<=reach && flat;++dx) flat=h16(x+dx,z+dz)==h0;
                    // Our unit on a cell centre, so the 32 px cells do not decide the result.
                    if (flat) {fx=float((x/2)*32+16);fz=float((z/2)*32+16);}
                }
            }
            check(fx>0,"found flat ground for 1.4 sight distances");
            // A hill: our unit high, the target 1.5x away (east or west) on lower ground
            // that retail's cone still reaches -- far from the flat fixture.
            for (int z=8;z<m.height-8 && hx<0;z+=2)
                for (int x=8;x<m.width-8 && hx<0;x+=2) {
                    const float ox=float((x/2)*32+16),oz=float((z/2)*32+16);
                    if (std::hypot(ox-fx,oz-fz)<4*S) continue;
                    for (float dir:{1.0f,-1.0f}) {
                        const float tx=ox+dir*1.5f*S;
                        if (tx<64 || tx>float(m.width*16-64)) continue;
                        if (h16(int(tx)/16,int(oz)/16)<=sea+2 || h16(x,z)<=sea+2) continue;
                        if (admits(ox,oz,tx,oz)) {hx=ox;hz=oz;hdx=dir;break;}
                    }
                }
            check(hx>0,"found a hill whose cone reaches 1.5 sight distances");
            // Radar: a keep, and an enemy at 0.9x its radar distance -- beyond twice its
            // sight (the footprint's cap) -- far from both other fixtures.
            float kx=-1,kz=-1;
            const float rd=0.9f*R;
            check(rd>2.0f*float(world_.sightDistance(*keep))+64,"the radar fixture is beyond any sight");
            for (int z=8;z<m.height-8 && kx<0;z+=4)
                for (int x=8;x<m.width-8 && kx<0;x+=4) {
                    const float ox=float(x*16),oz=float(z*16);
                    if (ox+rd>float(m.width*16-64) || std::hypot(ox-fx,oz-fz)<rd+4*S || std::hypot(ox-hx,oz-hz)<rd+4*S ||
                        std::hypot(ox+rd-fx,oz-fz)<4*S || std::hypot(ox+rd-hx,oz-hz)<4*S) continue;
                    if (!world_.canPlace(keep,ox,oz) || h16(int(ox+rd)/16,z)<=sea+2) continue;
                    // Nor in sight of the units the local match opened with (the commanders).
                    bool seen=false;
                    for (const auto& u:world_.units())
                        if (u.alive() && u.player==0 && std::hypot(ox+rd-u.x.toFloat(),oz-u.z.toFloat())<4*S) seen=true;
                    if (seen) continue;
                    kx=ox;kz=oz;
                }
            check(kx>0,"found room for the radar fixture");
            std::fprintf(stderr,"retail fog: sight %.0f radar %.0f flat %.0f,%.0f hill %.0f,%.0f (h %d -> %d) keep %.0f,%.0f\n",
                S,R,fx,fz,hx,hz,h16(int(hx)/16,int(hz)/16),h16(int(hx+hdx*1.5f*S)/16,int(hz)/16),kx,kz);
            const int own=spawn("armpw",fx,fz,0,0);
            const int inside=spawn("armpw",fx+0.9f*S,fz,0,1);
            const int outside=spawn("armpw",fx-1.1f*S,fz,0,1);
            const int high=spawn("armpw",hx,hz,0,0);
            const int below=spawn("armpw",hx+hdx*1.5f*S,hz,0,1);
            const int tower=spawn("armrad",kx,kz,0,0);
            const int blip=spawn("armpw",kx+rd,kz,0,1);
            check(own>0 && inside>0 && outside>0 && high>0 && below>0 && tower>0 && blip>0,"fixture spawned");
            for (int i=0;i<30;++i) writableWorld().tick(1.f/30);
            const char* capture=ta::devEnv("TA_RETAIL_FOG_CAPTURE");
            const auto frame=[&](float cx,float cz,const char* tag,float zoom=1.0f) {
                captureFrame();beginFrame();cosmeticStep(0);
                mapView_.setZoom(zoom);
                mapView_.setOffset(cx-float(W-300)*0.5f/zoom,cz-terrainLift(cx,cz)-float(H)*0.5f/zoom);
                prepare(W,H);draw(W,H);
                if (capture) {
                    int ow=0,oh=0;SDL_GetRendererOutputSize(ren_,&ow,&oh);
                    std::vector<uint8_t> px(size_t(ow)*size_t(oh)*4);
                    if (SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_RGBA32,px.data(),ow*4)==0)
                        ta::png::write(std::string(capture)+"-"+tag+".png",ow,oh,px);
                }
                endFrame();
            };
            const auto drawn=[&](int id) {
                return std::any_of(paintItems_.begin(),paintItems_.end(),[&](const auto& it){return it.u && it.u->id==id;});
            };
            frame(fx,fz,"flat");
            check(drawn(own),"own unit drawn");
            check(drawn(inside),"flat ground: an enemy at 0.9x sight is drawn");
            check(!drawn(outside),"flat ground: an enemy at 1.1x sight is not");
            frame(hx+hdx*0.75f*S,hz,"hill");
            check(drawn(high) && drawn(below),"from a hill: an enemy at 1.5x sight is drawn");
            frame(kx+rd*0.5f,kz,"radar",0.6f);   // both on screen: not drawn is the fog, not the cull
            const auto* dot=frameUnitP(blip);
            check(dot && radarVisible(*dot),"radar: the enemy is a minimap dot");
            check(!drawn(blip),"radar: the enemy is not drawn in the main view");
            if (capture) {   // and what was seen and is no longer: our unit walks west
                if (auto* u=writableWorld().unit(own)) u->x=ta::sim::Fixed::fromFloat(fx-0.9f*S);
                for (int i=0;i<10;++i) writableWorld().tick(1.f/30);
                frame(fx-0.45f*S,fz,"explored",0.8f);
            }
            std::fprintf(stderr,"PASS: retail fog (sight %.0f, radar %.0f)\n",S,R);
            return;
        }
        if (ta::devFlag("TA_SPECTATOR_BARS_TEST")) {
            // A spectator sees every side's health bars. Spectating stops the fog
            // pass, but the world keeps the last fog it built for the seat the client
            // was given (localPlayer_), so the overlays must answer from noFog_ and
            // not from that stale buffer. As a player the same fixture still hides
            // an enemy's bar under fog.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("spectator bars: "+what);
                std::fprintf(stderr,"PASS: spectator bars: %s\n",what.c_str());
            };
            constexpr int W=1280,H=960;
            constexpr float zoom=0.6f;
            noFog_=false;edgeScrollOn_=false;tacticalDotsOpt_=false;healthBars_=1;
            writableWorld().setPlayerCount(3);
            const float x0=float(mapView_.map().blocksX)*16.0f,z0=float(mapView_.map().blocksY)*16.0f;
            const int own=spawn("armpw",x0,z0,0,0);
            // In sight to the south: retail centres a sight footprint on the unit's
            // world cell, but tests units by their projected corners (z - y/2), so on
            // this raised ground the view reaches further south than north.
            const int foeSeen=spawn("armpw",x0+60,z0+60,0,1);
            const int foeHidden=spawn("armpw",x0+700,z0+200,0,1);
            const int thirdHidden=spawn("armpw",x0-700,z0-200,0,2);
            check(own>=0 && foeSeen>=0 && foeHidden>=0 && thirdHidden>=0,"fixture spawned");
            check(!world_.allied(0,1) && !world_.allied(0,2) && !world_.allied(1,2),"three separate sides");
            // A third of a second of sim builds the seat's fog the way a game's opening does.
            for (int i=0;i<10;++i) writableWorld().tick(1.f/30);
            for (const int id:{own,foeSeen,foeHidden,thirdHidden})
                if (auto* u=writableWorld().unit(id)) u->hp=ta::sim::Fixed::fromFloat(float(u->type->maxHp)/2);
            const auto frame=[&] {
                captureFrame();beginFrame();cosmeticStep(0);
                mapView_.setZoom(zoom);
                mapView_.setOffset(x0-float(W-300)*0.5f/zoom,z0-float(H)*0.5f/zoom);
                prepare(W,H);draw(W,H);
                endFrame();
            };
            const auto bar=[&](int id) {
                return std::find(debugHealthBarIds_.begin(),debugHealthBarIds_.end(),id)!=debugHealthBarIds_.end();
            };
            frame();
            check(bar(own) && bar(foeSeen),"player: own and in-sight enemy bars drawn");
            check(!bar(foeHidden) && !bar(thirdHidden),"player: enemy bars under fog stay hidden");
            beginSpectating();
            // Draw the seat's last frame again, the way the first frame after joining
            // as a spectator does: its plane is stale, and noFog_ must answer.
            beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);endFrame();
            check(!front().fog.empty(),"spectator: the shown frame still holds the seat's fog plane (stale)");
            check(bar(own) && bar(foeSeen) && bar(foeHidden) && bar(thirdHidden),
                  "spectator: every side's bars drawn");
            std::fprintf(stderr,"PASS: spectator bars (%zu bars)\n",debugHealthBarIds_.size());
            return;
        }
        if (const char* prefix = ta::devEnv("TA_REPLAYBAR_SHOT")) {
            // The replay bar for the eye: <prefix>-<size>-ui<scale>-<font>[-hud].png.
            noFog_=true;edgeScrollOn_=false;
            writableWorld().setPlayerCount(2);
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int a=spawn("armpw",cx,cz,0,0);
            const int b=spawn("corak",cx+60,cz+30,0,1);
            const int builder=spawn("armcom",cx-60,cz+60,0,0);
            for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
            replayMode_=true;
            replayBundles_.resize(23*60*30+2*30);
            replayTick_=3*60*30+30*30;
            replaySpeed_=7;
            replayRealRate_=27;
            const auto shot=[&](int W,int H,const std::string& name) {
                for (int i=0;i<3;++i) {
                    captureFrame();beginFrame();cosmeticStep(0);
                    mapView_.setZoom(1.0f);
                    const float vw=float(mapViewW(W)),vh=float(mapViewH(H));
                    mapView_.setOffset(cx-vw/2,cz-vh/2);
                    prepare(W,H);draw(W,H);
                    if (i==2) {
                        int ow=0,oh=0;
                        SDL_GetRendererOutputSize(ren_,&ow,&oh);
                        std::vector<uint8_t> px(size_t(ow)*size_t(oh)*4,0);
                        SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_RGBA32,px.data(),ow*4);
                        ta::png::write(std::string(prefix)+"-"+name+".png",ow,oh,px);
                    }
                    endFrame();
                }
            };
            const auto initialFont=ta::uiFontStyle();
            for (const auto style:{ta::UiFontStyle::Modern,ta::UiFontStyle::Classic})
                for (const float scale:{1.0f,2.0f}) {
                    ta::setUiFontStyle(style);
                    uiScale_=scale;
                    const std::string name=std::string("1024-ui")+std::to_string(int(scale))+
                        (style==ta::UiFontStyle::Modern ? "-modern" : "-classic");
                    selection_.clear();
                    renderStudyMouse(1,400);
                    shot(1024,768,name);
                    // Hovering the timeline at a third, with a builder of the player on show.
                    selection_={builder};
                    mouseX_=replayBar_.track.x+replayBar_.track.w/3;
                    mouseY_=replayBar_.track.y+2;
                    shot(1024,768,name+"-hud");
                }
            ta::setUiFontStyle(initialFont);uiScale_=1;
            selection_={b};renderStudyMouse(1,400);
            shot(1024,768,"1024-ui1-core");
            selection_={a};
            shot(1024,768,"1024-ui1-arm");
            replayMode_=false;replayBundles_.clear();replayTick_=0;selection_.clear();
            std::fprintf(stderr,"PASS: replay bar shots\n");
            return;
        }
        if (const char* mode = ta::devEnv("TA_WATCHONLY_TEST")) {
            // A watcher (replay or spectator) through the real input() and draw(): it
            // can select anything and see that player's interface, and nothing it does
            // reaches the game or is acknowledged in any way.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("watch only: "+what);
                std::fprintf(stderr,"PASS: watch only: %s\n",what.c_str());
            };
            constexpr int W=1280,H=960;
            const bool replay=std::string(mode)=="replay";
            noFog_=true;edgeScrollOn_=false;
            writableWorld().setPlayerCount(3);
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int a1=spawn("armpw",cx-80,cz,0,0),a2=spawn("armpw",cx-50,cz,0,0);
            const int b1=spawn("corak",cx+40,cz,0,1),b2=spawn("corak",cx+70,cz,0,1),b3=spawn("corak",cx+100,cz,0,1);
            const int builder=spawn("armcom",cx-80,cz+90,0,0);
            const int factory=spawn("corlab",cx+70,cz+130,0,1);
            check(a1>=0 && a2>=0 && b1>=0 && b2>=0 && b3>=0 && builder>=0 && factory>=0,"fixture spawned");
            // Hold fire: TA's Peewees and A.K.s 120 px apart would shoot it out, and the
            // replay part below needs the fixture alive until it kills b3 itself.
            for (const int id:{a1,a2,b1,b2,b3,builder,factory}) writableWorld().unit(id)->fireState=0;
            for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
            if (replay) replayMode_=true; else beginSpectating();
            const auto frame=[&] {
                captureFrame();beginFrame();cosmeticStep(0);
                prepare(W,H);draw(W,H);
                endFrame();
            };
            mapView_.setZoom(1.0f);
            const float vw=float(mapViewW(W)),vh=float(mapViewH(H));
            mapView_.setOffset(cx-vw/2,cz-vh/2);
            frame();frame();
            const uint64_t before=world_.stateHash();
            bool tint=false;
            int sounded=0;   // everything the sound bank was asked to play
            sounds_.requestHook=[&](const SoundBank::RequestTrace&) {++sounded;};
            // Nothing may have got out, been armed or been acknowledged. Checked after
            // every step: the state hash catches a command the harness's local world took.
            const auto untouched=[&](const std::string& what) {
                check(outbox_.empty() && !click_,what+": nothing left the client");
                check(world_.stateHash()==before,what+": the world is unchanged");
                check(sounded==0,what+": no sound");
                check(!pendingCmd_ && !placing_ && !rightDrag_ && !buildDrag_,what+": nothing armed");
                check(notice_.empty(),what+": no notice");
                check(!giveUnitsMenu_,what+": no diplomacy");
                const auto cursor=desiredCursor(tint);
                check((cursor==ta::CursorId::Normal || cursor==ta::CursorId::Select) && !tint,what+": a watcher's cursor");
            };
            const auto mouse=[&](Uint32 type,float x,float y,Uint8 button,int clicks=1) {
                SDL_Event e{};e.type=type;e.button.x=int(x);e.button.y=int(y);e.button.button=button;e.button.clicks=Uint8(clicks);
                if (type==SDL_MOUSEMOTION) {
                    e.motion.x=int(x);e.motion.y=int(y);e.motion.xrel=0;e.motion.yrel=0;e.motion.state=0;
                }
                inputFromView(e,W,H);   // points in the world view (the frame's are shifted below)
            };
            const auto clickAt=[&](float x,float y,Uint8 button=SDL_BUTTON_LEFT) {
                mouse(SDL_MOUSEMOTION,x,y,0);
                mouse(SDL_MOUSEBUTTONDOWN,x,y,button);
                mouse(SDL_MOUSEBUTTONUP,x,y,button);
                frame();
            };
            const auto at=[&](int id) { const auto* u=frameUnitP(id);return unitScreen(frameUnit(u->id)); };
            // The TA sidebar page's buttons on show (gameview_tahud.cpp, the side panel), each with a
            // point inside it: the panel scanned through the page's own hit test.
            const auto taButtons=[&] {
                std::vector<std::pair<std::string,SDL_FPoint>> out;
                ta::TaGuiView* page=taHudPage_.empty() ? nullptr : taPage(taHudPage_);
                if (!page) return out;
                std::set<std::string> seen;
                const SDL_FRect& o=taOrdersRect_;   // the order buttons in the right-hand column
                for (float y=o.y;y<o.y+o.h;y+=3)
                    for (float x=o.x;x<o.x+o.w;x+=3)
                        if (const auto* g=page->at(x,y,taHudLayout_);
                            g && g->type==ta::tagui::Type::Button && !g->grayedOut && seen.insert(g->name).second)
                            out.push_back({g->name,{x+2-float(viewX0()),y+2-float(viewY0())}});   // as view coordinates
                return out;
            };
            const auto clickUnit=[&](int id,Uint8 button=SDL_BUTTON_LEFT) { const auto p=at(id);clickAt(p.x,p.y,button); };
            check(hudPlayer()==-1 && !hudShown(),"nothing selected: no interface");
            renderStudyMouse(vw*0.5f,30);
            untouched("idle");
            {
                const auto p=at(b1);
                renderStudyMouse(p.x,p.y);
                check(desiredCursor(tint)==ta::CursorId::Select,"Select over another player's unit");
            }
            clickUnit(b1);
            check(selection_==std::vector<int>{b1},"a click selects any player's unit");
            check(hudPlayer()==1 && hudShown(),"the interface is that unit's player's");
            check(ta::sides::find(framePlayer(hudPlayer()).side)==ta::sides::find("CORE"),"fixture: player 1 is Core");
            {
                const SDL_Color shown=factionColor(),seat=playerColor(localPlayer_),owner=playerColor(1);
                check(shown.r==owner.r && shown.g==owner.g && shown.b==owner.b &&
                      (seat.r!=owner.r || seat.g!=owner.g || seat.b!=owner.b),"the fallback panel wears the owner's colour");
            }
            untouched("selecting");
            // (TA's sidebar and top bar are the seat's side's art -- gameview_tahud.cpp
            // draws side_'s -- so unlike the Kingdoms panel nothing here swaps side art.)
            // (The Kingdoms panel's KillCount -- the owner's unit showing its kills to a
            // watcher -- has no gadget in TA's interface, gameview_tahud.cpp.)
            // Right clicks: on the ground, on an enemy, on a friend, on the minimap.
            clickAt(vw*0.5f,H*0.25f,SDL_BUTTON_RIGHT);
            clickUnit(a1,SDL_BUTTON_RIGHT);
            clickUnit(b2,SDL_BUTTON_RIGHT);
            {
                SDL_FRect r=minimapRect(W,H);r.x-=float(viewX0());r.y-=float(viewY0());   // as view coordinates
                const float ox=mapView_.offX();
                clickAt(r.x+r.w*0.5f,r.y+r.h*0.5f,SDL_BUTTON_RIGHT);
                check(mapView_.offX()==ox,"a right click on the minimap does not move the camera");
                clickAt(r.x+r.w*0.2f,r.y+r.h*0.2f);
                check(mapView_.offX()!=ox,"a left click on the minimap moves the camera");
                mapView_.setOffset(cx-vw/2,cz-vh/2);frame();
                draggingMinimap_=false;
            }
            check(selection_==std::vector<int>{b1},"right clicks leave the selection");
            untouched("right clicks");
            const auto press=[&](ta::Act act) {
                const auto chord=hotkeys_.chord(act);
                uint16_t mod=0;
                if (chord.mod & ta::HK_CTRL) mod|=KMOD_CTRL;
                if (chord.mod & ta::HK_SHIFT) mod|=KMOD_SHIFT;
                if (chord.mod & ta::HK_ALT) mod|=KMOD_ALT;
                handleKey(chord.key,mod);
            };
            // Every key the rule blocks, as the player has them bound.
            for (const auto& def:ta::hotkeyDefs()) {
                if (ta::client::watchAllows(def.act) || !hotkeys_.chord(def.act).bound()) continue;
                const float ox=mapView_.offX();
                press(def.act);
                check(mapView_.offX()==ox,std::string(def.id)+": the camera stays");
                untouched(def.id);
            }
            // Pause and speed sit outside the table, and only a private game's host paces it
            // (ta::client::watchMayPace). In a replay they are the transport's, below.
            if (!replay) {
                const bool wasPaused=paused_;
                const int pace=gameSpeed_;
                for (const SDL_Keycode key:{SDLK_PAUSE,SDLK_EQUALS,SDLK_MINUS,SDLK_KP_PLUS,SDLK_KP_MINUS}) handleKey(key,0);
                check(paused_==wasPaused && gameSpeed_==pace,"a watcher's Pause and speed keys change nothing");
                untouched("Pause and speed");
            }
            // The panel: a button draws the true state and a click on it does nothing.
            clickUnit(a1);
            check(selection_==std::vector<int>{a1} && hudPlayer()==0,"another owner replaces the selection");
            frame();
            {
                const auto buttons=taButtons();
                check(!buttons.empty(),"the order buttons are on show");
                for (const auto& [name,p]:buttons) {
                    clickAt(p.x,p.y);
                    check(selection_==std::vector<int>{a1},"a button press ("+name+") does not reach the map");
                }
            }
            untouched("order buttons");
            selection_={builder};frame();frame();
            // A watcher gets no build menu at all (2026-11-08): no row, no icons.
            check(iconRects_.empty() && buildMenuRect_.w<=0,"a selected builder shows no build row");
            untouched("a builder selected");
            // A reclaimer's right press would arm the area drag.
            mouse(SDL_MOUSEBUTTONDOWN,vw*0.5f,H*0.25f,SDL_BUTTON_RIGHT);
            check(!rightDrag_,"a right press arms no reclaim drag");
            mouse(SDL_MOUSEBUTTONUP,vw*0.5f,H*0.25f,SDL_BUTTON_RIGHT);
            // And the backstop, for anything that ever gets past the gates.
            {
                ta::net::Command c;
                c.kind=ta::net::Cmd::Move;c.unitId=a1;c.x=cx+500;c.z=cz+500;
                issue(c);
            }
            untouched("a command forced at issue()");
            selection_={factory};frame();frame();   // the other player's factory, read-only too
            check(hudPlayer()==1,"a factory shows its owner");
            untouched("a factory selected");
            // Boxes: over 2 of one owner and 3 of another, the 3.
            {
                const auto p0=at(a1),p1=at(b3);
                const float y0=p0.y-40,y1=p0.y+30;
                mouse(SDL_MOUSEMOTION,p0.x-25,y0,0);
                mouse(SDL_MOUSEBUTTONDOWN,p0.x-25,y0,SDL_BUTTON_LEFT);
                mouse(SDL_MOUSEMOTION,p1.x+25,y1,0);
                mouse(SDL_MOUSEBUTTONUP,p1.x+25,y1,SDL_BUTTON_LEFT);
                frame();
                auto sorted=selection_;std::sort(sorted.begin(),sorted.end());
                check(sorted==std::vector<int>({b1,b2,b3}),"a box keeps the owner with the most units");
            }
            // Shift+click on another owner replaces, never mixes.
            {
                SDL_SetModState(KMOD_LSHIFT);
                clickUnit(a2);
                SDL_SetModState(KMOD_NONE);
                check(selection_==std::vector<int>{a2},"Shift+click on another owner replaces the selection");
            }
            // A gift or capture can never leave two owners either: the front unit wins.
            selection_={b1,b2};
            writableWorld().unit(b2)->player=0;
            frame();
            check(selection_==std::vector<int>{b1},"after a capture the front unit's owner stays");
            writableWorld().unit(b2)->player=1;
            for (int i=0;i<1;++i) frame();
            const uint64_t afterFixture=world_.stateHash();
            // Groups are the viewer's own: they never touch the units' squads.
            clickUnit(b2);
            handleKey(SDLK_1,KMOD_CTRL);
            check(selection_==std::vector<int>{b2},"assigning a group keeps the selection");
            selection_.clear();
            handleKey(SDLK_1,0);frame();
            check(selection_==std::vector<int>{b2},"a digit recalls the viewer's group");
            check(frameUnitP(b2)->squad==0 && world_.stateHash()==afterFixture,"the group is not a squad");
            check(!trackSel_,"one press does not follow");
            handleKey(SDLK_1,0);frame();
            check(trackSel_,"a second press follows the group");
            trackSel_=false;
            SDL_SetModState(KMOD_LCTRL);
            SDL_Event esc{};esc.type=SDL_KEYDOWN;esc.key.keysym.sym=SDLK_ESCAPE;input(esc,W,H);
            SDL_SetModState(KMOD_NONE);
            selection_.clear();handleKey(SDLK_1,0);frame();
            check(selection_.empty(),"Ctrl+Esc empties the group");
            // T tracks, N steps through the player on show, select keys reach them.
            clickUnit(b1);
            press(ta::Act::TrackSelection);
            check(trackSel_,"T tracks the selection");
            press(ta::Act::TrackSelection);
            check(!trackSel_,"T again stops");
            press(ta::Act::SelectAll);frame();
            check(selection_.size()>=3 && hudPlayer()==1,"select all stays on the player on show");
            selection_={b1};press(ta::Act::NextUnit);frame();
            check(selection_.size()==1 && hudPlayer()==1,"next unit stays on the player on show");
            selection_.clear();frame();
            check(!hudShown(),"Esc-cleared selection drops the interface");
            untouched("the whole run");
            // The transport, through the bar's own rects. A fixture recording of empty ticks.
            if (replay) {
                namespace rb=ta::replaybar;
                replayBundles_.resize(3000);
                replayKeyBudget_=uint64_t(1)<<30;
                std::map<size_t,uint64_t> straight;
                // Dead before the keyframe at 900, long enough to be a body. (Kingdoms' second
                // fixture, a unit still in its death fall at the keyframe, has no TA subject:
                // a TA unit's Killed script leaves its wreck at once.)
                const int body=spawn("armpw",cx-260,cz-260,0,0);
                replayStep(0);
                check(replayKeys_.size()==1 && replayKeys_[0].tick==0,"tick 0 is kept before the first tick runs");
                const auto base=world_.tickCount()-replayTick_;
                straight[replayTick_]=world_.stateHash();
                while (replayTick_<1800) {
                    // It dies on a sliver of HP (HP% a second before = 1): severity 1, so
                    // TA's Killed script leaves a wreck (corpse type 1), not a heap or nothing.
                    if (replayTick_==500) {
                        auto* u=writableWorld().unit(body);
                        u->hpPct1s=u->hpPctCur=1;u->hp=ta::sim::Fixed();
                    }
                    replayTickOnce();straight[replayTick_]=world_.stateHash();
                }
                check(replayKeys_.size()==3 && replayKeys_[2].tick==1800,"a keyframe every 30 s of game");
                {
                    uint64_t held=0;
                    for (const auto& key:replayKeys_) held+=key.bytes;
                    check(held==replayKeyBytes_,"the budget holds the keyframes' own sizes");
                    check(replayKeys_[1].bytes>=sizeof(ta::sim::World)+world_.units().size()*sizeof(ta::sim::Unit),
                          "a keyframe's size is what the copy really took");
                }
                paused_=true;   // the bar works paused, and a jump runs regardless
                frame();
                // Something dies in the part about to be undone: the display must forget it.
                writableWorld().unit(b3)->hp=ta::sim::Fixed();
                for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
                frame();
                check(anims_.count(b3) && anims_.at(b3).dying,"fixture: the dead unit shows dying");
                const auto land=[&] { for (int i=0;i<400 && replaySeekTo_;++i) replayStep(1.f/60);frame(); };
                // Alive here, so alive at every earlier moment.
                int keeper=0;
                for (const int id:{factory,builder,a1,a2,b1,b2})
                    if (const auto* u=frameUnitP(id);!keeper && u && u->alive()) keeper=id;
                check(keeper!=0,"fixture: someone outlives the fighting");
                selection_={keeper};trackSel_=true;
                frame();
                const rb::Layout l=replayBar_;
                const float y=l.track.y+l.track.h/2;
                const auto xFor=[&](size_t tick) { return rb::tickToX(l,tick,3000); };
                mouse(SDL_MOUSEMOTION,xFor(1500),y,0);
                mouse(SDL_MOUSEBUTTONDOWN,xFor(1500),y,SDL_BUTTON_LEFT);
                check(replayDragging_ && replayTick_==1800 && !replaySeekTo_,"a press only picks up the marker");
                SDL_Event move{};move.type=SDL_MOUSEMOTION;move.motion.x=int(xFor(450));move.motion.y=int(y);
                move.motion.state=SDL_BUTTON_LMASK;inputFromView(move,W,H);
                check(!replaySeekTo_,"dragging does not jump yet");
                mouse(SDL_MOUSEBUTTONUP,xFor(450),y,SDL_BUTTON_LEFT);
                check(replaySeekTo_.has_value() && !replayDragging_,"the release commits the jump");
                const size_t target=*replaySeekTo_;
                check(std::abs(int(target)-450)<=10,"the timeline's x picks the tick");
                land();
                check(replayTick_==target,"a jump back lands on its tick, paused or not");
                check(world_.stateHash()==straight.at(target),"a jump back lands on the recorded state");
                check(world_.tickCount()==base+target,"the sim clock went back too");
check(anims_.count(b3) && !anims_.at(b3).dying,"what the future did to a unit's display is gone");
                cameraFrame(0);
                check(selection_==std::vector<int>{keeper},"the selection survives the jump");
                check(trackSel_,"so does T");
                check(hudPlayer()==int(frameUnitP(keeper)->player),"and the interface");
                // A jump's frames can lack a unit its landing has: nothing is decided on them.
                {
                    const int late=spawn("armpw",cx-260,cz-200,0,0);
                    selection_={late};trackSel_=true;
                    frame();
                    check(hudPlayer()==0,"fixture: a unit that exists only here");
                    replayJumpTo(200);
                    frame();cameraFrame(0);
                    check(replaySeekTo_ && selection_==std::vector<int>{late} && trackSel_,"a frame of a jump prunes nothing");
                    check(hudPlayer()==0,"and keeps the interface");
                    land();frame();cameraFrame(0);
                    check(selection_.empty() && !trackSel_ && hudPlayer()==-1,"a unit not alive at the landing drops out");
                }
                // Bodies are dead units' records: a rebuilt display has to bring them back.
                replayJumpTo(900);land();
                check(replayTick_==900 && world_.stateHash()==straight.at(900),"a jump lands on a keyframe's own state");
                {
                    const auto* dead=frameUnitP(body);
                    check(dead && !dead->alive() && dead->corpsePhase,"fixture: a body lies on the field");
                    check(unitType_.registered(body) && unitType_.at(body)!=dead->type->id,"a restored body draws, as its corpse");
                    check(anims_.count(body) && anims_.at(body).dying && anims_.at(body).ownerVmStopRequested,"and does not die again");
                    check(pickRegistered_.complete,"and picking has every record again");
                }
                selection_={keeper};frame();
                // The frame pinned when a restore runs still draws: the display goes with the
                // frame on show, so no frame of a jump is an empty map.
                beginFrame();
                replayJumpTo(300);
                cosmeticStep(0);
                check(unitType_.registered(keeper) && anims_.count(keeper),"the frame on show keeps its units through a restore");
                endFrame();
                land();
                check(unitType_.registered(keeper),"and the landing has them");
                // Silent, and no effects, up to and including the frame that shows the landing.
                {
                    ta::sim::UnitType fleshy=*registry_.find("armpw");
                    fleshy.bodyType="flesh";
                    const auto queueEffects=[&] {
                        std::lock_guard<std::mutex> lock(hitQueueMutex_);
                        ta::sim::World::SoundReq sound;sound.name="warning1";sound.tick=world_.tickCount();
                        missionSoundQueue_.push_back(sound);
                        ta::sim::World::HitFx hit;hit.x=cx;hit.z=cz;
                        hit.target=&fleshy;
                        hitQueue_.push_back({world_.tickCount(),hit});
                    };
                    const auto shown=[&] { beginFrame();replayStep(1.f/60);endFrame(); };
                    shown();
                    sounded=0;
                    queueEffects();
                    shown();
                    check(sounded>0 && !particles_.empty(),"fixture: outside a jump these play");
                    sounded=0;
                    replayJumpTo(1000);
                    for (int i=0;i<400 && replaySeekTo_;++i) replayStep(1.f/60);
                    check(!replaySeekTo_ && replayInTransit_,"the jump is done and its landing not on show");
                    queueEffects();                          // the last ticks the jump ran
                    // A shake sequence that moved with the jump, as a rewind moves it back.
                    frameBuf_[published_].shakeReq.mag=8;frameBuf_[published_].shakeReq.dur=1;
                    shakeSeqSeen_=frameBuf_[published_].shakeReq.seq+1;
                    screenShake_=true;
                    shown();
                    check(!replayInTransit_,"the landing is on show");
                    check(sounded==0,"a jump's last chunk makes no sound on the landing");
                    check(particles_.empty() && rings_.empty() && beams_.empty(),"effects from before and during a jump are gone");
                    check(shakeSeqSeen_==front().shakeReq.seq && shakeTime_==0,"an undone shake does not fire");
                    screenShake_=false;
                    shown();
                    check(sounded==0,"nor on the frame after");
                    queueEffects();
                    shown();
                    check(sounded>0,"the sound is back after the landing");
                    sounded=0;
                    particles_.clear();
                }
                // A takeover in a future that is gone: the name goes back, and a jump posts nothing.
                playerAi_[1]=true;
                replayJumpTo(950);land();
                check(!playerAi_[1],"a rewind takes back a takeover");
                {
                    ta::net::Event takeover;takeover.kind=ta::net::Event::Kind::AiTakeover;takeover.player=1;
                    notice_.clear();
                    replayInTransit_=true;noteEvent(takeover);replayInTransit_=false;
                    check(playerAi_[1] && notice_.empty(),"a jump keeps the state and posts no notice");
                    playerAi_[1]=false;
                }
                // A tree the rewind brought back sways again; a burnt stage does not.
                {
                    const auto f=std::find_if(features_.begin(),features_.end(),[](const FeatureInst& i) { return i.simType>=0; });
                    check(f!=features_.end(),"fixture: a feature the sim knows");
                    const int type=f->simType;
                    f->treeType=type;f->simType=type+1;f->tree=false;
                    lastFeatGen_=UINT32_MAX;syncBurningFeatures();
                    check(f->simType==type && f->tree,"a tree back to its living stage sways");
                    f->treeType=-1;f->simType=type+1;f->tree=true;
                    lastFeatGen_=UINT32_MAX;syncBurningFeatures();
                    check(f->simType==type && !f->tree,"any other stage does not");
                }
                // Held keys: no pause flicker, and a jump key waits for the landing.
                {
                    const auto key=[&](SDL_Keycode sym,bool repeat) {
                        SDL_Event e{};e.type=SDL_KEYDOWN;e.key.keysym.sym=sym;e.key.repeat=repeat ? 1 : 0;input(e,W,H);
                    };
                    const bool wasPaused=paused_;
                    key(SDLK_SPACE,true);
                    check(paused_==wasPaused,"a held Space toggles once");
                    replayJumpTo(1000);
                    key(SDLK_RIGHTBRACKET,true);key(SDLK_LEFTBRACKET,true);
                    check(replaySeekTo_==size_t(1000),"a held jump key waits for the landing");
                    key(SDLK_RIGHTBRACKET,false);
                    check(replaySeekTo_==size_t(1000+rb::kStepTicks),"a fresh press stacks");
                    land();
                    replayHoldNudge(+1);
                    check(replaySeekTo_==size_t(1000+2*rb::kStepTicks),"after the landing a hold steps again");
                    replayHoldNudge(+1);
                    check(replaySeekTo_==size_t(1000+2*rb::kStepTicks),"and then waits again");
                    land();
                    // Pausing stops a jump where it stands.
                    paused_=false;
                    replayJumpTo(200);
                    replayJumpTo(2500);
                    const size_t stoppedAt=replayTick_;
                    key(SDLK_SPACE,false);
                    check(paused_ && replaySeekTo_==stoppedAt,"pausing stops a running jump");
                    land();
                    check(replayTick_==stoppedAt,"it lands where it stood");
                    check(world_.stateHash()==straight.at(stoppedAt),"on the recorded state");
                }
                // A marker let go under a menu, or where nothing could see it, comes off the pointer.
                {
                    selection_.clear();frame();
                    mouse(SDL_MOUSEBUTTONDOWN,xFor(600),y,SDL_BUTTON_LEFT);
                    check(replayDragging_,"fixture: the marker is in the hand");
                    SDL_Event esc{};esc.type=SDL_KEYDOWN;esc.key.keysym.sym=SDLK_ESCAPE;input(esc,W,H);
                    check(exitMenu_,"fixture: Esc opens the menu during the drag");
                    mouse(SDL_MOUSEBUTTONUP,xFor(600),y,SDL_BUTTON_LEFT);
                    check(!replayDragging_,"a release under the menu ends the drag");
                    check(exitMenu_,"and fires no menu button");
                    exitMenu_=false;
                    land();
                    mouse(SDL_MOUSEBUTTONDOWN,xFor(700),y,SDL_BUTTON_LEFT);
                    SDL_Event lost{};lost.type=SDL_MOUSEMOTION;lost.motion.x=int(xFor(800));lost.motion.y=int(y);lost.motion.state=0;
                    inputFromView(lost,W,H);
                    check(!replayDragging_ && !replaySeekTo_,"motion with the button up drops the marker, with no jump");
                    mouse(SDL_MOUSEBUTTONUP,xFor(800),y,SDL_BUTTON_LEFT);
                    check(!replaySeekTo_,"and the release that follows does nothing");
                }
                replayJumpTo(900);land();
                selection_.clear();
                // Forward from there over a keyframe, then << snapping to one.
                replayJumpTo(1000);land();
                check(replayTick_==1000 && world_.stateHash()==straight.at(1000),"a jump forward lands on the recorded state");
                const auto hit=[&](const rb::Rect& r) { clickAt(r.x+r.w/2,r.y+r.h/2); };
                hit(l.forward);land();
                check(replayTick_==1000+rb::kStepTicks,">> jumps 30 s");
                check(replayHeld_==0,"releasing >> stops the repeat");
                hit(l.back);land();
                check(replayTick_==900,"<< snaps to the keyframe near its target");
                hit(l.play);
                check(!paused_,"the play button toggles");
                hit(l.play);
                check(paused_,"and back");
                const int speed=replaySpeed_;
                hit(l.faster);
                check(replaySpeed_==speed+1 && speedMult()==rb::speedAt(replaySpeed_),"+ steps the speed");
                hit(l.slower);
                check(replaySpeed_==speed,"- steps it back");
                for (int i=0;i<20;++i) handleKey(SDLK_EQUALS,0);
                check(speedMult()==256,"the top speed is 256x");
                replaySpeed_=speed;
                handleKey(SDLK_SPACE,0);
                check(!paused_,"Space plays");
                handleKey(SDLK_SPACE,0);
                handleKey(SDLK_RIGHTBRACKET,KMOD_SHIFT);land();
                check(replayTick_==900+rb::kLongStepTicks || replayTick_==3000,"Shift+] jumps 5 minutes, or to the end");
                handleKey(SDLK_LEFTBRACKET,0);land();
                check(replayTick_<3000,"[ jumps back");
                // A press between the controls goes no further than the bar.
                selection_.clear();frame();
                const auto gapX=l.speed.x+l.speed.w+l.faster.w+40;
                clickAt(gapX,l.speed.y+2);
                check(selection_.empty() && !dragging_,"the bar swallows a press on its own background");
                // The budget is real bytes: squeezed, the keyframes thin to fit it.
                check(replayKeys_.size()==4 && replayKeys_.back().tick==2700 && replayTick_<2700,"fixture: one keyframe to lay again");
                replayKeyBytes_-=replayKeys_.back().bytes;
                replayKeys_.pop_back();
                replayKeyBudget_=replayKeyLast_*3+replayKeyLast_/2;
                replayJumpTo(3000);land();
                check(replayKeyBytes_<=replayKeyBudget_ && replayKeys_.size()==3,"keyframes stay inside the budget");
                // Room for one beside tick 0: the one kept still follows the playhead. (Capture used
                // to end for good here, and a rewind then ran from the last one it had laid.)
                replayBundles_.resize(4000);
                replayKeyBudget_=replayKeys_.front().bytes+replayKeyLast_+replayKeyLast_/2;
                replayJumpTo(3700);land();
                check(replayKeys_.size()==2 && replayKeys_.back().tick==3600 && replayKeyBytes_<=replayKeyBudget_,
                      "with room for one keyframe beside tick 0, capture goes on");
                // Room for tick 0 alone: nothing is laid, nothing is given up, and a rewind still lands.
                replayBundles_.resize(6000);
                replayKeyBudget_=replayKeys_.front().bytes+replayKeyLast_/2;
                replayJumpTo(4600);land();
                check(replayKeys_.size()==2 && replayKeys_.back().tick==3600,"with no room, the keyframes held stay");
                replayKeyBudget_=uint64_t(1)<<30;
                replayJumpTo(5500);land();
                check(replayKeys_.back().tick==5400,"and capture picks up again when there is room");
                // Too little room for a keyframe, for as long as it lasts: none is copied to find
                // out. (One was, every twentieth capture, on top of a full budget.)
                replayBundles_.resize(26000);
                replayKeyBudget_=replayKeys_.front().bytes+replayKeyLast_/2;
                const uint64_t asked=uint64_t(1)<<40;   // no copy is this size: taking one would change it
                replayKeyLast_=asked;
                replayJumpTo(5500+21*rb::kKeyframeTicks);
                for (int i=0;i<10 && replaySeekTo_;++i) land();
                check(!replaySeekTo_ && replayKeyLast_==asked && replayKeys_.size()==3 && replayKeys_.back().tick==5400,
                      "with no room for a keyframe, none is copied");
                // A jump back brings the size to ask for: that of the keyframe it restored.
                replayKeyBudget_=uint64_t(1)<<30;
                replayJumpTo(3650);land();
                check(replayTick_==3650,"a rewind lands from the keyframe behind it");
                check(replayKeyLast_==replayKeys_[1].bytes,"a jump asks for room for a keyframe like the one it restored");
                replayJumpTo(4600);land();
                check(replayKeys_.size()==4 && replayKeys_[2].tick==4500,"and capture goes on from there");
                sounded=0;
                sounds_.setEffectsMuted(false);
                replayBundles_.clear();
            }
            // The rule is one predicate: as a seated player the same fixture acknowledges.
            spectating_=false;replayMode_=false;
            const int seated=spawn("armpw",cx-300,cz-200,0,0);   // the replay's fighting may have left nobody
            selection_={seated};
            frame();
            {
                const auto buttons=taButtons();
                check(!buttons.empty(),"a seated player's order buttons are on show");
                // One TA's sidebar acts on as an order (taHudEvent): MOVE.
                const auto move=std::find_if(buttons.begin(),buttons.end(),[](const auto& b) {
                    return b.first.size()>=4 && b.first.compare(b.first.size()-4,4,"MOVE")==0;});
                check(move!=buttons.end(),"fixture: the sidebar has a MOVE button");
                clickAt(move->second.x,move->second.y);
                check(pendingCmd_=='m',"a seated player's MOVE button arms: the button check can fail");
                pendingCmd_=0;
            }
            // TA's sidebar presses play the side's click tone (tone<side>), which retail
            // TA data does not ship; a selection is acknowledged by the unit's sound
            // class (sound.tdf select), so that is what proves the sound check can fail.
            selection_.clear();frame();
            clickUnit(seated);
            check(sounded>0,"a seated player's selection is heard: the sound check can fail");
            pendingCmd_=0;
            press(ta::Act::Move);
            check(pendingCmd_=='m',"a seated player's order key still arms");
            pendingCmd_=0;
            sounds_.requestHook=nullptr;
            std::fprintf(stderr,"PASS: watch only (%s)\n",mode);
            return;
        }
        if (ta::devFlag("TA_AIR_OVER_FX_TEST")) {
            namespace hu = ta::hiddenunits;
            // Retail's world pass (0x4fc648) draws the airborne units after the
            // shots (52b210) and the effect manager (492070: debris, explosions):
            // a ground fight's explosion never paints over a flyer passing above.
            // Through the real draw(): the replay order, and the flyer's own
            // pixels with and without an explosion glow centred on it.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("air over fx: "+what);
                std::fprintf(stderr,"PASS: air over fx: %s\n",what.c_str());
            };
            constexpr int W=1024,H=768;
            noFog_=true;edgeScrollOn_=false;
            // A flyer's ground shadow is not its body: keep it out of the pixels.
            const bool shadows=!settings_ || settings_->unitShadows;
            if (settings_) settings_->unitShadows=false;
            const float x0=float(mapView_.map().blocksX)*16.0f,z0=float(mapView_.map().blocksY)*16.0f;
            // A Vamp: its fuselage is solid at its body centre (a Thunder's or an
            // Atlas's centre is a gap between parts). Zoom 2 puts the 7x7 probe well
            // inside it. TA_AIR_OVER_FX_TYPE=<unit> tries another flyer.
            const char* flyerType=ta::devEnv("TA_AIR_OVER_FX_TYPE");
            const int flyer=spawn(flyerType ? flyerType : "corvamp",x0,z0,0,0);
            constexpr float viewZoom=2.0f;
            check(flyer>=0,"fixture spawned");
            writableWorld().order(flyer,x0+3000,z0,false);
            float offX=0,offY=0;
            std::vector<uint8_t> px;
            int ow=0,oh=0;
            const auto frame=[&](bool follow,const char* capture) {
                captureFrame();beginFrame();cosmeticStep(0);
                mapView_.setZoom(viewZoom);
                if (const auto* r=frameUnitP(flyer); r && follow) {
                    offX=r->x-float(W-300)*0.5f/viewZoom;offY=r->z-float(H)*0.5f/viewZoom;
                }
                mapView_.setOffset(offX,offY);
                prepare(W,H);draw(W,H);
                SDL_GetRendererOutputSize(ren_,&ow,&oh);
                px.assign(size_t(ow)*size_t(oh)*4,0);
                readViewPixels(px.data(),ow*4);
                if (capture) ta::png::write(capture,ow,oh,px);
                endFrame();
            };
            // Up and well clear of the ground.
            float altitude=0;
            for (int t=0;t<600 && altitude<40;++t) {
                writableWorld().tick(1.f/30);
                frame(true,nullptr);
                if (const auto a=anims_.find(flyer);a!=anims_.end()) altitude=a->second.altitude;
            }
            check(altitude>=40,"the flyer is airborne");
            const char* capture=ta::devEnv("TA_AIR_OVER_FX_CAPTURE");
            const std::string base=capture ? capture : "";
            explosionGlows_.clear();
            frame(true,capture ? (base+"-plain.png").c_str() : nullptr);
            const std::vector<uint8_t> plain=px;
            const auto* r=frameUnitP(flyer);
            check(r!=nullptr,"the flyer is in the frame");
            const SDL_FPoint p=unitScreen(*r);
            check(p.x>32 && p.y>32 && p.x<float(W-332) && p.y<float(H-32),"the flyer is on screen");
            // A large explosion glow centred on the flyer's body (unitScreen) and
            // a control glow over bare ground 100 px to its left.
            const float zm=mapView_.zoom();
            const float alt=r->z-mapView_.offY()-terrainLift(r->x,r->z)-p.y/zm;
            explosionGlows_.push_back({r->x,r->z,alt,front().gameTick,2,std::nullopt});
            explosionGlows_.push_back({r->x-100,r->z,alt,front().gameTick,2,std::nullopt});
            frame(false,capture ? (base+"-glow.png").c_str() : nullptr);
            check(debugGlowDrawCount_==2,"both glows drew");
            check(debugPaintOrder_=="gea" && debugAirOps_>0,
                  "ground bodies, then the late effects, then the airborne bodies ("+debugPaintOrder_+")");
            // The ground fight's glows are ground-layer painter items now (PaintItem::fx):
            // drawn in the ground replay, still before every airborne body.
            check(debugFxReplayed_==2 && debugFxLate_==0,"both glows drew in the ground replay");
            const auto changed=[&](float cx,float cy,int half) {
                int n=0;
                for (int y=int(cy)-half;y<=int(cy)+half;++y)
                    for (int x=int(cx)-half;x<=int(cx)+half;++x) {
                        const size_t i=(size_t(y)*size_t(ow)+size_t(x))*4;
                        n+=std::memcmp(&plain[i],&px[i],3)!=0;
                    }
                return n;
            };
            const int control=changed(p.x-100*zm,p.y,3),body=changed(p.x,p.y,3);
            check(control>=40,"the control glow changes the bare ground ("+std::to_string(control)+" of 49 px)");
            check(body==0,"the glow under the flyer leaves its body untouched ("+std::to_string(body)+" of 49 px)");
            explosionGlows_.clear();
            // (Kingdoms' nimbus -- a spell aura drawn with its flyer in the air replay --
            // has no TA counterpart.)
            if (settings_) settings_->unitShadows=shadows;
            std::fprintf(stderr,"PASS: air over fx (altitude %.0f)\n",altitude);
            return;
        }
        if (ta::devFlag("TA_SHOT_FX_TEST")) {
            // TotalA.exe's sprite and model shots (0x49be60 rendertypes 1, 3,
            // 4, 5; client/tashotdraw.h): each case's shot in flight, drawn
            // through the real draw() with and without the TA shots; the art
            // must change pixels round the head. TA_SHOT_FX_CAPTURE=<prefix>
            // writes <prefix>-<shooter>.png.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("shot fx: "+what);
                std::fprintf(stderr,"PASS: shot fx: %s\n",what.c_str());
            };
            constexpr int W=1024,H=768;
            noFog_=true;edgeScrollOn_=false;healthBars_=0;
            const float x0=float(mapView_.map().blocksX)*16.0f,z0=float(mapView_.map().blocksY)*16.0f;
            struct Case { const char* shooter; int renderType; bool dgun; };
            const Case cases[]={{"armpw",4,false},{"armstump",4,false},{"corpyro",5,false},
                                {"armsam",1,false},{"armcom",3,true}};
            const char* capture=ta::devEnv("TA_SHOT_FX_CAPTURE");
            int row=0;
            for (const Case& c:cases) {
                const float z=z0+float(row++-2)*192.0f;
                const int shooter=spawn(c.shooter,x0,z,0,0);
                const int foe=spawn("corestor",x0+130,z,0,1);
                check(shooter>=0 && foe>=0,std::string(c.shooter)+" fixture spawned");
                if (c.dgun) writableWorld().dgun(shooter,foe,false);
                else writableWorld().attack(shooter,foe,false);
                std::optional<ta::sim::Projectile> shot;
                for (int t=0;t<600 && !shot;++t) {
                    writableWorld().tick(1.f/30);
                    for (const auto& p:world_.projectiles())
                        if (p.fromId==shooter && p.wsrc && p.wsrc->renderType==c.renderType && p.life>0 && p.age>=3)
                            {shot=p;break;}
                }
                check(shot.has_value(),std::string(c.shooter)+"'s rendertype "+std::to_string(c.renderType)+" shot is in flight");
                const auto head=ta::taShotScreenPoint(shot->position);
                std::vector<uint8_t> with,without;
                int ow=0,oh=0;
                for (const bool skip:{false,true}) {
                    debugSkipTaShots_=skip;debugTaShotDraws_={};
                    captureFrame();beginFrame();cosmeticStep(0);
                    mapView_.setZoom(1.0f);
                    mapView_.setOffset(std::floor(float(head[0])-float(W-300)*0.5f),std::floor(float(head[1])-float(H)*0.5f));
                    prepare(W,H);draw(W,H);
                    SDL_GetRendererOutputSize(ren_,&ow,&oh);
                    auto& px=skip ? without : with;
                    px.assign(size_t(ow)*size_t(oh)*4,0);
                    readViewPixels(px.data(),ow*4);
                    if (capture && !skip) ta::png::write((std::string(capture)+"-"+c.shooter+".png").c_str(),ow,oh,px);
                    endFrame();
                    if (!skip) check(debugTaShotDraws_[size_t(c.renderType)]>0,
                                     std::string(c.shooter)+": drawTaShot drew rendertype "+std::to_string(c.renderType)+" art");
                }
                debugSkipTaShots_=false;
                const int hx=int(float(head[0])-mapView_.offX()),hy=int(float(head[1])-mapView_.offY());
                int changed=0;
                for (int y=std::max(hy-12,0);y<=std::min(hy+12,oh-1);++y)
                    for (int x=std::max(hx-12,0);x<=std::min(hx+12,ow-1);++x) {
                        const size_t i=(size_t(y)*size_t(ow)+size_t(x))*4;
                        changed+=std::memcmp(&with[i],&without[i],3)!=0;
                    }
                check(changed>0,std::string(c.shooter)+": the shot changes "+std::to_string(changed)+" px round its head");
            }
            std::fprintf(stderr,"PASS: shot fx (%d cases)\n",row);
            return;
        }
        if (ta::devFlag("TA_BEAM_FX_TEST")) {
            // TotalA.exe's beam draw (0x49bf3a..0x49c097, client/tabeamdraw.h):
            // an Arm LLT's laser in flight is a line from its head to its tail
            // in palette.pal[map[232]] (211,43,0), with a line in map[234]
            // (155,131,47) a row above it. Through the real draw(), at zoom 1
            // and 3. TA_BEAM_FX_CAPTURE=<prefix> writes <prefix>-z1/-z3.png.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("beam fx: "+what);
                std::fprintf(stderr,"PASS: beam fx: %s\n",what.c_str());
            };
            constexpr int W=1024,H=768;
            noFog_=true;edgeScrollOn_=false;healthBars_=0;
            const float x0=float(mapView_.map().blocksX)*16.0f,z0=float(mapView_.map().blocksY)*16.0f;
            const int llt=spawn("armllt",x0,z0,0,0);
            const int foe=spawn("corestor",x0+240,z0,0,1);
            check(llt>=0 && foe>=0,"fixture spawned");
            writableWorld().attack(llt,foe,false);
            const ta::sim::Projectile* beam=nullptr;
            ta::sim::Projectile shot;
            for (int t=0;t<300 && !beam;++t) {
                writableWorld().tick(1.f/30);
                for (const auto& p:world_.projectiles())
                    if (p.fromId==llt && p.taLineOfSight && p.taTailMoving && p.life>0 &&
                        p.position[0]-p.taTail[0]>(40<<16)) {shot=p;beam=&shot;}
            }
            check(beam!=nullptr,"the LLT's laser is in flight with its tail moving");
            const char* capture=ta::devEnv("TA_BEAM_FX_CAPTURE");
            std::vector<uint8_t> px;
            int ow=0,oh=0;
            for (const float zoom:{1.0f,3.0f}) {
                captureFrame();beginFrame();cosmeticStep(0);
                mapView_.setZoom(zoom);
                const auto head=ta::taShotScreenPoint(shot.position),tail=ta::taShotScreenPoint(shot.taTail);
                const float cx=float(head[0]+tail[0])*0.5f,cy=float(head[1]+tail[1])*0.5f;
                mapView_.setOffset(std::floor(cx-float(W-300)*0.5f/zoom),std::floor(cy-float(H)*0.5f/zoom));
                prepare(W,H);draw(W,H);
                SDL_GetRendererOutputSize(ren_,&ow,&oh);
                px.assign(size_t(ow)*size_t(oh)*4,0);
                readViewPixels(px.data(),ow*4);
                if (capture) ta::png::write((std::string(capture)+(zoom>1 ? "-z3.png" : "-z1.png")).c_str(),ow,oh,px);
                endFrame();
                // The middle pixel of each line, at the centre of its zoomed cell.
                const auto& w=*shot.wsrc;
                const auto lines=ta::taBeamLines(shot.position,shot.taTail,taGuiColourMap_[w.color],w.color2,
                                                 taGuiColourMap_[w.color2]);
                check(lines.size()==2,"a laser with a color2 draws two lines");
                for (const auto& line:lines) {
                    const int mx=(line.x1+line.x2)/2;
                    const float t=line.x2==line.x1 ? 0.f : float(mx-line.x1)/float(line.x2-line.x1);
                    const int my=int(std::lround(float(line.y1)+t*float(line.y2-line.y1)));
                    const int sx=int((float(mx)-mapView_.offX()+0.5f)*zoom),sy=int((float(my)-mapView_.offY()+0.5f)*zoom);
                    const uint32_t want=nanoGamePalette_[line.colour];
                    int found=0;
                    for (int dy=-1;dy<=1 && !found;++dy) {
                        const size_t i=(size_t(sy+dy)*size_t(ow)+size_t(sx))*4;
                        if (px[i]==Uint8(want>>24) && px[i+1]==Uint8(want>>16) && px[i+2]==Uint8(want>>8)) found=1;
                    }
                    char what[160];
                    std::snprintf(what,sizeof what,"zoom %.0f: palette %d (%u,%u,%u) at the line's middle (%d,%d)",
                                  zoom,line.colour,unsigned(want>>24),unsigned((want>>16)&255),unsigned((want>>8)&255),sx,sy);
                    check(found,what);
                }
            }
            std::fprintf(stderr,"PASS: beam fx (head %d,%d tail %d,%d)\n",
                         ta::taShotScreenPoint(shot.position)[0],ta::taShotScreenPoint(shot.position)[1],
                         ta::taShotScreenPoint(shot.taTail)[0],ta::taShotScreenPoint(shot.taTail)[1]);
            return;
        }
        if (ta::devFlag("TA_DEPTH_FX_TEST")) {
            // Shots and effects in depth order (PaintItem::fx; the owner's request,
            // not retail): an explosion glow and a Hunter's spear BEHIND a tree are
            // covered by its crown and the same IN FRONT of it paint over it, through
            // the real draw(): the painter ops around the tree's op, the crown's own
            // pixels, and the late pass drawing none of them. A shot or effect is
            // never a HIDDEN UNITS cover or covered (FADE leaves the tree alone).
            // TA_DEPTH_FX_CAPTURE=<prefix> writes <prefix>-plain/-behind/-front.png.
            namespace hu = ta::hiddenunits;
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("depth fx: "+what);
                std::fprintf(stderr,"PASS: depth fx: %s\n",what.c_str());
            };
            constexpr int W=1280,H=960;
            noFog_=true;edgeScrollOn_=false;tacticalDotsOpt_=false;healthBars_=0;fogStill_=true;
            hiddenUnitsMode_=hu::kOff;
            const bool swayWas=settings_ && settings_->treeSway;
            if (settings_) settings_->treeSway=false;   // a still crown: exact cover points
            loadFeatureDefs();
            const int cx0=mapView_.map().blocksX/2,cz0=mapView_.map().blocksY/2;
            for (auto& f:features_)   // a clear stage
                if (std::fabs(f.x-float(cx0*16))<700 && std::fabs(f.z-float(cz0*16))<600) f.tex=nullptr;
            check(addFeature("tree1",cx0,cz0),"placed a tree");
            const size_t tree=features_.size()-1;
            check(!features_[tree].low,"the tree is tall scenery (sorts by depth)");
            const float x0=features_[tree].x,z0=features_[tree].z;
            std::vector<uint8_t> px;
            int ow=0,oh=0;
            const auto frame=[&](const std::string& capture,float zoom=1.0f) {
                captureFrame();beginFrame();cosmeticStep(0);
                mapView_.setZoom(zoom);
                mapView_.setOffset(x0-float(W-300)*0.5f/zoom,z0-float(H)*0.5f/zoom);
                prepare(W,H);mapView_.finishChunks();prepare(W,H);draw(W,H);   // the whole terrain: frames compare pixels
                SDL_GetRendererOutputSize(ren_,&ow,&oh);
                px.assign(size_t(ow)*size_t(oh)*4,0);
                readViewPixels(px.data(),ow*4);
                if (!capture.empty()) ta::png::write(capture.c_str(),ow,oh,px);
                endFrame();
            };
            const char* capture=ta::devEnv("TA_DEPTH_FX_CAPTURE");
            const auto shot=[&](const char* name) {return capture ? std::string(capture)+"-"+name+".png" : std::string();};
            explosionGlows_.clear();beams_.clear();effects_.clear();particles_.clear();rings_.clear();
            // Frames are compared pixel for pixel: draw until the terrain chunks under
            // the view have all baked and two frames in a row are the same.
            {
                std::vector<uint8_t> last;
                int same=0;
                for (int i=0;i<400 && same<3;++i) {
                    frame("");
                    same=px==last && mapView_.chunkStats().pending==0 ? same+1 : 0;
                    last=px;
                    SDL_Delay(5);
                }
                check(same>=3,"the terrain under the view settles");
            }
            frame(shot("plain"));
            const std::vector<uint8_t> plain=px;
            // The crown's middle, on screen: the opaque run up the anchor column.
            const FeatureSprite sp=featureSprite(features_[tree],mapView_.zoom());
            check(sp.tex && sp.mask && sp.mask->width()>0,"tree sprite with an opacity mask");
            const auto& mask=*sp.mask;
            const int col=std::clamp(features_[tree].xoff,0,mask.width()-1);
            int top=-1,bottom=-1;
            for (int y=0;y<mask.height();++y) if (mask.opaque(col,y)) { if (top<0) top=y; bottom=y; }
            check(top>=0,"the crown is opaque in the anchor column");
            const float cx=sp.dst.x+(float(col)+0.5f)*sp.dst.w/float(mask.width());
            const float cy=sp.dst.y+(float(top+bottom)*0.5f+0.5f)*sp.dst.h/float(mask.height());
            bool solid=true;
            for (int dy=-3;dy<=3;++dy) for (int dx=-3;dx<=3;++dx)
                solid=solid && hu::spriteCovers(mask,sp.dst.x,sp.dst.y,sp.dst.w,sp.dst.h,sp.shear,cx+float(dx),cy+float(dy));
            check(solid,"a 7x7 crown window is opaque");
            const float zm=mapView_.zoom();
            // Ground points 48 world px north of the trunk (behind) and 32 south (in
            // front), each lifted on screen onto the crown's middle.
            const float zBehind=z0-48.0f,zFront=z0+32.0f;
            const float offY1=mapView_.offY();   // the zoom-1 view the crown point is in
            const auto altFor=[&](float z) {return z-offY1-terrainLift(x0,z)-cy/zm;};
            const auto glowAt=[&](float z) {
                explosionGlows_.push_back({x0,z,altFor(z),front().gameTick,2,std::nullopt});
            };
            const auto spearAt=[&](float z) {
                BeamFx b;b.x1=b.x2=x0+6;b.z1=b.z2=z;b.alt1=b.alt2=altFor(z)-8;
                b.model="missile";b.life=1000;b.age=0;
                beams_.push_back(b);
            };
            const auto changed=[&] {
                int n=0;
                for (int y=int(cy)-3;y<=int(cy)+3;++y) for (int x=int(cx)-3;x<=int(cx)+3;++x) {
                    const size_t i=(size_t(y)*size_t(ow)+size_t(x))*4;
                    n+=std::memcmp(&plain[i],&px[i],3)!=0;
                }
                return n;
            };
            if (capture) {
                // The picture (not checked): a spear and an explosion behind the tree
                // on its left, the same in front of it on its right.
                loadExplosionClasses();
                std::string anim;int tallest=0;
                for (const auto& [cls,names]:explosionClasses_)
                    for (const auto& name:names)
                        if (const auto* a=effectFor(name); a && a->frames.size()>4) {
                            const int h=a->frames[a->frames.size()/3].h;
                            if (h>tallest && h<160) {tallest=h;anim=name;}
                        }
                for (const auto& [dx,z,up]:{std::tuple{-16.0f,zBehind,-14.0f},std::tuple{16.0f,zFront,14.0f}}) {
                    spawnEffectAnim(anim,x0+dx,z,0,0,1,altFor(z)+up+6);
                    effects_.back().age=effLoopLen(effects_.back())*0.3f;
                }
                frame(shot("scene-explosions"),2.0f);
                effects_.clear();
                for (const auto& [z,up]:{std::pair{zBehind,-10.0f},std::pair{zFront,10.0f}}) {
                    BeamFx b;b.x1=x0-60;b.x2=x0+60;b.z1=b.z2=z;   // flying east, head over the trunk
                    b.alt1=b.alt2=altFor(z)+up-8;
                    b.model="missile";b.life=1000;b.age=500;
                    beams_.push_back(b);
                }
                frame(shot("scene-spears"),2.0f);
                effects_.clear();beams_.clear();
            }
            // ---- order: both pairs in one frame ----
            glowAt(zBehind);glowAt(zFront);spearAt(zBehind);spearAt(zFront);
            frame("");
            {
                const auto opOf=[&](const void* p) {
                    for (size_t i=0;i<drawOps_.size();++i) {
                        const DrawOp& op=drawOps_[i];
                        for (uint32_t k=0;k<op.fxCount;++k)
                            if (fxItems_[fxOrder_[size_t(op.fxFirst)+k]].p==p) return i;
                    }
                    return SIZE_MAX;
                };
                size_t treeOp=SIZE_MAX;
                for (size_t i=0;i<drawOps_.size();++i) if (drawOps_[i].f==&features_[tree]) treeOp=i;
                const size_t gB=opOf(&explosionGlows_[0]),gF=opOf(&explosionGlows_[1]);
                const size_t sB=opOf(&beams_[0]),sF=opOf(&beams_[1]);
                check(treeOp!=SIZE_MAX && gB!=SIZE_MAX && gF!=SIZE_MAX && sB!=SIZE_MAX && sF!=SIZE_MAX,
                      "the tree, both glows and both spears are painter ops");
                check(gB<treeOp && sB<treeOp,"the glow and the spear behind the tree draw before it");
                check(gF>treeOp && sF>treeOp,"the glow and the spear in front of it draw after it");
                check(debugFxLate_==0 && debugFxReplayed_>=4,
                      "the late pass draws none of them ("+std::to_string(debugFxReplayed_)+" replayed)");
                check(debugPaintOrder_=="gea","ground replay, late fx pass, air replay ("+debugPaintOrder_+")");
            }
            // ---- pixels: behind is covered, in front is not ----
            explosionGlows_.clear();beams_.clear();
            glowAt(zBehind);spearAt(zBehind);
            frame(shot("behind"));
            check(debugGlowDrawCount_==1,"the glow behind the tree drew");
            const int behind=changed();
            check(behind==0,"the crown covers the glow and spear behind it ("+std::to_string(behind)+" of 49 px)");
            const std::vector<uint8_t> behindOff=px;
            {   // FADE: an effect behind a tree is no unit to fade it for.
                hiddenUnitsMode_=hu::kFade;
                frame("");
                bool fxCover=false;
                for (const auto& rec:hiddenRecs_) fxCover=fxCover || paintItems_[rec.item].fx>=0;
                check(!fxCover,"FADE: no shot or effect is a cover or covered");
                check(px==behindOff,"FADE: the tree in front of an effect does not fade");
                hiddenUnitsMode_=hu::kOff;
            }
            explosionGlows_.clear();beams_.clear();
            glowAt(zFront);spearAt(zFront);
            frame(shot("front"));
            const int infront=changed();
            check(infront>=40,"the glow in front paints over the crown ("+std::to_string(infront)+" of 49 px)");
            // ---- far outside the view: not queued, still drawn by the late pass ----
            explosionGlows_.clear();beams_.clear();
            explosionGlows_.push_back({x0+6000,z0,0,front().gameTick,2,std::nullopt});
            frame("");
            check(debugFxLate_==1 && debugFxReplayed_==0 && !fxItems_.empty() && !fxItems_[0].queued,
                  "an effect far outside the view goes to the late pass, not the painter list");
            explosionGlows_.clear();
            if (settings_) settings_->treeSway=swayWas;
            std::fprintf(stderr,"PASS: depth fx (behind %d, front %d px)\n",behind,infront);
            return;
        }
        if (ta::devFlag("TA_TACTICAL_DOTS_TEST")) {
            // Tactical Dots through the real draw(): dots replace models past the
            // threshold, the hysteresis band holds either state, fog-hidden enemies
            // get no dot, and click/box selection land on the dots.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("tactical dots: "+what);
                std::fprintf(stderr,"PASS: tactical dots: %s\n",what.c_str());
            };
            constexpr int W=1280,H=960;
            noFog_=false;edgeScrollOn_=false;
            writableWorld().setPlayerCount(2);
            const float x0=float(mapView_.map().blocksX)*16.0f,z0=float(mapView_.map().blocksY)*16.0f;
            const ta::sim::UnitType* flyerType=nullptr;
            for (const auto& [name,type]:registry_.types())
                if (type.canFly && !type.isStructure() && !type.commander) {flyerType=&type;break;}
            const int infantry=spawn("armpw",x0,z0,0,0);
            const int keep=spawn("armllt",x0+160,z0+40,0,0);
            const int flyer=flyerType ? spawn(flyerType->id,x0-120,z0,0,0) : -1;
            const int foeSeen=spawn("armpw",x0+60,z0-80,0,1);
            const int foeHidden=spawn("armpw",x0+900,z0+300,0,1);
            check(infantry>=0 && keep>=0 && flyer>=0 && foeSeen>=0 && foeHidden>=0,"fixture spawned");
            check(!world_.allied(0,1),"fixture enemy is not allied");
            selection_={keep};
            // Local fog for the fixture: everything explored, only a 400px disc
            // around the fixture centre currently in sight -- the foe 950px away
            // is in fog, exactly what the display plane reports in a real game.
            debugFogDisc(x0,z0,400.0f);
            const auto frame=[&](float zoom,const char* capture=nullptr) {
                captureFrame();beginFrame();cosmeticStep(0);
                mapView_.setZoom(zoom);
                // Keep the hidden foe on screen too, so "not drawn" means the fog
                // rule and not the viewport cull.
                mapView_.setOffset(x0+300-float(W-300)*0.5f/zoom,z0+100-float(H)*0.5f/zoom);
                prepare(W,H);draw(W,H);
                if (capture) {   // TA_TACTICAL_DOTS_CAPTURE: the frame as drawn
                    int ow=0,oh=0;SDL_GetRendererOutputSize(ren_,&ow,&oh);
                    std::vector<uint8_t> px(size_t(ow)*size_t(oh)*4);
                    if (readViewPixels(px.data(),ow*4)==0)
                        ta::png::write(capture,ow,oh,px);
                }
                endFrame();
            };
            const auto has=[&](int id) {
                return std::find(debugDotIds_.begin(),debugDotIds_.end(),id)!=debugDotIds_.end();
            };
            namespace td=ta::tacticaldots;
            // Slider at 50%: halfway (in wheel notches) from this window's floor to 1.0,
            // leaving room both below and above the threshold.
            tacticalDotsZoom_=50;
            const float floorZ=mapView_.minZoom(mapViewW(W),mapViewH(H));
            const float zT=td::thresholdZoom(50,floorZ)*td::kSlack;
            const float zIn=zT*0.95f,zBand=zT*1.08f,zOut=zT*td::kBuffer*1.05f;
            check(zIn>=floorZ,"fixture has room below the threshold");
            tacticalDotsOpt_=false;frame(zIn);
            check(!dotsFrame_ && !visUnits_.empty(),"option off draws models even far out");
            tacticalDotsOpt_=true;frame(zOut);
            check(!dotsFrame_ && !visUnits_.empty(),"above the threshold models still draw");
            frame(zBand);
            check(!dotsFrame_ && !visUnits_.empty(),"entering the hysteresis band from above keeps models");
            frame(zIn);
            check(dotsFrame_ && visUnits_.empty() && debugDotCount_>0,"below the threshold dots replace every model");
            check(has(infantry) && has(keep) && has(flyer) && has(foeSeen),"own units, building, flyer and seen enemy get dots");
            check(!has(foeHidden),"enemy in fog gets no dot");
            {   // Dots are the main view: a unit radar puts on the minimap gets none.
                const auto* hidden=frameUnitP(foeHidden);
                check(hidden && !mainViewVisible(*hidden),"dots follow the main view's fog rule");
                const auto c=dotCentre(*hidden);
                check(c.x>0 && c.x<W-300 && c.y>0 && c.y<H,"hidden enemy is inside the view (fog, not cull)");
            }
            frame(zBand);
            check(dotsFrame_ && visUnits_.empty(),"leaving through the hysteresis band keeps dots");
            frame(zOut);
            check(!dotsFrame_ && !visUnits_.empty(),"past the exit zoom models return");
            frame(zIn);
            check(dotsFrame_,"dots come back below the threshold");
            {   // World effects and shots follow the dots: an explosion glow in plain
                // sight draws with models, not with dots, and survives the dots frame
                // (drawing is skipped, the effect itself is not touched).
                explosionGlows_.clear();
                explosionGlows_.push_back({x0,z0,0,front().gameTick,0,std::nullopt});
                frame(zIn);
                check(!worldFxFrame() && debugGlowDrawCount_==0,"dots: no explosion glow drawn");
                check(fxItems_.empty() && std::none_of(paintItems_.begin(),paintItems_.end(),
                      [](const PaintItem& it) {return it.fx>=0;}),"dots: no effect in the painter list");
                check(explosionGlows_.size()==1,"dots: the skipped glow is still alive");
                tacticalDotsOpt_=false;frame(zIn);
                check(worldFxFrame() && debugGlowDrawCount_==1,"models: the same glow draws again");
                tacticalDotsOpt_=true;frame(zIn);
                check(dotsFrame_ && debugGlowDrawCount_==0,"back to dots: glow hidden again");
                explosionGlows_.clear();
            }
            // Picking: a click on the dot selects it, a small box around it too.
            const auto* inf=frameUnitP(infantry);
            const SDL_FPoint c=dotCentre(*inf);
            check(unitUnderCursor(*inf,c.x,c.y),"cursor over a dot hits its unit");
            const auto mouse=[&](uint32_t kind,float x,float y) {
                SDL_Event e{};e.type=kind;
                if (kind==SDL_MOUSEMOTION) {e.motion.x=int(x);e.motion.y=int(y);}
                else {e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x);e.button.y=int(y);}
                inputFromView(e,W,H);   // a point in the world view
            };
            const auto savedMod=SDL_GetModState();SDL_SetModState(KMOD_NONE);
            selection_.clear();
            mouse(SDL_MOUSEBUTTONDOWN,c.x,c.y);mouse(SDL_MOUSEBUTTONUP,c.x,c.y);
            check(selection_==std::vector<int>{infantry},"click on a dot selects the unit");
            selection_.clear();
            mouse(SDL_MOUSEBUTTONDOWN,c.x-12,c.y-12);mouse(SDL_MOUSEMOTION,c.x+12,c.y+12);
            mouse(SDL_MOUSEBUTTONUP,c.x+12,c.y+12);
            check(std::find(selection_.begin(),selection_.end(),infantry)!=selection_.end() &&
                  std::find(selection_.begin(),selection_.end(),foeSeen)==selection_.end(),
                  "box around a dot selects own unit, never an enemy");
            SDL_SetModState(savedMod);
            selection_={infantry,keep,flyer};
            frame(zIn,ta::devEnv("TA_TACTICAL_DOTS_CAPTURE"));
            check(debugDotCount_>0 && dotsFrame_,"selected dots still drawn");
            tacticalDotsOpt_=false;frame(zIn);
            check(!dotsFrame_ && !visUnits_.empty(),"turning the option off restores models at once");
            {   // The slider, as on a 7680x2160 window where the map-fills-window
                // floor (0.75, say) sits far above any fixed zoom threshold.
                const float floor=0.75f,notch=td::kNotch;
                td::Switch s;
                check(!s.update(true,0,floor*notch*notch,floor),"0%: two notches above the floor: models");
                check(s.update(true,0,floor,floor),"0%: fully zoomed out: dots");
                check(s.update(true,0,floor*notch,floor),"0%: one notch in from the floor keeps dots");
                check(!s.update(true,0,floor*notch*notch,floor),"0%: second notch in brings models back");
                check(!s.update(true,0,floor*notch,floor),"0%: one notch above the floor, coming out: still models");
                check(!s.update(false,0,floor,floor),"option off overrides the slider");
                check(std::fabs(td::thresholdZoom(100,0.19f)-1.0f)<1e-4f,"100%: dots from normal size");
                check(std::fabs(td::thresholdZoom(0,0.19f)-0.19f)<1e-4f,"0%: dots only at the floor");
                check(td::thresholdZoom(20,0.19f)>0.19f && td::thresholdZoom(20,0.19f)<td::thresholdZoom(50,0.19f),
                      "threshold grows with the slider");
                check(std::fabs(td::thresholdZoom(70,1.5f)-1.5f)<1e-4f,"floor above normal size: always at the floor");
                td::Switch d;
                check(d.update(true,td::kDefaultPercent,0.19f,0.19f),"default 20%: fully zoomed out shows dots");
                check(!d.update(true,td::kDefaultPercent,0.5f,0.19f),"default 20%: zoom 0.5 at 1080p shows models");
                // Size: building > infantry, a minimum, and compact at a high zoom.
                check(td::dotSide(6,6,0.25f)>td::dotSide(2,2,0.25f),"building dot larger than infantry");
                check(td::dotSide(1,1,0.05f)>=td::kMinDotPx,"dots never shrink below the minimum");
                check(td::dotSide(2,2,1.5f)==td::dotSide(2,2,td::kCompactZoom),"dots stay compact when zoomed in");
            }
            // Leave dots on with a selection so the --shot capture shows both.
            tacticalDotsOpt_=true;selection_={infantry,keep,flyer};mapView_.setZoom(zIn);
            std::fprintf(stderr,"PASS: tactical dots (threshold %.3f/%.3f zoom at 50%%, min zoom here %.3f)\n",
                         zT,zT*td::kBuffer,floorZ);
            return;
        }
        if (ta::devFlag("TA_ZOOM_FLOOR_TEST")) {
            // Full zoom-out through the real wheel handler and frame: the map fills the
            // WORLD VIEW (left of the right panel, above the bottom bar) in one dimension,
            // shows nothing past its edges, and every edge can be scrolled into that view;
            // the frame's own clamp and Tactical Dots use the same floor. Runs at the launch's window
            // size (--winsize), at UI SCALE 100% and 200%, then as a spectator (no bar).
            // TA_ZOOM_FLOOR_CAPTURE=<prefix> writes <prefix>-ui1.png/-ui2.png/-spec.png.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("zoom floor: "+what);
                std::fprintf(stderr,"PASS: zoom floor: %s\n",what.c_str());
            };
            int W=0,H=0;SDL_GetRendererOutputSize(ren_,&W,&H);
            noFog_=true;edgeScrollOn_=false;tacticalDotsOpt_=false;
            const float mapW=float(mapView_.map().blocksX)*32,mapH=float(mapView_.map().blocksY)*32;
            const auto frame=[&](const std::string& capture) {
                captureFrame();beginFrame();cosmeticStep(0);
                prepare(W,H);draw(W,H);
                if (!capture.empty()) {
                    mapView_.finishChunks();prepare(W,H);draw(W,H);
                    int ow=0,oh=0;SDL_GetRendererOutputSize(ren_,&ow,&oh);
                    std::vector<uint8_t> px(size_t(ow)*size_t(oh)*4);
                    if (readViewPixels(px.data(),ow*4)==0)
                        ta::png::write(capture,ow,oh,px);
                }
                endFrame();
            };
            const auto wheelOut=[&] {
                renderStudyMouse(float(mapViewW(W))*0.3f,float(mapViewH(H))*0.4f);
                for (int i=0;i<120;++i) {
                    SDL_Event e{};e.type=SDL_MOUSEWHEEL;e.wheel.y=-1;e.wheel.direction=SDL_MOUSEWHEEL_NORMAL;
                    input(e,W,H);
                }
            };
            const auto pass=[&](const std::string& tag) {
                const std::string at=tag+" "+std::to_string(W)+"x"+std::to_string(H)+": ";
                const float vw=float(mapViewW(W)),vh=float(mapViewH(H));
                mapView_.setZoom(1.0f);mapView_.setOffset(mapW*0.5f,mapH*0.5f);
                frame("");
                wheelOut();
                tacticalDotsOpt_=true;tacticalDotsZoom_=0;
                const char* cap=ta::devEnv("TA_ZOOM_FLOOR_CAPTURE");
                frame(cap ? std::string(cap)+"-"+tag+".png" : "");
                tacticalDotsOpt_=false;
                const float z=mapView_.zoom(),ox=mapView_.offX(),oy=mapView_.offY();
                std::fprintf(stderr,"zoom floor %s%.4f view %.0fx%.0f world %.1fx%.1f of map %.0fx%.0f at %.1f,%.1f\n",
                             at.c_str(),z,vw,vh,vw/z,vh/z,mapW,mapH,ox,oy);
                check(ox>=-0.5f && oy>=-0.5f && ox+vw/z<=mapW+0.5f && oy+vh/z<=mapH+0.5f,
                      at+"fully zoomed out, nothing past the map edges shows");
                check(std::fabs(vw/z-mapW)<1.0f || std::fabs(vh/z-mapH)<1.0f,
                      at+"fully zoomed out, the map spans the whole view in one dimension");
                check(dotsFrame_,at+"Tactical Dots at 0% are on at the reachable floor");
                mapView_.setOffset(1e7f,1e7f);frame("");
                check(std::fabs(mapView_.offX()+vw/z-mapW)<0.5f && std::fabs(mapView_.offY()+vh/z-mapH)<0.5f,
                      at+"the bottom-right corner scrolls into the view (not under the panel or bar)");
                mapView_.setZoom(0.05f);frame("");
                check(std::fabs(mapView_.zoom()-z)<1e-4f,at+"the frame's own clamp holds the wheel's floor");
                mapView_.setZoom(1.0f);mapView_.setOffset(mapW*0.5f,mapH*0.5f);
            };
            uiScale_=1.0f;pass("ui1");
            uiScale_=2.0f;pass("ui2");
            uiScale_=1.0f;
            beginSpectating();pass("spec");
            std::fprintf(stderr,"PASS: zoom floor fits the world view (%dx%d)\n",W,H);
            return;
        }
        if (ta::devFlag("TA_HIDDEN_UNITS_TEST")) {
            // HIDDEN UNITS through the real draw(): a flat feature sorts under every
            // unit; units behind a tree or a building get a dot (DOTS) or fade that
            // tree or building (FADE); units in the open, in front of the tree or on a
            // flat feature get nothing; fog still hides enemies; OFF and Tactical Dots
            // frames do nothing; a hidden dot is clickable.
            // TA_HIDDEN_UNITS_CAPTURE=<prefix> writes <prefix>-off/-dots/-fade-off/-fade.png.
            namespace hu = ta::hiddenunits;
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("hidden units: "+what);
                std::fprintf(stderr,"PASS: hidden units: %s\n",what.c_str());
            };
            constexpr int W=1280,H=960;
            float zoom=1.0f;
            noFog_=false;edgeScrollOn_=false;tacticalDotsOpt_=false;healthBars_=0;
            fogStill_=true;   // frames are compared pixel for pixel: hold the rolling fog
            const bool swayWas=settings_ && settings_->treeSway;
            if (settings_) settings_->treeSway=false;   // a still crown: exact cover points
            writableWorld().setPlayerCount(2);
            loadFeatureDefs();
            const auto heightOf=[&](const char* name) {
                const auto it=featureDefs_.find(name);
                return it==featureDefs_.end() ? -1 : int(it->second.numberOr("height",0));
            };
            check(heightOf("tree1")>=hu::kLowFeatureHeight && heightOf("smudge01")==0,
                  "fixture features: a tall tree and a flat smudge");
            const int cx0=mapView_.map().blocksX/2,cz0=mapView_.map().blocksY/2;
            // A clear stage: the map's own scenery around the fixture is not drawn.
            for (auto& f:features_)
                if (std::fabs(f.x-float(cx0*16+200))<700 && std::fabs(f.z-float(cz0*16+100))<600) f.tex=nullptr;
            const auto place=[&](const char* name,int cx,int cz) {
                check(addFeature(name,cx,cz),std::string("placed ")+name);
                return features_.size()-1;
            };
            const size_t treeA=place("tree1",cx0,cz0);
            const size_t treeB=place("tree1",cx0+8,cz0);
            const size_t treeFog=place("tree1",cx0+40,cz0+20);
            const size_t smudge=place("smudge01",cx0-9,cz0+1);
            const float x0=features_[treeA].x,z0=features_[treeA].z;
            // The tree's crown: the opaque texel run up the sprite's anchor column; a
            // unit whose dot centre sits in the middle of it is covered.
            const FeatArt* art=features_[treeA].art;
            check(art && !art->masks.empty(),"tree art has an opacity mask");
            const auto& mask=art->masks[0];
            int top=-1,bottom=-1;
            for (int y=0;y<mask.height();++y)
                if (mask.opaque(features_[treeA].xoff,y)) { if (top<0) top=y; bottom=y; }
            check(top>=0 && features_[treeA].yoff-(top+bottom)/2>12,"tree crown above its base");
            const float behindDz=float(features_[treeA].yoff-(top+bottom)/2);
            int particlesOn=-1;   // a unit given one construction particle (drawUnit path)
            debugFogDisc(x0,z0,500.0f);
            const auto frame=[&](const char* capture=nullptr) {
                captureFrame();beginFrame();cosmeticStep(0);
                Frame& f=frameBuf_[renderReadIdx_];
                for (auto& r:f.units)
                    if (r.id==particlesOn && r.type) r.constructionParticles.assign(1,{});
                mapView_.setZoom(zoom);
                mapView_.setOffset(x0+300-float(W-300)*0.5f/zoom,z0+100-float(H)*0.5f/zoom);
                prepare(W,H);mapView_.finishChunks();prepare(W,H);draw(W,H);   // the whole terrain: frames compare pixels
                std::vector<uint8_t> px;
                int ow=0,oh=0;SDL_GetRendererOutputSize(ren_,&ow,&oh);
                px.resize(size_t(ow)*size_t(oh)*4);
                if (readViewPixels(px.data(),ow*4)!=0) px.clear();
                else if (capture) ta::png::write(capture,ow,oh,px);
                endFrame();
                return std::pair{px,ow};
            };
            const int behind=spawn("armpw",x0,z0-behindDz,0,0);
            const int open=spawn("armpw",x0-70,z0-behindDz,0,0);
            const int front=spawn("armpw",x0,z0+28,0,0);
            const int foeSeen=spawn("armpw",features_[treeB].x,features_[treeB].z-behindDz,0,1);
            const int foeFog=spawn("armpw",features_[treeFog].x,features_[treeFog].z-behindDz,0,1);
            // On the smudge, a little north of its anchor: the smudge sorts after it by
            // depth, but flat scenery is drawn before every unit.
            const int onSmudge=spawn("armpw",features_[smudge].x,features_[smudge].z-6,0,0);
            const int keep=spawn("armlab",x0+320,z0+40,0,0);
            const int behindKeep=spawn("armpw",x0+320,z0+10,0,0);
            // Behind two overlapping trees, and behind a second kind of building (a vehicle
            // plant, open at the front).
            const size_t treeC=place("tree1",cx0+10,cz0-12);
            place("tree1",cx0+11,cz0-11);
            const int behindTwo=spawn("armpw",features_[treeC].x+8,features_[treeC].z-behindDz,0,0);
            const int lode=spawn("armvp",x0+120,z0+300,0,0);
            const int behindLode=spawn("armpw",x0+120,z0+270,0,0);
            check(behind>=0 && open>=0 && front>=0 && foeSeen>=0 && foeFog>=0 && onSmudge>=0 && keep>=0 &&
                  behindKeep>=0 && behindTwo>=0 && lode>=0 && behindLode>=0,"fixture spawned");
            const char* capture=ta::devEnv("TA_HIDDEN_UNITS_CAPTURE");
            const auto shot=[&](const std::string& mode) {
                return capture ? std::string(capture)+"-"+mode+".png" : std::string();
            };
            const auto at=[](const std::string& path) { return path.empty() ? nullptr : path.c_str(); };
            // ---- the low layer ----
            hiddenUnitsMode_=hu::kOff;
            frame(at(shot("off")));
            {
                size_t smudgeAt=SIZE_MAX,unitAt=SIZE_MAX,treeAt=SIZE_MAX,frontAt=SIZE_MAX,firstUnit=SIZE_MAX;
                for (size_t i=0;i<paintItems_.size();++i) {
                    const auto& it=paintItems_[i];
                    if (it.f==&features_[smudge]) smudgeAt=i;
                    if (it.f==&features_[treeA]) treeAt=i;
                    if (it.u && it.u->id==onSmudge) unitAt=i;
                    if (it.u && it.u->id==front) frontAt=i;
                    if (it.u && firstUnit==SIZE_MAX) firstUnit=i;
                }
                check(features_[smudge].low && !features_[treeA].low,"TDF height decides the low layer");
                check(features_[smudge].z>frameUnit(onSmudge).z,"the smudge is south of the unit on it");
                check(smudgeAt<firstUnit && smudgeAt<unitAt && unitAt!=SIZE_MAX,"flat feature drawn before every unit");
                check(treeAt!=SIZE_MAX && frontAt!=SIZE_MAX && treeAt<frontAt,"tall tree still sorts by depth");
                check(hiddenDotIds_.empty() && debugHiddenCandidates_==0,"OFF: nothing looked at");
            }
            // ---- DOTS ----
            hiddenUnitsMode_=hu::kDots;
            frame(at(shot("dots")));
            check(debugHiddenCandidates_>=3,"candidates found behind covers");
            check(hiddenDot(behind),"own unit behind a tree: dot");
            check(hiddenDot(foeSeen),"enemy in sight behind a tree: dot");
            check(hiddenDot(behindKeep),"unit behind a building: dot");
            check(!hiddenDot(open),"unit in the open: no dot");
            check(!hiddenDot(front),"unit in front of the tree: no dot");
            check(!hiddenDot(onSmudge),"unit on flat scenery: no dot");
            check(!hiddenDot(foeFog),"enemy in fog behind a tree: no dot");
            check(!hiddenDot(keep),"a building is never marked");
            {   // Under repair (its particles send it through drawUnit) it still covers.
                particlesOn=keep;
                frame();
                particlesOn=-1;
                check(!frameUnit(keep).constructionParticles.empty() && hiddenDot(behindKeep),
                      "unit behind a building under repair: dot");
            }
            {   // The dot is a click target.
                const SDL_FPoint c=dotCentre(frameUnit(behind));
                SDL_Event e{};
                const auto savedMod=SDL_GetModState();SDL_SetModState(KMOD_NONE);
                selection_.clear();
                e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(c.x);e.button.y=int(c.y);
                inputFromView(e,W,H);
                e.type=SDL_MOUSEBUTTONUP;inputFromView(e,W,H);
                SDL_SetModState(savedMod);
                check(selection_==std::vector<int>{behind},"click on the hidden dot selects the unit");
                selection_.clear();
            }
            // ---- Tactical Dots frames draw no hidden dots on top of their own ----
            {
                tacticalDotsOpt_=true;tacticalDotsZoom_=100;
                frame();
                check(dotsFrame_ && hiddenDotIds_.empty() && debugHiddenCandidates_==0,
                      "Tactical Dots frame: nothing extra");
                tacticalDotsOpt_=false;frame();
                check(!dotsFrame_,"models back");
            }
            // A unit's box (model box over all headings), in pixels (output scale 1).
            const auto boxOf=[&](int id) {
                const UnitR& u=frameUnit(id);
                const SDL_FPoint a=dotCentre(u);
                const SDL_FRect& hb=unitHitBox(u.type);
                return SDL_Rect{int(a.x+hb.x*zoom),int(a.y+hb.y*zoom),int(hb.w*zoom)+1,int(hb.h*zoom)+1};
            };
            // ---- FADE ----
            // A tree, rock or building over a unit the frame looks at fades whole once the
            // fade-out time has passed (FADE's clock is the harness's here); a tree with no
            // unit under it, and one over an enemy in fog, are drawn as with OFF; at 100%
            // the frame is OFF's. A small forest with units under it joins the view
            // (captures -fade-off / -fade).
            {
                uint64_t clock=1000;
                const auto fadeFrame=[&](uint64_t stepMs,const std::string& capture=std::string()) {
                    clock+=stepMs;debugHiddenFadeNowMs_=clock;
                    return frame(at(capture));
                };
                std::vector<size_t> forest;
                for (int j=0;j<3;++j) for (int i=0;i<4;++i) forest.push_back(place("tree1",cx0-8+3*i,cz0+12+3*j));
                const size_t treeLone=place("tree1",cx0+22,cz0+16);
                std::vector<int> underForest;
                for (const auto& [i,j]:{std::pair{0,0},std::pair{1,1},std::pair{2,0},std::pair{3,2},std::pair{0,2}}) {
                    const auto& t=features_[forest[size_t(j*4+i)]];
                    underForest.push_back(spawn("armpw",t.x,t.z-behindDz,0,0));
                }
                check(std::all_of(underForest.begin(),underForest.end(),[](int id) { return id>=0; }),"forest units spawned");
                const auto alphaOf=[&](size_t t) { return hiddenFeatureAlpha(features_[t]); };
                const auto rectOf=[&](size_t t) {
                    const FeatureSprite sp=featureSprite(features_[t],zoom);
                    return SDL_Rect{int(sp.dst.x)-1,int(sp.dst.y)-1,int(sp.dst.w)+3,int(sp.dst.h)+3};
                };
                const auto changedIn=[&](const std::vector<uint8_t>& p0,const std::vector<uint8_t>& p1,int w,SDL_Rect b) {
                    size_t strong=0;
                    const int h=int(p0.size()/4/size_t(std::max(1,w)));
                    for (int y=std::max(0,b.y);y<std::min(h,b.y+b.h);++y)
                        for (int x=std::max(0,b.x);x<std::min(w,b.x+b.w);++x) {
                            const size_t i=(size_t(y)*size_t(w)+size_t(x))*4;
                            int most=0;
                            for (int c=0;c<3;++c) most=std::max(most,std::abs(int(p0[i+c])-int(p1[i+c])));
                            strong+=most>2;
                        }
                    return strong;
                };
                // Every FADE frame also tests each unit's covers one by one: the
                // per-frame bookkeeping must give the same fades and answers (it aborts
                // otherwise).
                debugHiddenVerify_=true;
                const size_t verifiedBefore=debugHiddenVerified_;
                hiddenFadeOpacity_=hu::fadeOpacity(hu::kDefaultFadePercent);
                const uint8_t faded=hiddenFadeOpacity_;
                check(faded==128,"FADE: 50% is alpha 128");
                const auto keepAlpha=[&] { return hiddenStructureAlpha(frameUnit(keep)); };
                hiddenUnitsMode_=hu::kOff;
                const auto [p0,w0]=fadeFrame(16,shot("fade-off"));
                hiddenUnitsMode_=hu::kFade;
                fadeFrame(16);
                check(alphaOf(treeA)<255 && alphaOf(treeA)>faded,"FADE: the tree starts fading");
                check(keepAlpha()<255 && keepAlpha()>faded,"FADE: the keep starts fading");
                fadeFrame(uint64_t(hu::kFadeOutMs));
                const auto [p1,w1]=fadeFrame(16,shot("fade"));
                check(!p0.empty() && p1.size()==p0.size(),"FADE frames read back");
                check(alphaOf(treeA)==faded && alphaOf(treeB)==faded && alphaOf(treeC)==faded,
                      "FADE: trees over units faded to 50% after the fade-out time");
                check(keepAlpha()==faded,"FADE: the keep over a unit faded to 50%");
                check(hiddenStructureAlpha(frameUnit(lode))==faded,"FADE: the vehicle plant over a unit faded to 50%");
                size_t forestFaded=0;
                for (size_t t:forest) forestFaded+=alphaOf(t)==faded;
                std::fprintf(stderr,"  FADE: %zu of %zu forest trees faded, %zu fading covers\n",forestFaded,
                             forest.size(),hiddenFades_.size());
                check(forestFaded>=underForest.size(),"FADE: the forest trees over units faded");
                check(alphaOf(treeLone)==255 && changedIn(p0,p1,w0,rectOf(treeLone))==0,
                      "FADE: a tree with no unit under it unchanged");
                check(alphaOf(treeFog)==255,"FADE: an enemy in fog under a tree does not fade it");
                check(changedIn(p0,p1,w0,boxOf(behind))>20,"FADE: the unit behind the tree shows");
                check(changedIn(p0,p1,w0,boxOf(behindKeep))>20,"FADE: the unit behind the keep shows");
                check(changedIn(p0,p1,w0,boxOf(behindLode))>20,"FADE: the unit behind the vehicle plant shows");
                // 100%: the covers stay whole -- the frame is OFF's.
                hiddenFadeOpacity_=hu::fadeOpacity(100);
                const auto [p100,w100]=fadeFrame(16);
                check(p100==p0,"FADE at 100% draws exactly what OFF does");
                hiddenFadeOpacity_=faded;
                // Tactical Dots frames: no fade (dropped).
                tacticalDotsOpt_=true;tacticalDotsZoom_=100;
                fadeFrame(16);
                check(dotsFrame_ && hiddenFades_.empty(),"FADE: nothing while Tactical Dots shows");
                tacticalDotsOpt_=false;
                fadeFrame(16);
                check(!dotsFrame_,"FADE: models back");
                debugHiddenFadeNowMs_=0;
                check(debugHiddenVerified_>verifiedBefore+underForest.size(),
                      "FADE: covers found once per frame match testing every cover");
                debugHiddenVerify_=false;
            }
            SDL_RendererInfo info{};SDL_GetRendererInfo(ren_,&info);
            // ---- the policy on its own ----
            {
                hu::AlphaMask m;
                const uint8_t px[4*4]={0,0,0,0, 0,0,0,255, 0,0,0,255, 0,0,0,0};   // 2x2: TL clear, TR, BL opaque
                m.build(px,2,2);
                check(!hu::spriteCovers(m,10,10,20,20,0,12,12) && hu::spriteCovers(m,10,10,20,20,0,25,12),
                      "sprite cover reads the texel under the point");
                check(hu::spriteCovers(m,10,10,20,20,10,35,12) && !hu::spriteCovers(m,10,10,20,20,0,35,12) &&
                      hu::spriteCovers(m,10,10,20,20,10,12,28),"sprite cover follows the sway shear");
                check(hu::featureLayer(9)==hu::kLayerLowFeature && hu::featureLayer(10)==hu::kLayerGround,
                      "height 9 is low, 10 is not");
            }
            if (settings_) settings_->treeSway=swayWas;
            hiddenUnitsMode_=hu::kDefault;
            std::fprintf(stderr,"PASS: hidden units (%s)\n",info.name ? info.name : "?");
            return;
        }
        if (ta::devFlag("TA_RUIN_TEST")) {
            const bool previousShadows=shadowsOnFrame_;
            shadowsOnFrame_=true;
            int checked=0;
            for (const auto& [name,type]:registry_.types()) {
                if (!type.explosionCorpse) continue;
                const int corpse=world_.corpseTypeOf(&type);
                if (corpse<0) throw std::runtime_error("building ruin definition missing");
                const int id=spawn(name,512,512,0,localPlayer_);
                UnitR unit;unit.id=id;unit.type=&type;unit.player=localPlayer_;
                unit.x=unit.px=512;unit.z=unit.pz=512;
                unit.deadFor=0;unit.corpsePhase=true;unit.corpseFeat=corpse;
                maybeSwapCorpseModel(unit);
                UnitGeom geometry;std::vector<Tri> scratch;
                buildUnitGeom(unit,geometry,scratch);
                // Flat rubble decals need not cast a separate raised shadow.
                if (geometry.verts.empty())
                    throw std::runtime_error("empty building ruin geometry: "+name);
                std::fprintf(stderr,"PASS: %s ruin body=%zu shadow=%zu\n",name.c_str(),
                    geometry.verts.size(),geometry.shadowVerts.size());
                ++checked;
            }
            shadowsOnFrame_=previousShadows;
            // (Kingdoms shipped 45 such buildings; the count is whatever the data set has.)
            if (!checked) throw std::runtime_error("no building with an explosion corpse in the data");
            return;
        }
        if (ta::devFlag("TA_DIPLOMACY_TEST")) {
            int retiredFixtureId=0;
            if(ta::devFlag("TA_RETIREMENT_TEST")) {
                ta::sim::UnitType dummy;dummy.id="retired-ui";dummy.maxHp=100;dummy.footX=dummy.footZ=1;
                for(int i=0;i<240;++i) {
                    retiredFixtureId=writableWorld().spawn(&dummy,1800,2100,0,0);
                    writableWorld().scenarioDestroy(retiredFixtureId);
                }
                for(int i=0;i<60;++i)writableWorld().tick(1.f/30);
                for(const auto& u:world_.units())
                    if(u.type==&dummy)throw std::runtime_error("UI fixture retained final corpses");
            }
            writableWorld().setPlayerCount(8);writableWorld().setTeam(1,0);localPlayer_=0;
            resultParticipants_=uint8_t((1u<<0)|(1u<<1)|(1u<<2)|(1u<<7));
            playerName_[0]="PLAYER";playerName_[1]="ALLY";playerName_[2]="ENEMY";playerName_[7]="DEFEATED";
            {   // Humans in slots 0 (you), 1 and 7; the ENEMY in slot 2 is a computer player.
                // You and ALLY are team 0; ENEMY and DEFEATED have teams of their own.
                ta::net::RoomView room;
                for(int i:{0,1,7})room.slots[i].type=1;
                room.slots[2].type=2;
                room.slots[2].team=1;room.slots[7].team=2;room.mySlot=0;
                resetDiplomacy(room);
            }
            writableWorld().player(7).defeated=true;hotkeys_.load({});
            const int commander=spawn("armcom",1800,2100,0,0);
            const int soldier=spawn("armpw",1650,2100,0,0);
            spawn("armpw",2000,2100,0,1);spawn("corak",2200,2100,0,2);
            writableWorld().unit(soldier)->squad=3;
            const auto publish=[&] {captureFrame();beginFrame();};
            const auto check=[](bool yes,const char* why) {if(!yes)throw std::runtime_error(why);};
            const auto has=[&](DiplomacyAction action,int p) {
                return std::any_of(giveUnitsHots_.begin(),giveUnitsHots_.end(),[&](const auto& h){return h.action==action && h.player==p;});
            };
            const auto click=[&](DiplomacyAction action,int p) {
                auto found=std::find_if(giveUnitsHots_.begin(),giveUnitsHots_.end(),[&](const auto& h){return h.action==action && h.player==p;});
                check(found!=giveUnitsHots_.end(),"diplomacy control unavailable");
                SDL_Event e{};e.type=SDL_MOUSEBUTTONUP;e.button.button=SDL_BUTTON_LEFT;
                e.button.x=int(found->rect.x+found->rect.w/2);e.button.y=int(found->rect.y+found->rect.h/2);
                input(e,1000,700);
            };
            selection_.clear();publish();
            if(ta::devFlag("TA_RETIREMENT_TEST") &&
               (front().units.size()!=world_.units().size() || !frameUnitP(commander) || frameUnitP(retiredFixtureId)))
                throw std::runtime_error("compacted render snapshot lost high-ID units");
            SDL_Event key{};key.type=SDL_KEYDOWN;key.key.keysym.sym=SDLK_d;
            input(key,1000,700);check(giveUnitsMenu_,"D must open diplomacy without a selection");
            drawGiveUnitsMenu(1000,700);
            check(!has(DiplomacyAction::Give,1),"empty selection must disable gifts");
            check(has(DiplomacyAction::ShareMetal,1) && has(DiplomacyAction::ShareEnergy,0),"your own share-metal / share-energy switches");
            for(int p:{0,1,2,7})check(has(DiplomacyAction::Chat,p),"all participants need chat controls");
            check(!has(DiplomacyAction::Chat,3),"empty slots must not appear as players");
            check(chatRecipients_==0x03 && framePlayer(0).shareResources==0,"sharing off by default; chat to allies only");
            // VOICE: chat's column again, same defaults (allies only) and the same players; your
            // own box is shown but inert (your voice never comes back to you).
            check(voiceRecipients_==chatRecipients_ && voiceRecipients_==0x03,"voice defaults equal chat's: allies only");
            for(int p:{1,2,7})check(has(DiplomacyAction::Voice,p),"every other participant needs a voice control");
            check(!has(DiplomacyAction::Voice,0) && !has(DiplomacyAction::Voice,3),"no voice control for yourself or an empty slot");
            click(DiplomacyAction::Chat,2);check(chatRecipients_ & 4,"chat checkbox adds the enemy");
            check(voiceRecipients_==0x03,"the chat column leaves voice alone");
            click(DiplomacyAction::Voice,2);check((voiceRecipients_ & 4) && (voiceRecipients_ & 2),"voice checkbox updates its recipients");
            click(DiplomacyAction::Voice,2);check(voiceRecipients_==0x03,"and back");
            // SPECTATORS: a row of its own, chat and voice both off by default, each its own box.
            check(has(DiplomacyAction::Chat,kDiplomacySpectators) && has(DiplomacyAction::Voice,kDiplomacySpectators),
                  "the SPECTATORS row has chat and voice boxes");
            check(!chatSpectators_ && !voiceSpectators_,"spectators get neither chat nor voice by default");
            click(DiplomacyAction::Chat,kDiplomacySpectators);
            check(chatSpectators_ && !voiceSpectators_ && chatRecipients_==0x07,"ticking SPECTATORS chat leaves voice and the players alone");
            click(DiplomacyAction::Voice,kDiplomacySpectators);check(voiceSpectators_ && voiceRecipients_==0x03,"SPECTATORS voice");
            click(DiplomacyAction::Chat,kDiplomacySpectators);click(DiplomacyAction::Voice,kDiplomacySpectators);
            check(!chatSpectators_ && !voiceSpectators_,"and off again");
            // VOLUME: a slider for each other human (ALLY, DEFEATED), none for yourself or the
            // computer player, every one at 100; greyed and inert while voice chat is off.
            const auto allFull=[&]{return std::all_of(voiceVolume_.begin(),voiceVolume_.end(),[](uint8_t v){return v==100;});};
            const auto volumeHot=[&](int p) {
                auto found=std::find_if(giveUnitsHots_.begin(),giveUnitsHots_.end(),[&](const auto& h){return h.action==DiplomacyAction::Volume && h.player==p;});
                check(found!=giveUnitsHots_.end(),"VOLUME slider unavailable");
                return found->rect;
            };
            const auto mouse=[&](Uint32 type,float mx,float my,bool held) {
                SDL_Event e{};e.type=type;
                if(type==SDL_MOUSEMOTION){e.motion.x=int(mx);e.motion.y=int(my);e.motion.state=held?SDL_BUTTON_LMASK:0;}
                else{e.button.button=SDL_BUTTON_LEFT;e.button.x=int(mx);e.button.y=int(my);}
                input(e,1000,700);
            };
            check(allFull(),"every VOLUME starts at 100");
            check(voiceVolumeSlider(1) && voiceVolumeSlider(7),"a VOLUME slider for every other human");
            check(!voiceVolumeSlider(0) && !voiceVolumeSlider(2) && !voiceVolumeSlider(3),"none for yourself, a computer player or an empty slot");
            check(!has(DiplomacyAction::Volume,1) && !has(DiplomacyAction::Volume,7),"no voice engine: the sliders are greyed");
            ta::VoiceEngine engine;   // opens nothing: it is never pumped, only asked whether voice is on
            ta::Settings voiceCfg;
            engine.configure(voiceCfg,"");voice_=&engine;drawGiveUnitsMenu(1000,700);
            check(!has(DiplomacyAction::Volume,1) && !has(DiplomacyAction::Volume,7),"voice chat off: the sliders are greyed");
            {   // ...and inert: a press where ALLY's slider sits changes nothing.
                voiceCfg.voiceEnabled=true;engine.configure(voiceCfg,"");drawGiveUnitsMenu(1000,700);
                const SDL_FRect r=volumeHot(1);
                voiceCfg.voiceEnabled=false;engine.configure(voiceCfg,"");drawGiveUnitsMenu(1000,700);
                mouse(SDL_MOUSEBUTTONDOWN,r.x+r.w*0.3f,r.y+r.h/2,true);mouse(SDL_MOUSEBUTTONUP,r.x+r.w*0.3f,r.y+r.h/2,false);
                check(allFull() && voiceVolumeDrag_<0,"a greyed slider ignores the mouse");
            }
            voiceCfg.voiceEnabled=true;engine.configure(voiceCfg,"");drawGiveUnitsMenu(1000,700);
            check(has(DiplomacyAction::Volume,1) && has(DiplomacyAction::Volume,7),"voice chat on: the sliders are live");
            check(!has(DiplomacyAction::Volume,0) && !has(DiplomacyAction::Volume,2) && !has(DiplomacyAction::Volume,3),
                  "still no slider for yourself or the computer player");
            {
                const SDL_FRect r=volumeHot(1);
                mouse(SDL_MOUSEBUTTONDOWN,r.x+r.w*0.3f,r.y+r.h/2,true);
                check(voiceVolume_[1]>=29 && voiceVolume_[1]<=31,"a press sets ALLY's VOLUME where it lands");
                mouse(SDL_MOUSEBUTTONUP,r.x+r.w*0.3f,r.y+r.h/2,false);
                // A new game puts every slider back to 100.
                ta::net::RoomView room;
                for(int i:{0,1,7})room.slots[i].type=1;
                room.slots[2].type=2;
                resetDiplomacy(room);
                check(allFull() && humanSlots_==((1u<<0)|(1u<<1)|(1u<<7)),"a new game starts every VOLUME at 100");
                input(key,1000,700);drawGiveUnitsMenu(1000,700);check(giveUnitsMenu_,"diplomacy reopens");
                // Drag: past either end clamps to 0..100, and the release, wherever it lands,
                // ends the drag without clicking what is under it (ENEMY's CHAT box here).
                mouse(SDL_MOUSEBUTTONDOWN,r.x+r.w*0.5f,r.y+r.h/2,true);
                check(voiceVolume_[1]>=49 && voiceVolume_[1]<=51 && voiceVolumeDrag_==1,"a press starts a drag");
                mouse(SDL_MOUSEMOTION,r.x+r.w+80,r.y-40,true);check(voiceVolume_[1]==100,"dragged past the end: 100");
                mouse(SDL_MOUSEMOTION,r.x-80,r.y,true);check(voiceVolume_[1]==0,"dragged past the start: 0");
                mouse(SDL_MOUSEMOTION,r.x+r.w*0.5f,r.y,true);
                const auto chat=chatRecipients_;
                auto box=std::find_if(giveUnitsHots_.begin(),giveUnitsHots_.end(),[&](const auto& h){return h.action==DiplomacyAction::Chat && h.player==2;});
                check(box!=giveUnitsHots_.end(),"ENEMY has a CHAT box");
                mouse(SDL_MOUSEBUTTONUP,box->rect.x+box->rect.w/2,box->rect.y+box->rect.h/2,false);
                check(voiceVolumeDrag_<0 && chatRecipients_==chat && voiceVolume_[1]>=49 && voiceVolume_[1]<=51,
                      "releasing a drag over another control only ends the drag");
                check(voiceVolume_[7]==100 && voiceVolume_[0]==100 && voiceVolume_[2]==100,"one slider moves one player");
                const SDL_FRect r7=volumeHot(7);
                mouse(SDL_MOUSEBUTTONDOWN,r7.x+r7.w*0.25f,r7.y+r7.h/2,true);mouse(SDL_MOUSEBUTTONUP,r7.x+r7.w*0.25f,r7.y+r7.h/2,false);
                check(voiceVolume_[7]>=24 && voiceVolume_[7]<=26 && voiceVolume_[1]>=49 && voiceVolume_[1]<=51,"DEFEATED's slider on its own");
            }
            if(const char* cap=ta::devEnv("TA_DIPLOMACY_CAPTURE")) {   // the screen as drawn, to look at
                SDL_SetRenderDrawColor(ren_,40,60,40,255);SDL_RenderClear(ren_);drawGiveUnitsMenu(1000,700);
                int ow=0,oh=0;SDL_GetRendererOutputSize(ren_,&ow,&oh);
                std::vector<uint8_t> px(size_t(ow)*size_t(oh)*4);
                if (SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_RGBA32,px.data(),ow*4)==0)ta::png::write(cap,ow,oh,px);
                voiceCfg.voiceEnabled=false;engine.configure(voiceCfg,"");
                SDL_SetRenderDrawColor(ren_,40,60,40,255);SDL_RenderClear(ren_);drawGiveUnitsMenu(1000,700);
                if (SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_RGBA32,px.data(),ow*4)==0)
                    ta::png::write(std::string(cap)+".off.png",ow,oh,px);
                voiceCfg.voiceEnabled=true;engine.configure(voiceCfg,"");
            }
            // Push to talk never fires while a text field has focus.
            check(!textEntryActive(),"no text field focused");
            chatTyping_=true;check(textEntryActive(),"the chat composer counts as typing");chatTyping_=false;
            SDL_StartTextInput();check(textEntryActive(),"any focused text field counts as typing");SDL_StopTextInput();
            lbField_=4;check(!textEntryActive(),"a lobby field still marked once the game runs is not typing");lbField_=0;
            click(DiplomacyAction::ShareMetal,1);check(world_.player(0).shareResources & 1,"share-metal checkbox issues command");
            selection_={commander};drawGiveUnitsMenu(1000,700);
            check(!has(DiplomacyAction::Give,1),"an ineligible commander must disable gifts");
            selection_={commander,soldier};drawGiveUnitsMenu(1000,700);
            check(has(DiplomacyAction::Give,1) && !has(DiplomacyAction::Give,2),"eligible gifts only enabled for allies");
            click(DiplomacyAction::Give,1);publish();drawGiveUnitsMenu(1000,700);
            check(giveUnitsMenu_ && !has(DiplomacyAction::Give,1),"gift keeps menu open and disables exhausted selection");
            check(selection_==std::vector<int>{commander} && world_.unit(soldier)->player==1 && world_.unit(soldier)->squad==0,"gift filters selection and removes old control group");
            input(key,1000,700);check(!giveUnitsMenu_,"D closes diplomacy");
            input(key,1000,700);drawGiveUnitsMenu(1000,700);
            voice_=nullptr;   // the harness's engine is not the game's
            std::fprintf(stderr,"PASS: diplomacy hotkey, roster, defaults, checkboxes, voice column, volume sliders, eligibility, gift and selection cleanup\n");
            return;
        }
        // What a click orders, through the real input() (client/clickrules.h, PLAN
        // 2.1-2.11): per-unit right-clicks on release, the right-drag and armed boxes,
        // P4 (an armed click obeys its cursor), the one picker, the minimap's orders
        // and placement, and the left-click select rules.
        if (ta::devFlag("TA_CLICK_TEST")) {
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("clicks: "+what);
                std::fprintf(stderr,"PASS: clicks: %s\n",what.c_str());
            };
            constexpr int W=1000,H=700;
            winW_=W;winH_=H;noFog_=true;edgeScrollOn_=false;uiScale_=1.0f;
            writableWorld().setPlayerCount(2);
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const ta::sim::UnitType *flyerType=nullptr,*transportType=nullptr;
            for (const auto& [id,t]:registry_.types()) {
                if (!flyerType && t.canFly && t.canMove && !t.canTransport && !t.isBuilder && t.canGuard) flyerType=&t;
                if (!transportType && t.canTransport && t.canFly && t.canMove && !t.hasPrimaryWeaponBlock) transportType=&t;
            }
            check(flyerType && transportType,"fixture has a flyer and a weaponless flying transport");
            const ta::sim::UnitType* masonType=nullptr;
            for (const auto& [id,t]:registry_.types())
                if (t.isBuilder && !t.isStructure() && t.canReclaim && !t.hasPrimaryWeaponBlock && !t.canFly &&
                    t.canGuard && !t.canResurrect && !registry_.buildable(id).empty() &&
                    (!masonType || id<masonType->id)) masonType=&t;
            check(masonType!=nullptr,"fixture mason: a ground builder that reclaims, with no weapon");
            std::fprintf(stderr,"clicks: mason %s, flyer %s, transport %s\n",masonType->id.c_str(),
                         flyerType->id.c_str(),transportType->id.c_str());
            const int archer=spawn("armrock",cx-60,cz,0,localPlayer_);
            const int mason=spawn(masonType->id,cx-60,cz+60,0,localPlayer_);
            const int sword=spawn("armpw",cx-120,cz+30,0,localPlayer_);
            const int enemy1=spawn("corstorm",cx+120,cz-20,0,1);
            const int enemy2=spawn("corstorm",cx+160,cz+40,0,1);
            const int flyerA=spawn(flyerType->id,cx-200,cz-120,0,localPlayer_);
            const int flyerB=spawn(flyerType->id,cx-260,cz-120,0,localPlayer_);
            const int carrier=spawn(transportType->id,cx-300,cz+150,0,localPlayer_);
            for (int id:{archer,mason,sword,enemy1,enemy2,flyerA,flyerB,carrier}) check(id>=0 && world_.unit(id),"fixture unit spawned");
            mapView_.setZoom(1);mapView_.setOffset(cx-500,cz-300);
            const auto publish=[&] {captureFrame();beginFrame();cosmeticStep(0);};
            const auto savedMod=SDL_GetModState();SDL_SetModState(KMOD_NONE);
            const auto event=[&](uint32_t kind,uint8_t button,float x,float y) {
                SDL_Event e{};e.type=kind;
                if (kind==SDL_MOUSEMOTION) {e.motion.x=int(x);e.motion.y=int(y);}
                else {e.button.button=button;e.button.x=int(x);e.button.y=int(y);e.button.clicks=1;}
                inputFromView(e,W,H);   // a point in the world view
            };
            const auto at=[&](int id) {return unitScreen(frameUnit(id));};
            const auto stopAll=[&] {
                for (int id:{archer,mason,sword,flyerA,flyerB,carrier}) {writableWorld().stop(id);writableWorld().unit(id)->orders.clear();}
            };
            // The commands one gesture issues (captured, not applied).
            const auto capture=[&](auto&& gesture) {
                std::vector<ta::net::Command> cmds;
                ClickScope outer(*this);
                gesture();
                cmds=click_->cmds;
                click_->cmds.clear();
                return cmds;
            };
            const auto count=[](const std::vector<ta::net::Command>& cmds,int unit,ta::net::Cmd kind,int target=-999) {
                int n=0;
                for (const auto& c:cmds) n+=c.unitId==unit && c.kind==kind && (target==-999 || c.targetId==target);
                return n;
            };
            const auto clickAt=[&](float x,float y,uint8_t button) {
                event(SDL_MOUSEMOTION,0,x,y);event(SDL_MOUSEBUTTONDOWN,button,x,y);event(SDL_MOUSEBUTTONUP,button,x,y);
            };
            const auto dragTo=[&](float x0,float y0,float x1,float y1,uint8_t button) {
                event(SDL_MOUSEMOTION,0,x0,y0);event(SDL_MOUSEBUTTONDOWN,button,x0,y0);
                event(SDL_MOUSEMOTION,0,x1,y1);
            };
            bool tint=false;
            stopAll();publish();

            // 2.1 right-click an enemy: the archer attacks, the mason (no weapon, a
            // reclaimer) takes it apart -- TotalA.exe 0x43f0e0 action 1: canattack ->
            // the attack (0x43fa0d), else canreclamate -> RECLAIMUNIT (0x43fa13); the
            // order waits for the release (P3).
            selection_={archer,mason};publish();
            {
                const auto p=at(enemy1);
                std::vector<ta::net::Command> pressCmds;
                const auto cmds=capture([&] {
                    event(SDL_MOUSEMOTION,0,p.x,p.y);event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_RIGHT,p.x,p.y);
                    pressCmds=click_->cmds;
                    event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,p.x,p.y);
                });
                check(pressCmds.empty(),"P3 a right press orders nothing until the release");
                check(count(cmds,archer,ta::net::Cmd::Attack,enemy1)==1,"2.1 right-click an enemy: the archer attacks it");
                check(count(cmds,mason,ta::net::Cmd::ReclaimUnit,enemy1)==1 && cmds.size()==2,"2.1 right-click an enemy: the mason reclaims it");
            }
            // 2.1 right-click an own unit: the archer guards it.
            selection_={archer,mason};publish();
            {
                const auto p=at(sword);
                const auto cmds=capture([&] {clickAt(p.x,p.y,SDL_BUTTON_RIGHT);});
                check(count(cmds,archer,ta::net::Cmd::Guard,sword)==1,"2.1 right-click an own unit: the archer guards it");
            }
            // 2.2 the right-drag box over both enemies: the archer attacks each, nearest
            // first, the mason clears the box; the cursor says Attack while dragging.
            selection_={archer,mason};publish();
            {
                const auto p1=at(enemy1),p2=at(enemy2);
                const float x0=std::min(p1.x,p2.x)-40,y0=std::min(p1.y,p2.y)-40,x1=std::max(p1.x,p2.x)+40,y1=std::max(p1.y,p2.y)+40;
                ta::CursorId boxCursor=ta::CursorId::Hourglass;
                const auto cmds=capture([&] {
                    dragTo(x0,y0,x1,y1,SDL_BUTTON_RIGHT);
                    boxCursor=desiredCursor(tint);
                    event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,x1,y1);
                });
                check(boxCursor==ta::CursorId::Attack,"2.2 the box cursor over enemies with an archer: Attack");
                int first=-1,firstQueue=-1,second=-1,secondQueue=-1;
                for (const auto& c:cmds)
                    if (c.unitId==archer && c.kind==ta::net::Cmd::Attack) {
                        if (first<0) {first=c.targetId;firstQueue=c.queue;}
                        else {second=c.targetId;secondQueue=c.queue;}
                    }
                check(first==enemy1 && second==enemy2 && firstQueue==0 && secondQueue==1,
                      "2.2 the archer attacks both enemies, nearest first, the second queued");
                check(count(cmds,mason,ta::net::Cmd::ReclaimArea)==1,"2.2 the mason (no attack order) clears the box");
                // An empty box with only an attacker: Normal, and nothing.
                selection_={archer};publish();
                const auto p=at(sword);
                ta::CursorId emptyCursor=ta::CursorId::Hourglass;
                const auto none=capture([&] {
                    dragTo(p.x-200,p.y-150,p.x-100,p.y-80,SDL_BUTTON_RIGHT);
                    emptyCursor=desiredCursor(tint);
                    event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,p.x-100,p.y-80);
                });
                check(emptyCursor==ta::CursorId::Normal && none.empty(),"2.2 an empty box with only an attacker: Normal, nothing");
            }
            // 2.2 step L: a lone weaponless transport loads the own units in the box.
            // Only where the sim admits them (TotalA.exe 0x489a90,
            // sim/retailtransport.h retailTransportCapacity: the passenger's footprint
            // x within the carrier's transportsize, fewer aboard than its capacity).
            selection_={carrier};publish();
            const bool fits=frameUnitP(sword) && frameUnitP(mason) && frameUnitP(carrier) &&
                canLoadPassenger(*frameUnitP(sword),*frameUnitP(carrier)) &&
                canLoadPassenger(*frameUnitP(mason),*frameUnitP(carrier));
            if (!fits)
                std::fprintf(stderr,"clicks: SKIP 2.2 step L and C4: %s admits neither passenger "
                             "(0x489a90)\n",transportType->id.c_str());
            if (fits) {
                const auto p1=at(sword),p2=at(mason);
                const float x0=std::min(p1.x,p2.x)-40,y0=std::min(p1.y,p2.y)-40,x1=std::max(p1.x,p2.x)+40,y1=std::max(p1.y,p2.y)+40;
                ta::CursorId boxCursor=ta::CursorId::Hourglass;
                const auto cmds=capture([&] {
                    dragTo(x0,y0,x1,y1,SDL_BUTTON_RIGHT);
                    boxCursor=desiredCursor(tint);
                    event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,x1,y1);
                });
                check(boxCursor==ta::CursorId::Load,"2.2 the box cursor for a lone transport: Load");
                check(count(cmds,sword,ta::net::Cmd::Load,carrier)==1 && count(cmds,mason,ta::net::Cmd::Load,carrier)==1,
                      "2.2 a lone transport picks up the units in the box");
            }
            // C5: a loaded transport moves on a right-click; Alt+right-click unloads.
            {
                auto& t=*writableWorld().unit(carrier);
                auto& p=*writableWorld().unit(sword);
                t.cargo.push_back(sword);p.inTransport=carrier;
                selection_={carrier};publish();
                const auto q=at(archer);
                const float gx=q.x-150,gy=q.y+120;
                auto cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_RIGHT);});
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::Move,"C5 a loaded transport moves on a right-click");
                SDL_SetModState(KMOD_LALT);
                cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_RIGHT);});
                SDL_SetModState(KMOD_NONE);
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::Unload,"C5 Alt+right-click unloads it");
                t.cargo.clear();p.inTransport=0;publish();
            }
            // P4 / C3: armed GUARD, a ground unit over an own flyer: Normal, nothing,
            // still armed; with a flyer selected too every unit guards.
            selection_={archer};pendingCmd_='g';publish();
            {
                const auto p=at(flyerA);
                event(SDL_MOUSEMOTION,0,p.x,p.y);
                check(desiredCursor(tint)==ta::CursorId::Normal,"C3 armed G, ground unit over a flyer: Normal");
                const auto cmds=capture([&] {clickAt(p.x,p.y,SDL_BUTTON_LEFT);});
                check(cmds.empty() && pendingCmd_=='g',"P4 / C3 the click does nothing and G stays armed");
                selection_={archer,flyerB};publish();
                event(SDL_MOUSEMOTION,0,p.x,p.y);
                check(desiredCursor(tint)==ta::CursorId::Defend,"C3 a flyer in the selection: Defend");
                const auto both=capture([&] {clickAt(p.x,p.y,SDL_BUTTON_LEFT);});
                check(count(both,archer,ta::net::Cmd::Guard,flyerA)==1 && count(both,flyerB,ta::net::Cmd::Guard,flyerA)==1 &&
                      pendingCmd_==0,"C3 every unit guards the flyer, and G disarms");
            }
            // C1: armed ATTACK over an own unit or ground fires at the click point
            // (Cmd::AttackGround) for each unit the resolver gives it; Attack cursor.
            selection_={archer};pendingCmd_='a';publish();
            {
                const auto p=at(mason);
                event(SDL_MOUSEMOTION,0,p.x,p.y);
                check(desiredCursor(tint)==ta::CursorId::Attack,"C1 armed A over an own unit: Attack");
                const auto cmds=capture([&] {clickAt(p.x,p.y,SDL_BUTTON_LEFT);});
                float wx=0,wz=0;pickWorld(p.x,p.y,wx,wz);
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::AttackGround && cmds[0].unitId==archer &&
                      cmds[0].targetId==0 && std::abs(cmds[0].x-wx)<2 && std::abs(cmds[0].z-wz)<2,
                      "C1 armed A over an own unit: the archer fires at the click point, not at the unit");
            }
            {
                // Ground with two armed kbots: each fires there (TA has no melee
                // weapon that would leave one out).
                const float gx=float(W)/2+40,gy=float(H)/2+200;
                float wx=0,wz=0;pickWorld(gx,gy,wx,wz);
                check(!liveWorld()->clearableBlockerAtPoint(wx,wz),"fixture: bare ground at the point");
                selection_={archer,sword};pendingCmd_='a';publish();
                event(SDL_MOUSEMOTION,0,gx,gy);
                check(desiredCursor(tint)==ta::CursorId::Attack,"C1 armed A on ground, Rocko + Peewee: Attack");
                auto cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_LEFT);});
                check(cmds.size()==2 && count(cmds,archer,ta::net::Cmd::AttackGround)==1 &&
                      count(cmds,sword,ta::net::Cmd::AttackGround)==1 &&
                      cmds[0].queue==ta::net::kQueueReplace && pendingCmd_==0,
                      "C1 armed A on ground: both fire at the point, plain replaces, A disarms");
                // Shift appends with the toggle; Ctrl goes to the front with it.
                selection_={archer};pendingCmd_='a';publish();
                SDL_SetModState(KMOD_LSHIFT);
                cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_LEFT);});
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::AttackGround &&
                      cmds[0].queue==(ta::net::kQueueAppend|ta::net::kQueueToggle) && pendingCmd_=='a',
                      "C1 Shift armed A on ground: appended, toggling, A stays armed");
                SDL_SetModState(KMOD_NONE);event(SDL_MOUSEMOTION,0,gx,gy);   // Shift let go: A disarms
                pendingCmd_='a';SDL_SetModState(KMOD_LCTRL);
                cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_LEFT);});
                check(cmds.size()==1 && cmds[0].queue==(ta::net::kQueueFront|ta::net::kQueueToggle),
                      "C1 Ctrl armed A on ground: at the front, toggling");
                SDL_SetModState(KMOD_NONE);
                // A Legion net game packs the click's run as one explicit order.
                const ta::sim::UnitType* gunship=nullptr;
                for (const auto& [id,t]:registry_.types())
                    if (t.canFly && t.canMove && !t.isBuilder && t.hasPrimaryWeaponBlock &&
                        ta::sim::World::suppressRule(t,0)==ta::sim::World::SuppressRule::Fire &&
                        (!gunship || id<gunship->id)) gunship=&t;
                check(gunship!=nullptr,"fixture: an armed flyer the resolver gives a point attack");
                const int g1=spawn(gunship->id,cx-200,cz-200,0,localPlayer_),g2=spawn(gunship->id,cx-260,cz-200,0,localPlayer_);
                selection_={archer,g1,g2};pendingCmd_='a';publish();
                event(SDL_MOUSEMOTION,0,gx,gy);
                check(desiredCursor(tint)==ta::CursorId::Attack,"C1 armed A on ground, archer + flyers: Attack");
                cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_LEFT);});
                uint32_t serial=1;
                const auto packed=ta::net::packClick(cmds,{},serial);
                check(cmds.size()==3 && count(cmds,g1,ta::net::Cmd::AttackGround)==1 &&
                      packed.size()==1 && packed[0].kind==ta::net::Cmd::Order &&
                      packed[0].order->members.size()==3 && packed[0].order->tmpl.kind==ta::net::Cmd::AttackGround,
                      "C1 armed A on ground with an archer and two flyers: one AttackGround order over all three ("+
                      gunship->id+")");
                for (int id:{g1,g2}) writableWorld().destroy(id);
                pendingCmd_=0;
            }
            // C1, rally holders (ours: retail strips a factory's canattack at load and
            // gives an Attack at a point no producer kind, so a factory gets nothing
            // there; a repeating builder's SUPPRESS yields to our rally rule, C9): a
            // factory sets a fight-move rally at the point and keeps producing. Applied,
            // not only captured. (TA's mobile builders build no units, so they have no
            // repeat and no rally.)
            {
                const ta::sim::UnitType* factoryType=nullptr;
                for (const auto& [id,t]:registry_.types())
                    if (t.producesUnits() && !t.hasPrimaryWeaponBlock && !registry_.buildable(id).empty() &&
                        id.rfind("arm",0)==0 && (!factoryType || id<factoryType->id)) factoryType=&t;
                check(factoryType!=nullptr,"fixture: an unarmed factory");
                const int factory=spawn(factoryType->id,cx+300,cz+60,0,localPlayer_);
                check(factory>=0,"fixture rally holder spawned ("+factoryType->id+")");
                publish();
                const float gx=float(W)/2+40,gy=float(H)/2+200;
                check(!hoveredUnitAt(ta::client::PointerArea::World,gx,gy),"fixture: no unit under the ground point");
                const auto apply=[&](const std::vector<ta::net::Command>& cmds) {
                    for (auto c:cmds) {c.player=uint8_t(localPlayer_);ta::sim::applyCommand(writableWorld(),registry_,c);}
                    publish();
                };
                const auto fightLeg=[&](int id) {
                    const auto& r=world_.unit(id)->rally;
                    return !r.empty() && r.back().attackMove && r.back().targetId==0;
                };
                // A factory alone: Attack cursor, one AttackMove, a fight leg on its rally.
                selection_={factory};pendingCmd_='a';publish();
                event(SDL_MOUSEMOTION,0,gx,gy);
                check(desiredCursor(tint)==ta::CursorId::Attack,"C1 armed A on ground, a factory alone: Attack");
                auto cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_LEFT);});
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::AttackMove && cmds[0].unitId==factory,
                      "C1 armed A on ground, a factory alone: exactly one AttackMove");
                apply(cmds);
                check(fightLeg(factory) && world_.unit(factory)->orders.empty(),
                      "C1 the factory's rally gains a fight leg; it takes no order of its own");
                // A factory and an archer: the archer fires at the point, the factory rallies.
                writableWorld().unit(factory)->rally.clear();
                selection_={factory,archer};pendingCmd_='a';publish();
                event(SDL_MOUSEMOTION,0,gx,gy);
                check(desiredCursor(tint)==ta::CursorId::Attack,"C1 armed A on ground, factory + archer: Attack");
                cmds=capture([&] {clickAt(gx,gy,SDL_BUTTON_LEFT);});
                check(cmds.size()==2 && count(cmds,archer,ta::net::Cmd::AttackGround)==1 &&
                      count(cmds,factory,ta::net::Cmd::AttackMove)==1,
                      "C1 armed A on ground, factory + archer: AttackGround for the archer, AttackMove for the factory");
                apply(cmds);
                check(fightLeg(factory),"C1 factory + archer: the factory's rally gains a fight leg");
                stopAll();
                // Over the factory's own footprint the factory is the hovered unit and
                // is left out (as in retail): nothing.
                writableWorld().unit(factory)->rally.clear();
                selection_={factory};pendingCmd_='a';publish();
                const auto f=at(factory);
                check(hoveredUnitAt(ta::client::PointerArea::World,f.x,f.y)==frameUnitP(factory),
                      "fixture: the pointer is over the factory");
                cmds=capture([&] {clickAt(f.x,f.y,SDL_BUTTON_LEFT);});
                check(cmds.empty() && world_.unit(factory)->rally.empty(),
                      "C1 armed A inside the factory's own footprint: no command");
                writableWorld().destroy(factory);
                selection_.clear();pendingCmd_=0;publish();
            }
            // 2.9 the armed ATTACK box: the enemies in it, no clear.
            selection_={archer,mason};pendingCmd_='a';publish();
            {
                const auto p1=at(enemy1),p2=at(enemy2);
                const float x0=std::min(p1.x,p2.x)-40,y0=std::min(p1.y,p2.y)-40,x1=std::max(p1.x,p2.x)+40,y1=std::max(p1.y,p2.y)+40;
                ta::CursorId boxCursor=ta::CursorId::Hourglass;
                const auto cmds=capture([&] {
                    dragTo(x0,y0,x1,y1,SDL_BUTTON_LEFT);
                    boxCursor=desiredCursor(tint);
                    event(SDL_MOUSEBUTTONUP,SDL_BUTTON_LEFT,x1,y1);
                });
                check(boxCursor==ta::CursorId::Attack,"2.9 the armed A box cursor: Attack");
                check(count(cmds,archer,ta::net::Cmd::Attack)==2 && count(cmds,mason,ta::net::Cmd::ReclaimArea)==0 &&
                      pendingCmd_==0,"2.9 the armed A box attacks both, the mason does not clear, A disarms");
                check(selection_==std::vector<int>({archer,mason}),"2.9 the armed box leaves the selection");
            }
            // 2.3 armed MOVE over a damaged own unit: the mason repairs it.
            selection_={mason};pendingCmd_='m';
            writableWorld().unit(sword)->hp=ta::sim::Fixed::fromInt(10);publish();
            {
                const auto p=at(sword);
                event(SDL_MOUSEMOTION,0,p.x,p.y);
                check(desiredCursor(tint)==ta::CursorId::Repair,"2.3 armed M over a damaged unit with a mason: Repair");
                const auto cmds=capture([&] {clickAt(p.x,p.y,SDL_BUTTON_LEFT);});
                check(count(cmds,mason,ta::net::Cmd::Repair,sword)==1,"2.3 armed M over a damaged unit: the mason repairs it");
            }
            // 2.10 the minimap: a right-click on an enemy dot attacks it, the camera stays.
            selection_={archer};pendingCmd_=0;publish();
            {
                // In view coordinates, as this test's clicks are (the minimap is left of the view).
                SDL_FRect r=minimapRect(W,H);r.x-=float(viewX0());r.y-=float(viewY0());
                const float mapW=float(mapView_.map().blocksX)*32,mapH=float(mapView_.map().blocksY)*32;
                const auto* e=frameUnitP(enemy2);
                const float mx=r.x+e->x/mapW*r.w,my=r.y+e->z/mapH*r.h;
                const float ox=mapView_.offX();
                auto cmds=capture([&] {clickAt(mx,my,SDL_BUTTON_RIGHT);});
                check(mapView_.offX()==ox,"2.10 a right press on the minimap leaves the camera");
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::Attack,"2.10 a minimap right-click on an enemy dot attacks it");
                cmds=capture([&] {clickAt(r.x+2,r.y+r.h-2,SDL_BUTTON_RIGHT);});
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::Move,"2.10 a minimap right-click on ground moves");
                pendingCmd_='a';
                cmds=capture([&] {clickAt(mx,my,SDL_BUTTON_LEFT);});
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::Attack && mapView_.offX()==ox,
                      "2.10 armed A on an enemy dot attacks it");
                pendingCmd_='a';
                // The event carries whole pixels: the point is where the truncated press lands
                // (the minimap rect need not sit on whole pixels).
                const float cornerX=std::floor(r.x+2),cornerY=std::floor(r.y+r.h-2);
                float gx=0,gz=0;minimapToWorld(cornerX+float(viewX0()),cornerY+float(viewY0()),W,H,gx,gz);   // r is in view coordinates
                cmds=capture([&] {clickAt(cornerX,cornerY,SDL_BUTTON_LEFT);});
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::AttackGround && std::abs(cmds[0].x-gx)<2 && std::abs(cmds[0].z-gz)<2 &&
                      mapView_.offX()==ox,"2.10 / C1 armed A on minimap ground fires at that point");
                // C2b placement: the mason places a building at a valid minimap point.
                const ta::sim::UnitType* site=nullptr;
                for (const auto& id:registry_.buildable(masonType->id))
                    if (const auto* t=registry_.find(id); t && t->isStructure()) {site=t;break;}
                check(site!=nullptr,"fixture: the mason builds a structure");
                selection_={mason};pendingCmd_=0;placing_=site;publish();
                float px=-1,py=-1;bool sited=false;   // view coordinates, as r
                for (float fy=0.3f;fy<0.8f && !sited;fy+=0.05f)
                    for (float fx=0.3f;fx<0.8f;fx+=0.05f) {
                        float wx,wz;
                        const float qx=std::floor(r.x+r.w*fx),qy=std::floor(r.y+r.h*fy);   // whole pixels, as the event carries
                        if (!minimapToWorld(qx+float(viewX0()),qy+float(viewY0()),W,H,wx,wz) || !canPlaceLocked(site,wx,wz)) continue;
                        px=qx;py=qy;sited=true;break;
                    }
                check(sited,"fixture: a valid minimap site");
                event(SDL_MOUSEMOTION,0,px,py);
                check(desiredCursor(tint)==ta::CursorId::FindSite,"C2 placement over a valid minimap point: FindSite");
                cmds=capture([&] {clickAt(px,py,SDL_BUTTON_LEFT);});
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::Build && !placing_ && mapView_.offX()==ox,
                      "C2 a minimap click places the building there; the camera stays");
                placing_=nullptr;
            }
            // C4: TotalA.exe has no boarding order (LOAD is the carrier's,
            // 0x43f701): a unit right-clicking its own transport does not board it.
            selection_={sword};publish();
            if (fits) {
                const auto p=at(carrier);
                const auto cmds=capture([&] {clickAt(p.x,p.y,SDL_BUTTON_RIGHT);});
                check(std::none_of(cmds.begin(),cmds.end(),[](const ta::net::Command& c){return c.kind==ta::net::Cmd::Load;}),
                      "C4 a passenger right-clicking its own transport does not board it");
            }
            // P1 / 2.11: the click picks the smallest unit under the pointer, here a
            // swordsman standing in the middle of a keep.
            {
                const int keep=spawn("armlab",cx+250,cz-120,0,localPlayer_);
                check(keep>=0,"fixture kbot lab spawned");
                writableWorld().unit(sword)->x=writableWorld().unit(keep)->x;
                writableWorld().unit(sword)->z=writableWorld().unit(keep)->z;
                publish();
                const auto p=at(sword);
                float dk=0,ds=0,sk=0,ss=0;
                const bool overK=unitUnderCursor(*frameUnitP(keep),p.x,p.y,&dk,&sk);
                const bool overS=unitUnderCursor(*frameUnitP(sword),p.x,p.y,&ds,&ss);
                check(overK && overS && ss<sk,"fixture: the pointer is over both, the swordsman smaller");
                check(hoveredUnitAt(ta::client::PointerArea::World,p.x,p.y)==frameUnitP(sword),
                      "P1 the hovered unit is the smallest under the pointer");
                selection_.clear();publish();
                clickAt(p.x,p.y,SDL_BUTTON_LEFT);
                check(selection_==std::vector<int>{sword},"P1 a click over overlapping units selects the smallest");
            }
            // 2.11 / C6 / C7: left-click select rules.
            selection_={archer};publish();
            {
                const auto e=at(enemy1);
                clickAt(e.x,e.y,SDL_BUTTON_LEFT);
                check(selection_.empty(),"C6 a left-click on an enemy with a selection deselects all");
                clickAt(e.x,e.y,SDL_BUTTON_LEFT);
                check(selection_==std::vector<int>{enemy1},"C6 with nothing of ours selected it is selected to inspect");
                const auto a=at(archer);
                selection_={archer,mason};publish();
                SDL_SetModState(KMOD_LSHIFT);
                clickAt(a.x,a.y,SDL_BUTTON_LEFT);
                SDL_SetModState(KMOD_NONE);
                check(selection_==std::vector<int>{mason},"C7 Shift-click on a selected unit removes it");
            }
            SDL_SetModState(savedMod);
            std::fprintf(stderr,"PASS: clicks all\n");
            return;
        }
        // Clear orders go to EVERY selected reclaimer (retail 5214b0 resolves each
        // selected unit through 4de530): a right-drag box, a right-click on a feature
        // and the armed CLEAR click each give the three builders one command apiece,
        // the soldier none (its queue untouched). Also: a map feature the sim removed
        // at load (an earlier overlapped one) is not drawn or pickable.
        if (ta::devFlag("TA_RECLAIM_FANOUT_TEST")) {
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("reclaim fan-out: "+what);
                std::fprintf(stderr,"PASS: reclaim fan-out: %s\n",what.c_str());
            };
            constexpr int W=1000,H=700;
            winW_=W;winH_=H;noFog_=true;edgeScrollOn_=false;uiScale_=1.0f;
            writableWorld().setPlayerCount(2);
            // Ghosts: every raw anchor the install dropped has no drawn instance.
            {
                const auto& map=mapView_.map();
                const auto& installed=world_.installedMapFeatures();
                check(installed.size()==map.features.size(),"the sim kept its installed feature plane");
                size_t ghosts=0,drawn=0;
                for (int cz=0;cz<map.height;++cz)
                    for (int cx=0;cx<map.width;++cx) {
                        const size_t i=size_t(cz)*map.width+cx;
                        if (map.features[i]>=map.featureNames.size() || installed[i]==map.features[i]) continue;
                        ++ghosts;
                        for (const auto& f:features_)
                            if (int(std::floor((f.x-float(f.fx*8))/16))==cx &&
                                int(std::floor((f.z-float(f.fz*8))/16))==cz) ++drawn;
                    }
                std::fprintf(stderr,"reclaim fan-out: %zu removed map features, %zu drawn\n",ghosts,drawn);
                check(drawn==0,"no feature removed at load is drawn ("+std::to_string(ghosts)+" removed)");
            }
            const ta::sim::UnitType* builderType=nullptr;
            for (const char* id:{"armck","armcom"})
                if (const auto* t=registry_.find(id); t && t->isBuilder && t->canMove && t->canReclaim) {builderType=t;break;}
            check(builderType!=nullptr,"fixture has a mobile reclaimer type");
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            std::vector<int> builders;
            for (int i=0;i<3;++i) builders.push_back(spawn(builderType->id,cx+float(i)*48,cz,0,localPlayer_));
            const int soldier=spawn("armpw",cx-150,cz-100,0,localPlayer_);
            for (int id:builders) check(id>=0 && world_.unit(id),"builder spawned");
            check(soldier>=0 && world_.unit(soldier) && !world_.unit(soldier)->type->canReclaim,"soldier spawned, not a reclaimer");
            mapView_.setZoom(1);mapView_.setOffset(cx-400,cz-300);
            const auto publish=[&] {captureFrame();beginFrame();cosmeticStep(0);};
            const auto savedMod=SDL_GetModState();SDL_SetModState(KMOD_NONE);
            const auto reset=[&] {
                for (int id:builders) {writableWorld().stop(id);writableWorld().unit(id)->orders.clear();}
                writableWorld().stop(soldier);
                ta::net::Command m;m.kind=ta::net::Cmd::Move;m.unitId=soldier;m.player=uint8_t(localPlayer_);
                m.x=cx-400;m.z=cz-300;apply(m);   // a queue the clear must not touch
                selection_=builders;selection_.push_back(soldier);publish();
            };
            const auto soldierKept=[&] {
                const auto& o=world_.unit(soldier)->orders;
                return o.size()==1 && o.front().goal && !o.front().reclaimArea;
            };
            // Every builder got exactly one command of `kind`; the soldier got none.
            const auto fanned=[&](const std::vector<ta::net::Command>& cmds,ta::net::Cmd kind) {
                if (cmds.size()!=builders.size()) return false;
                for (int id:builders) {
                    int n=0;
                    for (const auto& c:cmds) if (c.unitId==id && c.kind==kind) ++n;
                    if (n!=1) return false;
                }
                for (const auto& c:cmds) if (c.unitId==soldier) return false;
                return true;
            };
            const auto event=[&](uint32_t kind,uint8_t button,float x,float y) {
                SDL_Event e{};e.type=kind;
                if (kind==SDL_MOUSEMOTION) {e.motion.x=int(x);e.motion.y=int(y);}
                else {e.button.button=button;e.button.x=int(x);e.button.y=int(y);}
                inputFromView(e,W,H);   // a point in the world view
            };
            // Right-drag box through the real input().
            reset();
            std::vector<ta::net::Command> cmds;
            {
                ClickScope outer(*this);
                event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_RIGHT,300,200);
                event(SDL_MOUSEMOTION,0,500,380);
                event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,500,380);
                cmds=click_->cmds;
            }
            std::fprintf(stderr,"reclaim fan-out: box issued %zu commands\n",cmds.size());
            check(fanned(cmds,ta::net::Cmd::ReclaimArea),"a box gives each of 3 reclaimers one ReclaimArea, the soldier none");
            check(cmds[0].x==cmds[1].x && cmds[0].z2==cmds[2].z2 && !cmds[0].queue,"every builder clears the same box, unqueued");
            for (int id:builders) {
                const auto& o=world_.unit(id)->orders;
                check(!o.empty() && o.front().reclaimArea,"builder "+std::to_string(id)+" holds the area order");
            }
            check(soldierKept(),"the soldier's own order is untouched");
            // Shift queues per unit.
            reset();
            for (int id:builders) {
                ta::net::Command m;m.kind=ta::net::Cmd::Move;m.unitId=id;m.player=uint8_t(localPlayer_);
                m.x=cx;m.z=cz+200;apply(m);
            }
            publish();
            SDL_SetModState(KMOD_LSHIFT);
            {
                ClickScope outer(*this);
                event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_RIGHT,300,200);
                event(SDL_MOUSEMOTION,0,500,380);
                event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,500,380);
                cmds=click_->cmds;
            }
            SDL_SetModState(KMOD_NONE);
            check(fanned(cmds,ta::net::Cmd::ReclaimArea) && cmds[0].queue && cmds[2].queue,"Shift-box queues one ReclaimArea per builder");
            for (int id:builders) {
                const auto& o=world_.unit(id)->orders;
                check(o.size()>=2 && !o.front().reclaimArea && o.back().reclaimArea,
                      "builder "+std::to_string(id)+" keeps its move and queues the clear");
            }
            check(soldierKept(),"Shift-box leaves the soldier's order alone");
            // Single feature: the nearest reclaimable, pickable map feature on the map.
            const FeatureInst* target=nullptr;float best=1e30f;
            for (const auto& f:features_) {
                if (!f.hasSim || !f.aliveVis || !f.reclaimable) continue;
                const float d=(f.x-cx)*(f.x-cx)+(f.z-cz)*(f.z-cz);
                if (d<best) {best=d;target=&f;}
            }
            check(target!=nullptr,"the map has a reclaimable feature");
            const float fx=target->x,fz=target->z;const int fid=target->simId;
            check(reclaimTargetAt(fx,fz)==fid,"the feature picks under its own centre");
            mapView_.setOffset(fx-400,fz-300);
            reset();
            renderStudyMouse(-1000,-1000);   // no unit under the cursor: not an attack
            {
                ClickScope outer(*this);
                rightClickOrder(fx,fz,false);
                cmds=click_->cmds;
            }
            {
                // Retail's mode 1 (0x4de530): a unit that can neither raise nor reclaim
                // moves to the point.
                std::vector<ta::net::Command> reclaims;
                int soldierMoves=0;
                for (const auto& c:cmds) {
                    if (c.unitId==soldier) soldierMoves+=c.kind==ta::net::Cmd::Move;
                    else reclaims.push_back(c);
                }
                check(fanned(reclaims,ta::net::Cmd::Reclaim) && reclaims[0].targetId==fid && reclaims[2].targetId==fid &&
                      soldierMoves==1 && cmds.size()==builders.size()+1,
                      "a right-click on a feature gives each reclaimer one Reclaim of it, the soldier a Move there");
            }
            reset();
            {
                ClickScope outer(*this);
                issueArmedOrder('c',fx,fz,false,false);
                cmds=click_->cmds;
            }
            check(fanned(cmds,ta::net::Cmd::Reclaim) && cmds[1].targetId==fid,
                  "an armed CLEAR click gives each reclaimer one Reclaim, the soldier none");
            check(soldierKept(),"armed CLEAR leaves the soldier's order alone");
            SDL_SetModState(savedMod);
            std::fprintf(stderr,"PASS: reclaim fan-out to every selected reclaimer\n");
            return;
        }
        if (ta::devFlag("TA_DEATH_BLAST_TEST")) {
            // A Commander and a Peewee self-destruct through the real key path:
            // each death weapon (SELFDESTRUCTAS: COMMANDER_BLAST's commboom, the
            // Peewee's own) plays its explosiongaf/explosionart (TotalA.exe
            // 0x499eb0 -> 0x49a00e), and the Killed script's SHATTER fragments
            // fall back to the ground (0x420f81).
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("death blast: "+what);
                std::fprintf(stderr,"PASS: death blast: %s\n",what.c_str());
            };
            noFog_=true;
            const auto& m=mapView_.map();
            float lx=-1,lz=-1;   // dry land near the middle
            for(int r=0;r<m.width/2 && lx<0;r+=4)for(int dz=-r;dz<=r && lx<0;dz+=4)for(int dx=-r;dx<=r && lx<0;dx+=4) {
                const float x=float(m.width/2+dx)*16+8,z=float(m.height/2+dz)*16+8;
                if(x>200 && z>200 && x<float(m.width)*16-200 && z<float(m.height)*16-200 &&
                   !worldTables().isWater(x,z) && !worldTables().isWater(x+120,z))
                    {lx=x;lz=z;}
            }
            check(lx>0,"fixture found dry land");
            for(const char* name:{"armcom","armpw"}) {
                const int unit=spawn(name,name[3]=='c' ? lx : lx+120,lz,0,localPlayer_);
                check(world_.unit(unit)!=nullptr,std::string("fixture has ")+name);
                const auto* type=world_.unit(unit)->type;
                const auto& weapon=type->hasSelfDestructAs ? type->selfDestructAs : type->explodeAs;
                const EffectAnim* want=effectFor(weapon.explosionGaf+":"+weapon.explosionArt,"palettes/palette.pal");
                check(want!=nullptr,std::string(name)+": its death weapon "+weapon.section+" has art "+
                      weapon.explosionGaf+":"+weapon.explosionArt);
                selection_={unit};captureFrame();beginFrame();cosmeticStep(0);
                handleKey(SDLK_d,KMOD_CTRL|KMOD_SHIFT);
                bool blast=false;size_t fragments=0;
                for(int i=0;i<30*20;++i) {
                    writableWorld().tick(1.f/30);
                    captureFrame();beginFrame();cosmeticStep(1.f/30);
                    for(const auto& e:effects_)blast|=e.anim==want;
                    size_t now=0;
                    for(const auto& d:detachedPieces_)now+=d.motion.shatter;
                    fragments=std::max(fragments,now);
                }
                check(blast,std::string(name)+": the death weapon's explosion played");
                size_t left=0;
                for(const auto& d:detachedPieces_)left+=d.motion.shatter;
                std::fprintf(stderr,"%s: most fragments %zu, left after 20 s %zu, debris %zu\n",
                             name,fragments,left,detachedPieces_.size());
                check(left==0,std::string(name)+": every SHATTER fragment came down");
                // Every shipped non-SHATTER debris piece has FALL: down well inside 20 s.
                check(detachedPieces_.empty(),std::string(name)+": every FALL piece came down");
            }
            return;
        }
        if (ta::devFlag("TA_SELFDESTRUCT_NOTICE_TEST")) {
            // Ctrl+Shift+D through the real key path: the notice reads the unit's
            // own countdown (selfdestructcountdown, 2 s) and counts it down with
            // the sim, then lets go once the unit has left.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("self-destruct notice: "+what);
                std::fprintf(stderr,"PASS: self-destruct notice: %s\n",what.c_str());
            };
            noFog_=true;
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int unit=spawn("armpw",cx,cz,0,localPlayer_);
            check(world_.unit(unit)!=nullptr,"fixture has a Peewee");
            const int countdown=world_.unit(unit)->type->selfDestructCountdown;
            const auto step=[&](int ticks) {
                for (int i=0;i<ticks;++i) {
                    writableWorld().tick(1.f/30);
                    captureFrame();beginFrame();cosmeticStep(1.f/30);
                }
            };
            selection_={unit};captureFrame();beginFrame();cosmeticStep(0);
            notice_.clear();
            handleKey(SDLK_d,KMOD_CTRL|KMOD_SHIFT);
            const std::string first="SELF-DESTRUCT IN "+std::to_string(countdown)+"...";
            check(notice_==first,"the press reads the type's countdown ("+notice_+")");
            step(5);
            check(world_.unit(unit)->selfDestructT>=0,"the order armed the unit");
            check(notice_==first,"just armed it still reads "+std::to_string(countdown)+" ("+notice_+")");
            step(countdown*30-15);   // under a second left
            check(notice_=="SELF-DESTRUCT IN 1...","with under a second left it reads 1 ("+notice_+")");
            check(noticeTimer_>0,"the notice stays up while the countdown runs");
            step(60);
            check(!world_.unit(unit) || !world_.unit(unit)->alive() || world_.unit(unit)->selfDestructT<0,
                  "the countdown ended");
            check(selfDestructWatch_.empty(),"the notice lets go of the unit");
            std::fprintf(stderr,"PASS: self-destruct notice follows the countdown\n");
            return;
        }
        if (ta::devFlag("TA_METAL_SNAP_TEST")) {
            // Metal extractors on a map with deposits (client/metalspots.h), through the
            // real input path: a click near a deposit builds on the deposit, a click with
            // none in reach builds nothing, and a box queues one on every free deposit
            // inside it, each on its spot.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("metal snap: "+what);
                std::fprintf(stderr,"PASS: metal snap: %s\n",what.c_str());
            };
            noFog_=true;edgeScrollOn_=false;
            constexpr int W=1280,H=960;
            winW_=W;winH_=H;
            const auto* mex=registry_.find("armmex");
            check(mex && mex->extractsMetal>0,"armmex extracts metal");
            check(snapsToMetal(mex),"Ashap Plateau has deposits: no metal planet");
            const auto spots=metalSpots(mex);
            check(spots.size()>=4,"its deposits are spots ("+std::to_string(spots.size())+")");
            const auto& s0=spots[spots.size()/2];
            const int me=localPlayer_;
            const int com=spawn("armcom",s0.x+64,s0.z+64,0,me);
            check(com>=0,"fixture commander spawned");
            for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
            const auto frame=[&] {captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);endFrame();};
            const auto mouse=[&](Uint32 type,float x,float y,Uint8 button) {
                SDL_Event e{};e.type=type;
                if (type==SDL_MOUSEMOTION) {e.motion.x=int(x);e.motion.y=int(y);}
                else {e.button.button=button;e.button.x=int(x);e.button.y=int(y);e.button.clicks=1;}
                input(e,W,H);
            };
            const auto mexOrders=[&] {
                std::vector<std::pair<float,float>> out;
                if (const auto* u=world_.unit(com))
                    for (const auto& o:u->orders) if (o.buildType==mex) out.push_back({o.buildX.toFloat(),o.buildZ.toFloat()});   // the site, not the approach
                return out;
            };
            const auto onSpot=[&](float x,float z) {
                for (const auto& s:spots) if (std::fabs(s.x-x)<0.5f && std::fabs(s.z-z)<0.5f) return true;
                return false;
            };
            mapView_.setZoom(1.0f);
            const float vw=float(mapViewW(W)),vh=float(mapViewH(H));
            mapView_.setOffset(s0.x-vw/2,s0.z-vh/2);
            selection_={com};
            frame();frame();
            // A click 30 px off the deposit.
            placing_=mex;
            mouse(SDL_MOUSEMOTION,vw/2+30,vh/2+20,0);frame();
            mouse(SDL_MOUSEBUTTONDOWN,vw/2+30,vh/2+20,SDL_BUTTON_LEFT);
            mouse(SDL_MOUSEBUTTONUP,vw/2+30,vh/2+20,SDL_BUTTON_LEFT);
            frame();
            auto o=mexOrders();
            check(o.size()==1,"a click near a deposit queues one extractor ("+std::to_string(o.size())+")");
            check(onSpot(o[0].first,o[0].second) && std::hypot(o[0].first-s0.x,o[0].second-s0.z)<1.f,
                  "on the deposit, snapped: order "+std::to_string(o[0].first)+","+std::to_string(o[0].second)+
                  " spot "+std::to_string(s0.x)+","+std::to_string(s0.z));
            check(!placing_,"and disarms (no Shift)");
            // A click with no deposit in reach (if the map has such ground on screen).
            {
                float fx=-1,fy=-1;
                for (float y=40;y<vh-40 && fx<0;y+=40)
                    for (float x=40;x<vw-40;x+=40) {
                        float wx,wz;pickWorld(x,y,wx,wz);
                        if (!ta::client::metalspots::nearest(spots,wx,wz,ta::client::metalspots::kSnapRadius+32)) {fx=x;fy=y;break;}
                    }
                if (fx>=0) {
                    placing_=mex;
                    SDL_SetModState(KMOD_SHIFT);
                    mouse(SDL_MOUSEBUTTONDOWN,fx,fy,SDL_BUTTON_LEFT);
                    mouse(SDL_MOUSEBUTTONUP,fx,fy,SDL_BUTTON_LEFT);
                    SDL_SetModState(KMOD_NONE);
                    frame();
                    check(mexOrders().size()==1,"a click with no deposit in reach builds nothing");
                    placing_=nullptr;
                }
            }
            // A box over the whole view: one on every free deposit inside, appended (Shift).
            mapView_.setZoom(0.5f);
            mapView_.setOffset(s0.x-vw,s0.z-vh);
            frame();
            float bx0,bz0,bx1,bz1;
            pickWorld(10,10,bx0,bz0);pickWorld(vw-10,vh-10,bx1,bz1);
            placing_=mex;
            const size_t want=metalBoxSpots(bx0,bz0,bx1,bz1).size();
            check(want>=2,"the box catches free deposits ("+std::to_string(want)+")");
            SDL_SetModState(KMOD_SHIFT);
            mouse(SDL_MOUSEBUTTONDOWN,10,10,SDL_BUTTON_LEFT);
            mouse(SDL_MOUSEMOTION,vw-10,vh-10,0);frame();
            if (const char* cap=ta::devEnv("TA_METAL_SNAP_CAPTURE")) {   // the box mid-drag
                captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);
                std::vector<uint8_t> px(size_t(W)*H*4);
                const SDL_Rect r{0,0,W,H};
                if (SDL_RenderReadPixels(ren_,&r,SDL_PIXELFORMAT_RGBA32,px.data(),W*4)==0) ta::png::write(cap,W,H,px);
                endFrame();
            }
            mouse(SDL_MOUSEBUTTONUP,vw-10,vh-10,SDL_BUTTON_LEFT);
            frame();
            o=mexOrders();
            check(o.size()>=want,"the box queues one per deposit ("+std::to_string(o.size())+" for "+std::to_string(want)+" + 1)");
            bool all=true;for (const auto& [x,z]:o) all=all&&onSpot(x,z);
            check(all,"every one on a deposit");
            check(placing_==mex,"Shift keeps the extractor armed");
            SDL_SetModState(KMOD_NONE);
            placing_=nullptr;
            std::fprintf(stderr,"PASS: metal snap build\n");
            return;
        }
        if (ta::devFlag("TA_FACTORY_MENU_TEST")) {
            // A factory's build page queues production (TotalA.exe 0x41aa00 ->
            // 0x419b00): a click on a mobile unit's picture is a Train order to the
            // factory, not a placement; left +1, right -1, Shift five. A building's
            // picture (bmcode 0, 0x41ab7f) still arms the placement. Through the real
            // input path, then the lab builds the Peewee and it rolls out.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("factory menu: "+what);
                std::fprintf(stderr,"PASS: factory menu: %s\n",what.c_str());
            };
            noFog_=true;edgeScrollOn_=false;
            constexpr int W=640,H=480;
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int me=localPlayer_;
            const int lab=spawn("armlab",cx,cz,0,me);
            const int com=spawn("armcom",cx-200,cz+150,0,me);
            check(lab>=0 && com>=0,"fixture spawned");
            for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
            const auto frame=[&] {captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);endFrame();};
            const auto mouse=[&](Uint32 type,float x,float y,Uint8 button) {
                SDL_Event e{};e.type=type;
                if (type==SDL_MOUSEMOTION) {e.motion.x=int(x);e.motion.y=int(y);}
                else {e.button.button=button;e.button.x=int(x);e.button.y=int(y);e.button.clicks=1;}
                input(e,W,H);
            };
            const auto clickAt=[&](float x,float y,Uint8 button) {
                mouse(SDL_MOUSEMOTION,x,y,0);mouse(SDL_MOUSEBUTTONDOWN,x,y,button);mouse(SDL_MOUSEBUTTONUP,x,y,button);frame();
            };
            // The build row's picture of `name` on screen (drawPanel's rectangles).
            const auto buttonAt=[&](const std::string& name) -> std::optional<SDL_FPoint> {
                for (const auto& [r,bt]:iconRects_)
                    if (bt && SDL_strcasecmp(bt->id.c_str(),name.c_str())==0) return SDL_FPoint{r.x+r.w/2,r.y+r.h/2};
                return std::nullopt;
            };
            const auto queued=[&](const char* id) {
                int n=0;
                if (const auto* u=world_.unit(lab)) for (const auto* t:u->buildQueue) n+=t && t->id==id;
                return n;
            };
            mapView_.setZoom(1.0f);
            selection_={lab};
            frame();frame();
            check(!iconRects_.empty() && buildMenuRect_.w>0,"the lab's build row is on show");
            if (const char* cap=ta::devEnv("TA_FACTORY_MENU_CAPTURE")) {   // the 640x480 frame, the lab selected
                captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);
                std::vector<uint8_t> px(size_t(W)*H*4);
                const SDL_Rect r{0,0,W,H};
                if (SDL_RenderReadPixels(ren_,&r,SDL_PIXELFORMAT_RGBA32,px.data(),W*4)==0) ta::png::write(cap,W,H,px);
                endFrame();
            }
            const auto pw=buttonAt("ARMPW");
            check(pw.has_value(),"the page has the Peewee's picture");
            clickAt(pw->x,pw->y,SDL_BUTTON_LEFT);
            check(!placing_,"a left click on it arms no placement");
            check(queued("armpw")==1,"it queues one Peewee in the lab: "+std::to_string(queued("armpw")));
            SDL_SetModState(KMOD_SHIFT);
            clickAt(pw->x,pw->y,SDL_BUTTON_LEFT);
            SDL_SetModState(KMOD_NONE);
            check(queued("armpw")==6,"Shift+click queues five more: "+std::to_string(queued("armpw")));
            clickAt(pw->x,pw->y,SDL_BUTTON_RIGHT);
            check(queued("armpw")==5,"a right click takes one off: "+std::to_string(queued("armpw")));
            SDL_SetModState(KMOD_SHIFT);
            clickAt(pw->x,pw->y,SDL_BUTTON_RIGHT);
            SDL_SetModState(KMOD_NONE);
            check(queued("armpw")==0,"Shift+right click takes five off");
            check(selection_==std::vector<int>{lab},"the clicks leave the selection");
            clickAt(pw->x,pw->y,SDL_BUTTON_LEFT);
            int built=-1;
            for (int t=0;t<30*120 && built<0;++t) {
                writableWorld().tick(1.f/30);
                for (const auto& u:world_.units())
                    if (u.alive() && u.type && u.type->id=="armpw" && u.player==me && !u.underConstruction) {built=u.id;break;}
            }
            check(built>=0,"the lab builds the Peewee");
            // A mobile builder's page: a building's picture arms the placement.
            selection_={com};
            frame();frame();
            const auto solar=buttonAt("ARMSOLAR");
            check(solar.has_value(),"the Commander's page has the solar collector");
            clickAt(solar->x,solar->y,SDL_BUTTON_LEFT);
            check(placing_ && placing_->id=="armsolar","a building's picture arms its placement");
            placing_=nullptr;
            std::fprintf(stderr,"PASS: factory build menu\n");
            return;
        }
        if (ta::devFlag("TA_IDLE_BUILDERS_TEST")) {
            // IDLE BUILDERS: one portrait per type of idle builder or factory of the local
            // player, a click selects the next one and goes to it, Shift selects them all,
            // TOP / MIDDLE / BOTTOM stack in that order, OFF and a watcher show none.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("idle builders: "+what);
                std::fprintf(stderr,"PASS: idle builders: %s\n",what.c_str());
            };
            noFog_=true;edgeScrollOn_=false;
            constexpr int W=1280,H=960;
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int me=localPlayer_;
            const int commander=spawn("armcom",cx-200,cz,0,me);
            const int idleFactory=spawn("armlab",cx+100,cz-150,0,me);
            const int busyFactory=spawn("armlab",cx+100,cz+150,0,me);
            const int other=spawn("corcom",cx-200,cz+200,0,(me+1)%2);
            const int worker=spawn("armck",cx-300,cz-100,0,me);   // a mobile builder, not a commander
            check(commander>=0 && idleFactory>=0 && busyFactory>=0 && other>=0 && worker>=0,"fixture spawned");
            const auto& menu=registry_.buildable("armlab");
            check(!menu.empty() && registry_.find(menu.front()),"the kbot lab has something to build");
            writableWorld().train(busyFactory,registry_.find(menu.front()),5);
            for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
            const auto frame=[&] {captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);endFrame();};
            mapView_.setZoom(1.0f);
            selection_.clear();idleBuilders_=2;
            frame();frame();
            const auto iconOf=[&](const char* id) -> const IdleBuilderIcon* {
                for (const auto& i:idleRects_) if (i.type && i.type->id==id) return &i;
                return nullptr;
            };
            check(idleRects_.size()>=3,"portraits for the commander, the worker and the idle lab: "+std::to_string(idleRects_.size()));
            const auto has=[](const IdleBuilderIcon* i,int id){return i && std::find(i->ids.begin(),i->ids.end(),id)!=i->ids.end();};
            check(has(iconOf("armcom"),commander) && !has(iconOf("armcom"),other),"the commander is in, the other player's is not");
            check(iconOf("armlab") && iconOf("armlab")->ids==std::vector<int>{idleFactory},"only the idle lab, not the building one");
            // The commander, then the mobile builders, then the factories; cost rising within each.
            check(idleRects_.front().type->commander,"the commander comes first");
            check(iconOf("armck") && !idleRects_.back().type->commander && idleRects_.back().type->isStructure(),
                  "factories come last");
            const auto rank=[](const ta::sim::UnitType* t){return t->commander ? 0 : t->isStructure() ? 2 : 1;};
            bool ordered=true;
            for (size_t i=1;i<idleRects_.size();++i) {
                const auto *a=idleRects_[i-1].type,*b=idleRects_[i].type;
                if (rank(a)>rank(b) || (rank(a)==rank(b) && a->buildCostMetal>b->buildCostMetal)) ordered=false;
            }
            check(ordered,"commander, mobile builders, factories, each basic (cheap) to advanced");
            const SDL_FRect mid=idleRects_.front().r;
            check(mid.x+mid.w<=float(mapViewW(W)) && mid.x>float(mapViewW(W))/2,"the column sits at the right of the play area");
            const std::vector<int> kings=iconOf("armcom")->ids;
            check(idleBuilderClick(mid.x+mid.w/2,mid.y+mid.h/2,true) && selection_==std::vector<int>{kings.front()},
                  "a click selects the first idle commander");
            if (kings.size()>1)
                check(idleBuilderClick(mid.x+mid.w/2,mid.y+mid.h/2,true) && selection_==std::vector<int>{kings[1]},
                      "the next click the next one");
            const SDL_FRect castle=iconOf("armlab")->r;
            SDL_SetModState(KMOD_SHIFT);
            check(idleBuilderClick(castle.x+castle.w/2,castle.y+castle.h/2,true) && selection_==std::vector<int>{idleFactory},
                  "Shift+click selects every idle lab");
            SDL_SetModState(KMOD_NONE);
            check(!idleBuilderClick(mid.x-50,mid.y+mid.h/2,true),"a click beside the column is not taken");
            const float smallW=mid.w;
            idleBuildersScale_=1.5f;frame();
            check(idleRects_.front().r.w>smallW*1.2f,"IDLE BUILDERS SCALE enlarges the portraits");
            idleBuildersScale_=0.5f;frame();
            idleBuilders_=1;frame();const float top=idleRects_.front().r.y;
            idleBuilders_=3;frame();const float bottom=idleRects_.front().r.y;
            check(top<mid.y && mid.y<bottom,"TOP, MIDDLE and BOTTOM stack in that order");
            idleBuilders_=0;frame();
            check(idleRects_.empty(),"OFF shows no column");
            // The same through the in-game Options screen, as a player picks it: the
            // IDLE BUILDERS list, then TOP -- the column moves to the top.
            if (settings_) {
                settings_->idleBuilders=2;applySettings(*settings_);frame();
                const float before=idleRects_.empty() ? -1 : idleRects_.front().r.y;
                openOptions(ta::OptionsPage::Interface);   // IDLE BUILDERS is on USER INTERFACE
                check(options_!=nullptr,"Options opens in game");
                options_->render(W,H);
                const auto press=[&](float x,float y) {
                    SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=int(x);e.button.y=int(y);
                    options_->input(e,W,H);
                    SDL_Event u=e;u.type=SDL_MOUSEBUTTONUP;options_->input(u,W,H);
                    options_->render(W,H);
                };
                // Scroll the row into view, then open its list and pick TOP (item 1).
                const SDL_FRect* row=nullptr;
                for (int i=0;i<60 && (!(row=options_->rowRect("IDLE BUILDERS")) || row->y+row->h>H*0.85f || row->y<H*0.15f);++i) {
                    SDL_Event w{};w.type=SDL_MOUSEWHEEL;w.wheel.y=-1;SDL_WarpMouseInWindow(nullptr,W/2,H/2);
                    options_->input(w,W,H);options_->render(W,H);
                }
                check(row!=nullptr,"the IDLE BUILDERS row is on the Options screen");
                press(row->x+row->w*0.9f,row->y+row->h/2);
                const SDL_FRect item=options_->dropItemRect(1);
                check(item.w>0,"its list opens");
                press(item.x+item.w/2,item.y+item.h/2);
                check(settings_->idleBuilders==1,"picking TOP sets the setting: "+std::to_string(settings_->idleBuilders));
                options_.reset();
                frame();
                check(idleBuilders_==1 && !idleRects_.empty() && idleRects_.front().r.y<before,
                      "the game takes it at once: the column moves up");
                settings_->idleBuilders=2;applySettings(*settings_);
            }
            idleBuilders_=2;beginSpectating();frame();
            check(idleRects_.empty(),"a watcher sees no column");
            std::fprintf(stderr,"PASS: idle builders strip\n");
            return;
        }
        if (ta::devFlag("TA_ORDER_PAGE_TEST")) {
            // The sidebar's ORDERS page (client/orderpage.h) on the real page: TotalA.exe
            // greys every order button no selected unit has (0x41a120), REPAIR following
            // canreclamate, and hides only LOAD or BLAST. FIREORD / MOVEORD show and step
            // the selection's own states (0x41a490). TA_ORDER_PAGE_CAPTURE=<prefix> saves
            // each unit's 640x480 frame as <prefix><unit>.png.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("order page: "+what);
                std::fprintf(stderr,"PASS: order page: %s\n",what.c_str());
            };
            noFog_=true;edgeScrollOn_=false;
            constexpr int W=640,H=480;
            winW_=W;winH_=H;
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int me=localPlayer_;
            std::map<std::string,int> ids;
            const char* kinds[]={"armcv","armpw","armcom","armatlas","armsolar","armspy","armck"};
            for (size_t i=0;i<std::size(kinds);++i) {
                ids[kinds[i]]=spawn(kinds[i],cx-240+float(i%4)*120,cz-120+float(i/4)*160,0,me);
                check(ids[kinds[i]]>=0,std::string("spawned ")+kinds[i]);
            }
            for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
            const auto frame=[&] {captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);endFrame();};
            mapView_.setZoom(1.0f);
            const std::string pre=ta::sides::at(std::max(0,ta::sides::find(side_))).prefix;
            const auto state=[&](const std::string& n) -> std::string {
                ta::TaGuiView* page=taPage(taHudPage_);
                if (!page) return "nopage";
                if (!page->visible(pre+n)) return "hidden";
                for (const auto& g:page->screen().gadgets)
                    if (SDL_strcasecmp(g.name.c_str(),(pre+n).c_str())==0) return g.grayedOut ? "grey" : "on";
                return "missing";
            };
            const auto show=[&](std::vector<int> sel,const std::string& tag) {
                selection_=std::move(sel);
                frame();frame();
                check(taHudPage_==pre+"GEN",tag+": the orders page is on show ("+taHudPage_+")");
                std::string line;
                for (const char* n:{"MOVE","STOP","ATTACK","DEFEND","PATROL","RECLAIM","REPAIR","CAPTURE","LOAD","UNLOAD",
                                    "BLAST","FIREORD","MOVEORD","ONOFF","CLOAK"})
                    line+=std::string(" ")+n+"="+state(n);
                std::fprintf(stderr,"order page: %s:%s\n",tag.c_str(),line.c_str());
                if (const char* cap=ta::devEnv("TA_ORDER_PAGE_CAPTURE")) {
                    captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);
                    std::vector<uint8_t> px(size_t(W)*H*4);
                    const SDL_Rect r{0,0,W,H};
                    if (SDL_RenderReadPixels(ren_,&r,SDL_PIXELFORMAT_RGBA32,px.data(),W*4)==0)
                        ta::png::write(std::string(cap)+tag+".png",W,H,px);
                    endFrame();
                }
            };
            show({ids["armcv"]},"armcv");
            check(state("REPAIR")=="on" && state("RECLAIM")=="on","armcv: REPAIR and RECLAIM work (canreclamate)");
            check(state("ATTACK")=="grey" && state("FIREORD")=="grey" && state("MOVEORD")=="on",
                  "armcv: no weapons, so ATTACK and FIREORD are grey; MOVEORD works");
            check(state("LOAD")=="hidden" && state("BLAST")=="grey" && state("UNLOAD")=="grey","armcv: BLAST grey in LOAD's slot");
            show({ids["armpw"]},"armpw");
            check(state("REPAIR")=="grey" && state("RECLAIM")=="grey","armpw: no canreclamate, REPAIR and RECLAIM grey");
            check(state("ATTACK")=="on" && state("FIREORD")=="on" && state("MOVE")=="on","armpw: ATTACK, FIREORD and MOVE work");
            show({ids["armcom"]},"armcom");
            check(state("BLAST")=="on" && state("CAPTURE")=="on" && state("CLOAK")=="on" && state("ONOFF")=="grey",
                  "armcom: BLAST (candgun), CAPTURE and CLOAK work, ONOFF grey");
            show({ids["armatlas"]},"armatlas");
            check(state("LOAD")=="on" && state("UNLOAD")=="on" && state("BLAST")=="hidden","armatlas: LOAD and UNLOAD, BLAST hidden");
            show({ids["armsolar"]},"armsolar");
            check(state("ONOFF")=="on" && state("STOP")=="grey" && state("MOVE")=="grey","armsolar: ONOFF alone");
            show({ids["armspy"]},"armspy");
            check(state("CLOAK")=="on" && state("ATTACK")=="grey","armspy: CLOAK works");
            show({ids["armcv"],ids["armpw"]},"armcv+armpw");
            check(state("REPAIR")=="on" && state("ATTACK")=="on" && state("FIREORD")=="on","a mixed selection: the union");
            // The tabs are hidden: the build row is the builder's menu (drawPanel).
            show({ids["armcv"]},"armcv");
            check(state("BUILD")=="hidden" && state("ORDERS")=="hidden","one builder: no tabs");
            check(!iconRects_.empty(),"one builder: its build row is on show");
            show({ids["armcv"],ids["armck"]},"armcv+armck");
            check(iconRects_.empty() && buildMenuRect_.w<=0,"two builders: no build row");
            // FIREORD steps the Peewee's fire state through the real input path.
            show({ids["armpw"]},"armpw");
            const auto fireOf=[&](int id){const auto* u=world_.unit(id);return u ? int(u->fireState) : -1;};
            const auto moveOf=[&](int id){const auto* u=world_.unit(id);return u ? int(u->moveState) : -1;};
            const auto press=[&](const std::string& gadget) {
                ta::TaGuiView* page=taPage(taHudPage_);
                float gx=0,gy=0;
                check(page && page->gadgetCentre(pre+gadget,gx,gy),taHudPage_+" has "+gadget);
                for (Uint32 kind:{Uint32(SDL_MOUSEBUTTONDOWN),Uint32(SDL_MOUSEBUTTONUP)}) {
                    SDL_Event e{};e.type=kind;e.button.button=SDL_BUTTON_LEFT;
                    e.button.x=int(taHudLayout_.px(gx));e.button.y=int(taHudLayout_.py(gy));
                    taHudEvent(e);
                }
                for (int i=0;i<3;++i) writableWorld().tick(1.f/30);
                frame();
            };
            const int fire0=fireOf(ids["armpw"]),move0=moveOf(ids["armpw"]);
            const auto next=[](int s){return s==0 ? 1 : s==1 ? 2 : 0;};
            press("FIREORD");
            check(fireOf(ids["armpw"])==next(fire0) && moveOf(ids["armpw"])==move0,
                  "FIREORD steps the fire state alone: "+std::to_string(fire0)+" -> "+std::to_string(fireOf(ids["armpw"])));
            check(taPage(taHudPage_)->stage(pre+"FIREORD")==fireOf(ids["armpw"]),"and the button shows it");
            press("MOVEORD");
            check(moveOf(ids["armpw"])==next(move0),"MOVEORD steps the move state: "+std::to_string(move0)+" -> "+
                  std::to_string(moveOf(ids["armpw"])));
            if (fireOf(ids["armpw"])!=fireOf(ids["armcom"])) {
                show({ids["armpw"],ids["armcom"]},"mixed");
                check(taPage(taHudPage_)->stage(pre+"FIREORD")==3,"fire states that differ show the mixed frame (3)");
            }
            const int cvFire=fireOf(ids["armcv"]);
            show({ids["armcv"],ids["armpw"]},"armcv+armpw");
            press("FIREORD");
            check(fireOf(ids["armcv"])==cvFire,"a unit without firestandorders keeps its fire state (0x48d0d7)");
            show({ids["armsolar"]},"armsolar");
            const bool on0=world_.unit(ids["armsolar"])->active;
            press("ONOFF");
            check(world_.unit(ids["armsolar"])->active!=on0,"ONOFF switches the solar collector");
            std::fprintf(stderr,"PASS: order page\n");
            return;
        }
        if (ta::devFlag("TA_GUI_SOUND_TEST")) {
            // TA's GUI sounds (client/guisound.h): a click on a sidebar gadget, through
            // taHudEvent as the input path delivers it, requests the allsound.tdf entry
            // its TotalA.exe handler names, which resolves to a shipped WAV.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("gui sound: "+what);
                std::fprintf(stderr,"PASS: gui sound: %s\n",what.c_str());
            };
            constexpr int W=1280,H=960;
            winW_=W;winH_=H;noFog_=true;edgeScrollOn_=false;
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int commander=spawn("armcom",cx,cz,0,localPlayer_);
            check(commander>=0,"fixture commander spawned");
            const auto frame=[&] {captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);endFrame();};
            selection_={commander};
            frame();frame();
            const std::string pre=ta::sides::at(std::max(0,ta::sides::find(side_))).prefix;
            const auto press=[&](const std::string& gadget,const char* entry,const char* wav) {
                ta::TaGuiView* page=taPage(taHudPage_);
                float gx=0,gy=0;
                check(page && page->gadgetCentre(gadget,gx,gy),taHudPage_+" has "+gadget);
                lastGuiSound_.clear();lastGuiWav_.clear();
                for (Uint32 kind:{Uint32(SDL_MOUSEBUTTONDOWN),Uint32(SDL_MOUSEBUTTONUP)}) {
                    SDL_Event e{};e.type=kind;e.button.button=SDL_BUTTON_LEFT;
                    e.button.x=int(taHudLayout_.px(gx));e.button.y=int(taHudLayout_.py(gy));
                    taHudEvent(e);
                }
                check(lastGuiSound_==entry && lastGuiWav_==wav && sounds_.has(lastGuiWav_.substr(0,lastGuiWav_.size()-4)),
                      gadget+" requests "+entry+" = "+wav+" (got "+lastGuiSound_+" = "+lastGuiWav_+")");
                frame();
            };
            press(pre+"MOVE","IMMEDIATEORDERS","button5.wav");        // 0x419c65
            press(pre+"STOP","IMMEDIATEORDERS","button5.wav");        // 0x419cf2 -> 0x41a0ea
            press(pre+"FIREORD","SETFIREORDERS","butnagr2.wav");      // 0x41a6af
            press(pre+"MOVEORD","SETMOVEORDERS","butnmbl2.wav");      // 0x41a5ac
            press(pre+"REPAIR","SPECIALORDERS","button5.wav");        // 0x419e9c
            {   // a building's picture on the build row: ADDBUILD (0x41ab97)
                std::optional<SDL_FPoint> solar;
                for (const auto& [r,bt]:iconRects_)
                    if (bt && bt->id==[&]{std::string l=pre;for(char& ch:l)ch=char(std::tolower(static_cast<unsigned char>(ch)));return l;}()+"solar") solar=SDL_FPoint{r.x+r.w/2,r.y+r.h/2};
                check(solar.has_value(),"the build row has the solar collector");
                lastGuiSound_.clear();lastGuiWav_.clear();
                check(buildIconClick(solar->x,solar->y,true,false),"the build row takes the press");
                check(lastGuiSound_=="ADDBUILD" && lastGuiWav_=="button9.wav",
                      "the solar collector requests ADDBUILD = button9.wav (got "+lastGuiSound_+" = "+lastGuiWav_+")");
                check(placing_ && placing_->id==[&]{std::string l=pre;for(char& ch:l)ch=char(std::tolower(static_cast<unsigned char>(ch)));return l;}()+"solar","and arms its placement");
                placing_=nullptr;
            }
            std::fprintf(stderr,"PASS: gui sound sidebar\n");
            return;
        }
        if (ta::devFlag("TA_UNIT_SPOTS_TEST")) {
            // Destination markers (GameView::drawUnitSpots): a mixed-speed right-click
            // move of Peewees, Hammers and Stumpies, four Peewee stragglers far outside
            // TotalA.exe's reach (0x48cf30: retail sends them all to the click) and
            // three Freedom Fighters, under Legion or (TA_UNIT_SPOTS_MODE=retail)
            // Retail pathfinding. Every unit's own spot is captured (UnitR::hasSpot),
            // no two alike, and outlined; with TA_UNIT_SPOTS_CAPTURE=<prefix> the
            // flash and the Shift view are saved.
            constexpr int W=1200,H=800;
            winW_=W;winH_=H;noFog_=true;edgeScrollOn_=false;uiScale_=1.0f;
            if(!cursors_.ok()) {cursorsInit_=true;cursors_.load(ren_,vfs_,settings_);}
            const char* modeEnv=ta::devEnv("TA_UNIT_SPOTS_MODE");
            const bool retail=modeEnv && std::string(modeEnv)=="retail";
            writableWorld().setPathfindingMode(retail ? ta::sim::PathfindingMode::Retail : ta::sim::PathfindingMode::Legion);
            writableWorld().setPathService(true);   // as a real match's setup (sim/matchsetup.cpp): Legion plans through it
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            static constexpr const char* kinds[]={"armpw","armham","armstump"};
            std::vector<int> ids;
            const auto add=[&](const char* kind,float x,float z) {
                const auto* type=registry_.find(kind);
                if (!type || !world_.canPlace(type,x,z)) return;
                if (const int id=spawn(kind,x,z,0,localPlayer_); id>0) ids.push_back(id);
            };
            for (int i=0;i<15;++i) add(kinds[i%3],cx-300+float(i%5)*44,cz-90+float(i/5)*60);
            for (int i=0;i<4;++i) add("armpw",cx-300+float(i)*40,cz+420);    // stragglers
            for (int i=0;i<3;++i) add("armfig",cx-300+float(i)*60,cz-260);   // flyers
            if (ids.size()<16) throw std::runtime_error("unit spots: fixture");
            selection_=ids;
            const auto publish=[&] {captureFrame();beginFrame();cosmeticStep(0);};
            publish();
            const float tx=cx+260,tz=cz;
            issueClickPlan(planClick(0,tx,tz,nullptr,0,false,true),0,0);
            // Legion hands a big click's slots out over several ticks: wait for them
            // (at most four seconds; a far straggler waits about two), the flyers still
            // climbing off the ground or on their way.
            for (int i=0;i<120;++i) {
                writableWorld().tick(1.f/30);
                if (i<29) continue;
                publish();
                if (std::all_of(ids.begin(),ids.end(),[&](int id) {
                        const UnitR* u=frameUnitP(id);return u && spotLeg(*u,0);})) break;
            }
            std::set<std::pair<float,float>> spots;
            int flyers=0;
            for (const int id:ids)
                if (const UnitR* u=frameUnitP(id); u && spotLeg(*u,0)) {
                    spots.insert({u->spotX,u->spotZ});
                    flyers+=u->type->canFly;
                    std::fprintf(stderr,"unit spots: %s id=%d at %.1f,%.1f spot %.1f,%.1f\n",
                                 u->type->id.c_str(),id,u->x,u->z,u->spotX,u->spotZ);
                }
            mapView_.setZoom(1.0f);
            mapView_.setOffset(tx-float(W-300)*0.5f,tz-terrainLift(tx,tz)-float(H)*0.5f);
            const char* cap=ta::devEnv("TA_UNIT_SPOTS_CAPTURE");
            const auto frame=[&](SDL_Keymod mods,const char* tag) {
                SDL_SetModState(mods);
                for (int i=0;i<20;++i) {beginFrame();prepare(W,H);draw(W,H);}   // terrain tiles stream in
                if (cap) {
                    int ow=0,oh=0;SDL_GetRendererOutputSize(ren_,&ow,&oh);
                    std::vector<uint8_t> px4(size_t(ow)*size_t(oh)*4);
                    if (SDL_RenderReadPixels(ren_,nullptr,SDL_PIXELFORMAT_RGBA32,px4.data(),ow*4)==0)
                        ta::png::write(std::string(cap)+"-"+tag+".png",ow,oh,px4);
                }
            };
            frame(KMOD_NONE,"flash");
            const size_t flashed=spotBatch_.size()/24;   // four segments of six vertices a marker
            frame(KMOD_LSHIFT,"shift");
            const size_t held=spotBatch_.size()/24;
            SDL_SetModState(KMOD_NONE);
            std::fprintf(stderr,"unit spots: %zu units, %zu own spots (%d flyers), %zu flashed, %zu held under Shift\n",
                         ids.size(),spots.size(),flyers,flashed,held);
            if (spots.size()!=ids.size()) throw std::runtime_error("unit spots: a unit has no spot of its own, or two share one");
            if (flyers!=3) throw std::runtime_error("unit spots: a flyer has no spot");
            if (flashed!=ids.size() || held!=ids.size()) throw std::runtime_error("unit spots: not every spot is outlined");
            std::fprintf(stderr,"PASS: every unit of a mixed %s move shows its own spot\n",retail ? "Retail" : "Legion");
            return;
        }
        if (ta::devFlag("TA_AREA_DRAG_TEST")) {
            // Map drags through the real input() and draw(): a small right-drag with a
            // reclaimer is the ordinary order, only a deliberate box clears an area,
            // and a pan mid-drag keeps the box (drawn and commanded) on the ground the
            // press was on. See client/areadrag.h.
            const auto check=[](bool ok,const std::string& what) {
                if (!ok) throw std::runtime_error("area drag: "+what);
                std::fprintf(stderr,"PASS: area drag: %s\n",what.c_str());
            };
            constexpr int W=1000,H=700;
            winW_=W;winH_=H;noFog_=true;edgeScrollOn_=false;uiScale_=1.0f;
            writableWorld().setPlayerCount(2);
            const float cx=float(mapView_.map().blocksX)*16.0f,cz=float(mapView_.map().blocksY)*16.0f;
            const int builder=spawn("armcom",cx+200,cz+150,0,localPlayer_);   // off the drag point (400,300)
            const int soldier=spawn("armpw",cx-150,cz-100,0,localPlayer_);
            check(world_.unit(builder) && world_.unit(builder)->type->canReclaim,"fixture has a reclaimer");
            mapView_.setZoom(1);
            const float baseX=cx-400,baseZ=cz-300;
            const auto publish=[&] {captureFrame();beginFrame();cosmeticStep(0);};
            const auto event=[&](uint32_t kind,uint8_t button,float x,float y) {
                SDL_Event e{};e.type=kind;
                if (kind==SDL_MOUSEMOTION) {e.motion.x=int(x);e.motion.y=int(y);}
                else {e.button.button=button;e.button.x=int(x);e.button.y=int(y);}
                inputFromView(e,W,H);   // a point in the world view
            };
            const auto savedMod=SDL_GetModState();SDL_SetModState(KMOD_NONE);
            const auto reset=[&] {
                mapView_.setOffset(baseX,baseZ);
                writableWorld().stop(builder);writableWorld().unit(builder)->orders.clear();
                selection_={builder};publish();
            };
            const auto areaOrder=[&]()->const ta::sim::RetailReclaimArea* {
                const auto& orders=world_.unit(builder)->orders;
                return !orders.empty() && orders.front().reclaimArea ? &*orders.front().reclaimArea : nullptr;
            };
            const auto plainOrder=[&] {
                const auto& orders=world_.unit(builder)->orders;
                return !orders.empty() && !orders.front().reclaimArea;
            };
            // Right-press at P, optional camera move, pointer to Q, release at Q.
            const auto rightDrag=[&](float px,float py,float qx,float qy,float panX=0,float panZ=0) {
                event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_RIGHT,px,py);
                if (panX!=0 || panZ!=0) mapView_.setOffset(mapView_.offX()+panX,mapView_.offY()+panZ);
                event(SDL_MOUSEMOTION,0,qx,qy);
                event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,qx,qy);
            };
            const float px=400,py=300;
            reset();rightDrag(px,py,px,py);
            check(plainOrder(),"right-click with a reclaimer is the ordinary order");
            reset();rightDrag(px,py,px+12,py+9);
            check(plainOrder(),"a 12x9 px twitch is the ordinary order, not a tiny clear");
            reset();rightDrag(px,py,px+31,py+31);
            check(plainOrder(),"31x31 px is still a click");
            reset();rightDrag(px,py,px+250,py+10);
            check(plainOrder(),"a long 10 px sliver is a click");
            // A click orders at the press, not where a twitch released it.
            {
                float tx,tz;pickWorld(px,py,tx,tz);
                reset();
                std::vector<ta::net::Command> cmds;
                {
                    ClickScope outer(*this);
                    rightDrag(px,py,px+20,py+20);
                    cmds=click_->cmds;
                }
                check(cmds.size()==1 && cmds[0].kind==ta::net::Cmd::Move &&
                      std::fabs(cmds[0].x-tx)<1 && std::fabs(cmds[0].z-tz)<1,
                      "a 20 px twitch orders at the pressed ground");
            }
            reset();
            float wx0,wz0,wx1,wz1;pickWorld(px,py,wx0,wz0);pickWorld(px+60,py+50,wx1,wz1);
            rightDrag(px,py,px+60,py+50);
            {
                const auto* area=areaOrder();
                check(area!=nullptr,"a 60x50 px box is an area clear");
                const auto closeTo=[](int32_t v,float f) {return std::fabs(ta::sim::Fixed::raw(v).toFloat()-f)<1.0f;};
                check(closeTo(area->minX,std::min(wx0,wx1)) && closeTo(area->maxX,std::max(wx0,wx1)) &&
                      closeTo(area->minZ,std::min(wz0,wz1)) && closeTo(area->maxZ,std::max(wz0,wz1)),
                      "the clear covers press to release on the ground");
            }
            uiScale_=2.0f;reset();rightDrag(px,py,px+50,py+50);
            check(plainOrder(),"at 200% UI a 50 px box is under the (64 px) threshold");
            reset();rightDrag(px,py,px+70,py+70);
            check(areaOrder()!=nullptr,"at 200% UI a 70 px box clears");
            uiScale_=1.0f;
            // Pan alone (the pointer never moves) is still a click.
            reset();rightDrag(px,py,px,py,120,90);
            check(plainOrder(),"a pan with the pointer held still is a click");
            // A pan mid-drag: the clear starts at the PRESSED ground, not at the press's
            // screen point on the panned map.
            reset();pickWorld(px,py,wx0,wz0);
            event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_RIGHT,px,py);
            mapView_.setOffset(baseX+80,baseZ+60);   // as edge scroll / keys / minimap do
            event(SDL_MOUSEMOTION,0,px+50,py+40);
            {
                const auto a=dragStartOnScreen(rdAnchor_);
                check(std::fabs(a.x-(px-80))<0.01f && std::fabs(a.y-(py-60))<0.01f,
                      "the box corner moved with the map");
                check(areaBoxAt(rdAnchor_,px+50,py+40),"the panned box is shown as an area");
                // The box as drawn: its top edge runs from the re-projected corner.
                captureFrame();beginFrame();cosmeticStep(0);prepare(W,H);draw(W,H);
                int ow=0,oh=0;SDL_GetRendererOutputSize(ren_,&ow,&oh);
                std::vector<uint8_t> px4(size_t(ow)*size_t(oh)*4);
                const bool read=readViewPixels(px4.data(),ow*4)==0;
                check(read,"the drawn frame reads back");
                {
                    if (const char* cap=ta::devEnv("TA_AREA_DRAG_CAPTURE")) ta::png::write(cap,ow,oh,px4);
                    const auto outline=[&](int x,int y) {
                        const uint8_t* q=&px4[(size_t(y)*size_t(ow)+size_t(x))*4];
                        return q[0]>=215 && q[1]>=155 && q[1]<=210 && q[2]<=110;
                    };
                    const auto edge=[&](float y,float xa,float xb) {
                        int hit=0,n=0;
                        for (int x=int(xa)+3;x<=int(xb)-3;++x,++n) hit+=outline(x,int(y));
                        return n>0 && hit*10>=n*9;
                    };
                    check(edge(py-60,px-80,px+50),"drawn top edge starts on the pressed ground");
                    check(!edge(py,px,px+50),"no box edge at the stale screen press point");
                }
                endFrame();
            }
            event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,px+50,py+40);
            {
                pickWorld(px+50,py+40,wx1,wz1);
                const auto* area=areaOrder();
                check(area!=nullptr,"the panned drag clears");
                const auto closeTo=[](int32_t v,float f) {return std::fabs(ta::sim::Fixed::raw(v).toFloat()-f)<1.0f;};
                check(closeTo(area->minX,std::min(wx0,wx1)) && closeTo(area->minZ,std::min(wz0,wz1)) &&
                      closeTo(area->maxX,std::max(wx0,wx1)) && closeTo(area->maxZ,std::max(wz0,wz1)),
                      "the panned clear covers what the box showed");
            }
            // A zoom mid-drag keeps the corner on its ground as well.
            reset();
            event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_RIGHT,px,py);
            mapView_.setZoom(0.5f);
            {
                const auto a=dragStartOnScreen(rdAnchor_);
                const float ex=(baseX+px-mapView_.offX())*0.5f,ey=(baseZ+py-mapView_.offY())*0.5f;
                check(std::fabs(a.x-ex)<0.01f && std::fabs(a.y-ey)<0.01f,"zoom: the corner stays on its ground");
            }
            event(SDL_MOUSEBUTTONUP,SDL_BUTTON_RIGHT,px,py);
            mapView_.setZoom(1);
            // The selection marquee: a pan mid-drag selects what the drawn box covers.
            reset();selection_.clear();publish();
            const SDL_FPoint s0=unitScreen(*frameUnitP(soldier));
            const float ax=s0.x-30,ay=s0.y-30;   // press just up-left of the soldier
            event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_LEFT,ax,ay);
            mapView_.setOffset(baseX+100,baseZ+100);publish();   // soldier now 100 px up-left
            event(SDL_MOUSEMOTION,0,ax-40,ay-40);   // box: pressed ground (ax-100) .. ax-40
            event(SDL_MOUSEBUTTONUP,SDL_BUTTON_LEFT,ax-40,ay-40);
            check(selection_==std::vector<int>{soldier},"marquee after a pan selects inside the drawn box");
            // A click-select held through a pan picks the unit pressed on, not the
            // ground the pan moved under the held pointer.
            reset();selection_.clear();publish();
            {
                const SDL_FPoint s1=unitScreen(*frameUnitP(soldier));
                event(SDL_MOUSEBUTTONDOWN,SDL_BUTTON_LEFT,s1.x,s1.y);
                mapView_.setOffset(baseX+150,baseZ+120);publish();
                event(SDL_MOUSEBUTTONUP,SDL_BUTTON_LEFT,s1.x,s1.y);
                check(selection_==std::vector<int>{soldier},"click-select across a pan picks the pressed unit");
            }
            reset();
            SDL_SetModState(savedMod);
            std::fprintf(stderr,"PASS: area drag threshold %.0f UI px both ways, map-anchored boxes\n",
                         ta::client::areadrag::kAreaCommandMinPx);
            return;
        }
        // TA_BUILDLINE_SHOT=dx,dy[,notches]: a construction kbot mid shift-drag of a Dragon's Teeth line,
        // dx,dy screen px from (300,300) at zoom 1.5, the wheel turned `notches` --
        // left in progress so --shot captures the preview (and its GAP label).
        if (const char* spec=ta::devEnv("TA_BUILDLINE_SHOT")) {
            noFog_=true;edgeScrollOn_=false;
            winW_=1024;winH_=768;   // --winsize 1024 768: the renderer has no size yet
            int dx=0,dy=0,notches=0;std::sscanf(spec,"%d,%d,%d",&dx,&dy,&notches);
            const auto* wall=registry_.find("armdrag");
            if(!wall)throw std::runtime_error("buildline shot needs armdrag");
            const float cx0=float(mapView_.map().blocksX)*16.0f,cz0=float(mapView_.map().blocksY)*16.0f;
            const float zoom=1.0f;mapView_.setZoom(zoom);
            float wx=0,wz=0;bool open=false;
            for(int i=0;i<1600 && !open;++i) {
                wx=cx0-2000+float(i%40)*100;wz=cz0-2000+float(i/40)*100;open=true;
                for(float z=wz-120;open && z<=wz+360;z+=24)for(float x=wx-120;open && x<=wx+360;x+=24)
                    if(!world_.canPlace(wall,x,z,localPlayer_))open=false;
            }
            if(!open)throw std::runtime_error("buildline shot found no open ground");
            mapView_.setOffset(wx-300/zoom,wz-300/zoom);
            const int commander=spawn("armck",wx-60,wz+90,0,localPlayer_);
            captureFrame();beginFrame();cosmeticStep(0);
            selection_={commander};placing_=wall;SDL_SetModState(KMOD_LSHIFT);
            SDL_Event e{};e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.x=300;e.button.y=300;inputFromView(e,winW_,winH_);
            SDL_Event m{};m.type=SDL_MOUSEMOTION;m.motion.x=300+dx;m.motion.y=300+dy;m.motion.state=SDL_BUTTON_LMASK;inputFromView(m,winW_,winH_);
            SDL_PushEvent(&m);   // again through the loop, after the window's own first motion
            for(int i=0;i<notches;++i){SDL_Event w{};w.type=SDL_MOUSEWHEEL;w.wheel.y=1;
#if SDL_VERSION_ATLEAST(2, 0, 18)
                w.wheel.preciseY=1;
#endif
                input(w,winW_,winH_);}
            std::fprintf(stderr,"buildline shot: %dx%d drag %d gap %d\n",winW_,winH_,int(buildDrag_),buildLineGap_);
            return;
        }
        // Shift-drag build lines through real mouse/wheel events: every site the preview
        // shows is queued, and after the local sim plays the builders out every one of
        // them stands finished on its own cells -- no site refused for a neighbour in
        // its own line. Gap 0 touching, a wheel-widened gap, the gap held across later
        // drags and building types, clamped at 0, and the camera untouched by the wheel.
        if (ta::devFlag("TA_BUILDLINE_TEST")) {
            noFog_=true;edgeScrollOn_=false;
            winW_=1280;winH_=960;
            mapView_.setZoom(1);
            const auto publish=[&] {captureFrame();beginFrame();cosmeticStep(0);};
            const auto fail=[](const std::string& m) {throw std::runtime_error("buildline: "+m);};
            // The commander's menu: the smallest building, a non-square one and a 4x4-ish
            // square, so the lines cover different footprints in both proportions.
            const ta::sim::UnitType *small=nullptr,*oblong=nullptr,*square=nullptr;
            const auto area=[](const ta::sim::UnitType* t) {return t->footX*t->footZ;};
            for (const auto& id:registry_.buildable("armcom")) {
                const auto* t=registry_.find(id);
                if (!t || !t->isStructure() || t->footX<1 || t->footZ<1) continue;
                if (!t->yardMap.empty() && t->yardMap.find_first_of("wWCY")!=std::string::npos) continue;
                if (!small || area(t)<area(small)) small=t;
                if (t->footX!=t->footZ && (!oblong || area(t)<area(oblong))) oblong=t;
                if (t->footX==t->footZ && (!square || std::abs(area(t)-16)<std::abs(area(square)-16))) square=t;
            }
            if (!small || !oblong || !square) fail("armcom's menu lacks the footprints this test needs");            for (const auto* t:{small,oblong,square})
                std::fprintf(stderr,"buildline: type %s footprint %dx%d\n",t->id.c_str(),t->footX,t->footZ);
            struct Case {const ta::sim::UnitType* type;int dx,dy;int wheel;int gap;const char* name;};
            // Screen-pixel drags at zoom 1; wheel = notches during the drag; gap = expected.
            std::vector<Case> cases={
                {small,  240,  0,  0, 0, "small horizontal"},
                {small,    0,240,  0, 0, "small vertical"},
                {small,  220,220,  0, 0, "small diagonal"},
                {oblong, 300,  0,  0, 0, "oblong horizontal"},
                {oblong,   0,300,  0, 0, "oblong vertical"},
                {oblong, 300,170,  0, 0, "oblong shallow diagonal"},
                {square, 300,-300, 0, 0, "square anti-diagonal"},
                {small,  300,  0,  3, 3, "small horizontal, wheel up 3"},
                {oblong, 300,  0,  0, 3, "oblong horizontal, gap held from the last drag"},
                {square,-260,130,  0, 3, "square leftward, gap held"},
                {small,  200,  0, -9, 0, "small horizontal, wheel down past 0"},
            };
            // TA_BUILDLINE_PROBE=dx,dy,wheel,bx,bz[,type]: just one drag (type 0 small, 1
            // oblong, 2 square), its commander at (bx,bz) px from the line's start -- for
            // sweeping approaches.
            float probeBx=-40,probeBz=-60;
            if (const char* pr=ta::devEnv("TA_BUILDLINE_PROBE")) {
                int dx=0,dy=0,w=0,t=0;std::sscanf(pr,"%d,%d,%d,%f,%f,%d",&dx,&dy,&w,&probeBx,&probeBz,&t);
                cases={{t==1?oblong:t==2?square:small,dx,dy,w,std::max(0,w),"probe"}};
            }
            const float cx0=float(mapView_.map().blocksX)*16.0f,cz0=float(mapView_.map().blocksY)*16.0f;
            struct Line {int builder;const ta::sim::UnitType* type;std::vector<std::pair<float,float>> sites;int gap;std::string name;};
            std::vector<Line> lines;
            const auto savedMod=SDL_GetModState();
            const auto send=[&](uint32_t kind,int x,int y) {
                SDL_Event e{};e.type=kind;
                if (kind==SDL_MOUSEMOTION) {e.motion.x=x;e.motion.y=y;e.motion.state=SDL_BUTTON_LMASK;}
                else {e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;}
                inputFromView(e,winW_,winH_);
            };
            const auto wheel=[&](float notches) {
                static uint32_t ms=1000;ms+=200;   // event times a notch apart, past the trackpad limit
                SDL_Event e{};e.type=SDL_MOUSEWHEEL;e.wheel.y=notches>0?1:-1;e.wheel.timestamp=ms;
#if SDL_VERSION_ATLEAST(2, 0, 18)
                e.wheel.preciseY=notches;
#endif
                e.wheel.direction=SDL_MOUSEWHEEL_NORMAL;input(e,winW_,winH_);
            };
            // Camera on a patch of open ground, clear of the earlier (queued, not yet
            // built) lines, where this drag's sites can all go; wx,wz its start.
            const auto findPatch=[&](const ta::sim::UnitType* type,int gap,int sx,int sy,int dx,int dy,float& wx,float& wz) {
                const int across=int(cx0*2/100)-4,down=int(cz0*2/100)-4;
                for (int i=0;i<across*down;++i) {
                    wx=200+float(i%across)*100;wz=200+float(i/across)*100;
                    mapView_.setOffset(wx-float(sx),wz-float(sy));
                    float x0,z0,x1,z1;pickWorld(float(sx),float(sy),x0,z0);pickWorld(float(sx+dx),float(sy+dy),x1,z1);
                    bool open=true;
                    const auto sites=ta::client::buildLineSites(type->footX,type->footZ,gap,x0,z0,x1,z1);
                    for (const auto& [x,z]:sites) {
                        if (!world_.canPlace(type,x,z,localPlayer_)) open=false;
                        for (const auto& l:lines) for (const auto& [px,pz]:l.sites)
                            if (std::abs(px-x)<400 && std::abs(pz-z)<400) open=false;
                        if (!open) break;
                    }
                    // Open ground all round the line too, so nothing but the line itself
                    // stands between a commander and its sites.
                    const float lx=std::min(x0,x1)-160,hx=std::max(x0,x1)+160,lz=std::min(z0,z1)-160,hz=std::max(z0,z1)+160;
                    for (float z=lz;open && z<=hz;z+=32)
                        for (float x=lx;open && x<=hx;x+=32)
                            if (!world_.canPlace(small,x,z,localPlayer_)) open=false;
                    if (open) return true;
                }
                return false;
            };
            for (size_t k=0;k<cases.size();++k) {
                const auto& c=cases[k];
                // Each line gets its own patch of open ground, clear of the earlier
                // (queued, not yet built) lines, and its own commander.
                const int sx=c.dx<0?560:220,sy=c.dy<0?560:220;
                float wx,wz;
                if (!findPatch(c.type,c.gap,sx,sy,c.dx,c.dy,wx,wz)) fail(std::string(c.name)+": no open patch for the line");
                const int builder=spawn("armcom",wx+probeBx,wz+probeBz,0,localPlayer_);
                if (builder<0) fail("could not spawn a commander");
                publish();
                selection_={builder};placing_=c.type;shiftBuildPlaced_=false;
                SDL_SetModState(KMOD_LSHIFT);
                send(SDL_MOUSEBUTTONDOWN,sx,sy);
                if (!buildDrag_) fail(std::string(c.name)+": shift-press did not start a line");
                send(SDL_MOUSEMOTION,sx+c.dx,sy+c.dy);
                const float zoomBefore=mapView_.zoom(),offBefore=mapView_.offX();
                if (c.wheel>0) {wheel(0.5f);wheel(0.5f);for (int i=1;i<c.wheel;++i) wheel(1);}   // two half notches = one
                for (int i=0;i<-c.wheel;++i) wheel(-1);
                if (mapView_.zoom()!=zoomBefore || mapView_.offX()!=offBefore)
                    fail(std::string(c.name)+": the wheel moved the camera during the drag");
                if (buildLineGap_!=c.gap)
                    fail(std::string(c.name)+": gap "+std::to_string(buildLineGap_)+", expected "+std::to_string(c.gap));
                float ex,ez;pickWorld(float(sx+c.dx),float(sy+c.dy),ex,ez);
                const auto sites=buildLinePositions(bdX0_,bdZ0_,ex,ez);
                if (sites.size()<2) fail(std::string(c.name)+": the drag shows fewer than two sites");
                for (const auto& [x,z]:sites) {
                    std::vector<int> clearing;
                    if (!world_.canPlace(c.type,x,z,localPlayer_) &&
                        !world_.clearableForPlacement(c.type,x,z,clearing,localPlayer_))
                        fail(std::string(c.name)+": the fixture patch is not open ground");
                }
                send(SDL_MOUSEBUTTONUP,sx+c.dx,sy+c.dy);
                SDL_SetModState(KMOD_NONE);
                {SDL_Event e{};e.type=SDL_KEYUP;e.key.keysym.sym=SDLK_LSHIFT;e.key.keysym.mod=KMOD_NONE;input(e,winW_,winH_);}
                size_t builds=0;
                for (const auto& o:world_.unit(builder)->orders) builds+=o.buildType==c.type;
                if (builds!=sites.size())
                    fail(std::string(c.name)+": "+std::to_string(builds)+" builds queued for "+std::to_string(sites.size())+" sites");
                lines.push_back({builder,c.type,sites,c.gap,c.name});
                std::fprintf(stderr,"buildline: %s: %zu sites queued, gap %d\n",c.name,sites.size(),c.gap);
            }
            {   // Out of a drag the wheel still zooms the camera.
                const float z=mapView_.zoom();wheel(1);
                if (mapView_.zoom()==z) fail("the wheel no longer zooms outside a line drag");
                mapView_.setZoom(1);
            }
            {   // A single shift-click is still one building, at the tight default.
                float wx,wz;
                if (!findPatch(small,0,220,220,0,0,wx,wz)) fail("no open patch for a single building");
                const int builder=spawn("armcom",wx-40,wz-60,0,localPlayer_);publish();
                selection_={builder};placing_=small;SDL_SetModState(KMOD_LSHIFT);
                send(SDL_MOUSEBUTTONDOWN,220,220);send(SDL_MOUSEBUTTONUP,220,220);SDL_SetModState(KMOD_NONE);
                float x,z;pickWorld(220,220,x,z);
                if (world_.unit(builder)->orders.size()!=1)fail("a shift-click did not queue exactly one building");
                lines.push_back({builder,small,buildLinePositions(x,z,x,z),0,"single shift-click"});
            }
            placing_=nullptr;shiftBuildPlaced_=false;SDL_SetModState(savedMod);selection_.clear();
            // Play the builders out: metal and energy topped up so cost never stalls a site.
            const auto busy=[&] {
                for (const auto& l:lines) if (!world_.unit(l.builder)->orders.empty()) return true;
                for (const auto& u:world_.units()) if (u.alive() && u.underConstruction) return true;
                return false;
            };
            int ticks=0;
            for (;ticks<30*60*30 && busy();++ticks) {
                if (ticks%15==0) {
                    if (const auto& b=world_.player(localPlayer_).books; b.stored[0]<50000 || b.stored[1]<50000)
                        writableWorld().setStartResources(localPlayer_,1e6f,1e6f);
                }
                simStep(1.0f/30.0f);
            }
            int built=0,expected=0;
            for (const auto& l:lines) {
                std::vector<std::pair<int,int>> cells;
                for (const auto& [x,z]:l.sites) {
                    ++expected;
                    const int ox=ta::sim::footprintOrigin(x,l.type->footX),oz=ta::sim::footprintOrigin(z,l.type->footZ);
                    bool found=false;
                    for (const auto& u:world_.units())
                        if (world_.finishedHere(u) && u.type==l.type && int(u.player)==localPlayer_ &&
                            ta::sim::footprintOrigin(u.x,l.type->footX)==ox && ta::sim::footprintOrigin(u.z,l.type->footZ)==oz)
                            found=true;
                    if (!found) {
                        const auto* b=world_.unit(l.builder);
                        std::string lineCells;
                        for (const auto& [sx2,sz2]:l.sites)
                            lineCells+=" "+std::to_string(ta::sim::footprintOrigin(sx2,l.type->footX))+","+
                                       std::to_string(ta::sim::footprintOrigin(sz2,l.type->footZ));
                        fail(l.name+": the site at cells "+std::to_string(ox)+","+std::to_string(oz)+
                             " was not built ("+std::to_string(ticks)+" ticks; line"+lineCells+"; builder "+
                             (b?std::to_string(b->x.toFloat()/16)+","+std::to_string(b->z.toFloat()/16)+" orders "+
                                std::to_string(b->orders.size()):std::string("gone"))+")");
                    }
                    if (!cells.empty()) {
                        const int dx=std::abs(ox-cells.back().first),dz=std::abs(oz-cells.back().second);
                        if (std::max(dx-l.type->footX,dz-l.type->footZ)!=l.gap)
                            fail(l.name+": built neighbours are not "+std::to_string(l.gap)+" cells apart");
                    }
                    cells.push_back({ox,oz});++built;
                }
            }
            std::fprintf(stderr,"PASS: %zu shift-drag lines, %d/%d sites queued and built in %d ticks; "
                         "gap 0 touching, wheel gap held across drags and clamped, camera still on drag\n",
                         lines.size(),built,expected,ticks);
            // --shot frames the first (tight, small, horizontal) line.
            mapView_.setZoom(1.5f);
            mapView_.setOffset(lines.front().sites.front().first-160,lines.front().sites.front().second-300);
            publish();
            return;
        }
        // TA_BUILDLINE_CHAIN=type;gap;bx,bz;dx,dy;dx,dy... (';' or '/'): ONE construction kbot (armck, whose menu has the walls), (bx,bz) px from
        // the first corner, lays a chain of shift-drags (each starting where the last
        // ended: an L, a U, a box) of type 0 armdrag (Dragon's Teeth), 1 armfort (Fortification Wall) or 2 armllt
        // (Light Laser Tower) at that gap.
        // Every distinct site must get built: chained lines plan their approach from
        // where the earlier ones leave the builder. Exits 1 on a stranded site.
        if (const char* spec=ta::devEnv("TA_BUILDLINE_CHAIN")) {
            writableWorld().setPathService(true);   // as a real match's setup (sim/matchsetup.cpp)
            noFog_=true;edgeScrollOn_=false;winW_=1280;winH_=960;mapView_.setZoom(1);
            const auto publish=[&] {captureFrame();beginFrame();cosmeticStep(0);};
            std::vector<std::string> parts;
            {std::string s(spec);size_t p0=0;
             for (size_t p;(p=s.find_first_of(";/",p0))!=std::string::npos;p0=p+1) parts.push_back(s.substr(p0,p-p0));
             parts.push_back(s.substr(p0));}
            if (parts.size()<4) {std::fprintf(stderr,"BUILDLINE_CHAIN bad spec\n");std::exit(2);}
            const char* ids[]={"armdrag","armfort","armllt"};
            const auto* type=registry_.find(ids[std::clamp(std::atoi(parts[0].c_str()),0,2)]);
            const auto* small=registry_.find("armdrag");
            if (!type || !small) {std::fprintf(stderr,"BUILDLINE_CHAIN missing types\n");std::exit(2);}
            buildLineGap_=std::atoi(parts[1].c_str());
            float bx=0,bz=0;std::sscanf(parts[2].c_str(),"%f,%f",&bx,&bz);
            std::vector<std::pair<int,int>> pts{{500,450}};   // screen corners at zoom 1
            for (size_t i=3;i<parts.size();++i) {
                int dx=0,dy=0;std::sscanf(parts[i].c_str(),"%d,%d",&dx,&dy);
                pts.push_back({pts.back().first+dx,pts.back().second+dy});
            }
            int lx=500,hx=500,ly=450,hy=450;
            for (const auto& [x,y]:pts) {lx=std::min(lx,x);hx=std::max(hx,x);ly=std::min(ly,y);hy=std::max(hy,y);}
            // Open ground under the whole chain and round the builder, for the chain's own
            // type too (a Light Laser Tower refuses slopes Dragon's Teeth take).
            const float mw=float(mapView_.map().blocksX)*32.0f,mh=float(mapView_.map().blocksY)*32.0f;
            bool open=false;float wx=0,wz=0;
            for (int i=0;i<40000 && !open;++i) {
                wx=300+float(i%200)*((mw-600)/200);wz=300+float(i/200)*((mh-600)/200);
                if (wx+float(hx-500)+200>mw || wz+float(hy-450)+200>mh || wx+float(lx-500)<200 || wz+float(ly-450)<200) continue;
                open=true;
                for (float z=wz+float(ly-450)-96;open && z<=wz+float(hy-450)+96;z+=32)
                    for (float x=wx+float(lx-500)-96;open && x<=wx+float(hx-500)+96;x+=32)
                        if (!world_.canPlace(small,x,z,localPlayer_)) open=false;
                // The chain's own type along the drags themselves (a Light Laser Tower
                // refuses slopes over 14 that Dragon's Teeth, maxslope 32, take).
                for (size_t i=0;open && i+1<pts.size();++i)
                    for (int s=0;open && s<=64;++s) {
                        const float f=float(s)/64;
                        const float x=wx+float(pts[i].first-500)+f*float(pts[i+1].first-pts[i].first);
                        const float z=wz+float(pts[i].second-450)+f*float(pts[i+1].second-pts[i].second);
                        if (!world_.canPlace(type,x,z,localPlayer_)) open=false;
                    }
                for (float z=wz+bz-64;open && z<=wz+bz+64;z+=32)
                    for (float x=wx+bx-64;open && x<=wx+bx+64;x+=32)
                        if (!world_.canPlace(small,x,z,localPlayer_)) open=false;
            }
            if (!open) {std::fprintf(stderr,"BUILDLINE_CHAIN no open patch\n");std::exit(3);}
            mapView_.setOffset(wx-500,wz-450);
            const int builder=spawn("armck",wx+bx,wz+bz,0,localPlayer_);
            publish();
            const auto send=[&](uint32_t kind,int x,int y) {
                SDL_Event e{};e.type=kind;
                if (kind==SDL_MOUSEMOTION) {e.motion.x=x;e.motion.y=y;e.motion.state=SDL_BUTTON_LMASK;}
                else {e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;}
                inputFromView(e,winW_,winH_);
            };
            std::vector<std::pair<float,float>> all;
            for (size_t i=0;i+1<pts.size();++i) {
                SDL_SetModState(KMOD_LSHIFT);placing_=type;selection_={builder};
                send(SDL_MOUSEBUTTONDOWN,pts[i].first,pts[i].second);
                send(SDL_MOUSEMOTION,pts[i+1].first,pts[i+1].second);
                float ex,ez;pickWorld(float(pts[i+1].first),float(pts[i+1].second),ex,ez);
                for (const auto& q:buildLinePositions(bdX0_,bdZ0_,ex,ez)) all.push_back(q);
                send(SDL_MOUSEBUTTONUP,pts[i+1].first,pts[i+1].second);
            }
            SDL_SetModState(KMOD_NONE);
            {SDL_Event e{};e.type=SDL_KEYUP;e.key.keysym.sym=SDLK_LSHIFT;e.key.keysym.mod=KMOD_NONE;input(e,winW_,winH_);}
            placing_=nullptr;selection_.clear();
            size_t moves=0,builds=0;
            for (const auto& o:world_.unit(builder)->orders) (o.buildType?builds:moves)+=1;
            int ticks=0;
            for (;ticks<54000;++ticks) {
                if (ticks%15==0) {
                    if (const auto& b=world_.player(localPlayer_).books; b.stored[0]<50000 || b.stored[1]<50000)
                        writableWorld().setStartResources(localPlayer_,1e6f,1e6f);
                }
                bool busy=!world_.unit(builder)->orders.empty();
                for (const auto& u:world_.units()) if (u.alive() && u.underConstruction) busy=true;
                if (!busy) break;
                simStep(1.0f/30.0f);
            }
            // The chain's distinct sites (a corner is the end of one line and the start of the next).
            std::vector<std::pair<int,int>> uniq;
            for (const auto& [x,z]:all) {
                const int ox=ta::sim::footprintOrigin(x,type->footX),oz=ta::sim::footprintOrigin(z,type->footZ);
                bool dup=false;
                for (const auto& [a,c]:uniq) if (std::abs(a-ox)<type->footX && std::abs(c-oz)<type->footZ) dup=true;
                if (!dup) uniq.push_back({ox,oz});
            }
            int built=0;std::string miss;
            for (const auto& [ox,oz]:uniq) {
                bool found=false;
                for (const auto& u:world_.units())
                    if (world_.finishedHere(u) && u.type==type &&
                        ta::sim::footprintOrigin(u.x,type->footX)==ox && ta::sim::footprintOrigin(u.z,type->footZ)==oz) found=true;
                if (found) ++built; else miss+=" "+std::to_string(ox)+","+std::to_string(oz);
            }
            const auto* b=world_.unit(builder);
            std::fprintf(stderr,"BUILDLINE_CHAIN %s: unique=%zu built=%d builds=%zu moves=%zu left=%zu ticks=%d builder=%.1f,%.1f%s%s\n",
                         spec,uniq.size(),built,builds,moves,b?b->orders.size():0,ticks,
                         b?b->x.toFloat()/16:0.f,b?b->z.toFloat()/16:0.f,miss.empty()?"":" miss:",miss.c_str());
            if (b && !b->orders.empty()) {   // where a stalled builder is stuck
                const auto& o=b->orders.front();
                std::fprintf(stderr,"BUILDLINE_CHAIN front order: %s at %.1f,%.1f (cells), goal=%d\n",
                             o.buildType?o.buildType->id.c_str():"move",o.x.toFloat()/16,o.z.toFloat()/16,int(o.goal));
            }
            if (built!=int(uniq.size())) std::exit(1);
            std::fprintf(stderr,"PASS: build-line chain, %d/%zu sites built\n",built,uniq.size());
            publish();
            return;
        }
        // Cursor picking (perf follow-up C) and a model that is not loaded yet. Registration
        // is budgeted (kRegistrationsPerFrame), so after a large arrival a unit can be live
        // before its type's model loads; a cursor pass that reaches unitUnderCursor for it
        // caches the fallback hit box for the type, for the rest of the game. A scan over
        // every unit always reaches it (nothing selected, the pointer on bare ground); the
        // grid's candidates must leave the same cache, so the type picks the same later.
        if (ta::devFlag("TA_PICK_LATE_MODEL_TEST")) {
            noFog_=true;edgeScrollOn_=false;
            winW_=1000;winH_=700;
            const auto publish=[&] {captureFrame();beginFrame();cosmeticStep(0);};
            const auto fail=[](const std::string& m) {throw std::runtime_error("late model pick: "+m);};
            const auto* filler=registry_.find("armpw");
            const ta::sim::UnitType* late=nullptr;
            const float lx=3000,lz=3000;
            // The structure with the largest footprint whose model is not loaded (first wins a tie).
            for (const auto& [id,type]:registry_.types())
                if (type.isStructure() && !type.ghost && id.find("null")==std::string::npos && !visuals_.count(id) &&
                    (!late || type.footX*type.footZ>late->footX*late->footZ) && world_.canPlace(&type,lx,lz))
                    late=&type;
            if (!filler || !late) fail("no fixture types");
            // The fillers come first in the live list and use up the registration budget.
            for (int i=0;i<kRegistrationsPerFrame+6;++i)
                if (writableWorld().spawn(filler,800+float(i%10)*40,800+float(i/10)*40,0.0f,localPlayer_)<=0)
                    fail("filler spawn failed");
            const int lateId=writableWorld().spawn(late,lx,lz,0.0f,localPlayer_);
            if (lateId<=0) fail("late spawn failed");
            mapView_.setZoom(1);
            mapView_.setOffset(400,600);   // the fillers, far from the late unit
            selection_.clear();pendingCmd_=0;placing_=nullptr;
            publish();
            if (unitType_.count(lateId) || visuals_.count(late->id)) fail("the late unit registered in the first frame");
            renderStudyMouse(600,60);   // bare ground above the fillers
            bool tint=false;
            const ta::CursorId first=desiredCursor(tint);
            const bool cached=hitBoxes_.count(late->id)!=0;
            for (int i=0;i<16 && !unitType_.count(lateId);++i) publish();
            if (!unitType_.count(lateId)) fail("the late unit never registered");
            mapView_.setOffset(lx-500,lz-350);
            publish();
            const UnitR* u=frameUnitP(lateId);
            if (!u) fail("late unit missing from the frame");
            const SDL_FPoint p=unitScreen(*u);
            const SDL_FRect& hb=unitHitBox(late);
            renderStudyMouse(p.x,p.y+12.0f-60.0f);   // 60 px above the anchor: past the fallback box
            const ta::CursorId probe=desiredCursor(tint);
            std::fprintf(stderr,"PICK_LATE_MODEL type=%s first=%d cached=%d box=%.1f,%.1f,%.1f,%.1f probe=%d\n",
                         late->id.c_str(),int(first),cached ? 1 : 0,hb.x,hb.y,hb.w,hb.h,int(probe));
            if (!cached) fail("the cursor pass left "+late->id+"'s hit box uncached, as a full scan does not");
            std::fprintf(stderr,"PASS: late model pick leaves the full scan's hit-box cache\n");
            return;
        }
        // Repeatable rendering workload for the shadow performance regression.
        // This entry point is only reached by the local development harness.
        // Render-optimization matrix knobs (tools/render_benchmark.py):
        //   TA_RENDER_CENTER=x,z   army centre (default 5000,5000)
        //   TA_RENDER_OFFSET=x,z   camera top-left world offset after setup
        //   TA_RENDER_EXTRAS=1     add construction sites, ghosts, flyers, corpses
        //   TA_RENDER_NAVAL=N      N floating units on the water nearest the centre
        //   TA_RENDER_FOREST=1     centre army + camera on the densest scenery region
        //   TA_RENDER_ROSTER=a,b   unit types replacing the 6-type roster ("structures": all of them)
        //   TA_RENDER_OVERLAYS=1   control squads, factories with production queues,
        //                           builders with queued build orders (ghosts)
        //   TA_RENDER_GHOST_QUEUES=B,Q[,T[,dx,dz]]  B commanders, each with Q queued (shift)
        //                           builds cycling through T small structure types (repeated
        //                           types), on a grid centred dx,dz from the army centre:
        //                           long queues, mostly off screen at zoom >= 0.5
        // All of them only shape the local harness scene; none run in a real game.
        // TA_BENCH_WIND=min,max: a bench world gets the map's wind (the match setup
        // that normally sets it does not run here), for wind generator animation checks.
        if (const char* w=ta::devEnv("TA_BENCH_WIND")) {
            int lo=100,hi=2000; std::sscanf(w,"%d,%d",&lo,&hi);
            writableWorld().setWindRange(lo,hi);
        }
        if (const char* countText=ta::devEnv("TA_SHADOW_BENCH")) {
            const int count=std::clamp(std::atoi(countText),1,16000);
            // The army stands at 5000,5000 (the render study's camera points and the
            // tests' sparse offsets are absolute): the ctests run on Krogoth Encounter
            // (12160 px square, dry land there).
            float centerX=5000,centerZ=5000;
            if(const char* center=ta::devEnv("TA_RENDER_CENTER"))
                std::sscanf(center,"%f,%f",&centerX,&centerZ);
            int forestTrees=0;
            if(ta::devFlag("TA_RENDER_FOREST")) {
                // Centre on the 512px region holding the most scenery (trees count
                // double, since they sway and carry shadows); first wins a tie.
                std::map<std::pair<int,int>,int> trees;
                for(const auto& f:features_)if(f.tex)trees[{int(f.x)/512,int(f.z)/512}]+=f.tree ? 2 : 1;
                for(const auto& [cell,n]:trees)if(n>forestTrees) {
                    forestTrees=n;centerX=float(cell.first)*512+256;centerZ=float(cell.second)*512+256;
                }
            }
            const char* only=ta::devEnv("TA_SHADOW_BENCH_TYPE");
            std::vector<std::string> roster={"armpw","corak","armmex","cormex","armthund","armrock"};
            if(const char* custom=ta::devEnv("TA_RENDER_ROSTER")) {
                // Comma-separated unit types replacing the 6-type roster.
                roster.clear();
                std::string list=custom;
                for(size_t at=0;at<=list.size();) {
                    const size_t comma=std::min(list.find(',',at),list.size());
                    if(comma>at)roster.push_back(list.substr(at,comma-at));
                    at=comma+1;
                }
                if(roster.size()==1 && roster[0]=="structures") {
                    // Every structure type, by id (workstream M: palette-lit models).
                    roster.clear();
                    for(const auto& [id,type]:registry_.types())if(type.isStructure())roster.push_back(id);
                    std::sort(roster.begin(),roster.end());
                    std::string list;
                    for(const auto& id:roster)list+=(list.empty() ? "" : ",")+id;
                    std::fprintf(stderr,"render roster: %s\n",list.c_str());
                }
                if(roster.empty())roster.push_back("armpw");
            }
            const int columns=std::max(1,int(std::ceil(std::sqrt(float(count)*1.6f))));
            const char* spacingText=ta::devEnv("TA_RENDER_SPACING");
            const float spacing=spacingText?std::clamp(float(std::atof(spacingText)),16.f,256.f):40.f;
            const int rows=(count+columns-1)/columns;
            // Interactive case (measurement B), TA_RENDER_COVER=type[,every]: a structure
            // of `type` (default armllt) on every `every`-th row and column of the army
            // grid (default 4), the army filling the rest of it around their footprints:
            // units stand just behind each structure (painted before it), the HIDDEN UNITS
            // cover workload under repeated buildings.
            std::vector<std::array<float,4>> coverRects;
            int covers=0;
            if(const char* coverText=ta::devEnv("TA_RENDER_COVER")) {
                std::string spec=coverText;
                int every=4;
                if(const size_t comma=spec.find(',');comma!=std::string::npos) {
                    every=std::clamp(std::atoi(spec.c_str()+comma+1),2,64);spec.resize(comma);
                }
                const auto* type=registry_.find(spec.empty() ? "armllt" : spec);
                if(!type || !isStructure(type))throw std::runtime_error("TA_RENDER_COVER: not a structure type: "+spec);
                for(int i=0;i<count;++i) {
                    if((i%columns)%every!=every/2 || (i/columns)%every!=every/2)continue;
                    const float x=centerX+(float(i%columns)-float(columns-1)*0.5f)*spacing;
                    const float z=centerZ+(float(i/columns)-float(rows-1)*0.5f)*spacing;
                    if(spawn(type->id,x,z,0,localPlayer_)<=0)continue;
                    ++covers;
                    const float hx=float(type->footX)*8+12,hz=float(type->footZ)*8+12;
                    coverRects.push_back({x-hx,z-hz,x+hx,z+hz});
                }
            }
            int spawned=0;
            std::vector<int> spawnedIds;
            for(int i=0;i<count;++i) {
                const char* name=only ? only : roster[size_t(i)%roster.size()].c_str();
                const auto* type=registry_.find(name);
                if(!type || world_.atUnitCap(localPlayer_) || world_.atTypeCap(localPlayer_,type)) continue;
                const float x=centerX+(float(i%columns)-float(columns-1)*0.5f)*spacing;
                const float z=centerZ+(float(i/columns)-float(rows-1)*0.5f)*spacing;
                if(std::any_of(coverRects.begin(),coverRects.end(),[&](const std::array<float,4>& r) {
                    return x>=r[0] && x<=r[2] && z>=r[1] && z<=r[3];
                })) continue;   // TA_RENDER_COVER: the structure stands here
                const bool battle=ta::devFlag("TA_RENDER_BATTLE");
                const int owner=battle?i%2:localPlayer_;
                if(const int id=spawn(name,x,z,float(i%8)*0.785398163f,owner)) {
                    ++spawned;spawnedIds.push_back(id);
                    if(ta::devFlag("TA_RENDER_MOTION"))writableWorld().order(id,x+320,z+160,false);
                    if(battle)writableWorld().attackMove(id,centerX,centerZ,false);
                }
            }
            int sites=0,ghosts=0,flyers=0,corpses=0,boats=0;
            if(ta::devFlag("TA_RENDER_EXTRAS")) {
                // Construction: commanders start real build sites south of the army.
                const float siteZ=centerZ+float(rows)*spacing*0.5f+160;
                for(int k=0;k<6;++k) {
                    const float bx=centerX+float(k-3)*160,bz=siteZ;
                    const int builder=spawn("armcom",bx,bz+96,0,localPlayer_);
                    if(builder<=0)continue;
                    for(const auto& option:registry_.buildable("armcom")) {
                        const auto* site=registry_.find(option);
                        if(!site || !isStructure(site) || site->footX>4 || site->footZ>4)continue;
                        if(writableWorld().startBuild(builder,site,bx,bz)) {++sites;break;}
                    }
                }
                // Translucent spectral bodies (type.ghost: Kingdoms data; TA has none) and
                // airborne units north of the army.
                const float northZ=centerZ-float(rows)*spacing*0.5f-160;
                for(const auto& [id,type]:registry_.types()) {
                    if(type.ghost && type.canMove && ghosts<8)
                        for(int k=0;k<8 && ghosts<8;++k)
                            ghosts+=spawn(id,centerX-480+float(k)*48,northZ,0,localPlayer_)>0;
                    if(type.canFly && type.canMove && flyers<16)
                        for(int k=0;k<4 && flyers<16;++k)
                            flyers+=spawn(id,centerX+120+float(flyers)*56,northZ-48,0,localPlayer_)>0;
                }
                // Corpses: every 9th grid unit dies at once and leaves its body.
                for(size_t k=0;k<spawnedIds.size();k+=9) {writableWorld().destroy(spawnedIds[k]);++corpses;}
            }
            int squads=0,factories=0,ghostQueues=0;
            if(ta::devFlag("TA_RENDER_OVERLAYS")) {
                // Per-unit overlays (workstream L): control squads on every fifth grid unit
                // (every third of those a formation), factories across the middle of the
                // army with long production queues (the units they make draw health
                // bars), and builders with queued build orders (ghosts).
                for(size_t k=0;k<spawnedIds.size();k+=5) {
                    const int n=int(k/5)%10+1;
                    writableWorld().setSquad(spawnedIds[k],(k/5)%3==2 ? -n : n);++squads;
                }
                for(const auto& [id,type]:registry_.types()) {
                    if(factories>=4)break;
                    if(!type.producesUnits())continue;
                    const ta::sim::UnitType* product=nullptr;
                    for(const auto& option:registry_.buildable(id))
                        if(const auto* t=registry_.find(option);t && !isStructure(t)) {product=t;break;}
                    if(!product)continue;
                    const float fx=centerX+(float(factories)-1.5f)*500,fz=centerZ;   // in the army's middle
                    if(const int factory=spawn(id,fx,fz,0,localPlayer_);factory>0) {
                        writableWorld().train(factory,product,50);++factories;
                    }
                }
                // Builders with queued (shift) build orders: the queued-build ghosts. A
                // queued build the map refuses is dropped (World::queueBuild ->
                // canPlaceOrder), and the Kingdoms-era spots (300 px south of the centre,
                // 600 px apart) are water and broken ground on Krogoth Encounter, and
                // inside TA's denser grids: each builder takes the nearest open row of
                // three sites to its spot instead (rings 128 px apart, clear of the other
                // builders' rows), so the scene holds three ghost queues on any map.
                const ta::sim::UnitType* queueSite=nullptr;
                for(const auto& option:registry_.buildable("armcom"))
                    if(const auto* t=registry_.find(option);t && isStructure(t) && t->footX<=4 && t->footZ<=4) {queueSite=t;break;}
                std::vector<std::pair<float,float>> queueRows;
                for(int k=0;k<3 && queueSite;++k) {
                    const float wantX=centerX+float(k-1)*600,wantZ=centerZ+300;
                    bool placed=false;
                    for(int ring=0;ring<=40 && !placed;++ring)
                        for(int step=0;step<std::max(1,ring*8) && !placed;++step) {
                            const float angle=float(step)*6.2831853f/float(std::max(1,ring*8));
                            const float bx=wantX+std::cos(angle)*float(ring)*128,bz=wantZ+std::sin(angle)*float(ring)*128;
                            if(std::any_of(queueRows.begin(),queueRows.end(),[&](const auto& r) {
                                return std::abs(r.first-bx)<480 && std::abs(r.second-bz)<320;
                            }))continue;
                            bool open=true;
                            for(int q=0;q<3 && open;++q)
                                open=world_.canPlaceOrder(queueSite,bx+float(q)*96,bz+160,localPlayer_);
                            if(!open || !world_.canPlace(registry_.find("armcom"),bx,bz,localPlayer_))continue;
                            const int builder=spawn("armcom",bx,bz,0,localPlayer_);
                            if(builder<=0)continue;
                            for(int q=0;q<3;++q)writableWorld().queueBuild(builder,queueSite,bx+float(q)*96,bz+160,true);
                            ghostQueues+=!world_.unit(builder)->orders.empty();
                            queueRows.emplace_back(bx,bz);
                            placed=true;
                        }
                }
            }
            int queueBuilders=0,queueOrders=0,queueTypes=0;
            if(const char* queues=ta::devEnv("TA_RENDER_GHOST_QUEUES")) {
                // Long queued-build lists (the build-ghost workload): builder k stands at the
                // west end of row k and queues its Q builds eastwards along the row, 128 px
                // apart (rows 192 px apart), types cycling through the first T small
                // structures the commander builds. Sites the map refuses are skipped. Each
                // builder is first sent 4000 px west (or to the map edge), so no build
                // starts during a study: the ghost count stays fixed and no conjuring
                // sparkle (whose particles differ run to run) lands in the captures.
                int builders=0,perQueue=0,typeCount=4;float dx=0,dz=0;
                std::sscanf(queues,"%d,%d,%d,%f,%f",&builders,&perQueue,&typeCount,&dx,&dz);
                builders=std::clamp(builders,0,64);perQueue=std::clamp(perQueue,0,256);
                std::vector<const ta::sim::UnitType*> sites;
                for(const auto& option:registry_.buildable("armcom"))
                    if(const auto* site=registry_.find(option);site && isStructure(site) && site->footX<=4 &&
                       site->footZ<=4 && int(sites.size())<std::max(1,typeCount))sites.push_back(site);
                queueTypes=int(sites.size());
                const float gx=centerX+dx,gz=centerZ+dz;
                for(int k=0;k<builders && !sites.empty();++k) {
                    const float rowZ=gz+(float(k)-float(builders-1)*0.5f)*192;
                    const float rowX=gx-float(perQueue-1)*0.5f*128;
                    const int builder=spawn("armcom",rowX-160,rowZ,0,localPlayer_);
                    if(builder<=0)continue;
                    ++queueBuilders;
                    writableWorld().order(builder,std::max(64.f,rowX-4160),rowZ,false);
                    for(int q=0;q<perQueue;++q) {
                        const size_t before=world_.unit(builder)->orders.size();
                        writableWorld().queueBuild(builder,sites[size_t(k+q)%sites.size()],
                                                   rowX+float(q)*128,rowZ,true);
                        queueOrders+=world_.unit(builder)->orders.size()>before;
                    }
                }
            }
            if(const char* navalText=ta::devEnv("TA_RENDER_NAVAL")) {
                const int wanted=std::clamp(std::atoi(navalText),1,2000);
                std::vector<const ta::sim::UnitType*> hulls;
                for(const auto& [id,type]:registry_.types())
                    if(type.floater && type.canMove && !type.canFly && !isStructure(&type))hulls.push_back(&type);
                const auto& map=mapView_.map();
                // Water cells nearest the centre, in a deterministic distance order.
                std::vector<std::pair<float,std::pair<int,int>>> water;
                for(int z=1;z+1<map.height;z+=3)for(int x=1;x+1<map.width;x+=3) {
                    const float wx=(float(x)+0.5f)*16,wz=(float(z)+0.5f)*16;
                    if(world_.isWater(wx,wz))
                        water.push_back({(wx-centerX)*(wx-centerX)+(wz-centerZ)*(wz-centerZ),{x,z}});
                }
                std::sort(water.begin(),water.end());
                for(size_t k=0;k<water.size() && boats<wanted && !hulls.empty();++k) {
                    const auto [x,z]=water[k].second;
                    boats+=spawn(hulls[size_t(boats)%hulls.size()]->id,(float(x)+0.5f)*16,(float(z)+0.5f)*16,
                                 float(boats%8)*0.785398163f,localPlayer_)>0;
                }
                if(!water.empty()) {
                    centerX=(float(water[0].second.first)+0.5f)*16;centerZ=(float(water[0].second.second)+0.5f)*16;
                }
            }
            noFog_=!ta::devFlag("TA_PROFILE_FOG");edgeScrollOn_=false;
            float zoom=std::min(1.5f,std::min(1400.f/(columns*40),760.f/(rows*40)));
            if(const char* requested=ta::devEnv("TA_PROFILE_ZOOM"))
                zoom=std::clamp(float(std::atof(requested)),0.05f,4.0f);   // the game's zoom range
            mapView_.setZoom(zoom);
            mapView_.setOffset(centerX-760/zoom,centerZ-terrainLift(centerX,centerZ)-420/zoom);
            if(const char* offset=ta::devEnv("TA_RENDER_OFFSET")) {
                float ox=0,oz=0;
                if(std::sscanf(offset,"%f,%f",&ox,&oz)==2)mapView_.setOffset(ox,oz);
            }
            // Interactive cases (measurement B, tools/render_benchmark.py --scenes
            // interactive). Presentation and local-harness state only, like the knobs
            // above; the frame-indexed pointer path is TA_RENDER_MOUSE (main.cpp).
            //   TA_RENDER_SELECT=N|all   the first N of the army's own units selected
            //   TA_RENDER_ARM=<key>      that order armed (pendingCmd_: a g r m f p c l u)
            //   TA_RENDER_GHOSTS=builders,queue[,offscreen%[,builder]]
            //                             builders (default armck) each with `queue`
            //                             queued (shift) build orders cycling over its
            //                             first three plain structure options; the first
            //                             offscreen% of every queue goes to the map corner
            //                             farthest from the view (the builders walk there
            //                             all run: their sites never start), the rest to
            //                             a 16x12 block (80 px apart) around the army
            //                             centre, one order a cell (a placed order blocks
            //                             another one over it). Use a tiny army.
            int ghostBuilders=0,ghostOrders=0,ghostOffscreen=0;
            if(const char* ghostText=ta::devEnv("TA_RENDER_GHOSTS")) {
                int builders=0,queue=0,offPercent=0;
                char builderName[64]="armck";
                if(std::sscanf(ghostText,"%d,%d,%d,%63s",&builders,&queue,&offPercent,builderName)<2)
                    throw std::runtime_error(std::string("TA_RENDER_GHOSTS: expected builders,queue[,offscreen%[,builder]], got ")+ghostText);
                builders=std::clamp(builders,1,256);queue=std::clamp(queue,1,200);offPercent=std::clamp(offPercent,0,100);
                std::vector<const ta::sim::UnitType*> options;
                for(const auto& option:registry_.buildable(builderName)) {
                    const auto* site=registry_.find(option);
                    if(site && isStructure(site) && site->footX<=4 && site->footZ<=4)options.push_back(site);
                    if(options.size()==3)break;
                }
                if(options.empty())throw std::runtime_error(std::string("TA_RENDER_GHOSTS: no plain structure option for ")+builderName);
                int outW=1280,outH=960;
                SDL_GetRendererOutputSize(ren_,&outW,&outH);
                const float viewW=float(mapViewW(outW))/mapView_.zoom(),viewH=float(std::max(1,outH-barH()))/mapView_.zoom();
                const float mapW=float(mapView_.map().width*16),mapH=float(mapView_.map().height*16);
                const float viewCX=mapView_.offX()+viewW*0.5f,viewCZ=mapView_.offY()+viewH*0.5f;
                const float farX=viewCX<mapW*0.5f ? mapW-700 : 700,farZ=viewCZ<mapH*0.5f ? mapH-700 : 700;
                const int offCount=queue*offPercent/100;
                const float blockX=centerX-900,blockZ=centerZ-420;
                for(int b=0;b<builders;++b) {
                    const float bx=centerX+(float(b%16)-7.5f)*64,bz=blockZ+12*80+40+float(b/16)*64;
                    const int builder=spawn(builderName,bx,bz,0,localPlayer_);
                    if(builder<=0)continue;
                    ++ghostBuilders;
                    for(int q=0;q<queue;++q) {
                        const auto* site=options[size_t(q)%options.size()];
                        float x,z;
                        if(q<offCount) {   // far corner, a 6-wide grid per builder
                            x=farX+(farX<mapW*0.5f ? 1.f : -1.f)*float((b*offCount+q)%6)*96;
                            z=farZ+(farZ<mapH*0.5f ? 1.f : -1.f)*float((b*offCount+q)/6%6)*96;
                        } else {
                            const int k=b*(queue-offCount)+(q-offCount);
                            x=blockX+float(k%16)*80;z=blockZ+float(k/16%12)*80;
                        }
                        const size_t before=world_.unit(builder)->orders.size();
                        writableWorld().queueBuild(builder,site,x,z,true);
                        if(world_.unit(builder)->orders.size()==before)continue;
                        ++ghostOrders;
                        // Off the setup view by more than the largest footprint (world px).
                        ghostOffscreen+=x<mapView_.offX()-96 || z<mapView_.offY()-96 ||
                                        x>mapView_.offX()+viewW+96 || z>mapView_.offY()+viewH+96;
                    }
                }
            }
            int selected=0;
            if(const char* selectText=ta::devEnv("TA_RENDER_SELECT")) {
                captureFrame();   // publish the army, or the next frame pin prunes the selection
                const size_t want=std::strcmp(selectText,"all")==0 ? spawnedIds.size() : size_t(std::max(0,std::atoi(selectText)));
                selection_.clear();
                for(int id:spawnedIds) {
                    if(selection_.size()>=want)break;
                    const auto* u=world_.unit(id);
                    if(u && u->alive() && u->player==localPlayer_)selection_.push_back(id);
                }
                selected=int(selection_.size());
            }
            if(const char* armText=ta::devEnv("TA_RENDER_ARM"))pendingCmd_=armText[0];
            if(covers || ghostBuilders || ta::devEnv("TA_RENDER_SELECT") || pendingCmd_)
                std::fprintf(stderr,"render interactive: selected=%d armed=%c covers=%d ghost_builders=%d ghost_orders=%d ghost_offscreen=%d\n",
                    selected,pendingCmd_ ? pendingCmd_ : '-',covers,ghostBuilders,ghostOrders,ghostOffscreen);
            std::fprintf(stderr,"shadow benchmark: requested=%d spawned=%d columns=%d zoom=%.3f type=%s\n",
                count,spawned,columns,zoom,only ? only : "mixed");
            std::fprintf(stderr,"render scene: sites=%d ghosts=%d flyers=%d corpses=%d boats=%d scenery=%d squads=%d factories=%d build_queues=%d center=%.0f,%.0f offset=%.0f,%.0f\n",
                sites,ghosts,flyers,corpses,boats,forestTrees,squads,factories,ghostQueues,centerX,centerZ,mapView_.offX(),mapView_.offY());
            if(ta::devEnv("TA_RENDER_GHOST_QUEUES"))
                std::fprintf(stderr,"render ghost queues: builders=%d queued=%d types=%d\n",queueBuilders,queueOrders,queueTypes);
            return;
        }
        // Capture the reclaim particle fix through the existing local build harness.
        if (ta::devFlag("TA_RECLAIM_CAPTURE")) {
            const char* name=ta::devEnv("TA_CONJURE_BUILDER");
            if (!name) name="armcom";
            const auto* type=registry_.find(name);
            if (!type || !type->canReclaim) return;
            for (const auto& feature:world_.features()) {
                if (!feature.alive || feature.energy<250) continue;
                const float x=feature.x.toFloat(),z=feature.z.toFloat();
                for (const auto& [dx,dz]:std::array<std::pair<float,float>,4>{
                         {{-96,0},{96,0},{0,-96},{0,96}}}) {
                    if (!world_.canPlace(type,x+dx,z+dz)) continue;
                    const int id=spawn(name,x+dx,z+dz,0,localPlayer_);
                    ta::net::Command command;
                    command.kind=ta::net::Cmd::Reclaim;command.player=localPlayer_;
                    command.unitId=id;command.targetId=feature.id;
                    ta::sim::applyCommand(writableWorld(),registry_,command);
                    selection_={id};noFog_=true;edgeScrollOn_=false;
                    mapView_.setZoom(3);
                    mapView_.setOffset(x+dx-500/3.f,
                        z+dz-terrainLift(x+dx,z+dz)-400/3.f);
                    std::printf("reclaim capture: %s #%d at %.0f,%.0f -> feature %d at %.0f,%.0f\n",
                        name,id,x+dx,z+dz,feature.id,x,z);
                    return;
                }
            }
            std::fprintf(stderr,"reclaim capture: no suitable feature/site\n");
            return;
        }
        // TA_ANIM_TEST: unit animation captures (--testbuild --time N --shot, with
        // TA_PIECELOG=<ids> for the display VM's poses; advance() runs the display VMs):
        //   factory:<factory>:<unit>  a finished factory building three of <unit>
        //                             (its output turns with the build pad)
        //   move:<unit>               a mobile unit driven 320 px east and back
        //   air:<flyer>               a landed flyer sent 320 px east; it lands there
        if (const char* spec=ta::devEnv("TA_ANIM_TEST")) {
            std::vector<std::string> part;
            for (std::string s(spec);;) {
                const size_t colon=s.find(':');
                part.push_back(s.substr(0,colon));
                if (colon==std::string::npos) break;
                s=s.substr(colon+1);
            }
            const float px=1920,pz=1616;
            float lookX=px,lookZ=pz;
            writableWorld().setStartResources(localPlayer_,100000,100000);
            if (ta::devFlag("TA_ANIM_LEGION")) {   // Legion pathfinding, as a match that picks it
                writableWorld().setPathfindingMode(ta::sim::PathfindingMode::Legion);
                writableWorld().setPathService(true);
            }
            if (part[0]=="factory" && part.size()==3 && registry_.find(part[1]) && registry_.find(part[2])) {
                const int factory=spawn(part[1],px,pz,0,localPlayer_);
                writableWorld().train(factory,registry_.find(part[2]),3);
                animFollowId_=factory;
                std::printf("anim test: factory #%d (%s) building %s\n",factory,part[1].c_str(),part[2].c_str());
            } else if ((part[0]=="move" || part[0]=="air") && part.size()==2 && registry_.find(part[1])) {
                const int id=spawn(part[1],px-160,pz,float(M_PI/2),localPlayer_);
                writableWorld().order(id,px+160,pz,false);
                if (part[0]=="move") writableWorld().order(id,px-160,pz,true);
                animFollowId_=id;
                std::printf("anim test: %s #%d\n",part[1].c_str(),id);
            } else { std::fprintf(stderr,"anim test: bad spec %s\n",spec); return; }
            float zoom=2.0f;
            if (const char* value=ta::devEnv("TA_ANIM_ZOOM")) zoom=std::max(0.25f,float(std::atof(value)));
            noFog_=true;edgeScrollOn_=false;
            mapView_.setZoom(zoom);
            mapView_.setOffset(lookX-640/zoom,lookZ-terrainLift(lookX,lookZ)-400/zoom);
            return;
        }
        // TA_AIRPAD_CAPTURE=<pad type>: a repair pad and a fighter at 40% health on
        // patrol past it; the fighter lands on the pad and is mended (SelfRepair):
        // the rider's pose and the pad's nano spray (--shot while it sits there).
        if (const char* padType=ta::devEnv("TA_AIRPAD_CAPTURE")) {
            const auto* pad=registry_.find(padType);
            const auto* fig=registry_.find("armfig");
            if (!pad || !fig) { std::fprintf(stderr,"airpad capture: unknown %s\n",padType); return; }
            const float px=1920,pz=1616;
            const int padId=spawn(pad->id,px,pz,0,localPlayer_);
            const int figId=spawn(fig->id,px+200,pz+120,0,localPlayer_);
            if (auto* u=writableWorld().unit(figId)) u->hp=ta::sim::Fixed::fromFloat(float(fig->maxHp)*0.4f);
            writableWorld().patrolTo(figId,px+600,pz+120,false);
            std::printf("airpad capture: pad #%d (%s), fighter #%d\n",padId,padType,figId);
            noFog_=true;edgeScrollOn_=false;
            mapView_.setZoom(3.0f);
            mapView_.setOffset(px-500/3.0f,pz-terrainLift(px,pz)-400/3.0f);
            return;
        }
        if (ta::devFlag("TA_CONJURE_TEST")) {
            const char* builder=ta::devEnv("TA_CONJURE_BUILDER");
            const char* target=ta::devEnv("TA_CONJURE_TARGET");
            const char* builderXEnv=ta::devEnv("TA_CONJURE_BUILDER_X");
            const char* builderZEnv=ta::devEnv("TA_CONJURE_BUILDER_Z");
            const char* siteXEnv=ta::devEnv("TA_CONJURE_SITE_X");
            const char* siteZEnv=ta::devEnv("TA_CONJURE_SITE_Z");
            if (!builder) builder="armck";
            if (!target) target="armllt";
            if (!registry_.find(builder) || !registry_.find(target)) {
                std::fprintf(stderr,"conjure fixture: unknown builder %s or target %s\n",builder,target);
                return;
            }
            auto parseFloat=[](const char* name,const char* value,float& out) {
                char* end=nullptr;
                out=std::strtof(value,&end);
                if (end==value || !end || *end || !std::isfinite(out)) {
                    std::fprintf(stderr,"conjure fixture: invalid %s value '%s'\n",name,value);
                    return false;
                }
                return true;
            };
            if (bool(builderXEnv)!=bool(builderZEnv) || bool(siteXEnv)!=bool(siteZEnv)) {
                std::fprintf(stderr,"conjure fixture: builder/site coordinates require both X and Z\n");
                return;
            }
            const bool explicitBuilder=builderXEnv && builderZEnv;
            const bool explicitSite=siteXEnv && siteZEnv;
            float builderX=1920,builderZ=1616,siteX=0,siteZ=0;
            if (explicitBuilder &&
                (!parseFloat("TA_CONJURE_BUILDER_X",builderXEnv,builderX) ||
                 !parseFloat("TA_CONJURE_BUILDER_Z",builderZEnv,builderZ))) return;
            if (explicitSite &&
                (!parseFloat("TA_CONJURE_SITE_X",siteXEnv,siteX) ||
                 !parseFloat("TA_CONJURE_SITE_Z",siteZEnv,siteZ))) return;
            float zoom=3.0f,screenX=500,screenY=400;
            if (const char* value=ta::devEnv("TA_CONJURE_ZOOM"))
                if (!parseFloat("TA_CONJURE_ZOOM",value,zoom) || zoom<=0) return;
            const char* screenXEnv=ta::devEnv("TA_CONJURE_SCREEN_X");
            const char* screenYEnv=ta::devEnv("TA_CONJURE_SCREEN_Y");
            if (bool(screenXEnv)!=bool(screenYEnv)) {
                std::fprintf(stderr,"conjure fixture: screen coordinates require both X and Y\n");
                return;
            }
            if (screenXEnv &&
                (!parseFloat("TA_CONJURE_SCREEN_X",screenXEnv,screenX) ||
                 !parseFloat("TA_CONJURE_SCREEN_Y",screenYEnv,screenY))) return;
            int id=0;
            if (!explicitBuilder && ta::devEnv("TA_CONJURE_BUILDER"))
                for (const auto& u:world_.units())
                    if (u.alive() && u.player==localPlayer_ && u.type==registry_.find(builder)) {
                        id=u.id;break;
                    }
            if (!id) id=spawn(builder,builderX,builderZ,0,0);
            const auto* fixtureBuilder=world_.unit(id);
            if (!fixtureBuilder) return;
            const float originX=fixtureBuilder->x.toFloat()+128;
            const float originZ=fixtureBuilder->z.toFloat();
            if (!explicitSite) { siteX=originX;siteZ=originZ; }
            const auto* targetType=registry_.find(target);
            if (explicitSite) {
                if (!world_.canPlace(targetType,siteX,siteZ)) {
                    std::fprintf(stderr,"conjure fixture: no legal site for %s at %.3f,%.3f\n",
                                 target,siteX,siteZ);
                    return;
                }
            } else if (ta::devEnv("TA_CONJURE_TARGET")) {
                bool found=false;
                for (int r=0;r<=640 && !found;r+=32)
                    for (int dz=-r;dz<=r && !found;dz+=32)
                        for (int dx=-r;dx<=r && !found;dx+=32) {
                            if (std::max(std::abs(dx),std::abs(dz))!=r) continue;
                            if (world_.canPlace(targetType,originX+dx,originZ+dz)) {
                                siteX=originX+dx;siteZ=originZ+dz;found=true;
                            }
                        }
                if (!found) {
                    std::fprintf(stderr,"conjure fixture: no legal site for %s\n",target);
                    return;
                }
            }
            if (ta::devFlag("TA_CONJURE_REPAIR")) {
                const int targetId=spawn(target,siteX,siteZ,0,localPlayer_);
                writableWorld().unit(targetId)->hp=ta::sim::Fixed::fromFloat(targetType->maxHp*0.5f);
                ta::net::Command command;command.kind=ta::net::Cmd::Repair;
                command.unitId=id;command.targetId=targetId;command.player=localPlayer_;
                ta::sim::applyCommand(writableWorld(),registry_,command);
                std::printf("repair capture: worker #%d -> target #%d\n",id,targetId);
            } else if (const char* what=ta::devEnv("TA_CONJURE_RECLAIM")) {
                // TA_CONJURE_RECLAIM=feature|unit: the worker reclaims the nearest
                // live map feature, or a <target> spawned at the site (the nano
                // spray's reverse, 0x472200; --shot while it works).
                ta::net::Command command;command.unitId=id;command.player=localPlayer_;
                if (std::string(what)=="unit") {
                    command.kind=ta::net::Cmd::ReclaimUnit;
                    command.targetId=spawn(target,siteX,siteZ,0,localPlayer_);
                } else {
                    command.kind=ta::net::Cmd::Reclaim;
                    float best=1e30f;
                    for (const auto& f:features_) {
                        if (f.simId<0 || !world_.featureAliveAt(f.x,f.z)) continue;
                        const float d=std::hypot(f.x-originX,f.z-originZ);
                        if (d<best) { best=d;command.targetId=f.simId;siteX=f.x;siteZ=f.z; }
                    }
                    if (best>1e29f) { std::fprintf(stderr,"conjure fixture: no feature to reclaim\n");return; }
                }
                ta::sim::applyCommand(writableWorld(),registry_,command);
                std::printf("reclaim capture: worker #%d -> %s %d\n",id,what,command.targetId);
            } else if (const char* value=ta::devEnv("TA_CONJURE_REMAINING")) {
                // TA_CONJURE_REMAINING=<f>: the site placed at once with that much of
                // it left to build (1 = just placed), so --shot catches the nanoframe
                // at a chosen stage (client/nanoframe.h) without waiting for the walk.
                float remaining=1;
                if (!parseFloat("TA_CONJURE_REMAINING",value,remaining) || remaining<=0 || remaining>1) return;
                const int siteId=writableWorld().startBuild(id,targetType,siteX,siteZ);
                if (auto* site=siteId>0 ? writableWorld().unit(siteId) : nullptr) {
                    site->buildRemaining=remaining;site->buildStarted=site->buildBegun=true;
                }
                std::printf("conjure fixture: site #%d at %.3f remaining\n",siteId,remaining);
            } else writableWorld().queueBuild(id,targetType,siteX,siteZ,false);
            // TA_CONJURE_METAL=<n>: start the local metal and energy at n, so a capture
            // can show the bars short of resources.
            if (const char* value=ta::devEnv("TA_CONJURE_METAL")) {
                float amount=0;
                if (!parseFloat("TA_CONJURE_METAL",value,amount) || amount<0) return;
                for (float& r:writableWorld().player(localPlayer_).books.stored) r=amount;
            }
            if (const auto* u=world_.unit(id))
                std::printf("conjure fixture: %s #%d at %.0f,%.0f -> %s at %.0f,%.0f; orders=%zu\n",
                            builder,id,u->x.toFloat(),u->z.toFloat(),target,siteX,siteZ,u->orders.size());
            selection_={id};
            noFog_=true;edgeScrollOn_=false;
            mapView_.setZoom(zoom);
            mapView_.setOffset(siteX-screenX/zoom,
                               siteZ-terrainLift(siteX,siteZ)-screenY/zoom);
            return;
        }
        const auto* keep = world_.unit(keepId_);
        if (!keep) return;
        const auto* lode = registry_.find("armsolar");
        // Probe outward from the keep for the first legal site.
        for (float r = 90; r < 400; r += 24) {
            for (float a = 0; a < 6.28f; a += 0.5f) {
                float x = keep->x.toFloat() + std::cos(a) * r, z = keep->z.toFloat() + std::sin(a) * r;
                if (world_.canPlace(lode, x, z)) {
                    int id = writableWorld().startBuild(builderId_, lode, x, z);
                    std::printf("testbuild: site id %d at %.0f,%.0f\n", id, x, z);
                    return;
                }
            }
        }
        std::printf("testbuild: no site found\n");
    }

    void GameView::soundTest() {
        int id = spawn("armpw", 900, 1000, 0, 0);
        selection_ = {id};
        for (int i = 0; i < 8; i++) voice(id, "move");
    }

    void GameView::faceTest() {
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        // Four Peewees walking out N/E/S/W, arrows on.
        float ddx[4]={0,400,0,-400}, ddz[4]={-400,0,400,0};
        for (int i=0;i<4;i++){
            int id=spawn("armpw", cx+ddx[i]*0.15f, cz+ddz[i]*0.15f, 0, 0);
            writableWorld().order(id, cx+ddx[i], cz+ddz[i], false);
        }
        mapView_.setOffset(cx - 640 / mapView_.zoom(), cz - 400 / mapView_.zoom());
    }

    void GameView::fireTest() {
        if(ta::devFlag("TA_PALETTE_ROSTER_TEST")) {
            std::set<std::string> names;
            const auto gather=[&](const auto& self,const PieceMeta& meta)->void {
                if(!meta.skip)names.insert(meta.primTex.begin(),meta.primTex.end());
                for(const auto& child:meta.children)self(self,child);
            };
            size_t structures=0;
            for(const auto& [name,type]:registry_.types())if(type.isStructure()) {
                if(loadVisual(name)) {gather(gather,visuals_.at(name).meta);++structures;}
            }
            size_t pixels=0,frames=0,missing=0;
            for(const auto& name:names) {
                auto source=paletteTextureSources_.find(name);
                if(source==paletteTextureSources_.end())continue;
                for(const auto& frame:source->second.frames) {
                    pixels+=size_t(frame.width)*frame.height;++frames;
                }
            }
            std::fprintf(stderr,"PALETTE_ROSTER structures=%zu textures=%zu frames=%zu rawMiB=%.2f\n",
                structures,names.size(),frames,double(pixels*20*4)/(1024*1024));
            for(const auto& name:names) {
                preparePaletteTexture(name);
                auto sheet=paletteTextureSheets_.find(name);
                if(sheet!=paletteTextureSheets_.end())for(const auto& frame:sheet->second)
                    missing+=!frame.texture;
            }
            // The pre-M source record held a palette copy, a shade-table copy and RGBA
            // frames per texture: its byte count for the same sources (excluding names
            // and sheet records, which are unchanged).
            size_t before=0,after=0;
            for(const auto& [n,source]:paletteTextureSources_) {
                before+=sizeof(ta::gaf::Palette)+ta::render::palette::kShadeTableBytes+
                        source.frames.size()*sizeof(ta::gaf::Frame);
                after+=source.frames.capacity()*sizeof(ta::render::palette::Frame);
                for(const auto& frame:source.frames) {before+=size_t(frame.width)*frame.height*4;after+=frame.la.capacity();}
            }
            for(const auto& bank:paletteBanks_)after+=sizeof(bank)+bank.shades.capacity();
            std::fprintf(stderr,"PALETTE_ROSTER pages=%zu missing=%zu mode=%s gpuMiB=%.2f cpuMiB=%.2f uploadMiB=%.2f "
                "sources=%zu sourceMiB=%.2f preMSourceMiB=%.2f\n",
                palettePages_.size(),missing,paletteMode_==PaletteMode::Indexed ? "indexed" : "baked",
                double(paletteGpuBytes())/(1024*1024),double(paletteCpuBytes())/(1024*1024),
                double(paletteUploadBytes_)/(1024*1024),paletteTextureSources_.size(),
                double(after)/(1024*1024),double(before)/(1024*1024));
            if(missing)throw std::runtime_error("shipped structure palette sheets exceed capacity");
            std::fprintf(stderr,"PASS: full structure palette texture roster fits bounded pages\n");
            return;
        }
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        if (ta::devFlag("TA_TRANSPORT_EFFECT_TEST")) {
            ta::sim::World::TransportFx event;
            event.tick=front().gameTick-4;
            event.passenger={int32_t(cx*65536),101*65536+32768,int32_t(cz*65536)};
            event.carrier={int32_t((cx+80)*65536),181*65536+32768,int32_t(cz*65536)};
            const auto before=effects_.size();
            transportEffectQueue_.push_back(event);
            cosmeticStep(0);
            if (!transportEffectQueue_.empty() || effects_.size()!=before+2)
                throw std::runtime_error("transport event did not create two loaded effects");
            const auto passenger=effects_[before],carrier=effects_[before+1];
            if (passenger.worldPosition!=event.passenger || carrier.worldPosition!=event.carrier ||
                passenger.started!=event.tick || carrier.started!=event.tick ||
                !passenger.authoredTiming || !carrier.authoredTiming)
                throw std::runtime_error("transport effects lost captured position or event tick");
            cosmeticStep(0);
            if (effects_.size()!=before+2)
                throw std::runtime_error("transport event was consumed more than once");
            effects_.clear();
            for (auto sample:{passenger,carrier}) {
                uint32_t duration=0;
                for (auto delay:sample.anim->durations) duration+=std::max(1u,unsigned(delay));
                for (uint32_t age:{0u,duration-1,duration,duration+20}) {
                    sample.started=front().gameTick-age;
                    effects_.push_back(sample);updateEffects(0);
                    if (effects_.empty()!=(age>=duration))
                        throw std::runtime_error("transport effect expiry differs from authored duration");
                    effects_.clear();
                }
            }
            effects_.push_back(passenger);effects_.push_back(carrier);
            // Exercise the actual draw paths across a passenger's visibility
            // transition. This is separate from detached transport-beam effects.
            const int passengerId=spawn("armpw",cx,cz,0,localPlayer_);
            const int carrierId=spawn("armatlas",cx+80,cz,0,localPlayer_);
            const auto* art=effectFor("bigsmoke");
            if(passengerId<0 || carrierId<0 || !art || art->frames.empty())
                throw std::runtime_error("transport attached-effect assets unavailable");
            smokeSprites_.clear();pointParticles_.clear();damageFlames_.clear();
            const std::array<int32_t,3> point{int32_t(cx*65536),0,int32_t(cz*65536)};
            ta::RetailSmokeParticle smoke;smoke.position=point;
            smokeSprites_[passengerId].push_back({smoke,art});
            ta::RetailPointParticle wake;wake.position=point;
            pointParticles_[passengerId].push_back(wake);
            DamageFlameSprite flame;flame.position=point;flame.art=art;flame.clock.start(art->durations);
            damageFlames_[passengerId].push_back(flame);
            int width=0,height=0;SDL_GetRendererOutputSize(ren_,&width,&height);
            winW_=width;winH_=height; // draw normally sets these before culling
            mapView_.setZoom(1);
            mapView_.setOffset(cx-float(mapViewW(width))*0.5f,cz-float(height-barH())*0.5f);
            for(bool aboard:{false,true,false}) {
                writableWorld().unit(passengerId)->inTransport=aboard ? carrierId : 0;
                captureFrame();beginFrame();endFrame();
                collectWorldFx(mapViewW(width),height,false);drawLateFx();
                const int expected=aboard ? 0 : 1;
                if(debugSmokeDrawCount_!=expected || debugPointDrawCount_!=expected ||
                   debugDamageFlameDrawCount_!=expected) {
                    std::fprintf(stderr,"cargo effects: aboard=%d smoke=%d points=%d flames=%d\n",
                        int(aboard),debugSmokeDrawCount_,debugPointDrawCount_,debugDamageFlameDrawCount_);
                    throw std::runtime_error("passenger attached effects did not hide aboard and resume after unloading");
                }
            }
            if(smokeSprites_[passengerId].size()!=1 || pointParticles_[passengerId].size()!=1 ||
               damageFlames_[passengerId].size()!=1)
                throw std::runtime_error("boarding discarded attached effect lifetime state");
            // A passenger destroyed aboard must not spawn visible death
            // sprites/bursts at the carrier's position or run its death scripts.
            writableWorld().unit(passengerId)->inTransport=carrierId;
            writableWorld().unit(passengerId)->deadFor=0;
            captureFrame();beginFrame();endFrame();
            const auto effectsBefore=effects_.size(),particlesBefore=particles_.size();
            cosmeticStep(0);
            if(effects_.size()!=effectsBefore || particles_.size()!=particlesBefore)
                throw std::runtime_error("hidden destroyed cargo spawned ordinary death effects");
            // Control: an ordinary unembarked Peewee also gets no fabricated
            // body blast before its authored Killed/Dying scripts are advanced.
            const int ordinary=spawn("armpw",cx+40,cz,0,localPlayer_);
            if(ordinary<0)throw std::runtime_error("ordinary death control failed to spawn");
            writableWorld().unit(ordinary)->deadFor=0;
            writableWorld().unit(ordinary)->deathType=1;
            captureFrame();beginFrame();endFrame();
            const auto ordinaryEffects=effects_.size(),ordinaryParticles=particles_.size();
            cosmeticStep(0);
            if(effects_.size()!=ordinaryEffects || particles_.size()!=ordinaryParticles)
                throw std::runtime_error("ordinary death fabricated effects outside its authored script");
            std::fprintf(stderr,"PASS: transport effects, cargo hide/release and authored deaths\n");
            return;
        }
        if(ta::devEnv("TA_FEATURE_CACHE_TEST")) {
            auto definition = featureDefs_.at("tree1");
            definition.values["animating"] = "0";

            const auto* standing = featureArtFor(definition);
            definition.values["seqnameburn"] = definition.valueOr("seqname", "");
            definition.values["seqnameburnshad"] = definition.valueOr("seqnameshad", "");
            const auto* burning = featureArtFor(definition, "seqnameburn", "seqnameburnshad");
            definition.values["animating"] = "1";
            const auto* animated = featureArtFor(definition);
            definition.values["seqnameshad"] = "";
            const auto* unshadowed = featureArtFor(definition);
            if (!standing || !burning || !animated || !unshadowed ||
                standing == burning || standing == animated || animated == burning ||
                animated == unshadowed || burning->shadowLoop ||
                !unshadowed->shadowFrames.empty() || featureArtFor(definition) != unshadowed)
                throw std::runtime_error("feature art cache aliases distinct playback/shadow definitions");
            std::fprintf(stderr, "PASS: feature cache separates static, animated, burn and shadow variants; repeated lookup reuses art\n");
            return;
        }
        if(ta::devEnv("TA_FEATURE_SMOKE_QUEUE_TEST")) {
            const auto definition=featureDefs_.find("tree1");
            const auto* art=effectFor("bigsmoke");
            if(definition==featureDefs_.end() || !art)throw std::runtime_error("feature smoke fixture art unavailable");
            smokeTickQueue_.clear();smokeSprites_.clear();featureSmokeSprites_.clear();
            smokeTick_=front().gameTick-4;
            ta::RetailSmokeParticle unitSmoke;unitSmoke.frameLimit=100;
            smokeSprites_[77].push_back({unitSmoke,art});
            SmokeTick first;first.tick=front().gameTick-3;
            for(int i=0;i<12;++i)first.features.push_back({77,"tree1",{0,0,0},0});
            first.features.push_back({78,"tree1",{0,0,0},0});
            smokeTickQueue_.push_back(first);consumeSmokeTicks();
            if(featureSmokeSprites_[77].size()!=10 || featureSmokeSprites_[78].size()!=1)
                throw std::runtime_error("feature smoke capacity or ownership mismatch");
            const auto before=featureSmokeSprites_[78][0].particle.position;
            const auto countdown=featureSmokeSprites_[78][0].particle.countdown;
            SmokeTick second;second.tick=front().gameTick-2;second.windX=123;second.windZ=-45;
            // Active burns send a tick event even when they emit no new smoke.
            // Finished burns stop advancing their feature-owned particles.
            second.features.push_back({78,"tree1",{0,0,0},0});
            second.features.back().emit=false;
            SmokeTick third=second;third.tick=front().gameTick-1;third.removedFeatures={77};
            smokeTickQueue_.push_back(second);smokeTickQueue_.push_back(third);consumeSmokeTicks();
            const auto after=featureSmokeSprites_[78][0].particle;
            if(featureSmokeSprites_.contains(77) || smokeSprites_[77].size()!=1 ||
               after.countdown!=countdown-2 ||
               uint32_t(after.position[0])!=uint32_t(before[0])+uint32_t(123*16) ||
               uint32_t(after.position[2])!=uint32_t(before[2])+uint32_t(-45*16))
                throw std::runtime_error("feature smoke delayed tick/retirement isolation failed");
            consumeSmokeTicks();
            if(featureSmokeSprites_[78][0].particle.position!=after.position)
                throw std::runtime_error("feature smoke queue was consumed twice");
            featureSmokeSprites_.clear();smokeSprites_.clear();
            std::fprintf(stderr,"PASS: feature smoke cap10, delayed tick motion, single consumption and unit/feature retirement isolation\n");
            return;
        }
        if(ta::devEnv("TA_DEBRIS_TEST") || ta::devEnv("TA_BLOOD_TEST")) {
            edgeScrollOn_=false;follow_=false;initialCamera_.reset();
            Visual visual;auto& root=visual.model.root;root.name="piece";
            root.vertices={-30,0,0,30,0,0,0,50,0};
            root.primitives={{200,"",{0,1,2}},{200,"",{2,1,0}}};
            visuals_["__debris_fixture"]=std::move(visual);
            unitType_.set(-123,"__debris_fixture");
            UnitR unit;unit.id=-123;unit.x=cx;unit.z=cz;
            unit.worldPosition={int32_t(cx*65536),int32_t((rawHeight(cx,cz)+100)*65536),int32_t(cz*65536)};
            const std::vector<std::string> names={"piece"};
            std::vector<ta::cob::PieceState> poses(1);
            Anim snapshot;snapshot.pieceNames=&names;snapshot.capturedPose=poses;
            ta::cob::File bloodScript;bloodScript.scripts={{"QueryBlood",0}};
            bloodScript.code={0x10021001,0,0x10023002,0,0x10021001,0,0x10065000};
            ta::cob::Vm bloodVm(std::move(bloodScript),true);bloodVm.enableRetailAnimation();
            ta::sim::UnitType bloodType;unit.type=&bloodType;snapshot.effectQueryVm=&bloodVm;
            if(!explodePiece(unit,snapshot,0,0))
                throw std::runtime_error("detached model admission rejected an empty pool");
            if(detachedPieces_.size()!=1 || detachedPieces_[0].model.vertices.size()!=9 ||
               !particles_.empty())throw std::runtime_error("detached model creation failed");
            if(detachedPieces_[0].blood.size()!=100)
                throw std::runtime_error("QueryBlood did not attach 100 particles");
            (void)heightAbove(cx,cz);
            const bool savedShadowFrame=shadowsOnFrame_;
            shadowsOnFrame_=true;
            buildDetachedShadow(detachedPieces_[0],2.0f);
            if(detachedShadowVerts_.size()<3 || detachedShadowVerts_.size()%3)
                throw std::runtime_error("detached model did not produce retail-projected shadow triangles");
            shadowsOnFrame_=false;
            buildDetachedShadow(detachedPieces_[0],2.0f);
            if(!detachedShadowVerts_.empty() || !detachedMaskedShadows_.empty())
                throw std::runtime_error("detached model ignored the unit-shadow toggle");
            shadowsOnFrame_=savedShadowFrame;
            while(detachedPieces_.size()<100)detachedPieces_.push_back(detachedPieces_.front());
            if(explodePiece(unit,snapshot,0,0) || detachedPieces_.size()!=100)
                throw std::runtime_error("detached model admission exceeded the retail pool");
            detachedPieces_.resize(1);
            visuals_.erase("__debris_fixture");unitType_.erase(-123);
            if(detachedPieces_[0].model.vertices[0]!=-30)
                throw std::runtime_error("detached geometry depended on source lifetime");
            if(ta::devEnv("TA_BLOOD_TEST")) {
                mapView_.setZoom(2);
                int width,height;SDL_GetRendererOutputSize(ren_,&width,&height);
                mapView_.setOffset(cx-float(mapViewW(width))/4,cz-float(height-barH())/4);
                std::fprintf(stderr,"PASS: live blood fixture created 100 script-enabled particles\n");
                return;
            }
            auto impact=detachedPieces_[0];
            impact.tick=front().gameTick-1;impact.motion.flags=16;
            // TotalA.exe 0x421450: a contact bounces at half speed and ends only
            // once the bounce is under 2 px a tick: land at 3 px a tick.
            {
                const auto& terrain=mapView_.map();
                const int ground=ta::retailDebrisTerrainHeight(impact.motion.position[0],impact.motion.position[2],
                    terrain.width,terrain.height,[&](int x,int z) {
                        return terrain.heights[size_t(z)*size_t(terrain.width)+size_t(x)];
                    });
                impact.motion.position[1]=(ground+1)*65536;
            }
            impact.motion.velocity={0,-3*65536,0};
            const auto position=impact.motion.position;
            detachedPieces_.push_back(std::move(impact));
            const auto glows=explosionGlows_.size();
            detachedPieces_[0].tick=front().gameTick-1;
            for(auto& particle:detachedPieces_[0].blood)particle.velocity[1]=-150*65536;
            updateEffects(0);
            // First contact below sea leaves no stain; use a separate above-sea
            // contact case for persistent marks in the particle oracle.
            if(!detachedPieces_[0].blood.empty())
                throw std::runtime_error("blood particles failed ground removal");
            if(detachedPieces_.size()!=1 || explosionGlows_.size()!=glows+1 ||
               explosionGlows_.back().worldPosition!=position ||
               explosionGlows_.back().started!=front().gameTick ||
               explosionGlows_.back().kind!=0)
                throw std::runtime_error("debris ground impact lost its positioned glow");
            const bool savedLava=debrisLavaWorld_,savedNoSea=debrisNoSeaTrigger_;
            for(bool lava:{false,true})for(bool suppressed:{false,true}) {
                debrisLavaWorld_=lava;debrisNoSeaTrigger_=suppressed;
                auto splash=detachedPieces_[0];splash.blood.clear();
                splash.tick=front().gameTick-1;splash.motion.flags=16;
                splash.motion.position[1]=int32_t(uint8_t(mapView_.map().seaLevel))*65536;
                const auto splashPosition=splash.motion.position;
                detachedPieces_.push_back(std::move(splash));
                const auto before=effects_.size(),beforeGlow=explosionGlows_.size();
                updateEffects(0);
                if(detachedPieces_.size()!=1 || effects_.size()!=before+size_t(!suppressed) ||
                   explosionGlows_.size()!=beforeGlow)
                    throw std::runtime_error("debris splash suppression/glow mismatch");
                if(!suppressed) {
                    const bool matched=effects_.back().anim==
                        effectFor(lava ? "fx:lavasplash" : "fx:h2oboom2","palettes/palette.pal");
                    if(!matched || effects_.back().worldPosition!=splashPosition ||
                       effects_.back().started!=front().gameTick)
                        throw std::runtime_error("debris splash class/position/tick mismatch");
                }
            }
            debrisLavaWorld_=savedLava;debrisNoSeaTrigger_=savedNoSea;
            mapView_.setZoom(2);
            int width,height;SDL_GetRendererOutputSize(ren_,&width,&height);
            mapView_.setOffset(cx-float(mapViewW(width))/4,cz-float(height-barH())/4);
            std::fprintf(stderr,"PASS: production EXPLODE retains independent model geometry without generic particles\n");
            return;
        }
        if (ta::devEnv("TA_PIECE_VISIBILITY_TEST")) {
            {
                ta::cob::File script;script.pieces={"root","arm","hand"};
                script.scripts={{"Test",0}};
                script.code={0x10021001,64,0x10071000,1,0x10021001,0,0x10065000};
                Anim test;test.vm=std::make_unique<ta::cob::Vm>(std::move(script));
                test.vm->enableRetailAnimation();
                ta::tdo::Object model;model.name="ROOT";model.children.resize(1);
                model.children[0].name="arm";model.children[0].children.resize(1);
                model.children[0].children[0].name="hand";
                configureExplosionHierarchy(test,model);
                // The replacement model moves the hand out of the arm branch.
                model.children.push_back(model.children[0].children[0]);
                model.children[0].children.clear();
                configureExplosionHierarchy(test,model);
                test.vm->start("Test");test.vm->tick(1.0f/30);
                if(test.vm->pieces()[1].visible || !test.vm->pieces()[2].visible)
                    throw std::runtime_error("replacement model retained stale detachment hierarchy");
            }
            // Exercise the production collector with a hidden, transformed parent.
            ta::tdo::Object root;root.name="parent";root.x=7;root.y=3;
            root.vertices={0,0,0, 12,0,0, 0,12,4};
            root.primitives={{0,"",{0,1,2}},{0,"",{2,1,0}}};
            root.children.push_back(root);
            root.children[0].name="child";root.children[0].x=19;
            const std::vector<std::string> names={"parent","child"};
            std::vector<ta::cob::PieceState> poses(2);
            poses[0].move[0]=11;poses[0].rot[1]=0.6f;
            Anim snapshot;snapshot.pieceNames=&names;snapshot.capturedPose=poses;
            for(bool shadow:{false,true}) {
                std::vector<Tri> baseline,hidden,all;
                collect(all,nullptr,root,Xform{},&snapshot,0.3f,0,false,false,shadow);
                auto reference=root;reference.primitives.clear();
                collect(baseline,nullptr,reference,Xform{},&snapshot,0.3f,0,false,false,shadow);
                poses[0].visible=false;
                collect(hidden,nullptr,root,Xform{},&snapshot,0.3f,0,false,false,shadow);
                if(baseline.empty() || all.size()<=baseline.size() || hidden.size()!=baseline.size())
                    throw std::runtime_error("hidden-parent collector lost visible child geometry");
                for(size_t i=0;i<hidden.size();++i)for(int v=0;v<3;++v)
                    if(hidden[i].v[v].position.x!=baseline[i].v[v].position.x ||
                       hidden[i].v[v].position.y!=baseline[i].v[v].position.y)
                        throw std::runtime_error("hidden-parent collector lost inherited transform");
                poses[1].visible=false;hidden.clear();
                collect(hidden,nullptr,root,Xform{},&snapshot,0.3f,0,false,false,shadow);
                if(!hidden.empty())throw std::runtime_error("hidden child still draws geometry");
                poses[0].visible=poses[1].visible=true;
            }
            // RENDER_OFF and DONT_CACHE must keep the body while removing only
            // this piece's shadow. Compare both collectors with all four flags.
            poses[1].visible=false;
            PieceMeta flagMeta;buildPieceMeta(root,flagMeta,false);
            for(unsigned flags=0;flags<16;++flags) {
                poses[0].visible=flags&1;poses[0].cached=flags&2;
                poses[0].shaded=flags&4;poses[0].rendered=flags&8;
                PreparedPiece prepared;
                preparePiece(root,flagMeta,Xform{},&snapshot,std::cos(0.3f),std::sin(0.3f),prepared);
                for(bool shadow:{false,true}) {
                    std::vector<Tri> reference,fast;
                    collect(reference,nullptr,root,Xform{},&snapshot,0.3f,0,false,false,shadow,
                            nullptr,&flagMeta);
                    collect(fast,nullptr,root,Xform{},&snapshot,0.3f,0,false,false,shadow,
                            nullptr,&flagMeta,false,&prepared);
                    const bool expected=shadow ? (flags&11)==11 : bool(flags&1);
                    if(reference.empty()==expected || fast.size()!=reference.size())
                        throw std::runtime_error("piece render/cache flags changed wrong body/shadow pass");
                    for(size_t i=0;i<fast.size();++i)for(int v=0;v<3;++v)
                        if(fast[i].v[v].position.x!=reference[i].v[v].position.x ||
                           fast[i].v[v].position.y!=reference[i].v[v].position.y)
                            throw std::runtime_error("piece flags prepared/reference transform mismatch");
                }
            }
            std::fprintf(stderr,"PASS: all16 piece flags preserve native body/shadow admission in both collectors\n");
            {
                const auto* model=loadVisual("armllt");
                if(!model)throw std::runtime_error("missing palette lighting fixture model");
                const auto& meta=visuals_.at("armllt").meta;
                preparePaletteTextures(meta);
                std::vector<std::string> pieceNames;
                const auto gather=[&](const auto& self,const ta::tdo::Object& object)->void {
                    pieceNames.push_back(object.name);
                    std::transform(pieceNames.back().begin(),pieceNames.back().end(),
                                   pieceNames.back().begin(),::tolower);
                    for(const auto& child:object.children)self(self,child);
                };
                gather(gather,model->root);
                std::vector<ta::cob::PieceState> state(pieceNames.size());
                Anim pose;pose.pieceNames=&pieceNames;pose.capturedPose=state;
                std::vector<Tri> shaded,neutral,preparedGeometry;
                auto* atlas=atlasFor(0);
                collect(shaded,atlas,model->root,Xform{},&pose,0.4f,0,false,true,
                        false,nullptr,&meta,false,nullptr,true);
                PreparedPiece prepared;
                preparePiece(model->root,meta,Xform{},&pose,std::cos(0.4f),std::sin(0.4f),prepared);
                collect(preparedGeometry,atlas,model->root,Xform{},&pose,0.4f,0,false,true,
                        false,nullptr,&meta,false,&prepared,true);
                if(shaded.size()!=preparedGeometry.size())
                    throw std::runtime_error("palette prepared geometry count mismatch");
                for(auto& piece:state)piece.shaded=false;
                collect(neutral,atlas,model->root,Xform{},&pose,0.4f,0,false,true,
                        false,nullptr,&meta,false,nullptr,true);
                if(shaded.empty() || neutral.size()!=shaded.size())
                    throw std::runtime_error("palette lighting fixture geometry missing");
                bool usesPalette=false,changed=false;
                for(size_t i=0;i<shaded.size();++i) {
                    for(const auto& page:palettePages_)
                        usesPalette|=shaded[i].tex==page.texture;
                    if(shaded[i].tex!=preparedGeometry[i].tex)
                        throw std::runtime_error("palette prepared texture mismatch");
                    for(int v=0;v<3;++v) {
                        if(std::memcmp(&shaded[i].v[v],&preparedGeometry[i].v[v],sizeof(SDL_Vertex)))
                            throw std::runtime_error("palette prepared vertex mismatch");
                        changed|=shaded[i].v[v].tex_coord.x!=neutral[i].v[v].tex_coord.x ||
                                 shaded[i].v[v].tex_coord.y!=neutral[i].v[v].tex_coord.y;
                    }
                }
                if(!usesPalette || !changed)
                    throw std::runtime_error("authored SHADE/DONT_SHADE did not select palette bands");
                std::fprintf(stderr,"PASS: palette lighting selects authored bands and matches prepared geometry\n");
            }
            std::fprintf(stderr,"PASS: production body/shadow collector preserves visible children and hidden-parent transforms\n");
            return;
        }
        if (ta::devEnv("TA_EXPLOSION_CLASSES_TEST")) {
            if(!effectFor("fx:explosion","palettes/palette.pal"))
                throw std::runtime_error("missing fx.gaf explosion sequences");
            UnitR unit{};unit.x=cx;unit.z=cz;
            Anim snapshot;
            const auto particleCount=particles_.size(),effectCount=effects_.size();
            for(int flags:{0x28,0x30,0x38})explodePiece(unit,snapshot,-1,flags);
            if(particles_.size()!=particleCount || effects_.size()!=effectCount)
                throw std::runtime_error("bitmap-only debris flags spawned immediate effects");
            for(unsigned cls=0;cls<6;++cls) {
                const auto before=effects_.size();
                explodePiece(unit,snapshot,-1,int32_t(0x20u|(0x100u<<cls)));
                if(effects_.size()!=before+1)throw std::runtime_error("numeric explosion class failed to spawn");
                if(!effects_.back().authoredTiming || effects_.back().started!=front().gameTick)
                    throw std::runtime_error("explosion class did not start its tick clock");
            }
            const auto before=effects_.size();
            explodePiece(unit,snapshot,-1,0x3f20);
            if(effects_.size()!=before+6)throw std::runtime_error("combined explosion flags lost effects");
            const auto samples=effects_;
            effects_.clear();
            for (auto sample:samples) {
                uint32_t duration=0;
                for (const auto delay:sample.anim->durations) duration+=std::max(1u,unsigned(delay));
                if (!duration) throw std::runtime_error("authored effect has no frame duration");
                for (const uint32_t age:{0u,duration-1,duration}) {
                    sample.started=front().gameTick-age;
                    effects_.push_back(sample);
                    updateEffects(0);
                    if (effects_.empty()!=(age==duration))
                        throw std::runtime_error("authored effect expired on the wrong tick");
                    effects_.clear();
                }
            }
            effects_=samples;
            std::fprintf(stderr,"PASS: live authored effect expiry retains the last frame and removes at exact duration\n");
            std::fprintf(stderr,"PASS: all six EXPLODE BITMAP sequences and combined flags spawn independently\n");
            return;
        }
        if (ta::devEnv("TA_GLOW_TEST")) {
            edgeScrollOn_=false;follow_=false;initialCamera_.reset();
            for(unsigned kind=0;kind<3;++kind)
                explosionGlows_.push_back({cx+(float(kind)-1)*100,cz,0,front().gameTick,kind});
            int width=0,height=0;SDL_GetRendererOutputSize(ren_,&width,&height);
            mapView_.setZoom(1.5f);
            mapView_.setOffset(cx-float(mapViewW(width))*0.5f/mapView_.zoom(),
                cz-float(height-barH())*0.5f/mapView_.zoom());
            return;
        }
        if (ta::devEnv("TA_POINT_TEST")) {
            edgeScrollOn_=false;follow_=false;initialCamera_.reset();
            const auto& map=mapView_.map();
            bool found=false;
            for(int z=16;z<map.height-16 && !found;++z)
                for(int x=16;x<map.width-32 && !found;++x) {
                    bool water=true;
                    for(int dz=-4;dz<=4 && water;++dz)for(int dx=-4;dx<=20;++dx)
                        if(map.heights[size_t(z+dz)*map.width+size_t(x+dx)]>=map.seaLevel-8) {water=false;break;}
                    if(water) {cx=float(x*16);cz=float(z*16);found=true;}
                }
            if(!found)throw std::runtime_error("point fixture requires a stretch of deep water");
            const int owner=spawn("armtship",cx,cz,0,0);
            writableWorld().order(owner,cx+240,cz,false);
            int width=0,height=0;SDL_GetRendererOutputSize(ren_,&width,&height);
            mapView_.setZoom(2.f);
            mapView_.setOffset(cx+80-float(mapViewW(width))*0.5f/mapView_.zoom(),
                cz-float(height-barH())*0.5f/mapView_.zoom());
            return;
        }
        if (ta::devEnv("TA_SMOKE_TEST")) {
            edgeScrollOn_=false;follow_=false;initialCamera_.reset();
            const int owner=spawn("armlab",cx,cz,0,0);
            if(auto* unit=writableWorld().unit(owner))unit->hp=ta::sim::Fixed::fromInt(unit->maximumHp()/5);
            int width=0,height=0;SDL_GetRendererOutputSize(ren_,&width,&height);
            mapView_.setZoom(1.5f);
            mapView_.setOffset(cx-float(mapViewW(width))*0.5f/mapView_.zoom(),
                cz-float(height-barH())*0.5f/mapView_.zoom());
            return;
        }
        if (const char* type=ta::devEnv("TA_PROJECTILE_TEST")) {
            // Screenshot fixtures must not drift with the dummy driver's
            // pointer at (0,0), and must fit the actual map viewport.
            edgeScrollOn_=false;follow_=false;initialCamera_.reset();
            const int shooter=spawn(type,cx-200,cz,1.57f,0);
            const int target=spawn("corak",cx+200,cz,-1.57f,1);
            if (auto* u=writableWorld().unit(target)) {
                u->hp=ta::sim::Fixed::fromInt(25000);writableWorld().setStance(target,2);
            }
            if (shooter>=0 && target>=0) writableWorld().attack(shooter,target,false);
            int width=0,height=0;SDL_GetRendererOutputSize(ren_,&width,&height);
            const float viewport=float(mapViewW(width));
            mapView_.setZoom(std::min(1.5f,std::max(0.25f,(viewport-100.f)/400.f)));
            mapView_.setOffset(cx-viewport*0.5f/mapView_.zoom(),
                cz-float(height-barH())*0.5f/mapView_.zoom());
            return;
        }
        int a = spawn("armpw", cx - 40, cz, 1.57f, 0);
        int e = spawn("corak", cx + 200, cz, -1.57f, 1);
        writableWorld().attack(a, e, false);
        // Aim-pipeline check: a Light Laser Tower (turret AimPrimary) with an
        // enemy off-axis to its north-east -- the turret must visibly swing to
        // face it, proving the AimWeapon heading sign.
        int tw = spawn("armllt", cx - 40, cz + 220, 0.0f, 0);
        int zm = spawn("corak", cx + 160, cz + 100, -1.57f, 1);
        writableWorld().attack(tw, zm, false);   // explicit order: aim runs even without LOS
        // Feature-burning check: park a victim right beside a flamable feature
        // and have a Pyro flame it -- the splash must ignite the tree
        // (spread + burnt swap then follow on their own).
        float tx = cx + 60, tz = cz - 60;
        bool burnTarget=false;
        std::string burnType;
        const bool flameProbe=ta::devFlag("TA_FEATURE_FLAME_TEST");
        float closest=std::numeric_limits<float>::max();
        for (const auto& ft : world_.features())
            if (ft.alive && ft.type >= 0 &&
                world_.featureTypes()[size_t(ft.type)].flamable) {
                if(flameProbe && !world_.featureTypes()[size_t(ft.type)].hasBurnAnim)continue;
                const auto& name=world_.featureTypes()[size_t(ft.type)].name;
                if(flameProbe) {
                    const auto definition=featureDefs_.find(name);
                    if(definition==featureDefs_.end() ||
                       (definition->second.valueOr("seqnamefrontflame","").empty() &&
                        definition->second.valueOr("seqnamebackflame","").empty()))continue;
                }
                float ddx = ft.x.toFloat() - cx, ddz = ft.z.toFloat() - cz;
                const float distance=ddx*ddx+ddz*ddz;
                if (distance < (flameProbe ? closest : 400 * 400)) {
                    tx = ft.x.toFloat() + 20; tz = ft.z.toFloat();burnTarget=true;closest=distance;
                    burnType=name;
                    if(!flameProbe)break;
                }
            }
        if(flameProbe) {
            if(!burnTarget)throw std::runtime_error("feature flame probe requires a map feature with authored burn animation");
            const auto& definition=featureDefs_.at(burnType);
            debugFeatureFlameExpected_=int(!definition.valueOr("seqnamefrontflame","").empty())+
                int(!definition.valueOr("seqnamebackflame","").empty());
            std::fprintf(stderr,"feature flame probe: target near %.0f,%.0f\n",tx,tz);
        }
        int dr = spawn("corpyro", tx - 160, tz - 40, 1.57f, 1);
        int ar = spawn("armpw", tx, tz, -1.57f, 0);
        writableWorld().attack(dr, ar, false);
        // Ordered corpse-reclaim check: a builder is sent (negative target id)
        // to consume a fresh corpse -- the body must vanish when it arrives.
        int rcv = spawn("armpw", cx - 200, cz + 120, 0.0f, 0);
        if (auto* rd = writableWorld().unit(rcv)) rd->hp = ta::sim::Fixed();   // dies this tick, normal corpse
        int rcb = spawn("armck", cx - 250, cz + 160, 1.57f, 0);
        writableWorld().reclaim(rcb, -rcv, false);
        // Watch the burn, not the tower: centre the camera on the target tree.
        mapView_.setOffset(tx - 640 / mapView_.zoom(), tz - 400 / mapView_.zoom());
        if (ta::devEnv("TA_FEATURE_FLAME_TEST")) {
            edgeScrollOn_=false;follow_=false;initialCamera_.reset();
            int width=0,height=0;SDL_GetRendererOutputSize(ren_,&width,&height);
            mapView_.setZoom(1.5f);
            mapView_.setOffset(tx-float(mapViewW(width))*0.5f/mapView_.zoom(),
                tz-float(height-barH())*0.5f/mapView_.zoom());
        }
    }

    void GameView::lodeTest() {
        float cx = mapView_.map().blocksX * 16.0f, cz = mapView_.map().blocksY * 16.0f;
        spawn(lodeUnit.empty()?"armmex":lodeUnit, cx, cz, 3.14159f, 0);
        mapView_.setOffset(cx - 640 / mapView_.zoom(), cz - 400 / mapView_.zoom());
    }

    void GameView::guardTest() {
        // Squad guards the first unit; the first unit marches east alone.
        int leader = -1;
        for (auto& u : world_.units())
            if (u.alive() && u.player == 0 && u.type && u.type->canMove) {
                if (leader < 0) leader = u.id;
                else writableWorld().guard(u.id, leader, false);
            }
        if (leader >= 0) writableWorld().order(leader, 1500, 950, false);
    }

    void GameView::marchTo(float dx, float dz) {
        ClickScope click(*this);   // one click: a Legion net game sends it as explicit orders
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


    void GameView::amphibDemo() {
        amphib_ = true;
        const auto* shipType = registry_.find("armtship");
        const auto& ground = writableWorld().nav();
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

        transportId_ = spawn("armtship", wax, waz, 0, 0);
        const char* squad[] = {"armrock", "armrock", "armpw", "armpw"};
        int i = 0;
        for (const char* t : squad) {
            int id = spawn(t, bax + float(i % 2) * 24 - 12, baz + float(i / 2) * 24 - 12,
                           0, 0);
            if (id >= 0) {
                writableWorld().loadInto(id, transportId_, amphibSquad_ > 0);   // the carrier's list: the rest queue
                ++amphibSquad_;
            }
            ++i;
        }
    }


    // Long-running churn for the render matrix: every `period` frames the oldest
    // twentieth of the living mobile army dies (leaving corpses) and as many new
    // units spawn beside the army. Unit ids keep growing, so retained render
    // entries see destruction, fresh ids and replacements. Deterministic: it
    // runs from the frame counter of the fixed-step render study.
    void GameView::renderStudyChurn(unsigned frame, unsigned period) {
        if(!period || !frame || frame%period)return;
        std::vector<int> mobile;
        for(const auto& unit:world_.units())
            if(unit.alive() && unit.type && unit.type->canMove && !unit.type->canFly)mobile.push_back(unit.id);
        if(mobile.empty())return;
        std::sort(mobile.begin(),mobile.end());
        const size_t kill=std::max<size_t>(1,mobile.size()/20);
        const char* roster[]={"armpw","corak","armrock","corstorm"};
        for(size_t k=0;k<kill;++k) {
            const auto* old=world_.unit(mobile[k]);
            const float x=old->x.toFloat(),z=old->z.toFloat();
            writableWorld().destroy(mobile[k]);
            const float dx=float(int((frame/period+k)%9)-4)*24;
            spawn(roster[(frame/period+k)%4],x+dx,z+48,float(k%8)*0.785398163f,localPlayer_);
        }
    }

    void GameView::renderStudyCursor(unsigned frame, int select, char cmd) {
        cursorStudyOn_ = true;
        const Frame& f = front();
        if (select > 0 && selection_.empty())
            for (const UnitR* u : f.live) {
                if (int(selection_.size()) >= select) break;
                if (u->player == localPlayer_ && u->alive() && !u->embarked() && !u->underConstruction && u->type)
                    selection_.push_back(u->id);
            }
        if (cmd == '*') {
            // Every armed order in turn, seven frames each (and none).
            static constexpr char kCycle[] = {0, 'a', 'r', 'g', 'l', 'm', 'p', 'c', 'f'};
            pendingCmd_ = kCycle[(frame / 7) % std::size(kCycle)];
        } else if (cmd) {
            pendingCmd_ = cmd;
        }
        const float w = float(mapViewW(winW_)), h = float(mapViewH(winH_));
        const float margin = 40.0f;   // clear of the edge-scroll band
        float x = w * (0.5f + 0.42f * std::sin(float(frame) * 0.037f));
        float y = h * (0.5f + 0.42f * std::sin(float(frame) * 0.051f + 1.0f));
        // The units whose anchor can be in the world view (the snapshot's unit grid), so
        // a large army seen through a small view still puts the pointer on bodies.
        std::vector<uint32_t> inView;
        if (f.unitGrid.size() == f.live.size()) {
            std::vector<uint64_t> bits;
            const auto rect = ta::render::anchorBandToWorld(-64.0f, -64.0f, w + 64.0f, h + 160.0f, mapView_.offX(),
                                                             mapView_.offY(), mapView_.zoom(), unitLiftBounds());
            f.unitGrid.query(rect.left, rect.top, rect.right, rect.bottom, bits, inView);
        } else {
            for (uint32_t i = 0; i < uint32_t(f.live.size()); ++i) inView.push_back(i);
        }
        if (frame % 2 == 0 && !inView.empty())
            for (uint32_t k = 0; k < 64; ++k) {
                const UnitR& u = *f.live[inView[(size_t(frame) * 2654435761u + size_t(k) * 40503u) % inView.size()]];
                if (!u.type || u.embarked() || !u.alive()) continue;
                const SDL_FPoint p = dotsFrame_ ? dotCentre(u) : unitScreen(u);
                if (p.x < margin || p.x > w - margin || p.y < margin || p.y > h - margin) continue;
                x = p.x; y = p.y;
                break;
            }
        mouseX_ = std::clamp(x, margin, std::max(margin, w - margin));
        mouseY_ = std::clamp(y, margin, std::max(margin, h - margin));
        renderStudyMouse(mouseX_, mouseY_);   // and the window position
    }

    // Material-only invalidation for the render matrix (workstream E): every
    // `period` frames every fifth living mobile unit moves to the next study
    // tint level in 4..9 (a gold body tint). Client-only: TA draws veterans
    // as any other unit, so the old rank tint lives on only as this study's
    // material change (renderStudyTint_), and the simulation is untouched.
    void GameView::renderStudyVeteran(unsigned frame, unsigned period) {
        if(!period || !frame || frame%period)return;
        size_t k=0;
        for(const auto& unit:world_.units()) {
            if(!unit.alive() || !unit.type || !unit.type->canMove || k++%5)continue;
            renderStudyTint_[unit.id]=4+int((frame/period+k)%6);
        }
    }

#endif   // !NDEBUG

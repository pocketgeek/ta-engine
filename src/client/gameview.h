#pragma once

// GameView -- the in-world game client: rendering, input, the retail HUD, fog,
// minimap, the COB animation VM host, benchmark, lobby, and the
// server/replay glue. Extracted from client/main.cpp (which keeps only the app
// shell: main(), the front-end loop, and local-server helpers). Client-only and
// NOT part of the hashed sim. Kept at global scope so main() references it
// unqualified. Method bodies are being split out into gameview_*.cpp.

// Must precede SDL.h: on Windows this pulls in winsock2 (with WIN32_LEAN_AND_MEAN)
// before SDL's <windows.h> would otherwise pull the incompatible winsock v1.
#include "net/netcompat.h"

#include "campaign/campaign.h"
#include "client/briefingscreen.h"
#include "client/resultscreen.h"
#include "client/loadscreen.h"
#include "cob/vm.h"
#include "crt/crt.h"
#include "gaf/gaf.h"
#include "gui/gui.h"
#include "hpi/hpi.h"
#include "net/client.h"
#include "util/procmetrics.h"   // benchmark: cross-platform CPU/RSS sampling
#include "net/lockstep.h"
#include "ai/ai.h"          // Difficulty <-> aiLevel + incomeMultFor (header-only helpers)
#include "sim/matchsetup.h"
#include "sim/scenario.h"
#include "sim/sim.h"
#include "tdf/tdf.h"
#include "tdo/tdo.h"
#include "terrain/terrain.h"
#include "tnt/tnt.h"
#include "tnt/mapgen.h"
#include "util/png.h"
#include "version.h"
#include "client/cursors.h"
#include "client/dirpicker.h"   // first-run data-dir folder picker
#include "client/aascalereset.h"   // RAII 1:1 render-scale guard (extracted leaf)
#include "client/font.h"      // GAF bitmap font (extracted leaf class)
#include "client/mapview.h"   // terrain pan/zoom + async chunk compositor (extracted leaf)
#include "client/modelmath.h"   // Tri/Xform/scriptRot (shared by GameView + model viewer)
#include "client/modelview.h"   // standalone 3DO model viewer (extracted leaf)
#include "client/renderframe.h"   // UnitR/PlayerR/Frame render snapshot (extracted leaf)
#include "client/replayfile.h"   // .takrep parser (extracted leaf)
#include "client/sound.h"     // WAV mixer + music + soundclasses (extracted leaf)
#include "client/threadpool.h"   // data-parallel worker pool (extracted leaf)
#include "client/gpuvram.h"   // central GPU-texture VRAM accountant + hard cap
#include "client/hotkeys.h"
#include "client/hotkeysscreen.h"
#include "client/options.h"
#include "client/settings.h"
#include "client/dev.h"
#include "client/appquit.h"
#include "client/mainmenu.h"
#include "client/menumusic.h"

// Keep our own main() on every platform (don't let SDL redefine it to SDL_main /
// pull in SDL2main + a WinMain); we call SDL_SetMainReady() in main() instead. This
// also keeps takclient usable as a console/headless tool on Windows. The build also
// defines this target-wide (CMake) so it holds even when a header pulls in <SDL.h>
// before this point; the guard avoids a redefinition warning.
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Single-player auto-launches a local takserver (AIs run only on the server).
// Sockets come from net/netcompat.h (included first, before SDL). Process control
// is the one genuinely platform-specific bit: fork/exec on POSIX, CreateProcess on
// Windows.
#ifdef _WIN32
  #include <windows.h>
#else
  #include <csignal>
  #include <sys/wait.h>
  #include <unistd.h>
#endif


#include "client/appglobals.h"   // kProjY/kProjZ, kSortZ/kSortY

class GameView {
public:
    struct FactionKit {
        const char* monarch;   // hero commander the faction starts with
        const char* keep;      // production building the monarch builds first
        const char* lode;
        const char* builder;
        const char* squad[4];
    };
    static const FactionKit& kit(const std::string& side) {
        static const std::map<std::string, FactionKit> kits = {
            {"ara", {"araking", "arakeep", "aralode", "arabuild",
                     {"araarch", "araarch", "arasword", "arasword"}}},
            {"tar", {"tarnecro", "tardung", "tarlode", "tarnecro",
                     {"tararch", "tararch", "tardemon", "tartb"}}},
            {"ver", {"vermage", "verkeep", "verlode", "verliege",
                     {"verarch", "verarch", "versword", "versword"}}},
            {"zon", {"zonhunt", "", "zonlode", "zonhand",
                     {"zongob", "zonter", "zontroll", "zonbat"}}},
            {"cre", {"cresage", "creacad", "crelode", "cremech",
                     {"creauto", "creauto", "crebeas", "creshoc"}}},
        };
        auto it = kits.find(side);
        return it != kits.end() ? it->second : kits.at("ara");
    }

    // Player start positions from the current map's sibling .ota, in world pixels,
    // ordered StartPos1, StartPos2, …. Delegates to the shared sim helper so the
    // client and the server's referee derive identical positions.
    std::vector<std::pair<float, float>> parseStartPositions() const;

    // Join the sim worker + minimap crunch before members die, then free every
    // GPU texture we own: the SDL_Renderer outlives the session (menu -> game ->
    // menu loops reuse it), so anything not destroyed here leaks REAL VRAM and
    // gpuvram budget across sessions -- a few benchmark runs used to pin the
    // budget and starve terrain-chunk uploads (map stuck at the low-res underlay).
    // Interactive teardown: cancel, do not drain (see simCancel_).
    ~GameView() { stopSimThread(/*drain=*/false); resetMinimap(); destroyGpuTextures(); }

    GameView(SDL_Renderer* ren, tak::hpi::Vfs vfs, const std::string& mapPath,
             const std::string& installRoot, tak::hpi::OverridePolicy policy,
             bool demo, bool scenario, bool mission,
             bool bare, const std::string& side = "ara", const std::string& aiSide = "tar",
             bool crusades = false)
        // (side_ initialized below before loadPanel uses it; vfs_ must precede
        //  mapView_ in the member list so the Compositor can borrow it)
        : ren_(ren), vfs_(std::move(vfs)), mapView_(ren, vfs_, mapPath),
          installRoot_(installRoot), policy_(policy),
          mapPath_(mapPath), crusades_(crusades), side_(side), aiSide_(aiSide) {
        // Unit registry: MOVEINFO + units + canbuild (+ Crusades overlay first).
        // The VFS merges base + Iron Plague + community data into one namespace,
        // precedence resolved by the retail newest-date rule.
        tak::sim::setupRegistry(registry_, vfs_, crusades_);
        if (crusades_)
            std::fprintf(stderr, "balance: Crusades (unitscb/canbuildcb)%s\n",
                         vfs_.list("unitscb").empty() ? " -- NOT FOUND" : "");
        // God economy timing (gamedata/gods.tdf). TAK_GODTIME overrides the
        // appear time (seconds) for testing; otherwise use AppearTimeMin minutes.
        try {
            auto g = vtdf("gamedata/gods.tdf");
            if (const auto* tm = g.child("TIMING")) {
                float appear = float(tm->numberOr("AppearTimeMin", 30.0)) * 60.0f;
                if (const char* e = tak::devEnv("TAK_GODTIME")) appear = std::stof(e);
                world_.enableGods(appear);
            }
        } catch (const std::exception&) {}
        loadTextures();
        mapView_.setZoom(0.9f);
        try {
            hudFont_ = Font(ren_, vfs_, "fonts/bodfontbody.gaf");
            bigFont_ = Font(ren_, vfs_, "fonts/font48.gaf");
            // A plain, legible font for the HUD stat readouts.
            try { statFont_ = Font(ren_, vfs_, "fonts/b_times new roman (100b).gaf"); }
            catch (const std::exception&) {
                try { statFont_ = Font(ren_, vfs_, "fonts/ig_times new roman (100).gaf"); }
                catch (const std::exception&) {}
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "font load: %s\n", e.what());
        }
        loadOrderButtons();
        loadBuildFx();
        sounds_.init(vfs_);
        soundClasses_.load(vfs_);   // music is started per-state by manageMusic()
        loadPanel(side_);
        loadGui(side_);

        if (mission) {
            world_.setTerrain(mapView_.map().heights, mapView_.map().width,
                              mapView_.map().height, mapView_.map().seaLevel,
                              &mapView_.map().features);
            tak::sim::registerMapFeatures(world_, mapView_.map(), vfs_, &registry_);
            try {
                auto ota = vtdf(mapSibling(".ota"));
                const auto* gh = ota.child("globalheader");
                const auto* md = gh ? gh->child("map data") : nullptr;
                const auto* units = md ? md->child("units") : nullptr;
                std::printf("mission: %s\n",
                            gh ? gh->valueOr("missiondescription", "").c_str() : "");
                int n = 0;
                float cx = 0, cz = 0;
                int pc = 0;
                if (units)
                    for (const auto& key : units->childOrder) {
                        const auto& u = units->children.at(key);
                        std::string id = u.valueOr("unitname", "");
                        std::transform(id.begin(), id.end(), id.begin(), ::tolower);
                        int playerSlot = int(u.numberOr("player", 1));
                        float x = float(u.numberOr("xpos", 0)) * 16 + 8;
                        float z = float(u.numberOr("zpos", 0)) * 16 + 8;
                        int player = std::clamp(playerSlot - 1, 0, 3);
                        int uid = spawn(id, x, z, 3.14159f, player);
                        if (uid >= 0) {
                            ++n;
                            float hpp = float(u.numberOr("healthpercentage", 100));
                            if (auto* su = world_.unit(uid)) {
                                su->hp *= hpp / 100.0f;
                                if (su->type->canMove &&
                                    su->type->domain ==
                                        tak::sim::UnitType::Domain::Ground)
                                    reinfPool_[player].push_back(id);
                            }
                            if (player == 0) { cx += x; cz += z; ++pc; }
                        }
                    }
                std::printf("mission: %d units spawned\n", n);
                if (pc) mapView_.setOffset(cx / float(pc) - 640 / 0.9f,
                                           cz / float(pc) - 400 / 0.9f);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "mission load: %s\n", e.what());
            }
            loadFeatures();
            // Mission scripts: run the authentic COB event handlers.
            try {
                std::string cobPath = mapSibling(".cob");
                auto roster = vtdf(mapSibling(".tdf"));
                for (const auto& name : roster.childOrder) {
                    std::string n = name;
                    std::transform(n.begin(), n.end(), n.begin(), ::tolower);
                    missionRoster_.push_back(n);
                    if (n == "verat" || n == "araat" || n == "tarat" || n == "zonat")
                        missionTowerIdx_ = int(missionRoster_.size()) - 1;
                }
                missionVm_ = std::make_unique<tak::cob::Vm>(tak::cob::load(vread(cobPath), cobPath),
                                                            /*deterministicRand=*/true);
                missionVm_->onMapCommand = [this](int sub, const std::vector<int32_t>& a)
                    -> int32_t { return mapCommand(sub, a); };
                missionVm_->onGet = [this](int32_t valId, const std::vector<int32_t>& a)
                    -> int32_t {
                    if (valId == 30 && !a.empty()) {
                        int idx = rosterIndexOf(a[0]);
                        if (trace_) {
                            static int lg = 0;
                            if (lg++ < 8)
                                std::printf("GET30 unit=%d -> roster %d (tower=%d)\n",
                                            a[0], idx, missionTowerIdx_);
                        }
                        return idx;
                    }
                    return 0;
                };
                missionVm_->onSetUnitValue = [this](int32_t valId, int32_t value) {
                    if (valId == 2 && value == 1 && outcome_ == 0) outcome_ = 1;
                };
                missionVm_->start("Start");
                std::printf("mission scripts: running\n");
                std::vector<uint8_t> tb;
                if (vhas(mapSibling(".txt"))) tb = vread(mapSibling(".txt"));
                std::istringstream bf(std::string(tb.begin(), tb.end()));
                std::string line;
                while (std::getline(bf, line) && briefing_.size() < 8) {
                    std::string clean;
                    for (char c : line)
                        if (uint8_t(c) >= 32 && uint8_t(c) < 127) clean += c;
                    while (!clean.empty() && clean.back() == ' ') clean.pop_back();
                    if (clean.empty()) continue;
                    // Wrap to ~54 chars per line for the panel.
                    std::string cur = "- ";
                    std::istringstream ws(clean);
                    std::string word;
                    while (ws >> word) {
                        if (cur.size() + word.size() > 42) {
                            briefing_.push_back(cur);
                            cur = "  ";
                        }
                        cur += word + " ";
                    }
                    if (cur.size() > 2) briefing_.push_back(cur);
                }
                if (briefing_.size() > 10) briefing_.resize(10);
                briefTimer_ = 30;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "mission cob: %s\n", e.what());
            }
            return;
        }

        if (scenario) {
            world_.setTerrain(mapView_.map().heights, mapView_.map().width,
                              mapView_.map().height, mapView_.map().seaLevel,
                              &mapView_.map().features);
            tak::sim::registerMapFeatures(world_, mapView_.map(), vfs_, &registry_);
            std::string crtPath = mapSibling(".crt");
            tak::crt::Scenario scen = vhas(crtPath) ? tak::crt::parse(vread(crtPath))
                                                    : tak::crt::Scenario{};
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
                    if (tak::sim::Unit* su = world_.unit(uid)) {   // apply the .crt stats
                        if (su->type)
                            su->hp = su->type->maxHp * float(std::clamp(u.health, 0, 100)) / 100.0f;
                        su->veteran = std::clamp(u.veteran, 0, 10);
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
                world_.setScenario(std::make_unique<tak::sim::ScenarioScript>(
                    scen, registry_, localPlayer_, world_.numPlayers(),
                    mapView_.map().width, mapView_.map().height));
            return;
        }

        world_.setTerrain(mapView_.map().heights, mapView_.map().width,
                          mapView_.map().height, mapView_.map().seaLevel,
                          &mapView_.map().features);
        tak::sim::registerMapFeatures(world_, mapView_.map(), vfs_, &registry_);
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
        mapView_.setOffset(px - 640 / 0.9f, pz - 400 / 0.9f);
        // Dev harness: TAK_FFA=N or TAK_FFA=N,t0.t1.t2... sets up an N-player
        // game (each on its own team unless a team list is given), one monarch +
        // a small army per player at N start positions, all AI-driven. Verifies
        // the 8-player / team / shared-vision / win-condition paths before the
        // real lobby exists. (multiplayer M1)
        if (const char* ff = tak::devEnv("TAK_FFA")) {
            int n = std::atoi(ff);
            n = std::clamp(n, 2, tak::sim::kMaxPlayers);
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
            world_.setPlayerCount(n);
            for (int i = 0; i < n; ++i) world_.setTeam(i, teams[size_t(i)]);
            // Spread players across the N most mutually-distant start positions
            // (fall back to a ring around centre when the map has too few).
            std::vector<std::pair<float, float>> spots = starts;
            while (int(spots.size()) < n) {
                float ang = float(spots.size()) / float(n) * 6.2831853f;
                spots.push_back({cx + std::cos(ang) * 300, cz + std::sin(ang) * 300});
            }
            const char* sides[5] = {"ara", "tar", "ver", "zon", "cre"};
            for (int i = 0; i < n; ++i) {
                std::string fkSide = sides[i % 5];
                const FactionKit& fk = kit(fkSide);
                float mx = spots[size_t(i)].first, mz = spots[size_t(i)].second;
                int mon = spawn(fk.monarch, mx, mz, 0, i);
                if (i == 0) { playerMonarchId_ = mon; builderId_ = mon; }
                for (int s = 0; s < 4; ++s)
                    spawn(fk.squad[s % 4], mx + (s % 2) * 26 - 13, mz - 50 + (s / 2) * 26, 0, i);
                world_.player(i).mana = 2800;
            }
            ffaPlayers_ = n;
            for (auto& u : world_.units()) {
                if (!u.type || u.type->canMove) continue;
                tak::sim::blockFootprint(world_.nav(), *u.type, u.x, u.z, true);
            }
            return;
        }
        if (!bare) {
        const FactionKit& pk = kit(side);
        const FactionKit& ak = kit(aiSide);
        // Monarchs face one another.
        float pFace = std::atan2(ax - px, az - pz);
        float aFace = std::atan2(px - ax, pz - az);
        playerMonarchId_ = spawn(pk.monarch, px, pz, pFace, 0);
        builderId_ = playerMonarchId_;
        aiMonarchId_ = spawn(ak.monarch, ax, az, aFace, 1);
        // Enough mogrium to bootstrap the opening: a handful of lodestones for
        // income and the start of a keep, without being able to skip economy
        // and rush one to completion.
        world_.player(0).mana = 2800;
        world_.player(1).mana = 2800;
        if (demo) {
            // Showcase: skip the slow build-up and pit two ready armies at the
            // start positions against each other.
            std::vector<int> playerA, playerB;
            for (int i = 0; i < 6; ++i) {
                int a = spawn(pk.squad[i % 4], px + float(i % 2) * 26,
                              pz - 60 + float(i / 2) * 30, pFace, 0);
                int b = spawn(ak.squad[i % 4], ax + float(i % 2) * 26,
                              az - 60 + float(i / 2) * 30, aFace, 1);
                if (a >= 0) playerA.push_back(a);
                if (b >= 0) playerB.push_back(b);
            }
            if (pk.keep[0]) keepId_ = spawn(pk.keep, px, pz + 60, pFace, 0);
            if (ak.keep[0]) aiKeepId_ = spawn(ak.keep, ax, az + 60, aFace, 1);
            if (!playerA.empty() && !playerB.empty()) {
                for (size_t k = 0; k < playerA.size(); ++k)
                    world_.attack(playerA[k], playerB[k % playerB.size()], false);
                for (size_t k = 0; k < playerB.size(); ++k)
                    world_.attack(playerB[k], playerA[k % playerA.size()], false);
            }
            // Showcase: pre-select the player's keep so the command panel shows its
            // conjure/build menu and the info bar shows the portrait (a fuller HUD for
            // screenshots). Harmless otherwise -- demo is a dev/showcase mode.
            if (keepId_ >= 0) selectOnly(keepId_);
            else if (!playerA.empty()) selectOnly(playerA.front());
        }
        }

        for (auto& u : world_.units()) {
            if (!u.type || u.type->canMove) continue;
            tak::sim::blockFootprint(world_.nav(), *u.type, u.x, u.z, true);
        }
    }

    // True while the multiplayer lobby is showing (before the game world exists).
    // Play the menu theme (track15) while in the lobby / not yet in a game, and the
    // faction playlist once the match starts. Called every frame; only (re)starts the
    // playlist on a state change so it doesn't restart the current track.
    int musicMode_ = 0;   // 0 = none yet, 1 = lobby, 2 = in-game
    bool discoWas_[8] = {};       // per-player disco state, to fire the track once on start
    bool headbangWas_[8] = {};    // ...and the headbang state

    // On the rising edge of a player's disco (Shift+D), play the 10s disco loop as a
    // positional SFX from each of that player's dancing monarchs (enemies only if in
    // view). It runs alongside the faction music -- a dance-floor track from the unit.
    void discoSound();

    // Same as discoSound but for the Shift+H headbang -> the heavy-metal track.
    void headbangSound();

    void manageMusic();

    bool inLobbyPhase() const;
    void setMpMapId(const std::string& id) { mpMapId_ = id; }
    void setMissionStem(const std::string& s) { missionStem_ = s; }
    void setResumePath(const std::string& p) { mpResumePath_ = p; }
    // Single-player from the menu: it's a private local game, so open the lobby on
    // the Create screen (the browser is empty by design) and mark it single-player
    // (labels change, no password / no game browser).
    void setSinglePlayer() { lobbyScreen_ = LobbyScreen::Create; singlePlayer_ = true; }
    // Return-to-menu request: a lobby/in-game action sets this; main()'s outer loop
    // tears the session down and re-shows the front-end menu.
    void requestMenu() { menuRequested_ = true; }
    bool menuRequested() const { return menuRequested_; }
    bool quitRequested() const { return quitRequested_; }   // in-game QUIT -> exit app
    // Menu-launched sessions: the front-end owns the lobby BGM (see manageMusic).
    void setExternalLobbyMusic() { externalLobbyMusic_ = true; }
    // Menu-launched sessions can return to the front-end, so the in-game menu
    // offers MAIN MENU; a direct CLI game only offers RESUME/QUIT.
    void setCanReturnToMenu() { canReturnToMenu_ = true; }

    // Apply local Options (audio / camera / UI scale) live -- at startup and
    // whenever the in-game Options screen changes a value. Never touches the sim.
    void applySettings(const tak::Settings& s);

    // main()'s live settings, so the in-game Options screen can edit + persist them.
    void setSettings(tak::Settings* s) { settings_ = s; }

    // Open the in-game Options overlay (from the Esc menu). onChange applies audio,
    // camera, UI scale and window state live; the host saves on close.
    void openOptions();
    // Open the hotkey-config overlay (from the Options screen's CONTROLS button).
    void openHotkeys();
    // Persist / read the resume ticket (gameId + rotating token) so a killed
    // client can rejoin its held slot on restart.
    void writeResume(uint32_t gid, uint64_t tok) const;
    bool readResume(uint32_t& gid, uint64_t& tok) const;
    // The map's player capacity = its start-position count (2..8). The server has
    // no map data, so a creating client tells it how many slots the map supports.
    uint8_t mpCapacity() const;

    void input(const SDL_Event& e, int winW, int winH);

    // Contextual right-click order, extracted so the reclaim right-drag can defer to
    // it on a plain click: transport unload/load, assist/guard, attack, or move.
    void rightClickOrder(float wx, float wz, bool queue);
    // Retail's order line: beads strung between a selected unit's queued orders.
    void drawOrderTrails(int mvw, int winH);
    static constexpr int kTrailUnits = 3;    // retail beaded only its few "focus" units
    static constexpr int kTrailBeads = 96;   // per leg, so a map-long order can't run away

    // A mobile, reclaim-capable builder of ours is selected (drives the right-drag).
    bool haveReclaimer();
    int firstReclaimer();
    // Right-drag "clear this area": order the builder to reclaim every reclaimable
    // feature in the box, nearest-first (approximating retail's greedy re-scan).
    void issueReclaimBox(float x0, float z0, float x1, float z1, bool queue);

    // Attach the multiplayer client. The game world is set up later, from the
    // server's GameStarting (startMpGame), not from the constructor.
    void setMpClient(tak::net::MpClient* mp);
    bool isNet() const { return mp_ != nullptr; }
    // Run the sim on its own worker thread (Stage B1c). On by default for interactive
    // games; the headless harness disables it (inline == deterministic) unless verifying.
    void setSimThreadMode(bool on) { simThreadMode_ = on; }
    // Benchmark run: an all-AI watch game on Ulasem Arena with the staged benchmark spawn
    // plan (see MatchConfig::benchmark). Drives the createGame/seat path in mpAutoStep.
    void setBenchmark(int level) { benchmarkLevel_ = level; benchmarkMode_ = level > 0; }
    bool benchmarkMode() const { return benchmarkMode_; }
    void setBenchmarkServerPid(long pid) { benchServerPid_ = pid; }   // local takserver, for its metrics
    bool benchmarkStatsShown() const { return benchStatsShown_; }
    // Establish the t=0 baseline for the CPU% deltas (called once when the run starts).
    void benchmarkBaseline();
    // Record one metrics row for game-second `gameSec`: client/server CPU% (delta over the
    // wall interval since the last sample), RSS, fps and sim speed.
    void pushBenchSample(int gameSec);
    // Take a metrics sample each time the game clock crosses a 5s milestone; when it reaches
    // the benchmark end tick, flag the stats overlay. Called once per rendered frame.
    void benchmarkSample();
    // Esc during a run: end the benchmark now. If the sim has started, record a closing
    // sample at the current second and show the stats screen; otherwise bail to the menu.
    void stopBenchmark();
    // Benchmark flythrough: each 10s leg tracks one AI's monarch (leg 0 -> AI1 .. leg 7 ->
    // AI8), starting fully zoomed out and zooming in across the leg, then cutting to the
    // next. Camera only -- view state is never hashed. Called each frame during the run.
    void benchmarkCamera(float dt, int winW, int winH);
    // The benchmark results overlay: a per-milestone table of client/server CPU + memory,
    // fps and sim speed, plus the display settings that produced them. A single centered
    // panel, scaled up on big displays. DONE / Esc / click -> main menu.
    void renderBenchmarkStats(int winW, int winH);
    // Flush + join the sim worker (drains any pending simInbox_ ticks first). For the headless
    // harness to call before reading the final world hash, so it reflects every processed tick.
    // Drains: the headless harness needs the final world hash to reflect every
    // tick it pushed, so this one waits for the backlog on purpose.
    void shutdownSim() { stopSimThread(/*drain=*/true); }
    // Dev --keytest helper: pick an own unit (builder preferred) + live count from the render
    // SNAPSHOT, so the harness never iterates live world_.units() while the worker ticks.
    std::pair<int, size_t> keytestPickOwnUnit() const;

    // Route a command: offline it applies immediately; in a net game it is queued
    // for the server, which stamps ownership and sequences it into a tick bundle.
    void issue(tak::net::Command c);

    // Set up the world for a multiplayer match from the server's final slot
    // table: one player per used slot (sim player index == slot), teams/colours
    // per slot, a monarch spawned at a start position each, seeded starting mana.
    void startMpGame(const tak::net::RoomView& room, uint32_t seed);

    // One networked frame: pump the connection, send this frame's local orders,
    // and simulate every tick the server has delivered a bundle for. Returns
    // false when the game/connection ends (see netError()).
    bool mpStep();

    // ---- replay playback (.takrep) ----------------------------------------
    // Build the world from a recorded match config and feed it the bundle log.
    void startReplay(tak::sim::MatchConfig cfg,
                     std::vector<tak::net::Bundle> bundles);
    bool replayMode() const { return replayMode_; }
    // Advance playback by `dt` (real seconds), scaled by the game-speed control;
    // Pause freezes it. Applies each recorded bundle then ticks the world.
    void replayStep(float dt);
    size_t replayTick() const { return replayTick_; }
    size_t replayLength() const { return replayBundles_.size(); }

    uint64_t worldHashPublic() const { return world_.stateHash(); }
    int missionOutcomePublic() const { return world_.missionOutcome(); }
    // Did this game resolve, and how? +1 win, -1 loss, 0 still running. Unlike
    // missionOutcomePublic this also covers a skirmish/MP last-team-standing result.
    int outcomePublic() const { return outcome_; }
    // The end-of-game statistics table, in slot order. Read from the render frame
    // (not live world_), so it is safe to call after the sim thread has stopped.
    tak::ResultStats resultStats() const;
    size_t aliveUnits() const;

    // Drive one iteration of the multiplayer lobby + game. autoMode: 0 = don't
    // auto-drive the lobby (a real UI will), 1 = auto-host (create + start at 2+
    // ready), 2 = auto-join the first game. Returns false when the session ends.
    // (M3 uses the auto modes; the interactive lobby UI is follow-on work.)
    // AI difficulty for the headless / auto seat paths: TAK_AI_LEVEL (0/1/2), default
    // Normal. The interactive lobby sets it per-slot via the Room UI instead.
    static uint8_t aiLevelEnv() {
        const char* e = tak::devEnv("TAK_AI_LEVEL");
        int v = e ? std::atoi(e) : 2;   // default normal (0=passive..4=absurd)
        return uint8_t(v < 0 ? 0 : v > 4 ? 4 : v);
    }

    bool mpAutoStep(int autoMode, const std::string& mapId, bool crusades);

    void applyEvent(const tak::net::Event& e) { tak::sim::applyEvent(world_, e); }

    // Apply one command (shared with the server's referee sim, so both mutate
    // the world identically).
    void apply(const tak::net::Command& c) { tak::sim::applyCommand(world_, registry_, c); }

    // One lockstep step: returns false while stalled waiting for the peer.
    uint32_t netTick() const { return netTick_; }
    tak::sim::World& worldRef() { return world_; }
    void selectOnly(int id) { if (spectating_) return; selection_.clear(); selection_.push_back(id); }

#ifndef NDEBUG
    // Harness hook: select one of the local player's mobile units and hand it a
    // short queue of moves, so a screenshot run can photograph the order line
    // without having to synthesise shift+right-clicks at guessed coordinates.
    // Returns the unit id, or 0 if there was nothing to order.
    int debugQueueDemo();
#endif
    const std::string& netError() const { return netError_; }

    void setFollow(float zoom) { follow_ = true; mapView_.setZoom(zoom); }

    void amphibDemo();

    void navyDemo();

    void creonDemo();

    void missionTest();

    void testBuild();

    void lookAt(float x, float z);

    std::string lodeUnit;
    void soundTest();
    void faceTest();
    void fireTest();
    void lodeTest();

    void guardTest();

    void marchTo(float dx, float dz);

    // Smoothed render FPS for the F4 overlay (set from the main loop each frame).
    void setFps(float f) { fps_ = fps_ > 0 ? fps_ * 0.9f + f * 0.1f : f; }

    // Choose which player-colour variant a player's units render in.
    void setPlayerColor(int player, int slot);

    // Mouse edge scrolling: pan the camera while the cursor rests in the margin
    // at a window edge — including the very bottom of the screen (the HUD panel
    // never sits at the extreme edge, so this doesn't fight the build icons).
    void edgeScroll(float dt, float zm);

    // Real-time per-RENDER-frame work: camera and audio that must stay smooth
    // regardless of the deterministic sim. In a net game the sim (update()) only
    // advances when a server bundle arrives, so anything lag-sensitive that isn't
    // "game state" belongs here, not in update() -- panning, zoom-follow, edge
    // scroll, camera shake, and music keep running even while the sim is stalled
    // waiting on the network. Called every frame in SP and net alike.
    void cameraFrame(float dt);
    // The SIM-MUTATING half of a game step: the world tick plus every block that
    // writes world or outcome state (god summons, scenario spawns, the legacy
    // mission VM, dev harnesses, the victory check). In multiplayer catch-up this
    // runs for EVERY drained bundle -- it is what lockstep requires -- while the
    // cosmetic half below runs ONCE per frame afterwards.
    void simStep(float dt);

    // Snapshot every unit's post-tick pose (x/z/heading) so the render can GLIDE units
    // between the 30Hz sim ticks instead of stepping them (visible stutter above 30Hz).
    // prev <- last tick's curr, curr <- now; a large jump (teleport / id reuse / respawn)
    // reseeds so we don't zip across the map. Client-only, viewer-only -- never hashed.
    void captureFrame();

    // Interpolated render pose for a unit: glides x/z (and shortest-path heading) between
    // the last two ticks by interpAlpha_ (0..1 through the current tick interval). Falls
    // back to live state when the option is off or there's no history. Viewer-only.
    void interpPose(const UnitR& s, float& x, float& z, float& heading) const;
    // Lookup helper: the render snapshot record for a unit id (empty default if absent).
    const UnitR& frameUnit(int id) const;
    // Pointer form matching world_.unit()'s semantics: nullptr unless the id is a unit that
    // is LIVE this tick (captured with this frame's gen). Use to replace world_.unit(id) in
    // render/HUD reads (the null check keeps working).
    const UnitR* frameUnitP(int id) const;
    // Player snapshot accessors (mirror world_.player()/numPlayers() for the HUD).
    const PlayerR& framePlayer(int p) const;
    int frameNumPlayers() const { return front().numPlayers; }
    // Fog snapshot accessors (mirror world_.cellVisible/visibility/visW/visGeneration).
    const std::vector<uint8_t>& frameVisibility() const { return front().vis; }
    int frameVisW() const { return front().visW; }
    int frameVisH() const { return front().visH; }
    uint32_t frameVisGeneration() const { return front().visGen; }
    bool cellVisibleR(float x, float z) const;
    // More snapshot accessors mirroring the World calls the render used to make directly.
    const std::vector<tak::sim::World::HitFx>& frameHits() const { return front().hits; }
    // Impacts are ONE-SHOT and the sim clears them every tick, while the render
    // only ever sees the newest published snapshot. Whenever the sim outruns the
    // renderer -- replay catch-up, or simply a frame rate under the 30 Hz tick --
    // whole snapshots are skipped and their impacts went with them: silently no
    // sound, no bolt, no effect for those hits. So the worker also appends them
    // here, and the render drains this instead, which survives skipped frames.
    // Bounded: a renderer that has fallen catastrophically behind drops the
    // OLDEST rather than growing without limit, since stale impacts are the ones
    // least worth drawing.
    static constexpr size_t kMaxPendingHits = 4096;
    std::mutex hitQueueMutex_;
    std::deque<tak::sim::World::HitFx> hitQueue_;
    int frameWinningTeam() const { return front().winningTeam; }
    // world_.discoActive/headbangActive(p) == players_[p].{disco,headbang}Left > 0.
    bool frameDiscoActive(int p) const { return framePlayer(p).discoLeft > 0; }
    bool frameHeadbangActive(int p) const { return framePlayer(p).headbangLeft > 0; }
    // world_.queuedCount(builderId,type): count of that type queued on the builder.
    int frameQueuedCount(int builderId, const tak::sim::UnitType* type) const;

    // The DISPLAY half: impact sounds/effects, particles, animation state and the
    // COB VMs, timers, camera-follow. Runs once per rendered frame with the game
    // time actually covered -- during a rejoin/spectate catch-up this used to run
    // its full body up to 512x per frame (~18ms of pure pool dispatch alone).
    void cosmeticStep(float dt);

    // Advance every unit's animation VM ONCE PER RENDER FRAME (decoupled from the 30Hz sim
    // tick). Each VM is independent: it only reads sim state through onGet (no writes), so
    // ticking it off the main thread is safe; the EMIT_SFX/PLAY_SOUND it stashes is drained
    // here serially. Viewer-only -- never touches the sim hash. `realDt` is the wall-clock
    // frame time, scaled to the game's apparent speed so the walk cycle matches movement.
    void animFrame(float realDt);

    // How fast the world appears to advance (so the animation walk cycle stays in step
    // with movement): the server-set game speed in a net game, else the local multiplier.
    float animSpeed() const;

    void update(float dt);

    // Create textures (terrain chunks, minimap) before the render pass.
    void prepare(int winW, int winH);

    // Fetch and reset the per-draw sub-phase timers (for TAK_PROF).
    void takeProf(double& projMs, double& submitMs, double& shadowMs, double& simMs,
                  long& unitsDrawn, long& shadowVerts);
    // Current accumulators WITHOUT resetting, so a per-frame delta can be taken.
    void profPeek(double& projMs, double& submitMs, double& shadowMs) const {
        projMs = profProjMs_; submitMs = profSubmitMs_; shadowMs = profShadowMs_;
    }

    void draw(int winW, int winH);

    void advance(float seconds);

    void setTrace(bool on) { trace_ = on; sounds_.setVerbose(on); }

    // Smallest window that still fits the widest build-icon row at full size
    // (icons are 60px on a 66px pitch, centred over the map viewport with a small
    // margin, beside the fixed right-hand panel). Enforced in main() so the icons
    // never have to shrink and the last builder tier is never clipped off.
    int minWindowWidth() const;

    // ---- Animated mouse cursor (retail anims/cursors.gaf) --------------------------
    // Draw the retail cursor on top of everything at native resolution. main() calls
    // this after the (optional) AA downscale, right before present, so it is crisp and
    // unambiguously topmost. Lazily loads the cursor art and hides the OS arrow once.
    // We never restore the OS arrow on teardown: the menu and the game both hide it and
    // draw their own, so restoring it only makes the arrow flash during the next screen's
    // (slow) load; the desktop cursor returns on its own when the window is destroyed.
    void drawCursorOverlay();

private:
    // The armed-order (command button / hotkey) -> its cursor. Fight-move reuses the
    // Attack glyph, matching retail (KINGDOMS.icd).
    static tak::CursorId cursorForCmd(char cmd) {
        switch (cmd) {
            case 'm': return tak::CursorId::Move;
            case 'f': return tak::CursorId::Attack;    // fight-move = tinted attack
            case 'a': return tak::CursorId::Attack;
            case 'p': return tak::CursorId::Patrol;
            case 'g': return tak::CursorId::Defend;    // guard
            case 'c': return tak::CursorId::Reclaim;
            case 'r': return tak::CursorId::Repair;
            case 'l': return tak::CursorId::Load;
            case 'u': return tak::CursorId::Unload;
            default:  return tak::CursorId::Normal;
        }
    }

    // Which cursor to show this frame, from the current UI/order state and what is under
    // the pointer -- the retail two-level scheme (an armed order beats plain hover).
    // `fightTint` is set when the cursor is the fight-move ('f') Attack glyph, which the
    // caller draws tinted so it reads apart from a real attack order.
    tak::CursorId desiredCursor(bool& fightTint);

    // Plain-hover cursor: classify what is under the world point, mirroring the priority
    // in rightClickOrder() so the pointer previews the order a right-click would issue.
    tak::CursorId hoverCursor(float wx, float wz);
    // Screen-space sprite hit test: the unit's projected model bounds at its
    // DRAWN position (terrain lift + flyer altitude), floored for tiny units --
    // the same region click-select uses, so the hover cursor and a click always
    // agree. Optional out: squared distance to the sprite centre (tie-breaks).
    bool unitUnderCursor(const UnitR& u, float mx, float my, float* d2 = nullptr) {
        float zms = mapView_.zoom();
        SDL_FPoint p = unitScreen(u);
        const SDL_FRect& hb = unitHitBox(u.type);
        float ax = p.x, ay = p.y + 12.0f * zms;         // sprite draw anchor
        float cx = ax + (hb.x + hb.w * 0.5f) * zms;
        float cy = ay + (hb.y + hb.h * 0.5f) * zms;
        float hw = std::max(hb.w * zms * 0.5f, 9.0f);
        float hh = std::max(hb.h * zms * 0.5f, 9.0f);
        if (std::fabs(mx - cx) > hw || std::fabs(my - cy) > hh) return false;
        if (d2) { float dx = cx - mx, dy = cy - my; *d2 = dx * dx + dy * dy; }
        return true;
    }

    // Everything about a model piece that never changes: whether it draws at all,
    // and its primitives' texture names folded to lower case. collect() used to
    // work this out per piece and per primitive on EVERY unit EVERY frame -- two
    // std::string allocations and a case fold each -- and then again for the
    // shadow pass. At ~1500 units that is six figures of throwaway string work a
    // frame for answers that were fixed when the model loaded.
    struct PieceMeta {
        bool skip = false;                  // ground plate / *off duplicate: draws nothing
        std::vector<std::string> primTex;   // lowercased per primitive ("" = untextured)
        std::vector<PieceMeta> children;    // 1:1 with Object::children
    };
    // The rules themselves, in ONE place, used both to precompute the tree and to
    // answer for a model that has no cached tree (ghosts, portraits). A second copy
    // of these predicates is how the cache and the live path would silently drift.
    static void pieceMetaFor(const tak::tdo::Object& o, bool isRoot, PieceMeta& m) {
        std::string oname = o.name;
        std::transform(oname.begin(), oname.end(), oname.begin(), ::tolower);
        auto ends = [&](const char* suf) {
            size_t n = std::strlen(suf);
            return oname.size() >= n && oname.compare(oname.size() - n, n, suf) == 0;
        };
        // Hidden pieces: ground-reference plates and deactivated-state duplicates
        // (*off), which the game shows only via activation scripts we don't run. The
        // model ROOT is always the flat base plate (AraGP, zonnull, or just the unit
        // name like zontrain/zonharpy1) with the real model in its children, so its
        // own primitives are skipped unconditionally.
        m.skip = isRoot || ends("gp") || ends("null") || ends("off") ||
                 oname.find("ground") != std::string::npos ||
                 oname.find("gpoly") != std::string::npos ||
                 oname.find("gpoint") != std::string::npos;
        m.primTex.clear();
        m.primTex.reserve(o.primitives.size());
        for (const auto& p : o.primitives) {
            std::string n = p.texture;
            std::transform(n.begin(), n.end(), n.begin(), ::tolower);
            m.primTex.push_back(std::move(n));
        }
    }
    // Precompute the whole tree for a model (once, at registration).
    static void buildPieceMeta(const tak::tdo::Object& o, PieceMeta& m, bool isRoot = true) {
        pieceMetaFor(o, isRoot, m);
        m.children.resize(o.children.size());
        for (size_t i = 0; i < o.children.size(); ++i)
            buildPieceMeta(o.children[i], m.children[i], false);
    }

    struct Visual {
        tak::tdo::Model model;
        PieceMeta meta;   // precomputed once; see collect()
    };
    struct EffectAnim;   // defined below; Anim only needs the pointer type
    struct Anim {
        std::unique_ptr<tak::cob::Vm> vm;
        // Points at the shared per-TYPE CobCache.pieceNames (node-stable in cobCache_,
        // which outlives every Anim), not a per-unit copy -- ~25 MB saved at 38k units.
        const std::vector<std::string>* pieceNames = nullptr;
        bool walking = false;
        bool dying = false;
        bool producing = false;
        bool building = false;   // mobile builder actively working a site (conjure anim)
        bool firing = false;
        bool flying = false;
        bool airborne = false;   // true while the flight animation should run
        float altitude = 0;      // flyers: 0 grounded, rising to cruiseAlt in flight
        // The ground datum this flyer is currently holding its altitude above,
        // walked toward flyerGround() at a limited rate. Retail never snaps a
        // flyer's Y: a servo in the flyer mover moves it max(1, speed/4) world
        // units per 30Hz tick toward the commanded altitude. Without that the
        // dilated sector datum is a STEP function -- it changes the instant the
        // unit crosses a 128-unit sector boundary -- and the flyer teleports
        // vertically at every boundary. That jump is the whole reason the servo
        // exists.
        float groundY = 0;
        bool groundInit = false;
        // Flyer attitude (bankscale/pitchscale): a smoothed roll into the turn and
        // pitch into the climb, derived from how the unit is actually moving. Retail
        // flyers visibly lean; ours flew dead level through every turn and dive.
        float prevHeading = 0, prevAlt = 0;
        float bank = 0, pitch = 0;
        bool attitudeInit = false;
        int flyGate = 8;         // static index that this unit's `fly` gates on
        int moveGate = 0;        // static index this unit's `walk` gates on (see walkGateOf)
        bool hasWalk = false;    // has a walk/walk_legs/tread script (vs a Create-ambient mover)
        bool hasAim = false;     // has an AimWeapon script (turret/torso target tracking)
        int aimTarget = 0;       // unit id the last AimWeapon start tracked (0 = none)
        int fireSlot = 0;        // multi-weapon: alternate FireWeapon's weapon index so
                                 // both crews (e.g. araat's two bows) loose over time
        float aimNext = 0;       // animClock_ time of the next aim refresh
        bool hasFlinch = false;  // has a HitByWeapon script (damage flinch)
        bool hasWind = false;    // has a WindChange script (flags/sails)
        bool hasFlightSM = false;// has BeginFlight/BeginLanding: drive the drake's VTOL
                                 // state machine (Create ambients + statics) instead of
                                 // the generic reset()+fly/land flyer path
        bool hasActivate = false;// onOffable + has Activate: watch u.active for door swing
        bool hasGateDoors = false;// onOffable + has `open`: a gate -> auto-open on proximity
        bool hasQueryWeapon = false;  // has QueryWeapon: resolve the muzzle emit piece
        bool flyAmbient = false; // canFly airship driven by Create ambients (MotionControl /
                                 // rotor loops), NO fly/land state machine: run Create + a
                                 // moving signal (setSFXoccupy/MoveRate), never reset()
        bool active = true;      // last active/door-open state (onoffable/gate swing edge)
        bool cloaked = false;    // last sim cloak state (StartCloaking/StopCloaking edge)
        bool hasCloakAnim = false;   // COB defines StartCloaking (araspy, npcheket)
        float gateNext = 0;      // animClock_ of the next gate proximity rescan (stagger)
        int windStamp = 0;       // last windGen_ this unit received (0 = never)
        bool hasMelee = false;   // has MoveWatcher/MeleeControl: the COB drives its own
                                 // gait retail-style (Create ambients poll GET 29/28/34
                                 // and CALL walk_* themselves) -- the manual walk state
                                 // machine must stay out, and NOTHING may reset the VM
        // emit-sfx (piece, sfxType) captured off the worker thread; drained on the
        // main thread after the parallel VM tick (SDL/effects_ are main-thread only).
        std::vector<std::pair<int, int32_t>> pendingSfx;
        std::vector<int32_t> pendingSnd;   // COB PLAY_SOUND name indices, drained on main
        std::vector<std::pair<int, int32_t>> pendingExplode;   // EXPLODE (piece, flags)
        bool cobSounds = false;   // script plays its own audio: skip the generic stand-ins
        int workId = 0;           // site/target the build anim last fired for (one-shot per job)
        // Continuous ambient fire/smoke: retail runs one persistent emitter per unit,
        // so we draw ONE looping flame/smoke, kept alive while the emit-loop re-fires
        // (fireT/smokeT = seconds since the last emit of each). Smooth, not per-emit.
        const EffectAnim* fireFx = nullptr;
        const EffectAnim* smokeFx = nullptr;
        float fireT = 1e9f, smokeT = 1e9f;
        float fireLift = 0, smokeLift = 0;   // screen lift of the emitting piece
        bool usesGlow = false;               // model has an animated glow texture
    };

    // Turn a COB emit-sfx (piece, packed type) into a one-shot world-space effect at
    // the unit. The Sacred Fire's FireControl loop re-emits every ~0.5s, so the short
    // flame/smoke puffs stack into a continuous flicker (as retail's persistent
    // particle emitter does). Cosmetic; not hashed.
    void emitSfx(const UnitR& u, Anim& a, int piece, int32_t sfx);
    // COB EXPLODE: the piece flies off as a debris chunk (retail icd 0x50dd20)
    // plus the TA-flag extras (SMOKE/FIRE bits, BITMAPn explosion classes).
    void explodePiece(const UnitR& u, Anim& a, int piece, int32_t flags);
    // Map a packed COB sfx code to the retail effect GAF sequence. (KINGDOMS.icd
    // emitSfx @0x50da20: the 0x100 bit flags the extended emitter family, low bits
    // pick the effect -- 4/5/6 = damage-flame small/med/large from anims/flames.gaf,
    // 1/2/3 = smoke/steam from anims/smoke.gaf.)
    static const char* sfxAnimFor(int32_t sfx) {
        int low = sfx & 0xFF;
        if (sfx & 0x100) {
            if (low == 6) return "flames:flame large";
            if (low == 5) return "flames:flame medium";
            if (low == 4) return "flames:flame small";
            if (low >= 1 && low <= 3) return "smoke:smoke01";
            return nullptr;
        }
        return (low == 0 || low == 1) ? "flames:flame large" : nullptr;
    }
    // Accumulated model-Y (height above the unit's ground origin) of a named piece,
    // for lifting the effect onto it. Ground-level pieces (the Sacred Fire's root)
    // give 0; a smokestack piece gives its height.
    static bool findPieceY(const tak::tdo::Object& o, const std::string& name,
                           float acc, float& out) {
        float y = acc + o.y;
        std::string on = o.name;
        std::transform(on.begin(), on.end(), on.begin(), ::tolower);
        if (on == name) { out = y; return true; }
        for (const auto& c : o.children)
            if (findPieceY(c, name, y, out)) return true;
        return false;
    }
    float pieceLift(const UnitR& u, const Anim& a, int piece);

    // The `fly` script's first instruction is a PUSH_STATIC that gates the
    // whole animation; different flyers use different indices (zonhunt=8,
    // zongod/zonharp=7). Read it straight from the bytecode.
    static int flyGateOf(const tak::cob::Vm& vm) {
        const auto& f = vm.file();
        int si = f.scriptIndex("fly");
        if (si < 0) return 8;
        uint32_t e = f.scripts[size_t(si)].entry;
        if (e + 1 < f.code.size() && f.code[e] == 0x10021004)   // PUSH_STATIC
            return int(f.code[e + 1]);
        return 8;
    }

    // Word length of a COB opcode (opcode word + inline args) for the ops that can
    // precede a walk gate; 0 = one we don't decode (caller stops). Mirrors vm.cpp.
    static int cobOpLen(uint32_t op) {
        switch (op) {
            case 0x10021001: case 0x10021002: case 0x10021004:   // PUSH_CONST/LOCAL/STATIC
            case 0x10023002: case 0x10023004:                    // POP_LOCAL/STATIC
                return 2;
            case 0x10022000: case 0x10024000:                    // CREATE_LOCAL / POP_STACK
            case 0x10031000: case 0x10032000: case 0x10033000: case 0x10034000:  // + - * /
            case 0x10035000: case 0x10036000: case 0x10037000: case 0x10038000:  // AND OR XOR NOT
            case 0x10039000: case 0x1003A000: case 0x1003B000:   // SHL SHR MOD
                return 1;
            default: return 0;
        }
    }

    // The static index a unit's WALK cycle gates its piece motion on -- the client's
    // state machine sets this to 1 while moving so the walk script actually animates.
    // Most ground units (araking/tarnecro/zonlord) read static 0, but a HOVER unit
    // like the Veruna monarch (vermage) reads static 3 -- retail's MoveWatcher thread
    // (which we don't run) fills it. The enabling gate is the FIRST PUSH_STATIC before
    // the walk script's first JUMP_IF_FALSE; decode forward to it, since a few units
    // (e.g. the Taros tarmind) front-load a loop-counter setup before the gate.
    // Tries walk / walk_legs / tread.
    // True if the unit has a leg/tread walk cycle. A mobile unit WITHOUT one (ships'
    // oars, wheeled war-machines' wheels/props) animates via its Create ambient loop
    // instead, so registerUnit starts Create for it and the walk state machine leaves
    // its VM alone (a walk-transition reset would wipe the ambient loop).
    static bool hasWalkCycle(const tak::cob::File& f) {
        for (const char* name : {"walk", "walk_legs", "tread"})
            if (f.scriptIndex(name) >= 0) return true;
        return false;
    }

    static int walkGateOf(const tak::cob::File& f) {
        for (const char* name : {"walk", "walk_legs", "tread"}) {
            int si = f.scriptIndex(name);
            if (si < 0) continue;
            uint32_t e = f.scripts[size_t(si)].entry;
            for (int guard = 0; guard < 24 && e + 1 < f.code.size(); ++guard) {
                uint32_t op = f.code[e];
                if (op == 0x10021004) return int(f.code[e + 1]);   // PUSH_STATIC -> the gate
                if (op == 0x10066000) break;                       // JUMP_IF_FALSE -> no static gate
                int len = cobOpLen(op);
                if (len <= 0) break;                               // unknown op: give up, fall to 0
                e += uint32_t(len);
            }
        }
        return 0;
    }

    // At max veterancy, a unit with a `veteranmodel` swaps its mesh for the
    // fancier promoted 3DO (same piece structure, so the COB/anim carries over).
    void maybeSwapVeteranModel(const UnitR& u);
    void maybeSwapCorpseModel(const UnitR& u);

    // Client-side per-unit setup (model + COB animation VM). Takes id+type only (not a
    // Unit/UnitR) so it is callable from either the sim path (spawn) or the render path
    // (cosmeticStep, off front().live) without touching live world_.
    void registerUnit(int id, const tak::sim::UnitType* type);

    // Manifest player `t`'s faction god at its army's centre (once favour fills).
    void summonGod(int t);

    int spawn(const std::string& typeId, float x, float z, float heading, int player);

    void loadTextures();

    const tak::cob::PieceState* pieceFor(const Anim* a, const std::string& objName) const;

    // The 3DO model for a type, loading it on demand (a queued build may have
    // no live unit of that type yet).
    const tak::tdo::Model* ghostModel(const std::string& typeId);

    // Draw a translucent, faintly blue ghost of a building where it will be
    // built later (a queued or not-yet-started site).
    // Draw a projectile that ships a real 3DO mesh (FBI `model`): arrows, spears,
    // daggers, boulders, harpoons, the egg bomb -- 39 weapons, every one of which
    // was a generic yellow streak before. Same projection as a ghost, but solid,
    // lifted by the shot's altitude and yawed along its flight.
    void drawShotModel(const std::string& name, int player, float x, float z,
                       float altPx, float facing) {
        const tak::tdo::Model* model = ghostModel(name);
        if (!model) return;
        tris_.clear();
        SDL_Texture* atlas = atlasFor(colorSlot_[player & 7]);
        collect(tris_, atlas, model->root, Xform{}, nullptr, facing, player);
        if (tris_.empty()) return;
        std::stable_sort(tris_.begin(), tris_.end(),
                         [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
        float zm = mapView_.zoom();
        float ax = (x - mapView_.offX()) * zm - terrainLiftX(x, z) * zm;
        float ay = (z - mapView_.offY()) * zm - terrainLift(x, z) * zm - altPx;
        triBatch_.clear();
        SDL_Texture* cur = nullptr;
        auto flush = [&] {
            if (!triBatch_.empty())
                SDL_RenderGeometry(ren_, cur, triBatch_.data(),
                                   int(triBatch_.size()), nullptr, 0);
            triBatch_.clear();
        };
        for (auto& t : tris_) {
            if (t.tex != cur) { flush(); cur = t.tex; }
            for (int i = 0; i < 3; ++i) {
                SDL_Vertex v = t.v[i];
                v.position.x = v.position.x * zm + ax;
                v.position.y = v.position.y * zm + ay;
                triBatch_.push_back(v);
            }
        }
        flush();
    }

    void drawGhostAt(const tak::sim::UnitType* type, float x, float z,
                     bool invalid = false) {
        const tak::tdo::Model* model = ghostModel(type->id);
        if (!model) return;
        tris_.clear();
        SDL_Texture* atlas = atlasFor(colorSlot_[localPlayer_ & 7]);
        collect(tris_, atlas, model->root, Xform{}, nullptr, 0.0f, localPlayer_);
        std::stable_sort(tris_.begin(), tris_.end(),
                  [](const Tri& a, const Tri& b) { return a.depth > b.depth; });
        float zm = mapView_.zoom();
        // The ghost lifts onto the relief exactly like the finished unit/building will.
        float ax = (x - mapView_.offX()) * zm - terrainLiftX(x, z) * zm;
        float ay = (z - mapView_.offY()) * zm - terrainLift(x, z) * zm;
        // Batch by texture (flush on change), like a live unit.
        triBatch_.clear();
        SDL_Texture* cur = nullptr;
        auto flush = [&] {
            if (!triBatch_.empty())
                SDL_RenderGeometry(ren_, cur, triBatch_.data(),
                                   int(triBatch_.size()), nullptr, 0);
            triBatch_.clear();
        };
        for (auto& t : tris_) {
            if (t.tex != cur) { flush(); cur = t.tex; }
            for (int i = 0; i < 3; ++i) {
                SDL_Vertex v = t.v[i];
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
                    v.color.r = Uint8(v.color.r * 0.55f);   // shift toward blue
                    v.color.g = Uint8(v.color.g * 0.8f);
                }
                triBatch_.push_back(v);
            }
        }
        flush();
    }

    // Per-unit screen-space geometry, built in parallel each frame (the expensive
    // model projection) so the single render thread only submits draw calls.
    struct UnitGeom {
        std::vector<SDL_Vertex> verts;                  // transformed, coloured
        std::vector<std::pair<SDL_Texture*, int>> runs; // (texture, vertex count)
        float ax = 0, ay = 0, occY = 0, alt = 0;
        bool canFly = false;
        // The projected silhouette, built HERE (on the worker) rather than on the
        // render thread. Measured at 1162 units: the serial version cost ~145ms of
        // a ~208ms draw -- the whole submit phase -- because every unit walked its
        // model a second time on the main thread while the body walk was already
        // parallel. Same traversal, same pool, so it costs what the body costs.
        std::vector<SDL_Vertex> shadowVerts;
        SDL_FPoint shadowLo{}, shadowHi{};   // screen bounds of the above
    };
    std::vector<const UnitR*> visUnits_;
    std::vector<SDL_Vertex> unitBatch_, shadowBatch_;   // cross-unit render batches
    // Body pass assembled in parallel: plan offsets serially, scatter the vertex
    // copies across the pool, then replay the draw ops. Keeps depth order exact.
    std::vector<SDL_Vertex> bodyVerts_;
    struct FeatureInst;   // defined below; DrawOp only needs the pointer type
    // `layer` puts AIRBORNE units in a pass of their own, after every feature.
    // Retail paints the world as bucketed z rows -- ground units interleaved with
    // tall features -- and then makes a SECOND full sweep over every row for the
    // units whose occupancy is not GROUND (icd: pass 1 gate 0x4fc9f3 requires
    // unit[0x130]&3 == 1, pass 2 at 0x4fcbd3 requires != 1). A flying unit
    // therefore paints over every tree and rock and cannot be occluded by feature
    // clutter. Sorting by z alone, as we did, let any tree with a larger z draw
    // over a flyer at any altitude -- which is what "flying under trees" was.
    struct PaintItem { float z; const UnitR* u; const FeatureInst* f; int layer = 0; };
    std::vector<PaintItem> paintItems_;   // per-frame painter list (capacity reused)
    std::unordered_set<int> targetSet_;   // per-frame attack-target ids (reused)
    // Parsed COB scripts shared per unit type (see registerUnit).
    struct CobCache {
        std::shared_ptr<const tak::cob::File> file;
        std::vector<std::string> pieceNames;
        bool hasSounds = false;   // any PLAY_SOUND op: the script provides its own audio
        int moveGate = 0;         // walk-cycle moving-flag static index (walkGateOf)
        bool hasWalk = false;     // has a walk/walk_legs/tread script (hasWalkCycle)
        bool hasMelee = false;    // has MoveWatcher/MeleeControl (retail self-driven gait)
    bool hasCloakAnim = false;   // defines StartCloaking (araspy, npcheket)
        bool hasAim = false;      // has an AimWeapon script
        bool hasFlinch = false;   // has a HitByWeapon script
        bool hasWind = false;     // has a WindChange script
        bool hasFlightSM = false; // has BeginFlight/BeginLanding (drake VTOL state machine)
        bool hasActivate = false; // has an Activate script (onOffable door/power toggle)
        bool hasQueryWeapon = false;  // has QueryWeapon (muzzle emit piece out-param)
        bool hasFly = false;      // has a `fly` script
        bool hasMotionControl = false;  // has MotionControl (airship gait ambient)
        bool hasOpen = false;     // has an `open` door-swing script (gate)
    };
    std::unordered_map<std::string, CobCache> cobCache_;
    struct CopyTask { int geom, src, count, dst; };
    struct DrawOp { const UnitR* u; const FeatureInst* f;
                    SDL_Texture* tex; int start, count; };   // seg if u&&f both null
    std::vector<CopyTask> copyTasks_;
    std::vector<DrawOp> drawOps_;
    double profProjMs_ = 0, profSubmitMs_ = 0;   // TAK_PROF sub-phase timers (main thread)
    double profShadowMs_ = 0;   // the projected-silhouette pass, inside submit
    long profUnits_ = 0;        // visible units accumulated over the sampled frames
    long profShadowVerts_ = 0;  // shadow vertices copied + submitted, likewise
    // "other" broken out: a periodic hitch was traced into this bucket and there
    // was no way to say WHICH of terrain / fog / effects / HUD it was.
    double profTerrainMs_ = 0, profFogMs_ = 0, profFxMs_ = 0, profHudMs_ = 0;
    double profAtlasMs_ = 0, profBodyMs_ = 0;
  public:
    void profOther(double& terrain, double& fog, double& fx, double& hud) const {
        terrain = profTerrainMs_; fog = profFogMs_; fx = profFxMs_; hud = profHudMs_;
    }
    void profOther2(double& atlas, double& body) const {
        atlas = profAtlasMs_; body = profBodyMs_;
    }
  private:
    std::atomic<int64_t> profSimTicks_{0};        // sim-tick time in raw perf-counter ticks,
                                                  // accumulated by the worker, read/reset on main.
                                                  // Integer atomic -- portable (atomic<double>
                                                  // arithmetic isn't supported by Apple libc++).

    // Texture atlas: every unit texture packed into one big texture per player-
    // colour slot, so a whole model (and a whole crowd of one player) shares a
    // single texture and collapses to a handful of draw calls. Depth-sorted
    // multi-texture models otherwise force ~one draw call per triangle.
    std::unordered_map<std::string, SDL_Rect> atlasRect_;  // name -> content rect
    int atlasW_ = 0, atlasH_ = 0;
    std::vector<SDL_Texture*> atlasTex_;   // per colour slot; nullptr until built
    bool atlasLaidOut_ = false;
    std::set<std::string> animatedTex_;    // multi-frame glow textures (cycle over time)

    // Re-render the current frame of each animated glow texture (lodestone/mana/
    // sacred-fire crystal) into its rect in every built atlas, so the glow cycles
    // over time instead of showing a single frame baked at atlas-build time. `live`
    // (a built glow-unit is on screen) advances it; otherwise it holds frame 0 so a
    // still-conjuring lodestone doesn't glow until it's finished.
    void animateGlowTextures(bool live);

    // Per-type on-screen sprite box (offset from the draw anchor, px @ zoom 1), the
    // union over facings of the projected model bounds. Drives click-selection so a
    // click anywhere on the drawn unit (a tall building's roof, a body above its
    // feet) hits it -- computed lazily via unitHitBox(), cached here.
    std::map<std::string, SDL_FRect> hitBoxes_;
    // Per-type MODEL-space extents (world units) for the retail selection ring:
    // its radii come from the model's x/z half-extents and it floats at the model's
    // mid height, none of which survives unitHitBox's projection into screen space.
    struct RingBox { float halfX = 12.0f, halfZ = 12.0f, midY = 12.0f; };
    std::map<std::string, RingBox> ringBoxes_;
    // After a failed GPU texture allocation (VRAM pressure), pause every bake /
    // atlas creation path for a few seconds instead of retrying next frame. The
    // bake paths run per visible unit per frame, so an un-cached failure becomes
    // a driver-flooding allocation storm: observed on an 8GB card driving a
    // 7680x2160 desktop as ~6k/sec nvidia-drm NVKMS GEM allocation errors that
    // starved the compositor itself ("Failed to start frame" = whole-screen
    // flicker). Units render as full 3D models during the pause -- correct, just
    // less cheap -- and baking resumes automatically once the pause lapses.
    // Backoff now lives in the shared gpuvram accountant (so terrain chunks, which are
    // baked by MapView, participate too). These thin forwarders keep the call sites.
    bool gpuAllocBlocked() const { return gpuvram::blocked(); }
    void noteGpuAllocFail() { gpuvram::noteFail(); }

public:
    // SDL_RENDER_TARGETS_RESET / _DEVICE_RESET: on a driver or device reset, every
    // TEXTUREACCESS_TARGET texture silently loses its pixels while its handle stays
    // valid -- so units baked/skinned into them render fully transparent (they
    // "disappear"), the classic intermittent-vanish some GPUs show under load or on
    // a focus/resolution change. SDL fires this event exactly then; we drop every
    // render-target-backed cache so the pre-pass re-bakes them cleanly next frame.
    // Surface-backed caches (shadows, build FX) keep their pixels and are untouched.
    void invalidateRenderTargets();
    // Session-teardown release of EVERY GPU texture GameView owns (unit frames,
    // atlases, sprite pages, GUI/HUD art, icons, shadows, feature art, effects,
    // fog, fonts). See the dtor comment: the renderer outlives the session.
    void destroyGpuTextures();
private:
    int buildBarAlign_ = 1;     // conjure/build row: 0=left 1=center 2=right (Options)
    float buildBarScale_ = 1.0f;   // extra row scale on top of uiScale_ (Options)
    bool bilinear_ = false;     // smooth terrain/feature scaling (Options)
    int healthBars_ = 1;        // 0=off 1=damaged-only 2=always (Options)
    // Max NEW units the client registers (model/COB/anim VM + Create script) per frame,
    // so a mass simultaneous spawn streams in over ~a second instead of freezing one frame.
    static constexpr int kRegistrationsPerFrame = 64;

    // Append two triangles for an axis-aligned quad (uv is ignored when the batch
    // is drawn untextured).
    static void pushQuad(std::vector<SDL_Vertex>& b, float x, float y, float w,
                         float h, SDL_Color c) {
        SDL_Vertex tl{{x, y}, c, {0, 0}}, tr{{x + w, y}, c, {1, 0}},
                   br{{x + w, y + h}, c, {1, 1}}, bl{{x, y + h}, c, {0, 1}};
        b.push_back(tl); b.push_back(tr); b.push_back(br);
        b.push_back(tl); b.push_back(br); b.push_back(bl);
    }
    // Oriented segment: a quad of thickness `th` from (x0,y0) to (x1,y1). The
    // selection ring's dashes are chords at arbitrary angles, so pushQuad's
    // axis-aligned rect can't draw them.
    static void pushSeg(std::vector<SDL_Vertex>& b, float x0, float y0, float x1,
                        float y1, float th, SDL_Color c) {
        float dx = x1 - x0, dy = y1 - y0;
        float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) return;
        float nx = -dy / len * th * 0.5f, ny = dx / len * th * 0.5f;
        SDL_Vertex a{{x0 + nx, y0 + ny}, c, {0, 0}}, d{{x1 + nx, y1 + ny}, c, {1, 0}},
                   e{{x1 - nx, y1 - ny}, c, {1, 1}}, f{{x0 - nx, y0 - ny}, c, {0, 1}};
        b.push_back(a); b.push_back(d); b.push_back(e);
        b.push_back(a); b.push_back(e); b.push_back(f);
    }

    // Shelf-pack every loaded unit texture into a single atlas layout (rects are
    // shared across colour slots -- only the pixels differ). Called once, lazily.
    void buildAtlasLayout();

    // Build (or return cached) the atlas texture for one colour slot by blitting
    // each texture's slot variant into its packed rect. Main thread only (render
    // target), so it must run before the parallel geometry pass.
    SDL_Texture* atlasFor(int slot);


    // Project + transform one unit's model into screen-space, coloured vertex runs.
    // No SDL calls and only reads shared state (models/textures/heightmap/anim), so
    // it is safe to run for many units at once on the worker pool. drawUnit() then
    // just submits g.runs. `scratch` is a reusable per-thread triangle buffer.
    // A monarch (the five hero units) -- the only thing that disco-dances.
    static bool isMonarchType(const tak::sim::UnitType* t) {
        if (!t) return false;
        for (int i = 0; i < 5; ++i) if (t->id == tak::sim::kMonarchs[i]) return true;
        return false;
    }
    // Fully-saturated hue wheel -> RGB, hue in [0,1). Drives the disco tint & floor.
    static SDL_Color discoHue(float h) {
        h = h - std::floor(h);
        float r = std::fabs(h * 6.0f - 3.0f) - 1.0f;
        float g = 2.0f - std::fabs(h * 6.0f - 2.0f);
        float b = 2.0f - std::fabs(h * 6.0f - 4.0f);
        auto cl = [](float v) { return Uint8(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
        return SDL_Color{cl(r), cl(g), cl(b), 255};
    }
    // Is this unit currently disco-dancing (a monarch whose player hit Shift+D)?
    bool dancing(const UnitR& u) const;
    // ...or headbanging to heavy metal (a monarch whose player hit Shift+H)?
    bool headbanging(const UnitR& u) const;

    // The projected silhouette for one unit, into g.shadowVerts. Split out because
    // it is built alongside the body geometry on the worker pool.
    void buildUnitShadow(const UnitR& u, UnitGeom& g, const tak::tdo::Object& root,
                         const PieceMeta& meta, const Anim* anim, float facing, float zm,
                         std::vector<Tri>& scratch);
    void buildUnitGeom(const UnitR& u, UnitGeom& g, std::vector<Tri>& scratch);

    // Sprinkle the faction build/summon nano-sparkle over a screen footprint centred at
    // (cx,cy), fpw x fph screen px. Retail draws this over BOTH the worker unit and its
    // build/reclaim target; kBuildFxScale enlarges the little sprites so the effect reads,
    // and the cloud spreads a little past the footprint like the retail effect.
    void sprinkleBuildFx(const std::string& sideLower, float cx, float cy, float fpw, float fph);

    void drawUnit(const UnitR& u);

    // Project model triangles relative to the unit anchor: yaw by heading,
    // fixed tilt so models read against TAK's painted top-down terrain.
    // Light direction for projected shadows, as a per-unit-of-height offset in
    // model space. A vertex at height y lands on the ground at
    // (x + kShadowLX*y, 0, z + kShadowLZ*y), which is what makes the silhouette
    // lean away from the unit instead of sitting under it as a disc.
    //
    // Both are a quarter, straight out of the retail projection at 0x4ec250.
    // That routine projects model vertices in 16.16 fixed point and takes a flag
    // deciding which of two shears to apply:
    //
    //     model  (flag 0):  screen = ( x,         -z - y/2 )   <- our kProjY
    //     shadow (flag 1):  screen = ( x + y/4,   -z - y/4 )
    //
    // 0x4ede50 passes 1 immediately before handing the geometry to the shadow
    // rasteriser (0x4eda80, the one that bumps the "( %d shadow )" poly counter
    // at 0x62a800); the normal model path passes 0 at 0x4ecf21. Shifting screen
    // Y by -y/4 against a ground point that sits at -z is the same as pushing
    // the ground point +y/4 in z, so in world terms the light leans the
    // silhouette equally in +x and +z -- down-and-right, as it does in retail.
    static constexpr float kShadowLX = 0.25f;
    static constexpr float kShadowLZ = 0.25f;
    // How dark a shadowed pixel goes, as a multiply of the ground under it.
    // 0.55 * 255; measured off a retail screenshot, identical on all three
    // channels, so retail multiplies rather than blending toward a grey.
    static constexpr Uint8 kShadowLevel = 140;
    SDL_Texture* shadowMask_ = nullptr;       // coverage mask, composited once
    int shadowMaskW_ = 0, shadowMaskH_ = 0;
    SDL_FPoint shadowLo_{}, shadowHi_{};      // batch bounds, accumulated as built
    std::unordered_map<size_t, std::string> burnNames_;   // feature type -> name, copied under the lock

    // The model's extent under ROTATION, gathered during the same walk that builds
    // the geometry. Spinning a unit about Y sends each vertex (x,z) round a circle
    // of radius hypot(x,z) and leaves y alone, so these three numbers describe the
    // union of the unit's projected bounds over EVERY facing exactly -- see
    // unitHitBox, which used to sample sixteen of them and union the results.
    struct RadialExtent {
        float maxR = 0;               // largest hypot(x,z): the screen-x half-width
        float minY = 1e30f, maxY = -1e30f;   // screen y, over all headings
        bool any = false;
    };
    void collect(std::vector<Tri>& out, SDL_Texture* atlas, const tak::tdo::Object& o,
                 const Xform& parent, const Anim* anim, float heading, int player,
                 bool mirror = false, bool isRoot = true, bool shadow = false,
                 RadialExtent* ext = nullptr, const PieceMeta* meta = nullptr) {
        const tak::cob::PieceState* ps = pieceFor(anim, o.name);
        if (ps && !ps->visible) return;
        float rr[3];
        Xform xf = parent.then(o.x + (ps ? ps->move[0] : 0),
                               o.y + (ps ? ps->move[1] : 0),
                               o.z + (ps ? ps->move[2] : 0),
                               scriptRot(ps, rr));
        // Precomputed when the model was registered; computed here only for a model
        // that has no cached tree (ghost previews, build-icon portraits). Same
        // function either way.
        PieceMeta local;
        if (!meta) { pieceMetaFor(o, isRoot, local); meta = &local; }
        const bool groundPlate = meta->skip;
        float cy = std::cos(heading), sy = std::sin(heading);
        for (size_t pi = 0; pi < o.primitives.size(); ++pi) {
            if (groundPlate) break;
            const auto& p = o.primitives[pi];
            if (p.indices.size() < 3) continue;
            SDL_Texture* tex = nullptr;
            const SDL_Rect* arect = nullptr;
            const std::string& name = pi < meta->primTex.size() ? meta->primTex[pi]
                                                                : p.texture;
            if (!name.empty()) {
                auto rit = atlasRect_.find(name);
                if (atlas && rit != atlasRect_.end()) {
                    tex = atlas;             // whole model shares one atlas texture
                    arect = &rit->second;
                } else {
                    // Fallback for any texture not packed into the atlas.
                    auto it = textures_.find(name);
                    if (it != textures_.end() && !it->second.empty()) {
                        // Each texture carries one variant per player colour; a
                        // player's slot is remappable (--color / --aicolor).
                        size_t ci = size_t(colorSlot_[player & 7]);
                        tex = it->second[ci < it->second.size() ? ci : 0];
                    }
                }
            }
            // Transform each of this primitive's vertices ONCE. The fan below
            // visits vertex 0 in every triangle and each middle vertex twice, so
            // doing the transform inside the triangle loop did it 3*(n-2) times
            // for n vertices -- six transforms for a quad that needs four. The
            // UVs still come from the fan position (retail maps a quad's corners
            // to the four UV corners), so only the geometry is shared.
            struct VOut { SDL_FPoint pos; float px, py, pz, d; bool ok; };
            thread_local std::vector<VOut> vcache;
            vcache.resize(p.indices.size());
            for (size_t vi2 = 0; vi2 < p.indices.size(); ++vi2) {
                VOut& vo = vcache[vi2];
                size_t vi = size_t(p.indices[vi2]) * 3;
                if (vi + 2 >= o.vertices.size()) { vo.ok = false; continue; }
                vo.ok = true;
                float w[3];
                xf.apply(o.vertices[vi], o.vertices[vi + 1], o.vertices[vi + 2], w);
                const float wx = mirror ? -w[0] : w[0];   // un-mirror Zhon models on X
                const float rx = wx * cy + w[2] * sy;
                const float rz = -wx * sy + w[2] * cy;
                // TAK billboards lean back (+y and +z together); moving away (+z)
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
            for (size_t i = 1; i + 1 < p.indices.size(); ++i) {
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
                    // No cull: a silhouette is the union of both faces, and
                    // culling half of it punches holes in the shape.
                    // Opaque, because these go into a COVERAGE MASK that is
                    // composited once. Per-triangle alpha stacks wherever the
                    // silhouette folds over itself (a wing across a body) and
                    // retail's shadow has no such structure: sampling a retail
                    // shot gives 124/225, 106/192, 92/170 -- a flat multiply to
                    // 0.55 everywhere, which is what its alloca'd span buffer
                    // (0x4eda83, 0x5f08 bytes) buys. kShadowLevel is that 0.55.
                    for (int k = 0; k < 3; ++k) {
                        tri.v[k].color =
                            SDL_Color{kShadowLevel, kShadowLevel, kShadowLevel, 255};
                        tri.v[k].tex_coord = {0, 0};
                    }
                    tri.tex = nullptr;
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
                tri.depth = depth / 3;
                out.push_back(tri);
            }
        }
        for (size_t ci = 0; ci < o.children.size(); ++ci) {
            const PieceMeta* cm = (ci < meta->children.size()) ? &meta->children[ci] : nullptr;
            collect(out, atlas, o.children[ci], xf, anim, heading, player, mirror, false,
                    shadow, ext, cm);
        }
    }

    // Walk the piece tree (exactly as collect(), but transform-only) to the named
    // piece and return its model-space origin M = the composed translation. Used to
    // place effects at a weapon's emit piece (QueryWeapon) or a unit's SweetSpot.
    bool pieceModelOrigin(const tak::tdo::Object& o, const Anim* anim,
                          const Xform& parent, const std::string& want, float out[3]) const {
        const tak::cob::PieceState* ps = pieceFor(anim, o.name);
        float rr[3];
        Xform xf = parent.then(o.x + (ps ? ps->move[0] : 0),
                               o.y + (ps ? ps->move[1] : 0),
                               o.z + (ps ? ps->move[2] : 0),
                               scriptRot(ps, rr));
        std::string on = o.name;
        std::transform(on.begin(), on.end(), on.begin(), ::tolower);
        if (on == want) { out[0] = xf.t[0]; out[1] = xf.t[1]; out[2] = xf.t[2]; return true; }
        for (const auto& c : o.children)
            if (pieceModelOrigin(c, anim, xf, want, out)) return true;
        return false;
    }

    // Resolve a model piece to a WORLD effect position (x,z,alt) for THIS unit, matching
    // the model's on-screen projection: a piece's model origin projects to screen offset
    // (rx,-ry) under facing=-heading + the projection (see collect()), which corresponds to placing
    // an effect at (u.x+rx, u.z, altitude+ry). Returns false if the model/piece is absent
    // (caller falls back to the unit centre). pieceName must be lowercase.
    bool pieceWorldFx(const UnitR& u, const Anim& a, const std::string& pieceName,
                      float& outX, float& outZ, float& outAlt) {
        auto vt = visuals_.find(u.type ? u.type->id : std::string());
        if (vt == visuals_.end() || !u.type) return false;
        float m[3];
        if (!pieceModelOrigin(vt->second.model.root, &a, Xform{}, pieceName, m)) return false;
        float facing = (u.type->canMove || u.type->canFly) ? -u.heading : 0.0f;
        float cy = std::cos(facing), sy = std::sin(facing);
        float rx = m[0] * cy + m[2] * sy;
        float rz = -m[0] * sy + m[2] * cy;
        // Fold the unit's flight altitude into the SAME tilt scaling the renderer
        // uses: collect() puts altitude in base.t[1], so the model lifts a piece by
        // (localY+altitude)*cos(tilt). Adding raw altitude at x1.0 here would float
        // the effect ~0.25*altitude above the drake's actual on-screen mouth/body.
        float ry = (m[1] + unitAltById(u.id)) * kProjY + rz * kProjZ;
        outX = u.x + rx;
        outZ = u.z;
        outAlt = ry;
        return true;
    }

    void drawRing(float wx, float wz, float r);

    SDL_Renderer* ren_;
    // vfs_ is declared BEFORE mapView_ so its Compositor can borrow it; it is owned
    // here (not a reference) so a multiplayer client can REMOUNT to the room's
    // override tier at game start -- move-assigning vfs_ keeps every borrowed
    // pointer (MapView's Compositor) valid because the object itself is reused.
    tak::hpi::Vfs vfs_;          // retail-root read-path (the only way we read files)
    MapView mapView_;
    std::string installRoot_;    // retail install dir (for remounting to a new tier)
    tak::hpi::OverridePolicy policy_ = tak::hpi::OverridePolicy::Full;
    std::string mapPath_;        // VFS path to the map .tnt (start positions, siblings)
    // VFS read helpers -- the engine's only game-file access.
    std::vector<uint8_t> vread(const std::string& p) const { return vfs_.read(p); }
    bool vhas(const std::string& p) const { return vfs_.has(p); }
    tak::tdf::Node vtdf(const std::string& p) const;
    // A map's sibling scenario file (map.tnt -> map.ota/.cob/.tdf/.txt/.crt).
    std::string mapSibling(const char* ext) const;
    // Remount the data set to a multiplayer room's override tier and rebuild the
    // gameplay registry, so this client's sim reads the same gameplay data the
    // referee does. Cosmetics already loaded stay (harmless local display).
    void remountPolicy(uint8_t p);
    // Fingerprint of the gameplay data THIS client will feed its sim (current tier).
    uint64_t gameDataHash() const { return tak::hpi::gameplayHash(vfs_); }
public:
    uint8_t overridePolicy() const { return uint8_t(policy_); }
private:
    bool crusades_ = false; // which balance registry_ currently holds
    std::string side_ = "ara";
    // Retail loading screen. Alive from the start of world setup until the first
    // tick lands, so the plate covers both our own load and the wait on peers.
    std::unique_ptr<tak::LoadScreen> loadScreen_;
    std::string aiSide_ = "tar";   // single-player: the AI opponent's faction
    // Faction name -> wire index (0 ara, 1 tar, 2 ver, 3 zon, 4 cre).
    static uint8_t facIdx(const std::string& s) {
        const char* n[5] = {"ara", "tar", "ver", "zon", "cre"};
        for (uint8_t i = 0; i < 5; ++i) if (s == n[i]) return i;
        return 0;
    }
    tak::sim::TypeRegistry registry_;
    tak::sim::World world_;
    // Hash maps (not std::map): these are looked up per unit per frame in the
    // serial anim loop and the parallel projection, and tree traversals were a
    // measurable slice of the update cost at thousands of units.
    std::unordered_map<std::string, Visual> visuals_;
    std::unordered_map<int, std::string> unitType_;
    std::unordered_map<int, Anim> anims_;
    // Summon fade-in: unit id -> age (s) since it was conjured from a building. Purely
    // cosmetic and viewer-only (driven by the sim's non-hashed justBuilt hook); it
    // fades the unit in and sprinkles the faction shimmer for kBirthFxDur. Updated on
    // the main thread each frame, then read-only during the parallel geometry build.
    std::unordered_map<int, float> birthFx_;
    static constexpr float kBirthFxDur = 0.9f;
    float birthProgress(int id) const;
    std::map<std::string, std::vector<SDL_Texture*>> textures_;
    std::vector<Tri> tris_;
    std::vector<SDL_Vertex> triBatch_;   // reused per-unit vertex batch
    std::vector<UnitGeom> geomPool_;              // reused across frames (keeps capacity)
    // unit id -> slot in geomPool_, rebuilt each frame. A flat vector (ids are dense:
    // id == index+1) instead of an unordered_map, so no per-visible-unit node alloc
    // and the three later passes index in O(1) instead of hashing. -1 = not in view.
    std::vector<int> geomIndex_;
    int geomSlot(int id) const;
    std::vector<int> selection_;
    std::unordered_set<int> selSet_;   // rebuilt each draw for O(1) membership
    bool dragging_ = false;
    bool buildDrag_ = false;          // shift-drag placing a line of buildings
    float bdX0_ = 0, bdZ0_ = 0;       // build-drag start (world)
    bool reclaimDrag_ = false;        // right-drag box: a builder clears the area
    float rdX0_ = 0, rdZ0_ = 0;       // reclaim-drag start (world)
    float rdSx0_ = 0, rdSy0_ = 0;     // reclaim-drag start (screen; click-vs-drag test)
    bool draggingMinimap_ = false;
    float dragX0_ = 0, dragY0_ = 0, dragX1_ = 0, dragY1_ = 0;
    char pendingCmd_ = 0;   // armed order awaiting a click: 'f' fight-move,
                            // 'm' move, 'a' attack, 'p' patrol, 'g' guard
    bool paused_ = false;
    bool exitMenu_ = false;          // in-game exit overlay (Esc) is open
    bool canReturnToMenu_ = false;   // launched from the front-end -> offer MAIN MENU
    std::vector<std::pair<SDL_FRect, std::function<void()>>> exitHots_;   // overlay hit-rects (screen space)
    // ---- Options (local display/input; see viewer/settings.h) ----
    float edgeScrollSpeed_ = 1.0f;
    bool  edgeScrollOn_ = true;
    float uiScale_ = 1.0f;
    tak::Settings* settings_ = nullptr;               // main()'s settings (for the in-game Options)
    std::unique_ptr<tak::OptionsScreen> options_;     // in-game Options overlay
    std::unique_ptr<tak::HotkeysScreen> hotkeysScreen_;   // opened from Options -> CONTROLS
    tak::Hotkeys hotkeys_;                             // effective bindings (from settings)
    int gameSpeed_ = 0;         // -10..+10 game-speed level (+/- keys); 0 = normal
    // 10^(level/10): +10 = 10x, 0 = 1x, -10 = 0.1x.
    // Game-speed multiplier. Forced to 1x in a networked game: the peers advance
    // the sim in lockstep at a fixed step, so scaling one peer's dt would desync.
    float speedMult() const;
    bool showCounts_ = false;   // F4: per-faction live unit counts
    bool spectating_ = false;   // watching a live net game (no control, no fog)
    std::string playerName_[8];   // net games: display name per player (from lobby)
    bool playerAi_[8] = {};       // net games: which players are server-run AI
    bool showHDebug_ = false;   // terrain-height / lift diagnostic overlay (TAK_HDEBUG env)
    float fps_ = 0;             // smoothed render FPS, shown on the F4 overlay
    int winW_ = 0, winH_ = 0;   // last-known window size (for centering/culling)
    tak::net::MpClient* mp_ = nullptr;
    std::vector<tak::net::Command> outbox_;   // local orders to send to the server
    uint64_t mpListMs_ = 0, mpFirstListMs_ = 0;   // auto-join: ListGames timing
    uint64_t mpSlowSinceMs_ = 0;                  // when the replay backlog went deep
    // Client-side jitter/receive buffer (ON by default; TAK_NET_DELAY overrides:
    // 0 = off, K = fixed depth, auto/unset = self-sizing). The sim is paced on the
    // wall clock at 30 Hz and kept ~netDelay_ bundles behind the newest received,
    // so brief server->client jitter is covered from the reserve instead of
    // stalling. Costs ~netDelay_*33ms of input latency (auto keeps that minimal).
    int netDelay_ = -2;          // -2 = read TAK_NET_DELAY once; then 0 = off, else depth
    bool netAuto_ = false;       // auto (default): size the buffer to the link
    bool netBufReady_ = false;   // built the initial reserve
    float netAccum_ = 0;         // wall-clock tick accumulator (seconds)
    uint64_t netStepMs_ = 0;     // last mpStep wall time
    long netBenchFrames_ = 0, netBenchStalls_ = 0;   // jitter-buffer stall metric
public:
    long netBenchFrames() const { return netBenchFrames_; }
    long netBenchStalls() const { return netBenchStalls_; }
    void netEnableRttProbe() { if (mp_) mp_->enableRttProbe(); }
    float netRttMs() const { return mp_ ? mp_->rttMs() : 0.0f; }
    int netDelay() const { return netDelay_; }
private:
    bool replayMode_ = false;                     // playing a recorded .takrep
    std::vector<tak::net::Bundle> replayBundles_;
    size_t replayTick_ = 0;
    float replayAccum_ = 0;
    bool mpReadied_ = false, mpStarted_ = false, mpSetupDone_ = false;
    // interactive lobby UI state
    enum class LobbyScreen { Browser, Create } lobbyScreen_ = LobbyScreen::Browser;
    bool menuRequested_ = false;   // set by a MAIN MENU action -> main() returns to the front-end
    bool quitRequested_ = false;   // set by the in-game QUIT button -> main() exits the app
    bool singlePlayer_ = false;    // menu single-player: private local game (SP-flavoured lobby)
    bool spSpectate_ = false;      // SP: watch the AIs (host takes no slot)
    bool specAutoSeated_ = false;  // SP spectate: one-shot -- fill every open slot with a random-race AI
    bool externalLobbyMusic_ = false;   // front-end owns the lobby BGM -> suppress ours
    int lbField_ = 0;   // active text field: 1=createName 2=createPass 3=joinPass 4=chat
    float lobbyScale_ = 2.0f;               // lobby fit scale (set in render)
    float lobbyOffX_ = 0, lobbyOffY_ = 0;   // lobby centre offset (logical units; set in render)
    std::string createName_ = "game", createPass_, joinPass_, chatDraft_;
    bool createCrusades_ = false, createGods_ = false;
    // Fog of war, chosen at CREATE time (0 = not explored, 1 = explored,
    // 2 = full vision). It is a room setting like the rest, so it belongs where
    // the room is set up -- the host could previously only change it after the
    // game already existed. Default matches GameOptions::fogExplored.
    uint8_t createFog_ = 1;
    bool createMonarchExp_ = false;   // create dialog: Monarch Expendable (default OFF = monarch matters)
    bool createStressTest_ = false;   // SP spectate: spawn ~95% of each AI's unit cap at start
    // One selectable map plus the attributes the picker can sort by, read once from
    // the map's .ota GlobalHeader (a tiny text file -- no need to decompress the TNT).
    struct MapInfo {
        std::string name, path;
        int players = 0;            // numplayers (else the StartPos count)
        int sizeW = 0, sizeH = 0;   // "size = W x H" (0 if the .ota omits it)
        int area() const { return sizeW * sizeH; }
    };
    std::vector<MapInfo> mapList_;      // cached, sorted per mapSort_
    int mapSort_ = 0;                  // 0 = name, 1 = players, 2 = size
    int mapSortDir_ = 1;              // 1 = ascending, -1 = descending
    // Create-screen map picker: a scrollable list box. mapScroll_ is the index of the
    // first visible row; the rest is geometry cached each frame for wheel + scrollbar
    // drag handling in lobbyInput (all in panel-local logical coords).
    int mapScroll_ = 0;
    bool mapPrefApplied_ = false;      // adopted settings_->lastMap once this session
    bool mapDrag_ = false;             // dragging the scrollbar thumb
    // Random-map generator ("Generate Random Map" in the picker): the current params,
    // which slider is being dragged (0=doodad 1=mana 2=water, -1=none), and the three
    // slider bar rects for drag hit-testing.
    tak::mapgen::Params genParams_{};
    int genSlider_ = -1;
    SDL_FRect genSliderRect_[5]{};
    void applyGenParams();             // re-encode genParams_ -> mpMapId_
    void setGenSlider(int i, float mx);  // drag a density slider from a panel-x
    SDL_FRect mapListRect_{};          // the list box (rows area) -- wheel target
    SDL_FRect mapThumbRect_{};         // the scrollbar thumb -- drag grab
    int mapVisRows_ = 0, mapTotalRows_ = 0;   // for clamping + thumb drag math
    // Selected-map preview (the .tnt's embedded minimap), rebuilt on selection.
    SDL_Texture* mapPreviewTex_ = nullptr;
    std::string mapPreviewFor_;        // mapPath_ the current preview was built for
    int mapPreviewW_ = 0, mapPreviewH_ = 0;
    std::string mapPreviewDims_;       // "W x H" cell size, shown under the preview
    std::map<std::string, std::vector<uint8_t>> kingdomPals_;  // kingdom -> RGBA palette (256*4)
    uint8_t createOverride_ = 1;   // create-dialog override tier (default cosmetic)
    std::string mpMapId_;   // set from the launched map basename
    std::string missionStem_;   // campaign mission to host (headless --mpmission)
    // Per-mission unit whitelist from missions/<stem>.tdf (lowercased ids). When set,
    // the human's conjure menu is filtered to it (a UI restriction; empty = anything).
    std::vector<std::string> missionAllowed_;
    std::vector<std::string> missionObjectives_;   // in-game objectives panel lines
    bool showObjectives_ = true;                   // panel visible (toggle with O)
    std::string mpResumePath_;   // where the resume ticket is saved (for reconnect)
    std::vector<std::pair<std::string, std::string>> chatLog_;
    // In-game chat: press Enter to compose, lines fade after a while. Kept apart
    // from the lobby chatLog_ so the in-game overlay and lobby panel don't mix.
    bool chatTyping_ = false;
    struct GameChat { std::string who, text; float age = 0; };
    std::vector<GameChat> gameChat_;
    uint64_t chatLastMs_ = 0;
    std::vector<std::pair<SDL_FRect, std::function<void()>>> lobbyHots_;
    int localPlayer_ = 0;
    // Player-colour slot per player (which colour variant of each unit texture to
    // use); defaults to the player index. Overridable via --color / --aicolor and
    // the in-game picker.
    int colorSlot_[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    // The 10 player-colour RGBs (sampled from a player-coloured logo texture at
    // load; the fallback below is a sensible distinct palette). Used so the HUD
    // and minimap match whatever slot a player renders in.
    SDL_Color playerColors_[10] = {
        {70, 130, 240, 255}, {215, 60, 55, 255}, {70, 185, 90, 255},
        {235, 205, 55, 255}, {225, 225, 225, 255}, {80, 200, 205, 255},
        {160, 95, 205, 255}, {230, 145, 50, 255}, {230, 120, 180, 255},
        {120, 120, 130, 255}};
    bool sampledColors_ = false;
    // RGB a player's units render in (its slot's player colour). The argument is
    // the sim ownership index (Unit::player) -- one player per slot in skirmish.
    SDL_Color playerColor(int player) const;

    // Height-aware 2.5D: the world-pixel lift for a point, from the terrain height
    // under it, so a unit (and its shadow/health bar/selection/effects) sits on
    // the elevation the tile art shows instead of the flat grid cell. Zero at the
    // map's ground level; water and flat ground are unaffected.
    int heightRef_ = -1;                          // map ground level (modal height)
    // Screen-Y lift per terrain height unit (also used by occlusion + picking).
    // RETAIL'S VALUE IS 0.5, and it is not a guess: 0x511140 returns byte +4 of
    // a cell record -- the terrain height -- and its caller at 0x426820 forms
    //     screenY = (z << 4) - cameraY - (height >> 1)
    //     screenX = (x << 4) - cameraX              <- no height term at all
    // Confirmed by driving both under emulation (tools/re/emulift.py). We used
    // 1.1 here, which is 2.2x too much: at height 120 that is 132px of lift
    // against retail's 60, so a unit on raised ground was drawn most of four
    // tiles too far up-screen and appeared to stand on terrain beside it.
    float kHeightScale_ = 0.5f;
    // Screen-X lift per height unit. Zero, and retail agrees: its screen X is
    // (x << 4) - cameraX with no height term (0x426820). Tunable via TAK_HSCALEX
    // for experiments only.
    float kHeightScaleX_ = 0.0f;
    int kOccScan_ = 12;                            // cells to scan south for a wall
    // Height above ground at a world point, bilinearly sampled so lifts ramp
    // smoothly across a slope. Lazily initialises the modal ground reference and
    // reads the tunable scales / debug flag from the environment.
    float heightAbove(float wx, float wz);
    float rawHeight(float wx, float wz);   // unclamped bilinear height (waterline sink)
    // Screen-Y sink for a wading (canhover) or floating (floater) unit standing in
    // water, from FBI `waterline`. Zero on land and for every other unit.
    float waterSink(const tak::sim::UnitType* t, float wx, float wz);
    const void* hMemoMap_ = nullptr;   // heightAbove 1-entry memo (see above)
    float hMemoX_ = 0, hMemoZ_ = 0, hMemoV_ = 0;
    // Screen-space displacement of a world point's surface from its flat grid cell,
    // baked into the tile art by the tilted 2.5D view: up (Y) AND sideways (X).
    float terrainLift(float wx, float wz) { return heightAbove(wx, wz) * kHeightScale_; }
    // ---- the datum a FLYER holds its altitude above --------------------------
    // Retail does not fly over the per-pixel heightmap. It precomputes, at map
    // load, one byte per 128x128-world-unit sector holding the MAXIMUM terrain
    // height in that sector (floored at sea level), then max-DILATES that grid
    // 3x3 -- so the value a flyer reads is the highest ground within a 384x384
    // window (icd 0x50e740; grid at world+0x19f18, sector index (x>>23,z>>23)).
    // Its navigator then aims at that value + cruisealt (0x4e42aa / 0x524b89) and
    // a servo walks the unit's Y toward it a step per tick rather than snapping.
    //
    // The visible difference is the whole point: over the real heightmap a flyer
    // twitches with every bump it crosses, which is what "the altitude visibly
    // changes with terrain height" was. Over a dilated maximum it climbs once,
    // smoothly, to clear the highest thing anywhere near it, and stays there.
    float flyerGround(float wx, float wz);
    void buildFlyerGround();
    std::vector<uint8_t> flyGround_;         // 3x3-dilated per-sector max height
    int flyGroundW_ = 0, flyGroundH_ = 0;    // sectors
    const void* flyGroundMap_ = nullptr;     // which map it was built for
    float terrainLiftX(float wx, float wz) { return heightAbove(wx, wz) * kHeightScaleX_; }
    // The terrain-relief lift is for MOBILE units standing on painted slopes. A
    // A "structure" (building) for render/build purposes = one that can't actually
    // move. NOTE: the FBI `canmove` flag is unreliable -- some buildings (the Keep,
    // arakeep) set canmove=1 with NO velocity -- so key off maxVel, not type->canMove.
    static bool isStructure(const tak::sim::UnitType* t) { return !t || t->maxVel <= 0.0f; }
    // Retail's shadow rule, and ONLY retail's: it skips the whole shadow block for
    // FBI `noshadow` (KINGDOMS.icd 0x4ec8d8) and, independently, for every `floater`
    // (0x4ecac0) -- which is why five ships carry a shadowart they never show.
    // Buildings are NOT excluded here: two of them (npcflag, vermort) declare a
    // shadow sprite and retail draws it.
    static bool castsShadow(const tak::sim::UnitType* t) {
        // Those two are the WHOLE test. The Glide shadow block reads the type and
        // bails on exactly two bits of UnitDef+0x260: 0x2000000 (noshadow, the
        // guard at 0x4ec8d8) and 0x80000 (floater, 0x4ecac6). There is no canfly
        // bit test and no building test anywhere in it, so a flyer and a keep
        // both cast.
        //
        // We DID exclude canfly once, on the strength of the drawable-build code
        // at 0x4ee340, which installs shadow art only when noshadow and canfly
        // are both clear. That reading was sound but it describes the SOFTWARE
        // renderer's sprite shadow, a path the Glide renderer never takes for
        // units -- which is why 24 flying types carry a `shadowgaf` that looked
        // like dead data and is not.
        return t && !t->noShadow && !t->floater;
    }
    // EVERY unit that casts at all casts a projected silhouette -- there is no
    // second kind of shadow in the Glide renderer we target. The shadow is emitted
    // by the model draw itself (0x4ee700 -> 0x4ec720 -> 0x4ec7b0 -> 0x4ede50),
    // which runs the same geometry through the shear at 0x4ec250 and hands it to
    // a rasteriser that bumps the "( %d shadow )" poly counter. Nothing in that
    // chain looks at `shadowart`.
    //
    // The FBI shadow sprites are the SOFTWARE renderer's cheaper answer, blitted
    // from a subsystem (0x4ee310, reached only from 0x511d00) that never calls the
    // model draw. So in Glide the 17 sequences in shadows.gaf go unused for units
    // and the 104 types declaring `shadowart` get silhouettes like everyone else.
    static bool castsBlobShadow(const tak::sim::UnitType* t) { return castsShadow(t); }
    // EVERYTHING on the map lifts onto the terrain relief by the same rule -- mobile
    // units, buildings, AND the feature decals (mana deposits, trees) -- so a mana
    // deposit sits at the height its heightmap claims and a lodestone/units built on
    // it stack right on top instead of the decal being flat while units float above.
    float uLiftY(const tak::sim::Unit& u) { return terrainLift(u.x, u.z); }
    float uLiftX(const tak::sim::Unit& u) { return terrainLiftX(u.x, u.z); }
    // Snapshot overloads. A FLYER rides the coarse dilated datum, not the relief
    // under its nose -- see flyerGround.
    float uLiftY(const UnitR& u) {
        if (u.type && u.type->canFly) return flyerDatum(u) * kHeightScale_;
        return terrainLift(u.x, u.z);
    }
    float uLiftX(const UnitR& u) {
        if (u.type && u.type->canFly) return flyerDatum(u) * kHeightScaleX_;
        return terrainLiftX(u.x, u.z);
    }
    // Screen-space height a flying unit is lifted by its own altitude. ONE
    // definition, because there used to be several that disagreed: the body applied
    // altitude as a model-space translation while unitScreen (which drives picking
    // and the marquee) used alt*0.8 on top of the per-pixel terrain lift rather
    // than the flyer datum -- so a flyer was drawn in one place and clicked in
    // another.
    float altLift(const UnitR& u) {
        if (!u.type || !u.type->canFly) return 0.0f;
        auto it = anims_.find(u.id);
        float alt = it != anims_.end() ? it->second.altitude : u.type->cruiseAlt;
        // kProjY, because that is what the MODEL projection does with it: the
        // renderer lifts a piece by (localY + altitude) * kProjY (the effect
        // anchor uses the same factor). unitScreen had a hand-tuned 0.8 instead,
        // so picking and the drawn unit disagreed by ~6% of the altitude even
        // before the datum change.
        return alt * kProjY;
    }
    // The SMOOTHED datum for this flyer (Anim::groundY), falling back to the raw
    // sector value for a unit with no live anim yet.
    float flyerDatum(const UnitR& u) {
        auto it = anims_.find(u.id);
        if (it != anims_.end() && it->second.groundInit) return it->second.groundY;
        return flyerGround(u.x, u.z);
    }

    // A unit's current render altitude (flyers rise to cruiseAlt; 0 for ground
    // units or units with no live anim). Used to lift a flyer's projectiles/effects
    // so they leave/strike at the altitude the unit is drawn, not the ground.
    float unitAltById(int id) const;
    // Render altitude of an airborne unit at ~this world point, else 0. Used to lift
    // an impact blast onto a flyer (the hit record only carries the impact point).
    float flyerAltAt(float x, float z) const;

    // Screen position of a unit's drawn body centre, matching the render lift:
    // terrain relief (uLift*) plus, for a flyer, its cruise altitude (the sprite is
    // raised by alt*0.8*zoom, same factor as drawUnit). Used for height-correct
    // marquee/click selection so a lifted or airborne unit is picked where it's SEEN,
    // not at its flat ground cell.
    SDL_FPoint unitScreen(const UnitR& u);

    // Per-type on-screen sprite box (offset from the draw anchor, px @ zoom 1),
    // computed once by projecting the model over all facings and cached in hitBoxes_.
    const SDL_FRect& unitHitBox(const tak::sim::UnitType* type);

    // Per-type model-space extents driving the selection ring, cached in ringBoxes_.
    const RingBox& unitRingBox(const tak::sim::UnitType* type);
    // Half-extent of the model's GROUND PLATE -- the flat untextured quad at
    // y = 0 that every TAK unit's root carries (AraGP and friends). It is the
    // unit's shadow: sized per unit (17.6 for a Monarch, 14.4 for a swordsman),
    // which is why a Monarch has a shadow in retail despite declaring no
    // `shadowart` in its FBI. Zero when the model has no such piece.
    struct PlateBox { float halfX = 0, halfZ = 0; };
    const PlateBox& unitPlateBox(const tak::sim::UnitType* type);
    std::map<std::string, PlateBox> plateBoxes_;

    // Height-aware picking: invert the render lift so a click on elevated terrain
    // (a wall/plateau top, drawn lifted UP on screen) resolves to the cell whose
    // *lifted* position is under the cursor, not the flat cell the raw screen->world
    // map would give (which lands on the low ground behind the wall). Returns the
    // FRONT-MOST surface (largest z) that projects to the click, like a depth pick.
    void pickWorld(float sx, float sy, float& wx, float& wz);

    // Terrain occlusion: a wall's baked-relief art projects up-and-north over the
    // flat ground behind it, but terrain is painted before units, so a unit on
    // that ground would draw ON the wall. Find the screen-Y of the projected top
    // surface of the tallest wall BETWEEN this unit and the camera (i.e. to its
    // south, larger z). Units are then clipped to above that line and the hidden
    // part re-drawn as a faint silhouette. Returns a huge value when nothing
    // occludes the unit. kHeightScale_/heightRef_ are lazily set by terrainLift.
    float wallOcclusionY(float wx, float wz);
    uint32_t netTick_ = 0;
    uint32_t lastSendTick_ = 0;     // tick the send credit was last accrued for
    int cmdCredit_ = tak::net::kCmdCapPerTick;   // rate: commands we may send now
    int cmdInFlight_ = 0;   // window: sent, not yet seen back in a bundle
    bool cmdCatchUp_ = false;   // replaying a rejoin's bundle log: hold orders, ignore acks
    uint32_t cmdReplayEnd_ = 0;  // server-declared tick where that history ends
    std::string netError_;
    // --- Sim/render decouple: the sim worker thread (Stage B1c) ----------------------
    // The worker runs the heavy sim tick (apply commands/events + world_.tick + captureFrame)
    // off the render thread. The main thread owns the network (mp_): it drains delivered
    // bundles and hands them to the worker via simInbox_ (FIFO == lockstep tick order), and
    // sends the worker's per-tick state hash back to the server from simOutbox_. world_ is
    // mutated ONLY by the worker (under simMutex_); the render reads the published snapshot,
    // and its remaining live-world_ read (canPlace) takes simMutex_. Off for headless/replay
    // (inline path) unless wantSimThread_ (TAK_SIM_THREAD) forces it on for verification.
    struct SimJob { tak::net::Bundle bundle; uint32_t tick = 0; bool wantHash = false; bool spectator = false; };
    struct HashJob { uint32_t tick = 0; uint64_t hash = 0; };
    std::thread::id mainThreadId_ = std::this_thread::get_id();   // set at construction (main thread)
    std::thread simThread_;
    std::mutex simMutex_;               // guards world_ mutation (worker) vs live reads (canPlace)
    std::mutex inboxMutex_;
    std::condition_variable inboxCv_;
    std::deque<SimJob> simInbox_;       // main -> worker: bundles to simulate, in tick order
    std::mutex outboxMutex_;
    std::deque<HashJob> simOutbox_;     // worker -> main: {tick, hash} to send to the server
    std::atomic<bool> simQuit_{false};
    // Quit WITHOUT draining: abandon whatever ticks are still queued instead of
    // simulating them all first. Leaving a game mid-replay-catchup can otherwise
    // queue thousands of ticks, and the join waits for every one of them.
    std::atomic<bool> simCancel_{false};
    std::atomic<uint32_t> simProcessedTick_{0};   // last tick the worker finished (backlog/ack)
    bool useSimThread_ = false;         // true while the worker is running for this game
    bool wantSimThread_ = false;        // decided once per game (see mpStep)
    bool simThreadDecided_ = false;
    bool simThreadMode_ = true;         // interactive default ON; the headless harness opts out
    bool benchmarkMode_ = false;        // menu Benchmark: all-AI watch run + staged spawn plan
    int benchmarkLevel_ = 0;            // benchmark intensity 1..5 (for the plan + results label)
    // Benchmark metrics: one sample per 5s milestone (the 7 spawn stages + the 40s end).
    struct BenchSample {
        int gameSec = 0, liveUnits = 0;
        double clientCpuPct = 0, serverCpuPct = 0;   // % of one core over the 5s interval
        size_t clientRss = 0, serverRss = 0;         // bytes
        size_t gpuBytes = 0;                         // tracked client texture VRAM (gpuvram)
        double gpuPct = -1;                          // whole-GPU utilization %, -1 = unknown
        size_t gpuSysUsed = 0;                       // whole-GPU VRAM used (all processes), bytes
        float fps = 0, simSpeed = 0;
    };
    std::string benchGpuName_;                       // GPU adapter name (captured once)
    size_t benchGpuTotal_ = 0;                       // total GPU VRAM, bytes
    std::vector<BenchSample> benchSamples_;
    tak::proc::Sample benchCliPrev_, benchSrvPrev_;
    uint64_t benchPrevWallMs_ = 0;
    uint32_t benchNextTick_ = 300;      // next milestone tick (300,600,...,1800)
    long benchServerPid_ = 0;           // local takserver pid (0 = N/A, e.g. headless)
    bool benchStatsShown_ = false;      // the benchmark stats overlay is up
    SDL_FRect benchDoneRect_{};          // the stats overlay's DONE button (set each render)
    int benchCamLeg_ = -1;              // benchmark flythrough: leg (faction) the camera is on
    float benchLegT_ = 0.0f;            // seconds into the current leg (drives the zoom-in)
    // The worker: pop bundles FIFO, simulate under simMutex_, hand back the state hash.
    void simWorkerLoop();
    void startSimThread();
    void stopSimThread(bool drain);
    // canPlace reads live world_ (units_, which the worker resizes on spawn/death, plus the
    // nav grid), so it can't run lock-free while the worker ticks. Take simMutex_ for the
    // read. Placement-UX only and rare (a ghost while positioning a building), so the brief
    // wait for the current tick is invisible; uncontended and cheap when inline.
    bool canPlaceLocked(const tak::sim::UnitType* type, float x, float z);
    // A site blocked ONLY by clearable features: collect those features' ids, so the
    // placement can queue reclaims ahead of the build instead of being refused.
    // Returns false when anything else blocks (terrain, a unit, a building, an
    // indestructible feature) -- those are refusals retail makes too and we keep.
    bool clearableAt(const tak::sim::UnitType* type, float x, float z,
                     std::vector<int>& outFeatures);
    // Queue reclaims for `feats`, then the build. Pure client macro: it emits only
    // the existing Reclaim and Build commands, so the SIM is untouched and stays
    // byte-for-byte what retail does.
    void issueClearThenBuild(int builderId, const tak::sim::UnitType* type,
                             float x, float z, const std::vector<int>& feats, bool queue);
    // HUD notice setter that is safe to call from the sim worker: the worker's sim events
    // (god summon, scenario/mission messages) defer into a pending slot that the main thread
    // applies in mpStep, so notice_ (a std::string draw() reads every frame) and noticeTimer_
    // are only ever touched on the main thread. Direct on the main thread.
    std::mutex noticeMutex_;
    std::string pendingNotice_;
    float pendingNoticeTimer_ = 0;
    bool pendingNoticeSet_ = false;
    void postNotice(std::string msg, float t);
    void drainPendingNotice();
    // Actual-vs-requested game-speed meter (F4): measured from our own tick advance.
    uint64_t actualSpeedT0_ = 0, actualSpeedTick0_ = 0;
    float actualSpeed_ = 0.0f;
    uint64_t lastSpecAckMs_ = 0;   // spectator flow-control heartbeat (see mpStep)
    uint64_t gameStartMs_ = 0;     // wall time the game/spectate began (F4 real-time elapsed)
    bool follow_ = false;
    bool trackSel_ = false;   // T: keep the camera centred on the selection
    bool trace_ = false;
public:
    bool noFog_ = false;
    // A campaign mission's own fog rules (.ota lineofsight / mapping), which
    // override the room's setting for that mission.
    bool missionFullVision_ = false, missionPreMapped_ = false;
    uint32_t shakeSeqSeen_ = 0;   // last mission ScreenShake sequence acted on
    uint32_t soundSeqSeen_ = 0;   // last mission PLAY_SOUND sequence acted on
private:
    int keepId_ = -1, aiKeepId_ = -1, builderId_ = -1;
    int playerMonarchId_ = -1, aiMonarchId_ = -1;
    const tak::sim::UnitType* placing_ = nullptr;
    float mouseX_ = -1, mouseY_ = -1;   // -1 until the first real mouse motion, so
                                        // edge-scroll can't fire from a (0,0) default
                                        // cursor on launch (before the mouse moves)
    // Retail animated mouse cursors (anims/cursors.gaf). Lazily loaded on the first
    // overlay draw; when it takes over, the OS arrow is hidden (restored in the dtor).
    tak::CursorSet cursors_;
    bool cursorsInit_ = false;          // attempted the one-time load yet?
    int  cursorMode_ = -1;              // -1 uninit, 0 software (drawn), 1 hardware (OS-tracked)
    bool hwCursorFailed_ = false;       // hardware cursor rejected once -> stay on software

    // Render-side motion interpolation: glide units between 30Hz sim ticks (see
    // captureFrame / interpPose). Viewer-only, never hashed.
    // TRIPLE-buffered render snapshot for the sim/render decouple. The writer (the sim
    // worker in threaded mode, or captureFrame inline) fills a spare buffer and publishes
    // it (published_); the render pins the newest published buffer for a whole frame
    // (beginFrame -> reading_) so a concurrent publish never tears its reads. The writer
    // always picks the third buffer -- never published_, never reading_ -- so with three
    // buffers there is always a free one even if a render frame spans two ticks. Two
    // buffers would tear in exactly that case. front() returns the render's pinned buffer.
    Frame frameBuf_[3];
    std::mutex frameMutex_;             // guards published_/reading_ (brief index swaps only)
    int published_ = 0;                 // newest fully-written buffer (writer sets under lock)
    int reading_ = -1;                  // buffer the render pinned this frame, or -1
    int renderReadIdx_ = 0;             // render-thread's pinned buffer (mirrors reading_)
    uint32_t captureCounter_ = 0;       // monotonic; each Frame.gen gets a unique value
    const Frame& front() const { return frameBuf_[renderReadIdx_]; }
public:
    // Render thread: pin the newest published buffer for this frame's reads, then release.
    // Call beginFrame() before any front() read and endFrame() once all are done. Public
    // because the top-level render loop drives them around the whole per-frame gameView pass.
    void beginFrame();
    void endFrame();
private:
    float interpAlpha_ = 0.0f;          // 0..1 through the current tick interval (per render frame)
    uint32_t lastCosmeticGen_ = 0;      // front().gen last processed by cosmeticStep's per-tick pass
    // Fight-move ('f') reuses the Attack glyph tinted this red-orange, so it reads apart
    // from a real attack order -- for both the order-column button and the mouse cursor.
    static constexpr SDL_Color kFightMoveTint{255, 90, 80, 255};
    SDL_Texture* fogTex_ = nullptr;
    SDL_Texture* miniTex_ = nullptr;
    // Async minimap crunch (see buildMinimap): thread + handoff buffer.
    std::thread miniThread_;
    std::mutex miniMu_;
    std::vector<uint8_t> miniPix_;
    bool miniBuilding_ = false, miniReady_ = false;

    // Screenshot path only: block until terrain chunks and the minimap are done so
    // a capture never shows half-composited scenery. Normal play never waits.
public:
    void finishTerrain();
private:
    // Join the minimap thread and drop its output -- required before the map it
    // reads is swapped (mapView_.reload) and at teardown.
    void resetMinimap();
    SDL_Texture* panelTex_ = nullptr;
    int panelW_ = 0, panelH_ = 0;
    SDL_Texture* botTex_ = nullptr;
    int botW_ = 0, botH_ = 0;
    // Retail GUI-driven HUD: the parsed .gui and, parallel to gui_.gadgets, the
    // loaded state-art textures for each gadget (imgs[0]=normal,1=hover,2=grayed).
    tak::gui::Gui gui_;
    std::vector<std::vector<SDL_Texture*>> guiTex_;
    std::map<std::string, SDL_Texture*> icons_;
    std::map<std::pair<std::string, int>, SDL_Texture*> modelIcons_;  // model-rendered fallback icons
    std::vector<std::pair<SDL_FRect, const tak::sim::UnitType*>> iconRects_;
    static constexpr int kMiniSizeBase = 180;
    int miniSize() const { return int(kMiniSizeBase * uiScale_); }   // UI-scale (Options)
    // Right-side UI strip (minimap + command panel). The map view is kept to the
    // left of it so the panel never draws over the world.
    int panelW() const { return miniSize() + int(20 * uiScale_); }   // fallback width (no GUI loaded)

    // Scale from the retail 640x480 GUI space to screen pixels. Authored at 640x480;
    // we scale by height so the panel art keeps its aspect (winH/480 is true retail
    // scale -- the /700 divisor keeps the HUD from dominating high-res displays while
    // holding retail proportions and button alignment).
    float guiS() const { return uiScale_ * (winH_ > 0 ? winH_ : 480) / 700.0f; }
    // Width of the right-hand command-panel strip (the retail UnitMenu is 128 wide in
    // 640-space); never narrower than the minimap.
    int cmdPanelW() const;
    int mapViewW(int winW) const { return std::max(64, winW - cmdPanelW()); }

    // Screen rect for a command-panel gadget (x >= 512 in 640-space): the whole
    // command panel is anchored to the bottom-right corner, so the ButtonPanel art
    // and its buttons share one transform and stay aligned at any scale.
    SDL_FRect guiCmdRect(const tak::gui::Gadget& g) const;

    // Screen rect for a bottom-bar gadget (the retail InfoPanel occupies 640-space
    // y 431..480). The whole bar layout is scaled uniformly to our barH()-tall bar and
    // anchored bottom-left, so the unit-info block clusters at the left while the bar
    // chrome stretches to fill the width.
    SDL_FRect guiBarRect(const tak::gui::Gadget& g) const;

    SDL_FRect minimapRect(int winW, int winH) const;
    // Retail's full-screen radar (TAB). It is the SAME radar, drawn at a bigger
    // rect: the setter (icd 0x4f9ce0) flips one bool and recomputes the radar
    // rectangle from the 126x126 corner box to the whole world viewport, then
    // rebuilds the map picture and the blips. The world is still painted
    // underneath, the side and bottom HUD panels stay put, and the simulation
    // keeps running -- the tick driver never reads the flag. Because every use of
    // the radar goes through minimapRect(), enlarging that rect gives the drawing,
    // the camera click and the order click all at once, exactly as it does there.
    bool fsRadar_ = false;

    // Build the minimap WITHOUT stalling the first frame: averaging every block
    // decodes every terrain tile on the map (hundreds of ms cold), so the crunch
    // runs on a background thread and the texture is adopted when it lands. Both
    // call sites poll `if (!miniTex_) buildMinimap();` each frame already, which
    // doubles as the completion poll; the minimap frame just paints empty briefly.
    void buildMinimap();

    void drawMinimap(int winW, int winH);

    // Returns true if the click was inside the minimap (and moved the camera).
    // Map a minimap-space click to world coords; false if outside the minimap.
    bool minimapToWorld(float mx, float my, int winW, int winH, float& wx, float& wz);

    bool minimapClick(float mx, float my, int winW, int winH);

    // Right-click on the minimap: order the selection to that world point.
    bool minimapOrder(float mx, float my, int winW, int winH, bool queue);

    // Issue the armed order (pendingCmd_) to the selection at world (wx,wz). On the
    // main map (`precise`) 'a' targets an enemy under the cursor and 'g' the friendly
    // under it; from the minimap (coarse) 'a' falls back to attack-move and 'g' to the
    // nearest friendly. Shared by the map click and the minimap click.
    void issueArmedOrder(char cmd, float wx, float wz, bool queue, bool precise);

    // Left-click on the minimap while an order is armed (e.g. F): issue it at the
    // minimap location (F + minimap = fight-move there). Returns true if handled.
    bool minimapArmedOrder(float mx, float my, int winW, int winH);

    struct FeatArt;   // defined just below; FeatureInst only needs the pointer type
    struct FeatureInst {
        SDL_Texture* tex = nullptr;
        const std::vector<SDL_Texture*>* frames = nullptr;
        const FeatArt* art = nullptr;   // per-frame geometry + GAF timing
        SDL_Texture* shadow = nullptr;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        int sw = 0, sh = 0, sxoff = 0, syoff = 0;
        float x = 0, z = 0;
        std::string name;    // lowercase feature key (burn art + burnt-swap lookups)
        const FeatArt* burnArt = nullptr;   // seqnameburn playback (lazy, on ignition)
        uint8_t burnVis = 0; // sim says burning: draw burnArt + emit smoke
        // Snapshotted once per frame in syncBurningFeatures, under simMutex_. The
        // draw loop used to ask world_.featureAliveAt() per feature with no lock,
        // and addFeature() does a push_back that can REALLOCATE the feature vector
        // under that read -- a dangling read from the hot path, every frame.
        uint8_t aliveVis = 1;
        int simType = -2;    // last-seen sim FeatType index (-2 = not yet synced)
        int simId = -1;      // cell-derived sim feature id (matches World's ids)
        float lastSmoke = 0; // animClock_ of the last smoke puff
        bool tree = false;   // category=trees (eligible for the wind-sway option)
        bool mana = false;   // category=mana (deposit cluster: kept walkable/buildable)
        bool glowy = false;  // the animated "Sacred Stone" centre -- the actual
                             // buildable spot; category=mana + animating=1. The
                             // static "Standing Stones" (animating=0) are decoration.
    };
    std::vector<FeatureInst> features_;
    std::unordered_set<int> featInstIds_;   // sim ids with a visual inst (dynamic adds)
    std::vector<std::pair<float, float>> manaSpots_;   // Sacred Stone deposits

    struct FeatArt {
        SDL_Texture* tex = nullptr;
        SDL_Texture* shadow = nullptr;
        int w = 0, h = 0, xoff = 0, yoff = 0;
        int sw = 0, sh = 0, sxoff = 0, syoff = 0;
        std::vector<SDL_Texture*> frames;   // >1 entries when animating
        // Animated sequences (waves especially) author motion through PER-FRAME
        // size + anchor offsets, and each frame carries a GAF display duration in
        // 30Hz engine ticks. Retail keeps one clock per TYPE (all instances in
        // sync) and loops continuously.
        struct FGeom { int w = 0, h = 0, xoff = 0, yoff = 0; };
        std::vector<FGeom> fgeom;   // per-frame geometry, parallel to frames
        std::vector<int> tickEnd;   // cumulative end tick per frame
        int totalTicks = 0;         // full loop length in 30Hz ticks
    };
    std::map<std::string, tak::tdf::Node> featureDefs_;
    std::map<std::string, tak::gaf::Palette> featurePals_;
    std::map<std::string, FeatArt> featureArt_;

    // Unit ground shadows: the sprites from data/anims/shadows.gaf named by each
    // unit's FBI `shadowart`, recoloured to translucent black. Loaded once.
    struct ShadowTex { SDL_Texture* tex = nullptr; int w = 0, h = 0, xoff = 0, yoff = 0; };
    std::map<std::string, ShadowTex> shadowTex_;
    bool shadowsLoaded_ = false;
    const ShadowTex* shadowFor(const std::string& art);

    void loadFeatureDefs();

    const tak::gaf::Palette* featurePalette(std::string world);

    FeatArt* featureArtFor(const tak::tdf::Node& def, const char* seqKey = "seqname",
                           const char* shadKey = "seqnameshad");
    void swapFeatureArt(FeatureInst& fi, const std::string& name);
    void syncBurningFeatures();

    // Place one feature instance by definition name; returns success.
    bool addFeature(const std::string& rawName, float x, float z, bool blockNav);

    void loadFeatures();
    // Scatter retail wave sprites along the coast (display only; see the impl).
    void addShorelineWaves();

    // Track numbers for a side from gamedata/sidedata.tdf (falls back to IP).
    std::vector<int> factionMusicTracks(const std::string& side);

    // Every faction's music tracks (deduped), for a SPECTATOR -- who has no side of their
    // own, so their in-game music draws from all factions' playlists (startMusic shuffles).
    std::vector<int> allFactionMusicTracks();

    void loadPanel(const std::string& side);

    // Palette for a GUI GAF: the sibling anims/<gaf>.pcx if it exists (per-faction
    // panels ship one), else the global palettes/guipal.pal used by gui.gaf.
    tak::gaf::Palette guiPalette(const std::string& gaf);

    // Load one GAF sequence frame (anims/<gaf>, sequence seq, frame idx) to a texture.
    SDL_Texture* loadGuiFrame(const std::string& gaf, const std::string& seq, int frame);

    // Parse the faction in-game .gui and load every gadget's state art. side is the
    // 3-letter faction ("ara"/"tar"/"ver"/"zon"/"cre"); the file is guis/<side>ingame.gui.
    void loadGui(const std::string& side);

    static constexpr int kBarHBase = 72;
    int barH() const { return int(kBarHBase * uiScale_); }   // UI-scale (Options)

    struct OrderBtn {
        SDL_Texture* frames[3] = {nullptr, nullptr, nullptr};   // normal/hover/armed
        int w = 0, h = 0;
        char cmd = 0;        // 'm','a','p'; 0 = stop (instant)
        const char* label;
    };
    std::vector<OrderBtn> orderBtns_;

    // The per-faction conjure/build effect animation (TAF), keyed by side.
    std::map<std::string, std::vector<SDL_Texture*>> buildFx_;

    void loadBuildFx();

    void loadOrderButtons();

    SDL_FRect orderBtnRect(size_t i, int winW) const;

    void drawOrderColumn(int winW, int winH);

    // The front selected unit, if it has more than one weapon (worth a picker).
    const UnitR* multiWeaponSel();

    std::vector<SDL_FRect> weaponRects_;
    // A small row of weapon-select buttons under the order column: retail fires
    // only the active weapon; click (or press W to cycle) to pick another.
    void drawWeaponButtons(int winW, int winH);

    // Issue SetWeapon(slot) for every selected unit that has that slot.
    void selectWeapon(int slot);

    bool weaponButtonClick(float mx, float my);

    // ---- Retail GUI-driven HUD ----------------------------------------------
    // The command panel (right strip) and its order buttons are laid out from the
    // faction .gui (see loadGui). Rects are anchored bottom-right and share the
    // guiCmdRect transform so the ButtonPanel art and its buttons stay aligned.

    int guiIdx(const char* name) const;

    // Leftmost gadget with this name. Several bar gadgets appear twice -- the primary
    // single-unit group (UnitInfo1, x~114) and the second-unit group (UnitInfo2,
    // x~386); the primary is always the left one, the conjure-target the right one.
    int guiIdxLeft(const char* name) const;
    int guiIdxRight(const char* name) const;

    // (gadget index, command char) for each command button to show for the current
    // selection. cmd: 'm'/'a'/'p'/'g' arm pendingCmd_; 's' = Stop (immediate);
    // '1'/'2'/'3' = weapon slot; 'O'/'D'/'H' = stance offensive/defensive/passive;
    // 'K'/'k' = cloak on/off; 'N'/'F' = active on/off (all immediate toggles).
    std::vector<std::pair<int, char>> guiActiveButtons() const;

    std::vector<std::pair<SDL_FRect, char>> guiBtnRects_;
    // Press flash: which command button was last clicked and when, so a push button
    // shows retail's Pressed face for a moment even though we dispatch on mouse-down.
    char guiPressed_ = 0;
    uint32_t guiPressedMs_ = 0;   // hit list, filled by renderGui

    // Draw the command panel chrome + buttons + idle crystal ball. Falls back to the
    // old vertical order column if no .gui loaded.
    void renderGui(int winW, int winH);

    // Returns true if the click hit (and was handled by) a command-panel button.
    bool guiClick(float mx, float my);
    // Retail HUD feedback: every side-panel / build-icon press plays the local
    // faction's click tone (sounds/tone<side>.wav -- the click.hpi overridables).
    // Retail acknowledgment bong: the faction tone. The soundclass "_NN-note"
    // entry names LOOK like chromatic semitones (A=1..G#=12), but whether
    // retail actually pitch-shifts per command is UNPROVEN -- player memory
    // says one unvarying bong, and this data has dead keys (TreeBurn). The
    // pitch stays OFF until the icd playback-rate disassembly proves it;
    // the mixer's rate support is ready if it does. gain 2.0 undoes the /2
    // mixing headroom: retail played tones at native amplitude, and quiet
    // click.hpi replacements vanish at half volume.
    void playClickTone(int semitone = 0) {
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

    // The conjure/build menu for a builder type, filtered by the active mission's unit
    // whitelist (missions/<stem>.tdf) when one is loaded -- so a campaign mission only
    // offers the units it allows. An empty whitelist means no restriction.
    std::vector<std::string> conjureMenu(const std::string& builderType) const;

    // Returns true if a click hit (and was handled by) a conjure/build icon. The icons
    // sit above the info bar (not inside it), so this is hit-tested independently of the
    // bottom-bar region -- placement arms for structures/mobile conjurers, else it
    // trains/unqueues at a building (Ctrl toggles infinite, Shift/Ctrl+Shift = 5/10).
    bool buildIconClick(float mx, float my, bool lmb, bool rmb);

    // Issue a per-unit toggle command (targetId = value) to every selected own unit.
    void issuePerUnit(tak::net::Cmd kind, int value);

    // Draw a thin fill gauge (HP/mana) at a bar gadget's .gui position.
    void drawGauge(const char* name, float frac, SDL_Color c);

    // Retail's Unit Info dialog (guis/unitinfo.gui): portrait + the three mobility
    // stats for the hovered conjure icon, else the first selected unit. Does not
    // pause -- retail's didn't either. See client/gameview_unitinfo.cpp.
    const tak::sim::UnitType* unitInfoSubject() const;
    void toggleUnitInfo();
    void drawUnitInfo(int winW, int winH);
    const tak::sim::UnitType* unitInfoType_ = nullptr;   // null = dialog closed
    SDL_Texture* unitInfoBg_ = nullptr;
    SDL_Texture* unitInfoOk_ = nullptr;
    SDL_Texture* unitInfoIcon_ = nullptr;
    std::string unitInfoIconFor_;      // which type unitInfoIcon_ was baked for
    SDL_FRect unitInfoOkRect_{0, 0, 0, 0};

    // Retail bottom InfoPanel bar: chrome (InfoPanel + EndCap) plus the selected
    // unit's portrait/name/HP/mana at the .gui positions. Returns false (so drawPanel
    // keeps its own chrome) when no .gui is loaded. The build menu + mana readout stay
    // in drawPanel and draw on top.
    bool drawGuiInfoBar(int winW, int winH);

    // A one-word description of what the selected unit is doing, for the command
    // panel's HelpText recess.
    const char* unitStatusText(const UnitR* u) const;

    // What a builder is currently conjuring/building, for the info bar (else "").
    std::string conjureTargetName(const UnitR* u) const;

    // A simple filled bar (HP/mana) at explicit pixel coords.
    void drawBar(float x, float y, float w, float h, float frac, SDL_Color c);

    // Returns true if the click hit (and was handled by) the order column.
    bool orderColumnClick(float mx, float my, int winW);

    std::map<std::string, SDL_Texture*> weaponIcons_;
    // Weapon-slot icon: anims/weaponpic/<name>{sb,sbh}.jpg (name lowercased, spaces
    // stripped; sb = normal, sbh = selected/gold), falling back to the default_* pics
    // for weapons that ship no icon. These 32x32 JPGs are the full button (icon +
    // recessed frame), so they replace the empty WPrimaryButton recess.
    SDL_Texture* weaponIcon(const std::string& wname, bool selected);

    SDL_Texture* iconFor(const std::string& typeId);

    // Fallback build icon: render the unit's 3D model into a small cached texture,
    // for buildables that ship no anims/buildpic. Retail never drew a build pic for
    // the Zhon trapdoor spider (zonspide) -- a base-game creature the Crusades
    // balance made buildable -- so without this its slot would be an empty box.
    SDL_Texture* modelIconTex(const std::string& id, int slot, bool canMove);

    // The selected builder (any builder in the selection).
    const UnitR* selectedBuilder();

    // Keys.TDF-derived hotkeys. Returns true when the key was consumed.
    bool handleKey(SDL_Keycode key, uint16_t mod);

    // Replace the selection with every owned, living unit matching `pred`.
    template <class Pred>
    void selectOwned(Pred pred) {
        if (spectating_) return;   // watch-only
        selection_.clear();
        for (const UnitR* _up : front().live) {
            const UnitR& u = *_up;
            if (u.alive() && u.player == localPlayer_ && u.type && !u.underConstruction &&
                pred(u))
                selection_.push_back(u.id);
        }
        if (!selection_.empty()) voice(selection_.front(), "select");
    }

    // --- control squads (groups / formations); backed by lockstep Cmd::SetSquad -------
    void issueSquad(int unitId, int val);

    // Assign the current selection to squad `num` as a group (sign +1) or formation
    // (sign -1). Assign REPLACES the squad (old members not selected are dropped); SHIFT
    // appends (keeps them, converting the whole number to this type). A unit is in one
    // squad -- setting it here removes it from any other -- and a number is a group XOR a
    // formation, so re-typing a number moves every member onto the new type.
    void assignSquad(int num, int sign, bool append);

    // Recall squad `num` (group or formation): select its members, but NOT any builders in
    // it -- a builder rides in a squad only to feed its products in, not to be commanded
    // with the fighters.
    void recallSquad(int num);

    // Ctrl+Esc: drop the selected units from whatever squad each is in.
    void clearSquad();

    bool onScreen(const UnitR& u) const;

    // Cycle the selection to the next owned unit (single-select stepping).
    void cycleNextUnit();

    void centerOn(int id);

    // Centre the camera on the average position of the live selected units.
    // Returns false if nothing in the selection is still alive.
    bool centerOnSelection();

    // The player's HUD accent — follows their chosen player colour, not faction.
    SDL_Color factionColor() const { return playerColor(localPlayer_); }

    // Is this player on the local player's team? (Allies share vision, so their
    // units render/appear on the minimap through fog just like your own.)
    bool alliedToLocal(int player) const { return world_.allied(player, localPlayer_); }

    // A built-in 5x7 pixel font (uppercase, digits, a few symbols), drawn as
    // solid blocks — unmistakably legible at any size, unlike the game's small
    // decorative fonts. Each glyph is 5 columns; bit 0 of a column is the top.
    static const uint8_t* glyph5x7(char c) {
        static const std::map<char, std::array<uint8_t, 5>> F = {
            {'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0x00,0x42,0x7F,0x40,0x00}},
            {'2',{0x42,0x61,0x51,0x49,0x46}}, {'3',{0x21,0x41,0x45,0x4B,0x31}},
            {'4',{0x18,0x14,0x12,0x7F,0x10}}, {'5',{0x27,0x45,0x45,0x45,0x39}},
            {'6',{0x3C,0x4A,0x49,0x49,0x30}}, {'7',{0x01,0x71,0x09,0x05,0x03}},
            {'8',{0x36,0x49,0x49,0x49,0x36}}, {'9',{0x06,0x49,0x49,0x29,0x1E}},
            {'A',{0x7E,0x11,0x11,0x11,0x7E}}, {'B',{0x7F,0x49,0x49,0x49,0x36}},
            {'C',{0x3E,0x41,0x41,0x41,0x22}}, {'D',{0x7F,0x41,0x41,0x22,0x1C}},
            {'E',{0x7F,0x49,0x49,0x49,0x41}}, {'F',{0x7F,0x09,0x09,0x09,0x01}},
            {'G',{0x3E,0x41,0x49,0x49,0x7A}}, {'H',{0x7F,0x08,0x08,0x08,0x7F}},
            {'I',{0x00,0x41,0x7F,0x41,0x00}}, {'J',{0x20,0x40,0x41,0x3F,0x01}},
            {'K',{0x7F,0x08,0x14,0x22,0x41}}, {'L',{0x7F,0x40,0x40,0x40,0x40}},
            {'M',{0x7F,0x02,0x0C,0x02,0x7F}}, {'N',{0x7F,0x04,0x08,0x10,0x7F}},
            {'O',{0x3E,0x41,0x41,0x41,0x3E}}, {'P',{0x7F,0x09,0x09,0x09,0x06}},
            {'Q',{0x3E,0x41,0x51,0x21,0x5E}}, {'R',{0x7F,0x09,0x19,0x29,0x46}},
            {'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7F,0x01,0x01}},
            {'U',{0x3F,0x40,0x40,0x40,0x3F}}, {'V',{0x1F,0x20,0x40,0x20,0x1F}},
            {'W',{0x7F,0x20,0x18,0x20,0x7F}}, {'X',{0x63,0x14,0x08,0x14,0x63}},
            {'Y',{0x07,0x08,0x70,0x08,0x07}}, {'Z',{0x61,0x51,0x49,0x45,0x43}},
            {'/',{0x20,0x10,0x08,0x04,0x02}}, {'+',{0x08,0x08,0x3E,0x08,0x08}},
            {'-',{0x08,0x08,0x08,0x08,0x08}}, {':',{0x00,0x36,0x36,0x00,0x00}},
            {'.',{0x00,0x60,0x60,0x00,0x00}}, {'%',{0x63,0x13,0x08,0x64,0x63}},
            // An unknown glyph advances like a space, so anything missing here is
            // silently SWALLOWED rather than shown wrong. '_' is legal in an account
            // name (see auth::validUsername) and this table drew the lobby's player
            // list, so "pocket_geek" read as "pocket geek"; the rest is the sentence
            // punctuation chat needs for the same reason.
            {'_',{0x40,0x40,0x40,0x40,0x40}}, {',',{0x00,0x50,0x30,0x00,0x00}},
            {'!',{0x00,0x00,0x5F,0x00,0x00}}, {'\'',{0x00,0x00,0x03,0x00,0x00}},
            {'?',{0x02,0x01,0x51,0x09,0x06}}, {'(',{0x00,0x1C,0x22,0x41,0x00}},
            {')',{0x00,0x41,0x22,0x1C,0x00}}, {'*',{0x14,0x08,0x3E,0x08,0x14}},
        };
        auto it = F.find(c);
        return it == F.end() ? nullptr : it->second.data();
    }

    // Draw text in the built-in block font. `px` is the size of one font pixel.
    void blockText(const std::string& s, float x, float y, float px, SDL_Color c);
    float blockWidth(const std::string& s, float px) const { return s.size() * 6 * px; }

    // A top-of-screen status line (network error, notice, pending-order prompt).
    // These print coloured text straight over the terrain, where it can be nearly
    // unreadable against grass/rock -- so back every one with a dark rounded panel
    // first. Returns the y for a following line so stacked messages don't collide.
    // `leftX < 0` centres the text in `winW`; otherwise it's the left edge.
    float hudBanner(const std::string& msg, float y, float scale, SDL_Color col,
                    int winW, float leftX = -1) {
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

    // ==================== interactive multiplayer lobby ====================

    static const char* factionName(int f) {
        static const char* n[5] = {"ARAMON", "TAROS", "VERUNA", "ZHON", "CREON"};
        return n[f % 5];
    }
    // Lobby design size (logical). The lobby always lays out at exactly this size and
    // is scaled to fit + centred in the window (lobbyScale_ / lobbyOffX_/Y_), so it
    // shows fully at any window size or aspect. Hit-tests undo the same transform.
    static constexpr float kLobbyW = 960.0f;
    static constexpr float kLobbyH = 540.0f;
    bool lbHot(const SDL_FRect& r) const;
    // A clickable button: panel + centered label; registers its action.
    void lbBtn(float x, float y, float w, float h, const std::string& label, bool enabled,
               std::function<void()> action, SDL_Color base = {60, 66, 86, 255}) {
        SDL_FRect r{x, y, w, h};
        bool hot = enabled && lbHot(r);
        SDL_Color c = enabled ? (hot ? SDL_Color{90, 110, 150, 255} : base)
                              : SDL_Color{40, 42, 50, 255};
        SDL_SetRenderDrawColor(ren_, c.r, c.g, c.b, 255);
        SDL_RenderFillRectF(ren_, &r);
        SDL_SetRenderDrawColor(ren_, hot ? 180 : 90, hot ? 200 : 100, hot ? 240 : 130, 255);
        SDL_RenderDrawRectF(ren_, &r);
        // Shrink the label if it would overflow the button (keeps long captions like
        // "OVERRIDES: COSMETIC" inside their box). 8px total horizontal padding.
        float px = 2.0f;
        float fit = (w - 8.0f) / std::max<size_t>(1, label.size()) / 6.0f;
        if (fit < px) px = std::max(fit, 1.0f);
        float tw = blockWidth(label, px);
        blockText(label, x + (w - tw) / 2, y + (h - 7 * px) / 2, px,
                  enabled ? SDL_Color{225, 230, 240, 255} : SDL_Color{110, 115, 125, 255});
        if (enabled && action) lobbyHots_.push_back({r, std::move(action)});
    }
    // A text-input field: label + box; clicking activates it (id != 0).
    void lbField(float x, float y, float w, const std::string& label,
                 const std::string& value, int id);
    void colorSwatch(float x, float y, float s, int color, std::function<void()> action);

    void drawLobby(int winW, int winH);

    void drawBrowser(int winW, int winH);

    // Locate an 8-bit-indexed {u32 w, u32 h, w*h bytes} image at TNT header field
    // `field` (little-endian u32 pointers). Returns false (with bounds checks) if the
    // field is absent or malformed. Field 12 = the hi-res overview, 11 = the minimap.
    static bool tntIndexedImage(const std::vector<uint8_t>& d, int field,
                                int& w, int& h, const uint8_t*& data) {
        auto u32 = [&](size_t o) -> uint32_t {
            return uint32_t(d[o]) | (uint32_t(d[o + 1]) << 8) | (uint32_t(d[o + 2]) << 16)
                 | (uint32_t(d[o + 3]) << 24);
        };
        size_t hoff = size_t(field) * 4;
        if (hoff + 4 > d.size()) return false;
        size_t p = u32(hoff);
        if (p == 0 || p + 8 > d.size()) return false;
        uint32_t iw = u32(p), ih = u32(p + 4);
        if (iw == 0 || ih == 0 || iw > 4096 || ih > 4096) return false;
        if (p + 8 + size_t(iw) * ih > d.size()) return false;
        w = int(iw); h = int(ih); data = &d[p + 8];
        return true;
    }

    // The map's "kingdom" (from its sibling .ota [GlobalHeader] kingdom=...), which
    // names the preview palette. The .ota carries the preview info too:
    // kingdom=<side>, size=<W x H>, numplayers=<N>, memory=<...>.
    std::string readOta(const std::string& tntPath) const;
    // Value of `key`=... in an .ota (up to ';' or EOL, trimmed). Keeps internal spaces
    // (so "size=9 x 7" -> "9 x 7"); matches only a whole-word key before its '='.
    static std::string otaField(const std::string& ota, const char* keyLower) {
        std::string low = ota;
        for (char& c : low) c = char(std::tolower((unsigned char)c));
        std::string k = keyLower;
        for (size_t pos = 0; (pos = low.find(k, pos)) != std::string::npos; pos += k.size()) {
            if (pos > 0 && (std::isalnum((unsigned char)low[pos - 1]) || low[pos - 1] == '_')) continue;
            size_t e = pos + k.size();
            while (e < low.size() && (low[e] == ' ' || low[e] == '\t')) ++e;
            if (e >= low.size() || low[e] != '=') continue;
            size_t a = e + 1, end = ota.find_first_of(";\r\n", a);
            if (end == std::string::npos) end = ota.size();
            std::string v = ota.substr(a, end - a);
            size_t s = v.find_first_not_of(" \t"), t = v.find_last_not_of(" \t");
            return s == std::string::npos ? std::string() : v.substr(s, t - s + 1);
        }
        return {};
    }

    // The 256-colour RGBA palette for a kingdom (palettes/<kingdom>.pcx), cached.
    // Returns nullptr if the kingdom is unknown / its .pcx is missing.
    const std::vector<uint8_t>* kingdomPalette(const std::string& kingdom);

    // Build the selected map's preview texture. Retail (KINGDOMS.icd, MapView gadget
    // at 0x4ae4f0) draws the .tnt's embedded RADARPIC through the map's KINGDOM palette
    // (palettes/<kingdom>.pcx named by the .ota [GlobalHeader] kingdom=) -- that's the
    // pale-parchment/pink-path look, not the terrain palette.pal. We colour the higher-
    // res BIGRADARPIC (overview, field 12; minimap field 11 is the fallback) through
    // that same kingdom palette, so it matches retail's style but crisper. Index 9 is
    // retail's transparent index. Rebuilt only on selection change.
    // Build the picker's map list once: names + paths from the VFS, enriched with
    // each map's player count and size read from its .ota GlobalHeader (a small text
    // file -- numplayers + "size = W x H" -- so no TNT decompression per map).
    void buildMapList();

    // Sort the map list by the active key (name / players / size), tie-broken by name,
    // in the chosen direction.
    void sortMapList();

    void buildMapPreview(const std::string& tntPath);
    void buildGenPreview(const std::string& id);   // thumbnail for a "~gen1~" map

    void drawCreate(int winW, int winH);

    void drawRoom(int winW, int winH);

    static bool startValid(const tak::net::RoomView& room) {
        int used = 0; bool color[10] = {};
        for (int i = 0; i < tak::net::kMaxSlots; ++i) {
            const auto& s = room.slots[i];
            if (s.type != 1 && s.type != 2) continue;
            ++used;
            if (s.type == 1 && !s.ready) return false;
            if (s.color < 10) { if (color[s.color]) return false; color[s.color] = true; }
        }
        return used >= 2;
    }
    const tak::net::RoomView& mpRoom() const { return mp_->room(); }

    // A panel-local logical point, undoing the lobby's fit-scale + centre offset.
    void lobbyMouse(float& mx, float& my) const;
    static bool ptIn(const SDL_FRect& r, float x, float y) {
        return x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h;
    }
    void clampMapScroll();
    // Set the map scroll from the current thumb-drag mouse position: map the cursor's
    // y within the list box to a first-visible-row index.
    void setMapScrollFromThumb();
    void lobbyInput(const SDL_Event& e, int winW, int winH);
    std::string* lbFieldBuf();

    // F4: per-faction live unit counts, top-left.
    // F7 diagnostic: tint elevated cells, and for each unit show its cell height,
    // computed lift, its RAW (unlifted) foot position (magenta dot) vs its LIFTED
    // foot position (cyan dot). Lets us see whether a unit that looks "on the wall"
    // is actually on a high heightmap cell or on flat ground beside painted relief.
    void drawHDebug();

    void drawUnitCounts(int winW);


    // The in-mission objectives panel (top-left): the mission's objectives, toggled
    // with the O key. Only shown for campaign missions (missionObjectives_ non-empty).
    void drawObjectivesPanel(int winW, int /*winH*/);

    void drawPanel(int winW, int winH);

    void drawFog();
    std::vector<SDL_Vertex> fogVerts_;
    std::vector<float> fogLift_;      // per-corner heightAbove scratch (see drawFog)
    uint32_t fogTexGen_ = ~0u;        // vis generation last uploaded to fogTex_

    // Positions along a build-drag line, spaced by the building's footprint.
    std::vector<std::pair<float, float>> buildLinePositions(
        float x0, float z0, float x1, float z1) const;

    // Queue a whole line of the current building from a shift-drag.
    void placeBuildLine(float x0, float z0, float x1, float z1);

    void drawGhost();

    int rosterIndexOf(int unitId);

    int32_t mapCommand(int sub, const std::vector<int32_t>& a);

    void voice(int unitId, const std::string& event);

    // --- GAF/TAF impact effects (gamedata/explosions -> data/anims/*.taf) ------
    // A hitscan shot's visible flash: instant-hit weapons (Line of Sight) spawn no
    // projectile, so this is the ONLY thing drawn for them. Lives ~0.15s and fades.
    struct BeamFx {
        float x1 = 0, z1 = 0, x2 = 0, z2 = 0;
        float alt1 = 0, alt2 = 0;
        // Retail treats a Line-of-Sight shot as a VIRTUAL projectile: the damage is
        // instant, but the bolt is drawn from the muzzle out to a head travelling at
        // weaponvelocity, and it expires when that head reaches the victim. So the
        // lifetime is per-shot (distance / velocity), not a fixed flash.
        float life = 0.16f;
        uint8_t inner[3] = {255, 255, 255};
        uint8_t middle[3] = {200, 230, 255};
        uint8_t outer[3] = {120, 170, 255};
        bool lightning = false;   // jagged bolt vs a clean beam
        float age = 0;
    };
    std::vector<BeamFx> beams_;
    // Live wandering storms by id, so a new one can announce itself and a
    // vanished one can leave its dissipation art behind.
    struct StormTrack { float x = 0, z = 0; const tak::sim::Weapon* w = nullptr; };
    std::unordered_map<int, StormTrack> stormsSeen_;
    struct EFrame { SDL_Texture* tex = nullptr; int w = 0, h = 0, ax = 0, ay = 0; };
    struct EffectAnim { std::vector<EFrame> frames; };
    std::map<std::string, std::vector<std::string>> explosionClasses_;  // class -> anim names
    std::map<std::string, EffectAnim> effectAnims_;                     // anim name -> frames
    bool explosionsLoaded_ = false;
    struct EffectInst {
        const EffectAnim* anim = nullptr;
        float x = 0, z = 0, age = 0;
        float delay = 0;   // seconds before it starts playing
        float dur = 0;     // seconds for one playthrough (0 => use kEffectFps)
        int loops = 1;     // how many times to repeat (ground fire loops)
        float alt = 0;     // extra screen lift (impact on an airborne target)
    };
    std::vector<EffectInst> effects_;
    static constexpr float kEffectFps = 20.0f;

    // Per-loop playback length of an effect instance, in seconds.
    static float effLoopLen(const EffectInst& e) {
        return e.dur > 0 ? e.dur : float(e.anim->frames.size()) / kEffectFps;
    }
    // Play a named effect anim at (x,z), optionally delayed / stretched / looped.
    // `alt` lifts it on screen (impact on an airborne target).
    void spawnEffectAnim(const std::string& anim, float x, float z,
                         float delay = 0, float dur = 0, int loops = 1, float alt = 0) {
        const EffectAnim* ea = effectFor(anim);
        if (ea) effects_.push_back({ea, x, z, 0.0f, delay, dur, loops, alt});
    }

    void loadExplosionClasses();
    // Load a named effect animation from its TAF/GAF (truecolor _4444 preferred).
    const EffectAnim* effectFor(const std::string& animName);
    // Play the named explosion class (a random variant) at (x,z). Returns false
    // if the class/art is unavailable (caller then falls back to particles).
    bool spawnEffect(const std::string& cls, float x, float z, float alt = 0) {
        if (cls.empty()) return false;
        loadExplosionClasses();
        auto it = explosionClasses_.find(cls);
        if (it == explosionClasses_.end() || it->second.empty()) return false;
        const std::string& anim = it->second[salt_++ % it->second.size()];
        const EffectAnim* ea = effectFor(anim);
        if (!ea) return false;
        effects_.push_back({ea, x, z, 0.0f, 0.0f, 0.0f, 1, alt});
        static const bool kLog = tak::devEnv("TAK_FXLOG") != nullptr;
        if (kLog) std::fprintf(stderr, "t=%.2f effect '%s' anim '%s' (%zu frames)\n",
                               animClock_, cls.c_str(), anim.c_str(), ea->frames.size());
        return true;
    }
    void updateEffects(float dt);
    // A shockwave ring: `sprites` copies of an effect anim arranged around a
    // circle that expands from the centre to maxR over `dur` (TAK radiusart).
    struct RingFx {
        const EffectAnim* anim = nullptr;
        float x = 0, z = 0, age = 0, delay = 0, dur = 1.2f, maxR = 120;
        int sprites = 24;
    };
    // Ambient wind (retail WindChange callin): a slow random walk, re-sent to
    // every unit with the script whenever it shifts. Purely cosmetic, per-client.
    // Ambient wind. Display-only (it drives the COB WindChange call-in on flags and
    // sails, plus smoke drift) and never hashed. The range is the MAP's: retail reads
    // minwindspeed/maxwindspeed from the .ota, defaulting to 100/2000.
    float windMin_ = 100.0f, windMax_ = 2000.0f;
    std::string windFrom_;   // mapPath_ the range above was read for ("" = not yet)
    void loadMapWind();      // lazily read min/maxwindspeed from the map's .ota
    float windHeading_ = 0.8f, windSpeed_ = 150;
    float windNext_ = 0;   // animClock_ time of the next shift
    int windGen_ = 1;      // bumped per shift; Anim.windStamp tracks delivery
    // Camera shake (weapon shakemagnitude/shakeduration on heavy impacts).
    float shakeTime_ = 0, shakeDur_ = 0, shakeMag_ = 0;
    void triggerShake(float mag, float dur);

    std::vector<RingFx> rings_;
    void spawnRing(const std::string& anim, float x, float z, float delay,
                   float dur, int sprites, float maxR);
    void updateRings(float dt);
    void drawRings();

    void drawEffects();

    // Persistent per-unit ambient fire/smoke (emit-sfx). One looping flame/smoke per
    // unit, cycled off the continuous animClock so it flows smoothly (no restart/gaps
    // like re-spawned one-shots), kept alive while the unit's emit-loop keeps firing.
    void drawUnitFx();

    // Procedural particle for impact explosions, flame, and blood spray.
    struct Particle {
        float x = 0, z = 0, vx = 0, vz = 0, alt = 0, valt = 0;
        float life = 0, maxLife = 1, size = 3;
        Uint8 r = 255, g = 200, b = 80;
        int kind = 0;   // 0 spark/blood (gravity), 1 smoke (rises, fades)
    };
    std::vector<Particle> particles_;
    // Spawn a burst of `n` particles at (x,z) with a colour and speed spread.
    void spawnBurst(float x, float z, int n, Uint8 r, Uint8 g, Uint8 b,
                    float spread, float sizeMax, int kind, float baseAlt = 0) {
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
    // An impact effect scaled to the weapon: fire/lightning tinted, aoe-sized.
    void spawnImpact(const tak::sim::Weapon& w, float x, float z, float baseAlt = 0) {
        using Fx = tak::sim::WeaponFx;
        float sc = 1.0f + std::min(w.aoe, 200.0f) / 40.0f;
        int n = int(6 + std::min(w.aoe, 200.0f) / 6);
        if (w.fx == Fx::Fire) {
            spawnBurst(x, z, n, 240, 130, 40, 34 * sc, 2.4f * sc, 0, baseAlt);
            spawnBurst(x, z, n / 2, 90, 80, 80, 20 * sc, 3.0f * sc, 1, baseAlt);   // smoke
        } else if (w.fx == Fx::Lightning) {
            spawnBurst(x, z, n, 200, 225, 255, 40 * sc, 2.0f * sc, 0, baseAlt);
        } else {
            spawnBurst(x, z, n, 210, 200, 170, 26 * sc, 2.0f * sc, 0, baseAlt);   // dust
            spawnBurst(x, z, n / 3, 110, 100, 90, 16 * sc, 2.6f * sc, 1, baseAlt);
        }
    }
    void updateParticles(float dt);
    void drawParticles();
    std::vector<SDL_Vertex> partBatch_;   // reused particle-quad batch

    SoundBank sounds_;
    ThreadPool pool_;                       // for parallel per-unit VM ticks
    std::vector<tak::cob::Vm*> vmTick_;     // scratch list for the parallel pass
    SoundClasses soundClasses_;
    uint32_t salt_ = 0;
    std::atomic<int> outcome_{0};   // 0 = playing, 1 = victory, -1 = defeat (worker writes, main reads)
    bool sawTeam_[tak::sim::kMaxPlayers] = {};   // teams that have ever fielded a unit
    // Dev-only N-player free-for-all / teams harness (TAK_FFA=N[,teams]); the
    // real lobby (multiplayer M3) replaces it. When >0, an AI Controller drives
    // every player, not just player 1.
    int ffaPlayers_ = 0;
    bool amphib_ = false;
    int amphibPhase_ = 0, amphibSquad_ = 0, transportId_ = -1;
    float amphibLandX_ = 0, amphibLandZ_ = 0, amphibSeaX_ = 0, amphibSeaZ_ = 0;
    Font hudFont_, bigFont_, statFont_;

    struct Region { int a, b, c, d; bool rect; bool armed; };
    std::unique_ptr<tak::cob::Vm> missionVm_;
    std::vector<std::string> missionRoster_;
    std::map<int, Region> regions_;
    int missionTowerIdx_ = -1;
    std::set<int> building_;
    std::set<int> missionAliveP0_;   // player-0 units seen alive (for the UnitDestroyed edge)
    std::map<int, std::vector<std::string>> reinfPool_;
    int reinfIdx_ = 0;
    std::vector<std::string> briefing_;
    float briefTimer_ = 0;
    std::string notice_;
    float noticeTimer_ = 0;
    float animClock_ = 0;
    float trigTimer_ = 0;
};


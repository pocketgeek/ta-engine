#include "util/virtualpath.h"
#include "util/scenariotrace.h"
// taclient — interactive TA asset viewer.
//
//   taclient map <map.tnt> <terrain-dir>       scrollable terrain (drag/arrows,
//                                             +/- zoom, S = screenshot)
//   taclient model <file.3do> [textures-dir palette.pcx]
//                                             rotating textured model
//                                             (drag to rotate, wheel zoom)
//   ... --shot <out.png>                      render one frame headless
//
// Textures dir = extracted data/textures; palette = faction palette PCX
// (e.g. palettes/ara_textures.pcx from sidedata.tdf).

// Must precede SDL.h: on Windows this pulls in winsock2 (with WIN32_LEAN_AND_MEAN)
// before SDL's <windows.h> would otherwise pull the incompatible winsock v1.
#include "net/netcompat.h"
#include "util/winargv.h"

#include "campaign/campaign.h"
#include <utility>
#include "client/tafrontend.h"
#include "client/streaming.h"
#include "client/uitext.h"
#include "client/serveraddress.h"
#include "client/presentationpacer.h"
#include "client/briefingscreen.h"
#include "client/artscale.h"
#include "client/videofilter.h"
#include "client/runtimesettings.h"
#include "client/resultscreen.h"
#include "cob/vm.h"
#include "crt/crt.h"
#include "gaf/gaf.h"
#include "gui/gui.h"
#include "hpi/hpi.h"
#include "net/client.h"
#include "net/mappackage.h"
#include "util/procmetrics.h"   // benchmark: cross-platform CPU/RSS sampling
#include "net/lockstep.h"
#include "ai/ai.h"          // Difficulty <-> aiLevel + incomeMultFor (header-only helpers)
#include "sim/matchsetup.h"
#include "sim/sim.h"
#include "sim/simcpus.h"
#include "sim/legion.h"
#include "tdf/tdf.h"
#include "tdo/tdo.h"
#include "terrain/terrain.h"
#include "tnt/tnt.h"
#include "tnt/mapgen.h"
#include "util/appicon.h"
#include "util/png.h"
#include "version.h"
#include "client/cursors.h"
#include "client/dirpicker.h"   // first-run data-dir folder picker
#include "client/font.h"      // GAF bitmap font (extracted leaf class)
#include "client/mapview.h"   // terrain pan/zoom + async chunk compositor (extracted leaf)
#include "client/modelmath.h"   // Tri/Xform/scriptRot (shared by GameView + model viewer)
#include "client/modelview.h"   // standalone 3DO model viewer (extracted leaf)
#include "client/renderframe.h"   // UnitR/PlayerR/Frame render snapshot (extracted leaf)
#include "client/replayfile.h"   // .tarep parser (extracted leaf)
#include "client/sound.h"     // WAV mixer + music + soundclasses (extracted leaf)
#include "client/threadpool.h"   // data-parallel worker pool (extracted leaf)
#include "client/gpuvram.h"   // GPU memory accountant + safety-net cap (Options limit)
#include "client/hotkeys.h"
#include "client/hotkeysscreen.h"
#include "client/options.h"
#include "client/voice.h"
#include "client/datafolder.h"
#include "util/relaunch.h"
#include "util/prefpath.h"
#include "client/settings.h"
#include "client/ccdpin.h"
#include "client/hidpi.h"
#include "client/waylandchoice.h"
#include "client/waylandmeasure.h"
#include "client/renderdriver.h"
#include "net/crypto.h"
#include "net/replaywriter.h"   // recoverReplayParts
#include "client/dev.h"
#include "client/appquit.h"
#include "client/mainmenu.h"
#include "client/menumusic.h"
#include "client/gameview.h"   // the in-world game client (extracted from this file)

// Keep our own main() on every platform (don't let SDL redefine it to SDL_main /
// pull in SDL2main + a WinMain); we call SDL_SetMainReady() in main() instead. This
// also keeps taclient usable as a console/headless tool on Windows. The build also
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

// Single-player auto-launches a local taserver (AIs run only on the server); the
// process itself is client/localserver.h. Sockets come from net/netcompat.h
// (included first, before SDL).
#include "client/localserver.h"
#include "client/sessionfailure.h"
#include "util/u8path.h"
#ifdef _WIN32
  #include <windows.h>
#endif

namespace {
// Pick a free loopback TCP port by binding to 0 and reading the assignment.
// The LOCAL SERVER's port: the usual 7677 when nothing else holds it on any interface
// (so a typed address needs no port), else any free one (the LAN announcement names it).
int pickLanPort() {
    ta::net::netStartup();
    int fd = int(socket(AF_INET, SOCK_STREAM, 0));
    if (fd >= 0) {
#ifndef _WIN32
        // As the server binds it (listenOn): connections lingering in TIME_WAIT from an
        // earlier game must not make the port look taken.
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#endif
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_ANY); a.sin_port = htons(7677);
        const bool free = ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
        ta::net::sockClose(fd);
        if (free) return 7677;
    }
    return 0;
}

int pickFreePort() {
    ta::net::netStartup();
    int fd = int(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return 0;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        socklen_t len = sizeof a;
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0) port = ntohs(a.sin_port);
    }
    ta::net::sockClose(fd);
    return port;
}

// Launch a taserver for a private single-player game. Returns true on success.
// --no-auth because there is nobody to sign in AS (this server exists for one
// player, on one machine), and --local so that concession stays on this machine:
// an unauthenticated server must not be reachable from the network.
// lan: the CONNECT screen's LOCAL SERVER instead -- taserver --lan, unencrypted and
// without accounts on every interface, so players on the network can join it.
// simThreads: the server's half of the player's SIMULATION THREADS, for its referee sim
// (0 = auto: only when a debug build's TA_SIM_WORKERS decides AUTO, there as here).
// simCpus: the CPUs that half is pinned to, as taserver --sim-cpus takes them (a list,
// or "none" when this game pins nothing).
// processCpus: PIN TO CCDS' CCD for every thread of the server (taserver --cpus), or none.
bool spawnLocalServer(ta::LocalServer& server, const std::string& serverBin,
                      const std::string& dataRoot, int port, bool lan = false, int simThreads = 0,
                      const std::string& simCpus = "none", const std::vector<int>& processCpus = {}) {
    // It shares this player's per-user map folder (hpi::userMapRoot): a map the client
    // verified (a download, a --play-map copy) is there for the referee too, and
    // nothing is written into the game data folder.
    std::vector<std::string> args{"--port", std::to_string(port), "--data", dataRoot,
                                  "--sim-threads", simThreads > 0 ? std::to_string(simThreads) : "auto",
                                  "--sim-cpus", simCpus};
    if (!processCpus.empty()) args.insert(args.end(), {"--cpus", ta::sim::cpuListText(processCpus)});
    if (lan) args.push_back("--lan");
    else args.insert(args.end(), {"--no-auth", "--local"});
    if (const auto cache = ta::hpi::userMapRoot(); !cache.empty()) {
        const auto text = cache.u8string();
        args.insert(args.end(), {"--map-cache-dir", std::string(text.begin(), text.end())});
    }
    return server.start(serverBin, args);
}

// Path to the taserver binary that sits beside this client. argv[0] is unreliable:
// launched from PATH (e.g. the /usr/bin .rpm/.deb install) it's the bare name
// "taclient" with no directory, and execl/CreateProcess do NOT search PATH -- which
// is why single-player "never started taserver". Resolve the REAL executable's
// directory (/proc/self/exe on Linux, the module path on Windows), then fall back to
// argv[0]'s directory and finally a PATH scan. Returns the first "taserver" found.
std::string resolveServerBin(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto tryDir = [&](const fs::path& dir) -> std::string {
        if (dir.empty()) return {};
#ifdef _WIN32
        const fs::path p=dir/"taserver.exe";
#else
        const fs::path p=dir/"taserver";
#endif
        if(!fs::is_regular_file(p,ec))return {};
        const auto text=p.u8string();return {text.begin(),text.end()};
    };
    if(char* base=SDL_GetBasePath()) {
        const auto directory=ta::u8path(base);SDL_free(base);
        if(auto result=tryDir(directory);!result.empty())return result;
    }
    if (auto r = tryDir(ta::u8path(argv0).parent_path()); !r.empty()) return r;
    if (const char* path = std::getenv("PATH")) {
        std::string ps(path);
        for (size_t s = 0; s <= ps.size();) {
            size_t sep = ps.find(
#ifdef _WIN32
                ';'
#else
                ':'
#endif
                , s);
            std::string dir = ps.substr(s, sep == std::string::npos ? std::string::npos : sep - s);
            if (auto r = tryDir(dir); !r.empty()) return r;
            if (sep == std::string::npos) break;
            s = sep + 1;
        }
    }
    // Nothing found -- return the old argv[0]-relative guess so the caller can report it.
#ifdef _WIN32
    const auto fallback=ta::u8path(argv0).parent_path()/"taserver.exe";
#else
    const auto fallback=ta::u8path(argv0).parent_path()/"taserver";
#endif
    const auto text=fallback.u8string();return {text.begin(),text.end()};
}
}  // namespace

namespace {

// Default windowed size (used when not fullscreen -- the default IS fullscreen; see
// Settings::fullscreen). 1920x1080 for modern displays. The 4:3 title menu
// letterboxes within it; the lobby scales to fit + centres itself (kLobbyW/kLobbyH),
// so both stay fully visible at this or any other size/aspect.
constexpr int kWinW = 1920, kWinH = 1080;

void screenshot(SDL_Renderer* ren, int w, int h, const std::string& path) {
    std::vector<uint8_t> px(size_t(w) * h * 4);
    // An explicit w x h rect: with nullptr SDL reads the CURRENT viewport, which a
    // fullscreen switch or resize earlier in the frame has already grown past the
    // buffer (it overran the heap in a Windows fullscreen-toggle capture).
    const SDL_Rect area{0, 0, w, h};
    if (SDL_RenderReadPixels(ren, &area, SDL_PIXELFORMAT_RGBA32, px.data(), w * 4) == 0) {
        ta::png::write(path, w, h, px);
        std::printf("screenshot: %s\n", path.c_str());
    }
}

// ---------------------------------------------------------------- map mode

// MapView (terrain pan/zoom + async chunk compositor) moved to
// client/mapview.h + mapview.cpp.

// -------------------------------------------------------------- model mode

// Tri / Xform / scriptRot moved to client/modelmath.h (shared with GameView).

// ModelView (standalone 3DO viewer) moved to client/modelview.h + modelview.cpp.

// SoundBank + SoundClasses moved to client/sound.h (header-only verbatim move).

// Font (GAF bitmap font) moved to client/font.h + font.cpp.

// ThreadPool moved to client/threadpool.h.

// --------------------------------------------------------------- game mode

// Units simulated on a real map: left-click select, right-click move order
// (shift queues waypoints), arrows scroll, wheel zoom.
// UnitR / PlayerR / Frame render-snapshot structs moved to client/renderframe.h.

// GameView moved to client/gameview.h (+ gameview_*.cpp for method bodies).

// ReplayFile + loadReplayFile() moved to client/replayfile.h + replayfile.cpp.

// Resolve the retail data folder (ta::chooseDataFolder, shared with Cartographer):
// an explicit --data (dataRoot already set) > the folder the game was started from >
// the folder saved in config (re-validated each launch) > a native folder picker. On a
// successful pick/validate the location + an authenticity manifest are saved to config.
// `allowPrompt` gates the picker (off for headless runs, which always pass --data).
// Leaves dataRoot empty if it can't be resolved; the caller then aborts. Returns where
// the folder came from, which decides whether Options may change it (an override would
// win again after the restart that applies a change).
static ta::DataFolderOrigin resolveDataDir(std::string& dataRoot, ta::Settings& settings, bool allowPrompt) {
    namespace hpi = ta::hpi;
    const auto valid = [](const std::string& f, std::string* why) {
        return hpi::validInstall(ta::u8path(f), why);
    };
    std::string here;
    {
        std::error_code ec;
        const auto cwd = std::filesystem::current_path(ec);
        if (!ec) { const auto text = cwd.u8string(); here.assign(text.begin(), text.end()); }
    }
    const ta::DataFolderChoice choice = ta::chooseDataFolder(dataRoot, here, settings.dataDir, valid);
    switch (choice.origin) {
        // 1. Explicit --data wins. Honour it even if it doesn't validate (a dev override);
        //    if it DOES validate, remember it so a later launch needs no --data.
        case ta::DataFolderOrigin::CommandLine:
            if (!ta::sameFolder(settings.dataDir, dataRoot) && valid(dataRoot, nullptr)) {
                settings.dataDir = dataRoot;
                settings.dataManifest = hpi::rootManifest(ta::u8path(dataRoot));
                ta::saveSettings(settings);
            }
            return choice.origin;
        // 2. The local directory: drop the binary into a game folder and it just
        //    works, no --data or picker. Used for THIS run without clobbering the
        //    saved install path (so launching elsewhere later still finds it).
        case ta::DataFolderOrigin::LaunchFolder:
            dataRoot = choice.folder;
            std::fprintf(stderr, "data: using the local directory %s\n", dataRoot.c_str());
            return choice.origin;
        // 3. The saved folder, if it still holds a valid install.
        case ta::DataFolderOrigin::Saved: {
            dataRoot = choice.folder;
            std::string m = hpi::rootManifest(ta::u8path(dataRoot));   // note if the root archives changed
            if (m != settings.dataManifest) { settings.dataManifest = m; ta::saveSettings(settings); }
            return choice.origin;
        }
        case ta::DataFolderOrigin::Picker: break;
    }
    // 4. Ask (interactive only). Loop so a wrong pick can be corrected in place.
    if (!allowPrompt) return choice.origin;
    if (!ta::haveDirPicker()) {
        std::fprintf(stderr, "no folder picker available (install kdialog or zenity) and no "
                             "--data given -- cannot locate the game data\n");
        return choice.origin;
    }
    for (int tries = 0; tries < 6; ++tries) {
        std::string picked = ta::pickDirectory(ta::kDataFolderPickerTitle, settings.dataDir);
        if (picked.empty()) return choice.origin;   // cancelled
        std::string why;
        if (valid(picked, &why)) {
            dataRoot = picked;
            settings.dataDir = picked;
            settings.dataManifest = hpi::rootManifest(ta::u8path(picked));
            ta::saveSettings(settings);
            return choice.origin;
        }
        ta::errorBox("Not a game folder", ta::dataFolderInvalidMessage(why));
    }
    return choice.origin;
}

// The local replay launch: loads a .tarep into a GameView playing it back. No
// multiplayer client is attached here.
std::unique_ptr<GameView> makeReplayView(SDL_Renderer* ren, const std::string& path,
                                        const std::string& dataRoot,
                                        ta::Settings& settings, bool canReturn) {
    ReplayFile rf;
    if (!loadReplayFile(path, rf))
        throw std::runtime_error(rf.error.empty() ? "cannot read that replay" : rf.error);
    // The replay's game mods come from the verified package it recorded (the
    // digest); this viewer's own cosmetic mods go on top -- the always-enabled
    // cosmetic packs, plus the ticked packs unless it is a campaign
    // replay (they never change the gameplay hash, so the data check below holds).
    ta::hpi::Vfs::Files game;
    if(!rf.modDigest.empty()) {
        const auto cache=ta::hpi::userMapRoot();
        auto pack=cache.empty()?nullptr:ta::net::mods::loadCache(cache,rf.modDigest);
        // A single-player room always verifies a game-mod package, an empty one
        // when no game pack was ticked; that one any install can rebuild.
        if(!pack) try {
            auto empty=ta::net::mods::build(ta::u8path(dataRoot),{});
            if(empty->digest==rf.modDigest)pack=std::move(empty);
        } catch (const std::exception&) {}
        if(!pack)throw std::runtime_error("the verified game mods for this replay are missing");
        game=*pack->files;
    }
    // A campaign mission loads no ticked cosmetic packs, as the live game did.
    const bool campaignReplay=!rf.mission.empty();
    std::vector<std::string> skippedMods;
    auto rvfs = ta::hpi::mountPlayVfs(ta::u8path(dataRoot),std::move(game),
        campaignReplay?std::vector<std::string>{}:settings.cosmeticModPacks,true,&skippedMods);
    for(const auto& why:skippedMods)std::fprintf(stderr,"replay: cosmetic mod left out: %s\n",why.c_str());
    std::string mapPath;
    if (!rf.mapDigest.empty()) {
        const auto cache = ta::hpi::userMapRoot();   // never the data folder
        auto package = cache.empty() ? nullptr : ta::net::maps::loadCache(cache, rf.mapDigest);
        if (!package) try {
            auto local = ta::net::maps::build(rvfs, rf.mapId);
            if (local->digest == rf.mapDigest) package = std::move(local);
        } catch (const std::exception&) {}
        if (!package) throw std::runtime_error("the verified map copy for this replay is missing");
        try { if (!cache.empty()) ta::net::maps::saveCache(cache, *package); }
        catch (const std::exception& e) { std::fprintf(stderr,"replay: verified map cache unavailable: %s\n",e.what()); }
        mapPath = package->mapPath; rvfs.setMapFiles(package->files);
    } else if (!rf.mission.empty()) {
        mapPath = ta::sim::missionMapPath(rvfs, rf.mission);
        if (!rvfs.has(mapPath)) throw std::runtime_error("mission '" + rf.mission + "' is not in this game data");
    } else {
        mapPath = ta::hpi::findMap(rvfs, rf.mapId);
        if (mapPath.empty()) throw std::runtime_error("map '" + rf.mapId + "' is not in this game data");
    }
    const uint64_t myDataHash = ta::hpi::gameplayHash(rvfs);
    if (rf.dataHash != myDataHash) {
        std::fprintf(stderr, "replay: WARNING -- recorded on gameplay data %016llx, yours is %016llx. Playback will diverge from the recording.\n",
                     (unsigned long long)rf.dataHash, (unsigned long long)myDataHash);
    }
    auto view = std::make_unique<GameView>(ren, std::move(rvfs), mapPath, dataRoot,
        ta::hpi::ModUse{}, false, false, true, "arm", "cor");
    view->applySettings(settings); view->setSettings(&settings);
    // One simulation: it gets them all, or its CCD's (PIN TO CCDS).
    if (const auto ccd = ta::client::planCcds(settings, false); ccd.on && ta::devFlag("TA_SIM_PIN", true)) {
        ta::client::applyGameCcd(ccd);
        view->latchSimThreads(int(ccd.gameThreads), false, false);
    } else {
        ta::client::applyGameCcd({});
        view->latchSimThreads(settings.simThreads, false, settings.simPin && ta::devFlag("TA_SIM_PIN", true));
    }
    if (canReturn) view->setCanReturnToMenu();
    std::fprintf(stderr, "replay: %s -- map '%s', %zu ticks (format %u, recorded by %s, %zu hash checkpoints)\n",
                 path.c_str(), rf.mapId.c_str(), rf.bundles.size(),
                 rf.formatVersion, rf.engineVersion.c_str(), rf.checks.size());
    view->setReplayChecks(std::move(rf.checks));
#ifndef NDEBUG
    // TA_SITUATION=<tick>:<path>: write the world at <tick> and the next 600 ticks of commands as a .scn
    // situation (src/client/situation.h). Debug builds only, like every TA_* hook.
    if (const auto req = ta::situation::parseRequest(ta::devEnv("TA_SITUATION")); req.valid) {
        ta::situation::Meta meta;
        meta.mapId = rf.mapId;
        meta.players = int(rf.cfg.slots.size());
        meta.seed = rf.cfg.startSeed;
        meta.source = std::filesystem::path(path).filename().string() + " (engine " + rf.engineVersion + ")";
        view->armSituation(req, meta);
    }
#endif
    view->startReplay(rf.cfg, std::move(rf.bundles), rf.mission);
    return view;
}

} // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();   // we defined SDL_MAIN_HANDLED; tell SDL our main is ready
    ta::sim::launchCpus();   // the CPUs this game may use, read before PIN TO CCDS changes them
    if (argc >= 2 && (!std::strcmp(argv[1], "--version") || !std::strcmp(argv[1], "-v"))) {
        std::printf("taclient (TA engine) %s (build %s)\n", ta::kVersion, ta::kBuildId);
        return 0;
    }
    if (argc >= 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h"))) {
        std::printf(
#ifdef NDEBUG
            "usage: taclient --data <retail-install-dir>\n"
            "  Launches the game and its front-end menu.\n"
            "  --play-map <snapshot.kmp>   open a Cartographer snapshot in a private lobby\n"
            "  --game-mod <pack>   with --play-map: tick only this mods/game pack in that room\n"
            "                      (mods/game/always_enabled packs apply as well)\n"
            "  --version, -v   print version and exit\n"
            "  <retail-install-dir> holds the shipped *.hpi plus Maps/ Music/ mods/.\n");
#else
            "usage: taclient [mode] --data <retail-install-dir> [options]\n"
            "  With no mode (or only flags), launches the front-end MENU.\n"
            "  modes: menu | game <map> | map <map> | replay <file.tarep> | model <file.3do>\n"
            "    game single-player: no --server -> auto-hosts a private game and opens its room\n"
            "      (seat AIs, READY, START by hand; TA_SP_AIS=N seats N AIs and starts at once).\n"
            "    game multiplayer:   add --server host [--serverport N] [--name X].\n"
            "    game --campaign <stem>: play a campaign mission (e.g. takmission01_mt).\n"
            "    game <stem> --mission: alias for the same campaign launch.\n"
            "  common: [--side X --aiside Y] [--mods none|cosmetic|game|all] [--shot out.png]\n"
            "  (debug build: all dev/test flags below are available.)\n"
            "  <retail-install-dir> holds the shipped *.hpi plus Maps/ Music/ mods/.\n");
#endif
        return 0;
    }
#ifdef NDEBUG
    // Hardened release CLI: data folder and the documented Cartographer handoff only.
    // Every gameplay/dev/test flag and every TA_* env var is debug-only, so a shipped
    // build has no hidden switches -- the game is configured through the menu + Options.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") { std::printf("taclient (TA engine) %s\n", ta::kVersion); return 0; }
        if (a == "--help" || a == "-h") { std::printf("usage: taclient --data <retail-install-dir>\n"); return 0; }
        if ((a == "--data" || a == "--play-map" || a == "--game-mod") && i + 1 < argc) { ++i; continue; }   // its value is consumed by the parser below
        std::fprintf(stderr,
            "taclient: unknown option '%s' -- release builds accept --data, --play-map, --game-mod, --help and --version.\n", a.c_str());
        return 2;
    }
#endif
    // The first positional arg is the launch mode only if it's a known keyword;
    // otherwise the default is the front-end menu, so `taclient --data <dir>` (or even
    // bare `taclient`) just opens it -- no need to type "menu".
    std::string mode = "menu";
    int argStart = 1;
    if (argc >= 2) {
        std::string a1 = argv[1];
        if (a1 == "menu" || a1 == "game" || a1 == "map" || a1 == "replay" || a1 == "model") {
            mode = a1;
            argStart = 2;
        }
    }
    std::string shot, cobPath, anim, side = "arm", aiSide = "cor";
    // model mode: --statics <bitmask> seeds the VM's static slots (bit i -> static i).
    // Walk/attack scripts gate on an "am I moving" static whose SLOT differs per unit.
    uint32_t staticMask = 1;
    std::string serverHost, playerName, dataRoot, modsArg, playMap;
    std::string playGameMod;   // --game-mod <pack>: Cartographer's TEST IN GAME ticks this pack (with --play-map)
    // Multiplayer account (menu-entered, or --user/--pass for the harnesses). The
    // password is used once and wiped; it is never written to settings.
    std::string loginUser, loginPass;
    int serverPort = 7677, mpHeadless = 0;
    std::string missionStem;   // --mpmission <stem>: headless campaign-mission host
    std::string cliCampaign;   // --campaign <stem>: launch straight into a mission (interactive)
    int winW = kWinW, winH = kWinH, maxFps = 60;
    bool winSizeExplicit = false;
    int playerColor = -1, aiColor = -1;   // --color / --aicolor slot overrides
    float startTime = 0, followZoom = 0;
    bool demo = false, trace = false,
         scenario = false, navy = false, amphib = false, missionFlag = false,
         nofog = false, doLook = false,
         keytest = false, selonly = false;
    std::string lodeUnitName;
    bool firetest = false, facetest = false, noVsync = false;
    // Debug/test harness flags (--march, --testbuild, --soundtest, ...): set from
    // argv but read only inside the #ifndef NDEBUG blocks below, so they are unused
    // in release builds.
    [[maybe_unused]] float marchX = 0, marchZ = 0;
    [[maybe_unused]] bool doMarch = false, testbuild = false, misstest = false,
        creon = false, guardtest = false, lodetest = false,
        soundtest = false;
    float lookX = 0, lookZ = 0;
    std::vector<std::string> args;
    for (int i = argStart; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--play-map" && i + 1 < argc) { playMap = argv[++i]; mode = "game"; }
        else if (a == "--game-mod" && i + 1 < argc) playGameMod = argv[++i];
        else if (a == "--shot" && i + 1 < argc) shot = argv[++i];
        else if (a == "--cob" && i + 1 < argc) cobPath = argv[++i];
        else if (a == "--anim" && i + 1 < argc) anim = argv[++i];
        else if (a == "--statics" && i + 1 < argc) staticMask = uint32_t(std::stoul(argv[++i]));
        else if (a == "--time" && i + 1 < argc) startTime = std::stof(argv[++i]);
        else if (a == "--demo") demo = true;
        else if (a == "--trace") trace = true;
        else if (a == "--testbuild") testbuild = true;
        else if (a == "--scenario") scenario = true;
        else if (a == "--navy") navy = true;
        else if (a == "--amphib") amphib = true;
        else if (a == "--mission") missionFlag = true;
        else if (a == "--misstest") misstest = true;
        else if (a == "--creon") creon = true;
        else if (a == "--side" && i + 1 < argc) side = argv[++i];
        else if (a == "--aiside" && i + 1 < argc) aiSide = argv[++i];
        else if (a == "--color" && i + 1 < argc) playerColor = std::atoi(argv[++i]);
        else if (a == "--aicolor" && i + 1 < argc) aiColor = std::atoi(argv[++i]);
        else if (a == "--keytest") keytest = true;
        else if (a == "--guardtest") guardtest = true;
        else if (a == "--lodetest") lodetest = true;
        else if (a == "--firetest") firetest = true;
        else if (a == "--facetest") facetest = true;
        else if (a == "--soundtest") soundtest = true;

        else if (a == "--winsize" && i + 2 < argc) {
            winW = std::atoi(argv[++i]);
            winH = std::atoi(argv[++i]);
            winSizeExplicit = true;
        }
        else if (a == "--maxfps" && i + 1 < argc) maxFps = std::atoi(argv[++i]);
        else if (a == "--novsync") noVsync = true;


        else if (a == "--lodeunit" && i + 1 < argc) lodeUnitName = argv[++i];
        else if (a == "--selonly") selonly = true;

        else if (a == "--data" && i + 1 < argc) dataRoot = argv[++i];
        else if (a == "--mods" && i + 1 < argc) modsArg = argv[++i];
        else if (a == "--server" && i + 1 < argc) serverHost = argv[++i];
        else if (a == "--serverport" && i + 1 < argc) serverPort = std::atoi(argv[++i]);
        else if (a == "--name" && i + 1 < argc) playerName = argv[++i];
        // Sign in without the menu, for the headless harnesses. A server run with
        // --no-auth (single-player, LAN) never asks, so these stay empty there.
        else if (a == "--user" && i + 1 < argc) loginUser = argv[++i];
        else if (a == "--pass" && i + 1 < argc) loginPass = argv[++i];
        // Headless multiplayer test drivers (auto-play through the server).
        else if (a == "--mphost") mpHeadless = 1;   // create a game, start it, play
        else if (a == "--mpjoin") mpHeadless = 2;   // join the first game, play
        else if (a == "--mpai") mpHeadless = 4;     // host vs one server-run AI
        else if (a == "--mpspectate") mpHeadless = 6; // watch the first running game
        else if (a == "--mpmission" && i + 1 < argc) { mpHeadless = 8; missionStem = argv[++i]; }  // host a campaign mission
        else if (a == "--campaign" && i + 1 < argc) { cliCampaign = argv[++i]; mode = "game"; }    // play a mission interactively
        else if (a == "--nofog") nofog = true;
        else if (a == "--cheat") ta::sim::gInstantBuild = true;
        else if (a == "--look" && i + 2 < argc) {
            lookX = std::stof(argv[++i]);
            lookZ = std::stof(argv[++i]);
            doLook = true;
        }
        else if (a == "--follow" && i + 1 < argc) followZoom = std::stof(argv[++i]);
        else if (a == "--march" && i + 2 < argc) {
            marchX = std::stof(argv[++i]);
            marchZ = std::stof(argv[++i]);
            doMarch = true;
        }
        else args.push_back(a);
    }
    // --mission is an alias for --campaign <stem>. Every campaign launch shares
    // the menu's authoritative server setup and in-sim mission interpreter.
    if (missionFlag) {
        if (cliCampaign.empty()) {
            if (args.empty()) {
                std::fprintf(stderr,"--mission requires a mission stem (or use --campaign <stem>)\n");
                return 1;
            }
            cliCampaign=ta::vpath::stem(args.front());
            std::transform(cliCampaign.begin(),cliCampaign.end(),cliCampaign.begin(),::tolower);
            args.clear();
        }
        mode="game";
    }
    if (!shot.empty() || mpHeadless) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");

    // The runtime data set: a retail install directory (root *.hpi + Maps/ +
    // Music/ + mods/). This is the ONLY way the engine reads game files. It
    // must outlive the views (GameView/MapView hold a reference), so it lives here
    // at function scope for the whole render loop, and is built BEFORE connecting
    // so the Hello can carry this install's gameplay-data fingerprint.
    // --mods (debug): which kinds of ticked mod pack this launch may mount.
    // all (default) = game + cosmetic, game, cosmetic, or none (pure retail).
    ta::hpi::ModUse modUse;
    if (modsArg == "none") modUse = {false, false};
    else if (modsArg == "cosmetic") modUse = {false, true};
    else if (modsArg == "game") modUse = {true, false};
    else if (!modsArg.empty() && modsArg != "all") {
        std::fprintf(stderr, "--mods: expected none|cosmetic|game|all, got '%s'\n", modsArg.c_str());
        return 2;
    }
    ta::hpi::Vfs vfs;   // mounted AFTER the data folder is resolved (below, post-settings)

    // The engine is client-server only: every real game runs on a server, and AIs
    // run ONLY on the server. Local dev/test harnesses (which free-run the sim with
    // no server) are DEBUG-only. A release build has none of them.
    bool localHarness = false;
#ifndef NDEBUG
    localHarness = demo || scenario || navy || amphib || firetest ||
                   facetest || guardtest || lodetest || keytest ||
                   soundtest || misstest || creon || testbuild ||
                   (ta::devEnv("TA_FFA") != nullptr) || ta::devFlag("TA_PATROL_PERF");
#endif
    if (missionFlag && localHarness) {
        std::fprintf(stderr,"--mission cannot be combined with standalone scenario/test harness flags\n");
        return 1;
    }
    // Create the window + renderer up front so the front-end menu can drive the
    // single-player / multiplayer setup that follows it.
    if (shot.empty()) SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    // Own SIGTERM/SIGINT ourselves: SDL would turn them into an SDL_QUIT event, which the
    // menu ignores (quit via the Exit door), so a kill/Ctrl-C would otherwise wedge a
    // headless run. Our handler sets a flag every event loop polls (see appquit.h).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#ifdef _WIN32
    // Opt out of Windows display scaling BEFORE video init (SDL reads the hint then):
    // per-monitor-v2 awareness makes window == pixels, so nothing is bitmap-stretched.
    // SDL's own high-DPI window scaling stays off (no SDL_WINDOW_ALLOW_HIGHDPI), so
    // window coords, mouse events and renderer output are all physical pixels.
    // The setting file is read directly here, ahead of SDL_Init.
    if (mpHeadless == 0 && shot.empty() && ta::loadSettings().ignoreDpiScaling) {
        SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
        ta::dpiIgnoredAtStartup() = true;
    }
#elif defined(__APPLE__)
    // macOS equivalent: a Retina window opts into its full backing resolution with
    // SDL_WINDOW_ALLOW_HIGHDPI (below; Info.plist already says NSHighResolutionCapable).
    // Without it the backbuffer is in points and the OS doubles it, a blurry stretch.
    // The window then speaks points and the renderer pixels -- see client/hidpi.h.
    if (mpHeadless == 0 && shot.empty() && ta::loadSettings().ignoreDpiScaling)
        ta::dpiIgnoredAtStartup() = true;
#elif defined(__linux__)
    bool waylandAppIdSet = false;   // SDL_Init reads it; unset after, so child processes do not inherit it
    // Linux: SDL2 prefers X11, which under a Wayland desktop is XWayland -- stretched
    // (blurry) at a display scale above 100% on most compositors. NATIVE WAYLAND asks for
    // a Wayland window instead (real pixels, SDL_WINDOW_ALLOW_HIGHDPI below); SDL falls
    // back to X11 by itself if Wayland cannot start. See client/waylandchoice.h.
    if (mpHeadless == 0 && shot.empty()) {
        auto& wl = ta::wayland::startup();
        wl.setting = ta::loadSettings().nativeWayland;
        wl.session = ta::wayland::probeSession();
        wl.requested = ta::wayland::wantNative(wl.setting, wl.session);
        if (wl.requested) {
            SDL_SetHint(SDL_HINT_VIDEODRIVER, "wayland,x11");
            // Wayland matches a window to its .desktop launcher (and icon) by app id.
            if (!std::getenv("SDL_VIDEO_WAYLAND_WMCLASS")) {
                setenv("SDL_VIDEO_WAYLAND_WMCLASS", "ta-client", 0);
                waylandAppIdSet = true;
            }
        }
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
#if defined(__linux__)
    if (waylandAppIdSet) unsetenv("SDL_VIDEO_WAYLAND_WMCLASS");
    {
        auto& wl = ta::wayland::startup();
        const char* drv = SDL_GetCurrentVideoDriver();
        wl.native = wl.requested && drv && std::strcmp(drv, "wayland") == 0;
        if (wl.requested || wl.setting != ta::wayland::kAuto)
            std::fprintf(stderr, "video: native wayland %s (setting %s), driver %s\n",
                         wl.native ? "on" : wl.requested ? "unavailable -- using X11" : "off",
                         ta::wayland::settingLabel(wl.setting), drv ? drv : "?");
    }
#endif
    ta::installSignalHandlers();
    // Persisted Options (audio/camera/display prefs). CLI flags still win where they
    // apply; the file is the source of truth for anything not passed on the CLI. No file
    // yet is a fresh profile: the first-run rule below sizes the HUD and cursor once.
    bool settingsFound = false;
    ta::Settings settings = ta::loadSettings(&settingsFound);
    const bool freshProfile = !settingsFound;
    ta::music::setCustom(settings.customMusicTracks);   // CUSTOM MUSIC, from the first note
    // Finish any replay a crash, kill or power cut left mid-recording, so it is
    // listed and playable like any other (net/replaywriter.h). Recordings still
    // live in another running client are locked and skipped.
    ta::net::recoverReplayParts(ta::replayDir());
    // Map and game-mod downloads, Test Map copies and saved generated maps live
    // beside settings.ini, never in the game data folder (hpi::setUserMapRoot).
    ta::hpi::setUserMapRoot(ta::u8path(ta::replayDir()));
#ifdef _WIN32
    if (ta::dpiIgnoredAtStartup()) {
        // Real pixels: a fresh profile starts its HUD and cursor at the display's scale.
        float ddpi = 0;
        float osScale = SDL_GetDisplayDPI(0, &ddpi, nullptr, nullptr) == 0 && ddpi > 0 ? ddpi / 96.0f : 1.0f;
        std::fprintf(stderr, "dpi: ignoring Windows display scaling; primary display scale %.2f (%.0f dpi)\n", osScale, ddpi);
        if (ta::applyFirstRunScale(settings, freshProfile, osScale)) ta::saveSettings(settings);
    }
#endif
    // Select the saved output device before ANY audio opens (validated -- an absent
    // device falls back to system default). Keep the setting so the picker still shows
    // the user's choice even if it's currently unplugged.
    // setAudioDevice() calls initAudioCaps() internally, which snapshots each device's true
    // channel layout NOW -- before the menu music / door videos open a stereo stream that
    // would collapse the 5.1 sink's advertised layout to 2 -- and inits the audio subsystem
    // so the saved device actually validates (SDL_Init above is video-only).
    ta::setAudioDevice(settings.audioDevice);
    // Locate the retail data folder now that SDL (message boxes) and the config are up:
    // explicit --data, else the saved folder, else a native picker. Headless/harness runs
    // always pass --data, so they never prompt. Then mount it (must precede menu/game use).
    ta::DataFolderOrigin dataOrigin = ta::DataFolderOrigin::CommandLine;
    if (!dataRoot.empty() || !localHarness) {
        dataOrigin = resolveDataDir(dataRoot, settings, /*allowPrompt=*/mpHeadless == 0);
        if (dataRoot.empty()) {
            std::fprintf(stderr, "no Total Annihilation data folder selected -- exiting\n");
            SDL_Quit();
            return 1;
        }
        vfs = ta::hpi::mountRetailRoot(ta::u8path(dataRoot));
        gInstallRoot = dataRoot;   // the loading screen reads Movies/Gui from here
    }
    std::shared_ptr<ta::ScenarioTraceLog> playtestTrace;
    if (!playMap.empty()) {
        try {
            const auto snapshot = ta::net::maps::importSnapshot(vfs, ta::u8path(playMap));
            const auto& ota=snapshot->files->at(ta::vpath::replaceExtension(snapshot->mapPath,".ota"));
            const auto metadata=ta::tdf::parseText(std::string(ota.begin(),ota.end()));
            const auto* test=metadata.child("taplaytest");
            if(test && test->numberOr("trace",0)!=0) {
                const std::string folder=ta::prefPath("Cartographer");
                if(folder.empty())throw std::runtime_error("Cannot locate the trigger log folder");
                playtestTrace=std::make_shared<ta::ScenarioTraceLog>(ta::scenarioTracePath(
                    ta::u8path(folder),ta::u8path(playMap)));
            }
            // The verified copy goes in the per-user MapCache, where this client's
            // private server (--map-cache-dir) finds it too; the data folder stays
            // untouched.
            const auto cache=ta::hpi::userMapRoot();
            if(cache.empty())throw std::runtime_error("Cannot locate the per-user map folder");
            ta::net::maps::saveCache(cache, *snapshot);
            vfs.refreshMapCache(cache);
            // MapCache exposes a digest-qualified picker name, not the source
            // archive's internal path. Select that exact revision.
            args = {ta::vpath::stem(snapshot->mapPath) + " [" + snapshot->digest.substr(0,16) + "]"};
            if(ta::hpi::findMap(vfs,args.front()).empty())throw std::runtime_error("Verified test map is missing from the map catalog");
        } catch (const std::exception& e) {
            ta::errorBox("Cannot test map", e.what());
            std::fprintf(stderr, "Test Map: %s\n", e.what()); SDL_Quit(); return 1;
        }
    }
    if (!playGameMod.empty()) {
        // --game-mod: the test room's ticked game mods are exactly this pack, for this run
        // only (ta::setLaunchGameModPacks keeps the player's saved ticks); the
        // always-enabled game packs apply too, as in every room this player hosts. Part
        // of the --play-map handoff: Cartographer's TEST IN GAME for an AI mod.
        std::string why;
        bool alreadyAlways = false;
        if (playMap.empty()) why = "--game-mod is only used with --play-map";
        else if (!modUse.game) why = "--mods leaves game mods out of this launch";
        else {
            const auto root = ta::u8path(dataRoot);
            const auto packs = ta::hpi::modPacks(root, ta::hpi::ModKind::Game);
            const auto always = ta::hpi::alwaysEnabledPacks(root, ta::hpi::ModKind::Game);
            if (std::find(always.begin(), always.end(), playGameMod) != always.end()) alreadyAlways = true;
            else if (std::find(packs.begin(), packs.end(), playGameMod) == packs.end())
                why = "There is no game mod \"" + playGameMod + "\" in " + dataRoot + "/mods/game or " +
                      dataRoot + "/mods/game/always_enabled";
        }
        if (!why.empty()) {
            ta::errorBox("Cannot test game mod", why);
            std::fprintf(stderr, "Test in game: %s\n", why.c_str()); SDL_Quit(); return 1;
        }
        // --play-map opens a single-player room: the single-player set holds the run's packs.
        settings.spRoom.gamePacks = alreadyAlways ? std::vector<std::string>{} : std::vector<std::string>{playGameMod};
        ta::setLaunchGameModPacks(settings.spRoom.gamePacks);
        if (alreadyAlways)
            std::fprintf(stderr, "Test in game: game mod %s is already always enabled; no pack ticked for this run\n", playGameMod.c_str());
        else
            std::fprintf(stderr, "Test in game: game mod %s ticked for this run\n", playGameMod.c_str());
    }
    if (maxFps != 60) settings.maxFps = maxFps;          // --maxfps (if given) wins the file
    bool vsyncOn = settings.vsync && !noVsync;            // --novsync forces off
    std::string winTitle = std::string("Total Annihilation ") + ta::kVersion;
    SDL_Window* win = SDL_CreateWindow(winTitle.c_str(), SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, winW, winH,
                                       SDL_WINDOW_RESIZABLE
#ifdef __APPLE__
                                       | (ta::dpiIgnoredAtStartup() ? SDL_WINDOW_ALLOW_HIGHDPI : 0)
#elif defined(__linux__)
                                       | (ta::wayland::startup().native ? SDL_WINDOW_ALLOW_HIGHDPI : 0)
#endif
                                       );
    {   // Application icon: the crown badge (src/util/appicon).
        std::vector<uint8_t> ic = ta::appicon::render(ta::appicon::Kind::Client, 64);
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
                ic.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32)) {
            SDL_SetWindowIcon(win, s);
            SDL_FreeSurface(s);
        }
    }
    if (win && settings.fullscreen)
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    // SDL starts text input with the video subsystem. Every text field here starts it
    // when it takes focus and stops it when it lets go, so stop it now: then "is a text
    // field focused?" is SDL_IsTextInputActive(), which push to talk relies on (Space
    // typed into a field is text, never voice).
    SDL_StopTextInput();
    Uint32 renFlags = SDL_RENDERER_SOFTWARE;
    if (shot.empty()) renFlags = noVsync ? SDL_RENDERER_ACCELERATED
                                         : SDL_RENDERER_PRESENTVSYNC;
#ifndef NDEBUG
    if(testbuild && ta::devFlag("TA_RENDER_STUDY") && SDL_GetCurrentVideoDriver() &&
       std::strcmp(SDL_GetCurrentVideoDriver(),"dummy")==0)renFlags=SDL_RENDERER_SOFTWARE;
#endif
    // Options -> RENDERER. A chosen driver, or AUTO's ordered try on Windows, is applied
    // only to a real window launch: the screenshot/test and dummy-driver paths above keep
    // their own flags untouched. If the choice is missing from this SDL build or fails to
    // create, this launch falls back to AUTO, logs it, and the menu shows a one-time
    // notice. The saved choice is kept (no implicit write): it is re-checked every launch,
    // and a transient failure does not silently lose it. See docs/user-guide.md.
    const char* vidDrv = SDL_GetCurrentVideoDriver();
    const bool applyRenderer = shot.empty() && !(vidDrv && std::strcmp(vidDrv, "dummy") == 0);
    auto& renState = ta::renderdriver::active();
    SDL_Renderer* ren = nullptr;
    // One named attempt. SDL turns command batching OFF for a renderer picked by name (it
    // assumes the app will mix in its own API calls). Auto keeps it on, and so must a
    // chosen driver: without it OpenGL measured ~20% slower per frame here.
#ifndef NDEBUG
    // TA_RENDER_FAIL=<id>[,<id>...]: those drivers "fail to create" (fallback tests).
    // Whole ids: "direct3d11" must not also fail "direct3d".
    const auto renderFailHook = [](std::string_view id) {
        const char* f = ta::devEnv("TA_RENDER_FAIL");
        for (std::string_view rest = f ? f : ""; !rest.empty();) {
            const size_t comma = rest.find(',');
            if (ta::renderdriver::sameId(rest.substr(0, comma), id)) return true;
            rest = comma == std::string_view::npos ? std::string_view() : rest.substr(comma + 1);
        }
        return false;
    };
#endif
    const auto createNamed = [&](const std::string& id, Uint32 flags) -> SDL_Renderer* {
#ifndef NDEBUG
        if (renderFailHook(id)) {
            SDL_SetError("TA_RENDER_FAIL");
            return nullptr;
        }
#endif
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, id.c_str(), SDL_HINT_OVERRIDE);
        SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
        SDL_Renderer* r = SDL_CreateRenderer(win, -1, flags);
        SDL_ResetHint(SDL_HINT_RENDER_BATCHING);
        SDL_ResetHint(SDL_HINT_RENDER_DRIVER);   // back to SDL's own (env) value
        return r;
    };
    const auto createdName = [](SDL_Renderer* r) -> std::string {
        SDL_RendererInfo ri{};
        return r && SDL_GetRendererInfo(r, &ri) == 0 && ri.name ? std::string(ri.name) : std::string();
    };
    const auto dropRenderer = [&](SDL_Renderer*& r) {
        if (!r) return;
        ta::releaseUiTextGlyphs();
        SDL_DestroyRenderer(r);
        r = nullptr;
    };
    if (applyRenderer && !settings.renderer.empty()) {
        ta::renderdriver::Plan rp =
            ta::renderdriver::plan(settings.renderer, ta::renderdriver::available(), renFlags);
        if (!rp.fallback) {
            ren = createNamed(rp.hint, rp.flags);
            const std::string got = createdName(ren);
            if (!ta::renderdriver::honoured(rp.hint, ren ? got.c_str() : nullptr)) {
                rp.fallback = true;
                rp.reason = ren ? "SDL created '" + got + "' instead"
                                : std::string("creation failed: ") + SDL_GetError();
                dropRenderer(ren);
            } else {
                renState.inEffect = rp.hint;
            }
        }
        if (rp.fallback) {
            std::fprintf(stderr, "renderer: %s unavailable (%s) -- using Auto\n",
                         ta::renderdriver::label(settings.renderer).c_str(), rp.reason.c_str());
            renState.failed = settings.renderer;
        }
    }
    // AUTO's ordered try (Windows and macOS; empty on Linux). An SDL_RENDER_DRIVER set in the
    // environment is SDL's own override and wins, as it always has.
    const char* envDriver = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    if (applyRenderer && !ren && !(envDriver && *envDriver)) {
#if defined(_WIN32)
        constexpr auto kAutoPlatform = ta::renderdriver::Platform::Windows;
        constexpr const char* kAutoOrderName = "Windows";
#elif defined(__APPLE__)
        constexpr auto kAutoPlatform = ta::renderdriver::Platform::MacOS;
        constexpr const char* kAutoOrderName = "macOS";
#else
        constexpr auto kAutoPlatform = ta::renderdriver::Platform::Other;
        constexpr const char* kAutoOrderName = "";
#endif
        const Uint32 autoFlags = renFlags & ~Uint32(SDL_RENDERER_SOFTWARE);
        // GL_VENDOR / GL_RENDERER of the current context.
        const auto glStrings = [](std::string& vendor, std::string& glRen) {
            using GetString = const GLubyte*(APIENTRY*)(GLenum);
            if (auto get = reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"))) {
                const auto str = [&](GLenum e) {
                    const auto* v = reinterpret_cast<const char*>(get(e));
                    return std::string(v ? v : "");
                };
                vendor = str(GL_VENDOR);
                glRen = str(GL_RENDERER);
            }
#ifndef NDEBUG
            // TA_RENDER_FAKE_GL=<GL_RENDERER>[|<GL_VENDOR>]: pretend to be that GL.
            if (const char* fake = ta::devEnv("TA_RENDER_FAKE_GL"); fake && *fake) {
                const std::string f = fake;
                const size_t bar = f.find('|');
                glRen = f.substr(0, bar);
                vendor = bar == std::string::npos ? std::string("Microsoft Corporation") : f.substr(bar + 1);
            }
#endif
        };
        // OpenGL is judged on a hidden 1x1 window BEFORE it touches the game's window: a
        // GL renderer made and destroyed on the window can leave it unusable for Direct3D
        // 11 (seen with Mesa's OpenGL-over-Direct3D 12, which keeps a swap chain on it).
        // The probe first takes its own hold on the OpenGL library and never lets go:
        // destroying the probe window would otherwise unload it, and unloading Mesa's
        // opengl32.dll mid-run hung in FreeLibrary on the test machine.
        const auto probeGl = [&](std::string& vendor, std::string& glRen) -> std::string {
            if (SDL_GL_LoadLibrary(nullptr) != 0) return std::string("creation failed: ") + SDL_GetError();
            SDL_Window* pw = SDL_CreateWindow("", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 1, 1,
                                              SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
            if (!pw) return std::string("creation failed: ") + SDL_GetError();
            SDL_GLContext ctx = SDL_GL_CreateContext(pw);
            std::string err;
            if (!ctx) err = std::string("creation failed: ") + SDL_GetError();
            else {
                glStrings(vendor, glRen);
                SDL_GL_MakeCurrent(pw, nullptr);
                SDL_GL_DeleteContext(ctx);
            }
            SDL_DestroyWindow(pw);
            return err;
        };
        for (const std::string& id :
             ta::renderdriver::autoOrder(kAutoPlatform, ta::renderdriver::available(), renState.failed)) {
            std::string why;
            const bool gl = id.compare(0, 6, "opengl") == 0;
            if (gl) {
#ifndef NDEBUG
                if (renderFailHook(id)) why = "creation failed: TA_RENDER_FAIL";
#endif
                std::string vendor, glRen;
                if (why.empty()) why = probeGl(vendor, glRen);
                if (why.empty()) {
                    std::fprintf(stderr, "renderer: auto: OpenGL is \"%s\" from \"%s\"\n",
                                 glRen.c_str(), vendor.c_str());
                    if (const auto v = ta::renderdriver::judge(id, true, vendor, glRen); !v.keep) why = v.reason;
                }
            }
            if (why.empty()) {
                ren = createNamed(id, autoFlags);
                if (!ren) {
                    why = std::string("creation failed: ") + SDL_GetError();
                } else {
                    SDL_RendererInfo ri{};
                    SDL_GetRendererInfo(ren, &ri);
                    const std::string got = ri.name ? ri.name : "";
                    // The renderer's own context (current right after creation) is judged
                    // again, with SDL's accelerated flag.
                    std::string vendor, glRen;
                    if (got.compare(0, 6, "opengl") == 0) glStrings(vendor, glRen);
                    const auto verdict = ta::renderdriver::judge(
                        got, (ri.flags & SDL_RENDERER_ACCELERATED) != 0, vendor, glRen);
                    if (!ta::renderdriver::honoured(id, got.c_str()))
                        why = "SDL created '" + got + "' instead";
                    else if (!verdict.keep)
                        why = verdict.reason;
                    if (!why.empty()) dropRenderer(ren);
                }
            }
            if (ren) {
                const std::string first = std::string(" (first in the ") + kAutoOrderName + " order";
                std::fprintf(stderr, "renderer: auto chose %s%s\n",
                             ta::renderdriver::label(id).c_str(),
                             !renState.autoNote.empty() ? (" after skipping " + renState.autoNote).c_str()
                             : renState.failed.empty()  ? (first + ")").c_str()
                                                        : (first + " after the saved choice)").c_str());
                break;
            }
            std::fprintf(stderr, "renderer: auto: skipping %s: %s\n",
                         ta::renderdriver::label(id).c_str(), why.c_str());
            if (!renState.autoNote.empty()) renState.autoNote += "; ";
            renState.autoNote += ta::renderdriver::label(id) + " (" + why + ")";
        }
        if (!ren && !renState.autoNote.empty())
            std::fprintf(stderr, "renderer: auto: no candidate kept -- SDL's own pick\n");
    }
    if (!ren) ren = SDL_CreateRenderer(win, -1, renFlags);
    if (!ren) {
        std::fprintf(stderr, "renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    if (shot.empty()) SDL_RenderSetVSync(ren, vsyncOn ? 1 : 0);
    // Name the backend. A profile is only as meaningful as the renderer that
    // produced it: under a headless/dummy video driver SDL falls back to its
    // SOFTWARE rasteriser, where submit and present are CPU rasterisation and say
    // nothing about a real GPU. Measuring there sent me to the wrong conclusion
    // once already.
#ifdef __APPLE__
    if (ta::dpiIgnoredAtStartup()) {
        // The same first-run rule as Windows. macOS has no DPI number to ask for before
        // the window exists; the backing scale is drawable / window.
        const float osScale = ta::hidpi::scale(ren);
        std::fprintf(stderr, "dpi: native Retina resolution; backing scale %.2f\n", osScale);
        if (ta::applyFirstRunScale(settings, freshProfile, osScale)) {
            // Save just these into the file, not this run's command-line overrides.
            ta::Settings disk = ta::loadSettings();
            disk.uiScale = settings.uiScale;
            disk.cursorScale = settings.cursorScale;
            ta::saveSettings(disk);
        }
    }
#elif defined(__linux__)
    if (mpHeadless == 0 && shot.empty()) {
        // Keep UI Scale / Cursor Size the same physical size when the window kind changes
        // (NATIVE WAYLAND switched, or KDE's legacy-X11 scaling changed): a stretched X11
        // window measured them in points of S screen pixels, a real-pixel X11 window and a
        // native Wayland one in screen pixels -- see client/waylandchoice.h.
        namespace wlc = ta::wayland;
        const auto& wl = wlc::startup();
        const char* drv = SDL_GetCurrentVideoDriver();
        wlc::Observation now;
        now.mode = wlc::sessionMode(drv ? drv : "", wl.native, wl.session);
        if (now.mode == wlc::kModeWayland) {
            // The scale arrives from the compositor once the surface is on an output: wait
            // for it rather than read an unsettled 1 (client/waylandmeasure.h).
            const Uint32 t0 = SDL_GetTicks();
            now.scale = wlc::settledNativeScale(win, ren);
            now.logicalWidth = wlc::desktopWidth();
            std::fprintf(stderr, "dpi: native Wayland resolution; backing scale %.2f%s after %u ms, logical desktop %d\n",
                         now.scale, now.scale > 0.0f ? "" : " (not settled -- sizes left for the next start)",
                         unsigned(SDL_GetTicks() - t0), now.logicalWidth);
        } else if (now.mode == wlc::kModeX11) {
            now.x11Width = wlc::desktopWidth();
        }
        // First run: a fresh profile in a native window starts at the compositor's scale.
        const bool firstRun = ta::applyFirstRunScale(settings, freshProfile, wlc::firstRunScale(now));
        if (firstRun)
            std::fprintf(stderr, "dpi: fresh profile: UI Scale and Cursor Size at the display scale -> uiScale=%.2f cursorScale=%d\n",
                         settings.uiScale, settings.cursorScale);
        wlc::DisplayHistory h{settings.displayMode, settings.displayScale, settings.displayX11Width,
                              settings.displayLogicalWidth};
        int kinds[2] = {0, 0};
        const float f = wlc::advanceDisplay(h, now, wlc::Units::GamePixels, wlc::fallbackX11Kind(wl.session), &kinds);
        if (now.mode != wlc::kModeUnknown) {
            const bool guessed = kinds[0] == wlc::kModeX11 || kinds[1] == wlc::kModeX11;
            std::fprintf(stderr, "dpi: X11 desktop %d, logical %d, scale %.2f: last window %s, this one %s%s%s%s\n",
                         h.x11Width, h.logicalWidth, h.scale, wlc::modeName(kinds[0]), wlc::modeName(kinds[1]),
                         guessed ? " (x11 = not measured, counted as " : "",
                         guessed ? wlc::modeName(wlc::fallbackX11Kind(wl.session)) : "", guessed ? ")" : "");
        }
        const bool resized = ta::rescaleUi(settings, f);
        if (firstRun || resized || h.mode != settings.displayMode || h.scale != settings.displayScale ||
            h.x11Width != settings.displayX11Width || h.logicalWidth != settings.displayLogicalWidth) {
            if (resized)
                std::fprintf(stderr, "dpi: display %s -> %s: UI Scale and Cursor Size x%.3f -> uiScale=%.2f cursorScale=%d\n",
                             wlc::modeName(kinds[0]), wlc::modeName(kinds[1]), f, settings.uiScale, settings.cursorScale);
            settings.displayMode = h.mode;
            settings.displayScale = h.scale;
            settings.displayX11Width = h.x11Width;
            settings.displayLogicalWidth = h.logicalWidth;
            // Save just these into the file, not this run's command-line overrides.
            ta::Settings disk = ta::loadSettings();
            disk.uiScale = settings.uiScale;
            disk.cursorScale = settings.cursorScale;
            disk.displayMode = settings.displayMode;
            disk.displayScale = settings.displayScale;
            disk.displayX11Width = settings.displayX11Width;
            disk.displayLogicalWidth = settings.displayLogicalWidth;
            ta::saveSettings(disk);
        }
    }
#endif
    // Set texture-build policy before loading art. Options hosts rebuild the current
    // interface when smoothing changes; ordinary frames reuse those textures.
    ta::applyRuntimeSettings(settings);
    bool renAccelerated = false;
    {
        SDL_RendererInfo ri{};
        if (SDL_GetRendererInfo(ren, &ri) == 0) {
            renAccelerated = (ri.flags & SDL_RENDERER_ACCELERATED) != 0;
            renState.created = ri.name ? ta::renderdriver::normalize(ri.name) : std::string();
            renState.accelerated = renAccelerated;
            std::fprintf(stderr, "renderer: %s%s [%s, setting %s]\n", ri.name ? ri.name : "?",
                         renAccelerated ? " (accelerated)" : " (SOFTWARE)",
                         ta::renderdriver::label(renState.created).c_str(),
                         ta::renderdriver::label(renState.inEffect).c_str());
        }
    }

#if defined(_WIN32) || defined(__APPLE__)
    {   // DPI evidence: with the option on, renderer output is physical pixels (Windows:
        // window size == output; macOS: output == window points x backing scale).
        int ww = 0, wh = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        float ddpi = 0;
        const int dpiOk = SDL_GetDisplayDPI(SDL_GetWindowDisplayIndex(win), &ddpi, nullptr, nullptr);
        std::fprintf(stderr, "dpi: window %dx%d, renderer output %dx%d, display dpi %.0f (scale %.2f), ignoreDpiScaling=%d uiScale=%.2f cursorScale=%d\n",
                     ww, wh, ow, oh, dpiOk == 0 ? ddpi : 0.f, dpiOk == 0 ? ddpi / 96.f : 0.f,
                     settings.ignoreDpiScaling ? 1 : 0, settings.uiScale, settings.cursorScale);
    }
#elif defined(__linux__)
    {   // With native Wayland the renderer output is window points x the desktop scale.
        int ww = 0, wh = 0, ow = 0, oh = 0;
        SDL_GetWindowSize(win, &ww, &wh);
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        const char* drv = SDL_GetCurrentVideoDriver();
        std::fprintf(stderr, "dpi: %s window %dx%d, renderer output %dx%d (scale %.2f), uiScale=%.2f cursorScale=%d\n",
                     drv ? drv : "?", ww, wh, ow, oh, ta::backingScale(ww, ow), settings.uiScale, settings.cursorScale);
    }
#endif
    // ---- outer session loop: menu -> game -> menu (menu launches only) ----------
    // After a menu-launched session ends (a MAIN MENU button or post-game Escape),
    // loop back to the front-end and let the player pick again. Non-menu launches
    // (direct game/map/replay, headless) run one pass and break. The window/renderer
    // and the menu's vfs outlive each session.
    const bool fromMenu = (mode == "menu");
    const std::string launchMode = mode;
    const std::string launchServerHost = serverHost;
    const int launchServerPort = serverPort;
    const std::vector<std::string> launchArgs = args;
    bool quitApp = false;
    bool restartApp = false;   // Options changed the data folder: relaunch after teardown
    // Campaign chaining: a Next/Retry from the result screen re-enters the game
    // directly (skipping the menu) with this mission, movie + briefing and all.
    std::string pendingCampaign, pendingCampaignId;
    ta::MenuMusic menuMusic;   // persists across menu -> lobby so the track doesn't restart
    menuMusic.setVolume(settings.masterVol, settings.bgmVol);
    // TA_SHOT_RESULT=<png> [TA_SHOT_RESULT_SIDE=<sidedata index>] [TA_SHOT_RESULT_LOSE=1]:
    // render the end-of-game plate with a sample table and exit. Playing a whole match
    // out just to look at the screen isn't practical, so this is how its layout gets
    // verified against the retail art. TA_SHOT_RESULT_WATCH=1 renders a watcher's plate
    // (GameView::resultStats): the second player the winner, or with ..._LOSE nobody.
    if (const char* rs = ta::devEnv("TA_SHOT_RESULT"); rs && *rs) {
        ta::ResultStats st;
        st.matchSec = 17 * 60 + 42;
        if (const char* sd = ta::devEnv("TA_SHOT_RESULT_SIDE")) st.faction = std::atoi(sd);
        const char* names[5] = {"Curtis", "Bruce", "Ludwin", "Pat", "Alex"};
        for (int i = 0; i < 5; ++i) {
            ta::ResultRow r;
            r.name = names[i];
            r.colorSlot = i;
            r.side = i % 2 ? "CORE" : "ARM";
            r.built = 120 - i * 23;
            r.kills = 48 - i * 11;
            r.losses = 31 + i * 9;
            r.isLocal = (i == 0);
            r.defeated = (i >= 2);
            r.timeSec = r.defeated ? 600 + i * 90 : st.matchSec;
            st.rows.push_back(std::move(r));
        }
        bool victory = ta::devEnv("TA_SHOT_RESULT_LOSE") == nullptr;
        const bool watch = ta::devFlag("TA_SHOT_RESULT_WATCH");
        if (watch) {
            st.rows[0].isLocal = false;
            if (!victory) st.bandWord = "Game Over";
            else {
                st.rows[1].isLocal = true;
                st.watchLine = st.rows[1].name + " wins";
                if (!ta::devEnv("TA_SHOT_RESULT_SIDE")) st.faction = 1;
            }
        }
        ta::ResultScreen::run(ren, vfs, victory, watch ? "" : "MISSION 7", false, &settings, nullptr, &st);
        ta::releaseUiTextGlyphs(); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return 0;
    }
    // Retail intro: play the logo movie once at startup (scaled to the window), then
    // fall through to the front-end. Any key / click / window-close skips it. Menu
    // launches only, and never for a headless screenshot run.
    //
    // LOGO ONLY. intro.bik belongs on the PlayStory door, not here -- retail
    // dispatches on that gadget name and plays it there, once per launch (see
    // MainMenu::run). Playing both back to back at startup was wrong.
    // TotalA.exe plays 1.zrb (its intro) at startup (0x426f0e); a Kingdoms-style
    // install's logo.bik wins when present.
    if (fromMenu && shot.empty())
        ta::MainMenu::playIntro(ren, dataRoot,
                                ta::MainMenu::findMovie(dataRoot, "logo.bik").empty() ? "1.zrb" : "logo.bik");
    auto streamingOwner = std::make_unique<ta::Streaming>(ren, &settings, [&settings] { ta::saveSettings(settings); });
    auto& streaming = *streamingOwner;
    // Voice chat (client/voice.h). Only for a real interactive window: the headless
    // harnesses and screenshot runs never construct it, so they never open a device.
    // Built for every interactive launch, it still opens nothing while voice chat is off.
    std::unique_ptr<ta::VoiceEngine> voiceOwner;
    if (mpHeadless == 0 && shot.empty() && applyRenderer) voiceOwner = std::make_unique<ta::VoiceEngine>();
    menuMusic.setAudioTap(&streaming.stream(),
        [](void* context, const int16_t* pcm, int frames, int channels) {
            static_cast<ta::video::Stream*>(context)->audio(pcm,frames,channels);
        });
    struct GameStreamAudio { ta::video::Stream* stream; ta::MenuMusic* menu; };
    GameStreamAudio gameStreamAudio{&streaming.stream(), &menuMusic};
    ta::PresentationPacer streamPacer;
    ta::SessionFailure menuFailure;   // failed menu launch -> shown where it came from
    ta::ResultChoice afterResult = ta::ResultChoice::Menu;   // a skirmish result's right door
    bool lanReturnToConnect = false;   // a LAN game (hosted or joined) ended: back to CONNECT, not the title
    std::string lastServer;                                    // the server its game was on
    // The last password login's sign-in token (src/net/auth.h): GAME SELECT's way back
    // to the game list. In memory only, and revoked when the app quits.
    ta::net::SessionToken sessionToken;
    uint64_t retailHash = 0;                                   // see the Hello below
    for (;;) {
    if (ta::termRequested()) { quitApp = true; break; }   // SIGTERM/SIGINT between sessions
    if (fromMenu || !pendingCampaign.empty()) { serverHost = launchServerHost;
                    serverPort = launchServerPort; args = launchArgs;
                    menuMusic.start(vfs, 15);   // front-end BGM (idempotent; loops into the lobby)
                    // A pending Next/Retry re-enters the game directly, skipping the menu.
                    mode = pendingCampaign.empty() ? launchMode : std::string("game"); }

    // main-loop lobby driver: 0 = UI-driven lobby (browse/join/host), 7 = auto SP,
    // 8 = auto campaign mission (create the mission room, seat, start, then play).
    int mpAutoMode = 0;
    ta::SkirmishSetup skirmishSetup;   // TA SKIRMISH screen (kept between games)
    ta::TaCampaign taCampaign;         // TA NEWGAME pick: campaign, mission, difficulty, W/L/U results
    bool skirmishLaunch = false;
    bool menuInteractive = false;   // menu single-player -> interactive lobby, not auto-play
    bool tokenLaunch = false;       // GAME SELECT, signing in with the sign-in token
    bool benchmarkLaunch = false;   // menu Benchmark -> auto-host an all-AI watch perf run
    int benchmarkLevel = 0;         // picked benchmark intensity 1..5
    std::string campaignStem;       // menu campaign pick -> host this mission (autoMode 8)
    std::string campaignId;         // ...its campaign id (for progress persistence)
    // Where a failed launch goes back to; Cli (no menu) exits with an error code.
    ta::LaunchOrigin origin = ta::LaunchOrigin::Cli;
    auto failLaunch = [&](std::string why) { return ta::routeFailure(origin, std::move(why), menuFailure); };
    // A sign-in token that did not work is no news to the player: GAME SELECT simply
    // becomes the CONNECT screen, with no message. One the server refused is gone
    // already; one lost on a bad link may still be good there, so quitting revokes it.
    auto tokenFallback = [&] {
        if (!tokenLaunch) return false;
        sessionToken.retire();
        afterResult = ta::ResultChoice::GameSelect;
        return true;
    };
    ta::LocalServer localServer;   // single-player's private taserver; dies with the session
    // Next/Retry chosen on the previous mission's result screen: re-enter directly.
    if (!pendingCampaign.empty()) {
        if (fromMenu) origin = ta::LaunchOrigin::Campaign;
        campaignStem = pendingCampaign; campaignId = pendingCampaignId;
        pendingCampaign.clear(); pendingCampaignId.clear();
        if (campaignId.rfind("ta:", 0) == 0) { args.clear(); args.push_back(taCampaign.mapName()); }
        if (args.empty()) args.push_back("athri cay");
    }
    // Direct launch into a mission (`--campaign <stem>`): same host path as a menu
    // pick, resolving the campaign id so a win still advances persisted progress.
    if (!cliCampaign.empty()) {
        campaignStem = std::exchange(cliCampaign, {});
        if (args.empty()) args.push_back("athri cay");   // GameView needs a map; the mission overrides it
        for (const auto& c : ta::loadCampaigns(vfs)) {
            if (ta::campaignChapter(c, campaignStem)) campaignId = c.id;
            if (!campaignId.empty()) break;
        }
    }

    // Front-end: the retail three-door main menu. Its choice drives the setup below
    // (single-player -> local server + lobby; multiplayer -> connect + browser).
    std::string rememberServer;   // menu-picked MP server; saved to Settings on a
                                  // successful connect (feeds the CONNECT dropdown)
    bool serverPlainFallback = false;   // no scheme typed: TLS first, then unencrypted
    // The CONNECT screen's LOCAL SERVER (start one here and host on it) or a LAN game
    // heard on the network: both go by the player's name alone.
    auto connectKind = ta::MainMenu::Connect::Server;
    bool lanHosting = false;
    std::string lanGamePass;   // a LAN game's password: the host's for its game, a joiner's to get in
    if (mode == "menu") {
        if (dataRoot.empty()) { std::fprintf(stderr, "menu: needs --data <retail-install-dir>\n"); return 1; }
        std::string menuServer;
        ta::MainMenu::Choice choice = ta::MainMenu::Choice::None;
        // Total Annihilation's own front end (the retail .GUI screens). SKIRMISH launches
        // straight from it; the other doors still use the engine menu below for now.
        skirmishLaunch = false;
        bool taFrontChose = false;
        if (afterResult == ta::ResultChoice::Menu &&
            (!menuFailure || menuFailure.to == ta::FailTo::Notice || menuFailure.to == ta::FailTo::ServerSelect ||
             menuFailure.to == ta::FailTo::CampaignPicker)) {
            ta::TaFrontEnd front(ren, win, vfs);
            if (menuFailure) {
                const auto f = std::exchange(menuFailure, {});
                front.setFailure(int(f.to), f.message);
            }
            const auto tc = front.run(skirmishSetup, taCampaign);
            if (tc == ta::TaFrontEnd::Choice::Campaign && taCampaign.active()) {
                // NEWGAME -> MSNBRIEF Start: host the mission (autoMode 8) on its map.
                choice = ta::MainMenu::Choice::Campaign;
                taFrontChose = true;
                campaignStem = taCampaign.spec();
                campaignId = "ta:" + taCampaign.name;
                args.clear();
                args.push_back(taCampaign.mapName());
            }
            if (tc == ta::TaFrontEnd::Choice::Exit) { quitApp = true; break; }
            if (tc == ta::TaFrontEnd::Choice::Menu) continue;   // back to the TA main menu
            if (tc == ta::TaFrontEnd::Choice::Multiplayer) {
                // SELPROV -> TCP: the engine server at the typed address (blank: this
                // machine), signed in as the player's name.
                choice = ta::MainMenu::Choice::Multiplayer;
                taFrontChose = true;
                menuServer = front.chosenServer();
                loginUser = settings.accountName.empty() ? settings.playerName : settings.accountName;
                connectKind = ta::MainMenu::Connect::Server;
            }
            if (tc == ta::TaFrontEnd::Choice::Skirmish) {
                choice = ta::MainMenu::Choice::SinglePlayer;
                skirmishLaunch = true;
                taFrontChose = true;
                args.clear();
                args.push_back(skirmishSetup.map);
            }
        }
        if (!taFrontChose) {
            ta::MainMenu menu(ren, vfs, dataRoot);
            if (menuFailure) menu.setFailure(std::exchange(menuFailure, {}));   // reopen where it failed
            {   // Options -> GAME DATA FOLDER: greyed when --data or the launch folder chose it.
                ta::DataFolderRow row{dataRoot, ta::dataFolderOverrideReason(dataOrigin), {}};
#ifndef NDEBUG
                // TA_DATADIR_PICK="a;b;..." stands in for the native picker: each pick
                // returns the next folder, then "" (cancel). Debug builds only.
                if (const char* fake = ta::devEnv("TA_DATADIR_PICK")) {
                    auto picks = std::make_shared<std::vector<std::string>>();
                    std::stringstream list(fake);
                    for (std::string one; std::getline(list, one, ';');) picks->push_back(one);
                    auto next = std::make_shared<size_t>(0);
                    row.picker = [picks, next](const std::string&, const std::string&) {
                        return *next < picks->size() ? (*picks)[(*next)++] : std::string();
                    };
                }
#endif
                menu.setDataFolder(std::move(row));
            }
            menu.setVoice(voiceOwner.get());
            // NEW GAME is the single-player door. GAME SELECT goes straight back to the
            // same server's game list on the sign-in token. With no token for it (a
            // plaintext server, or one refused), it is the CONNECT screen: the password
            // is never kept, so it has to be typed again.
            const auto next = std::exchange(afterResult, ta::ResultChoice::Menu);
            if (std::exchange(lanReturnToConnect, false) && next == ta::ResultChoice::Menu) {
                menu.reopenConnect("");
                std::fprintf(stderr, "menu: back to CONNECT TO SERVER\n");
            }
            if (next == ta::ResultChoice::GameSelect) {
                const auto address = ta::parseServerAddress(lastServer);
                tokenLaunch = address && sessionToken.heldFor(address->host, address->port);
                if (!tokenLaunch) menu.reopenConnect(lastServer);
            }
            choice = next == ta::ResultChoice::NewGame ? ta::MainMenu::Choice::SinglePlayer
                   : tokenLaunch ? ta::MainMenu::Choice::Multiplayer
                   : menu.run(shot, &menuServer, &menuMusic, &settings, &streaming);
            ta::renderdriver::active().noticeShown = true;   // the menu showed it (if any)
            if (choice == ta::MainMenu::Choice::Campaign) {
                campaignStem = menu.chosenMission();
                campaignId = menu.chosenCampaign();
            }
            if (tokenLaunch) {
                menuServer = lastServer;
                loginUser = sessionToken.account;
                ta::crypto::wipe(loginPass);
            } else if (choice == ta::MainMenu::Choice::Multiplayer) {
                // Take the account out of the menu while it is still alive, and
                // wipe its copy of the password immediately -- it has no further
                // use for it, and a secret should outlive its purpose by as little
                // as possible.
                loginUser = menu.chosenAccount();
                loginPass = menu.chosenPassword();
                menu.clearPassword();
                connectKind = menu.chosenConnect();
            }
            // SETTINGS -> LOAD REPLAY. Routed through the ordinary replay launch below
            // by setting the mode here, so the menu path and the debug CLI share one
            // implementation -- and so a RELEASE build can watch replays at all, which
            // it otherwise could not: the `replay` CLI keyword is debug-only.
            if (choice == ta::MainMenu::Choice::Replay) {
                mode = "replay";
                args.clear();
                args.push_back(menu.chosenReplay());
            }
            if (choice == ta::MainMenu::Choice::Benchmark) {
                benchmarkLevel = menu.chosenBenchmarkLevel();
                if (benchmarkLevel < 1 || benchmarkLevel > ta::sim::kBenchLevels) benchmarkLevel = 3;   // safety default = High
            }
            // The credits door rolls the credits and returns to the menu, rather
            // than being a way out of the app.
            if (choice == ta::MainMenu::Choice::Credits) {
                ta::MainMenu::playIntro(ren, dataRoot, "credits.bik");
                continue;
            }
        }
        if (choice == ta::MainMenu::Choice::Restart) { restartApp = true; quitApp = true; break; }
        if (!shot.empty() && choice == ta::MainMenu::Choice::None) { menuMusic.stop(); menuMusic.setAudioTap(nullptr,nullptr); streamingOwner.reset(); ta::releaseUiTextGlyphs(); SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit(); return 0; }
        if (choice != ta::MainMenu::Choice::SinglePlayer &&
            choice != ta::MainMenu::Choice::Multiplayer &&
            choice != ta::MainMenu::Choice::Benchmark &&
            choice != ta::MainMenu::Choice::Replay &&
            !(choice == ta::MainMenu::Choice::Campaign && !campaignStem.empty())) {
            quitApp = true; break;   // exit / options (or campaign with no pick) -> leave the app
        }
        benchmarkLaunch = (choice == ta::MainMenu::Choice::Benchmark);
        origin = choice == ta::MainMenu::Choice::Multiplayer ? ta::LaunchOrigin::Multiplayer
               : choice == ta::MainMenu::Choice::Replay      ? ta::LaunchOrigin::Replay
               : choice == ta::MainMenu::Choice::Campaign    ? ta::LaunchOrigin::Campaign
               : benchmarkLaunch                              ? ta::LaunchOrigin::Benchmark
                                                              : ta::LaunchOrigin::SinglePlayer;
        // A replay picked from the menu already set mode/args above, and must not be
        // overwritten with "game" here.
        if (choice != ta::MainMenu::Choice::Replay) mode = "game";
        if (args.empty()) args.push_back("athri cay");   // fallback map; the lobby has its own map list
        lanHosting = choice == ta::MainMenu::Choice::Multiplayer && !tokenLaunch &&
                     connectKind == ta::MainMenu::Connect::LanHost;
        if (choice == ta::MainMenu::Choice::Multiplayer && !tokenLaunch &&
            connectKind != ta::MainMenu::Connect::Server) {
            // A LAN game has no accounts: the name is the player, never blank (the
            // menu checked it), remembered like an account name; no password exists.
            playerName = loginUser;
            if (settings.accountName != loginUser) { settings.accountName = loginUser; ta::saveSettings(settings); }
            loginUser.clear();
            lanGamePass = loginPass;   // that box was the GAME password here
            ta::crypto::wipe(loginPass);
        } else {
            ta::crypto::wipe(lanGamePass);
        }
        if (lanHosting) {
            rememberServer.clear(); lastServer.clear();   // this session's own server: nothing to go back to
        } else if (choice == ta::MainMenu::Choice::Multiplayer) {
            std::string sv = menuServer.empty() ? std::string("127.0.0.1") : menuServer;
            rememberServer = sv;   // remembered (as picked/typed) if the connect succeeds
            lastServer = sv;
            const auto address = ta::parseServerAddress(sv);
            if (!address) {
                failLaunch("INVALID SERVER ADDRESS OR PORT");
                ta::crypto::wipe(loginPass);
                continue;
            }
            serverHost = address->host;
            serverPort = address->port;
            serverPlainFallback = address->plainFallback;
            // A LAN game heard on the network is not remembered: it is gone with its host.
            if (connectKind == ta::MainMenu::Connect::LanJoin || connectKind == ta::MainMenu::Connect::LanSpectate)
                rememberServer.clear();
        } else {
            // Remote credentials belong to that multiplayer session. Leaving the
            // account set makes the client reject the private server's no-auth
            // welcome on a later single-player / campaign / benchmark launch.
            loginUser.clear();
            ta::crypto::wipe(loginPass);
            playerName = settings.playerName;
            if (!benchmarkLaunch && !skirmishLaunch)
                menuInteractive = true;   // single-player: local server, but stop in the lobby
        }
        // Benchmark: like single-player (local server) but auto-hosts an all-AI watch run
        // -- no interactive lobby, and mpAutoMode is forced to 1 below.
    }

    // The chapter movie precedes loading. Retail's briefing is an overlay on the
    // loaded, paused battlefield; GameView shows it before releasing the server's
    // first-tick loading barrier.
    if (!campaignStem.empty() && !mpHeadless) {
        menuMusic.setVolume(0, 0);   // hush the front-end track under the movie's own audio
        ta::MainMenu::playIntro(ren, dataRoot, (campaignStem + ".bik").c_str());
        menuMusic.setVolume(settings.masterVol, settings.bgmVol);
        menuMusic.stop();   // the loaded-world briefing uses the game's music
    }

    // Present before synchronous local-server/data/lobby setup, not after the
    // first lobby draw discovers and parses the entire map catalogue.
    std::unique_ptr<ta::LoadScreen> lobbyLoading;
    // Multiplayer too: its connect and sign-in below are synchronous, and without a
    // screen of its own the window kept showing the title until the lobby drew.
    const bool menuMultiplayer = fromMenu && origin == ta::LaunchOrigin::Multiplayer;
    if (fromMenu && (menuInteractive || menuMultiplayer) && campaignStem.empty() && !benchmarkLaunch) {
        // Titled as the door that opened it: PlayComputer "Play the Machine", PlayPlayer
        // "Play an Opponent" (the doors' captions in guis/mainmenu.gui).
        lobbyLoading=std::make_unique<ta::LoadScreen>(ren,vfs,menuMultiplayer ? "Play an Opponent" : "Play the Machine",&settings);
        lobbyLoading->step(menuMultiplayer ? "Connecting..." : "Starting...",5);
    }

    // PIN SIMULATION THREADS (Options > GAME, OFF by default), for this session and its
    // local server: the setting decides, in every kind of session. TA_SIM_PIN=0 stops a
    // debug build pinning whatever the setting says, which is how ctest runs every test
    // (a suite runs many clients at once, and they would share the same cores); no
    // variable turns pinning on.
    const bool simPin = settings.simPin && ta::devFlag("TA_SIM_PIN", true);
    // PIN TO CCDS (Options > GAME): this game on its CCD now, its local server on its own
    // (taserver --cpus); while on, it replaces SIMULATION THREADS and its pinning. The
    // same TA_SIM_PIN=0 keeps a debug build off it.
    const bool hostsLocal = mode == "game" && serverHost.empty() && !mpHeadless && !localHarness;
    ta::client::CcdPlan ccdPlan = ta::client::planCcds(settings, hostsLocal);
    if (!ta::devFlag("TA_SIM_PIN", true)) ccdPlan = {};
    ta::client::applyGameCcd(ccdPlan);
    std::string serverBin;
    if (hostsLocal) {
        // Single-player: auto-launch a private local server and play a 1-v-AI game
        // on it (the AI runs server-side). Not visible to other players.
        int p = lanHosting ? pickLanPort() : 0;
        if (p <= 0) p = pickFreePort();
        serverBin = resolveServerBin(argv[0]);
#ifndef NDEBUG
        // TA_LOCAL_SERVER_BIN=<path>: run this instead (a missing or failing server
        // exercises the launch-failure path; see the menu_launch_failure test).
        if (const char* bin = ta::devEnv("TA_LOCAL_SERVER_BIN"); bin && *bin) serverBin = bin;
#endif
        // SIMULATION THREADS: this game and its local server get half each, and every
        // other CPU of the hand-out when threads are pinned (latchSimThreads below takes
        // the game's). A debug build's TA_SIM_WORKERS overrides AUTO in both.
        using ta::sim::SimPool;
        const ta::sim::CpuTopology cpus = ta::sim::detectCpus();
        bool debugAuto = false;
        const SimPool::Session session = SimPool::session(settings.simThreads, cpus, true, &debugAuto);
        const std::string serverCpus = ta::sim::cpuListText(
            SimPool::handOut(cpus.order, session, true, simPin && cpus.canPin).server);
        const bool launched = ccdPlan.on
            ? p > 0 && spawnLocalServer(localServer, serverBin, dataRoot, p, lanHosting, int(ccdPlan.serverThreads), "none", ccdPlan.server)
            : p > 0 && spawnLocalServer(localServer, serverBin, dataRoot, p, lanHosting, debugAuto ? 0 : int(session.server),
                                        serverCpus.empty() ? "none" : serverCpus);
        if (!launched) {
            std::fprintf(stderr, "single-player: could not launch a local server (%s)\n",
                         serverBin.c_str());
            if (failLaunch("could not start the local server (" + serverBin + ")")) continue;
            return 1;
        }
        serverHost = "127.0.0.1"; serverPort = p;
        // Campaign mission -> auto-host it (mode 8); PLAY THE MACHINE, a LOCAL SERVER host and
        // --play-map open their room from the remembered settings (mode 0, the room's own
        // create); CLI single-player creates its room and seats the player unready (mode 7),
        // then also waits for AIs, READY and START unless TA_SP_AIS is set.
        mpAutoMode = benchmarkLaunch ? 1
                   : !campaignStem.empty() ? 8 : (menuInteractive || lanHosting || !playMap.empty() ? 0 : 7);
        std::fprintf(stderr, "%s: local server on port %d%s\n", lanHosting ? "local server" : "single-player", p,
                     lanHosting ? " (LAN, unencrypted)" : !campaignStem.empty() ? " (campaign)" : menuInteractive ? " (lobby)" : "");
    }

    // Connect to the multiplayer server, if requested.
    std::unique_ptr<ta::net::MpClient> mp;
    if (!serverHost.empty()) {
        mp = std::make_unique<ta::net::MpClient>();
        mp->setMapRoot(ta::u8path(dataRoot));
        mp->setGameModsAllowed(modUse.game);   // debug --mods none|cosmetic: no game packs at all
        // The ticks of this launch's room kind: single player for the menu's Play the
        // Machine, a CLI game and --play-map (mode 7), multiplayer otherwise.
        mp->setGameModPacks(settings.prefsFor(menuInteractive || mpAutoMode == 7 || !playMap.empty()).gamePacks);
        if (playerName.empty()) playerName = settings.playerName;
        if (playerName.empty()) playerName = "player";
        // Hello carries the PURE-RETAIL gameplay fingerprint (no mods), so the
        // base game files are checked regardless of anyone's mods; the room's game
        // mods are agreed later, at load. The install is
        // immutable while the game runs, so the fingerprint (a mount + a read of
        // every gameplay file) is computed once and reused across menu->Play loops.
        if (lobbyLoading) lobbyLoading->step("Checking game data",20);
        if (!dataRoot.empty()) {
            if (!retailHash)
                retailHash = ta::hpi::gameplayHash(
                    ta::hpi::mountRetailRoot(ta::u8path(dataRoot)));
            mp->setDataHash(retailHash);
        }
        // A freshly-spawned local server takes a moment to mount + listen (~0.25s
        // warm); poll fast so single-player doesn't pay coarse-sleep quantization.
        // Credentials for a server that requires an account. A local single-player
        // server requires none (it is launched --no-auth --local), so this is empty
        // there and the handshake never asks.
        if (!loginUser.empty()) {
            mp->setLogin(loginUser, loginPass);
            mp->setSessionToken(&sessionToken, tokenLaunch);
            playerName = loginUser;   // the account IS the multiplayer identity
        }
        ta::crypto::wipe(loginPass);
        // The step lines name the server: the local one for Play the Machine, else the host.
        const std::string connectingTo = localServer.started() ? std::string("Connecting to local server")
                                                               : "Connecting to " + serverHost;
        const std::string signingIn = localServer.started() || loginUser.empty() ? connectingTo
                                                                                : "Signing in to " + serverHost;
        if (lobbyLoading) lobbyLoading->step(connectingTo,35);
        uint64_t lobbyProgressAt=SDL_GetTicks64();
        bool ok = false;
        std::string serverDied;   // the local server exited before it would answer
        std::string transportWhy;   // a failed connect, said for the transports tried
        for (int attempt = 0; attempt < (localServer.started() ? 200 : 1) && !ok; ++attempt) {
            ok = mp->connect(serverHost, uint16_t(serverPort), playerName);
            if (!ok && !localServer.started()) {
                // No scheme typed: TLS, then the same address unencrypted. A host that
                // cannot be resolved or reached is not tried twice.
                const std::string shown = serverHost.starts_with("tls://") ? serverHost.substr(6) : serverHost;
                const std::string at = shown + ":" + std::to_string(serverPort);
                const std::string first = mp->error();
                const bool unreachable = first.starts_with("cannot resolve") || first.starts_with("connect failed");
                if (serverPlainFallback && serverHost.starts_with("tls://") && !unreachable) {
                    if (lobbyLoading) lobbyLoading->step("Connecting to " + shown + " unencrypted", 35);
                    ok = mp->connect(shown, uint16_t(serverPort), playerName);
                    if (ok) serverHost = shown;
                    else transportWhy = "could not connect to " + at + " with TLS (" + first +
                                        ") or unencrypted (" + mp->error() + ")";
                } else if (unreachable) {
                    transportWhy = first.starts_with("cannot resolve") ? "cannot find the server " + shown
                                                                       : "no server answered at " + at;
                } else {
                    transportWhy = std::string("could not connect to ") +
                                   (serverHost.starts_with("tls://") ? "tls://" : "tcp://") + at + ": " + first;
                }
            }
            if (!ok && localServer.started()) {
                // A dead server will never listen: say so now, not after the 10 s poll.
                if (int code = 0; localServer.exited(&code)) {
                    serverDied = ta::localServerExitMessage(code, serverBin);
                    break;
                }
                if (lobbyLoading && SDL_GetTicks64()-lobbyProgressAt>=100) {
                    lobbyLoading->step("Waiting for local server",35);lobbyProgressAt=SDL_GetTicks64();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!ok) {
            const std::string why = !serverDied.empty() ? serverDied
                                  : !transportWhy.empty() ? transportWhy
                                  : mp->error().empty() ? "COULD NOT CONNECT TO " + serverHost
                                                        : mp->error();
            std::fprintf(stderr, "server: %s\n", why.c_str());
            localServer.stop();
            // A menu-launched connect failure returns to the screen it was picked
            // from with the error shown (the CONNECT dropdown for multiplayer) --
            // never exits the app.
            mp.reset();
            if (tokenFallback() || failLaunch(why)) continue;
            return 1;
        }
        // The TCP connect only queued the Hello. Settle the handshake HERE, before
        // the game starts loading, or a refused login would surface as a mystery
        // disconnect on the in-game lobby screen instead of as an answer in the
        // menu the player is still looking at. Signing in costs a round trip plus
        // a few hundred ms of deliberate password-stretching (on a worker thread),
        // so give it a generous ceiling and pump events meanwhile.
        bool menuTimedOut = false;
        {
            const uint64_t deadline = SDL_GetTicks64() + 30000;
            while (!mp->handshakeSettled() && SDL_GetTicks64() < deadline) {
                if (!mp->poll()) break;
                if (lobbyLoading && SDL_GetTicks64()-lobbyProgressAt>=100) {
                    lobbyLoading->step(signingIn,40);lobbyProgressAt=SDL_GetTicks64();
                }
                SDL_PumpEvents();          // keep the window responsive while we wait
                SDL_Delay(5);
            }
            if (!mp->handshakeSettled() && mp->error().empty()) {
                // Say what actually happened. Letting this fall through to the
                // socket close would report "the server closed the connection",
                // which sends the player looking for a network fault that is not
                // there.
                mp->disconnect("login timed out");
                menuTimedOut = true;
            }
        }
        const bool loginRefused = mp->auth() == ta::net::MpClient::Auth::Failed;
        if (loginRefused || menuTimedOut || mp->state() == ta::net::MpClient::State::Done) {
            std::string why =
                menuTimedOut ? std::string("the server stopped responding while signing in")
                : mp->error().empty() ? std::string("the server closed the connection")
                                      : mp->error();
            std::fprintf(stderr, "server: %s\n", why.c_str());
            localServer.stop();
            mp.reset();
            if (tokenFallback() || failLaunch(why)) continue;   // back where it was picked, the reason in red
            return 1;
        }
        if (mp->auth() == ta::net::MpClient::Auth::Created)
            std::printf("created account '%s' on %s\n", mp->account().c_str(), serverHost.c_str());
        if (!mp->account().empty()) {
            playerName = mp->account();
            // Remember the NAME for next time (never the password).
            if (settings.accountName != mp->account()) {
                settings.accountName = mp->account();
                ta::saveSettings(settings);
            }
        }
        std::printf("connected to %s:%d as '%s'\n", serverHost.c_str(), serverPort, playerName.c_str());
        // Remember a menu-picked server that connected successfully: move-to-front
        // (case-insensitive dedupe), cap 8, persist. Feeds the CONNECT dropdown.
        if (!rememberServer.empty()) {
            auto ieq = [](const std::string& a, const std::string& b) {
                if (a.size() != b.size()) return false;
                for (size_t i = 0; i < a.size(); ++i)
                    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
                        return false;
                return true;
            };
            auto& ks = settings.knownServers;
            ks.erase(std::remove_if(ks.begin(), ks.end(),
                                    [&](const std::string& s) { return ieq(s, rememberServer); }),
                     ks.end());
            ks.insert(ks.begin(), rememberServer);
            if (ks.size() > ta::Settings::kMaxKnownServers) ks.resize(ta::Settings::kMaxKnownServers);
            ta::saveSettings(settings);
        }
    }


    std::unique_ptr<MapView> mapView;
    std::unique_ptr<ModelView> modelView;
    std::unique_ptr<GameView> gameView;
    try {
        if (mode == "replay" && !args.empty() && !dataRoot.empty()) {
            try {
                gameView = makeReplayView(ren, args[0], dataRoot, settings, fromMenu);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "replay: cannot read %s -- %s\n", args[0].c_str(), e.what());
                if (failLaunch(e.what())) continue;   // back to the picker with the reason
                return 1;
            }
        } else if (mode == "map" && !args.empty() && !dataRoot.empty()) {
            // A "~gen1~" id is a random-map recipe MapView builds in memory; a plain
            // name resolves to a real .tnt in the mounted data.
            std::string mapPath = ta::mapgen::isGeneratedMapId(args[0])
                                      ? args[0]
                                      : ta::hpi::findMap(vfs, args[0]);
            if (mapPath.empty()) {
                std::fprintf(stderr, "map '%s' not found\n", args[0].c_str());
                if (failLaunch("map '" + args[0] + "' not found")) continue;
                return 1;
            }
            mapView = std::make_unique<MapView>(ren, vfs, mapPath);
        } else if (mode == "game" && !args.empty() && !dataRoot.empty()) {
            std::string mapPath = ta::hpi::findMap(vfs, args[0]);
#ifndef NDEBUG
            // Campaign missions live under missions/, which findMap does not search --
            // it covers Maps/ and kmap/, the skirmish namespaces, and the campaign
            // runner hands MapView a path directly rather than looking one up by name.
            // That left --mpmission unable to launch the very maps it exists to drive,
            // so the mission path had no automated coverage at all: a change to the
            // sim's feature/nav setup could regress campaigns with nothing to catch it.
            // Resolve a mission stem here, in the debug harness only, rather than
            // widening a lookup the whole engine shares.
            if (mapPath.empty() && mpHeadless == 8) {
                std::string mstem = args[0];
                if (mstem.size() > 4 && mstem.substr(mstem.size() - 4) == ".tnt")
                    mstem = mstem.substr(0, mstem.size() - 4);
                std::string mpath = "missions/" + mstem + ".tnt";
                if (vfs.has(mpath)) mapPath = mpath;
            }
#endif
            if (mapPath.empty()) {
                std::fprintf(stderr, "map '%s' not found in %s\n", args[0].c_str(), dataRoot.c_str());
                if (failLaunch("map '" + args[0] + "' not found")) continue;
                return 1;
            }
            // A multiplayer client builds the world from the server's GameStarting
            // later, so it constructs "bare" (no single-player 2-monarch spawn).
            // When looping back to the menu, keep this function's vfs alive for the
            // next session (+ its findMap); hand the game its own fresh mount.
            if (lobbyLoading) lobbyLoading->step("Preparing the game room",50);
            gameView = std::make_unique<GameView>(ren,
                                                  // Campaign result/progression and the next
                                                  // session still need the front-end catalog.
                                                  (fromMenu || !campaignStem.empty())
                                                      ? ta::hpi::mountRetailRoot(ta::u8path(dataRoot)) : std::move(vfs),
                                                  mapPath, dataRoot, modUse, demo,
                                                  scenario,
                                                  navy || amphib || firetest || facetest || mp,
                                                  side, aiSide);
#ifndef NDEBUG
            if (ta::devEnv("TA_SHOT_BRIEFING") && !campaignId.empty()) {
                const auto campaigns=ta::loadCampaigns(vfs);
                if (std::none_of(campaigns.begin(),campaigns.end(),[&](const auto& c) {
                    return c.id==campaignId && ta::campaignChapter(c,campaignStem).has_value();
                })) throw std::runtime_error("campaign catalog lost before result/progression");
                std::fprintf(stderr,"PASS: campaign catalog retained for result/progression\n");
            }
#endif
            gameView->applySettings(settings);   // audio / camera / UI-scale prefs
            gameView->setSettings(&settings);     // in-game Options edits + persists these
            // SIMULATION THREADS: half with a local server of its own (which got the other), else all.
            if (ccdPlan.on) {   // its CCD's share
                gameView->latchSimThreads(int(ccdPlan.gameThreads), false, false);
                gameView->latchCcds(ccdPlan.gameCcd + 1, localServer.started() ? ccdPlan.serverCcd + 1 : 0, ccdPlan.serverThreads);
            }
            else gameView->latchSimThreads(settings.simThreads, localServer.started(), simPin);
            gameView->setVoice(voiceOwner.get()); // voice chat in multiplayer rooms (none for replays/benchmarks)
            if (std::string n = ta::renderdriver::pendingNotice(); !n.empty()) {
                gameView->showNotice(n, 10);   // launched straight into a game: say it here
                ta::renderdriver::active().noticeShown = true;
            }
            if(playtestTrace)gameView->setScenarioTrace([log=playtestTrace](int32_t tick,int player,int group,int action,const ta::crt::Rule* rule) {
                log->append(tick,player,group,action,rule);
            });
            if (mp) {
                gameView->setMpClient(mp.get());
                gameView->setLocalName(playerName);
                gameView->setLocalServerPid(localServer.pid());
                gameView->setMpMapId(args[0]);
                if (fromMenu) { gameView->setExternalLobbyMusic();  // front-end owns the lobby BGM
                                gameView->setCanReturnToMenu(); }   // in-game menu can return to it
                if (skirmishLaunch) gameView->setSkirmish(skirmishSetup);   // TA SKIRMISH: seat + start
                if (!campaignStem.empty()) gameView->setMissionStem(campaignStem);   // autoMode 8 hosts this mission
                // PLAY THE MACHINE: the single-player room, opened straight away.
                else if (menuInteractive) gameView->openSinglePlayerRoom();
                // --play-map: the single-player room on the Test Map copy, and only that map.
                else if (!playMap.empty()) gameView->openSinglePlayerRoom(args[0], /*playMap=*/true);
                else if (mpAutoMode == 7) gameView->setSinglePlayerFlag();   // CLI game: a private room
                // LOCAL SERVER: its host's room, opened straight away; the server names it
                // "<name>'s Game", with the game password typed on CONNECT.
                else if (lanHosting) gameView->openLanHostRoom(lanGamePass);
                else if (benchmarkLaunch) {   // menu Benchmark: all-AI watch run + metrics
                    gameView->setBenchmark(benchmarkLevel, settings.benchmarkPathfinding, settings.benchmarkSpeed);
                }
                // A LAN game's password, for getting into its game if it asks one.
                if (!lanHosting) gameView->setLanJoinPassword(lanGamePass);
                if (connectKind == ta::MainMenu::Connect::LanSpectate && !tokenLaunch) gameView->setLanSpectate();
                ta::crypto::wipe(lanGamePass);
            }
            if (lobbyLoading) {
                gameView->prepareLobbyMaps(*lobbyLoading);
                lobbyLoading->step("Ready",100);
                lobbyLoading.reset();
            }
            // Normal windows keep the widest build-icon row visible. An explicit
            // --winsize is also used by retail-comparison captures, where clipping
            // the local palette to retail's smaller viewport is intentional.
            {
                // The HUD minimum is in output pixels; the window is sized in points
                // (they differ only on a macOS Retina window, client/hidpi.h).
                const float backing = ta::hidpi::scale(ren);
                int minW = winSizeExplicit ? winW : ta::pixelsToPoints(gameView->minWindowWidth(), backing);
                int minH = winSizeExplicit ? winH : ta::pixelsToPoints(480, backing);
                SDL_SetWindowMinimumSize(win, minW, minH);
                int cw, ch;
                SDL_GetWindowSize(win, &cw, &ch);
                if (cw < minW || ch < minH)
                    SDL_SetWindowSize(win, std::max(cw,minW), std::max(ch,minH));
            }
            if (playerColor >= 0) gameView->setPlayerColor(0, playerColor);
            if (aiColor >= 0) gameView->setPlayerColor(1, aiColor);
            if (followZoom > 0) gameView->setFollow(followZoom);
            if (trace) gameView->setTrace(true);
            if (nofog) gameView->noFog_ = true;
            if (doLook) gameView->lookAt(lookX, lookZ);
#ifndef NDEBUG
            // Local dev/test harnesses spawn units / issue orders / fast-forward the
            // LOCAL sim -- debug builds only, and never for a server-driven game
            // (localHarness is false whenever a server is involved).
            if (localHarness) {
                gameView->cancelInitialCamera();
                if (doMarch) gameView->marchTo(marchX, marchZ);
                if (testbuild) gameView->testBuild();
                if (navy) gameView->navyDemo();
                if (misstest) gameView->missionTest();
                if (creon) gameView->creonDemo();
                if (guardtest) gameView->guardTest();
                if (lodetest) { gameView->lodeUnit = lodeUnitName; gameView->lodeTest(); }
                if (firetest) gameView->fireTest();
                if (facetest) gameView->faceTest();
                if (soundtest) { gameView->setTrace(true); gameView->soundTest(); }
                if (amphib) gameView->amphibDemo();
                if (startTime > 0) gameView->advance(startTime);
            }
#endif
        } else if (mode == "model" && !args.empty()) {
            modelView = std::make_unique<ModelView>(ren, args[0],
                                                    args.size() > 1 ? args[1] : "",
                                                    args.size() > 2 ? args[2] : "",
                                                    cobPath, anim, staticMask);
            if (startTime > 0) modelView->advance(startTime);
        } else {
            std::fprintf(stderr, "bad arguments\n");
            return 2;
        }
    } catch (const std::exception& e) {
        // Data/parse errors building the session. From the menu, go back with the
        // reason: taclient is a GUI-subsystem app on Windows, so a bare return
        // here just made the window vanish. The views and the local server are
        // session-scoped and go with the `continue`.
        std::fprintf(stderr, "error: %s\n", e.what());
        SDL_SetWindowMinimumSize(win, 0, 0);   // the game may have set its minimum
        if (failLaunch(e.what())) continue;
        return 1;
    }

    bool running = true;
    float netAccum = 0;
    std::string serverMapId = args.empty() ? "" : ta::vpath::stem(args[0]);
    int ktPhase = keytest ? 0 : -1;
    float ktClock = 0;
    bool keytestSelectOnly = selonly;

    // Headless multiplayer test driver: auto-run the lobby + game loop against
    // taserver and print periodic hashes. Proves the server-sequenced lockstep
    // end to end without any SDL UI. (--mphost creates+starts, --mpjoin joins.)
#ifndef NDEBUG
    if (gameView && gameView->replayMode())
        if (const char* seek = ta::devEnv("TA_REPLAY_SEEK_SCRIPT"))
            gameView->setReplaySeekScript(seek, ta::devFlag("TA_REPLAY_SEEK_STRAIGHT"));
#endif
    // Headless replay verify: play the whole recording and print the final hash.
    if (gameView && gameView->replayMode() && ta::devEnv("TA_REPLAY_VERIFY")) {
        while (gameView->replayTick() < gameView->replayLength()
#ifndef NDEBUG
               || gameView->replaySeekScriptBusy()
#endif
              )
            gameView->replayStep(10.0f);   // each call simulates one frame's budget
        // PIN THE SNAPSHOT FIRST. captureFrame alternates buffers without moving
        // renderReadIdx_, and this loop never calls beginFrame, so framedUnits() would
        // read buffer 0 -- not the buffer just published. A replay consumed in a single
        // step then reported framed=0 having published its units perfectly, and a
        // longer one reported a stale count: the diagnostic would accuse playback of
        // exactly the bug it exists to detect.
        gameView->beginFrame();
        // `framed` is the published snapshot's unit count -- what the renderer would
        // draw. It is reported next to the world count because they answer different
        // questions, and a mode that simulates without publishing shows units=N
        // framed=0, which is what an empty-looking playback looks like from here.
        std::fprintf(stderr, "replay done: tick=%zu hash=%016llx units=%zu framed=%zu%s\n",
                     gameView->replayTick(), (unsigned long long)gameView->worldHashPublic(),
                     gameView->aliveUnits(), gameView->framedUnits(),
                     gameView->replayDiverged() ? " DIVERGED" : "");
        // The winner's banner playback now shows (GameView::replayResult): only at the
        // end of a recording that ended with one team left.
        if (gameView->replayResult())
            std::fprintf(stderr, "replay result: team %d wins\n", gameView->frameWinningTeam() + 1);
        // FAIL when playback did not reproduce the recording. This returned 0
        // unconditionally, so an automated verify passed a recording it had just
        // detected diverging from -- the one thing the mode exists to catch.
        if (const std::string why = gameView->replayDivergenceSummary(); !why.empty())
            std::fprintf(stderr, "replay verify: %s\n", why.c_str());
        return gameView->replayDiverged() ? 1 : 0;
    }
    if (gameView && mp && mpHeadless) {
        std::string mapId = ta::vpath::stem(args[0]);
        // The harness runs the sim INLINE (single-threaded == deterministic + reproducible)
        // unless TA_SIM_THREAD asks to verify the threaded sim against the referee.
        gameView->setSimThreadMode(ta::devEnv("TA_SIM_THREAD") != nullptr);
        if (mpHeadless == 8) gameView->setMissionStem(missionStem);
        int limitTicks = int((startTime > 0 ? startTime : 60) * 30);
        gameView->setNetStopTick(uint32_t(limitTicks));
        // TA_DEFEAT_TEST=1 is the defeat probe: play on through the three-second results
        // transition. Any other value only issues the self-destroy, and the run ends at its
        // result like an ordinary one -- at exactly the result's tick (setNetStopAtOutcome).
        const char* defeatEnv = ta::devEnv("TA_DEFEAT_TEST");
        const bool defeatProbe = defeatEnv && std::string(defeatEnv) == "1";
        // TA_WATCH_END_TEST=1 is the watch probe, for a spectating run: play on past the
        // result until the server ends the game (Msg::GameOver), then through the same
        // three-second results transition, and report the result plate a watcher gets.
        const bool watchProbe = ta::devFlag("TA_WATCH_END_TEST");
        gameView->setNetStopAtOutcome(!defeatProbe && !watchProbe);
        // Jitter benchmark: run the client loop at a FIXED 60 fps (so the stall
        // metric is frame-rate-consistent) and enable the RTT probe. Otherwise the
        // usual tight poll loop.
        bool bench = ta::devEnv("TA_NETBENCH") != nullptr;
        if (bench) gameView->netEnableRttProbe();
        // TA_FRAME_MS=N: sleep N ms per loop iteration, standing in for the display's frame time (the
        // interactive IN-04 run drew about 20 frames/s). tools/pace_check.sh sets it with TA_PACELOG.
        const int frameMs = ta::devEnv("TA_FRAME_MS") ? std::clamp(std::atoi(ta::devEnv("TA_FRAME_MS")), 1, 200)
                                                         : (bench ? 16 : 2);
        // TA_VSYNC_HZ=N: a fake vsync instead -- each iteration ends on an N Hz deadline (the same
        // deadline pacer the interactive client falls back to), so 240/144/60 Hz frame cadences can be
        // measured headless (the TA_PACELOG PRESENT line). It overrides TA_FRAME_MS.
        const int vsyncHz = ta::devEnv("TA_VSYNC_HZ") ? std::clamp(std::atoi(ta::devEnv("TA_VSYNC_HZ")), 1, 1000) : 0;
        ta::PresentationPacer vsyncPacer;
        while (true) {
            // Pin the snapshot for the iteration (mirrors the interactive render loop), so
            // cosmeticStep's front() reads can't tear against the worker under TA_SIM_THREAD.
            // presentFrame advances the presentation clock exactly as a rendered frame does.
            gameView->presentFrame();
            bool cont = gameView->mpAutoStep(mpHeadless, mapId);
#ifndef NDEBUG
            gameView->debugWatchPoke();   // TA_WATCHONLY_POKE
#endif
            gameView->endFrame();
            // Ordinary headless runs finish at their result. The defeat probe waits
            // for the same three-second transition the interactive client uses.
            static uint64_t defeatAt = 0;
            if (defeatProbe && gameView->outcomePublic() < 0 && !defeatAt)
                defeatAt = SDL_GetTicks64();
            if (defeatProbe && gameView->menuRequested()) {
                const uint64_t delay = SDL_GetTicks64() - defeatAt;
                if (!defeatAt || delay < 2950 || delay > 4000)
                    throw std::runtime_error("defeat results transition did not take three seconds");
                std::fprintf(stderr, "PASS: automatic defeat results after %llu ms at tick %u\n",
                             (unsigned long long)delay, gameView->netTick());
                break;
            }
            static uint64_t watchOverAt = 0;
            if (watchProbe && gameView->watchOverPublic() && !watchOverAt) watchOverAt = SDL_GetTicks64();
            if (watchProbe && gameView->menuRequested()) {
                const uint64_t delay = SDL_GetTicks64() - watchOverAt;
                if (!watchOverAt || delay < 2950 || delay > 4000)
                    throw std::runtime_error("watched game's results transition did not take three seconds");
                const ta::ResultStats st = gameView->resultStats();
                std::string gold;   // the highlighted rows: the winners'
                for (const auto& row : st.rows) if (row.isLocal) gold += (gold.empty() ? "" : ",") + row.name;
                std::fprintf(stderr, "PASS: watched game over: results after %llu ms at tick %u, result tick %u; "
                             "plate victory=%d faction=%d line='%s' band='%s' matchSec=%d gold=%s\n",
                             (unsigned long long)delay, gameView->netTick(), gameView->outcomeTickPublic(),
                             int(gameView->resultVictory()), st.faction, st.watchLine.c_str(), st.bandWord.c_str(),
                             st.matchSec, gold.c_str());
                break;
            }
            if (!cont || (!defeatProbe && !watchProbe && gameView->outcomePublic() != 0)) break;
            // A watched game the server ended (Msg::GameOver) is over, result or none.
            if (!watchProbe && gameView->watchOverPublic()) break;
            if (int(gameView->netTick()) >= limitTicks) break;
            if (ta::termRequested()) break;   // SIGTERM/SIGINT/SIGHUP: tear down (and finish the replay)
            // TA_KILL_AT_TICK=N: die like a crash at tick N -- SIGKILL (TerminateProcess on
            // Windows), so no handler, destructor or flush runs. tools/replay_always_test.py
            // checks that the next start recovers nearly every tick received.
            static const int killAt = ta::devEnv("TA_KILL_AT_TICK") ? std::atoi(ta::devEnv("TA_KILL_AT_TICK")) : 0;
            if (killAt > 0 && int(gameView->netTick()) >= killAt) {
                std::fprintf(stderr, "kill-at-tick: killing the client at tick %u with %zu ticks received\n",
                             gameView->netTick(), mp->replayLog().size());
                std::fflush(stderr);
#ifdef _WIN32
                TerminateProcess(GetCurrentProcess(), 9);
#else
                std::raise(SIGKILL);
#endif
            }
            if (vsyncHz) vsyncPacer.paceAtRefresh(vsyncHz);
            else SDL_Delay(Uint32(frameMs));   // ~60 fps for the benchmark
        }
        // Flush + join the worker so the final world hash reflects every pushed tick (no read
        // race against a still-running worker). No-op when inline.
        gameView->shutdownSim();
        // Report WHY the run stopped, not just where. A game that concluded at tick
        // 20k and one that ran the clock out both used to print the same line, so a
        // short run was indistinguishable from a stall.
        const int endOutcome = gameView->outcomePublic();
        // result=: the tick the result landed (0 = none). A concluded run stops there, so it
        // equals tick= -- whatever the load, the end hash is that tick's.
        std::fprintf(stderr, "mp-headless done: tick=%u hash=%016llx units=%zu err=%s end=%s result=%u\n",
                     gameView->netTick(), (unsigned long long)gameView->worldHashPublic(),
                     gameView->aliveUnits(),
                     gameView->netError().empty() ? "none" : gameView->netError().c_str(),
                     endOutcome != 0 ? "concluded" : gameView->watchOverPublic() ? "gameover" : "timelimit",
                     gameView->outcomeTickPublic());
        if (const auto* legion = gameView->legionPublic()) {
            // Which mission goals Legion routed in this game (legs taken on,
            // arrivals raised, failed approaches handed back), by kind.
            static const char* const kinds[] = {"none","move","fight","patrol","attack","guard",
                "build","repair","reclaim","load","unload","exit","park"};
            const auto s = legion->stats();
            std::string line;
            for (size_t k = 1; k < std::size(kinds); ++k)
                line += std::string(" ") + kinds[k] + "=" + std::to_string(s.missionLegs[k]) + "/" +
                        std::to_string(s.missionArrivals[k]) + "/" + std::to_string(s.missionFailures[k]);
            std::fprintf(stderr, "legion missions (legs/arrivals/failed):%s\n", line.c_str());
        }
        if (mpHeadless == 8)
            std::fprintf(stderr, "mission %s outcome=%d (%s)\n", missionStem.c_str(),
                         gameView->missionOutcomePublic(),
                         gameView->missionOutcomePublic() > 0 ? "VICTORY"
                             : gameView->missionOutcomePublic() < 0 ? "DEFEAT" : "running");
        if (bench) {
            long f = gameView->netBenchFrames(), s = gameView->netBenchStalls();
            std::fprintf(stderr, "NETBENCH delay=%d rtt=%.0fms frames=%ld stalls=%ld (%.1f%%)\n",
                         gameView->netDelay(), gameView->netRttMs(), f, s,
                         f ? 100.0 * double(s) / double(f) : 0.0);
        }
        // --shot also works for the multiplayer/campaign harness, which bypasses
        // the interactive render loop. Capture the final synchronized snapshot.
        if (!shot.empty()) {
            int w=0,h=0;
            SDL_GetRendererOutputSize(ren,&w,&h);
            gameView->prepare(w,h);
            gameView->finishTerrain();
            gameView->beginFrame();
            gameView->cosmeticStep(0);
            if (ta::devEnv("TA_SHOT_SELECT")) gameView->selectFirstOwnForShot();
            gameView->draw(w,h);
            screenshot(ren,w,h,shot);
            gameView->endFrame();
        }
        mp->disconnect();
        return gameView->netError().empty() ? 0 : 1;
    }

    // Lobby driver for THIS session (TA_MPAUTO overrides mpAutoMode). Computed once
    // per session -- NOT a function-static, which would freeze it at the first game's
    // value and break re-entry (e.g. a Benchmark launched after any earlier game would
    // inherit that game's mode and just sit in the lobby instead of auto-hosting).
    const int autoOv = ta::devEnv("TA_MPAUTO")
                           ? std::atoi(ta::devEnv("TA_MPAUTO")) : mpAutoMode;
    if (gameView) gameView->setAudioTap(&gameStreamAudio,
        [](void* context, const int16_t* pcm, int frames, int channels) {
            auto& audio = *static_cast<GameStreamAudio*>(context);
            // During the lobby the menu player owns streamed audio. Appending
            // the game's silent buffers as well would double the audio timeline.
            if (!audio.menu->playing()) audio.stream->audio(pcm,frames,channels);
        });
    uint64_t last = SDL_GetPerformanceCounter();
#ifndef NDEBUG
    const bool renderStudy=gameView && !gameView->isNet() && testbuild && ta::devFlag("TA_RENDER_STUDY");
    const char* studyFrameText=ta::devEnv("TA_RENDER_FRAMES");
    const unsigned studyFrameLimit=unsigned(studyFrameText?std::clamp(std::atoi(studyFrameText),1,10000):900);
    unsigned studyFrame=0;
    void (APIENTRY *studyBeginQuery)(GLenum,GLuint)=nullptr;
    void (APIENTRY *studyEndQuery)(GLenum)=nullptr;
    void (APIENTRY *studyGetQuery)(GLuint,GLenum,uint64_t*)=nullptr;
    void (APIENTRY *studyDeleteQuery)(GLsizei,const GLuint*)=nullptr;
    // GPU draw-timeline queries (GL_TIME_ELAPSED). A ring, read only once the
    // driver reports the result AVAILABLE, so a pipelined run is never forced to
    // wait for the GPU just to be measured. A result arrives some frames later and
    // is printed as "GPUTIMELINE frame=<the frame it measured>". With glFinish
    // before present (TA_PROF_FINISH, forced completion) the current frame's
    // result is already available and also appears in DRAWPHASE/RENDERSTATS.
    // The timeline includes GPU idle gaps waiting for submission: it is NOT busy time.
    constexpr unsigned kStudyQueries=4;
    GLuint studyQueries[kStudyQueries]={};
    unsigned studyQueryFrame[kStudyQueries]={};
    bool studyQueryPending[kStudyQueries]={};
    bool studyQuery=false;
    uint64_t studyQueryStalls=0;   // ring full: had to wait (pipelined runs only)
    if(renderStudy && SDL_GL_ExtensionSupported("GL_ARB_timer_query")) {
        auto gen=reinterpret_cast<void(APIENTRY*)(GLsizei,GLuint*)>(SDL_GL_GetProcAddress("glGenQueries"));
        studyBeginQuery=reinterpret_cast<decltype(studyBeginQuery)>(SDL_GL_GetProcAddress("glBeginQuery"));
        studyEndQuery=reinterpret_cast<decltype(studyEndQuery)>(SDL_GL_GetProcAddress("glEndQuery"));
        studyGetQuery=reinterpret_cast<decltype(studyGetQuery)>(SDL_GL_GetProcAddress("glGetQueryObjectui64v"));
        studyDeleteQuery=reinterpret_cast<decltype(studyDeleteQuery)>(SDL_GL_GetProcAddress("glDeleteQueries"));
        if(gen && studyBeginQuery && studyEndQuery && studyGetQuery && studyDeleteQuery) {
            gen(GLsizei(kStudyQueries),studyQueries);
            studyQuery=studyQueries[kStudyQueries-1]!=0;
        }
    }
    // Reads one ring slot: blocking only when asked (ring full, or the end of the run).
    // Returns the elapsed ms, or a negative value when not available yet.
    const auto studyReadQuery=[&](unsigned slot,bool wait)->double {
        if(!studyQueryPending[slot])return -1;
        uint64_t value=0;
        if(!wait) {
            studyGetQuery(studyQueries[slot],0x8867,&value);   // GL_QUERY_RESULT_AVAILABLE
            if(!value)return -1;
        }
        studyGetQuery(studyQueries[slot],GL_QUERY_RESULT,&value);
        studyQueryPending[slot]=false;
        const double ms=double(value)/1e6;
        std::printf("GPUTIMELINE frame=%u ms=%.4f\n",studyQueryFrame[slot],ms);
        return ms;
    };
    if(renderStudy) {
        // Record what the run ACTUALLY got, so a benchmark can refuse a run whose
        // backend or output size differs from what it asked for.
        SDL_RendererInfo info{};
        SDL_GetRendererInfo(ren,&info);
        int outW=0,outH=0,winPtW=0,winPtH=0;
        SDL_GetRendererOutputSize(ren,&outW,&outH);
        SDL_GetWindowSize(win,&winPtW,&winPtH);
        float scaleX=1,scaleY=1;SDL_RenderGetScale(ren,&scaleX,&scaleY);
        const char* glVendor="-";const char* glRenderer="-";const char* glVersion="-";
        if(info.name && std::strncmp(info.name,"opengl",6)==0) {
            using GetString=const GLubyte*(APIENTRY*)(GLenum);
            if(auto get=reinterpret_cast<GetString>(SDL_GL_GetProcAddress("glGetString"))) {
                const auto str=[&](GLenum e){const auto* v=reinterpret_cast<const char*>(get(e));return v?v:"-";};
                glVendor=str(GL_VENDOR);glRenderer=str(GL_RENDERER);glVersion=str(GL_VERSION);
            }
        }
        const auto worldView=gameView->renderStudyWorldView(outW,outH);
        std::printf("RENDER_ENV video=%s renderer=%s accelerated=%d vsync=%d target=%d output=%dx%d window=%dx%d world=%dx%d scale=%.3fx%.3f max_texture=%dx%d gl_vendor=\"%s\" gl_renderer=\"%s\" gl_version=\"%s\" timer_query=%d\n",
            SDL_GetCurrentVideoDriver()?SDL_GetCurrentVideoDriver():"-",info.name?info.name:"-",
            (info.flags&SDL_RENDERER_ACCELERATED)?1:0,(info.flags&SDL_RENDERER_PRESENTVSYNC)?1:0,
            (info.flags&SDL_RENDERER_TARGETTEXTURE)?1:0,outW,outH,winPtW,winPtH,worldView.first,worldView.second,scaleX,scaleY,
            info.max_texture_width,info.max_texture_height,glVendor,glRenderer,glVersion,studyQuery?1:0);
        std::printf("RENDER_TIMING frame_mode=%s gpu_timeline=%s gpu_busy=unmeasured telemetry=%s\n",
            ta::devFlag("TA_PROF_FINISH") ? "forced_completion" : "pipelined",
            studyQuery ? "gl_time_elapsed_deferred" : "unavailable",
            ta::devFlag("TA_RENDER_STATS",true) ? "on" : "off");
    }
    // Matrix knobs for the render study (tools/render_benchmark.py). Frame-indexed,
    // so every run of a scene sees the identical camera and event schedule.
    const auto studyPeriod=[](const char* name){const char* v=ta::devEnv(name);return v?unsigned(std::max(0,std::atoi(v))):0u;};
    const unsigned studyJump=studyPeriod("TA_RENDER_JUMP");
    const unsigned studyReset=studyPeriod("TA_RENDER_RESET");
    const unsigned studyChurn=studyPeriod("TA_RENDER_CHURN");
    // TA_RENDER_FULLSCREEN=<period>: toggle desktop fullscreen every <period> frames, the
    // way Options does (renderer window-event checks). TA_RENDER_CAPTURE_EVERY=<period>:
    // with TA_RENDER_CAPTURE_DIR, also capture every <period> frames.
    const unsigned studyFullscreen=studyPeriod("TA_RENDER_FULLSCREEN");
    const unsigned studyCaptureEvery=studyPeriod("TA_RENDER_CAPTURE_EVERY");
    const unsigned studyEpoch=studyPeriod("TA_RENDER_EPOCH");   // world-epoch bump (rejoin id reuse)
    const unsigned studyShake=studyPeriod("TA_RENDER_SHAKE");   // camera shake every N frames (workstream L)
    const unsigned studyVeteran=studyPeriod("TA_RENDER_VETERAN");   // veterancy cycle (material-only)
    const unsigned studyFilter=studyPeriod("TA_RENDER_FILTER");     // Texture Filtering Off<->Units (workstream M)
    const unsigned studyPanSnap=studyPeriod("TA_RENDER_CAMERA_SNAP");  // whole-pixel pan (1, or N moving/N still)
    const unsigned studySparseAfter=studyPeriod("TA_RENDER_SPARSE_AFTER");
    // Perf follow-up C: TA_RENDER_CURSOR=1 drives the pointer over the world view
    // (GameView::renderStudyCursor) and prints the frame's cursor evaluation
    // (CURSORSTATS); TA_RENDER_CURSOR_SELECT=<n> keeps up to n own units selected,
    // TA_RENDER_CURSOR_CMD=<c> arms order c ('a' attack, 'r' repair, 'g' guard,
    // 'l' load, 'm' move..., '*' each in turn).
    const bool studyCursor=renderStudy && ta::devFlag("TA_RENDER_CURSOR");
    const int studyCursorSelect=int(studyPeriod("TA_RENDER_CURSOR_SELECT"));
    const char studyCursorCmd=ta::devEnv("TA_RENDER_CURSOR_CMD") ? ta::devEnv("TA_RENDER_CURSOR_CMD")[0] : 0;
    float studySparseX=200,studySparseZ=200;
    if(const char* at=ta::devEnv("TA_RENDER_SPARSE_OFFSET"))std::sscanf(at,"%f,%f",&studySparseX,&studySparseZ);
    // TA_RENDER_GPU_CAP_AT=<frame>,<MiB>[/<frame>,<MiB>...]: from each frame on, the
    // tracked-GPU cap is <MiB>, or with 0 what is held at that frame (TA_GPU_CAP_MIB
    // mid-run: a limit lowered under a held working set, so the caches that obey the
    // cap give back -- and raised again).
    std::vector<std::pair<unsigned,unsigned>> studyCaps;
    if(const char* list=ta::devEnv("TA_RENDER_GPU_CAP_AT"))
        for(const char* at=list;at && *at;) {
            unsigned frame=0,mib=0;
            if(std::sscanf(at,"%u,%u",&frame,&mib)==2)studyCaps.push_back({frame,mib});
            at=std::strchr(at,'/');if(at)++at;
        }
    // TA_RENDER_SHADOWS_OFF_AT=<frame>: switch unit shadows off from that frame (the
    // Options SHADOWS toggle; perf follow-up K: what the shadow atlas holds meanwhile).
    const unsigned studyShadowsOffAt=studyPeriod("TA_RENDER_SHADOWS_OFF_AT");
    // Workstream K (terrain under camera motion). TA_RENDER_SWEEP=cx,cz,ax,az,period:
    // an elliptical sweep around (cx,cz) with amplitudes ax/az world px and the given
    // period in frames (fast pan with reversals in both axes). TA_RENDER_ZOOM_OSC=
    // lo,hi,period: zoom oscillating between lo and hi (across a chunk-level boundary).
    // TA_RENDER_JUMP_AT="x,z;x,z;..." replaces the four TA_RENDER_JUMP points.
    float studySweep[5]={};
    const bool studySweepOn=ta::devEnv("TA_RENDER_SWEEP") &&
        std::sscanf(ta::devEnv("TA_RENDER_SWEEP"),"%f,%f,%f,%f,%f",&studySweep[0],&studySweep[1],
                    &studySweep[2],&studySweep[3],&studySweep[4])==5 && studySweep[4]>0;
    float studyZoomOsc[3]={};
    const bool studyZoomOscOn=ta::devEnv("TA_RENDER_ZOOM_OSC") &&
        std::sscanf(ta::devEnv("TA_RENDER_ZOOM_OSC"),"%f,%f,%f",&studyZoomOsc[0],&studyZoomOsc[1],&studyZoomOsc[2])==3 &&
        studyZoomOsc[2]>0;
    std::vector<std::pair<float,float>> studyJumpAt={{3800,3800},{6200,3800},{6200,6200},{3800,6200}};
    if(const char* list=ta::devEnv("TA_RENDER_JUMP_AT")) {
        studyJumpAt.clear();
        for(const char* at=list;at && *at;) {
            float x=0,z=0;
            if(std::sscanf(at,"%f,%f",&x,&z)==2)studyJumpAt.push_back({x,z});
            at=std::strchr(at,';');if(at)++at;
        }
        if(studyJumpAt.empty())studyJumpAt.push_back({3800,3800});
    }
    // Interactive cases (measurement B, tools/render_benchmark.py --scenes interactive).
    // TA_RENDER_MOUSE puts the pointer in the world view every frame, in fractions of
    // that view: "fx,fy" holds it still; "sweep:x0,y0,x1,y1" moves it on a Lissajous
    // path inside that rectangle (periods 300 and 187 frames). The in-game cursor is then
    // classified each frame as under a real mouse (the matrix scenes leave the pointer
    // unset, which skips that work). TA_RENDER_FADE_CLOCK=1 runs HIDDEN UNITS FADE on
    // the study frame clock (1/60 s a frame) instead of wall time, so fades are identical
    // in every run and capture.
    int studyMouseMode=0;   // 0 unset, 1 fixed, 2 sweep
    float studyMouse[4]={};
    if(const char* mouse=ta::devEnv("TA_RENDER_MOUSE")) {
        if(std::sscanf(mouse,"sweep:%f,%f,%f,%f",&studyMouse[0],&studyMouse[1],&studyMouse[2],&studyMouse[3])==4)
            studyMouseMode=2;
        else if(std::sscanf(mouse,"%f,%f",&studyMouse[0],&studyMouse[1])==2)
            studyMouseMode=1;
        else
            throw std::runtime_error(std::string("TA_RENDER_MOUSE: expected fx,fy or sweep:x0,y0,x1,y1, got ")+mouse);
    }
    const bool studyFadeClock=ta::devFlag("TA_RENDER_FADE_CLOCK");
    if(renderStudy && (studyMouseMode || studyFadeClock))
        std::printf("RENDER_INTERACTIVE mouse=%s fade_clock=%d\n",
                    studyMouseMode==2 ? "sweep" : studyMouseMode==1 ? "fixed" : "unset",studyFadeClock?1:0);
#endif
    auto windowTarget = [&] {
        SDL_SetRenderTarget(ren, nullptr);
        SDL_RenderSetScale(ren, 1.0f, 1.0f);
        SDL_RenderSetViewport(ren, nullptr);
        SDL_RenderSetClipRect(ren, nullptr);
    };
    while (running) {
        if (ta::termRequested()) { running = false; quitApp = true; break; }
        // SDL stores viewport/scale per target. Always begin on the current
        // drawable, including after a resize or an interrupted off-screen pass.
        windowTarget();
        // Pin the newest published sim snapshot for this whole iteration -- input handlers
        // (below) AND the render pass (further down) read front(), so the pin must span both
        // so a concurrent publish from the sim worker (Stage B) can't tear them. Released by
        // endFrame() after the cursor overlay, once every front()-reading pass is done.
        // presentFrame picks the snapshot the presentation clock shows this frame (WE S4).
        if (gameView) gameView->presentFrame();
        SDL_Event e;
        bool targetsReset = false, windowResized = false;
        while (SDL_PollEvent(&e)) {
            // The window-manager close button (title-bar X) fires SDL_QUIT; we
            // deliberately IGNORE it so it can't yank the player out of a game. Quit
            // only through real paths: the menu's Exit door, the in-game QUIT button
            // (quitRequested_ below), or Escape in the asset viewers.
            if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE && !gameView) {
                running = false; quitApp = true;   // asset-viewer Esc -> quit the app
            }
            // The GPU lost every render-target texture's contents (device/driver
            // reset). Rebuild the baked atlases so sprites don't blink out.
            if (e.type == SDL_RENDER_TARGETS_RESET || e.type == SDL_RENDER_DEVICE_RESET)
                targetsReset = true;
            // A device reset loses every texture; the UI font's glyph sheets rebuild on
            // their next draw.
            if (e.type == SDL_RENDER_DEVICE_RESET) ta::releaseUiTextGlyphs();
            if (e.type == SDL_WINDOWEVENT &&
                (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED))
                windowResized = true;
            // 'S' grabs a screenshot in the asset viewers; in game it is the
            // Stop hotkey (Keys.TDF LOWER_S), handled by GameView::input.
            // Capture at the CURRENT output size, not the default window constants --
            // the window is resizable (and may be fullscreen), so kWinW/kWinH go stale
            // the moment it is dragged, and the grab then reads the wrong rectangle.
            if (!gameView && e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_s) {
                int sw, sh;
                SDL_GetRendererOutputSize(ren, &sw, &sh);
                screenshot(ren, sw, sh, "taclient_shot.png");
            }
            int ww, wh;
            SDL_GetRendererOutputSize(ren, &ww, &wh);
            // Mouse events arrive in window points; the renderer (and all our
            // world<->screen math) works in output pixels. Map between them with
            // SDL_RenderWindowToLogical, which uses SDL's INTERNAL window<->drawable
            // mapping -- reliable even on Wayland fractional scaling, where the size
            // getters report window==drawable yet pointer events are in a smaller
            // logical space (SDL_GetWindowSize-based rescaling was a no-op there).
            if (e.type == SDL_MOUSEMOTION) {
                float lx, ly, lx0, ly0;
                SDL_RenderWindowToLogical(ren, e.motion.x, e.motion.y, &lx, &ly);
                SDL_RenderWindowToLogical(ren, e.motion.x - e.motion.xrel,
                                          e.motion.y - e.motion.yrel, &lx0, &ly0);
                e.motion.x = int(lx); e.motion.y = int(ly);
                e.motion.xrel = int(lx - lx0); e.motion.yrel = int(ly - ly0);
            } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
                float lx, ly;
                SDL_RenderWindowToLogical(ren, e.button.x, e.button.y, &lx, &ly);
                e.button.x = int(lx); e.button.y = int(ly);
            }
            // TOGGLE FULLSCREEN (F11 by default), in game as on the menu.
            if (ta::toggleFullscreenKey(e, settings.hotkeys, settings.fullscreen)) {
                ta::saveSettings(settings);
                continue;
            }
            if (gameView && streaming.input(e,ww,wh)) continue;
            if (mapView) mapView->input(e);
            if (modelView) modelView->input(e);
            if (gameView) gameView->input(e, ww, wh);
        }
        if (targetsReset || windowResized) {
            windowTarget();
            if (targetsReset) {
                if (gameView) gameView->invalidateRenderTargets();
                if (mapView) mapView->invalidateRenderTargets();
            }
        }
        uint64_t now = SDL_GetPerformanceCounter();
        float dt = float(now - last) / float(SDL_GetPerformanceFrequency());
        last = now;
#ifndef NDEBUG
        if(renderStudy)dt=1.f/60;
        // Repeatable --shot captures (A/B image diffs): the frame clock advances a fixed
        // 1/60s instead of wall-clock time, so the sim and animations land identically.
        else if(!shot.empty() && ta::devFlag("TA_SHOT_FIXED_DT"))dt=1.f/60;
#endif
        // FPS readout in the window title (updated ~4x/sec).
        {
            static float fpsAcc = 0; static int fpsFrames = 0;
            fpsAcc += dt; ++fpsFrames;
            if (fpsAcc >= 0.25f) {
                char title[64];
                std::snprintf(title, sizeof title, "Total Annihilation %s  |  %.0f fps",
                              ta::kVersion, float(fpsFrames) / fpsAcc);
                SDL_SetWindowTitle(win, title);
                fpsAcc = 0; fpsFrames = 0;
            }
        }
        if (ktPhase >= 0) {
            ktClock += dt;
            auto click = [&](int x, int y, uint8_t btn) {
                SDL_Event ev{};
                ev.type = SDL_MOUSEBUTTONDOWN;
                ev.button.button = btn;
                ev.button.x = x;
                ev.button.y = y;
                SDL_PushEvent(&ev);
                ev.type = SDL_MOUSEBUTTONUP;
                SDL_PushEvent(&ev);
            };
            auto key = [&](SDL_Keycode k) {
                SDL_Event ev{};
                ev.type = SDL_KEYDOWN;
                ev.key.keysym.sym = k;
                SDL_PushEvent(&ev);
            };
            auto motion = [&](int x, int y) {
                SDL_Event ev{};
                ev.type = SDL_MOUSEMOTION;
                ev.motion.x = x;
                ev.motion.y = y;
                SDL_PushEvent(&ev);
            };
            if (ktPhase == 0 && ktClock > 0.3f) {
                {
                    auto [pick, ucount] = gameView->keytestPickOwnUnit();
                    if (pick >= 0) {
                        gameView->selectOnly(pick);
                        std::fprintf(stderr, "KEYTEST select unit %d (of %zu units)\n",
                                     pick, ucount);
                        // --selonly: hold this selection for the shot (no map clicks,
                        // which would deselect). Otherwise continue the order test.
                        if (keytestSelectOnly) { std::printf("KEYTEST done\n"); ktPhase = -1; }
                        else ktPhase = 1;
                    }
                    // else: units not synced yet -- retry next frame (stay in phase 0)
                }
            }

            else if (ktPhase == 1 && ktClock > 0.6f) { key(SDLK_f); ktPhase = 2; }
            else if (ktPhase == 2 && ktClock > 0.9f) { motion(400, 453); ktPhase = 3; }
            else if (ktPhase == 3 && ktClock > 1.2f) { click(400, 453, SDL_BUTTON_LEFT); ktPhase = 4; }
            else if (ktPhase == 4 && ktClock > 1.5f) { click(253, 453, SDL_BUTTON_LEFT); motion(1250, 245); ktPhase = 5; }
            else if (ktPhase == 5 && ktClock > 1.9f) {
                std::printf("KEYTEST done\n");
                ktPhase = -1;
            }
        }

        if (gameView && dt > 0) gameView->setFps(1.0f / dt);
        int w, h;
        SDL_GetRendererOutputSize(ren, &w, &h);
        // World passes own independent AA; the window and UI remain native.
        if (mapView) mapView->ensureChunks(w, h);
        if (gameView) gameView->prepare(w, h);
        windowTarget();
        SDL_SetRenderDrawColor(ren, 18, 18, 26, 255);
        SDL_RenderClear(ren);
        // Optional per-phase profiler (TA_PROF=1): prints where each frame's
        // wall-clock goes, once a second, so a stall can be localised on real
        // hardware that the headless software renderer can't show.
        static const bool prof = ta::devEnv("TA_PROF") != nullptr;
        static const bool profFrames = ta::devFlag("TA_PROF_FRAMES");
        static double pUpd = 0, pDraw = 0, pPres = 0, pAcc = 0;
        static int pFrames = 0;
        auto pnow = [] { return double(SDL_GetPerformanceCounter()) /
                         double(SDL_GetPerformanceFrequency()) * 1000.0; };
        double t0 = prof ? pnow() : 0;
        if (mapView) mapView->draw(w, h);
        if (modelView) modelView->draw(w, h, dt);
        double t1 = prof ? pnow() : 0;
        // Hoisted out of the gameView block so the spike logger can report the REAL
        // update (t1->t2) and draw (t2->t3) intervals separately.
        double t2 = t1, t3 = t1, animationMs = 0;
        if (gameView) {
            // Real-time camera/audio every frame, BEFORE the sim step -- so pan,
            // edge-scroll, follow, shake and music stay smooth even when a net
            // game's sim is stalled waiting on a bundle (the deferred netAccum-style
            // decoupling this comment used to promise).
            // Once the benchmark stats screen is up the game is PAUSED behind it:
            // no camera, sim stepping or animation until DONE/Esc returns to the menu.
            const bool benchFrozen = gameView->benchmarkStatsShown();
            if (!benchFrozen) gameView->cameraFrame(dt);
            if (benchFrozen) {
                // frozen -- fall through to draw() so the overlay still renders
#ifndef NDEBUG
                // TA_QUIT_TICK: the results are as far as a benchmark run goes (after
                // TA_BENCH_CLICKS has worked its buttons, if that is set).
                if (ta::devEnv("TA_QUIT_TICK") && gameView->benchClicksDone()) { running = false; quitApp = true; }
#endif
            } else if (gameView->replayMode()) {
                gameView->replayStep(dt);   // play back a recorded .tarep
            } else if (gameView->isNet()) {
                // Server-sequenced lockstep: one mpAutoStep pumps the connection,
                // advances the lobby (auto-matchmaking for now -- a lobby UI is
                // follow-on), and simulates every delivered tick. (void)netAccum.
                (void)netAccum;
                // autoOv (computed once per session above) drives the lobby: 0 =
                // UI-driven, 1 = auto-host, etc. TA_MPAUTO can override it.
                gameView->mpAutoStep(autoOv, serverMapId);
#ifndef NDEBUG
                // TA_QUIT_TICK=N: leave once the game reaches tick N (or the benchmark's stats
                // screen is up, above), so a short real-display pacing run (TA_PACELOG) ends by itself.
                if (const char* qt = ta::devEnv("TA_QUIT_TICK");
                    qt && gameView->netTick() >= uint32_t(std::max(1, std::atoi(qt)))) {
                    running = false; quitApp = true;
                }
#endif
                if (gameView->consumeBriefingPause()) {
                    last=SDL_GetPerformanceCounter();
                    dt=0;   // paused reading time is not animation time
                }
            } else {
                gameView->update(dt);
            }
            // Advance unit animation every render frame (decoupled from the 30Hz sim tick),
            // so the walk cycle is smooth at display rate and the heavy parallel VM pass no
            // longer piles onto the 1-in-8 net frame that runs the sim tick.
            if (!benchFrozen) {
                gameView->benchmarkCamera(dt, w, h);   // benchmark flythrough (no-op otherwise)
#ifndef NDEBUG
                if(renderStudy && studyPanSnap)gameView->renderStudyPanSnap(studyFrame,studyPanSnap);
                else if(renderStudy && studySweepOn) {
                    const float phase=float(studyFrame)*6.2831853f/studySweep[4];
                    gameView->lookAt(studySweep[0]+studySweep[2]*std::sin(phase),studySweep[1]+studySweep[3]*std::cos(phase));
                }
                else if(renderStudy && ta::devFlag("TA_RENDER_CAMERA")) {
                    const float phase=float(studyFrame)*.012f;
                    gameView->lookAt(5000+240*std::sin(phase),5000+160*std::cos(phase));
                }
                if(renderStudy && studyVeteran)gameView->renderStudyVeteran(studyFrame,studyVeteran);
                if(renderStudy && ta::devFlag("TA_RENDER_ZOOM"))
                    gameView->renderStudyZoom(.25f+.75f*(.5f+.5f*std::sin(float(studyFrame)*.008f)));
                else if(renderStudy && studyZoomOscOn)
                    gameView->renderStudyZoom(studyZoomOsc[0]+(studyZoomOsc[1]-studyZoomOsc[0])*
                        (.5f+.5f*std::sin(float(studyFrame)*6.2831853f/studyZoomOsc[2])));
                if(renderStudy && ta::devFlag("TA_RENDER_MOTION"))gameView->renderStudyMotion(studyFrame);
                if(renderStudy && studyJump && studyFrame%studyJump==0) {
                    // Camera jumps between distant points (no shared view; four by default).
                    const auto& p=studyJumpAt[(studyFrame/studyJump)%studyJumpAt.size()];
                    gameView->lookAt(p.first,p.second);
                }
                if(renderStudy && studyChurn)gameView->renderStudyChurn(studyFrame,studyChurn);
                if(studyCursor)gameView->renderStudyCursor(studyFrame,studyCursorSelect,studyCursorCmd);
                if(renderStudy && studyEpoch && studyFrame && studyFrame%studyEpoch==0)
                    gameView->renderStudyBumpWorldEpoch();
                if(renderStudy && studyShake && studyFrame%studyShake==0)
                    gameView->renderStudyShake(8.0f,float(studyShake)/90.0f);
                if(renderStudy && studySparseAfter && studyFrame>=studySparseAfter)
                    gameView->renderStudyOffset(studySparseX,studySparseZ);
                for(const auto& [capFrame,capMiB]:studyCaps)
                    if(renderStudy && studyFrame==capFrame) {
                        const size_t cap=capMiB ? size_t(capMiB)*gpuvram::kMiB : gpuvram::bytes();
                        gpuvram::g_policy.limit=gpuvram::g_policy.cap=cap;
                        std::printf("RENDER_GPU_CAP frame=%u cap=%zu KiB held=%zu KiB\n",studyFrame,cap>>10,
                                    gpuvram::bytes()>>10);
                    }
                if(renderStudy && studyShadowsOffAt && studyFrame==studyShadowsOffAt)
                    gameView->renderStudyShadows(false);
                if(renderStudy && studyFilter && studyFrame && studyFrame%studyFilter==0)
                    gameView->renderStudyToggleFilter();
                if(renderStudy && studyFullscreen && studyFrame && studyFrame%studyFullscreen==0) {
                    const bool fs=(SDL_GetWindowFlags(win)&SDL_WINDOW_FULLSCREEN_DESKTOP)==SDL_WINDOW_FULLSCREEN_DESKTOP;
                    std::printf("RENDER_FULLSCREEN frame=%u -> %s rc=%d\n",studyFrame,fs?"windowed":"fullscreen",
                                SDL_SetWindowFullscreen(win,fs?0:SDL_WINDOW_FULLSCREEN_DESKTOP));
                }
                if(renderStudy && studyReset && studyFrame && studyFrame%studyReset==0) {
                    // The SDL_RENDER_TARGETS_RESET path, on a fixed schedule.
                    windowTarget();
                    gameView->invalidateRenderTargets();
                }
                if(renderStudy && studyMouseMode) {
                    const auto view=gameView->renderStudyWorldView(w,h);
                    float fx=studyMouse[0],fy=studyMouse[1];
                    if(studyMouseMode==2) {
                        fx+=(studyMouse[2]-studyMouse[0])*(.5f+.5f*std::sin(float(studyFrame)*6.2831853f/300));
                        fy+=(studyMouse[3]-studyMouse[1])*(.5f+.5f*std::sin(float(studyFrame)*6.2831853f/187));
                    }
                    gameView->renderStudyMouse(fx*float(view.first),fy*float(view.second));
                }
                if(renderStudy && studyFadeClock)gameView->renderStudyFadeClock(1+uint64_t(studyFrame)*1000/60);
#endif
#ifndef NDEBUG
                if(ta::devFlag("TA_PROFILE_PAN"))
                    gameView->lookAt(15000+1000*std::sin(float(SDL_GetTicks64())*.0004f),14000);
#endif
                const double animationStart = prof ? pnow() : 0;
                gameView->animFrame(dt);
                if (prof) animationMs = pnow() - animationStart;
                gameView->benchmarkSample();   // perf samples at each 10s milestone (no-op unless benchmarking)
            }
            t2 = prof ? pnow() : 0;
#ifndef NDEBUG
            if(studyQuery) {
                const unsigned slot=studyFrame%kStudyQueries;
                if(studyQueryPending[slot]) {studyReadQuery(slot,true);++studyQueryStalls;}
                studyBeginQuery(0x88bf,studyQueries[slot]); // GL_TIME_ELAPSED
                studyQueryFrame[slot]=studyFrame;studyQueryPending[slot]=true;
            }
#endif
            gameView->pumpVoice();   // voice chat: frames in and out, push-to-talk upkeep
            gameView->posterStep(w, h);   // a MakePoster tile, drawn and read back before the frame
            // The stream banner is drawn after the capture (Streaming::frame); the HUD's
            // top-left overlays still start below it, on screen and in the broadcast alike.
            gameView->setStreamBannerBottom(streaming.bannerBottom(w));
            gameView->draw(w, h);
#ifndef NDEBUG
            if(studyQuery) {
                SDL_RenderFlush(ren); // include SDL's queued world draws in the query
                studyEndQuery(0x88bf);
            }
#endif
            t3 = prof ? pnow() : 0;
#ifndef NDEBUG
            if(renderStudy && (studyFrame==360 || (studyFrame>=480 && studyFrame<490) || studyFrame==720 ||
                               (studyCaptureEvery && studyFrame%studyCaptureEvery==0)))
                if(const char* directory=ta::devEnv("TA_RENDER_CAPTURE_DIR")) {
                    const auto path=ta::u8path(directory)/("frame-"+std::to_string(studyFrame)+".png");
                    const auto utf8=path.u8string();
                    screenshot(ren,w,h,std::string(utf8.begin(),utf8.end()));
                }
#endif
            if (prof) { pUpd += t2 - t1; pDraw += t3 - t2; }
            // Feed the whole real frame time (dt = last frame's total incl. present)
            // to the sprite auto-tuner, so a GPU-bound full-model crowd triggers it.
        }
        double t4 = prof ? pnow() : 0;
        // Custom animated mouse cursor, drawn last so it sits above the HUD (and above
        // the AA-resolved scene) at native resolution. Only in-game; the asset viewers
        // keep the OS arrow. A stream keeps the HARDWARE cursor: it is not in the
        // captured frame, so the stream worker stamps it in (docs/streaming.md). With
        // the setup panel open it is the plain arrow, for clicking the panel. A
        // SOFTWARE cursor is drawn before the capture, so the broadcast has it already,
        // then drawn again over the private panel, which is drawn after the capture.
        if (gameView) {
            const bool panel = streaming.shown();
            if (gameView->hardwareCursorInUse()) {
                gameView->drawCursorOverlay(panel);
                streaming.frame(w,h);
            } else {
                gameView->drawCursorOverlay();
                streaming.frame(w,h);
                if (panel) gameView->drawCursorOverlay(true);
            }
#ifndef NDEBUG
            if (studyCursor) {
                const auto& c = gameView->renderStudyCursorStats();
                std::printf("CURSORSTATS frame=%u ns=%llu tests=%llu cursor=%d tint=%d x=%.1f y=%.1f\n", studyFrame,
                            static_cast<unsigned long long>(c.ns), static_cast<unsigned long long>(c.tests),
                            c.cursor, c.tint ? 1 : 0, c.x, c.y);
            }
#endif
        }
        // Read both metrics from the pinned snapshot before releasing it.
        const uint32_t profTick = prof && gameView ? gameView->framedTick() : 0;
        const size_t profLive = prof && gameView ? gameView->framedAliveUnits() : 0;
        if (gameView) gameView->endFrame();   // release the pinned sim snapshot for this frame
#ifndef NDEBUG
        // Offscreen Present need not wait for the GPU. Use this for completed-frame
        // profiling and capture without changing normal rendering or vsync pacing.
        if (prof && ta::devFlag("TA_PROF_FINISH")) {
            SDL_RenderFlush(ren);
            auto finish=reinterpret_cast<void(APIENTRY*)()>(SDL_GL_GetProcAddress("glFinish"));
            if (finish) finish();
        }
        static bool profileCaptured=false;
        // TA_PROFILE_CAPTURE_MS moves the 10s capture later, e.g. until a large
        // fixture has registered every model (64 a frame) at a low frame rate.
        static const uint64_t profileCaptureMs=ta::devEnv("TA_PROFILE_CAPTURE_MS")
            ? uint64_t(std::max(0,std::atoi(ta::devEnv("TA_PROFILE_CAPTURE_MS")))) : 10000;
        if (!profileCaptured && SDL_GetTicks64()>profileCaptureMs)
            if (const char* path=ta::devEnv("TA_PROFILE_CAPTURE")) {
                screenshot(ren,w,h,path);profileCaptured=true;
            }
        // A "shot:" step of TA_BENCH_CLICKS: the Benchmark results as about to be shown.
        if (gameView)
            if (const std::string path = gameView->takeBenchShot(); !path.empty()) screenshot(ren, w, h, path);
#endif
        SDL_RenderPresent(ren);
#ifndef NDEBUG
        if(renderStudy && profFrames) {
            // Results that are ready, oldest first, without waiting for the GPU.
            double gpuMs=-1;
            if(studyQuery)
                for(unsigned age=kStudyQueries;age-->0;) {
                    const unsigned slot=(studyFrame+kStudyQueries-age)%kStudyQueries;
                    const unsigned measured=studyQueryFrame[slot];
                    const double ms=studyReadQuery(slot,false);
                    if(ms>=0 && measured==studyFrame)gpuMs=ms;
                }
            double pj=0,sb=0,sh=0,at=0,bd=0;
            gameView->profPeek(pj,sb,sh);gameView->profOther2(at,bd);
            static double lastPj=0,lastSb=0,lastSh=0,lastAt=0,lastBd=0;
            size_t vertices=0,calls=0,images=0,bakes=0,bakeVertices=0,bakeCalls=0,switches=0,imageBytes=0;
            gameView->renderWork(vertices,calls,images,bakes,bakeVertices,bakeCalls,switches,imageBytes);
            std::printf("DRAWPHASE frame=%u proj=%.3f submit=%.3f shadow=%.3f atlas=%.3f body=%.3f gpu=%.3f vram=%zu body_vertices=%zu body_calls=%zu images=%zu image_bakes=%zu bake_vertices=%zu bake_calls=%zu cache_switches=%zu image_bytes=%zu\n",
                studyFrame,pj-lastPj,sb-lastSb,sh-lastSh,at-lastAt,bd-lastBd,
                gpuMs,gpuvram::bytes(),vertices,calls,images,bakes,bakeVertices,bakeCalls,switches,imageBytes);
            lastPj=pj;lastSb=sb;lastSh=sh;lastAt=at;lastBd=bd;
            static char statsLine[16384];
            gameView->renderStats().format(statsLine,sizeof statsLine);
            std::printf("RENDERSTATS frame=%u gpu_timeline_ms=%.4f zoom=%.4f gpu_cap_bytes=%zu gpu_query_stalls=%llu%s\n",studyFrame,
                gpuMs,gameView->renderStudyZoomNow(),gpuvram::cap(),
                static_cast<unsigned long long>(studyQueryStalls),statsLine);
        } else if(gameView && !renderStudy) {
            // Outside the study, TA_RENDER_STATS=1 logs one frame's telemetry a second
            // (no GPU timing: that needs the study's query ring).
            static const bool statsLog=ta::devFlag("TA_RENDER_STATS");
            static uint64_t statsLogAt=0,statsFrame=0;
            ++statsFrame;
            if(statsLog && SDL_GetTicks64()-statsLogAt>=1000) {
                statsLogAt=SDL_GetTicks64();
                static char statsLine[16384];
                gameView->renderStats().format(statsLine,sizeof statsLine);
                std::printf("RENDERSTATS frame=%llu gpu_timeline_ms=-1%s\n",
                    static_cast<unsigned long long>(statsFrame),statsLine);
            }
        }
        if(renderStudy && ++studyFrame==studyFrameLimit) {
            // Pipelined runs: collect the last frames' timelines (the run is over, so
            // waiting here perturbs nothing that is measured).
            if(studyQuery)
                for(unsigned age=kStudyQueries;age-->0;)
                    studyReadQuery((studyFrame+kStudyQueries-age)%kStudyQueries,true);
            std::printf("RENDER_STUDY frames=%u hash=%016llx gpu_textures=%.2fMiB\n",studyFrame,
                static_cast<unsigned long long>(gameView->worldHashPublic()),
                double(gpuvram::bytes())/(1<<20));
            if(ta::devFlag("TA_REQUIRE_PALETTE")) {
                // Workstream M: on OpenGL the structures draw from the indexed form (and,
                // with TA_RENDER_FILTER, switch to the baked pages and back) without a
                // failed indexed draw; elsewhere they stay on the baked pages.
                const auto& t=gameView->paletteTotals();
                using ull=unsigned long long;
                SDL_RendererInfo info{};
                SDL_GetRendererInfo(ren,&info);
                const bool gl=info.name && std::strcmp(info.name,"opengl")==0;
                std::printf("PALETTE_STUDY renderer=%s indexed_frames=%llu baked_frames=%llu indexed_draws=%llu spans=%llu "
                            "fallbacks=%llu switches=%llu upload_bytes=%llu bake_runs=%llu\n",info.name ? info.name : "?",
                    ull(t.indexedFrames),ull(t.bakedFrames),ull(t.indexedDraws),ull(t.spans),ull(t.fallbacks),
                    ull(t.switches),ull(t.uploadBytes),ull(t.bakeRuns));
                if(t.fallbacks)throw std::runtime_error("an indexed palette draw failed");
                if(gl && (!t.indexedDraws || !t.indexedFrames))throw std::runtime_error("OpenGL study drew no indexed palette runs");
                if(!gl && t.indexedFrames)throw std::runtime_error("indexed palette form on a renderer without it");
                if(gl && studyFilter && (t.switches<2 || !t.bakedFrames))
                    throw std::runtime_error("texture-filter toggles did not switch the palette form both ways");
                // TA_REQUIRE_PALETTE=bakes: the scene must bake indexed runs into distant
                // images (structures at zoom 0.4): they must reach drawModelRun.
                const char* require=ta::devEnv("TA_REQUIRE_PALETTE");
                if(gl && require && !std::strcmp(require,"bakes") && !t.bakeRuns)
                    throw std::runtime_error("OpenGL study baked no indexed palette run through drawModelRun");
            }
            if(ta::devFlag("TA_NATIVE_PASS_VERIFY") || ta::devFlag("TA_REQUIRE_NATIVE_PASS")) {
                // Workstream N: native passes in the body replay. With the verifier every
                // pass's GL state was compared with SDL's priming (a mismatch threw at the
                // frame); TA_REQUIRE_NATIVE_PASS on OpenGL requires passes to have opened.
                const auto& t=gameView->nativePassTotals();
                using ull=unsigned long long;
                SDL_RendererInfo info{};
                SDL_GetRendererInfo(ren,&info);
                const bool gl=info.name && std::strcmp(info.name,"opengl")==0;
                std::printf("NATIVE_PASS_STUDY renderer=%s passes=%llu commands=%llu refused=%llu verified=%llu failures=%llu\n",
                    info.name ? info.name : "?",ull(t.passes),ull(t.commands),ull(t.refused),ull(t.verified),ull(t.verifyFailures));
                if(t.verifyFailures)throw std::runtime_error("a native pass left GL state different from SDL's");
                if(ta::devFlag("TA_REQUIRE_NATIVE_PASS") && gl && !t.passes)
                    throw std::runtime_error("OpenGL study opened no native pass");
                if(!gl && t.passes)throw std::runtime_error("native passes on a renderer without them");
                if(ta::devFlag("TA_NATIVE_PASS_VERIFY") && t.passes && t.verified<t.passes)
                    throw std::runtime_error("native passes went unverified");
                std::printf("NATIVE_PASS_OK\n");   // every check above held (the ctest requires this line)
            }
            if(ta::devFlag("TA_REQUIRE_GEOMETRY_REUSE")) {
                std::printf("GEOMETRY_VERIFY checks=%llu animated_cross_tick=%llu\n",
                    static_cast<unsigned long long>(gameView->geometryChecks()),
                    static_cast<unsigned long long>(gameView->animatedTextureChecks()));
                if(!gameView->animatedTextureChecks())
                    throw std::runtime_error("texture reuse test had no unchanged animated frames across ticks");
            }
            if(ta::devFlag("TA_GEOMETRY_VERIFY")) {
                // Staged geometry (workstream D): every reuse compared with a complete
                // rebuild (or verifyMonolithicBody/buildUnitGeom threw at the first mismatch).
                std::printf("GEOMETRY_STAGES hits=%llu placement=%llu material=%llu single_pass=%llu shadow_skipped=%llu\n",
                    static_cast<unsigned long long>(gameView->geometryChecks()),
                    static_cast<unsigned long long>(gameView->placementChecks()),
                    static_cast<unsigned long long>(gameView->materialChecks()),
                    static_cast<unsigned long long>(gameView->monolithicChecks()),
                    static_cast<unsigned long long>(gameView->shadowSkipChecks()));
            }
            if(ta::devFlag("TA_BODY_ARENA_VERIFY")) {
                // Retained body vertices (workstream H): every arena command gathered
                // from the arena contents, and every assembled command, matched its
                // sources, or draw() threw.
                const auto& t=gameView->bodyArenaVerifyTotals();
                using ull=unsigned long long;
                std::printf("BODY_ARENA_VERIFY frames=%llu commands=%llu vertices=%llu retained=%llu uploaded=%llu "
                            "appends=%llu rebuilds=%llu streams=%llu fallbacks=%llu "
                            "rollovers=%llu drains=%llu old_commands=%llu "
                            "stream_disallowed=%llu stream_empty=%llu stream_retry=%llu stream_unstable=%llu stream_budget=%llu "
                            "assembled=%llu assembled_vertices=%llu\n",
                    ull(t.frames),ull(t.commands),ull(t.vertices),ull(t.retained),ull(t.uploaded),
                    ull(t.appends),ull(t.rebuilds),ull(t.streams),ull(t.fallbacks),
                    ull(t.rollovers),ull(t.drains),ull(t.oldCommands),
                    ull(t.streamReasons[1]),ull(t.streamReasons[2]),ull(t.streamReasons[3]),
                    ull(t.streamReasons[4]),ull(t.streamReasons[5]),
                    ull(t.assembled),ull(t.assembledVertices));
                if(!t.frames && !t.streams)throw std::runtime_error("body arena verify ran no checks");
            }
            if(ta::devFlag("TA_CPU_TRIM_VERIFY")) {
                // Retained CPU buffers (workstream J): every trim kept the buffer's
                // contents and no buffer was trimmed twice within a window, or draw() threw.
                const auto& t=gameView->cpuTrimVerifyTotals();
                using ull=unsigned long long;
                std::printf("CPU_TRIM_VERIFY frames=%llu trims=%llu worker_trims=%llu released=%llu content_checks=%llu "
                            "regrowths=%llu peak_retained=%llu final_retained=%llu recovered=%llu\n",
                    ull(t.frames),ull(t.trims),ull(t.workerTrims),ull(t.released),ull(t.contentChecks),
                    ull(t.regrowths),ull(t.peakRetained),ull(t.finalRetained),
                    ull(t.peakRetained>t.finalRetained ? t.peakRetained-t.finalRetained : 0));
                if(!t.frames)throw std::runtime_error("cpu trim verify ran no frames");
            }
            if(ta::devFlag("TA_SCHEDULE_VERIFY")) {
                // Cost-planned pool passes (workstream I): every pass matched a serial
                // re-run and built each visible unit once, or draw() threw.
                const auto& t=gameView->scheduleVerifyTotals();
                using ull=unsigned long long;
                std::printf("SCHEDULE_VERIFY frames=%llu geometry_passes=%llu builds=%llu parallel_passes=%llu "
                            "checks=%llu prep_passes=%llu prep_points=%llu copy_passes=%llu copy_vertices=%llu\n",
                    ull(t.frames),ull(t.geometryPasses),ull(t.geometryBuilds),ull(t.parallelPasses),ull(t.checks),
                    ull(t.prepPasses),ull(t.prepPoints),ull(t.copyPasses),ull(t.copyVertices));
                if(!t.frames)throw std::runtime_error("schedule verify ran no checks");
            }
            if(ta::devFlag("TA_UNIT_INDEX_VERIFY")) {
                // Unit broad phase (workstream L): every indexed pass accepted exactly the
                // units of a full scan, in the same order, or draw() threw.
                const auto& t=gameView->unitIndexVerifyTotals();
                using ull=unsigned long long;
                std::printf("UNIT_INDEX_VERIFY frames=%llu passes=%llu live=%llu candidates=%llu accepted=%llu "
                            "health=%llu selfdestruct=%llu squads=%llu ghosts=%llu\n",
                    ull(t.frames),ull(t.passes),ull(t.live),ull(t.candidates),ull(t.accepted),
                    ull(t.overlays[0]),ull(t.overlays[1]),ull(t.overlays[2]),ull(t.ghosts));
                if(!t.passes)throw std::runtime_error("unit index verify ran no checks");
            }
            if(ta::devFlag("TA_PICK_INDEX_VERIFY")) {
                // Cursor picking (perf follow-up C): every unit the exact test accepted was
                // a candidate, and every cursor decision equalled a full-scan one, or the
                // cursor overlay threw.
                const auto& t=gameView->pickVerifyTotals();
                using ull=unsigned long long;
                std::printf("PICK_INDEX_VERIFY queries=%llu live=%llu candidates=%llu hits=%llu decisions=%llu "
                            "body_queries=%llu body_candidates=%llu body_hits=%llu\n",
                    ull(t.queries),ull(t.live),ull(t.candidates),ull(t.hits),ull(t.decisions),
                    ull(t.bodyQueries),ull(t.bodyCandidates),ull(t.bodyHits));
                if(!t.queries || !t.decisions)throw std::runtime_error("pick index verify ran no checks");
            }
            if(ta::devFlag("TA_DISTANT_VERIFY")) {
                // Distant-image lookup before body generation (workstream G): every
                // deferred body was regenerated in a copy and matched, or draw() threw.
                const auto& t=gameView->distantVerifyTotals();
                using ull=unsigned long long;
                std::printf("DISTANT_VERIFY frames=%llu deferred=%llu checked=%llu catchup=%llu late=%llu\n",
                    ull(t.frames),ull(t.deferred),ull(t.checked),ull(t.catchup),ull(t.late));
                if(!t.frames)throw std::runtime_error("distant verify ran no checks");
            }
            if(ta::devFlag("TA_SHADOW_CACHE_VERIFY")) {
                // Shadow tile reuse (workstream E): every kept tile equalled a complete
                // re-bake of the frame, or draw() threw at the first differing pixel.
                const auto& t=gameView->shadowVerifyTotals();
                // Workstream F appends the cell counts (kept = reused in a cell kept from
                // an earlier frame; validated = frames whose allocator passed validate()),
                // perf follow-up K the page budget (frames on extended pages, most pages
                // held, pressure sheds, frames that fell back for any reason, frames from
                // the last shed to the next frame on extended pages: the cooldown;
                // the pages held after the last frame).
                using ull=unsigned long long;
                std::printf("SHADOW_CACHE_VERIFY frames=%llu tiles=%llu reused=%llu body=%llu translated=%llu "
                            "original_reused=%llu kept=%llu allocs=%llu compactions=%llu migrated=%llu "
                            "evicted=%llu page_releases=%llu shrinks=%llu validated=%llu page_allocs=%llu "
                            "alloc_failures=%llu extended_frames=%llu max_pages=%llu sheds=%llu fallbacks=%llu "
                            "regrow_gap=%llu extended_releases=%llu held_pages=%llu\n",
                    ull(t.frames),ull(t.tiles),ull(t.reused),ull(t.body),ull(t.translated),ull(t.original),
                    ull(t.kept),ull(t.allocs),ull(t.compactions),ull(t.migrated),ull(t.evicted),
                    ull(t.pageReleases),ull(t.shrinks),ull(t.validated),ull(t.pageAllocs),ull(t.failures),
                    ull(t.extendedFrames),ull(t.maxPages),ull(t.sheds),ull(t.fallbacks),ull(t.regrowGap),
                    ull(t.extendedReleases),ull(t.heldPages));
                if(!t.frames)throw std::runtime_error("shadow cache verify ran no checks");
            }
            if(ta::devFlag("TA_GEOMETRY_OWNERSHIP_VERIFY")) {
                // Every frame passed, or verifyGeometryOwnership() threw at the first violation.
                const auto& t=gameView->ownershipTotals();
                std::printf("OWNERSHIP_VERIFY checks=%llu acquired=%u replaced=%u destroyed=%u expired=%u "
                            "budget=%u trimmed=%u chunks_freed=%u refused=%u peak_visible=%u peak_retained=%u "
                            "peak_allocated=%u\n",
                    static_cast<unsigned long long>(gameView->ownershipChecks()),t.acquired,t.replaced,
                    t.releasedDestroyed,t.releasedExpired,t.releasedBudget,t.trimmed,t.chunksFreed,
                    t.duplicates+t.refused,t.visible,t.retained,t.allocated);
                if(!gameView->ownershipChecks())throw std::runtime_error("ownership verify ran no checks");
            }
            if(ta::devFlag("TA_ATLAS_VERIFY")) {
                // Every check passed, or verifyAtlases() threw at the first mismatch.
                std::printf("ATLAS_VERIFY checks=%llu regions=%zu\n",
                    static_cast<unsigned long long>(gameView->atlasVerifyChecks()),
                    gameView->atlasVerifyRegions());
                if(!gameView->atlasVerifyChecks())
                    throw std::runtime_error("atlas verify ran no checks");
            }
            if(ta::devFlag("TA_GHOST_VERIFY")) {
                // Build ghosts (perf follow-up E): every ghost matched the reference
                // collection, or drawGhostAt() threw.
                const auto& t=gameView->ghostVerifyTotals();
                using ull=unsigned long long;
                std::printf("GHOST_VERIFY checked=%llu hits=%llu culled=%llu collected=%llu inexact=%llu\n",
                    ull(t.checked),ull(t.hits),ull(t.culled),ull(t.collected),ull(t.inexact));
                if(!t.checked)throw std::runtime_error("ghost verify ran no checks");
            }
            running=false;quitApp=true;
            if(studyQuery){studyDeleteQuery(GLsizei(kStudyQueries),studyQueries);studyQuery=false;}
        }
#endif
        // With vsync on, bound presentation at the display's refresh: some renderers report
        // vsync enabled yet let presents run ahead (macOS Metal under Core Animation was seen
        // jumping past the refresh and bouncing in a benchmark), and the frame cap below is
        // off whenever vsync is on. A swap that really waited is left alone (see the pacer).
        streamPacer.pace(win, settings.vsync && !noVsync);
        if (prof) {
            double t5 = pnow();
            if (profFrames)
                std::printf("FRAME ms=%.3f update=%.3f anim=%.3f draw=%.3f present=%.3f tick=%u live=%zu wall_ms=%llu\n",
                    t5-t0,t2-t1,animationMs,t3-t2,t5-t4,profTick,profLive,
                    static_cast<unsigned long long>(SDL_GetTicks64()));
            // Per-frame spike log. The PROF line below is a one-second AVERAGE, which
            // is exactly the wrong shape for a periodic hitch -- a 200ms stall twice a
            // second vanishes into a mean. This prints the frames that are outliers
            // against a rolling baseline, with the phase breakdown, so the stall can be
            // attributed instead of guessed at.
            //
            // The phase boundaries here are the real ones, and were not always: this
            // used to label t1-t0 (the ASSET-VIEWER draw, zero in a game) as "update"
            // and then report t4-t1 as "draw" -- which swallowed the whole gameplay
            // update. So a stall in the sim showed up as unattributed time inside
            // "draw", and I went looking for it in the renderer. It was the fog pass.
            static const bool spikes = ta::devEnv("TA_SPIKES") != nullptr;
            if (spikes) {
                const double total = t5 - t0;
                // NOT a median -- an asymmetric EMA of recent frame times (rises slowly,
                // falls fast) used purely as an outlier threshold. Named for what it is.
                static double base = 16.0;
                base += (total - base) * (total > base ? 0.02 : 0.20);
                double pj = 0, sb = 0, sh = 0, tr = 0, fg = 0, fx = 0, hd = 0;
                double at = 0, bd = 0;
                if (gameView) { gameView->profPeek(pj, sb, sh);
                                gameView->profOther(tr, fg, fx, hd);
                                gameView->profOther2(at, bd); }
                static double ppj = 0, psb = 0, psh = 0, ptr = 0, pfg = 0, pfx = 0, phd = 0;
                static double pat = 0, pbd = 0;
                // These previous totals are function-static, but the counters they track
                // belong to the GameView -- and returning to the menu and starting another
                // game builds a FRESH GameView whose counters start at zero. Carrying the
                // old game's totals across that boundary would make the first frame's
                // deltas negative and inflate the residual. Re-baseline whenever the
                // session changes, and treat any counter going backwards (a reset we did
                // not anticipate) the same way rather than printing nonsense.
                static const void* pSession = nullptr;
                const bool restarted = (static_cast<const void*>(gameView.get()) != pSession) ||
                                       pj < ppj || sb < psb || sh < psh || tr < ptr ||
                                       fg < pfg || fx < pfx || hd < phd ||
                                       at < pat || bd < pbd;
                if (restarted) {
                    pSession = gameView.get();
                    ppj = pj; psb = sb; psh = sh; ptr = tr; pfg = fg; pfx = fx; phd = hd;
                    pat = at; pbd = bd;
                    base = total > 0 ? total : 16.0;   // baseline belongs to the session too
                }
                // All of these counters are monotonic (see takeProf), so a plain
                // difference is this frame's share.
                const double dpj = pj - ppj, dsb = sb - psb, dsh = sh - psh;
                const double dtr = tr - ptr, dfg = fg - pfg, dfx = fx - pfx;
                const double dhd = hd - phd, dat = at - pat, dbd = bd - pbd;
                // draw() partitions into terrain + atlas + proj + submit + fx + fog + hud
                // + whatever is left. shadow and body are NESTED INSIDE submit (the submit
                // timer opens before the shadow pass and closes after the body loop), so
                // they are reported as detail and must NOT be subtracted again -- doing
                // that double-counted body and made the residual read far too small.
                const double drawMs = t3 - t2;
                const double rest = drawMs - dtr - dat - dpj - dsb - dfx - dfg - dhd;
                if (!restarted && total > base * 2.0 && total > 8.0)
                    std::printf("SPIKE %6.1fms (base %5.1f) | viewers=%.1f update=%.1f "
                                "draw=%.1f [terrain=%.1f atlas=%.1f proj=%.1f submit=%.1f "
                                "(shadow=%.1f body=%.1f) fx=%.1f fog=%.1f hud=%.1f "
                                "rest=%.1f] present=%.1f | t=%.1fs\n",
                                total, base, t1 - t0, t2 - t1, drawMs,
                                dtr, dat, dpj, dsb, dsh, dbd, dfx, dfg, dhd, rest,
                                t5 - t4, SDL_GetTicks64() / 1000.0);
                ppj = pj; psb = sb; psh = sh; ptr = tr; pfg = fg; pfx = fx; phd = hd;
                pat = at; pbd = bd;
            }
            pPres += t5 - t4;
            pAcc += t5 - t0; ++pFrames;
            if (pAcc >= 1000.0) {
                double proj = 0, submit = 0, shadow = 0, sim = 0;
                uint64_t units = 0, shVerts = 0;
                if (gameView) gameView->takeProf(proj, submit, shadow, sim, units, shVerts);
                std::printf("PROF fps=%.0f | update=%.1f [sim=%.1f] draw=%.1f "
                            "[proj=%.1f submit=%.1f (shadow=%.1f) other=%.1f] present=%.1f | units=%llu shverts=%lluk "
                            "tick=%u live=%zu wall_ms=%llu\n",
                            pFrames * 1000.0 / pAcc, pUpd / pFrames, sim / pFrames,
                            pDraw / pFrames, proj / pFrames, submit / pFrames,
                            shadow / pFrames, (pDraw - proj - submit) / pFrames, pPres / pFrames,
                            (unsigned long long)(units / uint64_t(std::max(1, pFrames))),
                            (unsigned long long)(shVerts / uint64_t(std::max(1, pFrames)) / 1000),
                            profTick, profLive,
                            (unsigned long long)SDL_GetTicks64());
                std::fflush(stdout);
                pUpd = pDraw = pPres = pAcc = 0; pFrames = 0;
            }
        }

        // Frame cap ONLY when vsync is off -- with vsync on, the swap already paces us, so
        // an extra SDL_Delay just double-paces and adds ±1ms wobble (a jitter source). When
        // it does apply, coarse-sleep the bulk then spin the last ~1ms: SDL_Delay alone
        // rounds to whole milliseconds and wobbles the frame time.
        if (settings.maxFps > 0 && !settings.vsync) {
            static uint64_t prevPresent = 0;
            const double freq = double(SDL_GetPerformanceFrequency());
            const double target = 1.0 / settings.maxFps;   // live via the Options slider
            if (prevPresent) {
                for (;;) {
                    double elapsed = double(SDL_GetPerformanceCounter() - prevPresent) / freq;
                    double remain = target - elapsed;
                    if (remain <= 0.0) break;
                    if (remain > 0.002) SDL_Delay(uint32_t((remain - 0.001) * 1000.0));
                }
            }
            prevPresent = SDL_GetPerformanceCounter();
        }

        // Front-end BGM continues through the lobby (same track, no restart), then
        // stops once the game proper starts so GameView's faction music takes over.
        if (fromMenu) {
            if (gameView && !gameView->inLobbyPhase()) menuMusic.stop();
            else menuMusic.poll();
        }
        // A MAIN MENU button or post-game Escape ends the session; the outer loop
        // then tears it down and re-shows the menu (quitApp stays false).
        if (gameView && gameView->menuRequested()) running = false;
        // The in-game QUIT button exits the whole app.
        if (gameView && gameView->quitRequested()) { running = false; quitApp = true; }
        // Leaving a running game on purpose is a Leave, not a dropped link: the server
        // must not hold the game for the disconnect wait and then hand the seat to an AI.
        if (!running && mp) mp->leaveRunningGame();

        if (!shot.empty()) {
            // Render a few frames so lazy content settles, then capture. For content
            // that settles asynchronously (a network spectator building its world),
            // TA_SHOT_MS waits that many wall-clock ms before capturing instead.
            static const char* shotMsEnv = ta::devEnv("TA_SHOT_MS");
            static uint64_t shotT0 = SDL_GetTicks64();
            static int frames = 0;
            bool ready = shotMsEnv ? (SDL_GetTicks64() - shotT0 >= uint64_t(std::atoi(shotMsEnv)))
                                   : (++frames >= 3);
#ifndef NDEBUG
            if(ta::devEnv("TA_GLOW_TEST") && gameView)ready=ready && gameView->debugGlowDrawCount_>=3;
            if(ta::devEnv("TA_POINT_TEST") && gameView)ready=ready && gameView->debugPointDrawCount_>=3;
            if(ta::devEnv("TA_SMOKE_TEST") && gameView)ready=ready && gameView->debugSmokeDrawCount_>=2;
            if(ta::devEnv("TA_DAMAGE_FLAME_TEST") && gameView)ready=ready && gameView->debugDamageFlameDrawCount_>=1;
            if(ta::devEnv("TA_FEATURE_FLAME_TEST") && gameView)ready=ready && gameView->debugFeatureFlameDrawCount_>=gameView->debugFeatureFlameExpected_ && gameView->debugFeatureSmokeDrawCount_>=1;
            if(ta::devEnv("TA_SHOT_FLAME") && gameView)ready=ready && gameView->debugFlameDrawCount_>=3;
            if(gameView && gameView->replayMode())ready=ready && !gameView->replaySeekScriptBusy();
#endif
            static bool shotArmed = false;
            if (ready && ktPhase < 0) {
                // Terrain + minimap build asynchronously now: finish them, let the
                // loop render ONE more frame with everything uploaded (this block
                // runs after the frame's draw, so finishing here is too late for
                // the current backbuffer), then capture on the next pass.
                if (gameView) gameView->finishTerrain();
                else if (mapView) mapView->finishChunks();
                if (!shotArmed) {
                    shotArmed = true;
#ifndef NDEBUG
                    // TA_SHOT_OPTIONS=1: capture the in-game Options overlay (its GAME
                    // DATA FOLDER row greyed, with the reason).
                    if (gameView && ta::devFlag("TA_SHOT_OPTIONS")) gameView->openOptions();
#endif
                    // TA_SHOT_PRESS=<SDL key name> taps one key before the capture, so
                    // a harness run can shoot an overlay (Unit Info, the F4 scoreboard)
                    // instead of only the plain game view.
                    // TA_SHOT_CLICKS="x,y;x,y" taps those points, one per armed pass,
                    // before the capture -- enough to walk a menu into the state worth
                    // photographing (the lobby's map picker needs two clicks). An "S"
                    // prefix ("S120,700") holds Shift for that click; an "M" prefix only moves
                    // the pointer there (a hover). A "W<ms>" entry waits that long before the
                    // next one (a room to fill, a map to verify).
                    static size_t clickIdx = 0;
                    static bool clickShift = false;
                    static uint64_t clickWaitUntil = 0;
                    if (clickShift) { SDL_SetModState(KMOD_NONE); clickShift = false; }
#ifndef NDEBUG
                    // TA_SHOT_SHIFT=1 holds Shift from here through the capture: the
                    // order overlay (order lines, area boxes, range circles) is drawn
                    // only while it is down.
                    if (ta::devFlag("TA_SHOT_SHIFT")) SDL_SetModState(KMOD_LSHIFT);
#endif
                    bool clickSent = false;
                    if (const char* cl = ta::devEnv("TA_SHOT_CLICKS")) {
                        std::vector<std::pair<int, int>> pts;
                        std::vector<bool> shifted, moveOnly;
                        std::vector<int> waits;   // > 0: a "W<ms>" pause, not a click
                        std::string acc(cl);
                        size_t p0 = 0;
                        while (p0 <= acc.size()) {
                            size_t e = acc.find(';', p0);
                            std::string one = acc.substr(p0, e == std::string::npos ? std::string::npos : e - p0);
                            const bool sh = !one.empty() && (one[0] == 'S' || one[0] == 's');
                            if (sh) one.erase(0, 1);
                            const bool mv = !one.empty() && (one[0] == 'M' || one[0] == 'm');
                            if (mv) one.erase(0, 1);
                            size_t c = one.find(',');
                            if (!one.empty() && (one[0] == 'W' || one[0] == 'w')) {
                                pts.push_back({0, 0}); shifted.push_back(false); moveOnly.push_back(false);
                                waits.push_back(std::max(1, std::atoi(one.c_str() + 1)));
                            } else if (c != std::string::npos) {
                                pts.push_back({std::atoi(one.c_str()), std::atoi(one.c_str() + c + 1)});
                                shifted.push_back(sh); moveOnly.push_back(mv); waits.push_back(0);
                            }
                            if (e == std::string::npos) break;
                            p0 = e + 1;
                        }
                        if (clickIdx < pts.size() && waits[clickIdx] > 0) {
                            if (!clickWaitUntil) clickWaitUntil = SDL_GetTicks64() + uint64_t(waits[clickIdx]);
                            if (SDL_GetTicks64() >= clickWaitUntil) { ++clickIdx; clickWaitUntil = 0; }
                            clickSent = true;
                            shotArmed = false;   // keep waiting, or take the next entry next pass
                        } else if (clickIdx < pts.size()) {
                            if (shifted[clickIdx]) { SDL_SetModState(KMOD_LSHIFT); clickShift = true; }
                            SDL_Event ev{};
                            ev.type = SDL_MOUSEMOTION;
                            ev.motion.x = pts[clickIdx].first; ev.motion.y = pts[clickIdx].second;
                            SDL_PushEvent(&ev);
                            if (!moveOnly[clickIdx]) {
                                ev.type = SDL_MOUSEBUTTONDOWN;
                                ev.button.button = SDL_BUTTON_LEFT;
                                ev.button.x = pts[clickIdx].first; ev.button.y = pts[clickIdx].second;
                                SDL_PushEvent(&ev);
                                ev.type = SDL_MOUSEBUTTONUP;
                                SDL_PushEvent(&ev);
                            }
                            ++clickIdx;
                            clickSent = true;
                            shotArmed = false;   // let it land, then try again next pass
                        }
                    }
                    static bool pressSent = false;
#ifndef NDEBUG
                    // TA_SHOT_TRAIL=1 queues a short move chain on one owned unit so
                    // the order line has something to draw. The orders have to round-trip
                    // through the server and come back in a bundle before they show up in
                    // a render snapshot, so hold the capture for a good number of passes:
                    // 30 was not enough and produced an empty picture that looked exactly
                    // like a broken feature.
                    static int trailWait = -1;
                    if (ta::devEnv("TA_SHOT_TRAIL") && gameView && trailWait != 0) {
                        if (trailWait < 0) trailWait = gameView->debugQueueDemo() ? 120 : 0;
                        else --trailWait;
                        if (trailWait > 0) shotArmed = false;
                    }
#endif
                    // Replay-return fixtures need their final key after the
                    // click sequence; preserve the original ordering otherwise.
                    if (const char* kn = ta::devEnv("TA_SHOT_PRESS"); kn && !pressSent &&
                        (!ta::devFlag("TA_SHOT_PRESS_AFTER_CLICKS") || !clickSent)) {
                        pressSent = true;
                        // Accepts "ctrl+a" / "shift+f1" / "ctrl+shift+d" as well as a
                        // bare key name: every SELECTION hotkey is Ctrl-modified, so
                        // without this a harness run cannot select anything to shoot.
                        std::string spec(kn);
                        uint16_t mod = 0;
                        for (;;) {
                            size_t plus = spec.find('+');
                            if (plus == std::string::npos) break;
                            std::string p = spec.substr(0, plus);
                            std::transform(p.begin(), p.end(), p.begin(),
                                           [](unsigned char ch) { return char(std::tolower(ch)); });
                            if (p == "ctrl") mod |= KMOD_LCTRL;
                            else if (p == "shift") mod |= KMOD_LSHIFT;
                            else if (p == "alt") mod |= KMOD_LALT;
                            else break;
                            spec.erase(0, plus + 1);
                        }
                        if (SDL_Keycode kc = SDL_GetKeyFromName(spec.c_str()); kc != SDLK_UNKNOWN) {
                            SDL_Event ev{};
                            ev.type = SDL_KEYDOWN;
                            ev.key.keysym.sym = kc;
                            ev.key.keysym.mod = mod;
                            SDL_PushEvent(&ev);
                        }
                        shotArmed = false;   // let the key land, then arm on the next pass
                    }
                } else {
#ifndef NDEBUG
                    // TA_SHOT_SIZES="WxH;WxH;..." captures the SAME running session at
                    // each size in turn, resizing the live window between captures. That
                    // is deliberately not the same test as launching once per size with
                    // --winsize: it is the only way to exercise a LIVE resize, where any
                    // layout that cached geometry from an earlier frame keeps drawing at
                    // the old size. Output goes to <shot-stem>.WxH.png.
                    static std::vector<std::pair<int, int>> shotSizes = [] {
                        std::vector<std::pair<int, int>> v;
                        if (const char* e = ta::devEnv("TA_SHOT_SIZES")) {
                            std::string acc(e);
                            size_t p0 = 0;
                            while (p0 <= acc.size()) {
                                size_t sep = acc.find(';', p0);
                                std::string one = acc.substr(p0, sep == std::string::npos ? std::string::npos : sep - p0);
                                if (size_t x = one.find('x'); x != std::string::npos)
                                    v.push_back({std::atoi(one.c_str()), std::atoi(one.c_str() + x + 1)});
                                if (sep == std::string::npos) break;
                                p0 = sep + 1;
                            }
                        }
                        return v;
                    }();
                    static size_t sizeIdx = 0;
                    static int settle = 0;
                    if (sizeIdx < shotSizes.size()) {
                        SDL_Window* win = SDL_RenderGetWindow(ren);
                        auto [sw, sh] = shotSizes[sizeIdx];
                        if (settle == 0) {
                            if (win) SDL_SetWindowSize(win, sw, sh);
                            settle = 3;          // let the resize reach the renderer
                            shotArmed = false;
                        } else if (--settle > 0) {
                            shotArmed = false;   // still settling
                        } else {
                            int ow, oh;
                            SDL_GetRendererOutputSize(ren, &ow, &oh);
                            std::string base = shot;
                            if (size_t dot = base.rfind('.'); dot != std::string::npos)
                                base = base.substr(0, dot);
                            screenshot(ren, ow, oh,
                                       base + "." + std::to_string(ow) + "x" + std::to_string(oh) + ".png");
                            ++sizeIdx;
                            if (sizeIdx < shotSizes.size()) shotArmed = false;   // next size
                            else running = false;
                        }
                    } else
#endif
                    {
                        screenshot(ren, w, h, shot);
                        running = false;
                    }
                }
            }
        }
    }
#ifndef NDEBUG
    if(studyQuery)studyDeleteQuery(GLsizei(kStudyQueries),studyQueries);
#endif
    if (gameView) gameView->setAudioTap(nullptr,nullptr);
    // Campaign mission ended (and resolved -- not a mid-mission quit): advance
    // persisted progress on a win, then show the result screen and act on the choice.
    bool resultToTitle = false;   // the end screen's left door: the title, whatever the game
    if (campaignId.rfind("ta:", 0) == 0 && gameView && !quitApp && gameView->missionOutcomePublic() != 0) {
        // Total Annihilation campaign (docs/retail-campaign.md 4.3): record W/L, then
        // ENDMSN; Start goes through MSNBRIEF to the picked mission.
        const int oc = gameView->missionOutcomePublic();
        localServer.stop(); gameView.reset(); mp.reset();
        ta::TaFrontEnd front(ren, win, vfs);
        if (oc > 0 && taCampaign.active()) {
            std::string glamour, glamourSound;
            bool noMovie = false;
            const std::string ota = "maps/" + taCampaign.missions[size_t(taCampaign.cur)].file;
            try {
                const auto bytes = vfs.read(ota);
                const auto root = ta::tdf::parseText(std::string(bytes.begin(), bytes.end()), ota);
                if (const auto* gh = root.child("GlobalHeader")) {
                    glamour = gh->valueOr("glamour", "");
                    glamourSound = gh->valueOr("glamoursound", "");
                    noMovie = gh->numberOr("nomovie", 0) != 0;
                }
            } catch (const std::exception&) {}
            front.showGlamour(glamour, glamourSound);
            // endgame.cpp 0x41fc85: winning the campaign's last mission plays the
            // side's ending -- Arm 3.zrb, Core 4.zrb -- then 5.zrb (frontend states 4/5).
            if (!noMovie && taCampaign.cur + 1 >= int(taCampaign.missions.size())) {
                menuMusic.setVolume(0, 0);
                ta::MainMenu::playIntro(ren, dataRoot, taCampaign.side == 0 ? "3.zrb" : "4.zrb");
                ta::MainMenu::playIntro(ren, dataRoot, "5.zrb");
                menuMusic.setVolume(settings.masterVol, settings.bgmVol);
            }
        }
        const auto tc = front.endMission(skirmishSetup, taCampaign, oc);
        if (tc == ta::TaFrontEnd::Choice::Campaign) { pendingCampaign = taCampaign.spec(); pendingCampaignId = campaignId; }
        else if (tc == ta::TaFrontEnd::Choice::Exit) quitApp = true;
    } else if (!campaignStem.empty() && !campaignId.empty() && gameView && !quitApp &&
        gameView->missionOutcomePublic() != 0) {
        int oc = gameView->missionOutcomePublic();
        std::string title = "MISSION", nextStem;
        bool finalMission = false;
        for (const auto& c : ta::loadCampaigns(vfs)) {
            if (c.id != campaignId) continue;
            int completedIdx = -1;   // which slot was just beaten (kAltMission for the alt branch)
            if (const auto chapter=ta::campaignChapter(c,campaignStem)) {
                title=chapter->title;
                nextStem=chapter->nextStem;
                finalMission=nextStem.empty();
                completedIdx=chapter->alternate ? ta::kAltMission : chapter->index;
            }
            // Victory: record THIS mission as completed. Nothing is ever locked -- this
            // only tracks what's been beaten.
            if (oc > 0 && completedIdx != -1 &&
                settings.campaignCompleted[campaignId].insert(completedIdx).second)
                saveSettings(settings);
            break;
        }
        ta::ResultStats st = gameView->resultStats();   // read before the sim is freed
        // gameView BEFORE mp: ~GameView saves the replay, which reads the net client.
        localServer.stop(); gameView.reset(); mp.reset();   // free the mission before the movie/modal
        if (oc > 0) {
            // Cinematics on victory: a per-mission "post<stem>" cutscene (retail ships
            // one after Book of Darien mission 24), then the campaign's ending credits
            // after its final mission. Each is a no-op if the movie isn't present.
            menuMusic.setVolume(0, 0);
            ta::MainMenu::playIntro(ren, dataRoot, ("post" + campaignStem + ".bik").c_str());
            if (finalMission)
                ta::MainMenu::playIntro(ren, dataRoot,
                                         campaignId == "book of darien" ? "posttakcredits.bik" : "credits.bik");
            menuMusic.setVolume(settings.masterVol, settings.bgmVol);
        }
        ta::ResultChoice rc = ta::ResultScreen::run(ren, vfs, oc > 0, title,
                                                      oc > 0 && !nextStem.empty(), &settings,
                                                      &menuMusic, &st);
        if (rc == ta::ResultChoice::Next)       { pendingCampaign = nextStem;     pendingCampaignId = campaignId; }
        else if (rc == ta::ResultChoice::Retry) { pendingCampaign = campaignStem; pendingCampaignId = campaignId; }
        // Menu -> pendingCampaign stays empty -> the outer loop re-shows the front-end.
    } else if (campaignStem.empty() && gameView && !quitApp && fromMenu &&
               (gameView->outcomePublic() != 0 || gameView->watchOverPublic())) {
        // Skirmish / multiplayer: retail shows the same victory/defeat plate here,
        // with the match's statistics table. No next mission and nothing to retry,
        // so both buttons come back to the front end. A watcher gets the plate too,
        // the winner's (GameView::resultStats), also when the server ended a watched
        // game that nobody won.
        const bool victory = gameView->resultVictory();
        ta::ResultStats st = gameView->resultStats();
        localServer.stop(); gameView.reset(); mp.reset();   // view first: it saves the replay
        const auto next = origin == ta::LaunchOrigin::SinglePlayer ? ta::ResultChoice::NewGame
                        : origin == ta::LaunchOrigin::Multiplayer  ? ta::ResultChoice::GameSelect
                                                                    : ta::ResultChoice::Menu;
        // Say where the doors really go. The left one is always the title screen. Single
        // player's right door is a new Play the Machine room; a LAN game's returns to
        // Connect to Server (its list); an online game's is that server's game list only
        // with a sign-in token for it, else Connect to Server too (see the menu loop above).
        const bool lanGame = origin == ta::LaunchOrigin::Multiplayer && !tokenLaunch &&
                             (lanHosting || connectKind == ta::MainMenu::Connect::LanJoin ||
                              connectKind == ta::MainMenu::Connect::LanSpectate);
        std::string menuTip, proceedTip;
        if (lanGame) proceedTip = "Connect to Server";
        else if (next == ta::ResultChoice::NewGame) proceedTip = "Play the Machine";
        else if (next == ta::ResultChoice::GameSelect) {
            const auto address = ta::parseServerAddress(lastServer);
            proceedTip = address && sessionToken.heldFor(address->host, address->port) ? "Game List"
                                                                                       : "Connect to Server";
        }
        const auto rc = ta::ResultScreen::run(ren, vfs, victory, "", false, &settings, &menuMusic, &st, next,
                                               menuTip, proceedTip);
        if (rc == next) afterResult = rc;
        else resultToTitle = true;
    }
    // Session ended: tear down any single-player local server, then either loop back
    // to the menu or exit the app.
    localServer.stop();
    // A LAN game (a LOCAL SERVER hosted here, or one joined or watched) goes back to the
    // CONNECT screen, where its list is -- unless its end screen's left door chose the title.
    if (origin == ta::LaunchOrigin::Multiplayer && !tokenLaunch && !resultToTitle &&
        (lanHosting || connectKind == ta::MainMenu::Connect::LanJoin || connectKind == ta::MainMenu::Connect::LanSpectate))
        lanReturnToConnect = true;
    // Destroy the session's views NOW (not at scope end below): the renderer is
    // reused across sessions, so their GPU textures must be freed before the next
    // session budgets against gpuvram. The log line is the leak canary -- healthy
    // teardown leaves single-digit MiB (menu-owned art only); before the teardown
    // fixes it climbed by hundreds of MiB per benchmark until terrain chunks
    // could no longer upload (map stuck at the low-res underlay).
    gameView.reset();
    mapView.reset();
    modelView.reset();
    // ...and only THEN the net client. ~GameView writes this player's replay, which
    // reads the client's recorded bundles -- destroying the client first left the
    // destructor reading freed memory on any quit before the result landed.
    mp.reset();
    std::fprintf(stderr, "gpu: %zu MiB in %zu textures tracked after session teardown (cap %s)\n",
                 gpuvram::bytes() >> 20, gpuvram::count(), gpuvram::cap() == gpuvram::kUnlimited
                     ? "unlimited" : (std::to_string(gpuvram::cap() >> 20) + " MiB").c_str());
    // Clear the in-game minimum-window-size constraint (set per game at the build-icon
    // sizing above). Leaving it on the persistent window makes the returned menu's
    // surface re-negotiation-prone on Wayland (see the Options click-death bug).
    SDL_SetWindowMinimumSize(win, 0, 0);
    if (quitApp || (!fromMenu && pendingCampaign.empty())) break;
    }  // ---- end outer session loop ----

    menuMusic.stop();
    menuMusic.setAudioTap(nullptr,nullptr);
    streamingOwner.reset();
    voiceOwner.reset();   // closes the voice devices before SDL goes
    ta::releaseUiTextGlyphs(); SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    // Quitting the game is signing out. After the window has gone, so a server that
    // does not answer costs nobody a frozen screen.
    ta::net::MpClient::revokeSessionToken(sessionToken, retailHash);
    if (restartApp) {
        // A new GAME DATA FOLDER is loaded by a fresh process (client/datafolder.h): the
        // same arguments, so the --data / launch-folder rules apply exactly as before.
        // POSIX: exec replaces this process (returns only on failure). Windows: starts
        // a new copy, and this one exits.
        std::string why;
        const auto restartArgs = ta::clientRestartArguments(std::vector<std::string>(argv + 1, argv + argc));
        std::fprintf(stderr, "restart: relaunching with %zu argument(s) for data folder %s\n", restartArgs.size(),
                     ta::loadSettings().dataDir.c_str());
#ifndef NDEBUG
        SDL_setenv("TA_MENU_PICK", "", 1);   // a scripted restart pick must not repeat in the new process
#endif
        if (!ta::relaunchSelf(restartArgs, why)) {
            std::fprintf(stderr, "restart failed: %s\n", why.c_str());
            ta::errorBox("Restart failed", "The new game data folder is saved. Start the game again to use it.\n\n(" + why + ")");
            return 1;
        }
    }
    return 0;
}

#ifdef _WIN32
// The GUI subsystem keeps Explorer launches console-free; preserve Unicode
// arguments from Explorer, Cartographer, and command-line launches.
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return ta::utf8Main(main);
}
#endif

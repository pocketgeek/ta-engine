// takclient — interactive TAK asset viewer.
//
//   takclient map <map.tnt> <terrain-dir>       scrollable terrain (drag/arrows,
//                                             +/- zoom, S = screenshot)
//   takclient model <file.3do> [textures-dir palette.pcx]
//                                             rotating textured model
//                                             (drag to rotate, wheel zoom)
//   ... --shot <out.png>                      render one frame headless
//
// Textures dir = extracted data/textures; palette = faction palette PCX
// (e.g. palettes/ara_textures.pcx from sidedata.tdf).

// Must precede SDL.h: on Windows this pulls in winsock2 (with WIN32_LEAN_AND_MEAN)
// before SDL's <windows.h> would otherwise pull the incompatible winsock v1.
#include "net/netcompat.h"

#include "campaign/campaign.h"
#include "client/briefingscreen.h"
#include "client/artscale.h"
#include "client/resultscreen.h"
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
#include "sim/sim.h"
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
#include "net/crypto.h"
#include "client/dev.h"
#include "client/appquit.h"
#include "client/mainmenu.h"
#include "client/menumusic.h"
#include "client/gameview.h"   // the in-world game client (extracted from this file)

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

namespace {
// A local takserver spawned for single-player; killed when the client exits.
bool gLocalServerUp = false;
#ifdef _WIN32
PROCESS_INFORMATION gLocalProc{};
void killLocalServer() {
    if (gLocalServerUp) {
        TerminateProcess(gLocalProc.hProcess, 0);
        CloseHandle(gLocalProc.hProcess);
        CloseHandle(gLocalProc.hThread);
        gLocalServerUp = false;
    }
}
#else
pid_t gLocalPid = 0;
void killLocalServer() {
    if (gLocalPid > 0) { kill(gLocalPid, SIGTERM); waitpid(gLocalPid, nullptr, 0); gLocalPid = 0; }
    gLocalServerUp = false;
}
#endif

// Pick a free loopback TCP port by binding to 0 and reading the assignment.
int pickFreePort() {
    tak::net::netStartup();
    int fd = int(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return 0;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0) {
        socklen_t len = sizeof a;
        if (getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len) == 0) port = ntohs(a.sin_port);
    }
    tak::net::sockClose(fd);
    return port;
}

// Launch a takserver for a private single-player game. Returns true on success.
// --no-auth because there is nobody to sign in AS (this server exists for one
// player, on one machine), and --local so that concession stays on this machine:
// an unauthenticated server must not be reachable from the network.
bool spawnLocalServer(const std::string& serverBin, const std::string& dataRoot, int port) {
#ifdef _WIN32
    std::string cmd = "\"" + serverBin + ".exe\" --port " + std::to_string(port) +
                      " --data \"" + dataRoot + "\" --no-auth --local";
    STARTUPINFOA si{}; si.cb = sizeof si;
    std::vector<char> mut(cmd.begin(), cmd.end()); mut.push_back('\0');
    if (!CreateProcessA(nullptr, mut.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &gLocalProc))
        return false;
    gLocalServerUp = true;
    return true;
#else
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        std::string ps = std::to_string(port);
        execl(serverBin.c_str(), serverBin.c_str(), "--port", ps.c_str(),
              "--data", dataRoot.c_str(), "--no-auth", "--local",
              static_cast<char*>(nullptr));
        _exit(127);   // exec failed
    }
    gLocalPid = pid;
    gLocalServerUp = true;
    return true;
#endif
}

// The local takserver's process id (for the benchmark's server-side CPU/memory sampling).
long localServerPid() {
#ifdef _WIN32
    return gLocalServerUp ? long(gLocalProc.dwProcessId) : 0;
#else
    return long(gLocalPid);
#endif
}

// Path to the takserver binary that sits beside this client. argv[0] is unreliable:
// launched from PATH (e.g. the /usr/bin .rpm/.deb install) it's the bare name
// "takclient" with no directory, and execl/CreateProcess do NOT search PATH -- which
// is why single-player "never started takserver". Resolve the REAL executable's
// directory (/proc/self/exe on Linux, the module path on Windows), then fall back to
// argv[0]'s directory and finally a PATH scan. Returns the first "takserver" found.
std::string resolveServerBin(const char* argv0) {
    namespace fs = std::filesystem;
    std::error_code ec;
    auto tryDir = [&](const fs::path& dir) -> std::string {
        if (dir.empty()) return {};
        fs::path p = dir / "takserver";
        return fs::exists(p, ec) ? p.string() : std::string{};
    };
#if defined(__linux__)
    { char buf[4096]; ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
      if (n > 0) { buf[n] = '\0'; auto r = tryDir(fs::path(buf).parent_path()); if (!r.empty()) return r; } }
#elif defined(_WIN32)
    { char buf[MAX_PATH]; DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
      if (n > 0 && n < MAX_PATH) { auto r = tryDir(fs::path(buf).parent_path()); if (!r.empty()) return r; } }
#endif
    if (auto r = tryDir(fs::path(argv0).parent_path()); !r.empty()) return r;
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
    return (fs::path(argv0).parent_path() / "takserver").string();
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
    if (SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32, px.data(), w * 4) == 0) {
        tak::png::write(path, w, h, px);
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

// AaScaleReset moved to client/aascalereset.h.

// Units simulated on a real map: left-click select, right-click move order
// (shift queues waypoints), arrows scroll, wheel zoom.
// UnitR / PlayerR / Frame render-snapshot structs moved to client/renderframe.h.

// GameView moved to client/gameview.h (+ gameview_*.cpp for method bodies).

// ReplayFile + loadReplayFile() moved to client/replayfile.h + replayfile.cpp.

// Resolve the retail data folder. Priority: an explicit --data (dataRoot already set) >
// the folder saved in config (re-validated each launch) > a native folder picker. On a
// successful pick/validate the location + an authenticity manifest are saved to config.
// `allowPrompt` gates the picker (off for headless runs, which always pass --data).
// Leaves dataRoot empty if it can't be resolved; the caller then aborts.
static void resolveDataDir(std::string& dataRoot, tak::Settings& settings, bool allowPrompt) {
    namespace hpi = tak::hpi;
    // 1. Explicit --data wins. Honour it even if it doesn't validate (a dev override);
    //    if it DOES validate, remember it so a later launch needs no --data.
    if (!dataRoot.empty()) {
        if (settings.dataDir != dataRoot && hpi::validInstall(dataRoot, nullptr)) {
            settings.dataDir = dataRoot;
            settings.dataManifest = hpi::rootManifest(dataRoot);
            tak::saveSettings(settings);
        }
        return;
    }
    // 2. The local directory: drop the binary into a game folder and it just
    //    works, no --data or picker. Used for THIS run without clobbering the
    //    saved install path (so launching elsewhere later still finds it).
    {
        std::error_code ec;
        std::filesystem::path here = std::filesystem::current_path(ec);
        if (!ec && hpi::validInstall(here, nullptr)) {
            dataRoot = here.string();
            std::fprintf(stderr, "data: using the local directory %s\n", dataRoot.c_str());
            return;
        }
    }
    // 3. The saved folder, if it still holds a valid install.
    if (!settings.dataDir.empty() && hpi::validInstall(settings.dataDir, nullptr)) {
        dataRoot = settings.dataDir;
        std::string m = hpi::rootManifest(dataRoot);   // note if the root archives changed
        if (m != settings.dataManifest) { settings.dataManifest = m; tak::saveSettings(settings); }
        return;
    }
    // 4. Ask (interactive only). Loop so a wrong pick can be corrected in place.
    if (!allowPrompt) return;
    if (!tak::haveDirPicker()) {
        std::fprintf(stderr, "no folder picker available (install kdialog or zenity) and no "
                             "--data given -- cannot locate the game data\n");
        return;
    }
    for (int tries = 0; tries < 6; ++tries) {
        std::string picked = tak::pickDirectory(
            "Select your Total Annihilation: Kingdoms install folder", settings.dataDir);
        if (picked.empty()) return;   // cancelled
        std::string why;
        if (hpi::validInstall(picked, &why)) {
            dataRoot = picked;
            settings.dataDir = picked;
            settings.dataManifest = hpi::rootManifest(picked);
            tak::saveSettings(settings);
            return;
        }
        tak::errorBox("Not a game folder",
                      "That folder isn't a Total Annihilation: Kingdoms install (" + why +
                      ").\n\nChoose the folder that contains data.hpi, terrain.hpi and Maps/.");
    }
}

} // namespace

int main(int argc, char** argv) {
    SDL_SetMainReady();   // we defined SDL_MAIN_HANDLED; tell SDL our main is ready
    if (argc >= 2 && (!std::strcmp(argv[1], "--version") || !std::strcmp(argv[1], "-v"))) {
        std::printf("takclient (TAK engine) %s\n", tak::kVersion);
        return 0;
    }
    if (argc >= 2 && (!std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h"))) {
        std::printf(
#ifdef NDEBUG
            "usage: takclient --data <retail-install-dir>\n"
            "  Launches the game and its front-end menu.\n"
            "  --version, -v   print version and exit\n"
            "  <retail-install-dir> holds the shipped *.hpi plus Maps/ Music/ overrides/.\n");
#else
            "usage: takclient [mode] --data <retail-install-dir> [options]\n"
            "  With no mode (or only flags), launches the front-end MENU.\n"
            "  modes: menu | game <map> | map <map> | replay <file.takrep> | model <file.3do>\n"
            "    game single-player: no --server -> auto-hosts a private game vs a server AI.\n"
            "    game multiplayer:   add --server host [--serverport N] [--name X].\n"
            "    game --campaign <stem>: play a campaign mission (e.g. takmission01_mt).\n"
            "  common: [--side X --aiside Y] [--overrides none|cosmetic|full] [--shot out.png]\n"
            "  (debug build: all dev/test flags below are available.)\n"
            "  <retail-install-dir> holds the shipped *.hpi plus Maps/ Music/ overrides/.\n");
#endif
        return 0;
    }
#ifdef NDEBUG
    // Hardened release CLI: only --data (plus the meta --version/--help) is honoured.
    // Every gameplay/dev/test flag and every TAK_* env var is debug-only, so a shipped
    // build has no hidden switches -- the game is configured through the menu + Options.
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version" || a == "-v") { std::printf("takclient (TAK engine) %s\n", tak::kVersion); return 0; }
        if (a == "--help" || a == "-h") { std::printf("usage: takclient --data <retail-install-dir>\n"); return 0; }
        if (a == "--data") { ++i; continue; }   // its value is consumed by the parser below
        std::fprintf(stderr,
            "takclient: unknown option '%s' -- release builds accept only --data and --version.\n", a.c_str());
        return 2;
    }
#endif
    // The first positional arg is the launch mode only if it's a known keyword;
    // otherwise the default is the front-end menu, so `takclient --data <dir>` (or even
    // bare `takclient`) just opens it -- no need to type "menu".
    std::string mode = "menu";
    int argStart = 1;
    if (argc >= 2) {
        std::string a1 = argv[1];
        if (a1 == "menu" || a1 == "game" || a1 == "map" || a1 == "replay" || a1 == "model") {
            mode = a1;
            argStart = 2;
        }
    }
    std::string shot, cobPath, anim, joinAddr, side = "ara", aiSide = "tar";
    // model mode: --statics <bitmask> seeds the VM's static slots (bit i -> static i).
    // Walk/attack scripts gate on an "am I moving" static whose SLOT differs per unit.
    uint32_t staticMask = 1;
    std::string serverHost, playerName, dataRoot, overridesArg;
    // Multiplayer account (menu-entered, or --user/--pass for the harnesses). The
    // password is used once and wiped; it is never written to settings.
    std::string loginUser, loginPass;
    int serverPort = 7677, mpHeadless = 0;
    std::string missionStem;   // --mpmission <stem>: headless campaign-mission host
    std::string cliCampaign;   // --campaign <stem>: launch straight into a mission (interactive)
    int hostPort = 0, joinPort = 0, winW = kWinW, winH = kWinH, maxFps = 60;
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
    bool crusades = false;
    float lookX = 0, lookZ = 0;
    std::vector<std::string> args;
    for (int i = argStart; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--shot" && i + 1 < argc) shot = argv[++i];
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
        }
        else if (a == "--maxfps" && i + 1 < argc) maxFps = std::atoi(argv[++i]);
        else if (a == "--novsync") noVsync = true;
        else if (a == "--crusades") crusades = true;


        else if (a == "--lodeunit" && i + 1 < argc) lodeUnitName = argv[++i];
        else if (a == "--selonly") selonly = true;

        else if (a == "--data" && i + 1 < argc) dataRoot = argv[++i];
        else if (a == "--overrides" && i + 1 < argc) overridesArg = argv[++i];
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
        else if (a == "--mprejoin") mpHeadless = 5; // rejoin a held slot (resume ticket)
        else if (a == "--mpspectate") mpHeadless = 6; // watch the first running game
        else if (a == "--mpmission" && i + 1 < argc) { mpHeadless = 8; missionStem = argv[++i]; }  // host a campaign mission
        else if (a == "--campaign" && i + 1 < argc) { cliCampaign = argv[++i]; mode = "game"; }    // play a mission interactively
        else if (a == "--nofog") nofog = true;
        else if (a == "--cheat") tak::sim::gInstantBuild = true;
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
    if (!shot.empty() || mpHeadless) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");
    (void)hostPort; (void)joinPort; (void)joinAddr;   // --host/--join retired (see --server)

    // The runtime data set: a retail install directory (root *.hpi + Maps/ +
    // Music/ + overrides/). This is the ONLY way the engine reads game files. It
    // must outlive the views (GameView/MapView hold a reference), so it lives here
    // at function scope for the whole render loop, and is built BEFORE connecting
    // so the Hello can carry this install's gameplay-data fingerprint.
    tak::hpi::OverridePolicy pol = tak::hpi::OverridePolicy::Full;
    if (overridesArg == "none") pol = tak::hpi::OverridePolicy::None;
    else if (overridesArg == "cosmetic") pol = tak::hpi::OverridePolicy::Cosmetic;
    tak::hpi::Vfs vfs;   // mounted AFTER the data folder is resolved (below, post-settings)

    // The engine is client-server only: every real game runs on a server, and AIs
    // run ONLY on the server. Local dev/test harnesses (which free-run the sim with
    // no server) are DEBUG-only. A release build has none of them.
    bool localHarness = false;
#ifndef NDEBUG
    localHarness = demo || scenario || missionFlag || navy || amphib || firetest ||
                   facetest || guardtest || lodetest || keytest ||
                   soundtest || misstest || creon || testbuild ||
                   (tak::devEnv("TAK_FFA") != nullptr);
#endif
    // Create the window + renderer up front so the front-end menu can drive the
    // single-player / multiplayer setup that follows it.
    if (shot.empty()) SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
    // Own SIGTERM/SIGINT ourselves: SDL would turn them into an SDL_QUIT event, which the
    // menu ignores (quit via the Exit door), so a kill/Ctrl-C would otherwise wedge a
    // headless run. Our handler sets a flag every event loop polls (see appquit.h).
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    tak::installSignalHandlers();
    // Persisted Options (audio/camera/display prefs). CLI flags still win where they
    // apply; the file is the source of truth for anything not passed on the CLI.
    tak::Settings settings = tak::loadSettings();
    // Select the saved output device before ANY audio opens (validated -- an absent
    // device falls back to system default). Keep the setting so the picker still shows
    // the user's choice even if it's currently unplugged.
    // setAudioDevice() calls initAudioCaps() internally, which snapshots each device's true
    // channel layout NOW -- before the menu music / door videos open a stereo stream that
    // would collapse the 5.1 sink's advertised layout to 2 -- and inits the audio subsystem
    // so the saved device actually validates (SDL_Init above is video-only).
    tak::setAudioDevice(settings.audioDevice);
    // Locate the retail data folder now that SDL (message boxes) and the config are up:
    // explicit --data, else the saved folder, else a native picker. Headless/harness runs
    // always pass --data, so they never prompt. Then mount it (must precede menu/game use).
    if (!dataRoot.empty() || !localHarness) {
        resolveDataDir(dataRoot, settings, /*allowPrompt=*/mpHeadless == 0);
        if (dataRoot.empty()) {
            std::fprintf(stderr, "no Total Annihilation: Kingdoms data folder selected -- exiting\n");
            SDL_Quit();
            return 1;
        }
        vfs = tak::hpi::mountRetailRoot(dataRoot, pol);
        gInstallRoot = dataRoot;   // the loading screen reads Movies/Gui from here
    }
    if (maxFps != 60) settings.maxFps = maxFps;          // --maxfps (if given) wins the file
    bool vsyncOn = settings.vsync && !noVsync;            // --novsync forces off
    std::string winTitle = std::string("takclient ") + tak::kVersion;
    SDL_Window* win = SDL_CreateWindow(winTitle.c_str(), SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, winW, winH,
                                       SDL_WINDOW_RESIZABLE);
    {   // Application icon: the crown badge (src/util/appicon).
        std::vector<uint8_t> ic = tak::appicon::render(tak::appicon::Kind::Client, 64);
        if (SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
                ic.data(), 64, 64, 32, 64 * 4, SDL_PIXELFORMAT_RGBA32)) {
            SDL_SetWindowIcon(win, s);
            SDL_FreeSurface(s);
        }
    }
    if (win && settings.fullscreen)
        SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP);
    Uint32 renFlags = SDL_RENDERER_SOFTWARE;
    if (shot.empty()) renFlags = noVsync ? SDL_RENDERER_ACCELERATED
                                         : SDL_RENDERER_PRESENTVSYNC;
    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, renFlags);
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
    // Static-art smoothing is sampled ONCE here, before any art is built. Textures keep
    // whatever factor they were built with, so a mid-session toggle must not be re-read
    // per texture -- the Options row says RESTART for exactly this reason.
    tak::art::setSmoothArt(settings.smoothArt);
    tak::art::setCursorFactor(settings.cursorScale);
    {
        SDL_RendererInfo ri{};
        if (SDL_GetRendererInfo(ren, &ri) == 0)
            std::fprintf(stderr, "renderer: %s%s\n", ri.name ? ri.name : "?",
                         (ri.flags & SDL_RENDERER_ACCELERATED) ? " (accelerated)"
                                                               : " (SOFTWARE)");
    }

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
    // Campaign chaining: a Next/Retry from the result screen re-enters the game
    // directly (skipping the menu) with this mission, movie + briefing and all.
    std::string pendingCampaign, pendingCampaignId;
    SDL_Texture* aaTex = nullptr;   // whole-frame supersampling target (Options AA); reused
    int aaW = 0, aaH = 0;
    tak::MenuMusic menuMusic;   // persists across menu -> lobby so the track doesn't restart
    menuMusic.setVolume(settings.masterVol, settings.bgmVol);
    // TAK_SHOT_RESULT=<png> [TAK_SHOT_RESULT_SIDE=0..4] [TAK_SHOT_RESULT_LOSE=1]:
    // render the end-of-game plate with a sample table and exit. Playing a whole match
    // out just to look at the screen isn't practical, so this is how its layout gets
    // verified against the retail art.
    if (const char* rs = tak::devEnv("TAK_SHOT_RESULT"); rs && *rs) {
        tak::ResultStats st;
        st.matchSec = 17 * 60 + 42;
        if (const char* sd = tak::devEnv("TAK_SHOT_RESULT_SIDE")) st.faction = std::atoi(sd);
        const char* names[4] = {"Curtis", "Bruce", "Ludwin", "Pat"};
        for (int i = 0; i < 4; ++i) {
            tak::ResultRow r;
            r.name = names[i];
            r.colorSlot = i;
            r.built = 120 - i * 23;
            r.kills = 48 - i * 11;
            r.losses = 31 + i * 9;
            r.isLocal = (i == 0);
            r.defeated = (i >= 2);
            r.timeSec = r.defeated ? 600 + i * 90 : st.matchSec;
            st.rows.push_back(std::move(r));
        }
        bool victory = tak::devEnv("TAK_SHOT_RESULT_LOSE") == nullptr;
        tak::ResultScreen::run(ren, vfs, victory, "MISSION 7", false, &settings, nullptr, &st);
        SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit();
        return 0;
    }
    // Retail intro: play the logo movie once at startup (scaled to the window), then
    // fall through to the front-end. Any key / click / window-close skips it. Menu
    // launches only, and never for a headless screenshot run.
    //
    // LOGO ONLY. intro.bik belongs on the PlayStory door, not here -- retail
    // dispatches on that gadget name and plays it there, once per launch (see
    // MainMenu::run). Playing both back to back at startup was wrong.
    if (fromMenu && shot.empty()) tak::MainMenu::playIntro(ren, dataRoot);
    std::string menuConnectError;   // failed MP connect -> shown when the menu reopens
    for (;;) {
    if (tak::termRequested()) { quitApp = true; break; }   // SIGTERM/SIGINT between sessions
    if (fromMenu) { serverHost = launchServerHost;
                    serverPort = launchServerPort; args = launchArgs;
                    menuMusic.start(vfs, 15);   // front-end BGM (idempotent; loops into the lobby)
                    // A pending Next/Retry re-enters the game directly, skipping the menu.
                    mode = pendingCampaign.empty() ? launchMode : std::string("game"); }

    // main-loop lobby driver: 0 = UI-driven lobby (browse/join/host), 7 = auto SP,
    // 8 = auto campaign mission (create the mission room, seat, start, then play).
    int mpAutoMode = 0;
    bool menuInteractive = false;   // menu single-player -> interactive lobby, not auto-play
    bool benchmarkLaunch = false;   // menu Benchmark -> auto-host an all-AI watch perf run
    int benchmarkLevel = 0;         // picked benchmark intensity 1..5
    std::string campaignStem;       // menu campaign pick -> host this mission (autoMode 8)
    std::string campaignId;         // ...its campaign id (for progress persistence)
    // Next/Retry chosen on the previous mission's result screen: re-enter directly.
    if (!pendingCampaign.empty()) {
        campaignStem = pendingCampaign; campaignId = pendingCampaignId;
        pendingCampaign.clear(); pendingCampaignId.clear();
        if (args.empty()) args.push_back("athri cay");
    }
    // Direct launch into a mission (`--campaign <stem>`): same host path as a menu
    // pick, resolving the campaign id so a win still advances persisted progress.
    if (!cliCampaign.empty()) {
        campaignStem = cliCampaign;
        if (args.empty()) args.push_back("athri cay");   // GameView needs a map; the mission overrides it
        for (const auto& c : tak::loadCampaigns(vfs)) {
            for (const auto& m : c.missions)
                if (m.stem == campaignStem) { campaignId = c.id; break; }
            if (!campaignId.empty()) break;
        }
    }

    // Front-end: the retail three-door main menu. Its choice drives the setup below
    // (single-player -> local server + lobby; multiplayer -> connect + browser).
    std::string rememberServer;   // menu-picked MP server; saved to Settings on a
                                  // successful connect (feeds the CONNECT dropdown)
    if (mode == "menu") {
        if (dataRoot.empty()) { std::fprintf(stderr, "menu: needs --data <retail-install-dir>\n"); return 1; }
        std::string menuServer;
        tak::MainMenu::Choice choice;
        {
            tak::MainMenu menu(ren, vfs, dataRoot);
            if (!menuConnectError.empty()) {   // reopen the dropdown with the error
                menu.setConnectError(menuConnectError);
                menuConnectError.clear();
            }
            choice = menu.run(shot, &menuServer, &menuMusic, &settings);
            if (choice == tak::MainMenu::Choice::Campaign) {
                campaignStem = menu.chosenMission();
                campaignId = menu.chosenCampaign();
            }
            if (choice == tak::MainMenu::Choice::Multiplayer) {
                // Take the account out of the menu while it is still alive, and
                // wipe its copy of the password immediately -- it has no further
                // use for it, and a secret should outlive its purpose by as little
                // as possible.
                loginUser = menu.chosenAccount();
                loginPass = menu.chosenPassword();
                menu.clearPassword();
            }
            if (choice == tak::MainMenu::Choice::Benchmark) {
                benchmarkLevel = menu.chosenBenchmarkLevel();
                if (benchmarkLevel < 1 || benchmarkLevel > tak::sim::kBenchLevels) benchmarkLevel = 3;   // safety default = High
            }
            // The credits door rolls the credits and returns to the menu, rather
            // than being a way out of the app.
            if (choice == tak::MainMenu::Choice::Credits) {
                tak::MainMenu::playIntro(ren, dataRoot, "credits.bik");
                continue;
            }
        }
        if (!shot.empty()) { SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit(); return 0; }
        if (choice != tak::MainMenu::Choice::SinglePlayer &&
            choice != tak::MainMenu::Choice::Multiplayer &&
            choice != tak::MainMenu::Choice::Benchmark &&
            !(choice == tak::MainMenu::Choice::Campaign && !campaignStem.empty())) {
            quitApp = true; break;   // exit / options (or campaign with no pick) -> leave the app
        }
        benchmarkLaunch = (choice == tak::MainMenu::Choice::Benchmark);
        mode = "game";
        if (args.empty()) args.push_back("athri cay");   // TODO: map picker (SP battle menu)
        if (choice == tak::MainMenu::Choice::Multiplayer) {
            std::string sv = menuServer.empty() ? std::string("127.0.0.1") : menuServer;
            rememberServer = sv;   // remembered (as picked/typed) if the connect succeeds
            auto colon = sv.find(':');   // accept host:port
            if (colon != std::string::npos) {
                int p = std::atoi(sv.substr(colon + 1).c_str());
                if (p > 0) serverPort = p;
                sv = sv.substr(0, colon);
            }
            serverHost = sv.empty() ? std::string("127.0.0.1") : sv;
        } else if (!benchmarkLaunch) {
            menuInteractive = true;   // single-player: local server, but stop in the lobby
        }
        // Benchmark: like single-player (local server) but auto-hosts an all-AI watch run
        // -- no interactive lobby, and mpAutoMode is forced to 1 below.
    }

    // Campaign mission: play the intro movie, then the briefing, before spinning up
    // the server and world. A BACK from the briefing skips the launch -- back to the
    // front-end for a menu pick, or exit for a --campaign launch.
    if (!campaignStem.empty() && !mpHeadless) {
        std::string title = "MISSION";
        for (const auto& c : tak::loadCampaigns(vfs))
            if (c.id == campaignId) {
                if (campaignStem == c.altFinal) title = "ALT ENDING";
                for (int i = 0; i < c.count(); ++i)
                    if (c.missions[size_t(i)].stem == campaignStem)
                        title = "MISSION " + std::to_string(i + 1);
            }
        menuMusic.setVolume(0, 0);   // hush the front-end track under the movie's own audio
        tak::MainMenu::playIntro(ren, dataRoot, (campaignStem + ".bik").c_str());
        menuMusic.setVolume(settings.masterVol, settings.bgmVol);
        if (!tak::BriefingScreen::run(ren, vfs, campaignStem, title, &settings, &menuMusic)) {
            campaignStem.clear(); campaignId.clear();
            if (fromMenu) continue;   // back to the front-end picker
            quitApp = true; break;    // a --campaign launch has nowhere to go back to
        }
    }

    if (mode == "game" && serverHost.empty() && !mpHeadless && !localHarness) {
        // Single-player: auto-launch a private local server and play a 1-v-AI game
        // on it (the AI runs server-side). Not visible to other players.
        int p = pickFreePort();
        std::string serverBin = resolveServerBin(argv[0]);
        if (p <= 0 || !spawnLocalServer(serverBin, dataRoot, p)) {
            std::fprintf(stderr, "single-player: could not launch a local server (%s)\n",
                         serverBin.c_str());
            return 1;
        }
        static bool atexitOnce = [] { std::atexit(killLocalServer); return true; }();
        (void)atexitOnce;   // register the safety-net teardown once (the loop kills it per session)
        serverHost = "127.0.0.1"; serverPort = p;
        // Campaign mission -> auto-host it (mode 8); menu skirmish stops in the lobby
        // (mode 0); CLI single-player auto-plays vs an AI (mode 7).
        mpAutoMode = benchmarkLaunch ? 1
                   : !campaignStem.empty() ? 8 : (menuInteractive ? 0 : 7);
        std::fprintf(stderr, "single-player: local server on port %d%s\n", p,
                     !campaignStem.empty() ? " (campaign)" : menuInteractive ? " (lobby)" : "");
    }

    // Connect to the multiplayer server, if requested.
    std::unique_ptr<tak::net::MpClient> mp;
    if (!serverHost.empty()) {
        mp = std::make_unique<tak::net::MpClient>();
        if (playerName.empty()) playerName = settings.playerName;
        if (playerName.empty()) playerName = "player";
        // Hello carries the PURE-RETAIL gameplay fingerprint (no overrides), so the
        // base game files are checked regardless of anyone's tier; the room's tier
        // and its gameplay overrides are agreed later, at load. The install is
        // immutable while the game runs, so the fingerprint (a mount + a read of
        // every gameplay file) is computed once and reused across menu->Play loops.
        static uint64_t retailHash = 0;
        if (!dataRoot.empty()) {
            if (!retailHash)
                retailHash = tak::hpi::gameplayHash(
                    tak::hpi::mountRetailRoot(dataRoot, tak::hpi::OverridePolicy::None));
            mp->setDataHash(retailHash);
        }
        // A freshly-spawned local server takes a moment to mount + listen (~0.25s
        // warm); poll fast so single-player doesn't pay coarse-sleep quantization.
        // Credentials for a server that requires an account. A local single-player
        // server requires none (it is launched --no-auth --local), so this is empty
        // there and the handshake never asks.
        if (!loginUser.empty()) {
            mp->setLogin(loginUser, loginPass);
            playerName = loginUser;   // the account IS the multiplayer identity
        }
        tak::crypto::wipe(loginPass);
        bool ok = false;
        for (int attempt = 0; attempt < (gLocalServerUp ? 200 : 1) && !ok; ++attempt) {
            ok = mp->connect(serverHost, uint16_t(serverPort), playerName);
            if (!ok && gLocalServerUp) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!ok) {
            std::fprintf(stderr, "server: %s\n", mp->error().c_str());
            killLocalServer();
            // A menu-launched connect failure returns to the front-end with the
            // error shown in the reopened CONNECT dropdown -- never exits the app.
            if (fromMenu) {
                menuConnectError = mp->error().empty()
                                       ? "COULD NOT CONNECT TO " + serverHost
                                       : mp->error();
                mp.reset();
                continue;
            }
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
        const bool loginRefused = mp->auth() == tak::net::MpClient::Auth::Failed;
        if (loginRefused || menuTimedOut || mp->state() == tak::net::MpClient::State::Done) {
            std::string why =
                menuTimedOut ? std::string("the server stopped responding while signing in")
                : mp->error().empty() ? std::string("the server closed the connection")
                                      : mp->error();
            std::fprintf(stderr, "server: %s\n", why.c_str());
            killLocalServer();
            if (fromMenu) {   // back to the sign-in panel with the reason in red
                menuConnectError = why;
                mp.reset();
                continue;
            }
            return 1;
        }
        if (mp->auth() == tak::net::MpClient::Auth::Created)
            std::printf("created account '%s' on %s\n", mp->account().c_str(), serverHost.c_str());
        if (!mp->account().empty()) {
            playerName = mp->account();
            // Remember the NAME for next time (never the password).
            if (settings.accountName != mp->account()) {
                settings.accountName = mp->account();
                tak::saveSettings(settings);
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
            if (ks.size() > 8) ks.resize(8);
            tak::saveSettings(settings);
        }
    }


    std::unique_ptr<MapView> mapView;
    std::unique_ptr<ModelView> modelView;
    std::unique_ptr<GameView> gameView;
    try {
        if (mode == "replay" && !args.empty() && !dataRoot.empty()) {
            // takclient replay <file.takrep> --data <retail-root>
            ReplayFile rf;
            if (!loadReplayFile(args[0], rf)) {
                std::fprintf(stderr, "replay: cannot read %s\n", args[0].c_str());
                return 1;
            }
            std::string mapPath = tak::hpi::findMap(vfs, rf.mapId);
            if (mapPath.empty()) { std::fprintf(stderr, "replay: map '%s' not found\n", rf.mapId.c_str()); return 1; }
            // Replay under the tier the game was recorded at (startReplay rebinds the vfs).
            auto rpol = tak::hpi::OverridePolicy(rf.overridePolicy <= 2 ? rf.overridePolicy : 2);
            if (rpol != pol) vfs = tak::hpi::mountRetailRoot(dataRoot, rpol);
            gameView = std::make_unique<GameView>(ren, std::move(vfs), mapPath, dataRoot, rpol,
                                                  false, false, false, /*bare=*/true, "ara", "tar",
                                                  rf.crusades);
            std::fprintf(stderr, "replay: %s -- map '%s', %zu ticks%s\n", args[0].c_str(),
                         rf.mapId.c_str(), rf.bundles.size(), rf.crusades ? " (Crusades)" : "");
            gameView->startReplay(rf.cfg, std::move(rf.bundles));
        } else if (mode == "map" && !args.empty() && !dataRoot.empty()) {
            // A "~gen1~" id is a random-map recipe MapView builds in memory; a plain
            // name resolves to a real .tnt in the mounted data.
            std::string mapPath = tak::mapgen::isGeneratedMapId(args[0])
                                      ? args[0]
                                      : tak::hpi::findMap(vfs, args[0]);
            if (mapPath.empty()) { std::fprintf(stderr, "map '%s' not found\n", args[0].c_str()); return 1; }
            mapView = std::make_unique<MapView>(ren, vfs, mapPath);
        } else if (mode == "game" && !args.empty() && !dataRoot.empty()) {
            std::string mapPath = tak::hpi::findMap(vfs, args[0]);
            if (mapPath.empty()) { std::fprintf(stderr, "map '%s' not found in %s\n", args[0].c_str(), dataRoot.c_str()); return 1; }
            // A multiplayer client builds the world from the server's GameStarting
            // later, so it constructs "bare" (no single-player 2-monarch spawn).
            // When looping back to the menu, keep this function's vfs alive for the
            // next session (+ its findMap); hand the game its own fresh mount.
            gameView = std::make_unique<GameView>(ren,
                                                  fromMenu ? tak::hpi::mountRetailRoot(dataRoot, pol) : std::move(vfs),
                                                  mapPath, dataRoot, pol, demo,
                                                  scenario, missionFlag,
                                                  navy || amphib || firetest || facetest || mp,
                                                  side, aiSide, crusades);
            gameView->applySettings(settings);   // audio / camera / UI-scale prefs
            gameView->setSettings(&settings);     // in-game Options edits + persists these
            if (mp) {
                gameView->setMpClient(mp.get());
                gameView->setMpMapId(args[0]);
                if (fromMenu) { gameView->setExternalLobbyMusic();  // front-end owns the lobby BGM
                                gameView->setCanReturnToMenu(); }   // in-game menu can return to it
                if (!campaignStem.empty())
                    gameView->setMissionStem(campaignStem);         // autoMode 8 hosts this mission
                else if (menuInteractive) gameView->setSinglePlayer();  // menu SP: SP-flavoured lobby, Create-first
                else if (benchmarkLaunch) {   // menu Benchmark: all-AI watch run + metrics
                    gameView->setBenchmark(benchmarkLevel);
                    gameView->setBenchmarkServerPid(localServerPid());
                }
                if (const char* rp = tak::devEnv("TAK_RESUME")) gameView->setResumePath(rp);
            }
            // Never let the window shrink below what the widest build-icon row
            // needs (full-size icons), and grow it now if it opened smaller.
            {
                int minW = gameView->minWindowWidth();
                SDL_SetWindowMinimumSize(win, minW, 480);
                int cw, ch;
                SDL_GetWindowSize(win, &cw, &ch);
                if (cw < minW) SDL_SetWindowSize(win, minW, ch);
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
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }

    bool running = true;
    float netAccum = 0;
    std::string serverMapId = args.empty() ? "" : std::filesystem::path(args[0]).stem().string();
    int ktPhase = keytest ? 0 : -1;
    float ktClock = 0;
    bool keytestSelectOnly = selonly;

    // Headless multiplayer test driver: auto-run the lobby + game loop against
    // takserver and print periodic hashes. Proves the server-sequenced lockstep
    // end to end without any SDL UI. (--mphost creates+starts, --mpjoin joins.)
    // Headless replay verify: play the whole recording and print the final hash.
    if (gameView && gameView->replayMode() && tak::devEnv("TAK_REPLAY_VERIFY")) {
        while (gameView->replayTick() < gameView->replayLength())
            gameView->replayStep(10.0f);   // guard caps to 64 ticks/call
        std::fprintf(stderr, "replay done: tick=%zu hash=%016llx units=%zu\n",
                     gameView->replayTick(), (unsigned long long)gameView->worldHashPublic(),
                     gameView->aliveUnits());
        return 0;
    }
    if (gameView && mp && mpHeadless) {
        std::string mapId = std::filesystem::path(args[0]).stem().string();
        // The harness runs the sim INLINE (single-threaded == deterministic + reproducible)
        // unless TAK_SIM_THREAD asks to verify the threaded sim against the referee.
        gameView->setSimThreadMode(tak::devEnv("TAK_SIM_THREAD") != nullptr);
        if (mpHeadless == 8) gameView->setMissionStem(missionStem);
        int limitTicks = int((startTime > 0 ? startTime : 60) * 30);
        // Jitter benchmark: run the client loop at a FIXED 60 fps (so the stall
        // metric is frame-rate-consistent) and enable the RTT probe. Otherwise the
        // usual tight poll loop.
        bool bench = tak::devEnv("TAK_NETBENCH") != nullptr;
        if (bench) gameView->netEnableRttProbe();
        while (true) {
            // Pin the snapshot for the iteration (mirrors the interactive render loop), so
            // cosmeticStep's front() reads can't tear against the worker under TAK_SIM_THREAD.
            gameView->beginFrame();
            bool cont = gameView->mpAutoStep(mpHeadless, mapId, crusades);
            gameView->endFrame();
            if (!cont || int(gameView->netTick()) >= limitTicks) break;
            SDL_Delay(bench ? 16 : 2);   // ~60 fps for the benchmark
        }
        // Flush + join the worker so the final world hash reflects every pushed tick (no read
        // race against a still-running worker). No-op when inline.
        gameView->shutdownSim();
        std::fprintf(stderr, "mp-headless done: tick=%u hash=%016llx units=%zu err=%s\n",
                     gameView->netTick(), (unsigned long long)gameView->worldHashPublic(),
                     gameView->aliveUnits(),
                     gameView->netError().empty() ? "none" : gameView->netError().c_str());
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
        mp->disconnect();
        return gameView->netError().empty() ? 0 : 1;
    }

    // Lobby driver for THIS session (TAK_MPAUTO overrides mpAutoMode). Computed once
    // per session -- NOT a function-static, which would freeze it at the first game's
    // value and break re-entry (e.g. a Benchmark launched after any earlier game would
    // inherit that game's mode and just sit in the lobby instead of auto-hosting).
    const int autoOv = tak::devEnv("TAK_MPAUTO")
                           ? std::atoi(tak::devEnv("TAK_MPAUTO")) : mpAutoMode;
    uint64_t last = SDL_GetPerformanceCounter();
    while (running) {
        if (tak::termRequested()) { running = false; quitApp = true; break; }
        // Pin the newest published sim snapshot for this whole iteration -- input handlers
        // (below) AND the render pass (further down) read front(), so the pin must span both
        // so a concurrent publish from the sim worker (Stage B) can't tear them. Released by
        // endFrame() after the cursor overlay, once every front()-reading pass is done.
        if (gameView) gameView->beginFrame();
        SDL_Event e;
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
            if ((e.type == SDL_RENDER_TARGETS_RESET ||
                 e.type == SDL_RENDER_DEVICE_RESET) && gameView)
                gameView->invalidateRenderTargets();
            // 'S' grabs a screenshot in the asset viewers; in game it is the
            // Stop hotkey (Keys.TDF LOWER_S), handled by GameView::input.
            if (!gameView && e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_s)
                screenshot(ren, kWinW, kWinH, "takclient_shot.png");
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
            if (mapView) mapView->input(e);
            if (modelView) modelView->input(e);
            if (gameView) gameView->input(e, ww, wh);
        }
        uint64_t now = SDL_GetPerformanceCounter();
        float dt = float(now - last) / float(SDL_GetPerformanceFrequency());
        last = now;
        // FPS readout in the window title (updated ~4x/sec).
        {
            static float fpsAcc = 0; static int fpsFrames = 0;
            fpsAcc += dt; ++fpsFrames;
            if (fpsAcc >= 0.25f) {
                char title[64];
                std::snprintf(title, sizeof title, "takclient %s  |  %.0f fps",
                              tak::kVersion, float(fpsFrames) / fpsAcc);
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
        // Whole-frame supersampling AA (Options): render the game to an oversized
        // target with SDL_RenderSetScale, then downscale it onto the window with
        // linear filtering. The scale only affects OUTPUT pixels -- framing, fixed-px
        // HUD and input all stay in 1x logical space, so nothing else has to change.
        // VRAM-safe: the target is allocated once and reused; if it can't be created
        // (VRAM pressure) we just fall back to no AA this frame. Baking runs at 1x
        // BEFORE the scale is set (the lazy atlas bakes reset the scale themselves
        // too, see their SetRenderTarget sites).
        float aaS = (settings.antiAlias == 4) ? 2.0f : (settings.antiAlias == 2) ? 1.4142f : 1.0f;
        // Cap the supersample target a safe margin below the GPU's texture/render
        // limit. A render target AT the max texture size misbehaves (renders/samples
        // short, then gets stretched to the window -- squeezing everything leftward,
        // the AA pointer drift). SDL can't report the true render limit and a readback
        // probe proved unreliable, so just stay 1/8 below the reported/assumed max. At
        // the very widest windows this trims 4X's supersample a touch -- imperceptible.
        static int aaMaxDim = 0;
        if (aaMaxDim == 0) {
            SDL_RendererInfo ri;
            int mx = (SDL_GetRendererInfo(ren, &ri) == 0)
                         ? std::min(ri.max_texture_width, ri.max_texture_height) : 0;
            if (mx <= 0 || mx > 16384) mx = 16384;
            aaMaxDim = mx - mx / 8;
            std::fprintf(stderr, "AA: max supersample dim %d (GPU reports %d)\n", aaMaxDim, mx);
        }
        if (aaS > 1.0f && w > 0 && h > 0)
            aaS = std::min(aaS, std::min(float(aaMaxDim) / w, float(aaMaxDim) / h));
        // VRAM-cap integration: the AA supersample target is a big optional texture
        // (up to ~230 MiB at 4K). Under memory pressure drop it entirely and release it;
        // otherwise step the scale down until the target fits the remaining budget.
        if (gpuvram::blocked()) { aaS = 1.0f; if (aaTex) { gpuvram::destroy(aaTex); aaTex = nullptr; aaW = aaH = 0; } }
        while (aaS > 1.0f && w > 0 && h > 0 &&
               !gpuvram::wouldFit(size_t(w * aaS) * size_t(h * aaS) * 4))
            aaS = (aaS > 1.5f) ? 1.4142f : 1.0f;   // 2x -> 1.41x -> off
        bool aaOn = false;
        if (gameView && aaS > 1.0f && !gameView->inLobbyPhase()) {
            int tw = int(w * aaS), th = int(h * aaS);
            if (!aaTex || aaW != tw || aaH != th) {
                if (aaTex) gpuvram::destroy(aaTex);
                aaTex = gpuvram::create(ren, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET, tw, th);
                if (aaTex) { SDL_SetTextureScaleMode(aaTex, SDL_ScaleModeLinear); aaW = tw; aaH = th; }
                else { aaW = aaH = 0; gpuvram::noteFail(); std::fprintf(stderr, "AA: %dx%d target alloc failed; AA off\n", tw, th); }
            }
            if (aaTex) aaOn = true;
        }
        // One-line diagnostic whenever the AA level changes, so it's clear on real
        // hardware whether supersampling actually engaged (or fell back on alloc).
        static int aaLoggedLevel = -99;
        if (gameView && !gameView->inLobbyPhase() && settings.antiAlias != aaLoggedLevel) {
            aaLoggedLevel = settings.antiAlias;
            if (settings.antiAlias == 0)
                std::fprintf(stderr, "AA: off\n");
            else if (aaOn)
                std::fprintf(stderr, "AA: %dX active -- %dx%d supersample target\n",
                             settings.antiAlias, int(w * aaS), int(h * aaS));
            else
                std::fprintf(stderr, "AA: %dX requested but INACTIVE (alloc failed?): %s\n",
                             settings.antiAlias, SDL_GetError());
        }
        // Create textures before the render pass (mid-pass creation glitches
        // the whole frame on some backends). Prepare/bake at 1x, then set the scale.
        if (mapView) mapView->ensureChunks(w, h);
        if (gameView) gameView->prepare(w, h);
        if (aaOn) { SDL_SetRenderTarget(ren, aaTex); SDL_RenderSetScale(ren, aaS, aaS); }
        SDL_SetRenderDrawColor(ren, 18, 18, 26, 255);
        SDL_RenderClear(ren);
        // Optional per-phase profiler (TAK_PROF=1): prints where each frame's
        // wall-clock goes, once a second, so a stall can be localised on real
        // hardware that the headless software renderer can't show.
        static const bool prof = tak::devEnv("TAK_PROF") != nullptr;
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
        double t2 = t1, t3 = t1;
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
            } else if (gameView->replayMode()) {
                gameView->replayStep(dt);   // play back a recorded .takrep
            } else if (gameView->isNet()) {
                // Server-sequenced lockstep: one mpAutoStep pumps the connection,
                // advances the lobby (auto-matchmaking for now -- a lobby UI is
                // follow-on), and simulates every delivered tick. (void)netAccum.
                (void)netAccum;
                // autoOv (computed once per session above) drives the lobby: 0 =
                // UI-driven, 1 = auto-host, etc. TAK_MPAUTO can override it.
                gameView->mpAutoStep(autoOv, serverMapId, crusades);
            } else {
                gameView->update(dt);
            }
            // Advance unit animation every render frame (decoupled from the 30Hz sim tick),
            // so the walk cycle is smooth at display rate and the heavy parallel VM pass no
            // longer piles onto the 1-in-8 net frame that runs the sim tick.
            if (!benchFrozen) {
                gameView->benchmarkCamera(dt, w, h);   // benchmark flythrough (no-op otherwise)
                gameView->animFrame(dt);
                gameView->benchmarkSample();   // perf samples at each 10s milestone (no-op unless benchmarking)
            }
            t2 = prof ? pnow() : 0;
            gameView->draw(w, h);
            t3 = prof ? pnow() : 0;
            if (prof) { pUpd += t2 - t1; pDraw += t3 - t2; }
            // Feed the whole real frame time (dt = last frame's total incl. present)
            // to the sprite auto-tuner, so a GPU-bound full-model crowd triggers it.
        }
        double t4 = prof ? pnow() : 0;
        if (aaOn) {   // downscale the supersampled frame onto the window
            SDL_RenderSetScale(ren, 1.0f, 1.0f);
            SDL_SetRenderTarget(ren, nullptr);
            SDL_SetTextureScaleMode(aaTex, SDL_ScaleModeLinear);   // ensure a smooth downscale
            // Blit to an EXPLICIT full-drawable rect. A nullptr dst resolves to the
            // renderer's logical size, which on some backends (Wayland) is smaller
            // than the real drawable -- squeezing the frame leftward so the cursor
            // drifts. Use the actual output size so it fills the whole window.
            int bw = 0, bh = 0;
            SDL_GetRendererOutputSize(ren, &bw, &bh);
            SDL_Rect dst{0, 0, bw, bh};
            SDL_RenderCopy(ren, aaTex, nullptr, &dst);
        }
        // Custom animated mouse cursor, drawn last so it sits above the HUD (and above
        // the AA-resolved scene) at native resolution. Only in-game; the asset viewers
        // keep the OS arrow.
        if (gameView) gameView->drawCursorOverlay();
        if (gameView) gameView->endFrame();   // release the pinned sim snapshot for this frame
        SDL_RenderPresent(ren);
        if (prof) {
            double t5 = pnow();
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
            static const bool spikes = tak::devEnv("TAK_SPIKES") != nullptr;
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
                            "[proj=%.1f submit=%.1f (shadow=%.1f) other=%.1f] present=%.1f | units=%llu shverts=%lluk\n",
                            pFrames * 1000.0 / pAcc, pUpd / pFrames, sim / pFrames,
                            pDraw / pFrames, proj / pFrames, submit / pFrames,
                            shadow / pFrames, (pDraw - proj - submit) / pFrames, pPres / pFrames,
                            (unsigned long long)(units / uint64_t(std::max(1, pFrames))),
                            (unsigned long long)(shVerts / uint64_t(std::max(1, pFrames)) / 1000));
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

        if (!shot.empty()) {
            // Render a few frames so lazy content settles, then capture. For content
            // that settles asynchronously (a network spectator building its world),
            // TAK_SHOT_MS waits that many wall-clock ms before capturing instead.
            static const char* shotMsEnv = tak::devEnv("TAK_SHOT_MS");
            static uint64_t shotT0 = SDL_GetTicks64();
            static int frames = 0;
            bool ready = shotMsEnv ? (SDL_GetTicks64() - shotT0 >= uint64_t(std::atoi(shotMsEnv)))
                                   : (++frames >= 3);
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
                    // TAK_SHOT_PRESS=<SDL key name> taps one key before the capture, so
                    // a harness run can shoot an overlay (Unit Info, the F4 scoreboard)
                    // instead of only the plain game view.
                    // TAK_SHOT_CLICKS="x,y;x,y" taps those points, one per armed pass,
                    // before the capture -- enough to walk a menu into the state worth
                    // photographing (the lobby's map picker needs two clicks).
                    static size_t clickIdx = 0;
                    if (const char* cl = tak::devEnv("TAK_SHOT_CLICKS")) {
                        std::vector<std::pair<int, int>> pts;
                        std::string acc(cl);
                        size_t p0 = 0;
                        while (p0 <= acc.size()) {
                            size_t e = acc.find(';', p0);
                            std::string one = acc.substr(p0, e == std::string::npos ? std::string::npos : e - p0);
                            size_t c = one.find(',');
                            if (c != std::string::npos)
                                pts.push_back({std::atoi(one.c_str()), std::atoi(one.c_str() + c + 1)});
                            if (e == std::string::npos) break;
                            p0 = e + 1;
                        }
                        if (clickIdx < pts.size()) {
                            SDL_Event ev{};
                            ev.type = SDL_MOUSEMOTION;
                            ev.motion.x = pts[clickIdx].first; ev.motion.y = pts[clickIdx].second;
                            SDL_PushEvent(&ev);
                            ev.type = SDL_MOUSEBUTTONDOWN;
                            ev.button.button = SDL_BUTTON_LEFT;
                            ev.button.x = pts[clickIdx].first; ev.button.y = pts[clickIdx].second;
                            SDL_PushEvent(&ev);
                            ev.type = SDL_MOUSEBUTTONUP;
                            SDL_PushEvent(&ev);
                            ++clickIdx;
                            shotArmed = false;   // let it land, then try again next pass
                        }
                    }
                    static bool pressSent = false;
#ifndef NDEBUG
                    // TAK_SHOT_TRAIL=1 queues a short move chain on one owned unit so
                    // the order line has something to draw. The orders have to round-trip
                    // through the server and come back in a bundle before they show up in
                    // a render snapshot, so hold the capture for a good number of passes:
                    // 30 was not enough and produced an empty picture that looked exactly
                    // like a broken feature.
                    static int trailWait = -1;
                    if (tak::devEnv("TAK_SHOT_TRAIL") && gameView && trailWait != 0) {
                        if (trailWait < 0) trailWait = gameView->debugQueueDemo() ? 120 : 0;
                        else --trailWait;
                        if (trailWait > 0) shotArmed = false;
                    }
#endif
                    if (const char* kn = tak::devEnv("TAK_SHOT_PRESS"); kn && !pressSent) {
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
                    screenshot(ren, w, h, shot);
                    running = false;
                }
            }
        }
    }
    // Campaign mission ended (and resolved -- not a mid-mission quit): advance
    // persisted progress on a win, then show the result screen and act on the choice.
    if (!campaignStem.empty() && !campaignId.empty() && gameView && !quitApp &&
        gameView->missionOutcomePublic() != 0) {
        int oc = gameView->missionOutcomePublic();
        std::string title = "MISSION", nextStem;
        bool finalMission = false;
        for (const auto& c : tak::loadCampaigns(vfs)) {
            if (c.id != campaignId) continue;
            int completedIdx = -1;   // which slot was just beaten (kAltMission for the alt branch)
            if (campaignStem == c.altFinal) {   // terminal alt branch
                title = "ALT ENDING"; finalMission = true; completedIdx = tak::kAltMission;
            } else for (int i = 0; i < c.count(); ++i)
                if (c.missions[size_t(i)].stem == campaignStem) {
                    title = "MISSION " + std::to_string(i + 1);
                    if (i + 1 >= c.count()) finalMission = true;
                    completedIdx = i;
                    if (i + 1 < c.count()) nextStem = c.missions[size_t(i + 1)].stem;  // "play next" convenience
                    break;
                }
            // Victory: record THIS mission as completed. Nothing is ever locked -- this
            // only tracks what's been beaten.
            if (oc > 0 && completedIdx != -1 &&
                settings.campaignCompleted[campaignId].insert(completedIdx).second)
                saveSettings(settings);
            break;
        }
        tak::ResultStats st = gameView->resultStats();   // read before the sim is freed
        killLocalServer(); mp.reset(); gameView.reset();   // free the mission before the movie/modal
        if (oc > 0) {
            // Cinematics on victory: a per-mission "post<stem>" cutscene (retail ships
            // one after Book of Darien mission 24), then the campaign's ending credits
            // after its final mission. Each is a no-op if the movie isn't present.
            menuMusic.setVolume(0, 0);
            tak::MainMenu::playIntro(ren, dataRoot, ("post" + campaignStem + ".bik").c_str());
            if (finalMission)
                tak::MainMenu::playIntro(ren, dataRoot,
                                         campaignId == "book of darien" ? "posttakcredits.bik" : "credits.bik");
            menuMusic.setVolume(settings.masterVol, settings.bgmVol);
        }
        tak::ResultChoice rc = tak::ResultScreen::run(ren, vfs, oc > 0, title,
                                                      oc > 0 && !nextStem.empty(), &settings,
                                                      &menuMusic, &st);
        if (rc == tak::ResultChoice::Next)       { pendingCampaign = nextStem;     pendingCampaignId = campaignId; }
        else if (rc == tak::ResultChoice::Retry) { pendingCampaign = campaignStem; pendingCampaignId = campaignId; }
        // Menu -> pendingCampaign stays empty -> the outer loop re-shows the front-end.
    } else if (campaignStem.empty() && gameView && !quitApp && fromMenu &&
               gameView->outcomePublic() != 0) {
        // Skirmish / multiplayer: retail shows the same victory/defeat plate here,
        // with the match's statistics table. No next mission and nothing to retry,
        // so both buttons come back to the front end.
        int oc = gameView->outcomePublic();
        tak::ResultStats st = gameView->resultStats();
        killLocalServer(); mp.reset(); gameView.reset();
        tak::ResultScreen::run(ren, vfs, oc > 0, "", false, &settings, &menuMusic, &st);
    }
    // Session ended: tear down any single-player local server, then either loop back
    // to the menu or exit the app.
    killLocalServer();
    mp.reset();
    // Destroy the session's views NOW (not at scope end below): the renderer is
    // reused across sessions, so their GPU textures must be freed before the next
    // session budgets against gpuvram. The log line is the leak canary -- healthy
    // teardown leaves single-digit MiB (menu-owned art only); before the teardown
    // fixes it climbed by hundreds of MiB per benchmark until terrain chunks
    // could no longer upload (map stuck at the low-res underlay).
    gameView.reset();
    mapView.reset();
    std::fprintf(stderr, "gpu: %zu MiB in %zu textures tracked after session teardown (cap %zu MiB)\n",
                 gpuvram::bytes() >> 20, gpuvram::count(), gpuvram::cap() >> 20);
    // Clear the in-game minimum-window-size constraint (set per game at the build-icon
    // sizing above). Leaving it on the persistent window makes the returned menu's
    // surface re-negotiation-prone on Wayland (see the Options click-death bug).
    SDL_SetWindowMinimumSize(win, 0, 0);
    // Free the (large) AA supersample target on the way back to the menu -- a benchmark
    // at 4K leaves a multi-hundred-MB texture allocated on a VRAM-tight GPU, which can
    // stall the menu's present. It's rebuilt on demand when the next game needs AA.
    if (aaTex) { gpuvram::destroy(aaTex); aaTex = nullptr; aaW = aaH = 0; }
    if (quitApp || !fromMenu) break;
    }  // ---- end outer session loop ----

    if (aaTex) gpuvram::destroy(aaTex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}

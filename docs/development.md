# Build and development guide

[Back to the README](../README.md) · [User guide](user-guide.md)

The engine version is set by `project(... VERSION ...)` in `CMakeLists.txt`.
It is separate from the multiplayer protocol version in `src/net/protocol.h`.

## Building

Use CMake ≥ 3.24, C and C++20 compilers (GCC/Clang or MinGW-w64), Ninja, Git,
Make, and pkg-config. x86 FFmpeg builds also need NASM for SIMD support.
Linux streaming dependencies need Python 3, patch, and Perl standard modules (`perl-core` on Fedora). Linux and macOS builds require the vendored static
zlib, libjpeg-turbo, SDL2, and Bink/streaming FFmpeg libraries. Installing a system
SDL2 package alone is not sufficient.

On Linux, install development headers for the SDL video/audio backends you need
(X11/Wayland, ALSA/PulseAudio, OpenGL/EGL). Backends whose headers are absent
when SDL is built may be unavailable. Linux also requires the static C++ runtime
archives; on Fedora these include `libstdc++-static`. The exact package lists
used by CI are in [linux.yml](../.github/workflows/linux.yml).

From the repository root:

```sh
./tools/build-ffmpeg-bink.sh
./tools/build-static-deps.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

The dependency scripts download their sources and reuse existing installations
under `third_party/`. Their `PREFIX` environment variable sets the destination;
pass matching `-DTAK_FFMPEG_PREFIX=...` and `-DTAK_STATIC_DEPS_PREFIX=...`
CMake options when using custom locations. Missing required libraries fail
configuration rather than silently switching to shared libraries.

Voice chat's codec, Opus 1.5.2, is built statically from its pinned,
SHA-256-verified source release (`cmake/Opus.cmake`, client only, BSD licence in
`OPUS-LICENSE.txt`). CMake downloads it on first configure; offline, set
`-DFETCHCONTENT_SOURCE_DIR_TAK_OPUS=/path/to/opus-1.5.2`. It adds no runtime
Opus DLL/shared-library dependency. The interface font (the chat box, and all plain UI text under UI FONT = MODERN; `src/client/uifont.h`, `src/client/uitext.h`), Noto Sans Bold subset to Latin-1
(`res/chat-font`, SIL OFL 1.1), is embedded into takclient by `cmake/EmbedFiles.cmake` and
drawn with the vendored stb_truetype (`src/vendor/stb`, public domain / MIT); both notices
are in `CHAT-FONT-LICENSE.txt`, which ships beside the binaries.

For Clang AddressSanitizer builds, select both `CMAKE_C_COMPILER=clang` and
`CMAKE_CXX_COMPILER=clang++`: Opus is compiled as C, so mixing GCC C with
Clang C++ can introduce incompatible sanitizer runtimes.

For developer launch modes, diagnostics, and headless harnesses, use a Debug
build. An optimized Debug build keeps those features while improving performance:

```sh
cmake -S . -B build-o2 -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS_DEBUG="-O2 -g" -DTAK_TEST_DATA=/path/to/tak_install
cmake --build build-o2 -j4
ctest --test-dir build-o2 --output-on-failure
```

`TAK_TEST_DATA` enables retail-data tests in addition to the data-independent
suite. Some animation checks also require extracted scripts under
`assets/extracted/all/scripts`. Rebuild **all targets** after simulation, AI, or
network changes so the client, server, and tools use the same code.

### Building a tagged release

Every release has a git tag (the [CHANGELOG](../CHANGELOG.md) lists them). To
build exactly a release, check out its tag:

```sh
git clone https://github.com/pocketgeek/tak-engine.git
cd tak-engine
git checkout v1.0.9
```

Then follow the build steps in that checkout's own `docs/development.md`. A
client plays only with servers and replays of its own protocol, so a client built
from a tag needs a server built from a tag of the same protocol.

### Static dependencies and menu videos

Menu door clips use Bink video decoded by the bundled, minimal FFmpeg build.
No separate FFmpeg installation is needed at runtime. FFmpeg is required for a
source build; a missing or unreadable clip can fall back to static menu art.
The dependency script also builds H.264/AAC streaming with static x264 (GPL),
compatible with the engine's GPLv3 license. See [streaming](streaming.md) for
platform backends, dependency boundaries and tests.

SDL2, libjpeg, zlib, and FFmpeg are linked statically. Linux also embeds the
GCC/C++ runtime but retains system C libraries; SDL loads available windowing and
audio backends at runtime. Windows binaries need no separately bundled SDL,
JPEG, zlib, FFmpeg, or MinGW runtime DLLs. macOS uses system libraries and
frameworks. CI checks these dependency boundaries for release packages.

### Platform builds and releases

- **Windows x64:** use the MSYS2 **MINGW64** environment with its GCC, CMake,
  Ninja, SDL2, libjpeg-turbo, zlib, and pkgconf packages, plus Git, Make, and patch.
  Run `./tools/build-ffmpeg-bink.sh`, then configure and build as above.
  CMake uses the toolchain's static dependency archives; the native
  `build-static-deps.sh` step is not needed. See
  [windows.yml](../.github/workflows/windows.yml) for the exact package list and
  installer build.
- **Windows x64 cross-built on Linux** (a local check, not how releases are
  made): with the MinGW-w64 GCC (Fedora `mingw64-gcc-c++`), cross-build the static
  archives first, since the distribution's MinGW packages ship DLL import libraries
  only. Run `build-static-deps.sh` with `PREFIX=<deps-win>`,
  `CHOST=x86_64-w64-mingw32`, `CMAKE_TOOLCHAIN_FILE=/usr/share/mingw/toolchain-mingw64.cmake`
  and `STATIC_DEPS_SKIP_WAYLAND_CHECK=1`, then `build-ffmpeg-bink.sh` with
  `PREFIX=<ffmpeg-win>`, `CROSS_PREFIX=x86_64-w64-mingw32-`, `TARGET_OS=mingw32` and
  `TARGET_ARCH=x86_64`. Configure with a toolchain file that includes the
  distribution's and puts both prefixes FIRST on `CMAKE_FIND_ROOT_PATH`
  (`list(PREPEND ...)`; otherwise SDL2 and libjpeg resolve to the sysroot's import
  libraries), plus `-DTAK_STATIC_DEPS_PREFIX=<deps-win> -DTAK_FFMPEG_PREFIX=<ffmpeg-win>`,
  with `PKG_CONFIG_LIBDIR=<ffmpeg-win>/lib/pkgconfig` in the environment.
- **Windows ARM64:** use Windows 11 ARM64 and MSYS2 **CLANGARM64** with its
  native Clang/LLVM and ARM64 dependency packages. Set `CC=clang`, `CXX=clang++`,
  `TARGET_OS=mingw32`, and `TARGET_ARCH=aarch64` before running the FFmpeg script
  and CMake commands. As on Windows x64, use the toolchain's static archives
  instead of `build-static-deps.sh`. The Windows workflow runs natively on
  `windows-11-arm`, checks executable architecture and static dependency imports,
  and produces Release and Debug portable ZIPs. ARM64 ZIPs are published with each
  release; the NSIS installer is x64 only. The pinned FFmpeg version
  excludes NVENC on Windows ARM64; x264 CPU streaming remains available. Qualcomm
  hardware encoding is not implemented.
- **macOS ARM64 and Intel x64:** install Xcode Command Line Tools, then
  `brew install cmake ninja pkg-config`. Run both dependency scripts and the
  source-build commands above, adding `-DCMAKE_FIND_FRAMEWORK=LAST` at configure
  time. CI builds and tests each architecture on a native runner, checks that
  executables link only system libraries/frameworks, and packages signed app
  bundles, DMGs, and Release/Debug ZIPs. CI packages target macOS 14 or later;
  use `MACOSX_DEPLOYMENT_TARGET=14.0` for matching source builds. Intel packages
  are published with each release, as well as CI artifacts.
  See [macos.yml](../.github/workflows/macos.yml) for app/DMG packaging.
- **Linux x64 and ARM64:** native CI runners build Ubuntu 22.04/24.04/26.04,
  Debian 12/13, Fedora 44/45, openSUSE Leap 16.0 and Tumbleweed, and EL9/EL10
  (one package per major release for RHEL, Rocky and Alma, built on AlmaLinux with a
  `gcc-toolset`) containers for each architecture. Arch's official container builds
  x64 only. The Ubuntu 22.04 leg (the oldest glibc) also makes an AppImage per
  architecture (`packaging/appimage/build.sh`) from the same static binaries, for any
  other distribution and the Steam Deck. Choose the package matching both your
  distribution and architecture (`amd64`/`arm64` for DEB, `x86_64`/`aarch64` for RPM).
  ARM64 uses GitHub's [native ARM runners](https://docs.github.com/en/actions/reference/runners/github-hosted-runners),
  not emulation. Dependencies and caches are isolated by distro and architecture;
  both architectures run the same package-workflow regression tests, ELF checks,
  package metadata checks, and static-link gate. Ubuntu 22.04 supplies each
  architecture's Debug tarball. ARM64 artifacts are available from CI and are
  attached to releases. This does not cover 32-bit ARM or certify
  graphical performance on individual ARM boards.
- **Slackware64 15.0:** CI builds inside the Slackware 15.0 container referenced by
  [SlackDocs](https://docs.slackware.com/howtos:misc:slackware_docker_image), using
  its native toolchain and libraries. `packaging/slackware/package.sh` stages
  CMake's install tree and creates a `.txz` with Slackware's `makepkg`; CI installs
  it and checks client/server startup. The package includes no active systemd unit
  because Slackware uses BSD-style init. Launch the server directly or configure
  local startup yourself. No daemon starts during installation.

The platform workflows build Release and selected Debug artifacts on every
`main` push. The determinism workflow runs for relevant source changes; Windows
and macOS also check the math golden hash natively. Retail-data tests run locally
because the game assets are not included in the repository or CI.

To cut a release, update the CMake project version and README version, commit and
push, and wait for platform checks to pass. Then tag that commit and push the tag:

```sh
git tag -a vX.Y.Z -m "TAK Engine X.Y.Z"
git push origin vX.Y.Z
```

Tags matching `v*` trigger package builds and GitHub Release uploads. Check every
platform job and the complete asset set, then update the release notes; workflows
can create/publish the release with generated notes during upload.
To collect packages privately, create a draft release for the pushed tag before
the upload steps run. Upload workflows reuse that draft; publish it only after
the platform checks, Windows signing and complete asset verification pass.

## Faster verification loop

- **Builds.** CMake uses `ccache` and `mold` automatically when they are installed
  (a second build of the same sources, or of another worktree, is mostly cache hits).
- **Targeted tests: `ctest -L quick -j 8`.** Every test that is not `nightly` and takes
  under 5 s is labelled `quick`, plus the four `retailgap_*` slices, which are slower but
  check too much to skip (about 630 of the roughly 930 tests, about a minute at `-j 8`,
  against several minutes for the full suite). Use it for a task's own checks; run the full suite
  (`ctest -j 8`) before handing a change over. Other labels: `legion`, `w2`, `nightly`
  (`ctest -L legion`, `-LE nightly`).
- **Tests never touch your profile.** CTest gives every test its own `XDG_DATA_HOME`,
  `XDG_CONFIG_HOME` and, on Linux, `HOME` under `<build>/test-profiles/<test>/`, so the
  settings file, replays and Cartographer preferences a test writes stay in the build
  dir; a new test needs nothing. Debug builds and test executables follow
  `XDG_DATA_HOME` on Windows and macOS too (`src/util/prefpath.h`; SDL alone honours it
  only on Linux), so the same variable isolates an ad-hoc debug run outside ctest,
  such as a `--mpai` harness game: `XDG_DATA_HOME=$(mktemp -d)`. Every ctest run ends with
  `pref_guard_check`, which fails if a test wrote a `TAKengine` folder into its scratch
  `HOME` (a process that lost its `XDG_DATA_HOME`: the real profile outside ctest);
  `pref_guard_lint` fails on an `SDL_GetPrefPath` call outside `prefpath.h`.
- **Long tests start first.** `tools/ctest_costs.txt` lists the tests of 5 s or more;
  CMake turns it into `COST` properties, so even in a fresh build dir (no
  `CTestCostData.txt` yet) the 150 s scenario runs start at once instead of last. After adding a
  slow test, or when timings move, regenerate it from a full local run:
  `ctest --test-dir build-o2 -j 8 && tools/ctest_costs.py build-o2`.
  The longest scenario tests (`legion_gen1_route_*`, `legion_situation_*`) are split
  into parallel parts (per start offset and per mode respectively); `legion_scenario`
  checks serial == workers per mode and offset and gates nothing across them, so the
  parts together check exactly what one whole run would.
- **Exact-identity harness: `tools/legion_identity.sh --base <build> --cand <build>`.**
  Base results are cached under `~/.cache/tak-identity/<base commit>/<build type>-v<N>/`
  (every replay, golden, `--mpai` game, ctest run, `check-determinism.sh` and crowdbench
  matrix row), so a chain of steps runs each base once and only the candidate afterwards.
  The cache is used only for a clean base tree whose build dir builds (`cmake --build`
  is run first); only complete, successful base results are stored; the candidate is
  never read from the cache, so a hashed-state change always shows as DIFF; a run that passes
  with every row SAME also stores the candidate's results under the candidate's own commit, so in
  a chain A->B, B->C the second step finds its base warm. `--no-cache` ignores it,
  `-j N --cores a-b` sets the job width and CPU list, `--quick` also leaves the `nightly` tests out of
  its ctest section (they are most of its time), and crowdbench starts beside the
  tail of the replays instead of after them. Bump `HARNESS_VERSION` in
  `tools/legion_identity.py` when a base row's definition changes. The 2-hour
  replays are platform independent, so running them on the Windows or macOS test hosts
  is possible but not automated: the harness relies on `taskset`/`setsid`.

## WE timing rounds (`tools/we_timing.sh`)

Engine-wide perf (WE) steps are judged by swapped, concurrent A/B pairs on reserved timing cores 0-3, with
everything else on 4-23. Every timed run goes through a local wrapper script that flocks those cores: `TAK_TIMING_SH`
names it (the default is the maintainer's `/home/pocket_geek/tak-tmp/tools/timing.sh`; set your own). Replaying R-2h /
L-2h / R-bench / L-bench in full, twice, swapped, takes about 70 minutes a round. `tools/we_timing.sh` does the same job
in about 10 minutes for two swapped rounds:

```sh
tools/we_timing.sh --base <base build-o2> --cand <cand build-o2> [--rounds 2] [--sits R-2h-1800,...] [--counters]
tools/we_timing.sh --base ... --cand ... --replays <dir of .takrep>   # exact mode, see below
```

- **Situations** (`tools/scenarios/we-timing/*.scn.gz`) are worlds cut out of the four WE recordings by
  `tools/we_timing_harvest.sh <takclient> <replay> <tick> <out.scn.gz>` (TAK_SITUATION, `src/client/situation.h`):
  every live body with its exact position, heading, hit points, speed, id and player goals, the recording's commands for
  the next 3000 ticks (`TAK_SITUATION_CMDS`), clock and RNG, the explored masks and the teams. The map is named
  (`map named Ulasem Arena`), so the files need `--data`. `R-2h-1800` and `L-2h-1800` are cut at tick 1800 (11.9k units,
  falling through the 9.5-10.5k window), the bench ones at 1900 (just after the 60 s benchmark ramp: the ramp's spawns are
  World::tick work a situation cannot replay). Re-cut with the o2 `takclient` of the tree whose recordings play back
  (`legion_identity.py patch-replay` re-stamps the protocol of a current-format recording) when Legion behaviour
  changes: an L situation holds the trajectory of the build that cut it.
- **Run shape.** Per situation, two swapped concurrent pairs (base on 0-1 + candidate on 2-3, then swapped) per round:
  `legion_scenario --workers --catchup 60 --ticks warm+measure` with `TAK_PHASE=1`. Defaults: warm-up 1000 ticks (dropped),
  measure 1000. `--catchup N` gives the path service an unlimited budget for the first N ticks: a situation re-issues
  7k in-flight orders at tick 0, and the retail request queue would otherwise drain that backlog over ~2000 ticks
  (the long waypoint queues of ~900 units, which findTarget walks, take that long to reappear). The report prints, per
  situation and SIMPHASE phase, the median of each build's per-run mean ms, their ratio, and the median and range of the
  per-pair ratios (the headline: pair partners run concurrently, which cancels most host noise), plus the final state hash
  of base and candidate (a hash-identical step must print `identical`). `--counters` adds the TAK_SIMSTATS work counters
  (do not compare ms from a `--counters` run).
- **Both trees need the tool files** (`tools/legion_scn.h`, `tools/legion_scenario.cpp` `--catchup`, `src/client/situation.h`,
  `src/client/ordershape.h`).
- **`--replays DIR`** runs the exact thing instead: each recording played from tick 0 and cut off at harvest + warm +
  measure ticks (a debug `takclient` per build dir; the SIMPHASE lines before the harvest tick are dropped). Same report,
  no situation fidelity limit, roughly twice the wall of the situation mode and still about a third of a full-length
  replay round. Use it whenever a result will be quoted for a phase where situations and replays disagree (below).
- **Identity is still the full replays** (`tools/legion_identity.sh`): a situation is not byte-identical to the recording
  and proves nothing about hashes.
- **Situation timing is for SCREENING.** Use it to find and rank candidate steps quickly; before calling a WE step done,
  confirm any claimed gain with `--replays` (exact mode). This applies above all to Legion combat and order-queue claims:
  one such step measured -12.5% in the replays and +1.9% as a situation on L-2h, so a situation result there is neither
  a confirmation nor a refutation. A quoted final number comes from `--replays`, never from a situation.

### How close situations are

Checked against truncated replays of the same pairs, window and cores, the combat-phase pair ratios of situations
agree with the replays to within about 3 percentage points in most cases and 7 in all but one, where the replay pairs
themselves spread by 5-15 points. The exception is a Legion combat step on L-2h (an acquisition-order change), where
the situation shows no gain and the replay -12..-14%, with matching work counters; the cause is not known. A situation
of R-2h regrows the recording's long waypoint queues only to about 85% even with `--catchup 60`, so a gain there can
read a few points smaller than in the replay. Absolute ms are not comparable between modes (a situation tick runs from
18% cheaper to 4% dearer than the recording's), and the host differs between days, so compare only within one pair.
A round of two swapped rounds of all four situations takes 7-11 minutes, against about 70 for the replays.

## Developer launch modes

Release clients accept only `--data`, `--version`, `--help` and the Cartographer
handoff `--play-map <snapshot.kmp>` (with optional `--game-mod <pack>`), and launch
games through the menus. Debug builds also expose asset viewers, direct game/replay launch, and
headless diagnostic harnesses. See the [command-line reference](user-guide.md#command-line)
and `build-o2/takclient --help`; additional harness arguments are defined in
[src/client/main.cpp](../src/client/main.cpp). `TAK_*` diagnostic
environment hooks (client, server, sim, AI and Cartographer) are disabled in Release
builds; the only ones a release build reads are takserver's test timers
`TAK_TIMEOUT_MS`, `TAK_LOAD_TIMEOUT_MS`, `TAK_STALL_MS`, `TAK_DROP_WAIT_MS` and
`TAK_OVER_DRAIN_MS` ([public server](public-server.md)).

A debug `takclient game "<map>" --data <dir>` without `--server` is single-player: it
starts a private local `takserver` (no auth, loopback) and opens a single-player room on
that map with the player seated: the room the menu's PLAY THE MACHINE opens, but on the
named map, with the default rules and nothing remembered. The game starts only when
someone seats AIs and clicks START, so a scripted run in a real window waits in the room
forever. `TAK_SP_AIS=N` seats N AI opponents (the first plays `--aiside`, default
Taros) and starts the game at once; add `TAK_QUIT_TICK=<tick>` to leave when the game
reaches that tick. For example
`TAK_SP_AIS=1 TAK_QUIT_TICK=300 build-o2/takclient game "Inner Circle" --data <dir>
--winsize 1280 720` (on Windows, `set` the two variables first) loads, plays 300 ticks
against one AI and exits.
The `--mpai` harness (with its own `takserver`, `--server` and `--serverport`)
needs neither.

`TAK_ROOM_SCRIPT` drives a debug client's game browser and game room through the
members their buttons call, in any launch mode: a `;`-separated list of steps. In the
browser: `create[:<name>[:<password>]]` (CREATE, its window's fields, its CREATE; a
name left out is the default) and `join:<game name>[:<password>]` (that game's row, or
WATCH on a live game; for a locked game the password window's field and its JOIN),
each done once the room (or the watched game) is open. `create!:...` and `join!:...`
expect a refusal and end on the answer shown, which they print (`room script:
join!:Friday:acorn: refused: CANNOT JOIN: wrong password`), leaving the window open.
`window:create[:<name>[:<password>]]` and `window:join:<game name>[:<password>]`
only open and fill in that window, for a picture. In the room: `ai:<slot>`, `map:<id>`
(CHANGE MAP and PICK; `map!:<id>` expects a refusal), `opt:<rule>=<value>`,
`scenario:<0|1>`, `watch`, `sit:<slot>`, `wait:humans=<n>` or `wait:ready=<n>`,
`ready`, `leave` and `start`. In either: `showpass:<0|1>` (the SHOW / HIDE beside the
room's password, or beside the password field of an open window; the room prints
`room: password shown` or `room: password hidden` when it changes, never the
password), `shot:<png>` and `browser:<ms>` (the game list, and nothing else, for that
long: after `leave` it fails if a room opens again). `start!` expects the server to
refuse the START and ends on the reason the room shows, which it prints.
`press:<x>:<y>[:<clicks>[:<ms>]]` is the one step that goes the pointer's way: the
left button pressed and released at that point of the lobby's 960×540, as SDL delivers
it, `<ms>` after the step came due; `<clicks>` is the press's click count (2 for the
second press of a double click). Each step
waits for its own result and ends the run with
the reason if it does not come within 10 s (30 s for `wait`, 20 s for `create!` and
`join!`). With `TAK_MENU_PICK=singleplayer` it drives the room the PLAY THE MACHINE
door opens; `tools/room_flow_test.py` uses both.

Game speed follows the original's −9..+10 levels (0.1× to 2×). For testing, a
debug client run with `TAK_FAST_SPEED=1` keeps stepping past +10 in 0.5× steps up
to 8×; only a debug `takserver` that is a test or private loopback server (such
as the single-player local server) accepts those speeds, and every other server
clamps them to 2×. `TAK_SPEED=<tenths>` (up to 80) sets the starting speed of a
headless harness game the same way.

The simulation's worker pool (`src/sim/simpool.h`) is as wide as the player's
**Simulation threads** option says (`simThreads` in the settings file: 0 = Auto,
one thread for each performance core, `SimPool::autoThreads`; else that many
threads, up to twice the CPU threads). A game
with its own local server (single player, a hosted Local server, the Benchmark,
`--play-map`) gives its simulation the larger half, rounded up, and starts the
server with the rest (`--sim-threads`), each at least 1; any other game gives its
one simulation all of them (`SimPool::sessionThreads`), never more than 64 per
simulation. A `takserver` shares its `--sim-threads
<auto|N>` total among its running games (`SimPool::serverShare`). Each process
logs what its simulation runs on when a game starts, for example
`sim threads: 4 (auto: 8, half; local server 4) pinned to cpus 0,2,4,6` in the
client and `sim threads: 4 (4 over 1 game) pinned to cpus 1,3,5,7` in its server.

The CPUs come from `sim::detectCpus()` (`src/sim/simcpus.h`): the performance
cores, efficiency cores and hyperthreads the process may use, and the order
threads take them in. With **Pin simulation threads** on (`simPin` in the settings
file, 0 by default: nothing is pinned) every simulation thread is pinned to a CPU
of its own (`SimPool::handOut`): the client's sim worker thread and its pool's
workers, and, through `takserver --sim-cpus <list>`, the local server's per-game
worker and its pool. A thread without a CPU is not pinned, and neither is a main
thread that ticks the simulation itself (replay playback, the headless harness).
macOS pins nothing. The setting alone decides, in every kind of session, the
headless debug harness (`--mpai` and the other `--mp...` modes) included: a
scratch profile has no `simPin`, so a harness run pins nothing unless its
settings file says `simPin = 1`. `TAK_SIM_PIN=0` stops a debug client pinning
whatever the setting says, and ctest sets it for every test
(`tak_isolate_test_profiles`), so that many clients run at once never all pin to
the same cores; any other value, like the variable unset, leaves it to the
setting (Pin to CCDs included), and no variable turns pinning on (a test of the pinning writes
`simPin = 1` and sets `TAK_SIM_PIN=1` over ctest's 0). The Benchmark results list
what the run latched: `SIM THREADS CLIENT <n>  SERVER <m>` with the setting, and
`PIN SIM THREADS ON` or `OFF`. Two more debug
hooks fake the detection: `TAK_CPU_SYSFS=<root>` reads a Linux sysfs fixture tree
in place of this machine, and `TAK_CPU_NOPIN=1` reports a system that cannot pin
(the macOS form of the Options row). `TAK_SIM_WORKERS=<n>` (debug
builds, 0 to 63 workers beside the ticking thread) overrides Auto, and only Auto:
a chosen count wins over it, in the client and in the server. Tests and tools
that set it leave the option at Auto. `SimPool::defaultWorkers()` (at most 3
workers) is only what a `World` gets when nothing sets its width, as in the tools
and tests. The game reads the CPU count once, as a session opens, and a server
once, as it starts: a CPU mask that changes during a game (`taskset -p`, a
container's cpuset) does not change a running game's width. No width changes a
result: `simpool`, `simpool_passes`,
`exploration_parallel`, `state_hash`, `aura_recipient` and `legion_parallel`
compare widths from 0 to 63 workers against the serial run.

**Pin to CCDs** (`ccdPin`, `ccdGame`, `ccdServer`; `client/ccdpin.h`) groups every
CPU the system has by the L3 it shares (`sim::detectCcds`: sysfs `cache/index*` on
Linux, `GetLogicalProcessorInformationEx` on Windows, nothing on macOS) and is
offered with two or more groups where threads can be pinned. When a session opens,
`planCcds` puts the whole client on its CCD (`sim::setProcessCpus`, every thread of
the process; back to `sim::launchCpus()` when it is off) and passes the server
`takserver --cpus <list>`; each simulation then gets `sim::ccdThreads` (half each
on a shared CCD) and nothing is pinned per thread; `TAK_SIM_PIN=0` keeps it off. `options_ccd` checks the Options
rows and the plan on sysfs fixtures, `simpool` the reader, and `cpu_topology` the
process pin on the machine it runs on.

A headless harness game ends with an `mp-headless done: tick=… hash=… end=… result=…`
line. `end=timelimit` stops at exactly the `--time` tick; `end=concluded` stops at
exactly the tick the result landed (`result=`, 0 when there was none), however far
behind the server the client was, so two runs of one game compare the same tick's hash.
The harness simulates inline and services its connection every 250 ms of wall time while
it does, so a slow stress game on a loaded machine keeps its seat
(`TAK_SIM_SLOW_US=<us>` plays such a client; `harness_seat_load` tests both).
`TAK_DEFEAT_TEST=1` destroys the player's Monarch and plays on through the results
transition; any other value only destroys it, and the run ends at its result.
A watching run (`--mpspectate`, or `--mphost` with `TAK_MP_WATCH`) also ends when the
server ends the watched game (`Msg::GameOver`): `end=gameover` when its own sim saw no
result. `TAK_WATCH_END_TEST=1` is the watch probe: the run plays on past the result
until the server ends the game, then through the same three-second results transition,
and prints the result plate a watcher gets (`watch_end` tests it). A `TAK_REPLAY_VERIFY`
playback that ends under the winner's banner prints `replay result: team N wins`.

## Server authentication

Sign-in uses SCRAM-SHA-256-style challenge/response over the game's binary
framing. The server stores salts and derived keys rather than passwords;
password derivation uses 600,000 PBKDF2-HMAC-SHA256 iterations. Authentication
does not encrypt game traffic, and first-time account registration has no
established server identity to authenticate against. See
[src/net/auth.h](../src/net/auth.h) for the protocol and its limits. Repeated
failures trigger account/address lockouts.

Accounts live in one plain-text file (`--accounts`, default
`takserver-accounts.conf`), written owner-read/write on POSIX and replaced atomically —
no database. New passwords must be at least 8 characters; names are 3-20
characters of letters, digits, `_`, `-` or `.`, unique case-insensitively.
`tools/authtest.cpp` checks the primitives against the published FIPS/RFC test
vectors and drives the exchange through replay, downgrade and stolen-file
attacks.

## Project layout

| Path | Contents |
| --- | --- |
| `src/hpi/` | HPI archive reader (TAK's revised format vs. classic TA) |
| `src/gaf/` | GAF/TAF sprite, animation, and font decoding |
| `src/video/` | `.bik` (Bink Video) decoding for the menu door clips (FFmpeg-backed) |
| `src/tnt/` | TNT map decoding |
| `src/tdo/` | 3DO model loading |
| `src/cob/` | COB script bytecode VM (unit animation/scripting) |
| `src/tdf/` | TDF/FBI/OTA text-config parsing |
| `src/crt/` | `.crt` scenario/trigger parsing |
| `src/campaign/` | campaign spine (`camps/*.tdf`) + in-sim mission/god-script runner |
| `src/sim/` | deterministic simulation (movement, pathfinding, combat, economy) |
| `src/net/` | multiplayer wire format, framed TCP, client protocol |
| `src/server/` | `takserver`, the headless lobby, lockstep relay, referee sim and server-run AI |
| `src/ai/` | the skirmish AI (server-portable; emits commands) |
| `src/terrain/` | terrain / palette handling |
| `src/util/` | shared helpers |
| `src/gui/` | retail `.gui` HUD/gadget layout parsing |
| `src/client/` | the SDL2 app (`takclient`: asset viewer + game) |
| `src/cartographer/` | `cartographer`, a clean-room port of the retail map editor |
| `tools/` | CLI dev tools (`hpitool`, `gaftool`, `tnttool`, `modeltool`, `cobtool`, `tdftool`, `missiontool`, `biktool`, `aitool`); a tool given too few arguments prints its usage on stderr and exits 2, `cartographer --version` / `--help` answer without opening the editor, and `simperf` (a benchmark, not a ctest target) generates its default map with the current random-map recipe and reports core ticks (AI + world tick: `median_ms`/`p95_ms`/`p99_ms`/`max_ms`) beside complete ticks that add the stateHash due every 30th tick (`complete_*`, `service_seconds`; the per-second diagnostic scan and output stay outside both); `tools/cli_tools_test.py` is the `cli_tools` test |
| `docs/` | format notes + reverse-engineering findings (`retail-engine.md` = the `KINGDOMS.icd` disassembly) |

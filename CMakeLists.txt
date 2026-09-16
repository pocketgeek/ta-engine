cmake_minimum_required(VERSION 3.24)
project(ta-engine VERSION 0.6.7 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to an optimised build -- the engine renders/sims thousands of units, so
# -O0 Debug tanks the frame rate. Configure with -DCMAKE_BUILD_TYPE=Debug for a
# debug build.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra)

# --- zlib / libjpeg / SDL2 are ALWAYS linked statically ------------------------
# There is no switch for this. A dynamic build was only ever useful for bisecting a
# system-library bug, and keeping the option around meant the shipped configuration was
# not the one anyone built locally -- the shape of bug that reaches users. The binaries
# carry zlib/libjpeg/SDL2 internally, at pinned versions. (SDL2 still dlopens its
# X11/Wayland/audio backends, so THOSE system libs are still needed at runtime; that is
# SDL's design and not something linking can change.)
set(TA_STATIC_DEPS_PREFIX "${CMAKE_SOURCE_DIR}/third_party/static-deps"
    CACHE PATH "Install prefix of the vendored static zlib/libjpeg/SDL2")
set(ZLIB_USE_STATIC_LIBS ON)                # FindZLIB honours this (CMake >= 3.24)
if(EXISTS "${TA_STATIC_DEPS_PREFIX}/lib" OR EXISTS "${TA_STATIC_DEPS_PREFIX}/lib64")
  # Vendored archives built by tools/build-static-deps.sh (Linux, macOS).
  list(PREPEND CMAKE_PREFIX_PATH "${TA_STATIC_DEPS_PREFIX}")
  set(ZLIB_ROOT "${TA_STATIC_DEPS_PREFIX}")
  set(JPEG_ROOT "${TA_STATIC_DEPS_PREFIX}")   # only the .a lives there -> found static
  message(STATUS "Static libs: vendored from ${TA_STATIC_DEPS_PREFIX}")
elseif(WIN32)
  # MSYS2/MinGW ships libz.a/libjpeg.a/libSDL2.a alongside the import libs. Prefer .a
  # over the .dll.a so find_package resolves the static ones.
  set(CMAKE_FIND_LIBRARY_SUFFIXES .a .dll.a ${CMAKE_FIND_LIBRARY_SUFFIXES})
  message(STATUS "Static libs: from the toolchain's static archives")
else()
  # Falling through here would silently find the system's SHARED SDL2 and produce a
  # dynamically linked binary that looks fine locally and fails the release gate -- or
  # worse, ships. Say what to run instead.
  message(FATAL_ERROR
    "The vendored static libraries are missing: ${TA_STATIC_DEPS_PREFIX}\n"
    "  Build them once with:  ./tools/build-static-deps.sh\n"
    "(zlib, libjpeg-turbo and SDL2 are always linked statically; there is no dynamic "
    "build to fall back to.)")
endif()

# --- Format libraries (no external dependencies) ------------------------------

find_package(ZLIB REQUIRED)
find_package(JPEG REQUIRED)

add_library(ta-formats
  src/hpi/hpi.cpp
  src/gaf/gaf.cpp
  src/tnt/tnt.cpp
  src/tnt/ota.cpp
  src/tnt/mapgen.cpp
  src/tdo/tdo.cpp
  src/tdf/tdf.cpp
  src/cob/cob.cpp
  src/cob/vm.cpp
  src/sim/sim.cpp
  src/sim/pathsearch.cpp
  src/sim/detmath.cpp
  src/sim/matchsetup.cpp
  src/sim/mission.cpp
  src/sim/scenario.cpp
  src/campaign/campaign.cpp
  src/crt/crt.cpp
  src/gui/gui.cpp
  src/net/protocol.cpp
  src/net/conn.cpp
  src/net/client.cpp
  src/net/crypto.cpp
  src/net/auth.cpp
  src/util/png.cpp
  src/util/appicon.cpp
  src/util/jpeg.cpp
  src/terrain/terrain.cpp
)
target_include_directories(ta-formats PUBLIC src)
target_link_libraries(ta-formats PUBLIC ZLIB::ZLIB JPEG::JPEG)

# Inject the project version (src/version.h) into every target that links
# ta-formats -- one source of truth, set by project(... VERSION ...) above.
target_compile_definitions(ta-formats PUBLIC TA_VERSION="${PROJECT_VERSION}")

# SOURCE fingerprint, separate from the release version. Two peers must run the same
# SIMULATION CODE to stay in lockstep, and nothing checked that: the gameplay-data hash
# covers data, kNetVersion covers the wire format, and neither moves when sim behaviour
# changes. A test server left one commit behind produced five "desyncs" that were
# nothing of the sort -- the placement code differed, so the worlds differed from tick
# zero. This is what tells those apart from a real divergence.
#
# The release version cannot serve here: it changes a handful of times a year, while
# the simulation changes daily.
#
# REGENERATED EVERY BUILD, never at configure time. Capturing it during cmake means a
# commit followed by an incremental build ships the PREVIOUS commit's id -- a stale
# fingerprint that would let the sweep's gate pass a mismatched pair, which is the
# whole failure it exists to prevent.
set(TA_BUILD_ID_H "${CMAKE_BINARY_DIR}/generated/ta_build_id.h")
add_custom_target(ta_build_id ALL
    BYPRODUCTS "${TA_BUILD_ID_H}"
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${TA_BUILD_ID_H}
            -P "${CMAKE_SOURCE_DIR}/cmake/BuildId.cmake"
    COMMENT "Stamping build id")
# Seed it now so the first configure leaves a header to compile against.
execute_process(COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${TA_BUILD_ID_H}
                        -P "${CMAKE_SOURCE_DIR}/cmake/BuildId.cmake")
target_include_directories(ta-formats PUBLIC "${CMAKE_BINARY_DIR}/generated")
add_dependencies(ta-formats ta_build_id)
target_compile_definitions(ta-formats PUBLIC TA_HAVE_BUILD_ID_H=1)

# Windows sockets: src/net (in ta-formats) uses Winsock2 -- link ws2_32 so it
# propagates to taclient and taserver. No-op on POSIX.
if(WIN32)
  # bcrypt: BCryptGenRandom, the OS CSPRNG behind crypto::randomBytes.
  target_link_libraries(ta-formats PUBLIC ws2_32 bcrypt)
  # Fold the GCC/C++ runtime into each .exe so a Windows box needs no MinGW DLLs.
  # -static also pulls winpthread. SDL2/jpeg/zlib are always static too, so
  # nothing is shipped alongside -- windows.yml gates on the .exe importing no
  # SDL2/jpeg/zlib/FFmpeg DLL at all. (This comment used to say those three were still
  # shipped as DLLs, which stopped being true when that gate went in.)
  add_link_options(-static-libgcc -static-libstdc++ -static)
endif()

# Linux: fold the C++ runtime in too. SDL2/jpeg/zlib and the Bink FFmpeg are always
# static, which left libstdc++/libgcc_s/libatomic
# as the only non-libc dependencies -- and they are the ones that tie a package to the
# distro that built it, which is why the release matrix carries a per-distro tag and a
# gcc-12 variant.
#
# NOT -static. glibc resolves hosts through NSS modules it dlopens (the netcode calls
# getaddrinfo), and SDL2 deliberately dlopens its X11/Wayland/audio backends; a fully
# static binary would either fail to resolve or fail to open a window. libc and libm stay
# shared on purpose. This is the most static a Linux build can usefully be.
# Each flag is TESTED before it is used, rather than assumed: -static-libstdc++ needs
# libstdc++.a, which several distros split into a separate package (Fedora's
# libstdc++-static, and it is absent by default). Adding the flag blind turns a missing
# package into "cannot find -lstdc++" at link time, so a developer without it could no
# longer build at all. Tested, it simply stays dynamic there and the CI gate -- which
# installs the package -- is what holds the release to the static result.
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  include(CheckCXXSourceCompiles)
  function(ta_try_link_flag flag var)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${flag})
    check_cxx_source_compiles("#include <string>\n#include <atomic>\nint main(){std::string s(\"x\");std::atomic<double> d{0};return int(s.size())+int(d.load());}" ${var})
    set(${var} "${${var}}" PARENT_SCOPE)
  endfunction()
  # A MISSING STATIC ARCHIVE IS A HARD FAILURE, not a quiet downgrade. Falling back to
  # the shared runtime produces a binary that looks fine, passes every test, and then
  # refuses to start on any machine whose libstdc++ is older than the build box -- the
  # exact failure this is meant to prevent, discovered by a user rather than by CI. The
  # probe exists to name the missing package, not to excuse it.
  ta_try_link_flag("-static-libgcc" TA_HAVE_STATIC_LIBGCC)
  ta_try_link_flag("-static-libstdc++" TA_HAVE_STATIC_LIBSTDCXX)
  if(NOT TA_HAVE_STATIC_LIBGCC OR NOT TA_HAVE_STATIC_LIBSTDCXX)
    message(FATAL_ERROR
      "The static C++ runtime is missing, so this build would depend on the host's "
      "libstdc++ version.\n"
      "  Fedora/RHEL:  sudo dnf install libstdc++-static\n"
      "  Debian/Ubuntu: sudo apt install libstdc++-*-dev  (ships libstdc++.a)\n"
      "  Arch:          already in the gcc package\n"
      "  Alpine:        apk add libstdc++-dev\n"
      "(-static-libgcc=${TA_HAVE_STATIC_LIBGCC} -static-libstdc++=${TA_HAVE_STATIC_LIBSTDCXX})")
  endif()
  add_link_options(-static-libgcc -static-libstdc++)
  # NOTE: libatomic is deliberately NOT handled here. add_link_options() places flags
  # BEFORE the object files, so a -Bstatic,-latomic there is processed while nothing has
  # referenced an atomic symbol yet and contributes nothing -- the plain -latomic that
  # FFmpeg's pkg-config adds AFTER the objects then binds the shared library anyway. It
  # is pinned where it is actually needed instead, inside the FFmpeg link group below.
endif()

# Determinism: the simulation (src/sim) must produce bit-identical results across
# machines running the same binary for lockstep multiplayer. Disable floating-
# point contraction (FMA fuses a*b+c differently on different CPUs) and never
# enable -ffast-math on this library. See docs/multiplayer-design.md. (A fuller
# cross-build contract -- a deterministic sin/cos/atan2 shim -- comes later; this
# covers same-binary play, which is the v1 requirement.)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ta-formats PRIVATE -ffp-contract=off)
endif()

# --- Command-line tools -------------------------------------------------------

add_executable(hpitool tools/hpitool.cpp)
target_link_libraries(hpitool PRIVATE ta-formats)

add_executable(detmath_test tools/detmath_test.cpp)
target_link_libraries(detmath_test PRIVATE ta-formats)

add_executable(sim_driver_equiv_test tools/sim_driver_equiv_test.cpp)
target_link_libraries(sim_driver_equiv_test PRIVATE ta-formats)

add_executable(makeicons tools/makeicons.cpp)
target_link_libraries(makeicons PRIVATE ta-formats)

add_executable(gaftool tools/gaftool.cpp)
target_link_libraries(gaftool PRIVATE ta-formats)

add_executable(tnttool tools/tnttool.cpp)
target_link_libraries(tnttool PRIVATE ta-formats)

add_executable(missiontool tools/missiontool.cpp)
target_link_libraries(missiontool PRIVATE ta-formats)

add_executable(authtest tools/authtest.cpp src/server/accounts.cpp)
target_link_libraries(authtest PRIVATE ta-formats)

# Drives a real MpClient against a deliberately hostile server.
add_executable(authrogue tools/authrogue.cpp)
target_link_libraries(authrogue PRIVATE ta-formats)

add_executable(retailgap_test tools/retailgap_test.cpp)
add_executable(navblock_test tools/navblock_test.cpp)
add_executable(pathsearch_test tools/pathsearch_test.cpp)
add_executable(pathgeom_test tools/pathgeom_test.cpp)
add_executable(ambush_test tools/ambush_test.cpp)
add_executable(yield_test tools/yield_test.cpp)
add_executable(crowdbench tools/crowdbench.cpp)
add_executable(cobanim_test tools/cobanim_test.cpp)
add_executable(cmdflow_test tools/cmdflow_test.cpp)
add_executable(conn_test tools/conn_test.cpp)
add_executable(featgen_test tools/featgen_test.cpp)
add_executable(transport_test tools/transport_test.cpp)
add_executable(production_test tools/production_test.cpp)
add_executable(replay_test tools/replay_test.cpp)
add_executable(pathblock_test tools/pathblock_test.cpp)
add_executable(navincr_test tools/navincr_test.cpp)
add_executable(statsfit_test tools/statsfit_test.cpp)
add_executable(settings_test tools/settings_test.cpp)
add_executable(hpi1_test tools/hpi1_test.cpp)
add_executable(tnt_test tools/tnt_test.cpp)
    add_executable(footprobe tools/footprobe.cpp)
target_link_libraries(retailgap_test PRIVATE ta-formats)
target_link_libraries(navblock_test PRIVATE ta-formats)
target_link_libraries(pathsearch_test PRIVATE ta-formats)
target_link_libraries(pathgeom_test PRIVATE ta-formats)
target_link_libraries(ambush_test PRIVATE ta-formats)
target_link_libraries(yield_test PRIVATE ta-formats)
target_link_libraries(crowdbench PRIVATE ta-formats)
target_link_libraries(cobanim_test PRIVATE ta-formats)
target_link_libraries(cmdflow_test PRIVATE ta-formats)
target_link_libraries(conn_test PRIVATE ta-formats)
target_link_libraries(featgen_test PRIVATE ta-formats)
target_link_libraries(transport_test PRIVATE ta-formats)
target_link_libraries(production_test PRIVATE ta-formats)
target_link_libraries(replay_test PRIVATE ta-formats)
target_link_libraries(pathblock_test PRIVATE ta-formats)
target_link_libraries(navincr_test PRIVATE ta-formats)
target_link_libraries(statsfit_test PRIVATE ta-formats)
target_link_libraries(settings_test PRIVATE ta-formats)
target_link_libraries(hpi1_test PRIVATE ta-formats)
target_link_libraries(tnt_test PRIVATE ta-formats)

# ctest registration. Without this `ctest` reports "No tests were found", which
# is how pathsearch_test and cobanim_test ended up protected by nothing but
# someone remembering to run them.
#
# The split matters: the first group is self-contained and runs anywhere
# (including CI), the second needs a real retail install, which is gitignored
# and absent from CI by design. Configure with -DTAK_TEST_DATA=/path/to/install
# to register those too.
enable_testing()
add_test(NAME detmath COMMAND detmath_test)
add_test(NAME sim_driver_equiv COMMAND sim_driver_equiv_test)
add_test(NAME pathsearch COMMAND pathsearch_test)
add_test(NAME auth COMMAND authtest)
add_test(NAME cmdflow COMMAND cmdflow_test)
add_test(NAME conn COMMAND conn_test)
add_test(NAME featgen COMMAND featgen_test)
add_test(NAME transport COMMAND transport_test)
add_test(NAME production COMMAND production_test)
add_test(NAME replay COMMAND replay_test)
add_test(NAME pathblock COMMAND pathblock_test)
add_test(NAME navincr COMMAND navincr_test)
add_test(NAME settings COMMAND settings_test)
add_test(NAME hpi1 COMMAND hpi1_test)
add_test(NAME tnt COMMAND tnt_test)
add_test(NAME pathgeom COMMAND pathgeom_test)
add_test(NAME ambush COMMAND ambush_test)
add_test(NAME yield COMMAND yield_test)
add_test(NAME statsfit COMMAND statsfit_test)
set(TA_TEST_DATA "" CACHE PATH "Retail install for the data-backed tests (optional)")
if(TA_TEST_DATA)
    add_test(NAME cobanim COMMAND cobanim_test "${CMAKE_SOURCE_DIR}/assets/extracted/all/scripts")
    add_test(NAME retailgap COMMAND retailgap_test "${TA_TEST_DATA}")
    add_test(NAME navblock COMMAND navblock_test "${TA_TEST_DATA}")
endif()
target_link_libraries(footprobe PRIVATE ta-formats)

# aitool bundles the AI (src/ai, otherwise server-only) so we can tune it headless.
add_executable(aitool tools/aitool.cpp src/ai/ai.cpp)
target_link_libraries(aitool PRIVATE ta-formats)

# --- Client (needs SDL2; skipped with a warning if unavailable) ---------------

option(TA_BUILD_CLIENT "Build the SDL2 client (asset viewer + game)" ON)

# FFmpeg decodes the retail Bink (.bik) menu videos -- libavcodec carries a clean-room
# Bink decoder (LGPL). It is ALWAYS the minimal Bink-only static build from
# tools/build-ffmpeg-bink.sh; there is no dynamic link and no no-video build.
#
# The system libavcodec is never consulted, which is deliberate twice over: stock
# Fedora's libavcodec-free omits the Bink decoder entirely (so "it built" would not mean
# "videos play"), and its .pc files were the last place a shared libatomic could sneak
# back into an otherwise static binary. TA_FFMPEG_PREFIX overrides where the static
# install lives.
set(TA_FFMPEG_PREFIX "" CACHE PATH "Install prefix of the static Bink-only FFmpeg")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  if(NOT TA_FFMPEG_PREFIX)
    set(TA_FFMPEG_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-bink")
  endif()
  if(EXISTS "${TA_FFMPEG_PREFIX}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "${TA_FFMPEG_PREFIX}/lib/pkgconfig")
    # Order high-level -> low-level so the static archives resolve left-to-right.
    pkg_check_modules(FFMPEG libavformat libavcodec libswscale libswresample libavutil)
  else()
    message(FATAL_ERROR
      "The static Bink-only FFmpeg is missing: ${TA_FFMPEG_PREFIX}\n"
      "  Build it once with:  ./tools/build-ffmpeg-bink.sh\n"
      "(FFmpeg is always linked statically; there is no dynamic build and no "
      "no-video build to fall back to.)")
  endif()
else()
  message(FATAL_ERROR "pkg-config is required to locate the static Bink-only FFmpeg")
endif()

# One INTERFACE target both taclient and biktool link.
if(FFMPEG_FOUND)
  add_library(ta_ffmpeg INTERFACE)
  target_compile_definitions(ta_ffmpeg INTERFACE TA_HAVE_FFMPEG)
  target_include_directories(ta_ffmpeg INTERFACE ${FFMPEG_STATIC_INCLUDE_DIRS})
  if(APPLE)
    # pkg-config lists macOS frameworks as the two tokens "-framework" "CoreVideo";
    # a plain link-libraries list would turn the bare "CoreVideo" into -lCoreVideo
    # (ld: library 'CoreVideo' not found). Re-pair each into a SHELL: link option so
    # both tokens reach the linker intact, and link the -L/-l rest normally. (Apple's
    # ld is multi-pass, so the circular archives need no --start-group.)
    set(_ff_rest "")
    list(LENGTH FFMPEG_STATIC_LDFLAGS _ff_n)
    set(_ff_i 0)
    while(_ff_i LESS _ff_n)
      list(GET FFMPEG_STATIC_LDFLAGS ${_ff_i} _ff_tok)
      if(_ff_tok STREQUAL "-framework")
        math(EXPR _ff_i "${_ff_i}+1")
        list(GET FFMPEG_STATIC_LDFLAGS ${_ff_i} _ff_fw)
        target_link_options(ta_ffmpeg INTERFACE "SHELL:-framework ${_ff_fw}")
      else()
        list(APPEND _ff_rest "${_ff_tok}")
      endif()
      math(EXPR _ff_i "${_ff_i}+1")
    endwhile()
    target_link_libraries(ta_ffmpeg INTERFACE ${_ff_rest})
  else()
    # --start/--end-group resolves the circular FFmpeg archives regardless of order
    # (GNU ld / MinGW).
    #
    # FFmpeg's .pc files list a bare -latomic, which resolves to libatomic.so and is
    # the last non-libc dependency left once libstdc++ is static. Swap it for the
    # archive HERE, where it sits after the objects that need it -- the one position
    # where asking for the static form actually takes effect.
    set(_ff_ldflags ${FFMPEG_STATIC_LDFLAGS})
    if(UNIX AND NOT APPLE)
      list(TRANSFORM _ff_ldflags REPLACE "^-latomic$"
           "-Wl,--push-state,-Bstatic,-latomic,--pop-state")
    endif()
    target_link_libraries(ta_ffmpeg INTERFACE
      -Wl,--start-group ${_ff_ldflags} -Wl,--end-group)
  endif()
  message(STATUS "FFmpeg: STATIC Bink-only build from ${TA_FFMPEG_PREFIX}")
endif()

if(TA_BUILD_CLIENT)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    add_executable(taclient src/client/main.cpp src/client/gameview_impl.cpp src/client/gameview_render.cpp src/client/gameview_hud.cpp src/client/gameview_net.cpp src/client/gameview_lobby.cpp src/client/gameview_benchmark.cpp src/client/gameview_input.cpp src/client/gameview_unitinfo.cpp src/client/font.cpp src/client/mapview.cpp src/client/modelview.cpp src/client/replayfile.cpp src/client/mainmenu.cpp src/client/settings.cpp src/client/options.cpp src/client/hotkeys.cpp src/client/hotkeysscreen.cpp src/client/campaignscreen.cpp src/client/briefingscreen.cpp src/client/resultscreen.cpp src/client/loadscreen.cpp src/client/guiart.cpp src/client/cursors.cpp src/util/procmetrics.cpp src/video/bink.cpp)
    if(WIN32)
      target_link_libraries(taclient PRIVATE psapi)   # GetProcessMemoryInfo (benchmark metrics)
    elseif(APPLE)
      # procmetrics reads the accelerator's IORegistry PerformanceStatistics for
      # the benchmark's device-wide GPU% (no public per-process stat on macOS).
      target_link_libraries(taclient PRIVATE "-framework IOKit" "-framework CoreFoundation")
    endif()
    # The vendored static SDL2 exposes SDL2::SDL2-static (with its
    # transitive -ldl/-lm/-lpthread etc. baked into the target); the system SDL2 exposes
    # SDL2::SDL2. We provide our own main (SDL_MAIN_HANDLED), so SDL2main is never needed.
    if(TARGET SDL2::SDL2-static)
      set(_ta_sdl2 SDL2::SDL2-static)
    else()
      set(_ta_sdl2 SDL2::SDL2)
    endif()
    target_link_libraries(taclient PRIVATE ta-formats ${_ta_sdl2})
    # We provide our own main() and call SDL_SetMainReady(). Define SDL_MAIN_HANDLED for
    # the WHOLE target (not just main.cpp) so it applies before ANY header pulls in
    # <SDL.h> -- otherwise, on Windows, SDL_main.h renames our main to SDL_main via a
    # transitive include and the MinGW link fails with "undefined reference to WinMain".
    target_compile_definitions(taclient PRIVATE SDL_MAIN_HANDLED)
    if(TARGET ta_ffmpeg)
      target_link_libraries(taclient PRIVATE ta_ffmpeg)
    else()
      message(STATUS "FFmpeg not found — menu .bik videos disabled (GAF door art fallback).")
    endif()
    if(WIN32)
      # SDL2::SDL2 drags in -mwindows (the GUI subsystem, whose CRT startup expects
      # WinMain). We deliberately keep our own console main() (SDL_MAIN_HANDLED, so no
      # SDL_main/WinMain), so force the console subsystem back on -- otherwise the
      # MinGW link fails with "undefined reference to WinMain".
      target_link_options(taclient PRIVATE -mconsole)
    endif()

    # Cartographer -- the clean-room map editor (docs/cartographer-port.md).
    # Shares the engine's terrain render stack (mapview + ta-formats) so maps
    # look byte-identical to the game.
    add_executable(cartographer src/cartographer/main.cpp src/cartographer/sections.cpp src/cartographer/newmap.cpp src/cartographer/features.cpp src/cartographer/units.cpp src/cartographer/triggers.cpp src/client/mapview.cpp)
    target_link_libraries(cartographer PRIVATE ta-formats ${_ta_sdl2})
    target_compile_definitions(cartographer PRIVATE SDL_MAIN_HANDLED)
    if(WIN32)
      target_link_options(cartographer PRIVATE -mconsole)
    endif()
  else()
    message(WARNING "SDL2 not found — skipping taclient. Install SDL2-devel.")
  endif()
endif()

# biktool: standalone Bink -> frame-dump debug tool (only when FFmpeg is present).
if(TARGET ta_ffmpeg)
  add_executable(biktool tools/biktool.cpp src/video/bink.cpp)
  target_link_libraries(biktool PRIVATE ta-formats ta_ffmpeg)
endif()

# --- Multiplayer server (headless; no SDL) ------------------------------------

# The skirmish AI (src/ai) is SERVER-ONLY -- it never links into taclient, so the
# client cannot run AI locally. AIs run only on the server.
add_executable(taserver src/server/server.cpp src/server/accounts.cpp src/ai/ai.cpp)
target_link_libraries(taserver PRIVATE ta-formats)

add_executable(modeltool tools/modeltool.cpp)
target_link_libraries(modeltool PRIVATE ta-formats)

add_executable(tdftool tools/tdftool.cpp)
target_link_libraries(tdftool PRIVATE ta-formats)

add_executable(cobtool tools/cobtool.cpp)
target_link_libraries(cobtool PRIVATE ta-formats)

# --- Packaging: native .deb + .rpm via CPack (Linux) --------------------------
# One source of truth for both package families. Build, then from the build dir:
#   cpack -G DEB   -> ta-engine-<version>-amd64.deb   (sudo apt install ./ta-engine-*-amd64.deb)
#   cpack -G RPM   -> ta-engine-<version>-x86_64.rpm  (sudo dnf install ./ta-engine-*-x86_64.rpm)
# Every build bakes SDL2/jpeg/zlib and the Bink FFmpeg in, so SHLIBDEPS/AUTOREQ below
# auto-detect only the C runtime.
# SDL2 still dlopens its X11/Wayland/audio backends, which every desktop that can run the
# game already has; those are not declared (they aren't link-time deps of a static SDL2).
if(UNIX AND NOT APPLE)
  install(TARGETS taserver RUNTIME DESTINATION bin)
  if(TARGET taclient)
    install(TARGETS taclient RUNTIME DESTINATION bin)
  endif()
  if(TARGET cartographer)
    install(TARGETS cartographer RUNTIME DESTINATION bin)
  endif()

  # Desktop integration: the procedurally-drawn app icons (res/icons, generated
  # by `makeicons`) + .desktop launchers so the apps show a proper icon.
  install(FILES res/icons/taclient.png    DESTINATION share/icons/hicolor/256x256/apps RENAME ta-client.png)
  install(FILES res/icons/cartographer.png DESTINATION share/icons/hicolor/256x256/apps RENAME ta-cartographer.png)
  install(FILES res/icons/taserver.png    DESTINATION share/icons/hicolor/256x256/apps RENAME ta-server.png)
  install(FILES res/linux/ta-client.desktop
                res/linux/ta-cartographer.desktop
                res/linux/ta-server.desktop
          DESTINATION share/applications)

  set(CPACK_PACKAGE_NAME "ta-engine")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Clean-room engine recreation of Total Annihilation: Kingdoms")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/pocketgeek/ta-engine")
  set(CPACK_PACKAGE_CONTACT "Curtis Edge <pocket_geek@pgnet.us>")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

  set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")   # SPDX identifier
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

  # Version-stamped asset names (the download filename records the release); the CI
  # globs these when attaching to the release, and the README links to the releases
  # page rather than a fixed latest/download URL. TA_DISTRO_TAG ("ubuntu22.04",
  # "fedora44", ...) is set by the per-distro CI matrix so each build's package
  # names the distro it was built on/for.
  set(TA_DISTRO_TAG "" CACHE STRING "distro tag baked into package file names (e.g. ubuntu22.04)")
  set(_ta_dtag "")
  if(TA_DISTRO_TAG)
    set(_ta_dtag "${TA_DISTRO_TAG}-")
  endif()
  set(CPACK_DEBIAN_FILE_NAME "ta-engine-${PROJECT_VERSION}-${_ta_dtag}amd64.deb")
  set(CPACK_RPM_FILE_NAME "ta-engine-${PROJECT_VERSION}-${_ta_dtag}x86_64.rpm")

  # Auto-detect the shared-library dependencies for each family.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_RPM_PACKAGE_AUTOREQ ON)
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")

  include(CPack)
elseif(WIN32)
  # Windows installer (NSIS via CPack; the CI's MSYS2 job runs `cpack -G NSIS`).
  # Binaries install into the chosen folder root (no bin/), with Start-menu
  # shortcuts and an uninstaller. The static build has no DLLs to carry.
  install(TARGETS taserver RUNTIME DESTINATION .)
  if(TARGET taclient)
    install(TARGETS taclient RUNTIME DESTINATION .)
  endif()

  set(CPACK_PACKAGE_NAME "ta-engine")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Clean-room engine recreation of Total Annihilation: Kingdoms")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/pocketgeek/ta-engine")
  set(CPACK_PACKAGE_CONTACT "Curtis Edge <pocket_geek@pgnet.us>")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  set(CPACK_PACKAGE_FILE_NAME "ta-engine-${PROJECT_VERSION}-windows-x64-setup")
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "TAK Engine")
  set(CPACK_NSIS_DISPLAY_NAME "TAK Engine")
  set(CPACK_NSIS_PACKAGE_NAME "TAK Engine")
  set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/pocketgeek/ta-engine")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")        # shortcuts point at the install root
  set(CPACK_PACKAGE_EXECUTABLES "taclient" "TAK Engine")
  set(CPACK_NSIS_MUI_FINISHPAGE_RUN "taclient.exe")

  include(CPack)
endif()

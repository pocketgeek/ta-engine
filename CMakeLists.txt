cmake_minimum_required(VERSION 3.24)
project(ta-engine VERSION 1.0.9 LANGUAGES CXX)

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
# Aggregates such as Order{...} name only the fields they set and leave the rest at their
# defaults on purpose; -Wextra's missing-field-initializers would flag every one.
add_compile_options(-Wno-missing-field-initializers)
# GCC 11 and 12 (Slackware 15.0, Debian 12, Ubuntu 22.04) report memcpy overlaps inside
# std::string and maybe-uninitialized values that later compilers no longer see.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
  add_compile_options(-Wno-restrict -Wno-maybe-uninitialized)
endif()

# Unoptimized builds (no -O1..-O3/-Os/-Oz/-Ofast in the configuration's C++ flags, as in a
# plain Debug build) and sanitizer builds run the simulation many times slower. A few long
# tests get a larger TIMEOUT there (TA_SLOW_TEST_BUILD); optimized builds keep the tighter
# limits, which still catch a test that slows down.
string(TOUPPER "${CMAKE_BUILD_TYPE}" _ta_build_type_upper)
set(_ta_test_flags " ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_ta_build_type_upper}} ")
if(_ta_test_flags MATCHES "-fsanitize" OR NOT _ta_test_flags MATCHES " -O([1-3sz]|fast) ")
  set(TA_SLOW_TEST_BUILD ON)
else()
  set(TA_SLOW_TEST_BUILD OFF)
endif()

# Build speed (developer machines only; neither tool changes the code generated).
# ccache: many worktrees build the same sources, so a shared compiler cache turns a
# fresh build into mostly cache hits. Worktrees only share entries when ccache's
# base_dir covers their paths (base_dir = / and hash_dir = false in ccache.conf).
# mold: the build links ~300 executables against large static libraries, which GNU
# ld spends much of an incremental build on. Both are used when found and can be
# turned off; CI and release builds without them are unaffected.
option(TA_CCACHE "Use ccache as the compiler launcher when it is installed" ON)
option(TA_MOLD "Link with mold on Linux when it is installed" ON)
if(TA_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(TA_CCACHE_PROGRAM ccache)
  if(TA_CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${TA_CCACHE_PROGRAM}")
  endif()
endif()
if(TA_MOLD AND UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  find_program(TA_MOLD_PROGRAM mold)
  if(TA_MOLD_PROGRAM)
    add_link_options(-fuse-ld=mold)
  endif()
endif()

# Determinism: no floating-point contraction ANYWHERE. A contracted a*b+c is one
# fused multiply-add (one rounding) instead of two, so the same source gives a
# different last bit wherever the compiler fuses. GCC on baseline x86-64 has no FMA
# instruction to fuse into, but Apple Clang on arm64 contracts by default
# (-ffp-contract=on), and a fused last bit is enough to split lockstep.
#
# This used to be set on ta-formats alone, which left everything compiled into the
# executables contracting on a Mac: the server's AI (src/ai/ai.cpp, part of
# taserver), whose `factory.z + sin(a)*r` door-clearing Move came out one ulp
# different on macOS arm64 and made every Mac-hosted Legion game diverge from the
# same game hosted on Linux/Windows from tick 3271 of the Inner Circle --mpai run;
# and every sim header inlined into client/server code (retailaim.h & co.), where
# the linker may keep the contracted copy of an inline function for the sim too.
# Hence a directory-wide option, not a per-target one: a new target or source file
# cannot opt out by forgetting. fp_contract_test proves it took effect.
# Never enable -ffast-math either. See docs/multiplayer-design.md.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-ffp-contract=off)
endif()

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
  # DROP ANY CACHED DISCOVERY THAT POINTS OUTSIDE THE PREFIX. find_package does not
  # re-search once its cache entries are set, so a build directory configured before
  # these libraries became mandatory-static keeps pointing at the system's .so -- and
  # then links it while this file cheerfully prints "vendored from ...". The result is
  # the worst kind: a build that reports the configuration it does not have. A fresh
  # directory never showed it, which is why it needed review to find.
  foreach(_v ZLIB_LIBRARY ZLIB_LIBRARY_RELEASE ZLIB_LIBRARY_DEBUG ZLIB_INCLUDE_DIR
             JPEG_LIBRARY JPEG_LIBRARY_RELEASE JPEG_LIBRARY_DEBUG JPEG_INCLUDE_DIR
             SDL2_DIR SDL2_LIBRARY SDL2_INCLUDE_DIR)
    if(DEFINED CACHE{${_v}})
      set(_cached "$CACHE{${_v}}")
      string(FIND "${_cached}" "${TA_STATIC_DEPS_PREFIX}" _inPrefix)
      if(NOT _inPrefix EQUAL 0)
        message(STATUS "Static libs: discarding stale ${_v} (${_cached})")
        unset(${_v} CACHE)
      endif()
    endif()
  endforeach()
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
find_package(Threads REQUIRED)

add_library(ta-crypto STATIC src/net/crypto.cpp)
target_include_directories(ta-crypto PUBLIC src)
if(WIN32)
  target_link_libraries(ta-crypto PUBLIC bcrypt)
endif()

add_library(ta-formats
  src/hpi/hpi.cpp
  src/gaf/gaf.cpp
  src/tnt/tnt.cpp
  src/tnt/ota.cpp
  src/tnt/mapgen.cpp
  src/tnt/mapgen_ta.cpp
  src/tnt/mapgen_balanced.cpp
  src/tnt/mapgen_kits.cpp
  src/tdo/tdo.cpp
  src/tdf/tdf.cpp
  src/cob/cob.cpp
  src/cob/vm.cpp
  src/sim/sim.cpp
  src/sim/simpool.cpp
  src/sim/simcpus.cpp
  src/sim/pathsearch.cpp
  src/sim/legion.cpp
  src/sim/convoy.cpp
  src/sim/detmath.cpp
  src/sim/matchsetup.cpp
  src/sim/sides.cpp
  src/sim/taweapons.cpp
  src/sim/taeconomy.cpp
  src/sim/mission.cpp
  src/sim/scenario.cpp
  src/sim/scenarioinfo.cpp
  src/campaign/campaign.cpp
  src/crt/crt.cpp
  src/gui/gui.cpp
  src/gui/tagui.cpp
  src/net/protocol.cpp
  src/net/matchconfig.cpp
  src/net/conn.cpp
  src/net/discovery.cpp
  src/net/client.cpp
  src/net/replaywriter.cpp
  src/net/mappackage.cpp
  src/net/modpackage.cpp
  src/net/auth.cpp
  src/util/png.cpp
  src/util/appicon.cpp
  src/util/jpeg.cpp
  src/terrain/terrain.cpp
  src/terrain/minimap.cpp
)
target_include_directories(ta-formats PUBLIC src)
target_link_libraries(ta-formats PUBLIC ZLIB::ZLIB JPEG::JPEG ta-crypto Threads::Threads)

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
set(TA_BUILD_ID_H "${CMAKE_BINARY_DIR}/generated/tak_build_id.h")
add_custom_target(tak_build_id ALL
    BYPRODUCTS "${TA_BUILD_ID_H}"
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${TA_BUILD_ID_H}
            -P "${CMAKE_SOURCE_DIR}/cmake/BuildId.cmake"
    COMMENT "Stamping build id")
# Seed it now so the first configure leaves a header to compile against.
execute_process(COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${TA_BUILD_ID_H}
                        -P "${CMAKE_SOURCE_DIR}/cmake/BuildId.cmake")
target_include_directories(ta-formats PUBLIC "${CMAKE_BINARY_DIR}/generated")
add_dependencies(ta-formats tak_build_id)
target_compile_definitions(ta-formats PUBLIC TA_HAVE_BUILD_ID_H=1)

# Windows sockets: src/net (in ta-formats) uses Winsock2 -- link ws2_32 so it
# propagates to taclient and taserver. No-op on POSIX.
if(WIN32)
  target_link_libraries(ta-formats PUBLIC ws2_32 shell32)
  # Fold the GCC/C++ runtime into each .exe so a Windows box needs no MinGW DLLs.
  # -static also pulls winpthread. SDL2/jpeg/zlib are always static too, so
  # nothing is shipped alongside -- windows.yml gates on the .exe importing no
  # SDL2/jpeg/zlib/FFmpeg DLL at all. (This comment used to say those three were still
  # shipped as DLLs, which stopped being true when that gate went in.)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(-static-libgcc -static-libstdc++)
  endif()
  # CLANGARM64 uses LLVM compiler-rt/libc++; -static folds those in too.
  add_link_options(-static)
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
# package into "cannot find -lstdc++" at link time. The probes fail configuration
# with an explanation of the missing package; they never select a shared fallback.
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  include(CheckCXXSourceCompiles)
  function(tak_try_link_flag flag var)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${flag})
    check_cxx_source_compiles("#include <string>\n#include <atomic>\nint main(){std::string s(\"x\");std::atomic<double> d{0};return int(s.size())+int(d.load());}" ${var})
    set(${var} "${${var}}" PARENT_SCOPE)
  endfunction()
  # A MISSING STATIC ARCHIVE IS A HARD FAILURE, not a quiet downgrade. Falling back to
  # the shared runtime produces a binary that looks fine, passes every test, and then
  # refuses to start on any machine whose libstdc++ is older than the build box -- the
  # exact failure this is meant to prevent, discovered by a user rather than by CI. The
  # probe exists to name the missing package, not to excuse it.
  tak_try_link_flag("-static-libgcc" TA_HAVE_STATIC_LIBGCC)
  tak_try_link_flag("-static-libstdc++" TA_HAVE_STATIC_LIBSTDCXX)
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

# --- Command-line tools -------------------------------------------------------

add_executable(hpitool tools/hpitool.cpp)
target_link_libraries(hpitool PRIVATE ta-formats)


add_executable(detmath_test tools/detmath_test.cpp)
target_link_libraries(detmath_test PRIVATE ta-formats)

add_executable(fp_contract_test tools/fp_contract_test.cpp)

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
add_executable(conjure_test tools/conjure_test.cpp)
add_executable(death_lifecycle_test tools/death_lifecycle_test.cpp)
target_link_libraries(death_lifecycle_test PRIVATE ta-formats)
add_executable(building_death_test tools/building_death_test.cpp)
target_link_libraries(building_death_test PRIVATE ta-formats)
add_executable(clear_build_test tools/clear_build_test.cpp)
target_link_libraries(clear_build_test PRIVATE ta-formats)
add_executable(fire_sweep_test tools/fire_sweep_test.cpp)
target_link_libraries(fire_sweep_test PRIVATE ta-formats)
add_test(NAME fire_sweep_vision COMMAND fire_sweep_test vision)
add_test(NAME fire_sweep_switch COMMAND fire_sweep_test switch)
add_test(NAME fire_sweep_retaliation COMMAND fire_sweep_test retaliation)
add_test(NAME fire_sweep_turnrate COMMAND fire_sweep_test turnrate)
add_test(NAME fire_sweep_sites COMMAND fire_sweep_test sites)
add_executable(placement_ids_test tools/placement_ids_test.cpp)
target_link_libraries(placement_ids_test PRIVATE ta-formats)
add_test(NAME placement_ids COMMAND placement_ids_test)
add_executable(door_animation_test tools/door_animation_test.cpp)
target_include_directories(door_animation_test PRIVATE src)
add_executable(turn_test tools/turn_test.cpp)
add_executable(placement_test tools/placement_test.cpp)
add_executable(cursor_enemy_test tools/cursor_enemy_test.cpp)
add_executable(friendly_order_test tools/friendly_order_test.cpp)
target_include_directories(friendly_order_test PRIVATE src)
add_test(NAME friendly_order COMMAND friendly_order_test)
add_executable(navblock_test tools/navblock_test.cpp)
add_executable(setup_reachability_test tools/setup_reachability_test.cpp)
add_executable(blocked_shot_test tools/blocked_shot_test.cpp)
add_executable(minrange_backoff_test tools/minrange_backoff_test.cpp)
add_executable(ballistic_line_test tools/ballistic_line_test.cpp)
add_executable(shot_range_test tools/shot_range_test.cpp)
add_executable(attack_stop_test tools/attack_stop_test.cpp)
add_executable(suppress_test tools/suppress_test.cpp)
add_executable(vermage_fight_test tools/vermage_fight_test.cpp)
add_executable(target_search_test tools/target_search_test.cpp)
add_executable(sweetspot_test tools/sweetspot_test.cpp)
add_executable(reclaimarea_test tools/reclaimarea_test.cpp)
add_executable(revive_order_test tools/revive_order_test.cpp)
add_executable(campaign_test tools/campaign_test.cpp)
add_executable(pathfinding_matchsetup_test tools/pathfinding_matchsetup_test.cpp)
target_link_libraries(pathfinding_matchsetup_test PRIVATE ta-formats)
add_test(NAME pathfinding_matchsetup COMMAND pathfinding_matchsetup_test)
add_executable(start_positions_test tools/start_positions_test.cpp)
target_link_libraries(start_positions_test PRIVATE ta-formats)
add_test(NAME start_positions COMMAND start_positions_test)

add_executable(pathsearch_test tools/pathsearch_test.cpp)
add_executable(pathgeom_test tools/pathgeom_test.cpp)
add_executable(ambush_test tools/ambush_test.cpp)
add_executable(mission_orders_test tools/mission_orders_test.cpp)
add_executable(cadence_test tools/cadence_test.cpp)
add_executable(crowdbench tools/crowdbench.cpp)
add_executable(navigation_telemetry_test tools/navigation_telemetry_test.cpp)
target_link_libraries(navigation_telemetry_test PRIVATE ta-formats)
add_test(NAME navigation_telemetry COMMAND navigation_telemetry_test)
add_executable(world_clone_test tools/world_clone_test.cpp src/client/alloccount.cpp)   # its copies are measured as the viewer measures a keyframe
target_link_libraries(world_clone_test PRIVATE ta-formats)
# Extracted retail files (scripts/, objects3d/) for the tests that read loose files.
# They sit beside the install TA_TEST_DATA names (assets/game -> assets/extracted/all),
# so a worktree without its own gitignored assets/ still finds them; else the source tree's.
if(TA_TEST_DATA AND NOT TA_TEST_EXTRACTED)
  get_filename_component(TA_TEST_EXTRACTED "${TA_TEST_DATA}/../extracted/all" ABSOLUTE)
  if(NOT EXISTS "${TA_TEST_EXTRACTED}/scripts")
    set(TA_TEST_EXTRACTED "${CMAKE_SOURCE_DIR}/assets/extracted/all")
  endif()
endif()
if(TA_TEST_DATA)
  add_test(NAME world_clone COMMAND world_clone_test --data "${TA_TEST_DATA}")
else()
  add_test(NAME world_clone COMMAND world_clone_test)
endif()
# Perf follow-up H: static-sync timing over a base-heavy world (not a test; prints BENCH lines).
add_executable(legion_world_test tools/legion_world_test.cpp)
target_link_libraries(legion_world_test PRIVATE ta-formats)
foreach(legion_case clearance groupreuse jagged trapped longroute crowdhold replace unreachable quota pens determinism formation
    slotblock deaths deathsshared splitgoal farclick churn approachhold approachopen churnfield approachchurn legacyyield planeincremental planeprebuild penstale lattice wallend staticblock facing
    navalclearance navalisland hovershore navalmissions squadformation formationreform formationreformai formationslotreset pinwheel landedflyers mixedformation flyerfront liftflyers aware b3events lanegeom
    lonetrees slideback stillturn forestedge mixedsettle entryplanes mixedrandom funnelmouth funneltie staticindex destinations
    unfinishedsite)
  add_test(NAME legion_${legion_case} COMMAND legion_world_test ${legion_case})
  # TA_LEGION_VERIFY (debug builds; compiled out of Release): every derived index, work list
  # and the incrementally kept soft state (W4 B2's softHash, C31) against a recompute, each tick.
  # Costs these 39 cases ~17 s -> ~44 s in total at -O2 (the longest, squadformation, 6 -> 11 s).
  set_tests_properties(legion_${legion_case} PROPERTIES LABELS legion ENVIRONMENT TA_LEGION_VERIFY=1)
endforeach()
# planeincremental also runs its churn with workers (three in debug builds) and requires serial == workers.
set_tests_properties(legion_planeincremental PROPERTIES ENVIRONMENT "TA_LEGION_VERIFY=1;TA_SIM_WORKERS=3")
# W6 step 0 (PLAN 4 W6): the flyer instruments of tools/legion_flyers_test.cpp. Each prints the numbers the W6
# steps move and asserts determinism (serial == workers); W6_REQUIRE=1 asserts the PLAN targets (the acceptance
# check of the step that meets them).
add_executable(legion_flyers_test tools/legion_flyers_test.cpp)
target_link_libraries(legion_flyers_test PRIVATE ta-formats)
foreach(legion_probe mixedsquad mixedseal mv08group mv08deadend deadendrejoin ctrlmixed ctrlmixedbig factoryjoin auditlift
    fl04 fl05match fl05unreach predicates ctrlfree formationedge)
  add_test(NAME legion_flyers_${legion_probe} COMMAND legion_flyers_test ${legion_probe})
  set_tests_properties(legion_flyers_${legion_probe} PROPERTIES LABELS "legion;w6")
endforeach()
# The lift and touchdown cases also run with TA_LEGION_VERIFY (debug builds): the lift buckets and homes against a scan each tick.
foreach(legion_probe landunder descentwalkin auditlift40 splitpatrol liftcombat)
  add_test(NAME legion_flyers_${legion_probe} COMMAND legion_flyers_test ${legion_probe})
  set_tests_properties(legion_flyers_${legion_probe} PROPERTIES LABELS "legion;w6" ENVIRONMENT TA_LEGION_VERIFY=1)
endforeach()
# W7 step 0 fixtures (PLAN 4 W7 step 0): the promoted RB-02 probe (ring clot,
# Holding-queue corridor, plug) and the awareness stream fixtures awarebig (768x768), awaredense (200 v 200 at 85% fill),
# crossthree and crosslong. Measurement only: each prints the numbers W7's steps move (done tick, standing share,
# contacts, hold_max, the aware / parting / give-way counters, the route-behind metric) in both awareness arms and asserts
# determinism (serial == workers) on the cheap rows; the bounds come with W7's steps.
foreach(legion_probe rb02 awarebig awaredense crossthree crosslong)
  add_test(NAME legion_w7_${legion_probe} COMMAND legion_world_test ${legion_probe})
  set_tests_properties(legion_w7_${legion_probe} PROPERTIES LABELS "legion;w7")
endforeach()
# T1 probes for W3 (measurement only; they assert determinism, not bounds).
foreach(legion_probe settlelatency doorplug)
  add_test(NAME legion_${legion_probe} COMMAND legion_world_test ${legion_probe})
  set_tests_properties(legion_${legion_probe} PROPERTIES LABELS legion)
endforeach()
# W2 step 0 probes (AR-03 structsquad / factorysquad, RB-04 patrolrepair): measurement only; they
# print the values the W2 steps move and assert determinism, not the targets.
foreach(legion_probe structsquad factorysquad patrolrepair)
  add_test(NAME legion_${legion_probe} COMMAND legion_world_test ${legion_probe})
  set_tests_properties(legion_${legion_probe} PROPERTIES LABELS "legion;w2")
endforeach()
# W4: staticidle -- parked groups beside continuous construction do no refresh work (B1, demand-driven
# refresh); it failed by construction on the step-0 head.
add_test(NAME legion_staticidle COMMAND legion_world_test staticidle)
set_tests_properties(legion_staticidle PROPERTIES LABELS "legion;w4")
# W4 C1: a paused first build resumes for a member's demand within 10 ticks.
add_test(NAME legion_pausedemand COMMAND legion_world_test pausedemand)
set_tests_properties(legion_pausedemand PROPERTIES LABELS "legion;w4")
# Legion 10,000 stage 4: more mobility classes than kMaxPlanes start together; no plane is evicted before
# the registrations waiting for it use it (it failed on the pre-review head: 36 builds for 25 classes).
add_test(NAME legion_planepin COMMAND legion_world_test planepin)
set_tests_properties(legion_planepin PROPERTIES LABELS "legion")
# Legion 10,000 stage 6: tile chains (transit tiles shared across destinations, the descent check; with a wall
# dropped and lifted while they build) and a reused plane slot never reading the previous class's tiles; all
# under TA_LEGION_VERIFY.
foreach(legion_case chains chainschurn chainplane)
  add_test(NAME legion_${legion_case} COMMAND legion_world_test ${legion_case})
  set_tests_properties(legion_${legion_case} PROPERTIES LABELS "legion" ENVIRONMENT TA_LEGION_VERIFY=1)
endforeach()
# Legion 10,000 stage 7 (static navigation apart from traffic): an improvement defers its route's refresh while a
# closure refreshes at once (improvedefer); a chain relabelled while it builds re-resolves its regions instead of
# leaving them off (chainresolve); the coarse search routes round a region full of idle bodies (trafficlane); and
# (trapped) a trapped order retires under never-ending churn elsewhere, all under TA_LEGION_VERIFY.
foreach(legion_case improvedefer chainresolve trafficlane)
  add_test(NAME legion_${legion_case} COMMAND legion_world_test ${legion_case})
  set_tests_properties(legion_${legion_case} PROPERTIES LABELS "legion" ENVIRONMENT TA_LEGION_VERIFY=1)
endforeach()
# Route requests on large maps (2026-11-15; docs/legion-pathfinding.md sections 2 and 9): the situations that asked
# for a route over the whole component where a small one does, each on a small map with the field allowance shrunk
# so that the component is large. fullplan: a group whose bounded window failed plans a chain. demandown: a second
# command to a point another still walks to takes a chain. seedcover: a covered goal is searched through its legal
# neighbours, and a buried one's window is charged for its seed (seedrestore: and for its window once a step opens).
# cutoff: a body cut off from its goal after its order is trapped by its component, walks on first, retires after
# the grace period, and goes on in place when the way reopens. splitseeds: a body beside one of its group's goals is
# not cut off. routeclone: the new route state is copied and hashed (clones run on hash for hash). All under
# TA_LEGION_VERIFY; all but splitseeds and routeclone fail on the commit before.
foreach(legion_case fullplan demandown seedcover seedrestore cutoff splitseeds routeclone)
  add_test(NAME legion_${legion_case} COMMAND legion_world_test ${legion_case})
  set_tests_properties(legion_${legion_case} PROPERTIES LABELS "legion" ENVIRONMENT TA_LEGION_VERIFY=1)
endforeach()
# Perf follow-up I: the route invalidation index (staticChanged through sim/legion_routebins.h) against the walk of every
# group, under churn, structures, re-orders and deaths, on two planes; index on == off == workers, under TA_LEGION_VERIFY.
add_test(NAME legion_routeindex COMMAND legion_world_test routeindex)
set_tests_properties(legion_routeindex PROPERTIES LABELS "legion" ENVIRONMENT TA_LEGION_VERIFY=1)
foreach(legion_probe pocket deadend tail)
  add_test(NAME legion_${legion_probe} COMMAND legion_world_test ${legion_probe})
  set_tests_properties(legion_${legion_probe} PROPERTIES LABELS "legion;nightly" TIMEOUT 1200)
endforeach()
# Legion's headway window (World::updateLegionStill, LegionHeadway) on made-up motion: the 0.8.1
# walk-in-place stands, genuine slow motion never does.
add_executable(legion_still_test tools/legion_still_test.cpp)
target_link_libraries(legion_still_test PRIVATE ta-formats)
add_test(NAME legion_still COMMAND legion_still_test)
set_tests_properties(legion_still PROPERTIES LABELS legion)
# The const scenario observer (tools/legion_observe.h) on hand-placed bodies.
add_executable(legion_observe_selftest tools/legion_observe_selftest.cpp)
target_link_libraries(legion_observe_selftest PRIVATE ta-formats)
add_test(NAME legion_observe_selftest COMMAND legion_observe_selftest)
set_tests_properties(legion_observe_selftest PROPERTIES LABELS legion)
# .scn scenarios and issueSelection (the client's 64-per-tick order split).
add_executable(navbudget_test tools/navbudget_test.cpp)
target_include_directories(navbudget_test PRIVATE src)
add_test(NAME navbudget_test COMMAND navbudget_test)
set_tests_properties(navbudget_test PROPERTIES LABELS legion)
# Legion 10,000 stage 6: the routing hierarchy (src/sim/legion_hier.h), header-only.
add_executable(legion_hier_test tools/legion_hier_test.cpp)
target_include_directories(legion_hier_test PRIVATE src)
add_test(NAME legion_hier_test COMMAND legion_hier_test)
set_tests_properties(legion_hier_test PROPERTIES LABELS legion)
# refreshPlane's dirty-rectangle clusters (src/sim/legion_cluster.h), header-only: pairwise and binned paths
# against the pairwise reference.
add_executable(legion_cluster_test tools/legion_cluster_test.cpp)
target_include_directories(legion_cluster_test PRIVATE src)
add_test(NAME legion_cluster_test COMMAND legion_cluster_test)
set_tests_properties(legion_cluster_test PROPERTIES LABELS legion)
# Legion 10,000 stage 5: the route-query interface (src/sim/legion_route.h), header-only.
add_executable(legion_route_test tools/legion_route_test.cpp)
target_include_directories(legion_route_test PRIVATE src)
add_test(NAME legion_route_test COMMAND legion_route_test)
set_tests_properties(legion_route_test PROPERTIES LABELS legion)
# Perf follow-up I: the route invalidation index (src/sim/legion_routebins.h), header-only: every route a random
# change box reaches is a candidate, through random listings, unlistings, query drops and compactions.
add_executable(legion_routebins_test tools/legion_routebins_test.cpp)
target_include_directories(legion_routebins_test PRIVATE src)
add_test(NAME legion_routebins_test COMMAND legion_routebins_test)
set_tests_properties(legion_routebins_test PROPERTIES LABELS legion)
add_executable(convoy_test tools/convoy_test.cpp)
target_link_libraries(convoy_test PRIVATE ta-formats)
add_test(NAME convoy_test COMMAND convoy_test)
set_tests_properties(convoy_test PROPERTIES LABELS legion)
# IdMap (sim/idmap.h): the flat groups/members/anchors maps visit as std::map.
add_executable(legion_idmap_test tools/legion_idmap_test.cpp)
target_include_directories(legion_idmap_test PRIVATE src)
add_test(NAME legion_idmap_test COMMAND legion_idmap_test)
set_tests_properties(legion_idmap_test PROPERTIES LABELS legion)
add_executable(selportraits_test tools/selportraits_test.cpp)   # header-only client logic, no SDL
target_include_directories(selportraits_test PRIVATE src)
add_test(NAME selportraits COMMAND selportraits_test)
add_executable(battle_music_test tools/battle_music_test.cpp)   # header-only client logic, no SDL
target_include_directories(battle_music_test PRIVATE src)
add_test(NAME battle_music COMMAND battle_music_test)
add_executable(menuclick_test tools/menuclick_test.cpp)   # header-only client logic, no SDL
target_include_directories(menuclick_test PRIVATE src)
add_test(NAME menuclick COMMAND menuclick_test)
add_executable(replaybar_test tools/replaybar_test.cpp src/client/alloccount.cpp)
target_include_directories(replaybar_test PRIVATE src)
add_test(NAME replaybar COMMAND replaybar_test)

add_executable(issue_selection_test tools/issue_selection_test.cpp)
target_link_libraries(issue_selection_test PRIVATE ta-formats)
add_test(NAME issue_selection_test COMMAND issue_selection_test)
set_tests_properties(issue_selection_test PROPERTIES LABELS legion)
# The scenario runner: a .scn file in both modes, serial and workers, three
# start offsets, observer keys as JSON (tools/legion_scenario.cpp). Exit 77 =
# the file needs a retail install (--data).
add_executable(legion_scenario tools/legion_scenario.cpp)
target_link_libraries(legion_scenario PRIVATE ta-formats)
# scn_truth: the situation fidelity check alone (no observer), small enough to build in the old trees that
# recorded the user's replays (see tools/scn_truth.cpp).
# The Legion 10,000 scale probe (tools/legion_scale.h, `probe scale`): it changes no hash with the observer on
# or off, serial or workers (legion_scale_neutral), and the smallest scale file still delivers every body
# (legion_scale_one100: all 100 arrive, none stuck). The suite itself (tools/scenarios/scale/, 100 to 10,000
# bodies) is run by tools/legion_scale.py at stage ends, not by ctest.
if(TA_SLOW_TEST_BUILD)
  # Unoptimized, run alone (2026-10-18): 609 s Clang Debug, 486 s GCC -O0, 365 s ASan.
  endif()
# Legion 10,000 stage 2 (exact optimisations): the brute-force references under TA_LEGION_VERIFY (debug builds;
# compiled out of Release, where these only run the file). Each file is one a deliberate mutation trips:
# battle-assault the join index (registerMove picks the first joinable group of all), crosslong awareScan's
# broad phase (a dropped mover with a conflict), gap8 gateCommit's static-decline memo.
foreach(legion_scn battle-assault crosslong gap8)
  add_test(NAME legion_verify_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode legion --offsets 0 --workers)
  set_tests_properties(legion_verify_${legion_scn} PROPERTIES LABELS legion TIMEOUT 600
                       ENVIRONMENT "TA_LEGION_VERIFY=1;TA_SIM_WORKERS=3")
endforeach()
# packedBound's complete-route cache after an opening at a seed covered when the route was built (a cache dropped
# on closures alone aborts at tick 1069).
# Asset-free scenario set, part A (tools/scenarios/*.scn): both modes, serial and
# workers (the runner fails on a state or key mismatch), three start offsets.
# No bounds yet: a later task adds `--check` against baseline.json. The long
# ones (more than ~10 s of the optimized Debug build, one core) are nightly.
foreach(legion_scn corner-1x48 uturn gap6 sbend opentangent wall-1x48
                   doorform bridgeform mazeform)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS legion)
endforeach()
foreach(legion_scn corner-4x50 corner-8x56 corner-1x448 split-450-open split-450-pocket wall-4x50
                   gapsweep slowturn)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;nightly" TIMEOUT 1200)
endforeach()
# Asset-free scenario set, part B: awareness, flyers in streams and mixed
# formations, boats (island, strait), refresh churn. Same run as part A: both
# modes, serial and workers (a mismatch fails), three start offsets; no bounds
# yet. mixed2 and the strait fleets of 24+ big boats are KNOWN-FAILING behaviour
# (a report, not an assert): each file's header says which.
foreach(legion_scn aware-cross aware-headon aware-seen aware-unseen aware-attack
                   aware-adjacent liftstream island-1x48 strait mixed2 strait-4x24 strait-3x48 strait-2x48
                   refreshchurn refreshchurn-control)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS legion)
endforeach()
# Bodies that stand for good (2026-11-14; the bounds are legion_settledcrowd, legion_passage_starve, legion_lateswap,
# legion_escapestep, legion_idleplug, legion_siegegap, legion_siegegap_move, legion_filetrade, legion_plugleaves,
# legion_knotthree, legion_guardplug, legion_churnknot, legion_routeopens, legion_reorder and legion_pocketfar
# below): the same run as part A, so serial == workers and both modes on the rules' fixtures. headon-attack has no
# bounds: it is a known limit (its header says which), kept as a fixture so that the limit has a measure.
foreach(legion_scn settled-crowd passage-starve headon-fight escape-step idle-plug siege-gap siege-gap-move file-trade
                   plug-leaves knot-three guard-plug headon-attack churn-knot reorder pocket-far)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS legion)
endforeach()
# W7 step 0 scenario fixtures: the route-behind metric (`behind`) and the aware.* counters (`probe aware`).
foreach(legion_scn aware-cross-behind crosslong)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;w7")
endforeach()
# W8 step 0 fixtures (PLAN 4 W8 step 0): the flowing-route guard doubleturn, the
# audit's gap sweep one gap per world (gap 6 is gap6), the MV-12 mixed-speed spacing probe at 0.7x / 0.5x / 0.4x, and the
# 8x30 multi-group corner. Same run as part A (both modes, serial and workers); the lane probes (`probe lanes`,
# lane / hug shapes in sbend) report with them. No bounds yet: S1's gates come with W8's steps.
foreach(legion_scn doubleturn gap8 gap10 gap14 gap20 mixed-spacing-07x mixed-spacing-05x mixed-spacing-04x)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;w8")
endforeach()
foreach(legion_scn densehead island-1x150 strait-3x80 strait-2x150)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;nightly" TIMEOUT 1200)
endforeach()
# W2 step 0 fixtures: AR-03 structure/Keep in an Alt+N squad, AR-11 unreachable orders and the
# ~8200-tick maze approach walk (`probe approach` keys). Run in both modes; the second form
# also checks every key against baseline.json over the gate's eleven offsets (tools/legion_check.py:
# small-count keys on all eleven, the rest on the core five), so a later W2 step that makes one worse
# fails here and one that improves it prints RATCHET (nightly: past the per-commit CI budget).
foreach(legion_scn structsquad structsquad-keep unreach-200 mazeapproach)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;w2" TIMEOUT 600)
  if(NOT WIN32)
    add_test(NAME legion_check_${legion_scn}
             COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both
                     --check ${CMAKE_SOURCE_DIR}/tools/scenarios/baseline.json)
    set_tests_properties(legion_check_${legion_scn} PROPERTIES LABELS "legion;w2;nightly" TIMEOUT 900)
  endif()
endforeach()
# W3 step 0 fixtures (PLAN 4 W3 step 0): the pocket, deadend, tail and doorplug probes of legion_world_test as .scn files,
# so the baseline gate (tools/legion_check.py) covers them over the eleven gate offsets and both modes. They report the
# W3 targets rather than meet them (pocket t90, deadend settled == n, tail open_still900, doorplug B crossings), so
# the baseline holds today's values and a W3 step that makes one worse fails here. Nightly: 2-60 s each per offset.
foreach(legion_scn
    deadend-w2-n120-closed-plain deadend-w2-n120-closed-squad deadend-w2-n120-room-plain
    deadend-w2-n120-room-squad deadend-w2-n40-closed-plain deadend-w2-n40-closed-squad
    deadend-w2-n40-room-plain deadend-w2-n40-room-squad deadend-w4-n120-closed-plain
    deadend-w4-n120-closed-squad deadend-w4-n120-room-plain deadend-w4-n120-room-squad
    deadend-w4-n40-closed-plain deadend-w4-n40-closed-squad deadend-w4-n40-room-plain
    deadend-w4-n40-room-squad deadend-w6-n120-closed-plain deadend-w6-n120-closed-squad
    deadend-w6-n120-room-plain deadend-w6-n120-room-squad deadend-w6-n40-closed-plain
    deadend-w6-n40-closed-squad deadend-w6-n40-room-plain deadend-w6-n40-room-squad doorplug-112 doorplug-124
    pocket-304-open pocket-304-wall pocket-615-open pocket-615-wall tail-corner380 tail-open304 tail-open570
    tail-wave)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;w3;nightly" TIMEOUT 1800)
  # TA's economy and data rules moved these seven off the TAK-era baseline
  # (baseline.json); their scenario runs above still pass. Re-baseline before
  # re-enabling them.
  if(NOT WIN32 AND NOT legion_scn MATCHES "^(pocket-(304|615)-(open|wall)|tail-(corner380|open570|wave))$")
    add_test(NAME legion_check_${legion_scn}
             COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both
                     --check ${CMAKE_SOURCE_DIR}/tools/scenarios/baseline.json)
    set_tests_properties(legion_check_${legion_scn} PROPERTIES LABELS "legion;w3;nightly" TIMEOUT 1800)
  endif()
endforeach()
# W5 step 0 fixtures (PLAN 4 W5 step 0; tools/scenarios/gen_w5_fixtures.py writes them): the AR-06 attackring rows
# (`reach` shape: attackers that ever get a shot, damage, the longest Holding run out of reach), fight-retarget and
# ringcross (`probe claims`: the arrival-slot claims invariant), the MV-05 lateblock modes, the AR-07 chase / goalblock
# re-seed fixtures, MV-18 keelturn and MV-07 staticblock (`creep`: the goal-area creep counter). Each reports the open
# problem it measures, so the baseline holds today's values and a W5 step that makes one worse fails
# legion_check_*; the gate (the PLAN target) is the exit of the step that fixes it. Sweep members of lateblock (the
# size sweep) are not baselined.
foreach(legion_scn
    attackring-f1-64 attackring-f2-64 attackring-squad-64 attackring-f1-16 attackring-r200-64 attackring-mob-40
    attackring-mob-100 attackring-struct-60 attackring-fight-60 attackring-guard-64 attackring-wall-64
    attackring-river-64 fight-retarget ringcross chase-1 chase-50 chase-line goalblock-200 goalblock-fight-200
    goalblock-near-200 keelturn-180 keelturn-90 keelturn-0 staticblock-own staticblock-other staticblock-open)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;w5" TIMEOUT 600)
  if(NOT WIN32)
    add_test(NAME legion_check_${legion_scn}
             COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both
                     --check ${CMAKE_SOURCE_DIR}/tools/scenarios/baseline.json)
    set_tests_properties(legion_check_${legion_scn} PROPERTIES LABELS "legion;w5;nightly" TIMEOUT 1800)
  endif()
endforeach()
foreach(legion_scn lateblock-none lateblock-before-16 lateblock-late-16)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;w5;nightly" TIMEOUT 1800)
  if(NOT WIN32)
    add_test(NAME legion_check_${legion_scn}
             COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both
                     --check ${CMAKE_SOURCE_DIR}/tools/scenarios/baseline.json)
    set_tests_properties(legion_check_${legion_scn} PROPERTIES LABELS "legion;w5;nightly" TIMEOUT 1800)
  endif()
endforeach()
# Long routes (2026-10-14 Legion review, item 6): serplong's serpentine puts the bodies past the old 16-bit
# field potentials (87,356 at its head), serplong-control is the same shape inside that range. Both walk the
# whole route (180,000 and 100,000 ticks), so they run nightly; legion_longroute is the per-commit check.
foreach(legion_scn serplong serplong-control)
  add_test(NAME legion_scenario_${legion_scn}
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both)
  set_tests_properties(legion_scenario_${legion_scn} PROPERTIES LABELS "legion;nightly" TIMEOUT 1800)
endforeach()
# One progress clock (2026-10-14 Legion review, item 1): a body that registered before its field reached it
# measured squared cells to its goal, then potential x64, so its stall clock aged through most of a free walk
# while its headway clock advanced (the observer's stall_stale_ticks, tools/legion_observe.h: 9,401 at offset 0
# on the parent, 8,248 to 9,664 over the gate offsets). Passes at two digits or fewer, serial equal to workers.
# Slots by arrival (2026-10-14 Legion review, item 4): 60 bodies sent into a room whose one door is on its far side.
# The formation ranks its members by the straight line to the click, which the way round the room turns over: on
# the parent the first bodies through the door kept the slots beside it and 22 to 37 settled outside, never done
# at any gate offset. Passes when, over the core five, the median run has every body in and the group done,
# serial equal to workers.
# Passage lanes (2026-10-14 Legion review, item 3): 48 bodies take a 14-cell gap in a wall several abreast. On the
# parent the bodies that came through after the crowd had thinned walked straight for their slots, diagonally across
# the gap, through the lanes the earlier ones had held: 17 to 47 lane swaps in the gap strip over the core five
# (median 27). Passes when the median run swaps none there and the group is done, serial equal to workers.
# Passage lanes through a wide gap (2026-10-19, Legion ~100 round 2): 100 bodies take a 20-cell gap, narrower than
# their formation's band (25 cells) but not than the lanes' old 16-origin cap, so on the parent nobody kept a lane
# there: 9 to 37 lane swaps in the gap strip over the core five (median 15; Retail 3 over the gate offsets). Passes
# when the median run swaps none there and the group is done, serial equal to workers.
# The turn ribbon (Legion ~100 round 4, 2026-10-20): sbend's 48 bodies round wall 1's top (a right turn) and then
# wall 2's bottom end (a left turn). The pinwheel ranked each member once per order against the part's live centroid,
# so the second turn met the first one's ranks with its inner side flipped, and every file left its arc for the
# straight walk the moment its line grazed the wall end: 18 to 29 lane swaps at the second turn over the core five
# (median 27; Retail 5 over the gate offsets). Passes when the median run swaps at most 9 times there and the group is
# done, serial equal to workers.
# A way to the slot closed by arrivals (Legion ~100 round 4 review, 2026-10-20): mixed2's second leg at start offset
# +2. The ribbon brings the formation in a new order, and one body comes to stand 2.5 cells from its free slot between
# two settled arrivals of its own formation, one on each cell of the diagonal step's corner, neither of them nearer the
# click nor able to yield: on the parent it never queued and stood there for the rest of the run (g.G.done -1). Passes
# when the ground group is done, serial equal to workers.
# One slot hand-out budget a tick (2026-10-14 Legion review, item 5): four 120-body clicks to four dead ends in one tick.
# The formation slot searches had a budget per point, and none for a formation's first 32 slots or for a member
# without a slot: on the parent they walk 602,176 ring cells in one tick (B's second part searching against the
# formation its first 8 bodies took), every point's queue its own 131,072 on the ticks after. Passes when the
# per-tick peak stays under 150,000 (kHandOutCells, 131,072, plus the searches that start before it is spent),
# serial equal to workers.
# Ring upkeep (2026-10-19, ~100-body fixtures in tools/scenarios/legion100). Combat brakes an attacker the moment it
# is in reach, so a ring spot with a body firing on or beside it is spent; on the parent the field still led the army
# to the first arc it met (100 attackers against a walled target seen from one side: a jam on that side, the far
# arc empty). Passes when, at offsets 3,-3,4 (the target on the wall's corner, seen from part of the ring), the
# median damage dealt is 1400 or more (parent 1162, Retail 2307 / 2135 / 2227), serial equal to workers.
# An attack on a target walled in out of the attackers' region (offset 0 of the same fixture) keeps its order and plans
# a ring in reach round the walls, as Retail's attackers press up to them; on the parent the army walked to the one
# nearest reachable point, piled against one face and dropped its orders there (19 attackers in reach at the end,
# Retail 61). Passes at 30 or more, serial equal to workers.
# A Move group crossing its own firing ring (ringcross-100: 50 attackers ring a target, 50 movers pass at tick 1500).
# On the parent the ring members held behind the firing line stood out of reach in the movers' way: over offsets
# 0,1,-1 in 12000 ticks the group's t90 is 5458 / never / 5551 and 38 attackers get in reach (Retail 3926, 43).
# Passes when the median t90 is under 5400 and at least 40 attackers get in reach, serial equal to workers.
# Pressing in never shuffles (2026-10-19 review): a held attacker's step into reach must end in reach. On the first
# ring-upkeep head it tested reach at the neighbour origin's centre while the step ends half a cell short, so a body
# stepped in, was not braked, walked back out and pressed again (attackring-wall-64 offset -4: 12 of 14 presses from
# the same origin as the member's previous one). Passes with no such repeat at any gate offset.
# Legion ~100 round 2 (2026-10-19). A ring fills round the outside of its own firing arc: the inside of the ring
# within band 0 is charged in its field like a mover's corridor, so the queued army walks round the arc instead of
# into its back (ringcross-100 before the Move group sets off). On the parent half the 50 attackers were in reach at
# tick 850 (Retail 653). Passes when that time is 780 or less, serial equal to workers.
# A Move group's formation members whose lanes run into (or by) another command's firing ring walk round it on their
# own lane's side, steering by the flow round the ring's clearance circle (ringBend). On the parent every body took
# the shared field round the side of its nearest slot in one or two files and the group never completed within
# 5000 ticks at offsets 0,1,-1 (t90 4815 / 4696 / 4741, Retail 3857 / 3948 / 3951). Passes when the median t90 is
# under 4500 and the group completes (median g.M.done set), serial equal to workers.
# ringBend only bends a lane near the ring (3 clearance radii) and clear of walls up to it (2026-10-19 review of round
# 2): a Move group whose lanes meet a U-shaped wall long before another command's firing ring (ringbend-pocket). On
# the first round-2 head every member was bent into the pocket and the field led it back out, for good: 0 of 20
# arrived at offsets 0,1,-1. Passes when the group completes (median g.M.done set), serial equal to workers.
# Legion ~100 round 3 (2026-10-20). An attack ring round a target standing still walks its local detours and
# flow-around shuffles at travel speed, as Retail's mover traces round a body at its own speed: at the facing cap of an
# eighth the bodies queued outside the firing arc crept to and fro there (attackring-r200-64, 64 attackers with range
# 200 on open ground: 20,977 statue ticks against Retail's 155, 52 ever in reach against 64). Passes with under 2000
# statue ticks and at least 57 ever in reach, serial equal to workers.
# A ring spot the firing arc has closed off (reached by no way from outside reach through origins free of still
# bodies) is spent (round 3): ringcross-100's far side filled one rescan at a time, 33 of 50 attackers in reach by
# tick 1500 and half of them at 717 (Retail 43 and 653). Passes when at least 41 are in reach by tick 1500 (39 with
# the round's travel-speed detours alone) and half by tick 600, serial equal to workers.
# A ring member whose spot is spent with no free spot left in band 0 waits on an outer band's spot, not behind the
# firing arc (round 3): with the travel-speed detours alone the spare attackers stood across the Move group's lanes
# round the ring, ringcross-100's crossing t90 4089 over the gate offsets (3841 on the parent, Retail 3926). Passes
# when the gate median t90 is under 3950, serial equal to workers.
# A target walled in on every side (attackring-wall-100 at offsets 0,1,-1): Retail's attackers pressing at it against a
# 1-cell wall are clamped to the last sub-pixel of their origin, whose float centre rounds into the wall cell where the
# sight cast starts, and fire through it (Retail 689 / 350 / 767 damage). Round 3's presses seek those flush points;
# on the parent the ring stood round the walls and dealt none. Passes when the median damage is 400 or more, serial
# equal to workers.
# The closed-off-spot flood walks through every in-reach origin out of the target's sight, by that origin's own sight
# test (round 3 review): it took band 1's listed spots for that shadow, a sample packed a footprint apart, so 2x2 bodies
# crossed none of it and a yard reached only through in-reach shadow had every band-0 spot spent
# (attackring-gatehouse-40 at offset 0: no damage; Retail 557, round 2 224). Passes when the damage is 150 or more,
# serial equal to workers.
# Legion ~100 round 6 (2026-10-21): a ring whose spots lie through a corridor one body wide fills back to front. Each
# member takes, on arrival, the spot it reaches by its own walk that lies on no other free spot's way (ringDeepClaim)
# and walks to it clear of the shot (spotRoute). attackring-gatehouse-40 at offsets 0, 1, -1 (the west gate): the first
# body walked straight at its spot beyond the yard wall and was braked flush in the corridor's mouth, shutting it (median
# damage 167 on the parent; Retail 557 / 482 / 590), 479 after. Passes when the median damage is 400 or more, serial
# equal to workers.
# Round 6 review (2026-10-21): a deep ring's spots are not handed out afresh by walk every scan. The owner of an
# unspent band-1 spot changes it only for a band-0 spot or one strictly nearer by walk, and looks every second scan;
# one whose spot reads spent waits outside only when it read spent the scan before too. On the parent members changed
# spots scan after scan (unit 9 swung between a band-1 spot and a waiting spot from t=2069 to the end), and those walks
# on one scan tick made attackring-gatehouse-40 at offset -2 peak at 627,023 units of Legion work in a tick (226,354
# before round 6), 331,673 after. Passes when the peak is under 400,000, serial equal to workers.
# Legion ~100 round 4 (2026-10-20): a blind ring (no spot of the members' region sees the target) presses into reach
# out of sight, holds there instead of walking back to a spot centre out of reach, and spends no spot for a body
# standing radially out at the reach edge (nothing brakes a body there). attackring-river-64 at offset 0: 6 bodies in
# reach at the end before (the bank's band-0 spots spent by the row behind them; Retail 14 now, 21 ever), 17 after.
# Passes when 15 or more are in reach at the end, serial equal to workers.
# Round 4: a body in reach out of the target's sight walks on through the shadow to ground in sight (pressRoute), or
# deeper toward the target, and members waiting on outer bands are handed band-1 spots, so the in-sight ground between
# the firing arc and the box's walls fills two deep (attackring-wall-64 offsets -3, -4, 4: median 32 in reach at the
# end before, 37 after; Retail 43 / 42 / 32). Passes at 35 or more, serial equal to workers.
# Legion ~100 round 5 (2026-10-21): a body held a few pixels out of reach of a shadowed or blind ring (walls hide the
# target from part of the ground in reach) presses into the shadow, out of sight, and holds there in reach; its hold
# walks on through the shadow to the shot (pressRoute). Before, a waiting body whose own origin lay wholly beyond reach
# stood outside for good (only the shot pulled a body in). attackring-wall-64 offsets 1, -1, -3: 55 / 54 / 37 in reach
# at the end before (median 54), 61 / 58 / 40 after (Retail 55 / 54 / 43). Passes at 57 or more, serial equal to
# workers.
# Round 5 review (2026-10-21): the in-reach hold out of sight of a shadowed ring holds only a member that has pressed in
# at the target during its order (Member::shadowPressed). Holding every member standing in reach out of sight parked bodies on
# their way through the shadow strip to spots beyond reach, in the way of a second footprint's group on the same
# target. attackring-wall-mixed-56 offsets 0, 2, 3, damage the target lost: 1,304 / 1,415 / 1,760 before round 5,
# 1,224 / 1,295 / 1,661 with every such member held (median 1,295), 1,293 / 1,355 / 1,707 now (median 1,355; Retail
# 666 / 1,243 / 1,586). Passes at a median of 1,330 or more, serial equal to workers.
# Round 4 review: a walk on from the shadow (pressRoute) restarts the hold clock, as a detour does. Left held, the
# route branch dropped the route at once (held 30 ticks or more) and the search ran again every update: bodies frozen
# in the shadow for the rest of the run. attackring-wall-64 at offset 0: 3,674 walks before (one body re-routed every
# update for ~1,600 ticks), 136 after. Passes under 1,000, serial equal to workers.
# Round 4 review: the blind-ring rules key on Ring::blind (the shot needs sight and no spot has it), not on a ring
# planned without the sight test: a shot of 64 px or less, a lobber's or melee needs no sight and combat brakes its
# bodies on the reach edge. tools/scenarios/sweeps/attackring-r60-64.scn at offset 0: statue ticks 1,905
# before, 611 after (the parent's 611: every gate offset hash-identical to it again). Passes under 1,000, serial equal
# to workers.
# Legion ~100 round 4 (2026-10-20): attack and guard members round a target (a ward) standing still walk their detours
# and shuffles at travel speed whether or not their group has a ring (stillTarget). A fight (attack-move) order acquires
# its attacks one body at a time, so their group never gets a ring, and crept round the bodies ahead at the facing cap
# (attackring-fight-60 at offset 0: 30,229 statue ticks and 1,483 damage; Retail 38 and 2,814). Passes when the statue
# ticks are under 2,000 and the damage 2,000 or more, serial equal to workers.
# The same for a guard round a ward standing still (no reach ring: its stance is Follow_Ground's ring, World::guardRing,
# since protocol 303): attackring-guard-64 at offset 0 crept 42,133 statue ticks and had half its guards within 6 cells
# at tick 1,837 (Retail 194 and 1,191). Passes when the statue ticks are under 2,000 and half are within the scenario's
# range (9 cells since protocol 303: the ring's outer edge and a cell) by tick 1,500, serial equal to workers.
# A route round a still target keeps travel speed to its last cell (round 4): the short tails at the facing cap were
# most of the rings' remaining creep (attackring-wall-64 at offset 0: 1,329 statue ticks; Retail's gate median 173).
# Passes when the statue ticks are under 300, serial equal to workers.
# A member round a still target that its last crowd window found still commits no new flow-around shuffle (round 4):
# at travel speed its shuffles down the field's potential and its field steps back up it reversed hundreds of times
# behind the firing arc (attackring-r200-64: 413 reversals, three bodies 307 of the 389 its steering made; Retail 45;
# 337 with the whole-route speed alone). Passes when the reversals are under 200 and 57 or more ever got in reach,
# serial equal to workers.
# A fight leg's flow-around shuffles keep travel speed as a move leg's do (round 4): the column of a fight order flows
# round its own members standing in reach and round each other, and at the facing cap it crept wherever its way in met
# the arc off its own line (attackring-fight-diag-60, the target north-east of the column, at offset 0: 2,988 statue
# ticks; Retail 161). Passes when the statue ticks are under 300, serial equal to workers.
# A late ring (round 5): a fight order's attack group starts its field with one member (its column acquires the target
# one body at a time), so it had no ring and the column queued on the near arc (attackring-fight-60 at offset 0: half
# the attackers in reach at tick 1,526, Retail's gate median 1,206; 1,510 with the in-reach press edge alone). Passes
# when half are in reach by tick 1,300, serial equal to workers.
# Retaken 2026-11-03 (retail target search, protocol 312; owner's decision): attack-move legs are retail's
# MoveFight, whose scan takes only enemies in weapon range (96 px), not range + 90 as the removed stride scan did,
# so attackers convert to attacks later: t50 1,211 -> 1,339 (Retail 1,329 -> 1,251). The owner accepted it as the
# retail rule. Passes at 1,400 or earlier.
# A press may end exactly on the reach edge (round 5): combat fires there, but pressIn wanted a point a sixteenth of a
# pixel inside, so bodies held flush on their origins' edges 1-2 px out of reach (attackring-f1-64 at offset 0: 39 in
# reach at the end, Retail's gate median 43; 41 with the late ring and the stop-origin rounding alone). Passes when 43
# or more are in reach at the end, serial equal to workers.
# A melee press tests contact, not the TDF range (round 5 review): melee fires on footprint adjacency (meleeInRange),
# but the reach-edge press read reachFloat's TDF range (300 px for araknigh), so every point was "in reach" and a held
# body re-pressed to its own origin instead of stepping into contact (attackring-melee-30 at offset 0: 14,433 damage,
# 2,453 of 2,454 presses repeats; 28,153 before the reach-edge press, Retail 27,351). Passes when the damage at the
# end is 25,000 or more, serial equal to workers.
# Needs the retail araknigh, so only with -DTAK_TEST_DATA (CI has no install).
if(TA_TEST_DATA)
  add_test(NAME legion_meleepress
           COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/attackring-melee-30.scn --mode legion --offsets 0 --json
                   --data "${TA_TEST_DATA}")
  set_tests_properties(legion_meleepress PROPERTIES LABELS legion SKIP_RETURN_CODE 77
                       PASS_REGULAR_EXPRESSION "\"serial_eq_workers\":true,.*\"reach\\.ring\\.damage_end\":(2[5-9][0-9][0-9][0-9]|[3-9][0-9][0-9][0-9][0-9]|[1-9][0-9][0-9][0-9][0-9][0-9]+),")
endif()
# Round 6 (2026-10-21): the walk on past the shot (walkOnShot). In attackring-gatehouse-40's 3-cell corridor the first
# 2x2 body combat braked plugged the way single file and the queue behind it stood out of reach; now a ring member
# whose braked footprint would cut its queue off from ground with the shot walks on to the farthest such origin first.
# Offsets 0, 1, -1, damage the target lost: 167 / 250 / 84 before (median 167), 1,395 / 1,369 / 1,312 now (median
# 1,369; Retail 557 / 482 / 590). Passes at a median of 600 or more, serial equal to workers.
# The same on a second layout (attackring-gateyard-36: a 4-cell corridor, gates on the west and the south, 2x2 and 3x3
# bodies, the yard open on the north). Offsets -2, -3, -5: 326 / 180 / 1,001 before (median 326), 857 / 791 / 1,541 now
# (median 857; Retail 521 / 400 / 1,052). Passes at a median of 600 or more, serial equal to workers.
# The walk on goes onward (review, 2026-10-21). In attackring-deadcorridor-30 (a straight dead-end corridor) a body
# between two braked neighbours walked back and forth till its 6 walks ran out: its queue counted members outside the
# corridor wall, so the ground either side looked like its own. Now only members whose ground comes up to the body
# (this target's braked attackers passable) queue, and a later walk never ends nearer where the last set out. Offsets
# 0, 1, -1, walks: 12 / 8 / 12 before (median 12; 9 of offset 0's 12 went back), 5 / 3 / 3 now (median 3, none back);
# damage 275 / 445 / 338 -> 526 / 362 / 271. Passes at a median of 6 walks or fewer, serial equal to workers.
# Legion work-scaling gate (W(4N0)/W(N0), slot work per slot, upkeep per group,
# every counter against tools/legion_cost_baseline.json) and the battle
# scenarios' window tables (tools/legion_cost.py). Counters only: wall time is
# printed, never gated. The larger battles run nightly.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
      foreach(battle battle-field-2x60 battle-field-2x250 battle-field-2x500 battle-assault)
    add_test(NAME legion_scenario_${battle}
             COMMAND "${Python3_EXECUTABLE}" ${CMAKE_SOURCE_DIR}/tools/legion_cost.py battle
                     ${CMAKE_SOURCE_DIR}/tools/scenarios/${battle}.scn
                     --runner $<TARGET_FILE:legion_scenario> --tmpdir ${CMAKE_BINARY_DIR}/legion_cost_tmp)
  endforeach()
      # The committed W5 fixtures are the output of tools/scenarios/gen_w5_fixtures.py (edit the script, not the files).
      # The baseline gate (tools/legion_check.py): every baseline.json entry, exception and accepted
  # regression carries a reason, and the gate's synthetic unit tests.
      # docs/legion-pathfinding.md stays tied to the instruments: every Known weakness cites a fixture and key (or a
  # test) that exists, and the quoted field quota is kFieldQuota. The selftest renames one cited scenario, key and
  # test in a copy and fails unless each is caught (tools/legion_doc_links.py).
        # Legion 10,000 stage 4: the field scheduler under continuous load (tools/scenarios/scale/sched-load.scn:
  # field requests faster than the quota serves them for the whole run). Service gaps and start-request ages
  # within the policy's bounds, no body waiting over 60 ticks for a field, serial == workers
  # (tools/legion_sched_load_test.py). The _div run divides the registration, plane-build and awareness
  # budgets by 512 under TA_LEGION_VERIFY: the registration FIFO (deferred registrations), the incremental
  # plane builds and the resumable awareness scan all run over budget, with every index checked.
  # legion_sched_mix (sched-mix.scn, a two-map field allowance): memory pressure -- the fit scan, the
  # reservation, request promotion and shares behind a stopped scan run in the sim, and every request's
  # wait stays within a bound derived from the allowance, the tenure and the measured queue.
  # (Stage 6: sched-load's squads take tile chains in the game, and then no longer keep the quota saturated; the
  # two load tests run it with every route the window build, --no-hier-routes, so they still test the scheduler
  # under the continuous load they were written for.)
        # legion_sched_pin: many-1000 under a two-map allowance, where fields shared by groups that never finish
  # once pinned the room a reservation waited for (admissions froze for the rest of the run): admission
  # liveness. legion_sched_inflight: many-2500, where refreshes promoted out of their class used to pile up
  # past the windows while memory was short (78 in flight): the per-kind cap.
        if(TA_SLOW_TEST_BUILD)
    # Unoptimized: 613 s alone in Clang Debug, 862 s of its 900 s in a parallel Clang Debug suite (2026-10-18).
      endif()
    # Route requests on large maps (2026-11-15; docs/legion-pathfinding.md sections 2 and 9; the audited 2048 x 2048
  # game, where a route over the whole map is an eighth of the field allowance and units stood waiting for one).
  # legion_starve (starve-1024: an allowance of eight whole-map fields, lone groups behind a wall, later commands to
  # a point another still walks to, a covered and a buried goal): nobody waits long, no reservation, and the fields
  # stay within 20 MB (the commit before: 414,687 waiting member-ticks, a longest wait of 2,101 ticks, 40 of 260
  # orders finished, 162 stuck, 4,348 reservation ticks, 33.5 MB).
  # legion_cutoff_closed / _reopen / _toggle (six bodies cut off from their goal after the order by a closed gap):
  # no field is built over and over for them (52 / 45 / 41 before; closed: one waited 3,392 ticks), they are
  # trapped where the gap stays closed, and where it opens again all six still arrive inside the authored radius
  # no later than they did before (the bounds are the commit before's arrival ticks plus 5%).
  # legion_refreshfirst (sched-load with the game's routes, offsets 0 and +-1): a body standing off a touched chain
  # is served as a waiting one (at most 5,000 waiting member-ticks an offset; 23,407 or more before).
  # legion_armies (armies14-2048: fourteen 60-body commands at the game's allowance, more whole-map windows than
  # it holds): a ratchet on a known weakness, not a pass -- the 60 one-body groups ordered meanwhile all arrive
  # (10 before) and the waiting more than halves (105,579 member-ticks and a longest wait of 2,829 ticks before).
  # legion_seedarmy (seedcorpse-army: a covered goal under a 60-body command): a ratchet on a known weakness -- its
  # window is the whole map from then on, 21 fields built where the same walk without the feature builds one.
                  # Legion 10,000 stage 5: the route consumers that state their region (lanes, slots, rings, the formation
  # entrance, awareness) on paused routes, every first build pausable (--pause-all), under TA_LEGION_VERIFY,
  # serial == workers; each consumer asked a paused build for its region and no read was uncertain
  # (tools/legion_route_paused_test.py).
      if(TA_SLOW_TEST_BUILD)
    # Unoptimized: 1266 s alone in Clang Debug (2026-10-18); it timed out at 900 s in every unoptimized suite
    # of the 0.8.4 sweep (GCC -O0, Clang Debug, ASan). Optimized: 43 s Release, 197 s -O2 -g in a parallel suite.
      endif()
  # Legion 10,000 stage 9: the parallel planning jobs (chain tiles built beside the serial pass, plane rows in
  # chunks) decide nothing: the state hash and every observer key equal the serial run at 1, 2, 3, 7 and 63 workers,
  # with the jobs' completion order perturbed, and forced inline under TA_LEGION_VERIFY (tools/legion_parallel_test.py).
      if(TA_SLOW_TEST_BUILD)
    # Unoptimized: 1605 s alone at GCC -O0 (8 cores, 2026-10-18); it timed out at 1800 s in every unoptimized
    # suite of the 0.8.4 sweep. Optimized: ~750 s in a parallel Release or -O2 -g suite.
      endif()
  # Legion 10,000 stage 7: crossing walkers no longer stand in the knots they form (scatter-400: 400 single
  # walkers to unrelated destinations, serial and workers; tools/legion_traffic_test.py).
      # Legion 10,000 stage 8: a formation member gaining on its own slot beyond the click is not settled short of
  # it (one-1000: at least 960 of 1,000 arrive; tools/legion_arrival_test.py).
      # Legion 10,000 stage 8: chokepoints as queues -- route choice by travel time plus queue delay and admission at
  # narrow passages (bridges-1000: both bridges used, at least 450 of 1,000 arrive; maze-1000: groups take the other
  # doors, at least 850 arrive; tools/legion_choke_test.py).
      # Legion round 2, item 3: allies swap places -- two allied members blocking each other head on trade cells, so
  # opposing streams on bridges-1000 no longer gridlock at offsets -2, -1, 1, 2 (at least 850 of 1,000 arrive, at
  # most 10 stuck; main 558..741 arrive, 182..279 stuck; tools/legion_swap_test.py).
      # ... and the same meet at the start offsets the test above does not read (-5, -4, -3, 3, 4, 5), and on attack-move
  # orders at the offsets where it threads (bridges-1000-fight at -2, -1, 2, 5): none of them may stand stuck. The
  # meet is bistable, and a rule that moves a body of a standing knot too early tips it: the give-way in a knot (below)
  # at 600 dead ticks left 302 stuck at offset -4 on move orders (565 of 1,000 arrived; 750 and none stuck without
  # it) and 197 at offset 5 on attack-move orders, which is why it waits kStandPairMove and kStandPair.
  # Offset -4 is the thin one: 750 arrive there, on the parent of these rules as now, against 909 to 1,000 at the
  # other five. The arrival bound alone failing at -4 with nobody stuck is a loss of over 6% at that offset from
  # whatever changed the meet, not the gridlock this test is for (that one leaves hundreds stuck).
        # Bodies that stand for good holding an order (the audit of an 18-hour all-computer game, 2026-11-14: 148 units
  # at its end; docs/legion-pathfinding.md, "Bodies that stand for good"). Each of these failed on the parent.
  # doorway-pair: a 4x4 and a 3x3 body that meet at the mouth of a channel one large body wide each stood on cells the
  # other needed, for good; the one whose step frees the other's now gives way (both arrive; Retail walks both
  # through). On attack-move orders after kStandPair dead ticks, on move orders (doorway-pair-move) after
  # kStandPairMove. Asset-free.
      # headon-fight: two bodies of one footprint head on in a one-body lane on attack-move orders, which take no swap,
  # stood for good; the swap serves a fight or patrol leg too once both stand dead kStandSwap ticks (both arrive;
  # Retail ends both orders where they meet). Asset-free.
    # settled-crowd: six bodies sent through a corridor that a settled crowd of another order fills stood before it for
  # good with their orders; the orders now end beside it (the stand-down, the counterpart of Retail's growing goal
  # circle: Retail ends all six there as well). Asset-free.
    # idle-plug: four corridors, each plugged wall to wall by idle bodies; six bodies sent through each stood before
  # the plug for good. Behind the player's own idle bodies (A on a move order, D on attack-move) and an ally's (B)
  # the orders now end; behind an enemy's (C) they are kept, as before: an enemy's body never ends an order.
  # Asset-free.
    # escape-step: a body on an origin illegal for its size whose straight step at its target the mover refuses stood
  # for good; it walks to the legal origin beside it (A arrives). The same body with every such origin covered by
  # an enemy's idle bodies keeps its order past the long stand-down (P: an enemy's bodies never end one).
  # Asset-free.
    # siege-gap: an army on one attack-move order behind an enemy body in a gap two cells wide: the bodies in reach
  # stand firing (an attack ahead of their leg, no Legion member: combat holds them in reach from its first tick)
  # and the rest wait behind them for some 6,000 ticks. The stand-down read the firing line as dead and ended the
  # waiting bodies' orders after 4,800 ticks (43, 23, 49 and 56 of 60 arrived at these offsets); a body on an attack
  # or guard leg is its owner's to end, so all 60 go on through when the target dies. At offsets 2..5 fewer bodies
  # reach the target and it outlives the run: the army keeps its orders to tick 14,000 (legion_siegegap_hold; 51 to
  # 58 of the 60 orders were ended). That test bounds only the orders ended off the arrival disc, which is what a
  # stand-down here is, so it holds as well if a change to combat lets the target die at those offsets.
  # siege-gap-move: the same with 24 bodies on a move order queued behind the firing army (55, 25, 58 and 33 of 60
  # arrived). All three pass on the parent of the stand-down as well: they guard the rule, with weapons on and past
  # its long threshold. Asset-free.
        # file-trade: two files of 14 on move orders trade through each other in a lane one body wide and come to their
  # own settled arrivals with a dead age as long as the trade (a swap's gain is no progress). The short stand-down
  # took 11 of the 28 on their first held update; it now waits until the body's route has stood kStandMeet ticks
  # too, and the settle rule settles all 28, as on the parent of the stand-down. Asset-free.
    # plug-leaves: a plug of the player's own idle bodies that is ordered away at tick 450 (A), 900 (B) or 2,400 (C).
  # An order the stand-down ended stays ended: C's six stand idle when their plug leaves, where all six arrived
  # before the rule (the rule's price; Retail ends all 18 orders within 583 ticks). A plug that leaves before the
  # bodies behind it have stood 600 dead ticks, or within the short stand-down's second clock, must cost nothing:
  # A and B arrive, as on the parent of the stand-down (a guard for those two bounds). Asset-free.
    # knot-three: a 3x3 body against two 2x2 bodies abreast in a corridor too narrow to pass in, on attack-move orders
  # (F) and on move orders (M). All six stood for good with their orders. The knot has no way out, so nobody gets
  # through now either (Retail squeezes five of the six past), but it is bounded: the big body and the small one
  # before it give way to each other at most kMaxGiveWays times each, a cell back and a cell on, and the long
  # stand-down then ends every order. Asset-free.
    # guard-plug: the player's own bodies on an attack order (B) and on an attack-move order (C) stand firing at an
  # enemy body they cannot destroy within the run and plug a corridor wall to wall; six bodies on a move order wait
  # behind each. A firing line is its owner's to end: the orders behind it are kept past the long stand-down (read
  # as bodies without progress, the line of C ended five of its six at 4,800 dead ticks). A guard for the rule;
  # siege-gap is the same on a whole army. The file's part A, a plug of guards beside a still ward, keeps its
  # orders as well and is not bounded here: whether such guards count as settled is an open question
  # (docs/legion-pathfinding.md, "Known weaknesses"). Asset-free.
    # churn-knot: pairs in each other's way while a 1x1 feature beside a lane's mouth is placed and lifted every 1,500
  # ticks. Each change refreshes the routes it touches and re-bases the stall clock's record to where the body
  # stands; a body one step back from its best place then read its next step forward as a gain, and its knot's dead
  # age restarted: on move orders (lane M) the pair held its orders for good, with a change every 150 to 4,500 ticks
  # alike. Liveness now keeps a route record of its own, re-based on the origins the body remembers standing on
  # (Member::liveLeft), and both orders end (K's too, as before). At offset -1 the pair's give-way works and the
  # test passed before as well; at 0 and 1 it failed (two stuck). Asset-free.
    # ... and its lane O, a route that opens under a pair that has stood dead some 2,500 ticks: the big body turns
  # round, walks the new way (back over ground it remembers first) and arrives near tick 7,000, some 1,700 ticks
  # past the tick the long stand-down would have ended its order at; the small one arrives behind it. A guard for
  # the record (it passes without the stand-down as well): with a body's own gains not counted the big body's order
  # ends on the way. Read at offset 0 (the file's header says where the meets fall at the others).
    # reorder: units that gave their order up behind a plug of their own idle units are ordered again, with the plug
  # still there (they give up again) and after it has left (all arrive). Run under TA_LEGION_VERIFY: at offset 1 a
  # body walking off an illegal origin reached its legal origin and was moved on by another body's swap in the same
  # tick, and the walk's stale record aborted the hook at tick 3,507 (a debug build; Release compiles the hook out).
  # Asset-free.
    # pocket-far: the give-way's reach (kGiveReach, 4 origins). A 3x3 against a 2x2 body on move orders in a lane whose
  # one pocket lies 7 cells behind the small body, and four bodies meeting at a crossing of two lanes, twice: all
  # ten stood for good. Nobody gets through now either (Retail walks the pair through), but every order ends and no
  # body gives way more than kMaxGiveWays times on its leg (80 in all at offset 0). The measure of that limit
  # (docs/legion-pathfinding.md, "Known weaknesses"). Asset-free.
        # crowdlock: the audited game's standing crowd rebuilt on its real ground (246 bodies among the wrecks of a base,
  # 147 attack-move orders). Stock Legion ended 53 of the orders in 9,000 ticks and left 85 bodies standing with
  # theirs; every order now ends and nobody stands (Retail ends 143).
    # crowdlock-churn: the same crowd while a 1x1 feature in its area is placed and lifted every 300 ticks (a real game
  # changes its map all the time). Each change restarted the dead age of a knot that had a body one step back from
  # its best place (legion_churnknot): the doorway pair and two bodies queued on it held their orders for good, 143
  # of 147 ended, at tick 20,000 as at 9,000. Every order ends now, the last at tick 7,528.
      # Corner lanes (protocol 313): a column turning from one corridor into another keeps its files (at least 3.0 past
  # the corner at offsets 0, +-1..3 and a median t90 of at most 1,400; main 2.22..2.62 files, t90 1,507;
  # tools/legion_corner_test.py). Asset-free.
      # Overtaking (2026-11-15): a fast body held behind a slower ally walking the same way steps aside and passes it
  # where there is room (single-file mixed columns on open ground and on a road 8 cells wide: fast bodies arrive
  # earlier and stop far less; a corridor 4 cells wide: no worse; nobody slow later; tools/legion_overtake_test.py).
  # One click of several speed groups and at most 48 bodies (the arrival layout, 2026-11-16): each body gets a spot
  # of its own, fast bodies in front, and the fast ones pass the slow ones anywhere on the way; by a narrowing just
  # behind the point, or a three-speed click by terrain beside its rows, the click keeps the formation's own shape. Over 28 one-click
  # fixtures of 10-48 bodies (four with the point past a gate, a bridge, a gap approached aslant and by a wall), per
  # body and per speed group against main: every body settles, none wanders after settling, no two settle on each
  # other, no more lanes crossed than on main, at most 5% (fast 8%) settle off their own slot and 6% of the faster
  # ones a row behind a slower group's front, main's per-offset caps on the fast bodies hold, and the fast group comes
  # in earlier than on main (no later where the formation's own shape is kept; not checked for fast bodies at the
  # rear or middle of a short order, which walk round the slow ones). Owner sign-off (2026-11-16): field and road may
  # come in level with main (at most 1% later) and knights starting behind slower rows have measured per-fixture bars
  # (rear-500, rear-600, rear-240). Asset-free.
    # About 70 s on 20 cores, but over 600 s on CI's 4-core Windows runners (b700ec15): nightly, so CI's Legion
  # selection skips it (tools/legion_ci_selection.txt) and ctest -L quick does too; ctest -L legion runs it locally.
    # Legion 10,000 stage 8: shared destination areas -- churn-1000's half stopped and sent again to the point the
  # other half is arriving at joins its area instead of planning round it as traffic (at least 780 of 1,000 arrive,
  # at most 40 stuck; the previous commit 717 / 73).
      # Legion 10,000 stage 8: a formation member with a free step toward its own slot is not pressed (one-1000: at
  # least 995 of 1,000 arrive, the 99th percentile too, at most 5 stuck; the previous commit 978 / never / 14).
      # Legion 10,000 stage 8: an entrance per group (room2door: one order of 2x2 and 3x3 bodies into a room through
  # two doors; the second group finds its own entrance on its own plane, and all 100 arrive).
      # One passage, one admission (2026-10-19): two allied armies of 50 head on at bridges-100's 12-cell bridge. A group
  # re-planned onto a tile chain skipped admission (a chain records no labels) and walked into the strip the other
  # way held (offset -5: 36 arrive, 64 stuck), and an admission that lapsed when the token turned left its bodies
  # standing in the bridge's mouth, in the way of the stream let in next (offset 0, once chains were admitted: 52 /
  # 48). Checked at both offsets: at least 95 of 100 arrive and at most 2 stand stuck at the end.
      # ... and on a bridge of varying width (taperbridge-100: 12-cell mouths, a 6-cell middle, five sector lines): the
  # passage is one admission over all its records, and a chain group's last few bodies (fewer than 8 left) still
  # ask before walking in. On the parent the stragglers met the other army in the middle (offset 0: 21 stuck,
  # -4: 25). Checked at 0 and -4: at most 2 stand stuck at the end.
      # Legion ~100 round 3 (2026-10-20): a way stays open for kGreenTurns of its crossings before the other way's wait
  # closes it, not a fixed 300 ticks. On taperbridge-100's 160-cell strip every turn cost a whole crossing and mixed
  # the two crowds on an apron; the parent turned six or more times and ended at offset +1 with 81 bodies gridlocked
  # in the east mouth (19 of 100 arrived), at -5 and +8 with 89 and 76 arrived. Checked at +1, -5 and +8: at least 98
  # arrive and at most 2 stand stuck at the end.
      # ... and a group of fewer than kOpeningMembers bodies waits its turn too (taperbridge-small: ten orders of 5 sent
  # west, 150 ticks apart, against an army of 50 going east). On the parent the small groups' routes recorded no
  # labels, so they walked into the strip the army held and met it head on in the 6-cell middle (offset 0: 17 of 100
  # arrived, 83 stuck; -8: 12 / 88; gridlocked at 16 of offsets -8..8). Checked at 0 and -8: at least 95 arrive and at
  # most 2 stand stuck at the end.
      # ... and a lone waiter takes its turn although a body of the other way stands admitted before the passage and never
  # uses it (passage-starve: on the parent the passage was freed every 120 ticks and retaken by that body, which has
  # the lower unit number and updates first, for good -- three units of the audited 18-hour game stood so for 14
  # hours). The waiter must arrive. Asset-free.
      # Head-on meets near both goals (many-100: two pairs of 25-body orders sent to each other's positions meet
  # within kPivotNear of both goals; the meet band now holds up to the destination area plus one lead): at least
  # 98 of 100 arrive, the 90th percentile within 1,000 ticks (the previous commit 96 / 1,616; Retail 92 / 1,144).
      # Departures through an arrived crowd (many-1000: each order's last bodies leave through the next order's
  # arrivals; settled friendly bodies of any allied player yield or part for them, and a long detour round a
  # crowd keeps travel speed): at least 900 of 1,000 arrive, at most 20 stuck (the previous commit 746 / 73).
      # Flow-around shuffles at travel speed (2026-10-19, Legion ~100 round 2): a body blocked by a body steps into a free
  # neighbour nearer its goal without turning, and walked that step at the facing cap -- an eighth of travel speed
  # sideways, a one-cell step in about 70 ticks -- so a crowd folding round a wall end or past a block of idle bodies
  # stood in its own shuffles. island-1x100 (100 bodies round a 24x24 rock) at offsets 0, 1, -1: t90 at most 2,300
  # and the last arrival by 3,000 (the previous commit 2,747-2,882 and 3,468-3,550; Retail 2,296 / 2,552 median).
      # ... and 50 movers past a block of 50 idle bodies (idle-50x50) at offsets 0, 1, -1: the 99th percentile arrives
  # by tick 3,650 (the previous commit 3,775-3,959; Retail 3,418 median).
      # Room at a merge (2026-10-20, Legion ~100 round 4): a formation descending into a ragged convex corner packed into
  # the wedge beside one moving file, and the corner's front bodies waited 1,000+ ticks for a gap in it. Members passing
  # (or about to pass) a queue head held 60 ticks move over a lane instead. corner-1x100 at offsets 0, 1, -1: no body
  # stands 900 ticks, stopped permille at most 245 and t90 at most 4,100 (the previous commit 28-36 bodies, 267-282
  # and 4,157-4,367; now 0, 221-238 and 3,888-4,013).
  # 2026-11-02 (no facing cap on unturned steps, no flow-around behind the aim, the press): the funnel drains sooner
  # (t90 3,512 / 3,737 / 3,487, stopped permille 160 / 193 / 159; before 3,759 / 3,827 / 3,737 and 236 / 219 / 218)
  # and the wedge's last bodies stand instead of creeping: at offset 1 three bodies at its back (77,62)-(77,66), each
  # held 991-1,098 ticks for the held bodies of its own file ahead (a hold the file's front releases one body at a
  # time, as before, when it crept at the facing cap and read as moving). The cap change alone gives 1 / 0 / 2 such
  # bodies with the old arrival; the three together 0 / 3 / 0. Bound 0 -> 3 on that measurement; the rest unchanged.
      # The Legion steering changes of 2026-11-02 (the commits a06d3f80, 740f0bd9, 75925618, 8dcb123f, c4933aa8 give the
  # measured basis and the shape landed), one test each. The
  # numbers are the final shape's against main's (0.9.18 plus the observer instrument), measured with both binaries at
  # the offsets named; the per-mechanism why.* keys are docs/legion-scenario-keys.md's.
  # 1. No facing cap on an unturned step: a formation member's flow-around shuffle, a side-step, a detour route and a
  # nudge keep travel speed (the cap applies to a step that turns the course, to a plain mover's flow-around shuffle
  # and to the in-reach press), so the creeps that were 98% of Legion's statue ticks are gone. island-1x100 (100 bodies
  # round a 24x24 rock) at offsets 0, 1, -1: the statue ticks the observer attributes to the two cap walks are at most
  # 10 each (why.flow_cap.statue now 0 / 0 / 0, main 420 / 121 / 222; why.route_cap.statue now 0 / 0 / 0, main
  # 157 / 284 / 435; both 0 at all eleven gate offsets), and t90 at most 1,640 (now 1,401 / 1,487 / 1,487; main
  # 1,549 / 1,487 / 1,532, so the t90 bound alone would pass main: it guards the arrival, the cap keys prove the change).
  # The total statue_ticks is no measure here: the press's sub-cell nudges count as statues too (why.nudge.statue
  # 866 / 1,153 / 1,354), so the total rose from 577 / 405 / 664 to 866 / 1,153 / 1,364 while the nudge-free remainder
  # fell to 0 / 0 / 10.
      # 2. No flow-around behind the aim: blocked by a body, a member no longer shuffles through a neighbour more than 90
  # degrees from the way it is going (a backward walk the next descent undid). bridges-100 (two allied armies of 50
  # cross a river in opposite directions over a 12-cell bridge) at offsets 0, 2, -2: at most 2,000 backward steps (now
  # 1,325 / 1,454 / 1,012; main 3,291 / 2,519 / 3,166), the first army's t90 at most 9,300 (g.A: now 8,498 / 8,242 /
  # 8,248; main 8,479 / 10,492 / 10,537 -- its median over the eleven gate offsets fell from 10,248 to 8,345) and all
  # 100 arrive. Offsets 2 and -2 rather than 1 and -1: main's 1 and -1 are two of its three fast starts (8,737 / 8,692,
  # within the bound), and the new shape keeps a slow start at -3, -4, -5 (9,997 / 10,161 / 9,997). The second army's
  # t90 is not bounded: a 5% gain (g.B 8,879-9,457 against 9,101-9,641) is inside any sensible slack.
      # 3. The press: on a leg that engages no enemies, a body blocked by bodies with no way round walks the free axis of
  # its step and the other to its origin cell's edge (Retail's clamp) instead of stopping dead, so a queue closes up
  # and slides along a file. corner-1x100 at offsets 0, 2, -2: stopped permille at most 205 (now 160 / 182 / 164; main
  # 236 / 235 / 226), the stopped samples of the queue hold (why.hold_movers.stopped) at most 5,200 (now 3,994 / 4,650 /
  # 4,061; main 6,014 / 6,203 / 6,109) and t90 at most 4,200 (now 3,512 / 3,843 / 3,669; main 3,759 / 3,917 / 3,827,
  # inside the bound: it guards the arrival, the stopped keys prove the change). Offset 1 is the new shape's highest
  # stopped permille of the nine gate offsets (193, main 219), so the test reads 0, 2 and -2 and legion_cornermerge
  # keeps 0, 1, -1 at the old bound. The nudges count as statue ticks (why.nudge.statue 6,192 / 6,918 / 7,093 of
  # statue_ticks 6,200 / 6,924 / 7,115; main 2,507 / 3,466 / 2,202 with no nudge), so statue_ticks is no bound here.
    endif()
# Legion navigation requirements as World checks (tools/legion_acceptance_test.cpp).
# Legion asserts; Retail runs the same checks with --report (comparison
# evidence, never failing).
add_executable(legion_acceptance_test tools/legion_acceptance_test.cpp)
target_link_libraries(legion_acceptance_test PRIVATE ta-formats)
foreach(legion_check singleunit group jagged trapped crowdheld)
  add_test(NAME legion_acceptance_${legion_check}_legion COMMAND legion_acceptance_test legion ${legion_check})
  set_tests_properties(legion_acceptance_${legion_check}_legion PROPERTIES TIMEOUT 600 LABELS legion)
  add_test(NAME legion_acceptance_${legion_check}_retail
           COMMAND legion_acceptance_test retail ${legion_check} --report)
  set_tests_properties(legion_acceptance_${legion_check}_retail PROPERTIES TIMEOUT 600 LABELS legion)
endforeach()
# crowdheld is a RATCHET, not a pass-all: 64 distinct goals packed at a 3-cell
# pitch (1-cell gaps) behind a gated door. The block leaves deepest-first, but
# the door reorders a pair in one row; the shallower unit settles first and
# seals the deeper goal, which is reachable only through neighbouring goals.
# Legion reaches 62/64 in goal at the W0 base (AR-09, docs/legion-pathfinding.md
# "Known weaknesses"); the test passes at 61 or more in goal (the physical_in_goal
# rule prints its value) and still fails on any spin, terrain-stuck unit or a
# lower count. Group right-clicks use a shared point with re-choosable slots and
# do not create this layout. Raise the 61 with the count as AR-09 improves.
# The one terrain-stuck unit (units_ever_terrain_stuck 1) is a limit the user accepted at the close of
# the Legion plan (decision W9-1, "Known weaknesses"), so only spin still fails the test.
set_tests_properties(legion_acceptance_crowdheld_legion PROPERTIES
                     PASS_REGULAR_EXPRESSION "physical_in_goal == moving \\(value 6[1-4]\\)"
                     FAIL_REGULAR_EXPRESSION "FAIL: spin_while")
add_executable(retail_search_scratch_test tools/retail_search_scratch_test.cpp)
target_include_directories(retail_search_scratch_test PRIVATE src)
add_executable(retail_group_test tools/retail_group_test.cpp)
target_link_libraries(retail_group_test PRIVATE ta-formats)
add_executable(cobanim_test tools/cobanim_test.cpp)
add_executable(unit_gift_test tools/unit_gift_test.cpp)
target_link_libraries(unit_gift_test PRIVATE ta-formats)
add_executable(cmdflow_test tools/cmdflow_test.cpp)
add_executable(conn_test tools/conn_test.cpp)
add_executable(validationworker_test tools/validationworker_test.cpp)
target_link_libraries(validationworker_test PRIVATE ta-formats)
add_test(NAME validationworker COMMAND validationworker_test)
set_tests_properties(validationworker PROPERTIES TIMEOUT 10)

add_executable(server_commands_test tools/server_commands_test.cpp)
add_executable(server_address_test tools/server_address_test.cpp)
target_include_directories(server_address_test PRIVATE src)
add_test(NAME server_address COMMAND server_address_test)
add_executable(session_failure_test tools/session_failure_test.cpp)
target_include_directories(session_failure_test PRIVATE src)
add_test(NAME session_failure COMMAND session_failure_test)
add_executable(local_server_test tools/local_server_test.cpp)
target_include_directories(local_server_test PRIVATE src)
add_test(NAME local_server COMMAND local_server_test)
set_tests_properties(local_server PROPERTIES TIMEOUT 30)
target_link_libraries(server_commands_test PRIVATE ta-formats)
add_test(NAME server_commands COMMAND server_commands_test)
# A player's order the sim refuses (a Load it may not board, a type off the
# producer's build menu) changes nothing, through the server's parser.
# Selection-order transactions at the server (server/orders.h, protocol 267).
add_executable(server_orders_test tools/server_orders_test.cpp)
target_link_libraries(server_orders_test PRIVATE ta-formats)
add_test(NAME server_orders COMMAND server_orders_test)
# Explicit selection orders end to end: codec, client packing, sim application (protocol 267).
add_executable(selection_order_test tools/selection_order_test.cpp)
target_link_libraries(selection_order_test PRIVATE ta-formats)
add_test(NAME selection_order COMMAND selection_order_test)
set_tests_properties(selection_order PROPERTIES LABELS legion)
add_executable(mod_transfer_test tools/mod_transfer_test.cpp)
target_link_libraries(mod_transfer_test PRIVATE ta-formats)
# The host's powers, speed and pause across reconnects, as MpClients see them (host_session).
add_executable(host_session_test tools/host_session_test.cpp)
target_link_libraries(host_session_test PRIVATE ta-formats)
add_executable(mod_packs_test tools/mod_packs_test.cpp)
target_link_libraries(mod_packs_test PRIVATE ta-formats)
add_test(NAME mod_packs COMMAND mod_packs_test)
add_executable(gameplay_hash_test tools/gameplay_hash_test.cpp)
target_link_libraries(gameplay_hash_test PRIVATE ta-formats)
add_test(NAME gameplay_hash COMMAND gameplay_hash_test)
# The menu Benchmark mounts and hosts no mod pack of any kind (src/client/benchmarkmods.h).
add_executable(benchmark_mods_test tools/benchmark_mods_test.cpp)
target_link_libraries(benchmark_mods_test PRIVATE ta-formats)
add_test(NAME benchmark_mods COMMAND benchmark_mods_test)
add_test(NAME no_game_mods_toggle COMMAND "${CMAKE_COMMAND}" -DLOBBY=${CMAKE_SOURCE_DIR}/src/client/gameview_lobby.cpp
         -DROOM=${CMAKE_SOURCE_DIR}/src/client/gameview_room.cpp -P "${CMAKE_SOURCE_DIR}/tools/no_game_mods_toggle.cmake")
# The server-side AI (src/ai): every executable that runs a Controller links these.
set(TA_AI_SOURCES src/ai/ai.cpp src/ai/taretail.cpp src/ai/builtinprofile.cpp src/ai/classicbrain.cpp src/ai/classicprofile.cpp src/ai/classictypes.cpp)
# mission_orders_test reads a scripted order as the Retail AI does (firstMissionOf).
target_sources(mission_orders_test PRIVATE ${TA_AI_SOURCES})
add_executable(room_worker_test tools/room_worker_test.cpp src/server/roomtick.cpp src/server/rosterdump.cpp ${TA_AI_SOURCES})
target_link_libraries(room_worker_test PRIVATE ta-formats)
add_test(NAME room_worker COMMAND room_worker_test)
set_tests_properties(room_worker PROPERTIES TIMEOUT 30)
# The match standing a room tick reports (one team left, nobody left, never for one team):
# what the server ends a game on when no human is seated.
add_executable(room_standing_test tools/room_standing_test.cpp src/server/roomtick.cpp src/server/rosterdump.cpp ${TA_AI_SOURCES})
target_link_libraries(room_standing_test PRIVATE ta-formats)
add_test(NAME room_standing COMMAND room_standing_test)
set_tests_properties(room_standing PROPERTIES TIMEOUT 30)
# Server tick pacing on a simulated clock: exact rates, speed changes, bounded catch-up.
add_executable(tick_pacer_test tools/tick_pacer_test.cpp)
target_include_directories(tick_pacer_test PRIVATE src)
add_test(NAME tick_pacer COMMAND tick_pacer_test)
# Client jitter-buffer pacing on a scripted clock: stalls are not jitter, the target stays reachable.
add_executable(pacing_test tools/pacing_test.cpp)
target_include_directories(pacing_test PRIVATE src)
add_test(NAME pacing COMMAND pacing_test)
add_executable(featgen_test tools/featgen_test.cpp)
add_executable(featdie_test tools/featdie_test.cpp)
add_executable(transport_test tools/transport_test.cpp)
add_executable(queued_load_test tools/queued_load_test.cpp)
add_executable(queue_modifiers_test tools/queue_modifiers_test.cpp)
add_executable(queued_target_test tools/queued_target_test.cpp)
add_executable(replay_test tools/replay_test.cpp)
add_executable(pathblock_test tools/pathblock_test.cpp)
add_executable(movement_orders_test tools/movement_orders_test.cpp)
target_link_libraries(movement_orders_test PRIVATE ta-formats)
add_executable(retail_script_test tools/retail_script_test.cpp)
target_link_libraries(retail_script_test PRIVATE ta-formats)
# Its single main() holds many World locals (~1.9 MB of frame), past MinGW's
# default 2 MB stack; Linux and macOS give 8 MB.
if(MINGW)
    target_link_options(retail_script_test PRIVATE -Wl,--stack,16777216)
endif()
add_executable(vfs_names_test tools/vfs_names_test.cpp)
target_link_libraries(vfs_names_test PRIVATE ta-formats)
add_test(NAME vfs_names COMMAND vfs_names_test)
add_library(ta-gpustats STATIC src/util/nvml.cpp src/util/windowsgpu.cpp)
target_include_directories(ta-gpustats PUBLIC src)
target_link_libraries(ta-gpustats PRIVATE ${CMAKE_DL_LIBS})
if(WIN32)
  target_link_libraries(ta-gpustats PRIVATE pdh dxgi)
endif()
add_executable(nvml_test tools/nvml_test.cpp)
target_link_libraries(nvml_test PRIVATE ta-gpustats)
add_test(NAME nvml COMMAND nvml_test)
add_executable(windowsgpu_test tools/windowsgpu_test.cpp)
target_link_libraries(windowsgpu_test PRIVATE ta-gpustats)
add_test(NAME windowsgpu COMMAND windowsgpu_test)
add_executable(weapon_hidden_release_test tools/weapon_hidden_release_test.cpp)
target_link_libraries(weapon_hidden_release_test PRIVATE ta-formats)
add_test(NAME weapon_hidden_release COMMAND weapon_hidden_release_test)
add_executable(retail_trace_test tools/retail_trace_test.cpp)
target_link_libraries(retail_trace_test PRIVATE ta-formats)
add_executable(retail_scheduler_test tools/retail_scheduler_test.cpp)
target_link_libraries(retail_scheduler_test PRIVATE ta-formats)
add_executable(retail_ai_test tools/retail_ai_test.cpp)
target_link_libraries(retail_ai_test PRIVATE ta-formats)
add_test(NAME retail_ai COMMAND retail_ai_test)
add_executable(map_transfer_test tools/map_transfer_test.cpp)
target_link_libraries(map_transfer_test PRIVATE ta-formats)
# MAP SCENARIO (protocol 259): the map scenario inspector, and its agreement with setupMatch.
add_executable(unit_status_test tools/unit_status_test.cpp)
target_link_libraries(unit_status_test PRIVATE ta-formats)
add_test(NAME unit_status COMMAND unit_status_test)
add_executable(kill_credit_test tools/kill_credit_test.cpp)
target_link_libraries(kill_credit_test PRIVATE ta-formats)
add_test(NAME kill_credit COMMAND kill_credit_test)
add_executable(unit_retirement_test tools/unit_retirement_test.cpp)
target_link_libraries(unit_retirement_test PRIVATE ta-formats)
add_test(NAME unit_retirement COMMAND unit_retirement_test)
add_executable(remote_timing_test tools/remote_timing_test.cpp)
target_link_libraries(remote_timing_test PRIVATE ta-formats)
add_test(NAME remote_timing COMMAND remote_timing_test)
add_executable(scenario_resources_test tools/scenario_resources_test.cpp)
target_link_libraries(scenario_resources_test PRIVATE ta-formats)
add_test(NAME scenario_resources COMMAND scenario_resources_test)

add_executable(mapgen_test tools/mapgen_test.cpp)
target_link_libraries(mapgen_test PRIVATE ta-formats)
# Seam hunt: generated tile boundaries against retail adjacency and pixel steps.
add_executable(mapgen_seams tools/mapgen_seams.cpp)
target_link_libraries(mapgen_seams PRIVATE ta-formats)
add_test(NAME mapgen COMMAND mapgen_test)

add_executable(terrainlift_test tools/terrainlift_test.cpp)
target_include_directories(terrainlift_test PRIVATE src)
add_test(NAME terrainlift COMMAND terrainlift_test)
add_executable(order_trail_test tools/order_trail_test.cpp src/client/ordertrail.cpp)
target_link_libraries(order_trail_test PRIVATE ta-formats)
add_test(NAME order_trail COMMAND order_trail_test)
# Retail's Shift range circles and area-box colours (client/orderoverlay.h); --rows/--ring
# feed tools/re/check_range_circles.py, which runs 0x4d6530/0x4d5c40 under emulation.
add_executable(range_circles_test tools/range_circles_test.cpp)
target_include_directories(range_circles_test PRIVATE src)
add_test(NAME range_circles COMMAND range_circles_test)
add_executable(reclaim_pick_test tools/reclaim_pick_test.cpp)
target_link_libraries(reclaim_pick_test PRIVATE ta-formats)
add_test(NAME reclaim_pick COMMAND reclaim_pick_test)
add_executable(area_drag_test tools/area_drag_test.cpp)
target_include_directories(area_drag_test PRIVATE src)
add_test(NAME area_drag COMMAND area_drag_test)
add_executable(buildline_test tools/buildline_test.cpp)
target_link_libraries(buildline_test PRIVATE ta-formats)
add_test(NAME buildline COMMAND buildline_test)
add_executable(navigation_checkpoint_test tools/navigation_checkpoint_test.cpp)
target_link_libraries(navigation_checkpoint_test PRIVATE ta-formats)
# Legion mission goals (attack, chase, guard, fight-move, patrol, group patrol,
# repair and build approaches).
add_test(NAME legion_movement_orders COMMAND movement_orders_test --legion)
# Legion work/logistics goals: production exits and rallies, boarding, unload
# approaches and PARK rings (each asserts Legion served the leg).
add_test(NAME legion_transport COMMAND transport_test --legion)
add_executable(retail_movement_animation_test tools/retail_movement_animation_test.cpp)
target_link_libraries(retail_movement_animation_test PRIVATE ta-formats)
add_test(NAME retail_movement_animation COMMAND retail_movement_animation_test)
add_executable(retail_builder_animation_test tools/retail_builder_animation_test.cpp)
target_link_libraries(retail_builder_animation_test PRIVATE ta-formats)
add_test(NAME retail_builder_animation COMMAND retail_builder_animation_test)
add_executable(gaf_rle_test tools/gaf_rle_test.cpp)
target_link_libraries(gaf_rle_test PRIVATE ta-formats)
add_test(NAME gaf_rle COMMAND gaf_rle_test)
# The JPEG writer (util/jpeg.h, the Benchmark results' SAVE AS JPG) on synthetic pictures.
add_executable(jpeg_encode_test tools/jpeg_encode_test.cpp)
target_link_libraries(jpeg_encode_test PRIVATE ta-formats)
add_test(NAME jpeg_encode COMMAND jpeg_encode_test)
# The CPU detection behind SIMULATION THREADS (sim/simcpus.h) on the machine it runs on:
# prints what it found and pins the calling thread to the first CPU (macOS: checks that
# it reports it cannot). The system's own answer is required: the fallback of plain
# cores fails. CI runs it on Linux, macOS and Windows.
add_executable(cpu_topology_test tools/cpu_topology_test.cpp)
target_link_libraries(cpu_topology_test PRIVATE ta-formats)
add_test(NAME cpu_topology COMMAND cpu_topology_test)
add_executable(retail_construction_test tools/retail_construction_test.cpp)
target_link_libraries(retail_construction_test PRIVATE ta-formats)
add_test(NAME retail_construction COMMAND retail_construction_test)
add_executable(retail_mission_test tools/retail_mission_test.cpp)
target_link_libraries(retail_mission_test PRIVATE ta-formats)
add_test(NAME retail_mission COMMAND retail_mission_test)
add_executable(navincr_test tools/navincr_test.cpp)
add_executable(simpool_test tools/simpool_test.cpp)
target_link_libraries(simpool_test PRIVATE ta-formats)
add_executable(fixed_test tools/fixed_test.cpp)
add_executable(statsfit_test tools/statsfit_test.cpp)
add_executable(settings_test tools/settings_test.cpp)
add_executable(start_markers_test tools/start_markers_test.cpp)
add_executable(texture_filter_test tools/texture_filter_test.cpp)
add_executable(hidden_units_test tools/hidden_units_test.cpp)
add_executable(reclaim_fade_test tools/reclaim_fade_test.cpp)
add_executable(gpuvram_test tools/gpuvram_test.cpp)
    target_link_libraries(retailgap_test PRIVATE ta-formats)
target_link_libraries(conjure_test PRIVATE ta-formats)
target_link_libraries(turn_test PRIVATE ta-formats)
target_link_libraries(placement_test PRIVATE ta-formats)
target_link_libraries(cursor_enemy_test PRIVATE ta-formats)
target_link_libraries(friendly_order_test PRIVATE ta-formats)
target_link_libraries(navblock_test PRIVATE ta-formats)
target_link_libraries(setup_reachability_test PRIVATE ta-formats)
target_link_libraries(blocked_shot_test PRIVATE ta-formats)
target_link_libraries(minrange_backoff_test PRIVATE ta-formats)
target_link_libraries(ballistic_line_test PRIVATE ta-formats)
target_link_libraries(shot_range_test PRIVATE ta-formats)
target_link_libraries(attack_stop_test PRIVATE ta-formats)
target_link_libraries(suppress_test PRIVATE ta-formats)
target_link_libraries(vermage_fight_test PRIVATE ta-formats)
target_link_libraries(target_search_test PRIVATE ta-formats)
target_link_libraries(sweetspot_test PRIVATE ta-formats)
target_link_libraries(reclaimarea_test PRIVATE ta-formats)
target_link_libraries(revive_order_test PRIVATE ta-formats)
target_link_libraries(campaign_test PRIVATE ta-formats)
target_link_libraries(pathsearch_test PRIVATE ta-formats)
target_link_libraries(pathgeom_test PRIVATE ta-formats)
target_link_libraries(ambush_test PRIVATE ta-formats)
target_link_libraries(mission_orders_test PRIVATE ta-formats)
target_link_libraries(cadence_test PRIVATE ta-formats)
target_link_libraries(crowdbench PRIVATE ta-formats)
target_link_libraries(cobanim_test PRIVATE ta-formats)
target_link_libraries(cmdflow_test PRIVATE ta-formats)
target_link_libraries(conn_test PRIVATE ta-formats)
target_link_libraries(featgen_test PRIVATE ta-formats)
target_link_libraries(featdie_test PRIVATE ta-formats)
target_link_libraries(transport_test PRIVATE ta-formats)
target_link_libraries(queued_load_test PRIVATE ta-formats)
target_link_libraries(queue_modifiers_test PRIVATE ta-formats)
target_link_libraries(queued_target_test PRIVATE ta-formats)
target_link_libraries(replay_test PRIVATE ta-formats)
target_link_libraries(pathblock_test PRIVATE ta-formats)
target_link_libraries(navincr_test PRIVATE ta-formats)
target_link_libraries(fixed_test PRIVATE ta-formats)
target_link_libraries(statsfit_test PRIVATE ta-formats)
target_link_libraries(settings_test PRIVATE ta-formats)
target_link_libraries(start_markers_test PRIVATE ta-formats)
target_link_libraries(texture_filter_test PRIVATE ta-formats)
target_link_libraries(hidden_units_test PRIVATE ta-formats)
target_link_libraries(reclaim_fade_test PRIVATE ta-formats)
target_link_libraries(gpuvram_test PRIVATE ta-formats)

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
add_test(NAME fp_contract COMMAND fp_contract_test)
add_test(NAME sim_driver_equiv COMMAND sim_driver_equiv_test)
add_test(NAME pathsearch COMMAND pathsearch_test)
add_test(NAME retail_scheduler COMMAND retail_scheduler_test)
add_test(NAME retail_trace COMMAND retail_trace_test --selftest)
add_test(NAME retail_search_scratch COMMAND retail_search_scratch_test)
add_test(NAME retail_group COMMAND retail_group_test)
add_test(NAME auth COMMAND authtest)
add_test(NAME unit_gift COMMAND unit_gift_test)
add_test(NAME cmdflow COMMAND cmdflow_test)
add_test(NAME reclaimarea COMMAND reclaimarea_test)
# Raising the dead is an order (Cmd::Resurrect, area-reclaim children); nothing
# raises or clears a body unordered (0.9.12).
add_test(NAME revive_order COMMAND revive_order_test)
add_test(NAME campaign COMMAND campaign_test)
add_test(NAME conn COMMAND conn_test)
add_test(NAME featgen COMMAND featgen_test)
add_test(NAME transport COMMAND transport_test)
add_test(NAME queued_load COMMAND queued_load_test)
add_test(NAME legion_queued_load COMMAND queued_load_test --legion)
add_test(NAME explode_capture COMMAND cobanim_test --explode-capture)
add_test(NAME queue_modifiers COMMAND queue_modifiers_test)
add_test(NAME legion_queue_modifiers COMMAND queue_modifiers_test --legion)
add_test(NAME queued_target COMMAND queued_target_test)
add_test(NAME legion_queued_target COMMAND queued_target_test --legion)
add_test(NAME replay COMMAND replay_test)
add_test(NAME pathblock COMMAND pathblock_test)
add_test(NAME movement_orders COMMAND movement_orders_test)
set_tests_properties(movement_orders PROPERTIES TIMEOUT 180)
add_test(NAME cadence COMMAND cadence_test)
add_test(NAME navincr COMMAND navincr_test)
add_test(NAME fixed COMMAND fixed_test)
add_test(NAME simpool COMMAND simpool_test)
add_test(NAME settings COMMAND settings_test)
add_test(NAME start_markers COMMAND start_markers_test)
add_test(NAME texture_filter COMMAND texture_filter_test)
add_test(NAME hidden_units_policy COMMAND hidden_units_test)
add_test(NAME reclaim_fade COMMAND reclaim_fade_test)
add_test(NAME gpuvram COMMAND gpuvram_test)
add_test(NAME door_animation COMMAND door_animation_test)
add_test(NAME pathgeom COMMAND pathgeom_test)
add_test(NAME ambush COMMAND ambush_test)
add_executable(exploration_parallel_test tools/exploration_parallel_test.cpp)
target_link_libraries(exploration_parallel_test PRIVATE ta-formats)
add_test(NAME exploration_parallel COMMAND exploration_parallel_test)
# Current-view count deltas (src/sim/retailviewcounts.h) against retail's
# full removal and re-add: the audit probe, footprint level, World level.
add_executable(viewcount_delta_test tools/viewcount_delta_test.cpp)
target_link_libraries(viewcount_delta_test PRIVATE ta-formats)
add_test(NAME viewcount_delta_probe COMMAND viewcount_delta_test probe)
add_test(NAME viewcount_delta_moves COMMAND viewcount_delta_test moves)
add_test(NAME viewcount_delta_world COMMAND viewcount_delta_test world)
# The same footprint updates of a real situation (or generated ones) replayed
# both ways, timed (tools/viewcount_bench.cpp; a timing tool, not a test).
add_executable(state_hash_test tools/state_hash_test.cpp)
target_link_libraries(state_hash_test PRIVATE ta-formats)
add_test(NAME state_hash COMMAND state_hash_test)
add_executable(simpool_passes_test tools/simpool_passes_test.cpp)
target_link_libraries(simpool_passes_test PRIVATE ta-formats)
add_test(NAME simpool_passes COMMAND simpool_passes_test)
add_executable(power_codes_test tools/power_codes_test.cpp)
target_link_libraries(power_codes_test PRIVATE ta-formats)
add_test(NAME power_codes COMMAND power_codes_test)
add_executable(vision_rules_test tools/vision_rules_test.cpp)
target_link_libraries(vision_rules_test PRIVATE ta-formats)
add_test(NAME vision_rules COMMAND vision_rules_test)
add_test(NAME statsfit COMMAND statsfit_test)
add_executable(systemcpu_test tools/systemcpu_test.cpp)
target_include_directories(systemcpu_test PRIVATE src)
add_test(NAME systemcpu COMMAND systemcpu_test)
add_executable(gaitpace_test tools/gaitpace_test.cpp)
target_link_libraries(gaitpace_test PRIVATE ta-formats)
add_test(NAME gaitpace COMMAND gaitpace_test)
add_executable(swapshow_test tools/swapshow_test.cpp)
target_link_libraries(swapshow_test PRIVATE ta-formats)
add_test(NAME swapshow COMMAND swapshow_test)
add_executable(animation_roster_test tools/animation_roster_test.cpp)
target_link_libraries(animation_roster_test PRIVATE ta-formats)
add_executable(structure_combat_test tools/structure_combat_test.cpp)
target_link_libraries(structure_combat_test PRIVATE ta-formats)
add_executable(seadragon_hold_test tools/seadragon_hold_test.cpp)
target_link_libraries(seadragon_hold_test PRIVATE ta-formats)
add_executable(hold_flyer_test tools/hold_flyer_test.cpp)
target_link_libraries(hold_flyer_test PRIVATE ta-formats)
add_executable(naval_combat_test tools/naval_combat_test.cpp)
target_link_libraries(naval_combat_test PRIVATE ta-formats)
add_executable(naval_reach_test tools/naval_reach_test.cpp)
target_link_libraries(naval_reach_test PRIVATE ta-formats)
add_executable(production_exit_test tools/production_exit_test.cpp)
target_link_libraries(production_exit_test PRIVATE ta-formats)
add_executable(doorway_test tools/doorway_test.cpp)
target_link_libraries(doorway_test PRIVATE ta-formats)
add_executable(shipyard_park_test tools/shipyard_park_test.cpp ${TA_AI_SOURCES})
target_link_libraries(shipyard_park_test PRIVATE ta-formats)
# A conjure order the game takes is carried out (the site test at the order, at the
# placement and in the computer player's ring search agree).
add_executable(conjure_site_test tools/conjure_site_test.cpp ${TA_AI_SOURCES})
target_link_libraries(conjure_site_test PRIVATE ta-formats)
add_executable(legion_park_test tools/legion_park_test.cpp)
target_link_libraries(legion_park_test PRIVATE ta-formats)
add_executable(navyyard_exit_test tools/navyyard_exit_test.cpp)
target_link_libraries(navyyard_exit_test PRIVATE ta-formats)
add_executable(factory_activate_test tools/factory_activate_test.cpp)
target_link_libraries(factory_activate_test PRIVATE ta-formats)
add_executable(landdoor_exit_test tools/landdoor_exit_test.cpp)
target_link_libraries(landdoor_exit_test PRIVATE ta-formats)
add_executable(flyer_combat_test tools/flyer_combat_test.cpp)
target_link_libraries(flyer_combat_test PRIVATE ta-formats)
add_executable(air_to_air_test tools/air_to_air_test.cpp)
target_link_libraries(air_to_air_test PRIVATE ta-formats)
add_executable(air_seek_attack_test tools/air_seek_attack_test.cpp)
target_link_libraries(air_seek_attack_test PRIVATE ta-formats)
# Nothing leaves the coordinate range on the largest maps: a 2048-cell map's east and south edges are the top
# of the 16.16 range, where sums of positions saturate (sim/fixed.h). Constructed 2048-cell worlds on synthetic
# units here; the Gryphons' attack runs at those edges are data-gated below.
add_executable(map_edge_test tools/map_edge_test.cpp)
target_link_libraries(map_edge_test PRIVATE ta-formats)
add_test(NAME map_edge COMMAND map_edge_test)
add_executable(capture_combat_test tools/capture_combat_test.cpp)
target_link_libraries(capture_combat_test PRIVATE ta-formats)
add_test(NAME capture_combat COMMAND capture_combat_test)
# Other units' orders, rally steps and weapon slots on a unit end when it changes owner (the
# charm, a gift, a transfer): synthetic types here, the shipped roster data-gated below.
add_executable(owner_orders_test tools/owner_orders_test.cpp)
target_link_libraries(owner_orders_test PRIVATE ta-formats)
add_test(NAME owner_orders COMMAND owner_orders_test)
add_executable(los_status_flight_test tools/los_status_flight_test.cpp)
target_link_libraries(los_status_flight_test PRIVATE ta-formats)
add_test(NAME los_status_flight COMMAND los_status_flight_test)
# Melee attackers on a building walk to Attack_Melee's rectangle round its footprint (the
# melee-*.scn fixtures, both pathfinding modes; data-gated below).
add_executable(melee_building_test tools/melee_building_test.cpp)
target_link_libraries(melee_building_test PRIVATE ta-formats)
# A melee weapon never reaches a flyer in the air (52b980, 412620, 404690; data-gated below).
add_executable(melee_air_test tools/melee_air_test.cpp)
target_link_libraries(melee_air_test PRIVATE ta-formats)
# Flyers step out of a stack (retail's air traffic, the air-stack-*.scn fixtures, both pathfinding modes;
# data-gated below).
add_executable(air_traffic_test tools/air_traffic_test.cpp ${TA_AI_SOURCES})
target_link_libraries(air_traffic_test PRIVATE ta-formats)
set(TA_TEST_DATA "" CACHE PATH "Retail install for the data-backed tests (optional)")
if(TA_TEST_DATA)
    # The data-backed half: a real match start, commands applied directly and
    # through the wire format, hashes equal every tick.
    add_test(NAME sim_driver_equiv_data COMMAND sim_driver_equiv_test "Ulasem Arena" "${TA_TEST_DATA}")
    set_tests_properties(sim_driver_equiv_data PROPERTIES PASS_REGULAR_EXPRESSION "direct == wire"
                         FAIL_REGULAR_EXPRESSION "FAIL")
    add_test(NAME movement_orders_maze COMMAND movement_orders_test "${TA_TEST_DATA}")
    set_tests_properties(movement_orders_maze PROPERTIES TIMEOUT 300)
    add_test(NAME movement_orders_trolls COMMAND movement_orders_test --trolls "${TA_TEST_DATA}")
    # 600 s, like legion_movement_orders_maze: unoptimized it takes 238 s alone (Clang Debug,
    # 2026-10-15) but timed out at 300 s under a parallel -O0 suite; at -O2 it takes ~40 s.
    set_tests_properties(movement_orders_trolls PROPERTIES TIMEOUT 600)
    add_test(NAME legion_movement_orders_maze COMMAND movement_orders_test --legion "${TA_TEST_DATA}")
    set_tests_properties(legion_movement_orders_maze PROPERTIES TIMEOUT 600)
    # Open-ground cohorts, and (separately, serial == workers) the 100-Troll
    # rally cohort on the generated maze.
    add_test(NAME legion_movement_orders_trolls COMMAND movement_orders_test --legion --trolls "${TA_TEST_DATA}" open)
    add_test(NAME legion_movement_orders_trolls_maze COMMAND movement_orders_test --legion --trolls "${TA_TEST_DATA}" maze)
    set_tests_properties(legion_movement_orders_trolls legion_movement_orders_trolls_maze PROPERTIES TIMEOUT 900)
    add_test(NAME map_transfer_data COMMAND map_transfer_test "${TA_TEST_DATA}")
    add_test(NAME mapgen_roster COMMAND mapgen_test "${TA_TEST_DATA}")
    add_test(NAME issue_selection_test_data COMMAND issue_selection_test "${TA_TEST_DATA}")
    add_test(NAME mapgen_mana COMMAND mapgen_test "${TA_TEST_DATA}" --mana)
    set_tests_properties(mapgen mapgen_mana PROPERTIES TIMEOUT 180)
    add_test(NAME mapgen_naval COMMAND mapgen_test "${TA_TEST_DATA}" --naval)
    add_test(NAME mapgen_themes COMMAND mapgen_test "${TA_TEST_DATA}" --themes)
    add_test(NAME mapgen_kits COMMAND mapgen_test "${TA_TEST_DATA}" --kits)
    add_test(NAME mapgen_seams COMMAND mapgen_seams "${TA_TEST_DATA}" sweep 0 1 quick)
    add_test(NAME mapgen_ports COMMAND mapgen_test "${TA_TEST_DATA}" --ports)
    add_test(NAME campaign_roster COMMAND campaign_test "${TA_TEST_DATA}")
    add_test(NAME structure_combat COMMAND structure_combat_test "${TA_TEST_DATA}")
    add_test(NAME structure_combat_reach COMMAND structure_combat_test "${TA_TEST_DATA}" reach)
    add_test(NAME structure_combat_site COMMAND structure_combat_test "${TA_TEST_DATA}" site)
    add_test(NAME seadragon_hold COMMAND seadragon_hold_test "${TA_TEST_DATA}")
    add_test(NAME hold_flyer COMMAND hold_flyer_test "${TA_TEST_DATA}")
    add_test(NAME naval_combat COMMAND naval_combat_test "${TA_TEST_DATA}")
    add_test(NAME legion_naval_combat COMMAND naval_combat_test "${TA_TEST_DATA}" --legion)
    add_test(NAME naval_reach COMMAND naval_reach_test "${TA_TEST_DATA}")
    add_test(NAME legion_naval_reach COMMAND naval_reach_test "${TA_TEST_DATA}" --legion)
    add_test(NAME transport_roster COMMAND transport_test "${TA_TEST_DATA}")
    add_test(NAME transport_post_unload COMMAND transport_test --post-unload "${TA_TEST_DATA}")
    add_test(NAME transport_map_roundtrip COMMAND transport_test
        --surface-transport-map-roundtrip-type "${TA_TEST_DATA}"
        "Lake Lokken" vertrans araarch 240 120 240 350)
    add_test(NAME transport_map_roundtrip_crusades COMMAND transport_test
        --surface-transport-map-roundtrip-type "${TA_TEST_DATA}"
        "Lake Lokken" vertrans araarch 240 120 240 350 1)
    add_test(NAME animation_roster COMMAND animation_roster_test "${TA_TEST_DATA}")
    add_test(NAME production_exit COMMAND production_exit_test "${TA_TEST_DATA}")
    add_test(NAME doorway COMMAND doorway_test "${TA_TEST_DATA}")
    add_test(NAME shipyard_park COMMAND shipyard_park_test "${TA_TEST_DATA}")
    add_test(NAME legion_park COMMAND legion_park_test "${TA_TEST_DATA}")
    add_test(NAME navyyard_exit COMMAND navyyard_exit_test "${TA_TEST_DATA}")
    add_test(NAME factory_activate COMMAND factory_activate_test "${TA_TEST_DATA}")
    add_test(NAME landdoor_exit COMMAND landdoor_exit_test "${TA_TEST_DATA}")
    add_test(NAME flyer_combat COMMAND flyer_combat_test "${TA_TEST_DATA}")
    add_test(NAME featdie COMMAND featdie_test "${TA_TEST_DATA}")
    add_test(NAME air_to_air COMMAND air_to_air_test "${TA_TEST_DATA}")
    add_test(NAME air_seek_attack COMMAND air_seek_attack_test "${TA_TEST_DATA}")
    add_test(NAME map_edge_gryphons COMMAND map_edge_test "${TA_TEST_DATA}")
    add_test(NAME capture_combat_roster COMMAND capture_combat_test "${TA_TEST_DATA}")
    add_test(NAME owner_orders_roster COMMAND owner_orders_test "${TA_TEST_DATA}")
    add_test(NAME los_status_flight_roster COMMAND los_status_flight_test "${TA_TEST_DATA}")
    add_test(NAME melee_building COMMAND melee_building_test "${TA_TEST_DATA}" "${CMAKE_SOURCE_DIR}/tools/scenarios")
    add_test(NAME melee_air COMMAND melee_air_test "${TA_TEST_DATA}")
    add_test(NAME air_traffic COMMAND air_traffic_test "${TA_TEST_DATA}" "${CMAKE_SOURCE_DIR}/tools/scenarios")
    add_test(NAME gate_scripts COMMAND retail_script_test --gates "${TA_TEST_DATA}")
    add_test(NAME scenery_damage COMMAND retail_script_test --features "${TA_TEST_DATA}")
    set_tests_properties(animation_roster explode_capture PROPERTIES
        ENVIRONMENT "TA_VERIFY_NATIVE_EXPORT=1")
    # Loose extracted scripts; left out (not failed) where they were never extracted.
    if(EXISTS "${TA_TEST_EXTRACTED}/scripts")
      add_test(NAME cobanim COMMAND cobanim_test "${TA_TEST_EXTRACTED}/scripts")
      add_test(NAME gaitpace_shipped COMMAND gaitpace_test "${TA_TEST_EXTRACTED}/scripts")
      set_tests_properties(cobanim PROPERTIES ENVIRONMENT "TA_VERIFY_NATIVE_EXPORT=1")
    endif()
    # One test per slice so they run side by side. The slices take more than 5 s, but
    # the retail-gap checks cover too much to leave out of a targeted run, so they are
    # labelled `quick` here (the rule below only ADDS the label).
    foreach(retailgap_slice weapons rules fireatwill orders)
      add_test(NAME retailgap_${retailgap_slice} COMMAND retailgap_test "${TA_TEST_DATA}" ${retailgap_slice})
      set_tests_properties(retailgap_${retailgap_slice} PROPERTIES LABELS quick)
    endforeach()
    add_test(NAME mission_orders_shipped COMMAND mission_orders_test --data "${TA_TEST_DATA}")
    add_test(NAME conjure COMMAND conjure_test "${TA_TEST_DATA}")
    add_test(NAME conjure_site COMMAND conjure_site_test "${TA_TEST_DATA}")
    add_test(NAME legion_conjure_site COMMAND conjure_site_test "${TA_TEST_DATA}" --legion)
    set_tests_properties(legion_conjure_site PROPERTIES LABELS legion)
    add_test(NAME death_lifecycle COMMAND death_lifecycle_test "${TA_TEST_DATA}")
    add_test(NAME kill_credit_shipped COMMAND kill_credit_test "${TA_TEST_DATA}")
    add_test(NAME building_death COMMAND building_death_test "${TA_TEST_DATA}")
    add_test(NAME clear_build COMMAND clear_build_test "${TA_TEST_DATA}")
    add_test(NAME turn COMMAND turn_test "${TA_TEST_DATA}")
    add_test(NAME navblock COMMAND navblock_test "${TA_TEST_DATA}")
    add_test(NAME setup_reachability COMMAND setup_reachability_test "${TA_TEST_DATA}")
    add_test(NAME blocked_shot COMMAND blocked_shot_test "${TA_TEST_DATA}")
    add_test(NAME minrange_backoff COMMAND minrange_backoff_test "${TA_TEST_DATA}")
    add_test(NAME sweetspot COMMAND sweetspot_test "${TA_TEST_DATA}")
    add_test(NAME legion_blocked_shot COMMAND blocked_shot_test "${TA_TEST_DATA}" --legion)
    set_tests_properties(legion_blocked_shot PROPERTIES LABELS legion)
    add_test(NAME ballistic_line COMMAND ballistic_line_test "${TA_TEST_DATA}")
    add_test(NAME legion_ballistic_line COMMAND ballistic_line_test "${TA_TEST_DATA}" --legion)
    set_tests_properties(legion_ballistic_line PROPERTIES LABELS legion)
    add_test(NAME shot_range COMMAND shot_range_test "${TA_TEST_DATA}")
    add_test(NAME legion_shot_range COMMAND shot_range_test "${TA_TEST_DATA}" --legion)
    set_tests_properties(legion_shot_range PROPERTIES LABELS legion)
    add_test(NAME attack_stop COMMAND attack_stop_test "${TA_TEST_DATA}")
    add_test(NAME legion_attack_stop COMMAND attack_stop_test "${TA_TEST_DATA}" --legion)
    set_tests_properties(legion_attack_stop PROPERTIES LABELS legion)
    add_test(NAME suppress COMMAND suppress_test "${TA_TEST_DATA}")
    add_test(NAME legion_suppress COMMAND suppress_test "${TA_TEST_DATA}" --legion)
    set_tests_properties(legion_suppress PROPERTIES LABELS legion)
    add_test(NAME vermage_fight COMMAND vermage_fight_test "${TA_TEST_DATA}")
    add_test(NAME legion_vermage_fight COMMAND vermage_fight_test "${TA_TEST_DATA}" --legion)
    set_tests_properties(legion_vermage_fight PROPERTIES LABELS legion)
    add_test(NAME idle_coverage COMMAND target_search_test "${TA_TEST_DATA}" idle_coverage)
    add_test(NAME legion_idle_coverage COMMAND target_search_test "${TA_TEST_DATA}" idle_coverage --legion)
    set_tests_properties(legion_idle_coverage PROPERTIES LABELS legion)
    add_test(NAME target_search_missions COMMAND target_search_test "${TA_TEST_DATA}" mission_ports)
    add_test(NAME legion_target_search_missions COMMAND target_search_test "${TA_TEST_DATA}" mission_ports --legion)
    set_tests_properties(legion_target_search_missions PROPERTIES LABELS legion)
    add_test(NAME target_search_attack_move COMMAND target_search_test "${TA_TEST_DATA}" attack_move)
    add_test(NAME legion_target_search_attack_move COMMAND target_search_test "${TA_TEST_DATA}" attack_move --legion)
    set_tests_properties(legion_target_search_attack_move PROPERTIES LABELS legion)
    add_test(NAME target_search_shared_sight COMMAND target_search_test "${TA_TEST_DATA}" shared_sight)
    add_test(NAME legion_target_search_shared_sight COMMAND target_search_test "${TA_TEST_DATA}" shared_sight --legion)
    set_tests_properties(legion_target_search_shared_sight PROPERTIES LABELS legion)
    add_test(NAME placement COMMAND placement_test "${TA_TEST_DATA}")
    add_test(NAME cursor_enemy COMMAND cursor_enemy_test "${TA_TEST_DATA}")
    add_test(NAME friendly_order_data COMMAND friendly_order_test "${TA_TEST_DATA}")
    # The Release client intentionally strips the local game/dev command path.
    # Keep this headless client integration fixture in Debug configurations.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_test(NAME idle_builders COMMAND taclient game "Ulasem Arena"
            --data "${TA_TEST_DATA}" --testbuild --time 0
            --shot "${CMAKE_CURRENT_BINARY_DIR}/idle-builders-test.png")
        set_tests_properties(idle_builders PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_IDLE_BUILDERS_TEST=1"
            PASS_REGULAR_EXPRESSION "PASS: idle builders strip"
            TIMEOUT 60)
        add_test(NAME diplomacy_ui COMMAND taclient game "Ulasem Arena"
            --data "${TA_TEST_DATA}" --testbuild --time 0
            --shot "${CMAKE_CURRENT_BINARY_DIR}/diplomacy-test.png")
        set_tests_properties(diplomacy_ui PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_DIPLOMACY_TEST=1"
            PASS_REGULAR_EXPRESSION "PASS: diplomacy hotkey, roster, defaults, checkboxes, voice column, volume sliders"
            TIMEOUT 60)
        # The result plate's buttons play their .gui clicks (defeat.gui / victory<race>.gui:
        # MainMenu cancel.wav, Proceed ok.wav) on the press, like the title menu's.
        foreach(result_press IN ITEMS menu proceed)
            set(result_env "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_RESULT_PRESS=${result_press};TA_SHOT_RESULT=${CMAKE_CURRENT_BINARY_DIR}/result-click-${result_press}.png;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/result-click-profile;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/result-click-profile;TA_SETTINGS_FILE=${CMAKE_CURRENT_BINARY_DIR}/result-click-profile/settings.ini")
            if(result_press STREQUAL "menu")
                set(result_pass "result click: choice=0 sound=cancel.wav queued=1")
            else()
                list(APPEND result_env "TA_SHOT_RESULT_LOSE=1")   # a lost mission's Proceed is Retry
                set(result_pass "result click: choice=1 sound=ok.wav queued=1")
            endif()
            add_test(NAME result_click_${result_press} COMMAND taclient --data "${TA_TEST_DATA}")
            set_tests_properties(result_click_${result_press} PROPERTIES ENVIRONMENT "${result_env}"
                PASS_REGULAR_EXPRESSION "${result_pass}" TIMEOUT 60)
        endforeach()
        add_test(NAME retirement_ui COMMAND taclient game "Ulasem Arena"
            --data "${TA_TEST_DATA}" --testbuild --time 0
            --shot "${CMAKE_CURRENT_BINARY_DIR}/retirement-ui.png")
        set_tests_properties(retirement_ui PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_DIPLOMACY_TEST=1;TA_RETIREMENT_TEST=1"
            PASS_REGULAR_EXPRESSION "PASS: diplomacy hotkey, roster, defaults, checkboxes, voice column, volume sliders"
            TIMEOUT 60)
        add_test(NAME script_transient COMMAND taclient game "ulasem arena"
            --data "${TA_TEST_DATA}" --firetest --nofog --time 0.1
            --shot "${CMAKE_CURRENT_BINARY_DIR}/script-transient-test.png")
        set_tests_properties(script_transient PROPERTIES ENVIRONMENT
            "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_SCRIPT_TRANSIENT_TEST=1")
        add_test(NAME script_transient_263 COMMAND taclient game "Lake Lokken"
            --data "${TA_TEST_DATA}" --firetest --nofog --time 0.1
            --shot "${CMAKE_CURRENT_BINARY_DIR}/script-transient-263-test.png")
        set_tests_properties(script_transient_263 PROPERTIES ENVIRONMENT
            "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_SCRIPT_263_TEST=1")
        # Killed/Dying sounds as the simulation chose them, through the client's
        # sound drain: exactly per death, never doubled, fog/class gated, at the
        # authored cry rates and variants for every Creon caller.
        add_test(NAME creon_death_sound COMMAND taclient game "Ulasem Arena"
            --data "${TA_TEST_DATA}" --firetest --time 0.1
            --shot "${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-test.png")
        set_tests_properties(creon_death_sound PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_CREON_DEATH_SOUND_TEST=1;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-profile;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-profile;TA_SETTINGS_FILE=${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-profile/settings.ini"
            PASS_REGULAR_EXPRESSION "PASS: Creon death sounds are the simulation's"
            TIMEOUT 120)
        # The same game again beside a peer that presents every batch late (a
        # rejoin or spectator catch-up): both must hear the same death sounds.
        add_test(NAME creon_death_sound_peers COMMAND ${CMAKE_COMMAND} "-DCLIENT=$<TARGET_FILE:taclient>"
            "-DDATA=${TA_TEST_DATA}" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-peers"
            -P "${CMAKE_SOURCE_DIR}/tools/creon_death_sound.cmake")
        set_tests_properties(creon_death_sound_peers PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-peers-profile;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-peers-profile;TA_SETTINGS_FILE=${CMAKE_CURRENT_BINARY_DIR}/creon-death-sound-peers-profile/settings.ini"
            PASS_REGULAR_EXPRESSION "PASS: both peers hear the simulation's Creon death sounds"
            TIMEOUT 240)
        add_test(NAME weapon_impact_effect_route COMMAND taclient game "Lake Lokken"
            --data "${TA_TEST_DATA}" --firetest --nofog --time 0.1
            --shot "${CMAKE_CURRENT_BINARY_DIR}/weapon-impact-effect-test.png")
        set_tests_properties(weapon_impact_effect_route PROPERTIES
            ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;TA_WEAPON_IMPACT_EFFECT_TEST=1"
            PASS_REGULAR_EXPRESSION "PASS: Arapult land/water authored impact variants"
            TIMEOUT 120)
    endif()
endif()
# Data-gated Legion scenarios (need the install; label data;nightly). The motion
# cases are tools/legion_group_motion's open/wall/cross with 12 real unit types,
# wanderers off (motion-wall-wanderers-on reproduces the old tool's confound);
# the gen1 routes are the seven big army orders of the legion-r9 replay on the
# user's generated map.
if(TA_TEST_DATA)
    foreach(legion_motion open wall cross)
        add_test(NAME legion_motion_${legion_motion}
                 COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/motion-${legion_motion}.scn
                         --data "${TA_TEST_DATA}" --json)
        set_tests_properties(legion_motion_${legion_motion} PROPERTIES
                             SKIP_RETURN_CODE 77 LABELS "data;nightly" TIMEOUT 600)
    endforeach()
    add_test(NAME legion_motion_wall_wanderers_on
             COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/motion-wall.scn
                     --data "${TA_TEST_DATA}" --wanderers on --json)
    set_tests_properties(legion_motion_wall_wanderers_on PROPERTIES
                         SKIP_RETURN_CODE 77 LABELS "data;nightly" TIMEOUT 600)
    # Melee attackers on a building (protocol 307, Attack_Melee's rectangle round the footprint): every key
    # against baseline.json over the gate offsets, both modes. melee_building gates the outcome per commit.
    foreach(legion_scn melee-lode-late melee-lode-together melee-castle-4 melee-keep-late melee-castle-fight
                       melee-castle-20)
        if(NOT WIN32)   # gate modes need python3 and a POSIX shell (legion_scenario's runChecker)
          add_test(NAME legion_check_${legion_scn}
                   COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both
                           --data "${TA_TEST_DATA}" --check ${CMAKE_SOURCE_DIR}/tools/scenarios/baseline.json)
          set_tests_properties(legion_check_${legion_scn} PROPERTIES
                               SKIP_RETURN_CODE 77 LABELS "legion;data;nightly" TIMEOUT 600)
        endif()
    endforeach()
    # Flyers stepping out of a stack (protocol 307, retail's air traffic): the same, for the air-stack-*.scn
    # fixtures and air-guard-lost.scn (VTOL_SeekGuard). air_traffic gates the outcome per commit.
    foreach(legion_scn air-stack-barracks air-stack-barracks-5 air-stack-duel air-stack-move air-stack-fight
                       air-stack-fight-cell air-stack-patrol air-stack-repair air-stack-help air-guard-lost)
        if(NOT WIN32)   # gate modes need python3 and a POSIX shell (legion_scenario's runChecker)
          add_test(NAME legion_check_${legion_scn}
                   COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/${legion_scn}.scn --mode both
                           --data "${TA_TEST_DATA}" --check ${CMAKE_SOURCE_DIR}/tools/scenarios/baseline.json)
          set_tests_properties(legion_check_${legion_scn} PROPERTIES
                               SKIP_RETURN_CODE 77 LABELS "legion;data;nightly" TIMEOUT 600)
        endif()
    endforeach()
    # Harvested situations (tools/scenarios/situations/README.txt): not faithful enough to gate on, so these only
    # check that each file loads and runs deterministically, serial == workers, in both modes.
    # Each of these is one of the longest tests in the suite (50-150 s), so it is split into independent
    # parts that run in parallel: legion_scenario checks serial == workers per (mode, offset) and gates
    # nothing across them, so the union of the parts is exactly the unsplit run.
    foreach(legion_situation r9-36060 r9-65850 r9-61930 r10-46292 r10-87332 r10-104540)
        foreach(legion_part legion retail)
            add_test(NAME legion_situation_${legion_situation}_${legion_part}
                     COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/situations/situation-${legion_situation}.scn
                             --data "${TA_TEST_DATA}" --offsets 0 --mode ${legion_part} --json)
            set_tests_properties(legion_situation_${legion_situation}_${legion_part} PROPERTIES
                                 SKIP_RETURN_CODE 77 LABELS "data;nightly" TIMEOUT 1800)
        endforeach()
    endforeach()
    foreach(legion_route 21096 27459 36057 45941 54772 61923 65845)
        foreach(legion_part 0 1 -1)
            string(REPLACE "-" "m" legion_part_name "${legion_part}")
            add_test(NAME legion_gen1_route_${legion_route}_o${legion_part_name}
                     COMMAND legion_scenario ${CMAKE_SOURCE_DIR}/tools/scenarios/gen1-route-${legion_route}.scn
                             --data "${TA_TEST_DATA}" --offsets ${legion_part} --json)
            set_tests_properties(legion_gen1_route_${legion_route}_o${legion_part_name} PROPERTIES
                                 SKIP_RETURN_CODE 77 LABELS "data;nightly" TIMEOUT 1800)
        endforeach()
    endforeach()
endif()

# aitool bundles the AI (src/ai, otherwise server-only) so we can tune it headless.
add_executable(ai_behavior_test tools/ai_behavior_test.cpp ${TA_AI_SOURCES})
target_link_libraries(ai_behavior_test PRIVATE ta-formats)
if(TA_TEST_DATA)
    add_test(NAME ai_behavior_data COMMAND ai_behavior_test "${TA_TEST_DATA}")
endif()
# Hard's and Absurd's economy and army make-up, and the factory rule Normal
# shares with them, as a real Controller's orders on constructed bases
# (factories, lodestones, deposits, Zhon's producers, limits).
# Our levels' army orders, as a real Controller's commands on constructed worlds;
# with the game data, a Catapult's back-off from its minimum range under the real sim.
add_executable(ai_orders_test tools/ai_orders_test.cpp ${TA_AI_SOURCES})
target_link_libraries(ai_orders_test PRIVATE ta-formats)
if(TA_TEST_DATA)
    add_test(NAME ai_orders_data COMMAND ai_orders_test "${TA_TEST_DATA}")
endif()

# The Classic level's pieces (profile baker, type info, priority clock); with
# --profile it is the native side of tools/re/check_ai_profile.py, with
# --printweights that of tools/re/check_ai_printweights.py.
add_executable(classic_ai_test tools/classic_ai_test.cpp ${TA_AI_SOURCES})
target_link_libraries(classic_ai_test PRIVATE ta-formats)
if(TA_TEST_DATA)
    add_test(NAME classic_ai_data COMMAND classic_ai_test "${TA_TEST_DATA}")
    # A Classic seat of each faction builds its base on Abnar's Terrace (10 game-minutes each).
    add_test(NAME classic_ai_bases COMMAND classic_ai_test --bases "${TA_TEST_DATA}")
    # A scripted raid into a Classic base: defenders sent at the raiders, the Monarch's retreat.
    add_test(NAME classic_ai_raid COMMAND classic_ai_test --raid "${TA_TEST_DATA}")
    # U13: what a Move and an AttackMove do to a column walking through enemies.
    add_test(NAME classic_ai_u13 COMMAND classic_ai_test --u13 "${TA_TEST_DATA}")
    # A Classic seat's waves: launch, march by Move, a member sent home, dissolve.
    add_test(NAME classic_ai_waves COMMAND classic_ai_test --waves "${TA_TEST_DATA}")
    # Classic against Classic: three faction pairs on Edmont's Field end with a winner.
    add_test(NAME classic_ai_duels COMMAND classic_ai_test --duels "${TA_TEST_DATA}")
    # A Classic seat of each faction founds bases on sacred sites and its lodestones grow (20 game-minutes each).
    add_test(NAME classic_ai_expansion COMMAND classic_ai_test --expansion "${TA_TEST_DATA}")
endif()
# Opt-in (it needs the retail binary for emulation, Python's unicorn, and a
# directory of captured retail PrintWeights dumps, none of which CI has):
# -DTAK_CLASSIC_PRINTWEIGHTS=<dumps dir with index.json>.
set(TA_CLASSIC_PRINTWEIGHTS "" CACHE PATH "captured retail PrintWeights dumps for classic_ai_printweights")
if(TA_TEST_DATA AND TA_CLASSIC_PRINTWEIGHTS AND Python3_Interpreter_FOUND)
    add_test(NAME classic_ai_printweights
             COMMAND "${Python3_EXECUTABLE}" ${CMAKE_SOURCE_DIR}/tools/re/check_ai_printweights.py
                     --binary $<TARGET_FILE:classic_ai_test> --data "${TA_TEST_DATA}"
                     --dumps "${TA_CLASSIC_PRINTWEIGHTS}")
endif()
# Opt-in (it needs the retail binary at assets/game and Python's unicorn, which
# CI has neither of): -DTAK_CLASSIC_ORACLES=ON runs the Classic brain's squad
# and production layers against retail's routines in emulation -- the manager
# constructor, the assignment pass, the base think's factory path (with the
# economy step's allocation), the block grids and site search, the
# constructor planner and the static grade its reachability reads, the threat
# test and enemy query, the combat-member planner and the Monarch's retreat,
# the base think's wave launch, the strike, backup and VTOL thinks and the
# target picker, the zone labels, the transfers and the expansion through
# classic_ai_test; the scheduler, squad clocks, nearest-base selection,
# factory planner, menu draw, entry weight and patrol points through the shared
# templates in retail_ai_test.
option(TA_CLASSIC_ORACLES "run the Classic AI emulation oracles (needs KINGDOMS.icd and unicorn)" OFF)
if(TA_CLASSIC_ORACLES AND Python3_Interpreter_FOUND)
    foreach(oracle squad_ctor assignment factory_think placement constructor static_grade threat defence retreat
            launch target_pick zone_labels transfer expand)
        add_test(NAME classic_ai_${oracle}
                 COMMAND "${Python3_EXECUTABLE}" ${CMAKE_SOURCE_DIR}/tools/re/check_ai_${oracle}.py
                         --binary $<TARGET_FILE:classic_ai_test>)
    endforeach()
    foreach(routine strike backup vtol)
        add_test(NAME classic_ai_${routine}
                 COMMAND "${Python3_EXECUTABLE}" ${CMAKE_SOURCE_DIR}/tools/re/check_ai_strike.py
                         --binary $<TARGET_FILE:classic_ai_test> --routine ${routine})
    endforeach()
    foreach(oracle scheduler squads base_selection base_radius factory_plan build_choice build_weight patrol_points)
        add_test(NAME classic_ai_${oracle}
                 COMMAND "${Python3_EXECUTABLE}" ${CMAKE_SOURCE_DIR}/tools/re/check_ai_${oracle}.py
                         --binary $<TARGET_FILE:retail_ai_test>)
    endforeach()
endif()
# The retail AI harness's teardown, against a fake kwin_wayland (no retail, no
# Proton): the display-holder and everything else of the run must be gone.
if(Python3_Interpreter_FOUND AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    endif()

add_executable(aitool tools/aitool.cpp ${TA_AI_SOURCES})
target_link_libraries(aitool PRIVATE ta-formats)
add_executable(naval_ai_test tools/naval_ai_test.cpp ${TA_AI_SOURCES})
target_link_libraries(naval_ai_test PRIVATE ta-formats)
if(TA_TEST_DATA)
    add_test(NAME naval_ai COMMAND naval_ai_test "${TA_TEST_DATA}")
endif()
# Our AI levels' built-in profiles + their per-level game-mod files (ai/builtinprofile.h);
# with data, the built-in Normal table is re-checked against the shipped ai/default.txt.
add_executable(ai_profile_test tools/ai_profile_test.cpp ${TA_AI_SOURCES})
target_link_libraries(ai_profile_test PRIVATE ta-formats)
if(TA_TEST_DATA)
    add_test(NAME ai_profile_data COMMAND ai_profile_test "${TA_TEST_DATA}")
endif()
add_executable(simperf tools/simperf.cpp ${TA_AI_SOURCES})
target_link_libraries(simperf PRIVATE ta-formats)
# Feature burn/fade timing harness (World::tickBurning, tickFeatureFades); not a CTest.
# Microbenchmark of Controller::reviveIdle (not a CTest).
# The stat-aura pass alone (World::tickAuras), for screening its cost: tools/aura_bench.cpp.

# --- Client (needs SDL2; skipped with a warning if unavailable) ---------------

option(TA_BUILD_CLIENT "Build the SDL2 client (asset viewer + game)" ON)

# FFmpeg decodes the retail Bink (.bik) menu videos -- libavcodec carries a clean-room
# Bink decoder (LGPL). It is ALWAYS the minimal Bink and streaming static build from
# tools/build-ffmpeg-bink.sh; there is no dynamic link and no no-video build.
#
# The system libavcodec is never consulted, which is deliberate twice over: stock
# Fedora's libavcodec-free omits the Bink decoder entirely (so "it built" would not mean
# "videos play"), and its .pc files were the last place a shared libatomic could sneak
# back into an otherwise static binary. TA_FFMPEG_PREFIX overrides where the static
# install lives.
set(TA_FFMPEG_PREFIX "" CACHE PATH "Install prefix of the static Bink and streaming FFmpeg")

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  if(NOT TA_FFMPEG_PREFIX)
    set(TA_FFMPEG_PREFIX "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-bink")
  endif()
  if(EXISTS "${TA_FFMPEG_PREFIX}/lib/pkgconfig")
    set(ENV{PKG_CONFIG_PATH} "${TA_FFMPEG_PREFIX}/lib/pkgconfig")
    # Order high-level -> low-level so the static archives resolve left-to-right.
    if(NOT EXISTS "${TA_FFMPEG_PREFIX}/stream-build-version")
      message(FATAL_ERROR "Static FFmpeg needs streaming support; run tools/build-ffmpeg-bink.sh")
    endif()
    unset(__pkg_config_checked_FFMPEG CACHE)
    pkg_check_modules(FFMPEG libavformat libavcodec libswscale libswresample libavutil)
  else()
    message(FATAL_ERROR
      "The static Bink and streaming FFmpeg is missing: ${TA_FFMPEG_PREFIX}\n"
      "  Build it once with:  ./tools/build-ffmpeg-bink.sh\n"
      "(FFmpeg is always linked statically; there is no dynamic build and no "
      "no-video build to fall back to.)")
  endif()
else()
  message(FATAL_ERROR "pkg-config is required to locate the static Bink and streaming FFmpeg")
endif()

# Multiplayer TLS shares the pinned, static OpenSSL distribution FFmpeg's RTMPS
# uses on Linux and macOS. Windows builds the same archives; no SSL DLLs are shipped.
foreach(_tls_lib ssl crypto)
  if(NOT EXISTS "${TA_FFMPEG_PREFIX}/lib/lib${_tls_lib}.a")
    message(FATAL_ERROR "Missing static TLS archive; run tools/build-ffmpeg-bink.sh")
  endif()
endforeach()
add_library(ta_tls STATIC src/net/tls.cpp src/net/revocation.cpp)
target_include_directories(ta_tls PUBLIC src PRIVATE "${TA_FFMPEG_PREFIX}/include")
target_include_directories(ta-formats PRIVATE "${TA_FFMPEG_PREFIX}/include")
target_link_libraries(ta_tls PUBLIC "${TA_FFMPEG_PREFIX}/lib/libssl.a"
  "${TA_FFMPEG_PREFIX}/lib/libcrypto.a" ${CMAKE_DL_LIBS} Threads::Threads)
if(WIN32)
  target_link_libraries(ta_tls PUBLIC crypt32 ws2_32 bcrypt advapi32 user32 gdi32)
elseif(APPLE)
  target_link_libraries(ta_tls PUBLIC "-framework Security" "-framework CoreFoundation")
endif()
target_link_libraries(ta-formats PUBLIC ta_tls)

add_executable(tls_test tools/tls_test.cpp)
target_include_directories(tls_test PRIVATE "${TA_FFMPEG_PREFIX}/include")
target_link_libraries(tls_test PRIVATE ta-formats)
add_test(NAME tls COMMAND tls_test)

# One INTERFACE target both taclient and biktool link.
if(FFMPEG_FOUND)
  add_library(ta_ffmpeg INTERFACE)
  target_compile_definitions(ta_ffmpeg INTERFACE TA_HAVE_FFMPEG)
  target_include_directories(ta_ffmpeg INTERFACE ${FFMPEG_STATIC_INCLUDE_DIRS})
  # Explicit archive paths: a stray .so/.dll import library in the prefix must
  # never silently turn a streaming dependency into a runtime requirement.
  set(_ff_archives "")
  foreach(_flag IN LISTS FFMPEG_STATIC_LDFLAGS)
    if(_flag MATCHES "^-l(avformat|avcodec|avutil|swscale|swresample|x264|ssl|crypto|va|va-drm|drm|vpl)$")
      set(_archive "${TA_FFMPEG_PREFIX}/lib/lib${CMAKE_MATCH_1}.a")
      if(NOT EXISTS "${_archive}")
        message(FATAL_ERROR "Missing static streaming archive ${_archive}; run tools/build-ffmpeg-bink.sh")
      endif()
      list(APPEND _ff_archives "${_archive}")
    else()
      list(APPEND _ff_archives "${_flag}")
    endif()
  endforeach()
  set(FFMPEG_STATIC_LDFLAGS ${_ff_archives})
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
  message(STATUS "FFmpeg: STATIC Bink and streaming build from ${TA_FFMPEG_PREFIX}")
endif()

add_executable(stream_network_test tools/stream_network_test.cpp src/video/stream.cpp src/video/streamcursor.cpp src/video/rgbascale.cpp)
target_compile_definitions(stream_network_test PRIVATE TA_STREAM_TESTING)
target_link_libraries(stream_network_test PRIVATE ta-formats ta_ffmpeg)
# A real RTMPS upload to a local TLS proxy + RTMP sink (no ffmpeg program needed): it
# must stay LIVE. Caught macOS SecureTransport stalling every upload after a few frames;
# also checks certificate refusal and, on macOS, the system trust anchors OpenSSL is given,
# and that a too-slow upload shows as the encoder waiting to send (UPLOAD CAN'T KEEP UP).
# Not on Windows: Schannel ignores the test's ca_file.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND AND NOT WIN32)
  add_test(NAME streaming_rtmps
           COMMAND "${Python3_EXECUTABLE}" ${CMAKE_SOURCE_DIR}/tools/check-stream-rtmps.py $<TARGET_FILE:stream_network_test>)
  set_tests_properties(streaming_rtmps PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
endif()

add_executable(stream_test tools/stream_test.cpp src/video/stream.cpp src/video/streamcursor.cpp src/video/rgbascale.cpp)
target_compile_definitions(stream_test PRIVATE TA_STREAM_TESTING=1)
target_link_libraries(stream_test PRIVATE ta-formats ta_ffmpeg)
add_test(NAME streaming_overload COMMAND stream_test libx264 overload)
add_test(NAME streaming_native COMMAND stream_test libx264 native)
# A 7680x2160 capture into a small 60 FPS stream: the worker's resize keeps pace.
# Real-time encoding of a 7680x2160 capture at 60 fps: meaningless where everything runs
# several times slower (ASan dropped 74 of 105 frames), so optimized builds only.
if(NOT TA_SLOW_TEST_BUILD)
  add_test(NAME streaming_pacing COMMAND stream_test libx264 pacing)
  set_tests_properties(streaming_pacing PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
endif()
add_executable(rgbascale_test tools/rgbascale_test.cpp src/video/rgbascale.cpp)
target_include_directories(rgbascale_test PRIVATE src)
target_link_libraries(rgbascale_test PRIVATE Threads::Threads)
add_test(NAME rgbascale COMMAND rgbascale_test)
# Hardware-cursor stamping into stream frames (no GPU, no FFmpeg): position, scale,
# letterbox mapping, edge clipping and alpha.
add_executable(streamcursor_test tools/streamcursor_test.cpp src/video/streamcursor.cpp)
target_include_directories(streamcursor_test PRIVATE src)
add_test(NAME streamcursor COMMAND streamcursor_test)
# The stream worker end to end: a captured frame plus a cursor -> the stamped picture.
add_test(NAME streaming_cursor COMMAND stream_test libx264 cursor)
set_tests_properties(streaming_cursor PROPERTIES TIMEOUT 60)
# 77: this FFmpeg build predates the local copy's MP4 muxer (rerun build-ffmpeg-bink.sh).

# Linux VA-API goes through tools/vashim.c (the prefix's libva.a), which forwards to the
# system libva.so.2. vashim_test checks the unavailable path against a fake libva.so.2
# and libva-drm.so.2 that have no functions, and that the system loader (not a bundled one) finds a driver
# exporting only __vaDriverInit_1_23. The latter two skip without libva or a render node.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(vashim_fake_va SHARED tools/vashim_fake.cpp)
  target_compile_definitions(vashim_fake_va PRIVATE TA_FAKE_LIBVA)
  set_target_properties(vashim_fake_va PROPERTIES OUTPUT_NAME va SOVERSION 2
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vashim-fake-libva")
  add_library(vashim_fake_va_drm SHARED tools/vashim_fake.cpp)
  target_compile_definitions(vashim_fake_va_drm PRIVATE TA_FAKE_LIBVA)
  set_target_properties(vashim_fake_va_drm PROPERTIES OUTPUT_NAME va-drm SOVERSION 2
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vashim-fake-libva")
  add_library(vashim_fake_driver MODULE tools/vashim_fake.cpp)
  target_compile_definitions(vashim_fake_driver PRIVATE TA_FAKE_DRIVER)
  set_target_properties(vashim_fake_driver PROPERTIES PREFIX "" OUTPUT_NAME takfake_drv_video
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vashim-fake-driver")
  add_executable(vashim_test tools/vashim_test.cpp src/video/stream.cpp src/video/streamcursor.cpp src/video/rgbascale.cpp)
  target_link_libraries(vashim_test PRIVATE ta-formats ta_ffmpeg)
  add_dependencies(vashim_test vashim_fake_va vashim_fake_va_drm vashim_fake_driver)
  add_test(NAME vashim_unavailable COMMAND vashim_test unavailable)
  set_tests_properties(vashim_unavailable PROPERTIES TIMEOUT 60
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/vashim-fake-libva")
  add_test(NAME vashim_system COMMAND vashim_test system)
  add_test(NAME vashim_system_loader COMMAND vashim_test driver "${CMAKE_BINARY_DIR}/vashim-fake-driver")
  set_tests_properties(vashim_system vashim_system_loader PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
endif()

# Streaming REMEMBER KEY: the stream key in the OS credential store (src/client/credstore.h),
# one platform file each. Linux dlopens libsecret at run time, so static release builds gain
# no runtime dependency; without it (or without a Secret Service) the option is greyed out.
if(WIN32)
  set(_ta_credstore src/client/credstore_win.cpp)
elseif(APPLE)
  set(_ta_credstore src/client/credstore_mac.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_ta_credstore src/client/credstore_linux.cpp)
else()
  set(_ta_credstore src/client/credstore_none.cpp)
endif()
add_library(ta-credstore STATIC ${_ta_credstore} src/client/streamkey.cpp)
target_include_directories(ta-credstore PUBLIC src)
target_link_libraries(ta-credstore PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(ta-credstore PUBLIC advapi32)
elseif(APPLE)
  target_link_libraries(ta-credstore PUBLIC "-framework Security" "-framework CoreFoundation")
else()
  target_link_libraries(ta-credstore PUBLIC ${CMAKE_DL_LIBS})
endif()
# Round trip against the real store under a test-only name; exits 77 (skipped) when this
# session has no usable store (CI containers, ssh without a keychain, no Secret Service).
add_executable(credstore_test tools/credstore_test.cpp)
target_link_libraries(credstore_test PRIVATE ta-credstore)
add_test(NAME credstore COMMAND credstore_test)
set_tests_properties(credstore PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
if(TA_BUILD_CLIENT)
  find_package(SDL2 QUIET)
  if(SDL2_FOUND)
    add_executable(taclient src/client/main.cpp src/client/alloccount.cpp src/client/gameview_impl.cpp src/client/gameview_render.cpp src/client/gameview_hud.cpp src/client/gameview_clicks.cpp src/client/gameview_net.cpp src/client/gameview_lobby.cpp src/client/gameview_room.cpp src/client/gameview_mappicker.cpp src/client/roomchanges.cpp src/client/bigmapview.cpp src/cartographer/features.cpp src/client/gameview_benchmark.cpp src/client/gameview_input.cpp src/client/gameview_voice.cpp src/client/gameview_powercodes.cpp src/client/ordertrail.cpp src/client/gameview_unitinfo.cpp src/client/gameview_hidden.cpp src/client/distantmodels.cpp src/client/animatedatlas.cpp src/client/mapview.cpp src/client/terrainchunks.cpp src/client/modelview.cpp src/client/replayfile.cpp src/client/mainmenu.cpp src/client/settings.cpp src/client/options.cpp src/client/hotkeys.cpp src/client/hotkeysscreen.cpp src/client/campaignscreen.cpp src/client/briefingscreen.cpp src/client/resultscreen.cpp src/client/loadscreen.cpp src/client/guiart.cpp src/client/taguiview.cpp src/client/tafrontend.cpp src/client/gameview_tahud.cpp src/client/cursors.cpp src/util/procmetrics.cpp src/util/relaunch.cpp src/video/bink.cpp src/video/stream.cpp src/video/streamcursor.cpp src/video/rgbascale.cpp src/client/streaming.cpp src/client/streamcapture_d3d.cpp)
    # Local dev/test harness (standalone scenario/demo/FFA setup, --testbuild/--firetest...):
    # debug builds only. The file is also wrapped in #ifndef NDEBUG, as are its callers, so
    # this just skips an empty TU when the config's flags define NDEBUG.
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _ta_bt)
    if(NOT "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_ta_bt}}" MATCHES "NDEBUG")
      target_sources(taclient PRIVATE src/client/gameview_devharness.cpp)
    endif()
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
    # The interface's text (client/uitext.h, client/font.h) and its TrueType face
    # (client/uifont.h): Noto Sans Bold, subset to Latin-1 (res/chat-font,
    # CHAT-FONT-LICENSE.txt), embedded as a byte array so nothing new ships beside the
    # binary, and rasterized by stb_truetype (src/vendor/stb). The chat box always uses
    # it; UI FONT = MODERN routes every plain interface text through it too.
    set(TA_CHATFONTS_INC "${CMAKE_BINARY_DIR}/generated/chatfonts.inc")
    set(_ta_chatfont_ttf "${CMAKE_SOURCE_DIR}/res/chat-font/NotoSans-Bold.ttf")
    add_custom_command(OUTPUT "${TA_CHATFONTS_INC}"
        COMMAND ${CMAKE_COMMAND} "-DOUT=${TA_CHATFONTS_INC}" "-DFILES=kNotoSansBold=${_ta_chatfont_ttf}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        DEPENDS "${_ta_chatfont_ttf}" "${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        COMMENT "Embedding the interface font" VERBATIM)
    add_library(ta-uifont STATIC src/client/uifont.cpp src/client/uitext.cpp src/client/font.cpp
        "${TA_CHATFONTS_INC}")
    target_include_directories(ta-uifont PUBLIC src PRIVATE "${CMAKE_BINARY_DIR}/generated")
    target_include_directories(ta-uifont SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src/vendor/stb")
    target_link_libraries(ta-uifont PUBLIC ta-formats ${_ta_sdl2})
    target_link_libraries(taclient PRIVATE ta-uifont)
    # Music decoding and CUSTOM MUSIC (client/musicfile.h): WAV via SDL, MP3/FLAC (dr_libs)
    # and Ogg Vorbis (stb_vorbis), vendored single files. Linked by everything that plays
    # music (sound.h, menumusic.h).
    add_library(ta-music STATIC src/client/musicfile.cpp)
    target_include_directories(ta-music PUBLIC src)
    target_link_libraries(ta-music PUBLIC ta-formats ${_ta_sdl2})
    target_link_libraries(taclient PRIVATE ta-music)
    # The title lettering: Cinzel Bold (res/title-font, TITLE-FONT-LICENSE.txt), embedded
    # the same way for the front end.
    set(TA_TITLEFONT_INC "${CMAKE_BINARY_DIR}/generated/titlefont.inc")
    set(_ta_titlefont_ttf "${CMAKE_SOURCE_DIR}/res/title-font/Cinzel-Bold.ttf")
    add_custom_command(OUTPUT "${TA_TITLEFONT_INC}"
        COMMAND ${CMAKE_COMMAND} "-DOUT=${TA_TITLEFONT_INC}" "-DFILES=kCinzelBold=${_ta_titlefont_ttf}"
                -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        DEPENDS "${_ta_titlefont_ttf}" "${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake"
        COMMENT "Embedding the title font" VERBATIM)
    target_sources(taclient PRIVATE "${TA_TITLEFONT_INC}")
    target_include_directories(taclient PRIVATE "${CMAKE_BINARY_DIR}/generated")
    # The interface font: metrics, measuring, wrapping with it, UTF-8 (invalid bytes too),
    # glyph sheets built once per pixel size and bounded (on SDL's software renderer),
    # and the text API in both UI FONT styles (measure and draw agree, fit, wrap).
    add_executable(uifont_test tools/uifont_test.cpp src/client/settings.cpp)
    target_compile_definitions(uifont_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(uifont_test PRIVATE ta-uifont ${_ta_sdl2})
    add_test(NAME uifont COMMAND uifont_test)
    set_tests_properties(uifont PROPERTIES TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    # MakePoster: the MAKE POSTER hotkey and the streaming RGB PNG writer.
    add_executable(poster_test tools/poster_test.cpp src/client/hotkeys.cpp)
    target_compile_definitions(poster_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(poster_test PRIVATE ta-formats ${_ta_sdl2})
    add_test(NAME poster COMMAND poster_test)
    set_tests_properties(poster PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    # Voice chat (client/voice.h): the engine and the Opus codec, built from a pinned
    # source release (cmake/Opus.cmake). Client only -- taserver relays voice frames as
    # opaque bytes and never links Opus.
    include(cmake/Opus.cmake)
    add_library(ta-voice STATIC src/client/voice.cpp src/client/voicecodec.cpp)
    target_include_directories(ta-voice PUBLIC src)
    target_compile_definitions(ta-voice PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(ta-voice PUBLIC ta-formats ${_ta_sdl2} PRIVATE opus)
    # The codec, jitter buffer, push-to-talk rules, the engine on SDL's disk/dummy audio
    # drivers with synthetic PCM, the VOICE CHAT page (greyed while off) and the settings.
    add_executable(voice_test tools/voice_test.cpp src/client/options.cpp src/client/settings.cpp
      src/client/hotkeys.cpp)
    target_compile_definitions(voice_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(voice_test PRIVATE ta-voice ta-uifont ta-formats ${_ta_sdl2})
    add_test(NAME voice COMMAND voice_test)
    set_tests_properties(voice PROPERTIES TIMEOUT 60 ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    # Options, Voice Chat and Controls save every change as it is made (no SAVE button):
    # a slider on release, the player name when it loses focus, DEFAULTS after YES.
    add_executable(options_autosave_test tools/options_autosave_test.cpp src/client/options.cpp
      src/client/settings.cpp src/client/hotkeys.cpp src/client/hotkeysscreen.cpp)
    target_compile_definitions(options_autosave_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(options_autosave_test PRIVATE ta-uifont ta-formats ${_ta_sdl2})
    add_test(NAME options_autosave COMMAND options_autosave_test)
    set_tests_properties(options_autosave PROPERTIES TIMEOUT 60
      ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
    # With SDL, settings_test also reads settings files.
    target_sources(settings_test PRIVATE src/client/settings.cpp)
    target_compile_definitions(settings_test PRIVATE TA_SETTINGS_FILE_TEST)
    target_link_libraries(settings_test PRIVATE ${_ta_sdl2})
    if(UNIX AND NOT APPLE)
      add_executable(mod_settings_test tools/mod_settings_test.cpp src/client/settings.cpp)
      target_link_libraries(mod_settings_test PRIVATE ta-formats ${_ta_sdl2})
      add_test(NAME mod_settings COMMAND mod_settings_test)
    endif()
    # GAME DATA FOLDER (client/datafolder.h): precedence, picks, elision, restart
    # arguments for taclient and Cartographer, the Windows restart command line, and
    # the shared settings file (a private temp file via setSettingsPath).
    add_executable(datafolder_test tools/datafolder_test.cpp src/client/settings.cpp src/util/relaunch.cpp)
    target_compile_definitions(datafolder_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(datafolder_test PRIVATE ta-formats ${_ta_sdl2})
    add_test(NAME datafolder COMMAND datafolder_test)
    # The in-game chat box (client/chatbox.h): wrap, placement, fade, and its two
    # Options keys (chatScale / chatAlign) through a private settings file.
    add_executable(chatbox_test tools/chatbox_test.cpp src/client/settings.cpp)
    target_compile_definitions(chatbox_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(chatbox_test PRIVATE ta-formats ${_ta_sdl2})
    add_test(NAME chatbox COMMAND chatbox_test)
    # The conjure (build) menu's layout (client/buildrow.h): wrapping into rows left of
    # the command panel, never under it, and one row exactly as before where it fits.
    add_executable(buildrow_test tools/buildrow_test.cpp)
    target_include_directories(buildrow_test PRIVATE src)
    add_test(NAME buildrow COMMAND buildrow_test)
    # The CHANGE MAP panel's map list FILTER (client/mapfilter.h): case-insensitive.
    add_executable(mapfilter_test tools/mapfilter_test.cpp)
    target_include_directories(mapfilter_test PRIVATE src)
    add_test(NAME mapfilter COMMAND mapfilter_test)
    # The map view's top-left overlays (client/hudcorner.h): the stream banner, GAME TIME
    # clock and F4 scoreboard stack instead of overlapping, at every size and scale.
    add_executable(hudcorner_test tools/hudcorner_test.cpp)
    target_include_directories(hudcorner_test PRIVATE src)
    add_test(NAME hudcorner COMMAND hudcorner_test)
    # Double-click select (client/doubleclick.h): same type, mine, drawn in the map view.
    add_executable(doubleclick_test tools/doubleclick_test.cpp)
    target_include_directories(doubleclick_test PRIVATE src)
    add_test(NAME doubleclick COMMAND doubleclick_test)
                add_executable(stream_capture_test tools/stream_capture_test.cpp src/client/hotkeys.cpp src/client/streaming.cpp src/client/streamcapture_d3d.cpp src/video/stream.cpp src/video/streamcursor.cpp src/video/rgbascale.cpp
                   src/client/settings.cpp)
    target_link_libraries(stream_capture_test PRIVATE ta-uifont ta-formats ta-credstore ta_ffmpeg ${_ta_sdl2})
    target_compile_definitions(stream_capture_test PRIVATE SDL_MAIN_HANDLED TA_STREAM_TESTING=1)
    add_test(NAME streaming_capture COMMAND stream_capture_test)
    # The hardware cursor stamped through the portable capture path (software renderer).
    add_test(NAME streaming_capture_cursor COMMAND stream_capture_test "" "" 640 480 cursor)
    set_tests_properties(streaming_capture_cursor PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 15)
    set_tests_properties(streaming_capture PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 15)
    # Remember Key logic and panel against a fake credential store (never the real one).
    add_executable(stream_key_test tools/stream_key_test.cpp src/client/hotkeys.cpp src/client/streaming.cpp src/client/streamcapture_d3d.cpp src/video/stream.cpp src/video/streamcursor.cpp src/video/rgbascale.cpp
                   src/client/settings.cpp)
    target_link_libraries(stream_key_test PRIVATE ta-uifont ta-formats ta-credstore ta_ffmpeg ${_ta_sdl2})
    target_compile_definitions(stream_key_test PRIVATE SDL_MAIN_HANDLED)
    add_test(NAME stream_key COMMAND stream_key_test)
    set_tests_properties(stream_key PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 30)
    # The panel's bitrate against YouTube's table, and the UPLOAD CAN'T KEEP UP warning.
    add_executable(stream_bitrate_test tools/stream_bitrate_test.cpp src/client/hotkeys.cpp src/client/streaming.cpp src/client/streamcapture_d3d.cpp src/video/stream.cpp src/video/streamcursor.cpp src/video/rgbascale.cpp
                   src/client/settings.cpp)
    target_link_libraries(stream_bitrate_test PRIVATE ta-uifont ta-formats ta-credstore ta_ffmpeg ${_ta_sdl2})
    target_compile_definitions(stream_bitrate_test PRIVATE SDL_MAIN_HANDLED)
    add_test(NAME stream_bitrate COMMAND stream_bitrate_test)
    set_tests_properties(stream_bitrate PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy" TIMEOUT 30)
    add_executable(cursor_test tools/cursor_test.cpp src/client/cursors.cpp)
    target_include_directories(cursor_test PRIVATE src)
    target_compile_definitions(cursor_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(cursor_test PRIVATE ta-formats ${_ta_sdl2})
    add_test(NAME cursor_roster COMMAND cursor_test)
    # Which cursor shows when (client/cursorrules.h), row by row against retail's selector.
    add_executable(cursor_rules_test tools/cursor_rules_test.cpp src/client/cursors.cpp)
    target_include_directories(cursor_rules_test PRIVATE src)
    target_compile_definitions(cursor_rules_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(cursor_rules_test PRIVATE ta-formats ${_ta_sdl2})
    add_test(NAME cursor_rules COMMAND cursor_rules_test)
    # What a click orders (client/clickrules.h), with the cursor parity test.
    add_executable(click_rules_test tools/click_rules_test.cpp src/client/cursors.cpp)
    target_include_directories(click_rules_test PRIVATE src)
    target_compile_definitions(click_rules_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(click_rules_test PRIVATE ta-formats ${_ta_sdl2})
    add_test(NAME click_rules COMMAND click_rules_test)
    if(TA_TEST_DATA AND CMAKE_BUILD_TYPE STREQUAL "Debug")
      add_test(NAME build_controls COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_CURSOR_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/ui-test-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/ui-test-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1000 700 --shot "${CMAKE_CURRENT_BINARY_DIR}/build-controls.png")
      set_tests_properties(build_controls PROPERTIES TIMEOUT 90)
      add_test(NAME gate_pick COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_GATE_PICK_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/gate-pick-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/gate-pick-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1000 700 --shot "${CMAKE_CURRENT_BINARY_DIR}/gate-pick.png")
      set_tests_properties(gate_pick PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: every gate \\(4 types\\)")
      # Cursor picking (perf follow-up C): a unit live before its type's model loads (the
      # registration budget) gets the fallback hit box cached by the cursor pass, as a scan
      # over every unit does, though the pointer is nowhere near it.
      add_test(NAME pick_late_model COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_PICK_LATE_MODEL_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/pick-late-model-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/pick-late-model-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1000 700 --shot "${CMAKE_CURRENT_BINARY_DIR}/pick-late-model.png")
      set_tests_properties(pick_late_model PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: late model pick leaves the full scan's hit-box cache")
      # Clear orders fan out to every selected reclaimer (box, right-click, armed
      # CLEAR; Shift per unit, a soldier untouched), and Inner Circle's one
      # overlapped map feature (removed at load) is not drawn.
      add_test(NAME reclaim_fanout COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_RECLAIM_FANOUT_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/reclaim-fanout-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/reclaim-fanout-profile
          $<TARGET_FILE:taclient> game "Inner Circle" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1000 700 --shot "${CMAKE_CURRENT_BINARY_DIR}/reclaim-fanout.png")
      set_tests_properties(reclaim_fanout PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: reclaim fan-out to every selected reclaimer")
      # What a click orders through the real input() (client/clickrules.h): right-click
      # per unit on release, right-drag and armed boxes, armed clicks gated by their
      # cursor, the minimap's orders and placement, the picker and select rules.
      add_test(NAME click_input COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_CLICK_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/click-input-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/click-input-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1000 700 --shot "${CMAKE_CURRENT_BINARY_DIR}/click-input.png")
      set_tests_properties(click_input PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: clicks all")
      add_test(NAME output_stance_hud COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_OUTPUT_STANCE_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/output-stance-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/output-stance-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1200 800 --shot "${CMAKE_CURRENT_BINARY_DIR}/output-stance.png")
      set_tests_properties(output_stance_hud PROPERTIES TIMEOUT 120
          PASS_REGULAR_EXPRESSION "PASS: output stance: its new unit starts Passive")
      # Units that die petrified or frozen stand as statues in stone or ice
      # (the side's stoneanim/frozenanim texture on every face), not their colours.
      add_test(NAME statue_texture COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_STATUE_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/statue-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/statue-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 900 600 --shot "${CMAKE_CURRENT_BINARY_DIR}/statue.png")
      set_tests_properties(statue_texture PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: statue: the petrified archer is drawn in stone")
      add_test(NAME area_build_ghosts COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_AREA_GHOST_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/area-ghost-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/area-ghost-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1200 800 --shot "${CMAKE_CURRENT_BINARY_DIR}/area-ghosts.png")
      set_tests_properties(area_build_ghosts PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: an area lodestone job ghosts every deposit")
      # Destination markers: every unit of a mixed-speed Legion move shows its own spot,
      # flashed after the order and held under Shift (GameView::drawUnitSpots).
      add_test(NAME unit_spot_markers COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_UNIT_SPOTS_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/unit-spots-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/unit-spots-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1200 800 --shot "${CMAKE_CURRENT_BINARY_DIR}/unit-spots.png")
      set_tests_properties(unit_spot_markers PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: every unit of a mixed Legion move shows its own spot")
      add_test(NAME area_drag_input COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_AREA_DRAG_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/area-drag-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/area-drag-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1000 700 --shot "${CMAKE_CURRENT_BINARY_DIR}/area-drag.png")
      set_tests_properties(area_drag_input PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: area drag threshold")
      # The self-destruct notice follows the unit's real countdown (client only).
      add_test(NAME selfdestruct_notice COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_SELFDESTRUCT_NOTICE_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/selfdestruct-notice-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/selfdestruct-notice-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1000 700 --shot "${CMAKE_CURRENT_BINARY_DIR}/selfdestruct-notice.png")
      set_tests_properties(selfdestruct_notice PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: self-destruct notice follows the countdown")
      # Shift-drag build lines through real mouse/wheel input: every previewed site is
      # queued and then built, touching at gap 0, wheel gap held and clamped.
      add_test(NAME buildline_harness COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_BUILDLINE_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/buildline-test-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/buildline-test-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/buildline.png")
      set_tests_properties(buildline_harness PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "PASS: [0-9]+ shift-drag lines")
      # Chained shift-drags for one monarch (TA_BUILDLINE_CHAIN=type/gap/builder/drags):
      # a U with the builder between its arms, a closed box from outside, a tower V,
      # and a line doubling back along the last. Every distinct site gets built.
      foreach(_chain u=0/0/450,40/400,0/0,300/-400,0
                     box=0/0/-60,-80/400,0/0,300/-400,0/0,-300
                     tower_v=2/0/-60,-80/300,300/300,-300
                     double_back=0/0/-60,-80/400,0/-400,64)
        string(REGEX MATCH "^[a-z_]+" _name "${_chain}")
        string(REGEX REPLACE "^[a-z_]+=" "" _spec "${_chain}")
        add_test(NAME buildline_chain_${_name} COMMAND ${CMAKE_COMMAND} -E env
            SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_BUILDLINE_CHAIN=${_spec}
            XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/buildline-chain-${_name}
            XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/buildline-chain-${_name}
            $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
            --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/buildline-chain-${_name}.png")
        set_tests_properties(buildline_chain_${_name} PROPERTIES TIMEOUT 300
            PASS_REGULAR_EXPRESSION "PASS: build-line chain")
      endforeach()
      add_test(NAME render_texture_reuse COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          TA_SHADOW_BENCH=16 TA_SHADOW_BENCH_TYPE=arakeep TA_PROFILE_ZOOM=0.5
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=180 TA_GEOMETRY_VERIFY=1 TA_REQUIRE_GEOMETRY_REUSE=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/render-test-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/render-test-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --novsync)
      set_tests_properties(render_texture_reuse PROPERTIES TIMEOUT 90)
      # Workstream M: every structure type at zoom 0.5 (distant-image bakes), construction
      # sites (special units), Texture Filtering toggled every 70 frames and render-target
      # resets: no failed indexed draw; on OpenGL indexed draws and switches both ways,
      # elsewhere the baked pages only (the ctest's dummy driver; run it with
      # SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl for the GL form).
      add_test(NAME render_palette_indexed COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          TA_SHADOW_BENCH=160 TA_RENDER_ROSTER=structures TA_RENDER_SPACING=96 TA_PROFILE_ZOOM=0.5
          TA_RENDER_EXTRAS=1 TA_RENDER_CAMERA=1 TA_RENDER_FILTER=70 TA_RENDER_RESET=110
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=300 TA_REQUIRE_PALETTE=1 TA_GEOMETRY_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/render-test-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/render-test-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --novsync)
      set_tests_properties(render_palette_indexed PROPERTIES TIMEOUT 120)
      # Workstream N: the same scene with native passes required and every pass's GL
      # state checked against SDL's priming. The ctest's dummy driver has no native
      # passes (the study requires none to open: the runtime fallback); run it with
      # SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=opengl for the GL form, where
      # passes must open across palette-form switches and render-target resets.
      add_test(NAME render_native_pass COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          TA_SHADOW_BENCH=160 TA_RENDER_ROSTER=structures TA_RENDER_SPACING=96 TA_PROFILE_ZOOM=0.5
          TA_RENDER_EXTRAS=1 TA_RENDER_CAMERA=1 TA_RENDER_FILTER=70 TA_RENDER_RESET=110
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=300 TA_REQUIRE_NATIVE_PASS=1 TA_NATIVE_PASS_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/native-pass-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/native-pass-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --novsync)
      set_tests_properties(render_native_pass PROPERTIES TIMEOUT 120
          PASS_REGULAR_EXPRESSION "NATIVE_PASS_OK")
      # Animated colour atlases in a real scene (two player colours, simulated
      # render-target resets): after every update each atlas must equal a complete
      # repaint byte for byte (TA_ATLAS_VERIFY; throws on the first mismatch).
      add_test(NAME render_atlas_verify COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          TA_SHADOW_BENCH=120 TA_RENDER_BATTLE=1 TA_PROFILE_ZOOM=0.75
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=130 TA_RENDER_RESET=45 TA_ATLAS_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/atlas-verify-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/atlas-verify-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1024 768 --novsync)
      set_tests_properties(render_atlas_verify PROPERTIES TIMEOUT 120
          PASS_REGULAR_EXPRESSION "ATLAS_VERIFY checks=[1-9]")
      # Stable per-unit geometry entries in a real scene (workstream C): a moving,
      # panning army reorders in depth every frame, 5% dies and respawns every 30
      # frames, render targets reset every 70 and the world epoch advances every 110
      # (the id reuse of a rejoin); retention is shortened to 100 ms and released
      # memory is trimmed at once (debug knobs), so entries of units that left the
      # pan expire, are recycled and trimmed within the run. Every frame each visible
      # unit must hold a distinct entry owned by its own identity with no other
      # unit's content (TA_GEOMETRY_OWNERSHIP_VERIFY), and every cache hit must
      # equal a rebuild (TA_GEOMETRY_VERIFY); both throw at the first violation.
      add_test(NAME render_geometry_ownership COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          TA_SHADOW_BENCH=200 TA_PROFILE_ZOOM=0.75 TA_RENDER_MOTION=1 TA_RENDER_CAMERA=1
          TA_RENDER_CHURN=30 TA_RENDER_RESET=70 TA_RENDER_EPOCH=110
          TA_GEOMETRY_RETAIN_MS=100 TA_GEOMETRY_TRIM_MS=0
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=230 TA_GEOMETRY_VERIFY=1 TA_GEOMETRY_OWNERSHIP_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/geometry-ownership-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/geometry-ownership-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 800 600 --novsync)
      set_tests_properties(render_geometry_ownership PROPERTIES TIMEOUT 180
          PASS_REGULAR_EXPRESSION "OWNERSHIP_VERIFY checks=[1-9][0-9]* acquired=[1-9][0-9]* replaced=[1-9][0-9]* destroyed=[0-9]+ expired=[1-9][0-9]* budget=[0-9]+ trimmed=[1-9]")
      # Staged unit geometry in a real scene (workstream D): a battle with construction
      # sites, ghosts, flyers and corpses under a panning, zooming camera, so units are
      # re-placed (pan/zoom), re-coloured (conjuring, summoning, ghosts) and rebuilt
      # (animation). Every reuse, complete or partial, must equal a complete rebuild,
      # every local build must equal the former single-pass transform
      # (TA_GEOMETRY_VERIFY) and every shadow the full collector (TA_SHADOW_VERIFY);
      # each throws at the first mismatch.
      add_test(NAME render_geometry_stages COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
          TA_SHADOW_BENCH=150 TA_RENDER_BATTLE=1 TA_RENDER_EXTRAS=1 TA_PROFILE_ZOOM=1
          TA_RENDER_CAMERA=1 TA_RENDER_ZOOM=1
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=150 TA_GEOMETRY_VERIFY=1 TA_SHADOW_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/geometry-stages-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/geometry-stages-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 800 600 --novsync)
      set_tests_properties(render_geometry_stages PROPERTIES TIMEOUT 180
          PASS_REGULAR_EXPRESSION "GEOMETRY_STAGES hits=[0-9]+ placement=[1-9][0-9]* material=[1-9][0-9]* single_pass=[1-9]")
      # Shadow silhouette tile reuse in a real scene (workstream E), on SDL's software
      # renderer (TA_SHADOW_ATLAS_SOFTWARE runs the atlas there): a stationary mixed
      # roster with construction, ghosts, flyers and corpses, animated textures with
      # and without shadow masks (aralode; tarnecro/tarwitch), a veterancy cycle
      # (material-only), a whole-pixel pan that pauses (translation and body-only
      # reuse) and render-target resets. Every frame each kept tile must equal a
      # complete re-bake pixel for pixel and every tile the pre-E key kept must be
      # kept (TA_SHADOW_CACHE_VERIFY); every skipped silhouette must equal a fresh
      # collection and every reuse a complete rebuild (TA_GEOMETRY_VERIFY).
      add_test(NAME render_shadow_cache COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
          TA_SHADOW_ATLAS_SOFTWARE=1 TA_SHADOW_BENCH=80 TA_PROFILE_ZOOM=1
          TA_RENDER_ROSTER=araarch,zonter,versword,tarnecro,tarwitch,aralode TA_RENDER_EXTRAS=1
          TA_RENDER_CAMERA_SNAP=20 TA_RENDER_VETERAN=25 TA_RENDER_RESET=70
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=140 TA_SHADOW_CACHE_VERIFY=1 TA_GEOMETRY_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/shadow-cache-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/shadow-cache-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1024 768 --novsync)
      set_tests_properties(render_shadow_cache PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "shadow_skipped=[1-9][0-9]*\nSHADOW_CACHE_VERIFY frames=[1-9][0-9]* tiles=[0-9]+ reused=[0-9]+ body=[1-9][0-9]* translated=[1-9]")
      # Cost-planned pool passes (workstream I), on the software renderer with the
      # shadow atlas: a mixed roster that moves (local rebuilds), whole-pixel camera
      # steps (shadow point checks), a veterancy cycle, deaths/respawns (cold
      # entries) and render-target resets, on a 4-thread pool with policies that
      # make every pass parallel. Each planned geometry pass must build every
      # visible unit exactly once; the shadow checks, tile transforms and body
      # copies must equal a serial re-run bit for bit (TA_SCHEDULE_VERIFY); every
      # reuse equals a complete rebuild (TA_GEOMETRY_VERIFY).
      add_test(NAME render_schedule COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
          TA_SHADOW_ATLAS_SOFTWARE=1 TA_SHADOW_BENCH=120 TA_PROFILE_ZOOM=1
          TA_RENDER_ROSTER=araarch,zonter,versword,tarnecro,tarwitch,aralode TA_RENDER_EXTRAS=1
          TA_RENDER_MOTION=1 TA_RENDER_CAMERA_SNAP=20 TA_RENDER_VETERAN=25 TA_RENDER_CHURN=30
          TA_RENDER_RESET=70 TA_RENDER_POOL_THREADS=4
          TA_SCHEDULE_GEOMETRY=0,4000,3000 TA_SCHEDULE_SHADOW_CHECK=0,500,100
          TA_SCHEDULE_SHADOW_PREP=0,500,100 TA_SCHEDULE_COPY=0,500,100,4
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=140 TA_SCHEDULE_VERIFY=1 TA_GEOMETRY_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/schedule-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/schedule-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1024 768 --novsync)
      set_tests_properties(render_schedule PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "SCHEDULE_VERIFY frames=[1-9][0-9]* geometry_passes=[1-9][0-9]* builds=[1-9][0-9]* parallel_passes=[1-9][0-9]* checks=[1-9][0-9]* prep_passes=[1-9][0-9]* prep_points=[1-9][0-9]* copy_passes=[1-9][0-9]* copy_vertices=[1-9]")
      # Distant-image lookup before body generation (workstream G), on the software
      # renderer (TA_DISTANT_SOFTWARE runs the image cache there): a mixed roster at
      # zoom 0.5 under a panning camera (held images re-placed, bodies deferred),
      # with a veterancy cycle, deaths/respawns and render-target resets. Every
      # deferred body is regenerated in a copy and must keep the entry's revision,
      # vertex count and shadow output, and its image must have been shown
      # (TA_DISTANT_VERIFY); every generated body equals a complete rebuild
      # (TA_GEOMETRY_VERIFY). No prediction may miss (catchup=0 late=0).
      add_test(NAME render_distant_lookup COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
          TA_DISTANT_SOFTWARE=1 TA_SHADOW_BENCH=150 TA_PROFILE_ZOOM=0.5 TA_RENDER_CAMERA=1
          TA_RENDER_VETERAN=25 TA_RENDER_CHURN=40 TA_RENDER_RESET=70
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=120 TA_DISTANT_VERIFY=1 TA_GEOMETRY_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/distant-lookup-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/distant-lookup-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 800 600 --novsync)
      set_tests_properties(render_distant_lookup PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "DISTANT_VERIFY frames=[1-9][0-9]* deferred=[1-9][0-9]* checked=[1-9][0-9]* catchup=0 late=0")
      # Unit broad phase (workstream L), on the software renderer: every frame the
      # painter candidates taken from the snapshot's unit grid must equal a full scan
      # of all live units -- same units, order, depth keys and layers -- and so must
      # the units each per-unit overlay draws, or draw() throws
      # (TA_UNIT_INDEX_VERIFY). The base variant is a fogged two-player battle with
      # construction, ghosts, flyers, corpses, self-destruct countdowns, control
      # squads, production queues and queued builds (the overlays and the snapshot's
      # build-ghost list), moving units (interpolation), a
      # panning and zooming camera with reversals, camera shake, deaths/respawns and
      # world-epoch bumps (id reuse); _jump is a 3000-unit army with squads seen
      # through a camera jumping between distant views (a small visible subset).
      foreach(_variant IN ITEMS "" "_jump")
        if(_variant STREQUAL "")
          set(_scene TA_SHADOW_BENCH=300 TA_RENDER_SPACING=120 TA_RENDER_BATTLE=1 TA_RENDER_EXTRAS=1
              TA_RENDER_OVERLAYS=1 TA_PROFILE_FOG=1 TA_RENDER_MOTION=1 TA_RENDER_CAMERA=1
              TA_RENDER_ZOOM=1 TA_RENDER_SHAKE=37 TA_RENDER_CHURN=40 TA_RENDER_EPOCH=90 TA_RENDER_FRAMES=200)
          set(_expect "health=[1-9][0-9]* selfdestruct=[1-9][0-9]* squads=[1-9][0-9]* ghosts=[1-9]")
        else()
          set(_scene TA_SHADOW_BENCH=3000 TA_RENDER_OVERLAYS=1 TA_PROFILE_ZOOM=1 TA_RENDER_JUMP=15
              TA_RENDER_SHAKE=23 TA_RENDER_FRAMES=120)
          set(_expect "squads=[1-9]")
        endif()
        add_test(NAME render_unit_index${_variant} COMMAND ${CMAKE_COMMAND} -E env
            SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software ${_scene}
            TA_RENDER_STUDY=1 TA_UNIT_INDEX_VERIFY=1
            XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/unit-index${_variant}-profile
            XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/unit-index${_variant}-profile
            $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
            --testbuild --winsize 1024 768 --novsync)
        set_tests_properties(render_unit_index${_variant} PROPERTIES TIMEOUT 300
            PASS_REGULAR_EXPRESSION "UNIT_INDEX_VERIFY frames=[1-9][0-9]* passes=[1-9][0-9]* live=[1-9][0-9]* candidates=[1-9][0-9]* accepted=[1-9][0-9]* .*${_expect}")
      endforeach()
      # Interactive render cases (perf follow-up measurement B, tools/render_benchmark.py
      # --scenes interactive): one scene with every knob -- the pointer sweeping the
      # view (cursor classified every frame), the army selected, builders with queued
      # ghosts (half at the far map corner), towers with Archers behind them under
      # HIDDEN UNITS Fade on the study frame clock, the shadow atlas on the software
      # renderer. Some frame must show all of that work in its telemetry.
      add_test(NAME render_interactive_cases COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
          TA_SHADOW_BENCH=400 TA_SHADOW_BENCH_TYPE=araarch TA_RENDER_SPACING=32 TA_PROFILE_ZOOM=1
          TA_RENDER_COVER=vertower,4 TA_RENDER_FADE_CLOCK=1 TA_HIDDEN_UNITS=fade TA_RENDER_SELECT=all
          TA_RENDER_MOUSE=sweep:0.1,0.1,0.9,0.9 TA_RENDER_GHOSTS=4,6,50 TA_SHADOW_ATLAS_SOFTWARE=1
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=120 TA_PROF=1 TA_PROF_FRAMES=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/interactive-cases-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/interactive-cases-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1024 768 --novsync)
      set_tests_properties(render_interactive_cases PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "hidden_faded=[1-9][0-9]* cursor_evals=1 cursor_pick_tests=[1-9][0-9]* cursor_pick_hits=[1-9][0-9]* ghosts_queued=[1-9][0-9]* ghost_preps=[1-9][0-9]* hidden_cover_tests=[1-9][0-9]* shadow_demand_px=[1-9]")
      # Cursor picking (perf follow-up C), on the software renderer: the render study's
      # scripted pointer (TA_RENDER_CURSOR) moves over unit bodies and bare ground every
      # frame; every unit the exact pick test accepts must be among the candidates the
      # unit grid returned, so must every record whose body pick circle holds the
      # pointer's ground point (Revive/Reclaim), and every cursor decision must equal a
      # re-evaluation over the whole unit list, or the cursor overlay throws
      # (TA_PICK_INDEX_VERIFY). The base
      # variant is a fogged two-player battle with construction, ghosts, flyers, corpses,
      # squads, factories and queued builds, moving units (interpolation), a panning and
      # zooming camera, camera shake, deaths/respawns and world-epoch bumps, with 20
      # units selected; _orders selects every own unit (large selection) and arms each
      # order in turn (attack, repair, guard, load, move, patrol, reclaim, fight), with
      # Tactical Dots from normal size and HIDDEN UNITS dots; _jump is a 3000-unit army
      # seen through a camera jumping between distant views, nothing selected.
      file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pick-index-dots-settings.ini"
           "fullscreen = 0\ntacticalDots = 1\ntacticalDotsZoom = 100\nhiddenUnits = dots\n")
      foreach(_variant IN ITEMS "" "_orders" "_jump")
        set(_settings "")
        set(_bodies "body_queries=[1-9][0-9]* body_candidates=[0-9]* body_hits=[1-9]")
        if(_variant STREQUAL "_jump")
          set(_scene TA_SHADOW_BENCH=3000 TA_RENDER_OVERLAYS=1 TA_PROFILE_ZOOM=1 TA_RENDER_JUMP=15
              TA_RENDER_SHAKE=23 TA_RENDER_FRAMES=120)
          set(_bodies "body_queries=")   # nothing selected: no body picks
        else()
          set(_scene TA_SHADOW_BENCH=300 TA_RENDER_SPACING=120 TA_RENDER_BATTLE=1 TA_RENDER_EXTRAS=1
              TA_RENDER_OVERLAYS=1 TA_PROFILE_FOG=1 TA_RENDER_MOTION=1 TA_RENDER_CAMERA=1
              TA_RENDER_ZOOM=1 TA_RENDER_SHAKE=37 TA_RENDER_CHURN=40 TA_RENDER_EPOCH=90 TA_RENDER_FRAMES=200)
          if(_variant STREQUAL "_orders")
            list(APPEND _scene TA_RENDER_CURSOR_SELECT=100000 TA_RENDER_CURSOR_CMD=*)
            set(_settings TA_SETTINGS_FILE=${CMAKE_CURRENT_BINARY_DIR}/pick-index-dots-settings.ini)
          else()
            list(APPEND _scene TA_RENDER_CURSOR_SELECT=20)
          endif()
        endif()
        add_test(NAME render_pick_index${_variant} COMMAND ${CMAKE_COMMAND} -E env
            SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software ${_scene} ${_settings}
            TA_RENDER_STUDY=1 TA_RENDER_CURSOR=1 TA_PICK_INDEX_VERIFY=1
            XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/pick-index${_variant}-profile
            XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/pick-index${_variant}-profile
            $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
            --testbuild --winsize 1024 768 --novsync)
        set_tests_properties(render_pick_index${_variant} PROPERTIES TIMEOUT 600
            PASS_REGULAR_EXPRESSION "PICK_INDEX_VERIFY queries=[1-9][0-9]* live=[1-9][0-9]* candidates=[0-9]* hits=[1-9][0-9]* decisions=[1-9][0-9]* ${_bodies}")
      endforeach()
      # Build ghosts (perf follow-up E), on the software renderer: queued-build lists
      # cycling through repeated structure types under a panning, zooming camera, the
      # placed-site ghosts of TA_RENDER_EXTRAS and two simulated render-target resets
      # (atlases recreated: the retained ghost geometry must be dropped), plus a ghost
      # of every registry type each frame, on and around the view, half of them
      # red-washed (TA_GHOST_ALL_TYPES: every model, including those textured from
      # outside the colour atlas). Every ghost is also collected and sorted afresh:
      # retained triangles must equal that reference exactly, and a ghost not drawn
      # must lie wholly outside the clip (TA_GHOST_VERIFY), or draw() throws.
      add_test(NAME render_build_ghosts COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
          TA_SHADOW_BENCH=40 TA_RENDER_EXTRAS=1 TA_RENDER_GHOST_QUEUES=6,12,4,0,600
          TA_RENDER_CAMERA=1 TA_RENDER_ZOOM=1 TA_RENDER_RESET=50 TA_RENDER_FRAMES=120
          TA_RENDER_STUDY=1 TA_GHOST_VERIFY=1 TA_GHOST_ALL_TYPES=1 TA_UNIT_INDEX_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/build-ghosts-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/build-ghosts-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1024 768 --novsync)
      set_tests_properties(render_build_ghosts PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "GHOST_VERIFY checked=[1-9][0-9]* hits=[1-9][0-9]* culled=[1-9][0-9]* collected=[1-9][0-9]* inexact=[1-9][0-9]*")
      # CPU budgets (workstream J) in a real scene: a battle, then a sustained empty
      # view, with short trim windows and tiny terrain budgets. Retained buffers must
      # be trimmed after the demand falls, keep their contents byte for byte, never be
      # trimmed twice within a window, and the renderer's CPU memory must come down
      # (TA_CPU_TRIM_VERIFY); every geometry reuse still equals a rebuild.
      add_test(NAME render_cpu_budget COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
          TA_SHADOW_BENCH=300 TA_PROFILE_ZOOM=0.5 TA_RENDER_BATTLE=1
          TA_RENDER_SPARSE_AFTER=90 TA_RENDER_SPARSE_OFFSET=8800,200
          TA_CPU_TRIM_MS=400 TA_CPU_TRIM_FRAMES=20 TA_CPU_TRIM_FLOOR_KIB=1
          TA_TERRAIN_SECTION_BUDGET_MIB=8 TA_TERRAIN_SECTION_MIN_IDLE_MS=1000
          TA_TERRAIN_MIP_BUDGET_MIB=2 TA_TERRAIN_MIP_IDLE_MS=1000
          TA_RENDER_STUDY=1 TA_RENDER_FRAMES=200 TA_CPU_TRIM_VERIFY=1 TA_GEOMETRY_VERIFY=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/cpu-budget-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/cpu-budget-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 800 600 --novsync)
      set_tests_properties(render_cpu_budget PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "CPU_TRIM_VERIFY frames=[1-9][0-9]* trims=[1-9][0-9]* .*content_checks=[1-9][0-9]* .*recovered=[1-9]")
      # Retained body vertices (workstream H) on the software renderer: the arena's
      # storage is simulated in CPU memory (TA_BODY_ARENA_SOFTWARE) and its commands
      # are drawn by gathering their ranges, so the planner's bookkeeping runs in the
      # real body pass; TA_BODY_ARENA_MIN=1 puts every command in the arena (small
      # segments and distant-image quads by value included). A mixed roster at zoom
      # 0.5 with a veterancy cycle, deaths/respawns and render-target resets. The base
      # scene alternates 20 moving / 20 still camera frames (streamed / retained); the
      # _still variant holds the camera on a tight storage (TA_BODY_ARENA_HEADROOM)
      # so it rolls over: commands draw from the draining storage, migrate, and the
      # storage drains. Every arena command must equal the command assembled from its
      # sources, and every assembled command its sources with no gap in the copy
      # buffer (TA_BODY_ARENA_VERIFY); no command may fall back. In the _fail
      # variant the first arena write with uploads in every 40 frames fails
      # (TA_BODY_ARENA_FAIL_WRITE): that frame's arena commands are re-assembled in
      # place of the staged uploads.
      foreach(_variant IN ITEMS "" "_still" "_fail")
        if(_variant STREQUAL "_still")
          set(_arena_env TA_BODY_ARENA_HEADROOM=1.2)
          set(_arena_pass "fallbacks=0 rollovers=[1-9][0-9]* drains=[1-9][0-9]* old_commands=[1-9].* assembled=[1-9]")
        elseif(_variant STREQUAL "_fail")
          set(_arena_env TA_BODY_ARENA_FAIL_WRITE=40)
          set(_arena_pass "fallbacks=[1-9][0-9]* .* assembled=[1-9]")
        else()
          set(_arena_env TA_RENDER_CAMERA_SNAP=20)
          set(_arena_pass "streams=[1-9][0-9]* fallbacks=0")
        endif()
        add_test(NAME render_body_arena${_variant} COMMAND ${CMAKE_COMMAND} -E env
            SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
            TA_BODY_ARENA_SOFTWARE=1 TA_BODY_ARENA_MIN=1 ${_arena_env} TA_DISTANT_SOFTWARE=1
            TA_SHADOW_BENCH=150 TA_PROFILE_ZOOM=0.5 TA_RENDER_ROSTER=araarch,zonter,versword,aralode
            TA_RENDER_VETERAN=25 TA_RENDER_CHURN=40 TA_RENDER_RESET=70
            TA_RENDER_STUDY=1 TA_RENDER_FRAMES=140 TA_BODY_ARENA_VERIFY=1
            XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/body-arena${_variant}-profile
            XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/body-arena${_variant}-profile
            $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
            --testbuild --winsize 800 600 --novsync)
        set_tests_properties(render_body_arena${_variant} PROPERTIES TIMEOUT 300
            PASS_REGULAR_EXPRESSION "BODY_ARENA_VERIFY frames=[1-9][0-9]* commands=[1-9][0-9]* vertices=[0-9]+ retained=[1-9][0-9]* uploaded=[1-9][0-9]* appends=[1-9][0-9]* rebuilds=[1-9][0-9]* .*${_arena_pass}")
      endforeach()
      # Persistent shadow tile cells (workstream F) under space pressure, on the
      # software renderer: the render_shadow_cache scene with 288-pixel pages, short
      # policy windows and deaths/respawns. render_shadow_atlas then moves the
      # camera so part of the army stays in view (cells expire, a sparse last page
      # is emptied by bounded migration and released, compaction); the _zoom
      # variant zooms continuously (cells outgrown every frame, free repacks,
      # retained cells evicted under pressure). Every kept tile must equal a
      # complete re-bake and the allocator must pass its consistency check every
      # frame (TA_SHADOW_CACHE_VERIFY).
      # The _band variant holds a still army whose tiles fit four pages by area but
      # not by packing (every frame falls back: the page count is held at the four
      # base pages), then goes sparse: a frame that does not fit must create no page
      # texture, so nothing is created and released over and over while the view
      # stays in that band (at most the sparse view's pages).
      # Perf follow-up K: _extended is the same army with the page limit at 16 -- its
      # tiles take extended pages instead of falling back, every frame; once the view
      # goes sparse the extended pages empty and are released. _yield lowers the GPU
      # cap under it at frame 40 (TA_RENDER_GPU_CAP_AT, to what is held) and lifts it
      # at frame 50: the extended pages are given back once (a pressure shed), the
      # army falls back, and extended pages return only after the cooldown (400 ms /
      # 16 frames with these windows: at least 24 frames after the shed). _full is a
      # smaller army that the four base pages hold only just: the four-page atlas
      # repacks it when a tile outgrows its cell; with extended pages it adds a
      # page or two instead and keeps its cells (no fallback, no page released).
      # Its size is tuned to the scene: 52 fitted before units were drawn at
      # retail's absolute z - (y >> 1) lift (335eb065), after which 52 released an
      # extended page and repacked twice; 58 holds on both lifts. _off is the
      # _extended army with unit shadows switched off at frame 60 (Options SHADOWS):
      # no frame maintains the atlas then, so its extended pages go at once.
      foreach(_variant IN ITEMS "" "_zoom" "_band" "_extended" "_yield" "_full" "_off")
        if(_variant STREQUAL "")
          set(_scene TA_RENDER_CAMERA_SNAP=20 TA_PROFILE_ZOOM=1 TA_RENDER_SPARSE_AFTER=100
              TA_RENDER_SPARSE_OFFSET=5200,5000 TA_RENDER_FRAMES=170)
          set(_expect "kept=[1-9][0-9]* allocs=[1-9][0-9]* compactions=[1-9][0-9]* migrated=[1-9][0-9]* evicted=[0-9]+ page_releases=[1-9].* validated=[1-9]")
        elseif(_variant STREQUAL "_zoom")
          set(_scene TA_RENDER_ZOOM=1 TA_RENDER_CAMERA=1 TA_RENDER_FRAMES=120)
          set(_expect "kept=[0-9]+ allocs=[1-9][0-9]* compactions=[1-9][0-9]* migrated=[0-9]+ evicted=[1-9][0-9]*.* validated=[1-9]")
        elseif(_variant STREQUAL "_band")
          set(_scene TA_RENDER_SPARSE_AFTER=80 TA_RENDER_SPARSE_OFFSET=5200,5000 TA_RENDER_FRAMES=130
              TA_SHADOW_PAGES_MAX=4)
          set(_expect "page_releases=[0-2] shrinks=[0-9]+ validated=[1-9][0-9]* page_allocs=[1-4] alloc_failures=[1-9][0-9]")
        elseif(_variant STREQUAL "_extended")
          set(_scene TA_RENDER_SPARSE_AFTER=80 TA_RENDER_SPARSE_OFFSET=5200,5000 TA_RENDER_FRAMES=130
              TA_SHADOW_PAGES_MAX=16)
          set(_expect "page_releases=[1-9][0-9]* shrinks=[0-9]+ validated=[1-9][0-9]* page_allocs=[0-9]+ alloc_failures=0 extended_frames=[1-9][0-9]* max_pages=([5-9]|1[0-6]) sheds=0 fallbacks=0 regrow_gap=0 extended_releases=[1-9]")
        elseif(_variant STREQUAL "_full")
          set(_scene TA_SHADOW_BENCH=58 TA_RENDER_FRAMES=100 TA_RENDER_RESET=0 TA_SHADOW_PAGES_MAX=16)
          set(_expect "compactions=[0-1] .* page_releases=0 .* alloc_failures=0 extended_frames=[1-9][0-9]* max_pages=[5-6] sheds=0 fallbacks=0")
        elseif(_variant STREQUAL "_off")
          set(_scene TA_RENDER_FRAMES=80 TA_RENDER_RESET=0 TA_SHADOW_PAGES_MAX=16
              TA_RENDER_SHADOWS_OFF_AT=60)
          set(_expect "extended_frames=[1-9][0-9]* max_pages=([5-9]|1[0-6]) .* held_pages=0")
        else()
          set(_scene TA_RENDER_FRAMES=80 TA_RENDER_RESET=0 TA_SHADOW_PAGES_MAX=16
              TA_RENDER_GPU_CAP_AT=40,0/50,100000)
          set(_expect "extended_frames=[1-9][0-9]* max_pages=([5-9]|1[0-6]) sheds=1 fallbacks=[1-9][0-9]* regrow_gap=(2[4-9]|[3-9][0-9])")
        endif()
        add_test(NAME render_shadow_atlas${_variant} COMMAND ${CMAKE_COMMAND} -E env
            SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy SDL_RENDER_DRIVER=software
            TA_SHADOW_ATLAS_SOFTWARE=1 TA_SHADOW_BENCH=80
            TA_RENDER_ROSTER=araarch,zonter,versword,tarnecro,tarwitch,aralode TA_RENDER_EXTRAS=1
            TA_RENDER_VETERAN=25 TA_RENDER_RESET=70 TA_RENDER_CHURN=30 ${_scene}
            TA_SHADOW_PAGE_MAX=288 TA_SHADOW_ATLAS_DELAY_MS=100
            TA_RENDER_STUDY=1 TA_SHADOW_CACHE_VERIFY=1
            XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/shadow-atlas${_variant}-profile
            XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/shadow-atlas${_variant}-profile
            $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
            --testbuild --winsize 1024 768 --novsync)
        set_tests_properties(render_shadow_atlas${_variant} PROPERTIES TIMEOUT 300
            PASS_REGULAR_EXPRESSION "SHADOW_CACHE_VERIFY frames=[1-9][0-9]* .* ${_expect}")
      endforeach()
      add_test(NAME tactical_dots COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_TACTICAL_DOTS_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/tactical-dots-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/tactical-dots-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/tactical-dots.png")
      set_tests_properties(tactical_dots PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: tactical dots \\(threshold")
      # Airborne flyers draw over the shots, debris and explosions (retail 0x4fc648).
      add_test(NAME air_over_fx COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_AIR_OVER_FX_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/air-over-fx-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/air-over-fx-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1024 768 --shot "${CMAKE_CURRENT_BINARY_DIR}/air-over-fx.png")
      set_tests_properties(air_over_fx PROPERTIES TIMEOUT 120
          PASS_REGULAR_EXPRESSION "PASS: air over fx \\(altitude")
      # Shots and effects draw in depth order with the ground units and the tall
      # features (the owner's request, not retail): behind a tree covered, in front over it.
      add_test(NAME depth_fx COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_DEPTH_FX_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/depth-fx-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/depth-fx-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/depth-fx.png")
      set_tests_properties(depth_fx PROPERTIES TIMEOUT 120
          PASS_REGULAR_EXPRESSION "PASS: depth fx \\(behind")
      # Full zoom-out fits the map to the world view (left of the right panel, above
      # the bottom bar), at 1080p and on a 7680x2160 super-ultrawide.
      # Windowed (a fresh profile is fullscreen, i.e. the dummy driver's 1024x768
      # desktop), so --winsize is the size tested.
      file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/zoom-floor-settings.ini" "fullscreen = 0\n")
      foreach(_size "1920;1080" "7680;2160")
        list(GET _size 0 _zw)
        list(GET _size 1 _zh)
        add_test(NAME zoom_floor_${_zw}x${_zh} COMMAND ${CMAKE_COMMAND} -E env
            SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_ZOOM_FLOOR_TEST=1
            TA_SETTINGS_FILE=${CMAKE_CURRENT_BINARY_DIR}/zoom-floor-settings.ini
            XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/zoom-floor-${_zw}-profile
            XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/zoom-floor-${_zw}-profile
            $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
            --testbuild --winsize ${_zw} ${_zh} --shot "${CMAKE_CURRENT_BINARY_DIR}/zoom-floor-${_zw}.png")
        set_tests_properties(zoom_floor_${_zw}x${_zh} PROPERTIES TIMEOUT 120
            PASS_REGULAR_EXPRESSION "PASS: zoom floor fits the world view")
      endforeach()
      # HIDDEN UNITS (Options > Graphics): flat features under every unit, the cover
      # rules of DOTS (tree, building, open ground, in front, fog) and its click
      # target, and FADE (the covers over a unit fade, nothing else does; at 100% the
      # frame is OFF's).
      add_test(NAME hidden_units COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_HIDDEN_UNITS_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/hidden-units-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/hidden-units-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/hidden-units.png")
      set_tests_properties(hidden_units PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: hidden units \\(")
      # The display fog is retail's sight plane through LosOn: an enemy at 0.9x
      # sight on flat ground is drawn and at 1.1x is not, from a hill at 1.5x it is,
      # and radar makes a minimap dot only (client/retailfog.h).
      add_test(NAME retail_fog COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_RETAIL_FOG_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/retail-fog-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/retail-fog-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/retail-fog.png")
      set_tests_properties(retail_fog PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: retail fog \\(")
      add_test(NAME spectator_health_bars COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_SPECTATOR_BARS_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/spectator-bars-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/spectator-bars-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/spectator-bars.png")
      set_tests_properties(spectator_health_bars PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: spectator bars \\(")
      foreach(watch_mode replay spectator)
        add_test(NAME watch_only_${watch_mode} COMMAND ${CMAKE_COMMAND} -E env
            SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_WATCHONLY_TEST=${watch_mode}
            XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/watch-only-profile
            XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/watch-only-profile
            $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
            --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/watch-only.png")
        set_tests_properties(watch_only_${watch_mode} PROPERTIES TIMEOUT 90
            PASS_REGULAR_EXPRESSION "PASS: watch only \\(${watch_mode}\\)")
      endforeach()
      add_test(NAME gui_smoothing_menu COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_GUI_SMOOTH_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/gui-smooth-menu-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/gui-smooth-menu-profile
          $<TARGET_FILE:taclient> --data "${TA_TEST_DATA}" --winsize 1280 960
          --shot "${CMAKE_CURRENT_BINARY_DIR}/gui-smooth-menu.png")
      set_tests_properties(gui_smoothing_menu PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: title art smoothing applies live")
      add_test(NAME gui_smoothing_game COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_GUI_SMOOTH_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/gui-smooth-game-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/gui-smooth-game-profile
          $<TARGET_FILE:taclient> game "Ulasem Arena" --data "${TA_TEST_DATA}"
          --testbuild --winsize 1280 960 --shot "${CMAKE_CURRENT_BINARY_DIR}/gui-smooth-game.png")
      set_tests_properties(gui_smoothing_game PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: HUD smoothing applies live")
      # Options > GAME DATA FOLDER through real input and a fake picker (invalid, cancel,
      # valid + restart prompt, LATER, RESTART NOW, the locked form), and the menu's own
      # row greyed by this launch's --data. The test writes the settings file, so it is
      # named outright: SDL_GetPrefPath ignores XDG_* on Windows and macOS.
      add_test(NAME options_datafolder COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_OPTIONS_DATADIR_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/options-datafolder-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/options-datafolder-profile
          TA_SETTINGS_FILE=${CMAKE_CURRENT_BINARY_DIR}/options-datafolder-settings.ini
          $<TARGET_FILE:taclient> --data "${TA_TEST_DATA}" --winsize 1280 960
          --shot "${CMAKE_CURRENT_BINARY_DIR}/options-datafolder.png")
      set_tests_properties(options_datafolder PROPERTIES TIMEOUT 90
          PASS_REGULAR_EXPRESSION "PASS: Options data folder")
      # Options > PIN TO CCDS on two-CCD and one-CCD sysfs fixtures (the test writes them
      # and points TA_CPU_SYSFS at them): offered or not, what it greys, each slider's
      # CCD stats, and the session plan. Nothing is saved or pinned.
      add_test(NAME options_ccd COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_OPTIONS_CCD_TEST=1
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/options-ccd-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/options-ccd-profile
          TA_SETTINGS_FILE=${CMAKE_CURRENT_BINARY_DIR}/options-ccd-settings.ini
          $<TARGET_FILE:taclient> --data "${TA_TEST_DATA}" --winsize 1280 960
          --shot "${CMAKE_CURRENT_BINARY_DIR}/options-ccd.png")
      set_tests_properties(options_ccd PROPERTIES TIMEOUT 90 LABELS quick
          PASS_REGULAR_EXPRESSION "PASS: Options PIN TO CCDS")
      # RESTART NOW end to end: the client execs itself and the new process mounts the
      # new folder (POSIX exec; Windows starts a second process, not testable this way).
      if(Python3_EXECUTABLE AND NOT WIN32)
        add_test(NAME client_restart COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/client_restart_test.py" "$<TARGET_FILE:taclient>" "${TA_TEST_DATA}")
        set_tests_properties(client_restart PROPERTIES TIMEOUT 150)
        # Settings -> Benchmark's PATHFINDING row: a click is saved, and a menu launch
        # with the pick in the settings file runs that pathfinder on both sims.
        add_test(NAME benchmark_pathfinding COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/benchmark_pathfinding_test.py" "$<TARGET_FILE:taclient>" "${TA_TEST_DATA}")
        set_tests_properties(benchmark_pathfinding PROPERTIES TIMEOUT 300)
        # The Benchmark results' SAVE AS JPG, clicked where it is drawn: one JPEG of the
        # window's size in the profile's benchmarks folder and a greyed SAVED button; a
        # folder that cannot be written says so, leaves no file and keeps the button live;
        # a name that is taken, and its -2, are left as they were (the picture is the -3).
        add_test(NAME benchmark_save COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/tools/benchmark_save_test.py" "$<TARGET_FILE:taclient>" "${TA_TEST_DATA}")
        set_tests_properties(benchmark_save PROPERTIES TIMEOUT 300)
      endif()
      # A menu single-player launch whose local server cannot run must come back to
      # the title screen with the reason (not the multiplayer sign-in dialog, not a
      # silent exit); the --shot then captures that screen and exits.
      add_test(NAME menu_launch_failure COMMAND ${CMAKE_COMMAND} -E env
          SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy TA_MENU_PICK=singleplayer
          "TA_LOCAL_SERVER_BIN=${CMAKE_CURRENT_BINARY_DIR}/no-such-taserver"
          XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/menu-launch-failure-profile
          XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/menu-launch-failure-profile
          $<TARGET_FILE:taclient> menu --data "${TA_TEST_DATA}" --winsize 1024 768
          --shot "${CMAKE_CURRENT_BINARY_DIR}/menu-launch-failure.png")
      set_tests_properties(menu_launch_failure PROPERTIES TIMEOUT 60
          PASS_REGULAR_EXPRESSION "menu: launch failed -> notice: could not (run|start) the local server")
      # These isolated UI processes own SDL's DBus initialization; release its
      # global cache at shutdown so leak checks can see engine allocations.
      set_tests_properties(build_controls render_texture_reuse render_atlas_verify render_geometry_ownership render_geometry_stages render_schedule render_shadow_cache render_shadow_atlas render_shadow_atlas_zoom render_shadow_atlas_band render_shadow_atlas_extended render_shadow_atlas_yield render_shadow_atlas_full render_shadow_atlas_off render_distant_lookup render_cpu_budget render_body_arena render_body_arena_still render_body_arena_fail render_unit_index render_unit_index_jump render_pick_index render_pick_index_orders render_pick_index_jump render_build_ghosts render_interactive_cases tactical_dots depth_fx hidden_units retail_fog spectator_health_bars gui_smoothing_menu gui_smoothing_game menu_launch_failure options_datafolder PROPERTIES ENVIRONMENT "SDL_SHUTDOWN_DBUS_ON_QUIT=1")
    endif()
    add_executable(shadow_test tools/shadow_test.cpp)
    target_include_directories(shadow_test PRIVATE src)
    target_compile_definitions(shadow_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(shadow_test PRIVATE ${_ta_sdl2})
    add_test(NAME shadow COMMAND shadow_test)
    add_executable(render_telemetry_test tools/render_telemetry_test.cpp)
    target_include_directories(render_telemetry_test PRIVATE src)
    target_compile_definitions(render_telemetry_test PRIVATE SDL_MAIN_HANDLED)
    # ThreadPool telemetry checks; SDL for the vertex types in client/geometrystages.h.
    target_link_libraries(render_telemetry_test PRIVATE Threads::Threads ${_ta_sdl2})
    add_test(NAME render_telemetry COMMAND render_telemetry_test)
    # Cost-planned worker-pool passes (workstream I): plans follow the estimates and
    # caps; passes equal the serial loop; only requested workers wake; late workers
    # never touch a finished pass.
    add_executable(work_schedule_test tools/work_schedule_test.cpp)
    target_include_directories(work_schedule_test PRIVATE src)
    target_link_libraries(work_schedule_test PRIVATE Threads::Threads)
    add_test(NAME work_schedule COMMAND work_schedule_test)
    # Stable generational ownership of per-unit geometry entries (workstream C).
    # Retained-buffer trim policy (client/cpubudget.h, workstream J).
    add_executable(cpu_budget_test tools/cpu_budget_test.cpp)
    target_include_directories(cpu_budget_test PRIVATE src)
    add_test(NAME cpu_budget COMMAND cpu_budget_test)
    add_executable(geometry_owners_test tools/geometry_owners_test.cpp)
    target_include_directories(geometry_owners_test PRIVATE src)
    target_link_libraries(geometry_owners_test PRIVATE Threads::Threads)
    add_test(NAME geometry_owners COMMAND geometry_owners_test)
    # Staged unit geometry (workstream D): placement/material arithmetic equals the
    # former single pass; partial stage updates equal a complete rebuild.
    add_executable(geometry_stages_test tools/geometry_stages_test.cpp)
    target_include_directories(geometry_stages_test PRIVATE src)
    target_compile_definitions(geometry_stages_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(geometry_stages_test PRIVATE ${_ta_sdl2})
    add_test(NAME geometry_stages COMMAND geometry_stages_test)
    # Shadow silhouette tile reuse (workstream E): a tile is kept exactly when its
    # raster inputs (pre-E expressions) are bit-identical, across every invalidation
    # cause; body-only local inputs are separated from the shadow's.
    add_executable(shadow_tiles_test tools/shadow_tiles_test.cpp)
    target_include_directories(shadow_tiles_test PRIVATE src)
    target_compile_definitions(shadow_tiles_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(shadow_tiles_test PRIVATE ${_ta_sdl2})
    add_test(NAME shadow_tiles COMMAND shadow_tiles_test)
    # Persistent shadow tile allocation (workstream F): the sequential mode equals
    # the former per-frame packer slot for slot; randomized churn never overlaps
    # cells or moves a live one; generational handles; full page reclamation.
    add_executable(shadow_atlas_test tools/shadow_atlas_test.cpp)
    target_include_directories(shadow_atlas_test PRIVATE src)
    add_test(NAME shadow_atlas COMMAND shadow_atlas_test)
    # Unit broad phase (workstream L): grid queries are supersets of brute force, in
    # ascending order; the band-to-world rectangle keeps every unit the renderer's
    # exact float cull accepts, edges included; randomized movement and churn.
    add_executable(unit_index_test tools/unit_index_test.cpp)
    target_include_directories(unit_index_test PRIVATE src)
    add_test(NAME unit_index COMMAND unit_index_test)
    # Workstream M: structure palette sheet layout, sheet-id coordinates and the
    # indexed decode against the baked expansion (pure, no SDL).
    add_executable(palette_sheets_test tools/palette_sheets_test.cpp)
    target_include_directories(palette_sheets_test PRIVATE src)
    add_test(NAME palette_sheets COMMAND palette_sheets_test)
    # ... and the indexed GL program against the baked pages (pixel-identical NEAREST);
    # GL runs need TA_GEOMETRY_TEST_ACCELERATED=1, the ctest checks the software refusal.
    add_executable(palette_indexed_test tools/palette_indexed_test.cpp)
    target_include_directories(palette_indexed_test PRIVATE src)
    target_compile_definitions(palette_indexed_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(palette_indexed_test PRIVATE ${_ta_sdl2})
    add_test(NAME palette_indexed COMMAND palette_indexed_test)
    # Workstream N: native passes against SDL drawing the same commands, with the GL
    # state checked after every pass; GL runs need TA_GEOMETRY_TEST_ACCELERATED=1, the
    # ctest checks the software refusal.
    add_executable(native_pass_test tools/native_pass_test.cpp)
    target_include_directories(native_pass_test PRIVATE src)
    target_compile_definitions(native_pass_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(native_pass_test PRIVATE ${_ta_sdl2})
    add_test(NAME native_pass COMMAND native_pass_test)
    add_executable(geometrysubmit_test tools/geometrysubmit_test.cpp)
    target_include_directories(geometrysubmit_test PRIVATE src)
    target_compile_definitions(geometrysubmit_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(geometrysubmit_test PRIVATE ${_ta_sdl2})
    add_test(NAME geometrysubmit COMMAND geometrysubmit_test)
    # Workstream H: the retained vertex arena planner against a simulated buffer.
    add_executable(vertex_arena_test tools/vertex_arena_test.cpp)
    target_include_directories(vertex_arena_test PRIVATE src)
    target_compile_definitions(vertex_arena_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(vertex_arena_test PRIVATE ${_ta_sdl2})
    add_test(NAME vertex_arena COMMAND vertex_arena_test)
    add_executable(fogupload_test tools/fogupload_test.cpp)
    target_include_directories(fogupload_test PRIVATE src)
    target_compile_definitions(fogupload_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(fogupload_test PRIVATE ${_ta_sdl2})
    add_test(NAME fogupload COMMAND fogupload_test)
    set_tests_properties(fogupload PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    add_executable(audio_device_test tools/audio_device_test.cpp src/client/options.cpp src/client/settings.cpp
      src/client/hotkeys.cpp)
    target_compile_definitions(audio_device_test PRIVATE SDL_MAIN_HANDLED
      SDL_GetNumAudioDevices=TA_Test_GetNumAudioDevices
      SDL_OpenAudioDevice=TA_Test_OpenAudioDevice)
    target_link_libraries(audio_device_test PRIVATE ta-uifont ta-formats ${_ta_sdl2})
    add_test(NAME audio_device COMMAND audio_device_test)
    add_executable(renderdriver_test tools/renderdriver_test.cpp)
    target_include_directories(renderdriver_test PRIVATE src)
    target_compile_definitions(renderdriver_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(renderdriver_test PRIVATE ${_ta_sdl2})
    add_test(NAME renderdriver COMMAND renderdriver_test)
    add_executable(menu_music_test tools/menu_music_test.cpp)
    target_compile_definitions(menu_music_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(menu_music_test PRIVATE ta-music ta-formats ${_ta_sdl2})
        if(TA_TEST_DATA)
      add_test(NAME menu_music_roster COMMAND menu_music_test "${TA_TEST_DATA}")
    endif()
    add_executable(death_sound_test tools/death_sound_test.cpp)
    target_compile_definitions(death_sound_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(death_sound_test PRIVATE ta-music ta-formats ${_ta_sdl2})
    add_test(NAME death_sound COMMAND death_sound_test)
                        add_executable(game_music_test tools/game_music_test.cpp)
    target_compile_definitions(game_music_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(game_music_test PRIVATE ta-music ta-formats ${_ta_sdl2})
        add_executable(sound_hit_test tools/sound_hit_test.cpp)
    target_compile_definitions(sound_hit_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(sound_hit_test PRIVATE ta-music ta-formats ${_ta_sdl2})
    add_test(NAME sound_hit COMMAND sound_hit_test)
    if(TA_TEST_DATA)
      add_test(NAME sound_hit_data COMMAND sound_hit_test "${TA_TEST_DATA}")
    endif()
    add_executable(campaign_audio_test tools/campaign_audio_test.cpp)
    target_compile_definitions(campaign_audio_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(campaign_audio_test PRIVATE ta-music ta-formats ${_ta_sdl2})
    add_test(NAME campaign_audio COMMAND campaign_audio_test)
    add_executable(terrain_cache_test tools/terrain_cache_test.cpp src/client/mapview.cpp src/client/terrainchunks.cpp)
    target_include_directories(terrain_cache_test PRIVATE src)
    target_compile_definitions(terrain_cache_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(terrain_cache_test PRIVATE ta-formats ${_ta_sdl2})
    if(TA_TEST_DATA)
      add_test(NAME terrain_cache COMMAND terrain_cache_test "${TA_TEST_DATA}")
      set_tests_properties(terrain_cache PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    endif()
    # The lobby's full-window map view: closing it waits for its terrain workers held at a
    # read, so the data set can be remounted after (build with -fsanitize=thread too).
    add_executable(bigmap_lifetime_test tools/bigmap_lifetime_test.cpp src/client/bigmapview.cpp
      src/cartographer/features.cpp src/client/mapview.cpp src/client/terrainchunks.cpp)
    target_include_directories(bigmap_lifetime_test PRIVATE src)
    target_compile_definitions(bigmap_lifetime_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(bigmap_lifetime_test PRIVATE ta-formats ${_ta_sdl2})
    if(TA_TEST_DATA)
      add_test(NAME bigmap_lifetime COMMAND bigmap_lifetime_test "${TA_TEST_DATA}")
      set_tests_properties(bigmap_lifetime PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    endif()
    # Camera-driven terrain work in a real MapView (workstream K): priority, cancellation
    # with hysteresis, stand-ins under memory pressure, nearby work, edits/resets mid-queue.
    add_executable(terrain_schedule_test tools/terrain_schedule_test.cpp src/client/mapview.cpp src/client/terrainchunks.cpp)
    target_include_directories(terrain_schedule_test PRIVATE src)
    target_compile_definitions(terrain_schedule_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(terrain_schedule_test PRIVATE ta-formats ${_ta_sdl2})
    if(TA_TEST_DATA)
      add_test(NAME terrain_schedule COMMAND terrain_schedule_test "${TA_TEST_DATA}")
      set_tests_properties(terrain_schedule PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy")
    endif()
    # Decoded terrain sections: pinned lifetimes, LRU budget with watermarks,
    # redecode accounting, concurrent pins (synthetic JPEGs, no game data).
    add_executable(terrain_compositor_test tools/terrain_compositor_test.cpp)
    target_include_directories(terrain_compositor_test PRIVATE src)
    target_link_libraries(terrain_compositor_test PRIVATE ta-formats Threads::Threads)
    add_test(NAME terrain_compositor COMMAND terrain_compositor_test)
    # Camera-driven terrain work policy (workstream K): tiers, priority, keep band and
    # level grace, stand-in eviction ranks, the shared upload budget, queue simulations.
    add_executable(terrain_work_test tools/terrain_work_test.cpp)
    target_include_directories(terrain_work_test PRIVATE src)
    add_test(NAME terrain_work COMMAND terrain_work_test)
    # Animated colour-atlas regions: incremental repaint == complete repaint ==
    # the previous repaint-everything scheme (frames, gutters, expiry, late atlas,
    # renderer reset). Software renderer under the dummy video driver.
    add_executable(animated_atlas_test tools/animated_atlas_test.cpp src/client/animatedatlas.cpp)
    target_include_directories(animated_atlas_test PRIVATE src)
    target_compile_definitions(animated_atlas_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(animated_atlas_test PRIVATE ${_ta_sdl2})
    add_test(NAME animated_atlas COMMAND animated_atlas_test)
    set_tests_properties(animated_atlas PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_SHUTDOWN_DBUS_ON_QUIT=1")
    add_executable(distant_models_test tools/distant_models_test.cpp src/client/distantmodels.cpp)
    target_include_directories(distant_models_test PRIVATE src)
    target_compile_definitions(distant_models_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(distant_models_test PRIVATE ${_ta_sdl2})
    add_test(NAME distant_models COMMAND distant_models_test)
    set_tests_properties(distant_models PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_SHUTDOWN_DBUS_ON_QUIT=1")
    add_executable(campaign_presentation_test tools/campaign_presentation_test.cpp
        src/client/campaignscreen.cpp src/client/guiart.cpp)
    target_compile_definitions(campaign_presentation_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(campaign_presentation_test PRIVATE ta-uifont ta-formats ${_ta_sdl2})
    add_test(NAME campaign_presentation COMMAND campaign_presentation_test)
    add_executable(screen_lifecycle_test tools/screen_lifecycle_test.cpp
        src/client/loadscreen.cpp src/client/resultscreen.cpp src/client/campaignscreen.cpp
        src/client/guiart.cpp src/client/cursors.cpp src/video/bink.cpp
        src/client/hotkeys.cpp src/client/settings.cpp)
    target_compile_definitions(screen_lifecycle_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(screen_lifecycle_test PRIVATE ta-uifont ta-music ta-formats ta_ffmpeg ${_ta_sdl2})
    if(TA_TEST_DATA)
      add_test(NAME screen_lifecycle COMMAND screen_lifecycle_test "${TA_TEST_DATA}")
      set_tests_properties(screen_lifecycle PROPERTIES
          ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy" TIMEOUT 60)
      add_test(NAME campaign_presentation_roster COMMAND campaign_presentation_test "${TA_TEST_DATA}")
      add_test(NAME campaign_audio_roster COMMAND campaign_audio_test "${TA_TEST_DATA}")
      add_test(NAME death_sound_roster COMMAND death_sound_test "${TA_TEST_DATA}")
      add_test(NAME game_music_roster COMMAND game_music_test "${TA_TEST_DATA}")
      add_executable(firing_sound_test tools/firing_sound_test.cpp)
      target_compile_definitions(firing_sound_test PRIVATE SDL_MAIN_HANDLED)
      target_link_libraries(firing_sound_test PRIVATE ta-formats ${_ta_sdl2})
      add_test(NAME firing_sound_roster COMMAND firing_sound_test "${TA_TEST_DATA}")
      add_executable(crusades_registry_test tools/crusades_registry_test.cpp)
      target_compile_definitions(crusades_registry_test PRIVATE SDL_MAIN_HANDLED)
      target_link_libraries(crusades_registry_test PRIVATE ta-formats ${_ta_sdl2})
      add_test(NAME crusades_registry COMMAND crusades_registry_test "${TA_TEST_DATA}")
      add_executable(balance_roster_test tools/balance_roster_test.cpp)
      target_compile_definitions(balance_roster_test PRIVATE SDL_MAIN_HANDLED)
      target_link_libraries(balance_roster_test PRIVATE ta-formats ${_ta_sdl2})
      add_test(NAME balance_roster COMMAND balance_roster_test "${TA_TEST_DATA}")
    endif()
                add_executable(model_transform_test tools/model_transform_test.cpp)
    target_include_directories(model_transform_test PRIVATE src)
    target_compile_definitions(model_transform_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(model_transform_test PRIVATE ${_ta_sdl2})
    add_test(NAME model_transform COMMAND model_transform_test)
    add_executable(selection_ring_test tools/selection_ring_test.cpp)
    target_include_directories(selection_ring_test PRIVATE src)
    target_compile_definitions(selection_ring_test PRIVATE SDL_MAIN_HANDLED)
    target_link_libraries(selection_ring_test PRIVATE ta-formats ${_ta_sdl2})
    add_test(NAME selection_ring COMMAND selection_ring_test)
    # The retail-backed COB/3DO trace is an optional local regression. It uses
    # Unicorn and the extracted LIFBIRD assets, so keep it out of environments
    # that only have the C++ build dependencies installed.
    if(TA_TEST_DATA AND EXISTS "${TA_TEST_DATA}/KINGDOMS.icd" AND
       EXISTS "${TA_TEST_EXTRACTED}/scripts/lifbird.cob" AND
       EXISTS "${TA_TEST_EXTRACTED}/objects3d/lifbird.3do")
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import unicorn"
          RESULT_VARIABLE _ta_unicorn_status
          OUTPUT_QUIET ERROR_QUIET)
        if("${_ta_unicorn_status}" STREQUAL "0")
          add_test(NAME native_lifbird_ambient_pose
            COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/tools/re/check_native_lifbird_ambient_pose.py"
              --script-binary "$<TARGET_FILE:retail_script_test>"
              --model-binary "$<TARGET_FILE:model_transform_test>"
              --icd "${TA_TEST_DATA}/KINGDOMS.icd")
          set_tests_properties(native_lifbird_ambient_pose PROPERTIES TIMEOUT 60
            ENVIRONMENT "TA_TEST_EXTRACTED=${TA_TEST_EXTRACTED}")
        endif()
      endif()
    endif()
    # ZONROC's mission-driven BeginFlight pose is separate from the generic
    # mover callback roster. Keep the retail COB/model join available when
    # the local ICD, extracted assets, and Unicorn emulator are installed.
    if(TA_TEST_DATA AND EXISTS "${TA_TEST_DATA}/KINGDOMS.icd" AND
       EXISTS "${TA_TEST_EXTRACTED}/scripts/zonroc.cob" AND
       EXISTS "${TA_TEST_EXTRACTED}/objects3d/zonroc.3do")
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import unicorn"
          RESULT_VARIABLE _ta_zonroc_unicorn_status
          OUTPUT_QUIET ERROR_QUIET)
        if("${_ta_zonroc_unicorn_status}" STREQUAL "0")
          add_test(NAME native_air_transport_animation_pose
            COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/tools/re/check_native_air_transport_animation_pose.py"
              --script-binary "$<TARGET_FILE:retail_script_test>"
              --model-binary "$<TARGET_FILE:model_transform_test>"
              --icd "${TA_TEST_DATA}/KINGDOMS.icd")
          set_tests_properties(native_air_transport_animation_pose PROPERTIES TIMEOUT 60
            ENVIRONMENT "TA_TEST_EXTRACTED=${TA_TEST_EXTRACTED}")
        endif()
      endif()
    endif()
    # The accepted-site BeginLanding producer has a distinct ZONROC wing pose
    # from BeginFlight, so keep its mission-to-COB-to-model join independently
    # repeatable when the local retail assets and emulator are available.
    if(TA_TEST_DATA AND EXISTS "${TA_TEST_DATA}/KINGDOMS.icd" AND
       EXISTS "${TA_TEST_EXTRACTED}/scripts/zonroc.cob" AND
       EXISTS "${TA_TEST_EXTRACTED}/objects3d/zonroc.3do")
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import unicorn"
          RESULT_VARIABLE _ta_zonroc_landing_unicorn_status
          OUTPUT_QUIET ERROR_QUIET)
        if("${_ta_zonroc_landing_unicorn_status}" STREQUAL "0")
          add_test(NAME native_zonroc_landing_pose
            COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/tools/re/check_native_zonroc_landing_pose.py"
              --script-binary "$<TARGET_FILE:retail_script_test>"
              --model-binary "$<TARGET_FILE:model_transform_test>"
              --icd "${TA_TEST_DATA}/KINGDOMS.icd")
          set_tests_properties(native_zonroc_landing_pose PROPERTIES TIMEOUT 60
            ENVIRONMENT "TA_TEST_EXTRACTED=${TA_TEST_EXTRACTED}")
        endif()
      endif()
    endif()
    # CREAERI has no BeginFlight/BeginLanding call-ins; its engine-driven
    # flight-to-ground mover edge animates the propellers and ground occupancy.
    if(TA_TEST_DATA AND EXISTS "${TA_TEST_DATA}/KINGDOMS.icd" AND
       EXISTS "${TA_TEST_EXTRACTED}/scripts/creaeri.cob" AND
       EXISTS "${TA_TEST_EXTRACTED}/objects3d/creaeri.3do")
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND)
        execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import unicorn"
          RESULT_VARIABLE _ta_creaeri_unicorn_status
          OUTPUT_QUIET ERROR_QUIET)
        if("${_ta_creaeri_unicorn_status}" STREQUAL "0")
          add_test(NAME native_creaeri_landing_pose
            COMMAND "${Python3_EXECUTABLE}"
              "${CMAKE_SOURCE_DIR}/tools/re/check_native_creaeri_landing_pose.py"
              --script-binary "$<TARGET_FILE:retail_script_test>"
              --model-binary "$<TARGET_FILE:model_transform_test>")
          set_tests_properties(native_creaeri_landing_pose PROPERTIES TIMEOUT 60
            ENVIRONMENT "TA_TEST_EXTRACTED=${TA_TEST_EXTRACTED}")
        endif()
      endif()
    endif()

    target_link_libraries(taclient PRIVATE ta-formats ta-gpustats ta-credstore ta-voice ${_ta_sdl2})
    # We provide our own main() and call SDL_SetMainReady(). Define SDL_MAIN_HANDLED for
    # the WHOLE target (not just main.cpp) so it applies before ANY header pulls in
    # <SDL.h> -- otherwise, on Windows, SDL_main.h renames our main to SDL_main via a
    # transitive include and the MinGW link fails with "undefined reference to WinMain".
    # TA_PRODUCT: a shipped program. Its Release build keeps SDL's per-user folder
    # off Linux even under XDG_DATA_HOME, which test executables honour (util/prefpath.h).
    target_compile_definitions(taclient PRIVATE SDL_MAIN_HANDLED TA_PRODUCT)
    if(TARGET ta_ffmpeg)
      target_link_libraries(taclient PRIVATE ta_ffmpeg)
    else()
      message(STATUS "FFmpeg not found — menu .bik videos disabled (GAF door art fallback).")
    endif()
    if(WIN32)
      # WinMain forwards to our SDL_MAIN_HANDLED main without allocating a console.
      set_target_properties(taclient PROPERTIES WIN32_EXECUTABLE TRUE)
      enable_language(RC)
      configure_file(packaging/windows/taclient.rc.in
                     "${CMAKE_CURRENT_BINARY_DIR}/taclient.rc" @ONLY)
      target_sources(taclient PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/taclient.rc")
      set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/taclient.rc" PROPERTIES
          OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/icons/taclient.ico")
    endif()

            
    add_executable(cartographer_savejob_test tools/cartographer_savejob_test.cpp
      src/cartographer/savejob.cpp src/cartographer/document.cpp src/cartographer/newmap.cpp
      src/cartographer/sections.cpp src/cartographer/units.cpp)
    target_link_libraries(cartographer_savejob_test PRIVATE ta-formats)
    add_test(NAME cartographer_savejob COMMAND cartographer_savejob_test)

            
                # A map scenario under the CRUSADES roster (skipped custom types, the named refusal,
    # Check Map's warning). Needs the retail unitscb/, so only with -DTAK_TEST_DATA.
    if(TA_TEST_DATA)
      add_executable(crusades_scenario_test tools/crusades_scenario_test.cpp
        src/cartographer/validation.cpp src/cartographer/triggers.cpp src/cartographer/aiprofiles.cpp
        src/cartographer/document.cpp src/cartographer/units.cpp src/ai/classicprofile.cpp src/ai/builtinprofile.cpp)
      target_link_libraries(crusades_scenario_test PRIVATE ta-formats)
      add_test(NAME crusades_scenario COMMAND crusades_scenario_test "${TA_TEST_DATA}")
    endif()
    # The AI-profile editor's model, checked against the engine's own profile parsers.
    # It links the whole AI, so the levels' limits in play are checked against ai::paramsFor.
                add_executable(cartographer_features_test tools/cartographer_features_test.cpp src/cartographer/features.cpp)
    target_link_libraries(cartographer_features_test PRIVATE ta-formats)
    add_test(NAME cartographer_feature_art COMMAND cartographer_features_test)

    # Cartographer -- the clean-room map editor (docs/cartographer-port.md).
    # Shares the engine's terrain render stack (mapview + ta-formats) so maps
    # look byte-identical to the game.
    add_executable(cartographer src/client/modelview.cpp src/util/relaunch.cpp src/cartographer/savejob.cpp src/cartographer/playtest.cpp src/cartographer/editorfont.cpp src/cartographer/thumbnails.cpp src/cartographer/recovery.cpp src/cartographer/preferences.cpp src/cartographer/ruleedit.cpp src/cartographer/overlay.cpp src/cartographer/generator.cpp src/cartographer/regions.cpp src/cartographer/validation.cpp src/cartographer/history.cpp src/client/settings.cpp src/cartographer/document.cpp src/cartographer/main.cpp src/cartographer/session.cpp src/cartographer/headless.cpp src/cartographer/palette.cpp src/cartographer/canvas.cpp src/cartographer/chrome.cpp src/cartographer/dialogs.cpp src/cartographer/scripting.cpp src/cartographer/symmetrytools.cpp src/cartographer/symmetry.cpp src/cartographer/sections.cpp src/cartographer/sectiongroups.cpp src/cartographer/newmap.cpp src/cartographer/features.cpp src/cartographer/units.cpp src/cartographer/triggers.cpp src/cartographer/aiprofiles.cpp src/cartographer/aiprofileswindow.cpp src/ai/classicprofile.cpp src/ai/builtinprofile.cpp src/client/mapview.cpp src/client/terrainchunks.cpp)
    target_link_libraries(cartographer PRIVATE ta-formats ${_ta_sdl2})
    target_compile_definitions(cartographer PRIVATE SDL_MAIN_HANDLED TA_PRODUCT)   # see taclient
    get_target_property(_cartographer_sources cartographer SOURCES)
    add_executable(cartographer_ui_test tools/cartographer_ui_test.cpp ${_cartographer_sources})
    target_compile_definitions(cartographer_ui_test PRIVATE SDL_MAIN_HANDLED TA_CARTOGRAPHER_TEST)
    target_link_libraries(cartographer_ui_test PRIVATE ta-formats ${_ta_sdl2})
            if(TA_TEST_DATA)
      add_test(NAME cartographer_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}")
      add_test(NAME cartographer_generation_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" generation)
      add_test(NAME cartographer_overlay_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" overlay)
      add_test(NAME cartographer_objects_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" objects)
      add_test(NAME cartographer_model_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" model)
      add_test(NAME cartographer_thumbnails COMMAND cartographer_ui_test "${TA_TEST_DATA}" thumbnails)
      add_test(NAME cartographer_minimap_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" minimap)
      add_test(NAME cartographer_features_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" features)
      add_test(NAME cartographer_resize_save_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" resize-save)
      add_test(NAME cartographer_aiprofile_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" aiprofile)
      add_test(NAME cartographer_aiprofile_mod_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" aiprofile-mod)
      add_test(NAME cartographer_aiprofile_extras_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" aiprofile-extras)
      add_test(NAME cartographer_aiprofile_packs_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" aiprofile-packs)
      add_test(NAME cartographer_aimod_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" aimod-nomap)
      add_test(NAME cartographer_blank_launch_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" blank-launch)
      add_test(NAME cartographer_failed_open_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" failed-open)
      add_test(NAME cartographer_templates_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" templates)
      add_test(NAME cartographer_rules_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" rules)
      add_test(NAME cartographer_regions_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" regions)
      add_test(NAME cartographer_validation_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" validation)
      add_test(NAME cartographer_headless_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" headless)
      add_test(NAME cartographer_symmetry_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" symmetry)
      add_test(NAME cartographer_terrain_clipboard_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" terrain-clipboard)
      add_test(NAME cartographer_sections_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" sections)
      add_test(NAME cartographer_palette_scroll_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" palette-scroll)
      add_test(NAME cartographer_display_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" display)
      add_test(NAME cartographer_datafolder_ui COMMAND cartographer_ui_test "${TA_TEST_DATA}" datafolder)
      set_tests_properties(cartographer_palette_scroll_ui cartographer_display_ui cartographer_datafolder_ui PROPERTIES TIMEOUT 120 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_SHUTDOWN_DBUS_ON_QUIT=1")
      find_package(Python3 COMPONENTS Interpreter QUIET)
      if(Python3_Interpreter_FOUND AND UNIX AND NOT APPLE)
        add_test(NAME cartographer_recovery COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/tools/cartographer_recovery_test.py" "$<TARGET_FILE:cartographer_ui_test>" "${TA_TEST_DATA}")
        set_tests_properties(cartographer_recovery PROPERTIES TIMEOUT 70)
      endif()
      set_tests_properties(cartographer_objects_ui cartographer_model_ui cartographer_templates_ui cartographer_resize_save_ui cartographer_aiprofile_ui cartographer_aiprofile_mod_ui cartographer_aiprofile_extras_ui cartographer_aiprofile_packs_ui cartographer_aimod_ui cartographer_blank_launch_ui cartographer_failed_open_ui cartographer_rules_ui cartographer_ui cartographer_generation_ui cartographer_overlay_ui cartographer_regions_ui cartographer_features_ui cartographer_minimap_ui cartographer_thumbnails cartographer_validation_ui cartographer_headless_ui cartographer_symmetry_ui cartographer_terrain_clipboard_ui cartographer_sections_ui PROPERTIES TIMEOUT 90 ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_SHUTDOWN_DBUS_ON_QUIT=1")
    endif()
    # Placement and robustness: every browser item placed through the real click
    # path, the STARTS crash regression, every feature operation and a UI smoke pass.
    add_executable(cartographer_placement_test tools/cartographer_placement_test.cpp ${_cartographer_sources})
    target_compile_definitions(cartographer_placement_test PRIVATE SDL_MAIN_HANDLED TA_CARTOGRAPHER_TEST)
    target_link_libraries(cartographer_placement_test PRIVATE ta-formats ${_ta_sdl2})
    if(TA_TEST_DATA)
      add_test(NAME cartographer_starts_ui COMMAND cartographer_placement_test "${TA_TEST_DATA}" starts)
      add_test(NAME cartographer_browser_rescan COMMAND cartographer_placement_test "${TA_TEST_DATA}" rescan)
      add_test(NAME cartographer_place_slots COMMAND cartographer_placement_test "${TA_TEST_DATA}" slots)
      add_test(NAME cartographer_place_features COMMAND cartographer_placement_test "${TA_TEST_DATA}" features)
      add_test(NAME cartographer_place_units COMMAND cartographer_placement_test "${TA_TEST_DATA}" units)
      add_test(NAME cartographer_place_blocked COMMAND cartographer_placement_test "${TA_TEST_DATA}" blocked)
      set(_cartographer_placement_tests cartographer_starts_ui cartographer_browser_rescan cartographer_place_slots
          cartographer_place_features cartographer_place_units cartographer_place_blocked)
      add_test(NAME cartographer_ui_smoke COMMAND cartographer_placement_test "${TA_TEST_DATA}" smoke)
      list(APPEND _cartographer_placement_tests cartographer_ui_smoke)
      foreach(_world aramon veruna taros zhon creon)
        add_test(NAME cartographer_place_terrain_${_world} COMMAND cartographer_placement_test "${TA_TEST_DATA}" terrain ${_world})
        add_test(NAME cartographer_feature_ops_${_world} COMMAND cartographer_placement_test "${TA_TEST_DATA}" feature-ops ${_world})
        list(APPEND _cartographer_placement_tests cartographer_place_terrain_${_world} cartographer_feature_ops_${_world})
      endforeach()
      set_tests_properties(${_cartographer_placement_tests} PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77 LABELS cartographer
        ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_SHUTDOWN_DBUS_ON_QUIT=1")
    endif()
    if(WIN32)
      set_target_properties(cartographer PROPERTIES WIN32_EXECUTABLE TRUE)
      configure_file(packaging/windows/cartographer.rc.in
                     "${CMAKE_CURRENT_BINARY_DIR}/cartographer.rc" @ONLY)
      target_sources(cartographer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/cartographer.rc")
      set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/cartographer.rc" PROPERTIES
          OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/icons/cartographer.ico")
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

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME crowdbench_matrix COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/crowdbench_matrix_test.py")
  add_test(NAME render_benchmark_tool COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/render_benchmark_test.py")
  # AR-11: crowdbench `unreachable` 200x1, per-unit approach arrival and order completion; every
  # order completes on arrival at its unit's nearest reachable spot, all by tick 3000 (W2 step 2).
  add_test(NAME legion_unreachable_orders COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/legion_unreachable_test.py" "$<TARGET_FILE:crowdbench>" --require-w2)
  set_tests_properties(legion_unreachable_orders PROPERTIES LABELS "legion;w2" TIMEOUT 300)
  # One passage, one admission (2026-10-19): crowdbench `opposingbridges` 100x1, half of a lane's bodies sent each
  # way through a 6-cell gap 64 cells long. The gap crosses two sector boundaries, two opening records with a token
  # each: each stream took the record at its own end and both were admitted into the one strip (0 of 100 arrived,
  # all 100 held). One record now holds the admission of the passage; at least 90 arrive.
  add_test(NAME legion_passage_records COMMAND crowdbench --mode legion --scenario opposingbridges --units 100
           --players 1 --moving-percent 100 --ticks 6000 --seed 0)
  set_tests_properties(legion_passage_records PROPERTIES LABELS legion TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "\"physical_in_goal\":(9[0-9]|100),")
  add_test(NAME navigation_determinism COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/navigation_determinism_test.py"
    "$<TARGET_FILE:navigation_checkpoint_test>"
    "${CMAKE_SOURCE_DIR}/tools/navigation_checkpoints.json")
  set_tests_properties(navigation_determinism PROPERTIES TIMEOUT 1200)
endif()
# ALLOW SPECTATORS as a real MpClient sees it (scripted loopback server, no data).
add_executable(spectator_client_test tools/spectator_client_test.cpp)
target_link_libraries(spectator_client_test PRIVATE ta-formats)
add_test(NAME spectator_client COMMAND spectator_client_test)
add_executable(voice_client_test tools/voice_client_test.cpp)
target_link_libraries(voice_client_test PRIVATE ta-formats)
add_test(NAME voice_client COMMAND voice_client_test)
add_executable(voice_uplink_test tools/voice_uplink_test.cpp)
target_link_libraries(voice_uplink_test PRIVATE ta-formats)
add_test(NAME voice_uplink COMMAND voice_uplink_test)
set_tests_properties(voice_uplink PROPERTIES TIMEOUT 150)
set_tests_properties(spectator_client PROPERTIES TIMEOUT 60)
# A room's messages count only while the server has the client in that room (MpClient
# against a scripted loopback server, no data): a table that arrives after LEAVE opens no
# room, a request taken back opens none, and each real way in still takes its first table.
add_executable(room_scope_test tools/room_scope_test.cpp)
target_link_libraries(room_scope_test PRIVATE ta-formats)
add_test(NAME room_scope COMMAND room_scope_test)
set_tests_properties(room_scope PROPERTIES TIMEOUT 60)
# TA_PACELOG's summary (src/client/pacelog.h, header-only).
add_executable(pacelog_test tools/pacelog_test.cpp)
target_include_directories(pacelog_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
add_test(NAME pacelog COMMAND pacelog_test)
add_executable(replay_file_test tools/replay_file_test.cpp src/client/replayfile.cpp)
target_link_libraries(replay_file_test PRIVATE ta-formats)
add_test(NAME replay_file COMMAND replay_file_test)
# The streamed replay writer and the recovery of a recording a crash left unfinished.
# A room's seats when its capacity changes (net/roomseats.h, protocol 323).
add_executable(room_seats_test tools/room_seats_test.cpp)
target_link_libraries(room_seats_test PRIVATE ta-formats)
add_test(NAME room_seats COMMAND room_seats_test)
# What a guest sees of the host's room changes (client/roomchanges.h): the flashes and notes.
add_executable(room_changes_test tools/room_changes_test.cpp src/client/roomchanges.cpp)
target_link_libraries(room_changes_test PRIVATE ta-formats)
add_test(NAME room_changes COMMAND room_changes_test)

# The skirmish AI (src/ai) is SERVER-ONLY -- it never links into taclient, so the
# client cannot run AI locally. AIs run only on the server.
# Exclusive process lease on a path: the ACME state directory's lock, so two
# servers never share one certificate store.
add_library(ta-filelease STATIC src/server/filelease.cpp)
target_include_directories(ta-filelease PUBLIC src)
add_executable(filelease_test tools/filelease_test.cpp)
target_link_libraries(filelease_test PRIVATE ta-filelease)
if(WIN32)
  target_link_libraries(filelease_test PRIVATE shell32)
endif()
add_test(NAME filelease COMMAND filelease_test)
set_tests_properties(filelease PROPERTIES TIMEOUT 30)
add_library(ta-acme STATIC src/server/acme.cpp)
target_include_directories(ta-acme PRIVATE "${TA_FFMPEG_PREFIX}/include")
target_link_libraries(ta-acme PUBLIC ta-formats ta-filelease)
add_executable(acme_test tools/acme_test.cpp)
target_link_libraries(acme_test PRIVATE ta-acme)
target_include_directories(acme_test PRIVATE "${TA_FFMPEG_PREFIX}/include")
add_test(NAME acme COMMAND acme_test)
set_tests_properties(acme PROPERTIES TIMEOUT 30)
add_executable(taserver src/server/server.cpp src/server/status.cpp src/server/accounts.cpp src/server/roomtick.cpp src/server/rosterdump.cpp ${TA_AI_SOURCES})
target_link_libraries(taserver PRIVATE ta-acme ta-formats)
if(Python3_Interpreter_FOUND)
  add_test(NAME server_status_cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_SOURCE_DIR}/tools/server_status_test.py" --server "$<TARGET_FILE:taserver>")
  set_tests_properties(server_status_cli PROPERTIES TIMEOUT 30)
  # taserver --sim-threads <auto|N>: auto and a count are accepted, 0, negatives and junk are a
  # usage error; with game data, each game start logs the threads its referee sim runs on (its
  # share of the total: every usable thread for auto, the machine's most for a bigger count)
  # and several running games share the total, never fewer than 2 threads each.
  # --sim-cpus <list|auto|none>: junk is a usage error, and with a list each running game's
  # line names the CPUs of its turn; a number the server may not use is said at startup
  # and left out of the line.
  if(TA_TEST_DATA)
    add_test(NAME server_sim_threads COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_sim_threads_test.py" --server "$<TARGET_FILE:taserver>"
      --data "${TA_TEST_DATA}")
  else()
    add_test(NAME server_sim_threads COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_sim_threads_test.py" --server "$<TARGET_FILE:taserver>")
  endif()
  set_tests_properties(server_sim_threads PROPERTIES TIMEOUT 240)
endif()
if(TA_TEST_DATA)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME mod_transfer COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/mod_transfer_test.py"
      --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:mod_transfer_test>" --data "${TA_TEST_DATA}")
    set_tests_properties(mod_transfer PROPERTIES TIMEOUT 150)
    # The game data folder is only read: a host-only map through a read-only server
    # (default per-user cache) and --play-map on a read-only install (hpi::userMapRoot).
    if(TARGET taclient)
      # A Release taclient off Linux ignores XDG_DATA_HOME (util/prefpath.h) and would
      # write the player's real profile, so there the --play-map half is skipped.
      set(map_cache_readonly_flags "")
      if((WIN32 OR APPLE) AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        set(map_cache_readonly_flags --no-play-map)
      endif()
      add_test(NAME map_cache_readonly COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/map_cache_readonly_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>"
        --map-tool "$<TARGET_FILE:map_transfer_test>" --data "${TA_TEST_DATA}" ${map_cache_readonly_flags})
      set_tests_properties(map_cache_readonly PROPERTIES TIMEOUT 300)
    endif()
    # A game mod's ai/n.txt (the real mod transfer) or the map's (its .kmp, the map
    # package) reaches the server's Normal AI only, the game mod's winning; misplaced
    # n.txt files (pack root, cosmetic, base) are not read
    # (ai/builtinprofile.h); needs the debug client's --mphost harness and the roster dump.
    if(TARGET taclient AND CMAKE_BUILD_TYPE STREQUAL "Debug")
      add_test(NAME ai_mod_profile COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/ai_mod_profile_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --hpitool "$<TARGET_FILE:hpitool>"
        --data "${TA_TEST_DATA}")
      set_tests_properties(ai_mod_profile PROPERTIES TIMEOUT 900)
      # The Classic AI reads ai/default.txt from the room's layers: game mod over map
      # package (a single-map archive's own file) over base; a mission plays Classic.
      add_test(NAME classic_ai_map_profile COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/classic_ai_map_profile_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --hpitool "$<TARGET_FILE:hpitool>"
        --data "${TA_TEST_DATA}")
      set_tests_properties(classic_ai_map_profile PROPERTIES TIMEOUT 900)
      # Cartographer --bundle maps carrying a DEFAULT profile and ai/n.txt: the server
      # plays each map's own files (two colliding maps), a ticked pack's files win, and
      # the stock map's hash is unchanged.
      if(TARGET cartographer)
        add_test(NAME cartographer_ai_profile_e2e COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/tools/cartographer_ai_profile_e2e_test.py"
          --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>"
          --cartographer "$<TARGET_FILE:cartographer>" --hpitool "$<TARGET_FILE:hpitool>" --data "${TA_TEST_DATA}")
        set_tests_properties(cartographer_ai_profile_e2e PROPERTIES TIMEOUT 600)
      endif()
    endif()
    add_test(NAME server_command_flood COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_command_flood_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_command_flood PROPERTIES TIMEOUT 60)
    add_test(NAME server_power_codes COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_power_codes_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_power_codes PROPERTIES TIMEOUT 120)
    add_test(NAME server_orders_network COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_orders_network_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_orders_network PROPERTIES TIMEOUT 120)
    add_test(NAME server_seed COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_seed_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_seed PROPERTIES TIMEOUT 120)
    add_test(NAME server_speed_pacing COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_speed_pacing_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_speed_pacing PROPERTIES TIMEOUT 120)
    add_test(NAME server_public COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_public_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}" --tls-tool "$<TARGET_FILE:tls_test>" --map-tool "$<TARGET_FILE:map_transfer_test>")
    set_tests_properties(server_public PROPERTIES TIMEOUT 120)
    # The server's game limits refuse a request and keep the connection: CreateGame at
    # --max-games is JoinResult(0), START at --max-running-games a start refusal.
    add_test(NAME server_limits COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_limits_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_limits PROPERTIES TIMEOUT 120)
    add_test(NAME server_spectators COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_spectators_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_spectators PROPERTIES TIMEOUT 120)
    # The end of a game nobody is seated in: a decided match with a human seated plays on;
    # with none the server ends it on that tick, tells the watchers (Msg::GameOver) and its
    # replay ends there; abandoned games tell their watcher; a watcher still catching up
    # gets the whole log before the room goes, which meanwhile is no longer listed or
    # counted as a running game.
    add_test(NAME server_endgame COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_endgame_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}" --map-tool "$<TARGET_FILE:map_transfer_test>")
    set_tests_properties(server_endgame PROPERTIES TIMEOUT 240)
    # The disconnect wait (protocol 289): a silent or dropped seat stops the game, then a
    # server Normal AI takes it over; resume, reconnect, an explicit leave, the per-seat
    # budget counted from the last contact, short holds charged past a 1 s grace.
    add_test(NAME server_disconnect COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_disconnect_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_disconnect PROPERTIES TIMEOUT 300)
    # Host powers: a spectator host's room goes with it (no orphan lobbies), the host's seat
    # keeps them across a reconnect and passes them on when it goes, a reconnect is told the
    # speed, pause and host as they are now, a LOCAL SERVER's game closes with its host.
    foreach(tak_case lobby authority speed lan)
      add_test(NAME server_host_${tak_case} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/server_host_test.py"
        --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}" --case ${tak_case})
      set_tests_properties(server_host_${tak_case} PROPERTIES TIMEOUT 180)
    endforeach()
    add_test(NAME host_session COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/host_session_test.py"
      --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:host_session_test>" --data "${TA_TEST_DATA}")
    set_tests_properties(host_session PROPERTIES TIMEOUT 180)
    # Silence timeouts around game start: a seat that is loading gets 30 s, play keeps 15 s.
    add_test(NAME server_load_timeout COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_load_timeout_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_load_timeout PROPERTIES TIMEOUT 120)
    add_test(NAME server_seat_move COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_seat_move_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_seat_move PROPERTIES TIMEOUT 120)
    # Room editing (protocol 323): the host changes the map, rules, name and seats in the
    # lobby; ready guests ready again; map changes race transfers (map_transfer_test --room-edit).
    add_test(NAME server_room_edit COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_room_edit_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}" --map-tool "$<TARGET_FILE:map_transfer_test>")
    set_tests_properties(server_room_edit PROPERTIES TIMEOUT 180)
    # AI levels through the lobby (protocol 290): RETAIL (6, Classic) round-trips, 7+ store Normal.
    add_test(NAME server_ai_level COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_ai_level_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_ai_level PROPERTIES TIMEOUT 120)
    add_test(NAME server_voice COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_voice_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    set_tests_properties(server_voice PROPERTIES TIMEOUT 120)
    # Voice to a listener on a slow link (and, --case cpu, to a slow machine, which keeps it).
    add_test(NAME server_voice_slow COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_voice_slow_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}")
    add_test(NAME server_voice_cpu COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_voice_slow_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}" --case cpu)
    set_tests_properties(server_voice_slow server_voice_cpu PROPERTIES TIMEOUT 120)
    add_test(NAME server_map_scenario COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_map_scenario_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}" --map-tool "$<TARGET_FILE:map_transfer_test>")
    set_tests_properties(server_map_scenario PROPERTIES TIMEOUT 180)
    add_test(NAME server_status COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/server_status_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}" --tls-tool "$<TARGET_FILE:tls_test>")
    set_tests_properties(server_status PROPERTIES TIMEOUT 120)
    # Account login (SCRAM): register, log in, a wrong proof, a pending registration.
    # Sign-in tokens over TLS, with the real client (authrogue --live).
    add_test(NAME account_auth_network COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/tools/account_auth_network_test.py"
      --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}"
      --tls-tool "$<TARGET_FILE:tls_test>" --client-tool "$<TARGET_FILE:authrogue>")
    set_tests_properties(account_auth_network PROPERTIES TIMEOUT 120)
    if(TARGET taclient AND CMAKE_BUILD_TYPE STREQUAL "Debug")
      # Replays are saved however a game ends: crash, kill, play on after the result,
      # SIGTERM, a dead server (the headless harness is debug-only).
      add_test(NAME replay_always COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/replay_always_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --data "${TA_TEST_DATA}")
      set_tests_properties(replay_always PROPERTIES TIMEOUT 300)
      # A watched game the server ends (no human seated at its result): an all-AI game
      # ends on the tick it is decided, one whose player left moments after; the watcher
      # plays it to its last tick, gets the winner's result plate after three seconds, and
      # its recording and the server's replay end on that tick and play back to the same state.
      add_test(NAME watch_end COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/watch_end_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --data "${TA_TEST_DATA}")
      set_tests_properties(watch_end PROPERTIES TIMEOUT 600)
      # Replay seeking end to end: every landing and the end of the game hash as a straight
      # run does, on Retail, Legion, a campaign mission, a .crt scenario and a spectator's recording.
      add_test(NAME replay_seek COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/replay_always_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --data "${TA_TEST_DATA}"
        --case seek)
      set_tests_properties(replay_seek PROPERTIES TIMEOUT 600)
      # A map that cannot build ends a headless --mphost run quickly with a message (it used to spin).
      add_test(NAME harness_createfail COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/harness_createfail_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --data "${TA_TEST_DATA}"
        --hpitool "$<TARGET_FILE:hpitool>")
      set_tests_properties(harness_createfail PROPERTIES TIMEOUT 150)
      # The lobby's map previews (generated-map rolls, the full-window view) are held at a
      # read of the data set as the game starts and as the view is destroyed: the data set
      # must outlive them, and the games then play normally.
      add_test(NAME preview_lifetime COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/preview_lifetime_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --data "${TA_TEST_DATA}")
      set_tests_properties(preview_lifetime PROPERTIES TIMEOUT 240)
      # The game room driven through its own buttons (TA_ROOM_SCRIPT): seats, map changes,
      # rules, WATCH, the multiplayer unready, each flow to a started game.
      add_test(NAME room_flow COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/room_flow_test.py"
        --client "$<TARGET_FILE:taclient>" --server "$<TARGET_FILE:taserver>" --data "${TA_TEST_DATA}"
        --hpitool "$<TARGET_FILE:hpitool>")
      set_tests_properties(room_flow PROPERTIES TIMEOUT 600)
      # A slowly simulating harness client (a stress game under load) keeps its seat, and a
      # concluded run reports exactly its result tick.
      add_test(NAME harness_seat_load COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/harness_seat_load_test.py"
        --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --data "${TA_TEST_DATA}")
      set_tests_properties(harness_seat_load PROPERTIES TIMEOUT 300)
      if(NOT WIN32)
        # The disconnect wait end to end (SIGSTOP): AI takeover with no desync and a replay
        # that verifies, resume and reconnect inside the wait ending on the same hash.
        add_test(NAME disconnect_client COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/tools/disconnect_client_test.py"
          --server "$<TARGET_FILE:taserver>" --client "$<TARGET_FILE:taclient>" --data "${TA_TEST_DATA}")
        set_tests_properties(disconnect_client PROPERTIES TIMEOUT 300)
      endif()
    endif()
  endif()
endif()

add_executable(modeltool tools/modeltool.cpp)
target_link_libraries(modeltool PRIVATE ta-formats)

add_executable(tdftool tools/tdftool.cpp)
target_link_libraries(tdftool PRIVATE ta-formats)

add_executable(cobtool tools/cobtool.cpp)
target_link_libraries(cobtool PRIVATE ta-formats)

# Dev-tool command lines: usage on bad arguments, Cartographer --version/--help, simperf's default map.
if(Python3_Interpreter_FOUND)
  set(CLI_TOOLS_ARGS --cobtool "$<TARGET_FILE:cobtool>" --modeltool "$<TARGET_FILE:modeltool>"
    --gaftool "$<TARGET_FILE:gaftool>" --tnttool "$<TARGET_FILE:tnttool>" --tdftool "$<TARGET_FILE:tdftool>"
    --hpitool "$<TARGET_FILE:hpitool>" --simperf "$<TARGET_FILE:simperf>" --data "${TA_TEST_DATA}")
  if(TARGET cartographer)
    list(APPEND CLI_TOOLS_ARGS --cartographer "$<TARGET_FILE:cartographer>")
  endif()
    endif()

# --- Packaging: native .deb + .rpm via CPack (Linux) --------------------------
# One source of truth for both package families. Build, then from the build dir:
#   cpack -G DEB   -> ta-engine-<version>-{amd64,arm64}.deb
#   cpack -G RPM   -> ta-engine-<version>-{x86_64,aarch64}.rpm
# Every build bakes SDL2/jpeg/zlib and the Bink FFmpeg in, so SHLIBDEPS/AUTOREQ below
# auto-detect only the C runtime.
# SDL2 still dlopens its X11/Wayland/audio backends, which every desktop that can run the
# game already has; those are not declared (they aren't link-time deps of a static SDL2).
if(UNIX AND NOT APPLE)
  # Ship the service without enabling it. ACME is an opt-in, edited drop-in.
  install(FILES packaging/systemd/taserver.service DESTINATION lib/systemd/system)
  install(FILES packaging/systemd/taserver-acme.conf packaging/systemd/taserver-settings.conf
                packaging/systemd/README.md
          DESTINATION share/doc/ta-engine/systemd)
  install(FILES STREAMING-LICENSES.txt EDITOR-FONT-LICENSE.txt CHAT-FONT-LICENSE.txt TITLE-FONT-LICENSE.txt ACME-LICENSES.txt OPUS-LICENSE.txt DESTINATION share/licenses/ta-engine)
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
          DESTINATION share/applications)
  install(FILES res/linux/us.pgnet.ta_engine.metainfo.xml DESTINATION share/metainfo)

  set(CPACK_PACKAGE_NAME "ta-engine")
  set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
      "Clean-room engine recreation of Total Annihilation")
  # The long description, vendor and packager are what package installers show
  # (CPack's defaults read "Humanity" and "This is an installer created using CPack").
  set(CPACK_PACKAGE_DESCRIPTION
      "TA Engine is a modern, clean-room re-implementation of Total Annihilation (Cavedog, 1997), written in C++20 with SDL2. It plays the original campaign, skirmishes against the computer and online multiplayer, and includes Cartographer, a port of the original map editor, and taserver, a dedicated multiplayer server.\n\nThe game's data is not included: point the engine at your own retail Total Annihilation install.")
  set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/pocketgeek/ta-engine")
  set(CPACK_PACKAGE_VENDOR "Curtis Edge")
  set(CPACK_PACKAGE_CONTACT "Curtis Edge <pocket_geek@pgnet.us>")
  set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_PACKAGE_DESCRIPTION}")
  set(CPACK_RPM_PACKAGE_VENDOR "Curtis Edge")
  set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
  set(CPACK_RPM_SPEC_MORE_DEFINE "%define packager Curtis Edge <pocket_geek@pgnet.us>")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Curtis Edge <pocket_geek@pgnet.us>")
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
  include(cmake/LinuxPackageArchitecture.cmake)
  tak_linux_package_architecture("${CMAKE_SYSTEM_PROCESSOR}" _ta_deb_arch _ta_rpm_arch)
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${_ta_deb_arch}")
  set(CPACK_RPM_PACKAGE_ARCHITECTURE "${_ta_rpm_arch}")
  set(CPACK_DEBIAN_FILE_NAME "ta-engine-${PROJECT_VERSION}-${_ta_dtag}${_ta_deb_arch}.deb")
  set(CPACK_RPM_FILE_NAME "ta-engine-${PROJECT_VERSION}-${_ta_dtag}${_ta_rpm_arch}.rpm")

  # Auto-detect the shared-library dependencies for each family.
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_RPM_PACKAGE_AUTOREQ ON)
  set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")

  include(CPack)
elseif(WIN32)
  install(FILES STREAMING-LICENSES.txt EDITOR-FONT-LICENSE.txt CHAT-FONT-LICENSE.txt TITLE-FONT-LICENSE.txt ACME-LICENSES.txt OPUS-LICENSE.txt DESTINATION .)
  # Windows installer (NSIS via CPack; the CI's MSYS2 job runs `cpack -G NSIS`).
  # Binaries install into the chosen folder root (no bin/), with Start-menu
  # shortcuts and an uninstaller. The static build has no DLLs to carry.
  install(TARGETS taserver RUNTIME DESTINATION .)
  if(TARGET taclient)
    install(TARGETS taclient RUNTIME DESTINATION .)
  endif()

  set(TA_PACKAGE_CARTOGRAPHER OFF)
  if(TARGET cartographer)
    install(TARGETS cartographer RUNTIME DESTINATION .)
    set(TA_PACKAGE_CARTOGRAPHER ON)
  endif()
  include(packaging/windows/installer.cmake)

  include(CPack)
elseif(APPLE)
  install(TARGETS taserver RUNTIME DESTINATION bin)
endif()

# Per-user files. Every test gets its own XDG_DATA_HOME, XDG_CONFIG_HOME and (Linux)
# HOME under test-profiles/<test>/, unless its ENVIRONMENT names that variable itself,
# so whatever a test writes to the per-user folder (util/prefpath.h: settings.ini,
# replays, Cartographer's preferences and recovery files) stays in the build dir. The
# per-user folder follows XDG_DATA_HOME on every platform in Debug builds and test
# executables; SDL alone honours it only on Linux. On 2026-10-14 diplomacy_ui and four
# more taclient tests, which set no XDG_DATA_HOME, saved their --data folder as the
# player's dataDir in the real ~/.local/share/TAengine/TAEngine/settings.ini. A new
# test needs nothing to be safe. The tak_prefs fixture brackets every run:
# pref_guard_check fails if anything wrote a TAengine folder into a test's sentinel
# HOME (a process that dropped its XDG_DATA_HOME, i.e. the real profile outside ctest);
# pref_guard_lint fails on an SDL_GetPrefPath call outside prefpath.h;
# pref_guard_selftest replays the leak and checks that the guard names it. The defaults
# are applied at the end of this file (cmake_language DEFER), so a test added or an
# ENVIRONMENT set anywhere below is covered too.
set(TA_TEST_PROFILES "${CMAKE_CURRENT_BINARY_DIR}/test-profiles")
set(TA_PREF_GUARD "${CMAKE_SOURCE_DIR}/tools/pref_guard.cmake")
add_test(NAME pref_guard_setup COMMAND ${CMAKE_COMMAND} -DMODE=setup "-DPROFILES=${TA_TEST_PROFILES}" -P "${TA_PREF_GUARD}")
add_test(NAME pref_guard_check COMMAND ${CMAKE_COMMAND} -DMODE=check "-DPROFILES=${TA_TEST_PROFILES}" -P "${TA_PREF_GUARD}")
set_tests_properties(pref_guard_setup PROPERTIES FIXTURES_SETUP tak_prefs)
set_tests_properties(pref_guard_check PROPERTIES FIXTURES_CLEANUP tak_prefs)
add_test(NAME pref_guard_lint COMMAND ${CMAKE_COMMAND} -DMODE=lint "-DSOURCE=${CMAKE_SOURCE_DIR}" -P "${TA_PREF_GUARD}")
if(TARGET taclient AND TA_TEST_DATA AND CMAKE_BUILD_TYPE STREQUAL "Debug" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME pref_guard_selftest COMMAND ${CMAKE_COMMAND} -DMODE=selftest "-DCLIENT=$<TARGET_FILE:taclient>"
           "-DDATA=${TA_TEST_DATA}" "-DROOT=${CMAKE_CURRENT_BINARY_DIR}/pref-guard-selftest" -P "${TA_PREF_GUARD}")
  set_tests_properties(pref_guard_selftest PROPERTIES TIMEOUT 60)
endif()
# A scratch HOME hides the user's own Python modules (unicorn, for the native_*
# emulation checks); keep them importable.
set(ta_python_userbase "")
if(UNIX AND NOT APPLE AND Python3_EXECUTABLE)
  execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import site; print(site.getuserbase())"
                  OUTPUT_VARIABLE ta_python_userbase OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
function(tak_isolate_test_profiles)
  get_property(tak_all_tests DIRECTORY PROPERTY TESTS)
  foreach(ta_test IN LISTS tak_all_tests)
    if(ta_test MATCHES "^pref_guard_(setup|check)$")
      continue()
    endif()
    set(ta_profile "${TA_TEST_PROFILES}/${ta_test}")
    # TA_SIM_PIN=0: a debug client pins no simulation thread, whatever its settings file
    # says (PIN SIMULATION THREADS is OFF by default, but a profile the isolation does
    # not reach may have it ON): the clients of a parallel run would all pin to the same
    # first cores. A test of the pinning turns the setting ON in its own settings file
    # and sets this to 1 itself.
    set(ta_defaults "XDG_DATA_HOME=${ta_profile}/data" "XDG_CONFIG_HOME=${ta_profile}/config"
                     "XDG_CACHE_HOME=${TA_TEST_PROFILES}/cache" "TA_SIM_PIN=0")
    if(UNIX AND NOT APPLE)
      list(APPEND ta_defaults "HOME=${ta_profile}/home")
      if(ta_python_userbase)
        list(APPEND ta_defaults "PYTHONUSERBASE=${ta_python_userbase}")
      endif()
      # The scratch HOME also hides the global git config, where CI records the checkout as a
      # safe.directory; without it git refuses the source tree as "dubious ownership" and
      # legion_doc_links cannot tell that CI's clone is shallow.
      list(APPEND ta_defaults "GIT_CONFIG_COUNT=1" "GIT_CONFIG_KEY_0=safe.directory"
                               "GIT_CONFIG_VALUE_0=${CMAKE_SOURCE_DIR}")
    endif()
    # Plain ENVIRONMENT (not ENVIRONMENT_MODIFICATION), so runners that read only that,
    # like the Windows host's runctest.py, isolate tests too.
    get_test_property(${ta_test} ENVIRONMENT ta_env)
    if(NOT ta_env)
      set(ta_env "")
    endif()
    set(tak_merged "")
    foreach(tak_pair IN LISTS ta_defaults)
      string(REGEX MATCH "^[A-Z_]+" tak_var "${tak_pair}")
      if(NOT ta_env MATCHES "(^|;)${tak_var}=")
        list(APPEND tak_merged "${tak_pair}")
      endif()
    endforeach()
    list(APPEND tak_merged ${ta_env})
    set_property(TEST ${ta_test} PROPERTY ENVIRONMENT ${tak_merged})
    set_property(TEST ${ta_test} APPEND PROPERTY FIXTURES_REQUIRED tak_prefs)
  endforeach()
endfunction()
cmake_language(DEFER CALL tak_isolate_test_profiles)

# Test scheduling and the `quick` label. tools/ctest_costs.txt lists the tests that take
# 5 s or more on an optimized Debug build (`<test> <seconds>`, longest first). Each gets a
# COST so ctest starts the longest first (ctest's own CTestCostData.txt only exists after a
# first run in a build dir; this is the fresh-checkout default and the lower bound that
# keeps a long test from being scheduled last). Every other test that is not `nightly` is
# labelled `quick` (a slow test may also carry the label explicitly, as the retailgap
# slices do): `ctest -L quick -j N` is the targeted run for task agents (about a
# minute of wall time at -j 8; it still covers every area). Refresh the file after adding a
# slow test: tools/ctest_costs.py build-o2 (see docs/development.md).
set(TA_CTEST_COSTS "${CMAKE_SOURCE_DIR}/tools/ctest_costs.txt")
if(EXISTS "${TA_CTEST_COSTS}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TA_CTEST_COSTS}")
  file(STRINGS "${TA_CTEST_COSTS}" tak_cost_lines)
  set(tak_slow_tests "")
  foreach(tak_line IN LISTS tak_cost_lines)
    if(tak_line MATCHES "^([A-Za-z0-9_.+-]+)[ \t]+([0-9.]+)")
      if(TEST ${CMAKE_MATCH_1})
        set_tests_properties(${CMAKE_MATCH_1} PROPERTIES COST ${CMAKE_MATCH_2})
        list(APPEND tak_slow_tests ${CMAKE_MATCH_1})
      endif()
    endif()
  endforeach()
  get_property(tak_all_tests DIRECTORY PROPERTY TESTS)
  foreach(ta_test IN LISTS tak_all_tests)
    if(NOT ta_test IN_LIST tak_slow_tests)
      get_test_property(${ta_test} LABELS tak_labels)
      if(NOT tak_labels MATCHES "nightly")
        set_property(TEST ${ta_test} APPEND PROPERTY LABELS quick)
      endif()
    endif()
  endforeach()
endif()

# Everything that compiles the Options screen (client/options.cpp) also links the music
# module its AUDIO page tries chosen files with.
if(TARGET ta-music)
  foreach(_t audio_device_test options_autosave_test voice_test)
    if(TARGET ${_t})
      target_link_libraries(${_t} PRIVATE ta-music)
    endif()
  endforeach()
endif()

# TA data check: registry_dump <install-dir> [unit...]
add_executable(registry_dump tools/registry_dump.cpp)
target_link_libraries(registry_dump PRIVATE ta-formats)
add_executable(ta_economy_test tools/ta_economy_test.cpp)   # header-only sim arithmetic
target_include_directories(ta_economy_test PRIVATE src)
add_test(NAME ta_economy COMMAND ta_economy_test)
if(TARGET SDL2::SDL2-static OR TARGET SDL2::SDL2)
  # guitool <install-dir> <screen> <out.png>: render a TA .GUI screen offscreen
  add_executable(guitool tools/guitool.cpp src/client/taguiview.cpp)
  target_compile_definitions(guitool PRIVATE SDL_MAIN_HANDLED)
  target_link_libraries(guitool PRIVATE ta-formats ${_ta_sdl2})
endif()
# tagen <install> <world> <out.png>: a TA random map rendered (tnt/mapgen_ta.cpp)
add_executable(tagen tools/tagen.cpp)
target_link_libraries(tagen PRIVATE ta-formats)
# reclaim_test: feature reclaim timing per TotalA.exe 0x404bba (docs/retail-economy.md section 7)
add_executable(reclaim_test tools/reclaim_test.cpp)
add_test(NAME reclaim COMMAND reclaim_test)
target_link_libraries(reclaim_test PRIVATE ta-formats)
# ta_cloak_test: cloak paid from stored energy at the settle (docs/retail-economy.md section 2)
add_executable(ta_cloak_test tools/ta_cloak_test.cpp)
target_link_libraries(ta_cloak_test PRIVATE ta-formats)
add_test(NAME ta_cloak COMMAND ta_cloak_test)

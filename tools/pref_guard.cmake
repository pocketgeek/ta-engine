# Keeps tests off the player's real per-user folder (util/prefpath.h): the game's
# settings.ini, replays and replay cache, Cartographer's preferences and recovery files.
# CMakeLists.txt ("Per-user files") gives every test its own XDG_DATA_HOME,
# XDG_CONFIG_HOME and, on Linux, HOME under <build>/test-profiles/<test>/. That HOME is
# a sentinel: only a process that dropped the test's XDG_DATA_HOME ends up writing
# there -- which, outside ctest, is the real ~/.local/share/TAKengine.
#
#   cmake -DMODE=setup    -DPROFILES=<dir> -P pref_guard.cmake  # clear old sentinels
#   cmake -DMODE=check    -DPROFILES=<dir> -P pref_guard.cmake  # fail on a TAKengine in one
#   cmake -DMODE=lint     -DSOURCE=<repo>  -P pref_guard.cmake  # SDL_GetPrefPath only in prefpath.h
#   cmake -DMODE=selftest -DCLIENT=<takclient> -DDATA=<install> -DROOT=<dir> -P pref_guard.cmake
#
# selftest replays the 2026-10-14 leak: the pre-isolation command line of the
# diplomacy_ui test, which saved its --data folder as the player's dataDir, and passes
# only if `check` names it.
cmake_minimum_required(VERSION 3.24)

# The tests whose sentinel HOME (<root>/<test>/home) holds a TAKengine folder, and the
# files there.
function(tak_pref_leaks root out_tests out_files)
    set(tests "")
    set(files "")
    file(GLOB homes LIST_DIRECTORIES true "${root}/*/home")
    foreach(home IN LISTS homes)
        if(NOT IS_DIRECTORY "${home}")
            continue()
        endif()
        file(GLOB_RECURSE hits LIST_DIRECTORIES true "${home}/*")
        foreach(hit IN LISTS hits)
            if(hit MATCHES "/TAKengine(/|$)")
                get_filename_component(test "${home}" DIRECTORY)
                get_filename_component(test "${test}" NAME)
                list(APPEND tests "${test}")
                if(NOT IS_DIRECTORY "${hit}")
                    list(APPEND files "${hit}")
                endif()
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES tests)
    set(${out_tests} "${tests}" PARENT_SCOPE)
    set(${out_files} "${files}" PARENT_SCOPE)
endfunction()

if(MODE STREQUAL "setup")
    file(GLOB homes LIST_DIRECTORIES true "${PROFILES}/*/home")
    if(homes)
        file(REMOVE_RECURSE ${homes})
    endif()
    file(MAKE_DIRECTORY "${PROFILES}")
    message("pref guard: per-test profiles under ${PROFILES}")
elseif(MODE STREQUAL "check")
    tak_pref_leaks("${PROFILES}" tests files)
    if(tests)
        string(REPLACE ";" "\n  " files "${files}")
        message(FATAL_ERROR "pref guard: these tests wrote a per-user TAKengine folder outside "
            "their own XDG_DATA_HOME -- outside ctest that is the player's real profile:\n  ${tests}\n"
            "files:\n  ${files}")
    endif()
    message("PASS: no test reached a real per-user folder")
elseif(MODE STREQUAL "lint")
    file(GLOB_RECURSE sources "${SOURCE}/src/*.h" "${SOURCE}/src/*.cpp" "${SOURCE}/src/*.c" "${SOURCE}/src/*.mm"
                              "${SOURCE}/tools/*.h" "${SOURCE}/tools/*.cpp")
    set(bad "")
    foreach(source IN LISTS sources)
        if(source MATCHES "/src/util/prefpath\\.h$")
            continue()
        endif()
        file(STRINGS "${source}" calls REGEX "SDL_GetPrefPath[ \t]*\\(")
        if(calls)
            file(RELATIVE_PATH rel "${SOURCE}" "${source}")
            list(APPEND bad "${rel}")
        endif()
    endforeach()
    if(bad)
        string(REPLACE ";" "\n  " bad "${bad}")
        message(FATAL_ERROR "pref guard: call tak::prefPath (src/util/prefpath.h), not SDL_GetPrefPath: "
            "SDL ignores a test's XDG_DATA_HOME on Windows and macOS\n  ${bad}")
    endif()
    message("PASS: every per-user folder lookup goes through tak::prefPath")
elseif(MODE STREQUAL "selftest")
    file(REMOVE_RECURSE "${ROOT}")
    set(home "${ROOT}/diplomacy_ui/home")
    file(MAKE_DIRECTORY "${home}")
    # The leak's environment: no XDG_DATA_HOME, so SDL falls back to $HOME -- here the
    # sentinel, so even this deliberate leak lands in scratch space.
    set(ENV{HOME} "${home}")
    unset(ENV{XDG_DATA_HOME})
    unset(ENV{XDG_CONFIG_HOME})
    unset(ENV{TAK_SETTINGS_FILE})
    set(ENV{SDL_VIDEODRIVER} dummy)
    set(ENV{SDL_AUDIODRIVER} dummy)
    execute_process(COMMAND "${CLIENT}" game "Ulasem Arena" --data "${DATA}" --testbuild --time 0
                            --shot "${ROOT}/diplomacy-test.png"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out TIMEOUT 50)
    set(written "${home}/.local/share/TAKengine/TAKingdoms/settings.ini")
    if(NOT EXISTS "${written}")
        message(FATAL_ERROR "selftest: the leaking client (exit ${rc}) wrote no ${written}; "
            "the sentinel no longer sees this leak\n${out}")
    endif()
    file(STRINGS "${written}" saved REGEX "^dataDir = ")
    if(NOT saved STREQUAL "dataDir = ${DATA}")
        message(FATAL_ERROR "selftest: expected 'dataDir = ${DATA}' in ${written}, found '${saved}'")
    endif()
    tak_pref_leaks("${ROOT}" tests files)
    if(NOT tests STREQUAL "diplomacy_ui")
        message(FATAL_ERROR "selftest: check named '${tests}', not diplomacy_ui")
    endif()
    message("PASS: the guard names diplomacy_ui's leaked settings.ini (${saved})")
else()
    message(FATAL_ERROR "pref_guard.cmake: unknown MODE '${MODE}'")
endif()

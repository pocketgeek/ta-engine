# BuildId.cmake -- regenerate the source fingerprint on EVERY build, not at configure.
#
# Run in script mode by a custom target. Writes <OUT> only when the value actually
# changes, so an unchanged id does not force a rebuild of everything that includes it.
#
# WHY NOT execute_process AT CONFIGURE TIME. That was the first version, and it bakes
# the id in when cmake last ran: commit, rebuild incrementally, and the binary still
# claims the previous commit. A fingerprint that goes quietly stale is worse than none
# -- it is the same false confidence as a test that cannot fail, and it would make the
# sweep's build-id gate pass a genuinely mismatched pair.

find_package(Git QUIET)
set(ID "unknown")
if(GIT_FOUND AND EXISTS "${SRC}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --abbrev=12
                  WORKING_DIRECTORY "${SRC}"
                  OUTPUT_VARIABLE ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(ID STREQUAL "")
    set(ID "unknown")
  endif()
endif()

set(CONTENT "#pragma once\n#define TAK_BUILD_ID \"${ID}\"\n")
set(OLD "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" OLD)
endif()
if(NOT OLD STREQUAL CONTENT)
  file(WRITE "${OUT}" "${CONTENT}")
  message(STATUS "Build id: ${ID}")
endif()

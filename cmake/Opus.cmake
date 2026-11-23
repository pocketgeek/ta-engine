# Pinned Opus release (BSD-3-Clause; OPUS-LICENSE.txt ships beside the binaries), the
# voice chat codec. Built from source on every platform: a static archive compiled by our own toolchain, so no system libopus and no extra DLL
# or dylib. Only the client links it (tak-voice); takserver relays voice as opaque bytes.
enable_language(C)
# CMake appends C's implicit libraries when this archive joins a C++ link. Fedora
# names its shared unwinder gcc_s_asneeded, unlike C++'s gcc_s, so the extra -l can
# override the Linux -static-libgcc policy. Leave the driver to select that runtime
# and retain all other implicit C libraries.
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  list(FILTER CMAKE_C_IMPLICIT_LINK_LIBRARIES EXCLUDE REGEX "^gcc_s(_asneeded)?$")
endif()
include(FetchContent)
# SOURCE_SUBDIR names a directory the tarball does not have, so MakeAvailable only
# downloads and extracts; the add_subdirectory below brings Opus in EXCLUDE_FROM_ALL,
# which keeps its install() rules (libopus.a and headers) out of our packages.
# Two sources for the same tarball (the hash pins it): CI has seen xiph.org refuse
# connections, and FetchContent tries each URL in turn.
FetchContent_Declare(tak_opus
  URL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
      https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz
  URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
  TLS_VERIFY TRUE
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR tak-not-a-directory
)
# For offline builds, set FETCHCONTENT_SOURCE_DIR_TAK_OPUS to an extracted copy of
# opus-1.5.2.tar.gz.
FetchContent_MakeAvailable(tak_opus)

# Opus's own options (normal variables win over its option() defaults, CMP0077).
set(OPUS_BUILD_SHARED_LIBRARY OFF)
set(OPUS_BUILD_TESTING OFF)
set(OPUS_BUILD_PROGRAMS OFF)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF)
# No neural-network extensions: they need large weight tables and more CPU per frame,
# and voice chat at 24 kbit/s does not need them.
set(OPUS_DRED OFF)
set(OPUS_OSCE OFF)
set(OPUS_DEEP_PLC OFF)
if(MINGW)
  # Opus links libssp for these on MinGW, which the fully static Windows build lacks.
  set(OPUS_STACK_PROTECTOR OFF)
  set(OPUS_FORTIFY_SOURCE OFF)
endif()
# Opus registers its tests whenever BUILD_TESTING is on (it is, through our
# enable_testing); keep its directory from adding any to ctest.
set(_tak_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
set(_tak_shared_libs "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
add_subdirectory("${tak_opus_SOURCE_DIR}" "${tak_opus_BINARY_DIR}" EXCLUDE_FROM_ALL)
set(BUILD_TESTING "${_tak_build_testing}")
set(BUILD_SHARED_LIBS "${_tak_shared_libs}")
set_target_properties(opus PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_options(opus PRIVATE -w)   # third-party code; our -Wall -Wextra is not for it

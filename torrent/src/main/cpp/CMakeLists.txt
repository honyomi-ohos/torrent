cmake_minimum_required(VERSION 3.5.0)
project(honyomi_torrent)

# ============================================================================
# libtorrent-rasterbar 2.0.15 (git submodule) + OpenSSL 3.5.9 prebuilt
# + Boost 1.86 headers — BitTorrent download engine, packaged as the
# standalone NAPI module libtorrent.so (module name "torrent").
# torrent_bridge.cpp compiles against the submodule headers; the prebuilt
# static libs (libtorrent-rasterbar.a + libcrypto.a + libssl.a) come from
# prebuilt/<abi>/. Rebuild instructions: see third_party/torrent/README.md.
# ============================================================================

set(TORRENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/torrent)
set(TORRENT_PREBUILT ${TORRENT_ROOT}/prebuilt/${OHOS_ARCH})
set(TORRENT_BOOST ${TORRENT_ROOT}/deps/boost-1.86)

# --- Boost 1.86 headers: auto-fetched at configure time when missing -------
# Mirrors fetch-boost.mjs (kept as the offline fallback). The tarball
# layout is boost-1.86.0/boost/...; cmake -E tar has no --strip-components,
# so the member filter extracts into a staging dir and the boost/ tree is
# moved into deps/boost-1.86 afterwards. Candidate tarballs, in order:
#   1. TORRENT_BOOST_TARBALL cache variable (explicit override)
#   2. deps/downloads/boost-1.86.0-b2-nodocs.tar.gz (earlier auto-fetch)
#   3. fetch-boost cache: $ENV{TEMP}/mihon-torrent/downloads (Windows) or
#      $ENV{TMPDIR}/mihon-torrent/downloads (macOS/Linux)
#   4. download from github.com/boostorg/boost into deps/downloads/
set(BOOST_URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.gz)
set(BOOST_TGZ_NAME boost-1.86.0-b2-nodocs.tar.gz)

if(NOT EXISTS ${TORRENT_BOOST}/boost/version.hpp)
    set(BOOST_TGZ "")
    if(DEFINED TORRENT_BOOST_TARBALL AND EXISTS "${TORRENT_BOOST_TARBALL}")
        set(BOOST_TGZ "${TORRENT_BOOST_TARBALL}")
    endif()
    if(BOOST_TGZ STREQUAL "" AND EXISTS "${TORRENT_ROOT}/deps/downloads/${BOOST_TGZ_NAME}")
        set(BOOST_TGZ "${TORRENT_ROOT}/deps/downloads/${BOOST_TGZ_NAME}")
    endif()
    if(BOOST_TGZ STREQUAL "" AND EXISTS "$ENV{TEMP}/mihon-torrent/downloads/${BOOST_TGZ_NAME}")
        set(BOOST_TGZ "$ENV{TEMP}/mihon-torrent/downloads/${BOOST_TGZ_NAME}")
    endif()
    if(BOOST_TGZ STREQUAL "" AND EXISTS "$ENV{TMPDIR}/mihon-torrent/downloads/${BOOST_TGZ_NAME}")
        set(BOOST_TGZ "$ENV{TMPDIR}/mihon-torrent/downloads/${BOOST_TGZ_NAME}")
    endif()
    if(BOOST_TGZ STREQUAL "")
        file(MAKE_DIRECTORY ${TORRENT_ROOT}/deps/downloads)
        set(BOOST_TGZ "${TORRENT_ROOT}/deps/downloads/${BOOST_TGZ_NAME}")
        if(NOT EXISTS "${BOOST_TGZ}")
            message(STATUS "Downloading Boost 1.86 headers (~48 MB) from github.com/boostorg/boost ...")
            file(DOWNLOAD ${BOOST_URL} "${BOOST_TGZ}"
                SHOW_PROGRESS
                TLS_VERIFY ON)
        endif()
    endif()

    message(STATUS "Extracting Boost 1.86 headers from ${BOOST_TGZ} ...")
    set(BOOST_STAGING ${TORRENT_ROOT}/deps/_boost_staging)
    file(REMOVE_RECURSE "${BOOST_STAGING}")
    file(MAKE_DIRECTORY "${BOOST_STAGING}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -E tar xzf "${BOOST_TGZ}" boost-1.86.0/boost
        WORKING_DIRECTORY "${BOOST_STAGING}"
        RESULT_VARIABLE BOOST_EXTRACT_RESULT)
    if(NOT BOOST_EXTRACT_RESULT EQUAL 0 OR NOT EXISTS "${BOOST_STAGING}/boost-1.86.0/boost/version.hpp")
        file(REMOVE_RECURSE "${BOOST_STAGING}")
        message(FATAL_ERROR
            "Failed to extract Boost headers from ${BOOST_TGZ}. Delete the file and "
            "re-configure, or run third_party/torrent/fetch-boost.mjs once to "
            "fetch them manually.")
    endif()
    file(MAKE_DIRECTORY "${TORRENT_BOOST}")
    file(RENAME "${BOOST_STAGING}/boost-1.86.0/boost" "${TORRENT_BOOST}/boost")
    file(REMOVE_RECURSE "${BOOST_STAGING}")
    if(NOT EXISTS "${TORRENT_BOOST}/boost/version.hpp")
        message(FATAL_ERROR "Boost headers missing after extraction.")
    endif()
endif()

# Header/define surface shared by the prebuilt lib and torrent_bridge.cpp.
# Definitions mirror the libtorrent CMake build (OpenSSL integration). The
# pthread cancellation shim cannot live here: commas/parens in a compile
# definition get re-split by the CMake generator, so torrent_bridge.cpp
# defines it as a real #define before any include instead.
add_library(torrent_deps INTERFACE)
target_include_directories(torrent_deps INTERFACE
    ${TORRENT_ROOT}/libtorrent/include
    ${TORRENT_BOOST}
    ${TORRENT_ROOT}/openssl-include)
target_compile_definitions(torrent_deps INTERFACE
    TORRENT_USE_OPENSSL
    TORRENT_USE_LIBCRYPTO
    TORRENT_SSL_PEERS
    OPENSSL_NO_SSL2
    OPENSSL_NO_SSL3
    OPENSSL_NO_TLS1
    OPENSSL_NO_TLS1_1
    OPENSSL_NO_DTLS1
    _GNU_SOURCE)
# Feature/ABI defines mirroring the prebuilt lib build (build-torrent.mjs).
# The bridge MUST see the same header surface: TORRENT_NO_DEPRECATE bumps
# TORRENT_ABI_VERSION 1 -> 3 (v2 inline namespace), which removes deprecated
# declarations and changes some class layouts (e.g. torrent_alert::name).
target_compile_definitions(torrent_deps INTERFACE
    TORRENT_NO_DEPRECATE
    TORRENT_DISABLE_ALERT_MSG
    TORRENT_DISABLE_SHARE_MODE
    TORRENT_DISABLE_SUPERSEEDING
    TORRENT_DISABLE_STREAMING
    TORRENT_DISABLE_MUTABLE_TORRENTS
    TORRENT_DISABLE_LOGGING
    TORRENT_USE_I2P=0)

add_library(torrent_rasterbar STATIC IMPORTED)
set_target_properties(torrent_rasterbar PROPERTIES
    IMPORTED_LOCATION ${TORRENT_PREBUILT}/libtorrent-rasterbar.a)

add_library(torrent SHARED
    torrent_bridge.cpp)

# Dead-code elimination: every .a above is compiled with
# -ffunction-sections -fdata-sections (build-openssl.mjs / build-torrent.mjs),
# so gc-sections drops unreferenced functions at link time (unused OpenSSL
# algorithms, libtorrent features, and any unused static-libc++ pieces).
target_compile_options(torrent PRIVATE -ffunction-sections -fdata-sections)
target_link_options(torrent PRIVATE -Wl,--gc-sections)

target_link_libraries(torrent PUBLIC
    torrent_deps
    torrent_rasterbar
    ${TORRENT_PREBUILT}/libcrypto.a
    ${TORRENT_PREBUILT}/libssl.a
    libace_napi.z.so
    libhilog_ndk.z.so)

# NOTE on stripping: hvigor's StripNativeLibs task already runs
# llvm-strip --strip-unneeded on every packaged .so, so the shipped
# libtorrent.so (~12MB x86_64 / ~11.2MB arm64, no .symtab/.debug_*) is
# already minimal. The unstripped prebuilt .a archives in
# third_party/torrent/prebuilt/<abi>/ keep full symbols as the
# symbolization source of truth for any future native-crash analysis.

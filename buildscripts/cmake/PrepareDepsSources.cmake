# Pre-fetches pristine dependency sources into the cache so source/REBUILD
# builds — and offline/distro builds — need no network at configure time:
#
#   cmake [-DMUSE_DEPS_CACHE=<dir>] -P buildscripts/cmake/PrepareDepsSources.cmake
#
# Reads the manifest (DependencyManifest.cmake) for MUSE_DEPS_URL and the dep
# list, fetches each source recipe's tarball, and verifies its SHA-256 into
# <cache>/downloads/<name>/. SYSTEM deps and deps without a tarball recipe are
# skipped (logged). MUSE_DEPS_CACHE defaults to $XDG_CACHE_HOME/~/.cache.

cmake_minimum_required(VERSION 3.16)
set(_self_dir "${CMAKE_CURRENT_LIST_DIR}")

if (NOT MUSE_DEPS_CACHE)
    if (DEFINED ENV{MUSE_DEPS_CACHE})
        set(MUSE_DEPS_CACHE "$ENV{MUSE_DEPS_CACHE}")
    elseif (DEFINED ENV{XDG_CACHE_HOME})
        set(MUSE_DEPS_CACHE "$ENV{XDG_CACHE_HOME}/muse_deps")
    else()
        set(MUSE_DEPS_CACHE "$ENV{HOME}/.cache/muse_deps")
    endif()
endif()
# Export so source-delivery consume scripts (which read $MUSE_DEPS_CACHE) agree.
set(ENV{MUSE_DEPS_CACHE} "${MUSE_DEPS_CACHE}")

# Consider all deps regardless of the manifest's OS/option guards, so the cache
# is portable across platforms.
set(OS_IS_LIN FALSE)
set(OS_IS_WIN FALSE)
set(AU_USE_LIBCURL ON)

# Fetch + SHA-verify one dep's tarball into the cache (function scope isolates
# the DEP_* vars the spec sets).
function(_pds_fetch name version)
    set(tmp "${MUSE_DEPS_CACHE}/.recipe/${name}-spec.cmake")
    file(DOWNLOAD "${MUSE_DEPS_URL}/${name}/${version}/recipe/spec.cmake" "${tmp}" STATUS st)
    list(GET st 0 code)
    if (NOT code EQUAL 0)
        message(STATUS "[prepare] skip ${name} (no recipe)")
        return()
    endif()
    include("${tmp}")
    if (NOT DEFINED DEP_SOURCE_URL OR NOT DEFINED DEP_SOURCE_SHA256)
        message(STATUS "[prepare] skip ${name} (no tarball source)")
        return()
    endif()

    get_filename_component(an "${DEP_SOURCE_URL}" NAME)
    set(dl_dir "${MUSE_DEPS_CACHE}/downloads/${name}")
    set(archive "${dl_dir}/${an}")
    if (EXISTS "${archive}")
        file(SHA256 "${archive}" got)
        if (got STREQUAL "${DEP_SOURCE_SHA256}")
            message(STATUS "[prepare] cached ${name}/${an}")
            return()
        endif()
    endif()
    file(MAKE_DIRECTORY "${dl_dir}")
    message(STATUS "[prepare] fetch ${name}: ${DEP_SOURCE_URL}")
    file(DOWNLOAD "${DEP_SOURCE_URL}" "${archive}" EXPECTED_HASH SHA256=${DEP_SOURCE_SHA256} STATUS st)
    list(GET st 0 code)
    if (NOT code EQUAL 0)
        file(REMOVE "${archive}")
        message(FATAL_ERROR "[prepare] ${name} download failed: ${st}")
    endif()
endfunction()

function(require_dep name)
    if ("${ARGV1}" STREQUAL "SYSTEM")
        message(STATUS "[prepare] skip ${name} (system)")
        return()
    endif()
    _pds_fetch("${name}" "${ARGV1}")
endfunction()

# Source-delivery deps prefetch their sources into the cache (fetch-only).
function(require_source_dep name version)
    set(dir "${MUSE_DEPS_CACHE}/downloads/${name}")
    file(DOWNLOAD "${MUSE_DEPS_URL}/${name}/${name}.cmake" "${dir}/${name}.cmake"
         HTTPHEADER "Cache-Control: no-cache" STATUS st)
    list(GET st 0 code)
    if (NOT code EQUAL 0)
        message(FATAL_ERROR "[prepare] failed to fetch ${name} consume script")
    endif()
    include("${dir}/${name}.cmake")
    cmake_language(CALL ${name}_PrepareSources "${version}")
    message(STATUS "[prepare] source ${name}")
endfunction()

include("${_self_dir}/DependencyManifest.cmake")
message(STATUS "[prepare] sources ready in ${MUSE_DEPS_CACHE}/downloads")

# Vendors the muse_deps consume scripts into the source tree (deps_bundle/) so
# offline / sandboxed builds — notably Linux distro packaging with SYSTEM deps —
# need no network at configure time. Run when cutting a release source tarball
# (e.g. via the `bundle_deps` target), then source-package the tree.
#
#   cmake -P buildscripts/cmake/BundleDeps.cmake
#
# Reads the manifest (DependencyManifest.cmake) for MUSE_DEPS_URL and the dep
# list, and vendors each consume script + the shared builder + each source
# recipe (spec + patches). Combined with prepare_deps_sources (sources into
# deps_bundle/sources), both SYSTEM and SOURCE builds then run with no network.

cmake_minimum_required(VERSION 3.16)

set(_self_dir "${CMAKE_CURRENT_LIST_DIR}")
if (NOT DEFINED BUNDLE_DIR)
    set(BUNDLE_DIR "${_self_dir}/deps_bundle")
endif()

# Bundle every dep regardless of the manifest's OS/option guards, so the
# resulting source tarball is portable across platforms.
set(OS_IS_LIN FALSE)
set(OS_IS_WIN FALSE)
set(AU_USE_LIBCURL ON)

function(_bundle_fetch url dst)
    file(DOWNLOAD "${url}" "${dst}" HTTPHEADER "Cache-Control: no-cache" STATUS st)
    list(GET st 0 code)
    set(_bundle_code "${code}" PARENT_SCOPE)
endfunction()

function(_bundle_consume name)
    set(dst "${BUNDLE_DIR}/${name}/${name}.cmake")
    _bundle_fetch("${MUSE_DEPS_URL}/${name}/${name}.cmake" "${dst}")
    file(READ "${dst}" content)
    if (NOT _bundle_code EQUAL 0 OR NOT content MATCHES "function\\(")
        message(FATAL_ERROR "[bundle_deps] failed to fetch ${name} consume script")
    endif()
endfunction()

# The shared builder, fetched once.
function(_bundle_builder)
    if (NOT EXISTS "${BUNDLE_DIR}/buildtools/build_dep_lib.cmake")
        _bundle_fetch("${MUSE_DEPS_URL}/buildtools/build_dep_lib.cmake"
                      "${BUNDLE_DIR}/buildtools/build_dep_lib.cmake")
        if (NOT _bundle_code EQUAL 0)
            message(FATAL_ERROR "[bundle_deps] failed to fetch build_dep_lib")
        endif()
    endif()
endfunction()

# A source recipe (spec + its patches). Non-fatal: prebuilt-only deps without a
# source recipe are skipped (function scope isolates the DEP_* the spec sets).
function(_bundle_recipe name version)
    set(rdir "${BUNDLE_DIR}/${name}/recipe")
    _bundle_fetch("${MUSE_DEPS_URL}/${name}/${version}/recipe/spec.cmake" "${rdir}/spec.cmake")
    if (NOT _bundle_code EQUAL 0)
        file(REMOVE "${rdir}/spec.cmake")
        message(STATUS "[bundle_deps] ${name}: no source recipe (skipped)")
        return()
    endif()
    include("${rdir}/spec.cmake")
    set(patches ${DEP_PATCHES})
    foreach(os MACOS LINUX WINDOWS)
        list(APPEND patches ${DEP_PATCHES_${os}})
    endforeach()
    foreach(p ${patches})
        _bundle_fetch("${MUSE_DEPS_URL}/${name}/${version}/recipe/${p}" "${rdir}/${p}")
        if (NOT _bundle_code EQUAL 0)
            message(FATAL_ERROR "[bundle_deps] ${name}: patch ${p} fetch failed")
        endif()
    endforeach()
endfunction()

function(require_dep name)
    _bundle_consume("${name}")
    if (NOT "${ARGV1}" STREQUAL "SYSTEM")
        _bundle_builder()
        _bundle_recipe("${name}" "${ARGV1}")
    endif()
    message(STATUS "[bundle_deps] ${name}")
endfunction()

# Source-delivery deps vendor their consume script; their sources are vendored
# into deps_bundle/sources by prepare_deps_sources.
function(require_source_dep name version)
    _bundle_consume("${name}")
    message(STATUS "[bundle_deps] ${name} (source-delivery)")
endfunction()

include("${_self_dir}/DependencyManifest.cmake")
message(STATUS "[bundle_deps] vendored into ${BUNDLE_DIR}")

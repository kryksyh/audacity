

message(STATUS "Setup dependencies")

# Config
include(GetPlatformInfo)
include(GetBuildType)

set(LIB_OS )
if (OS_IS_WIN)
    set(LIB_OS "windows")
elseif(OS_IS_LIN)
    set(LIB_OS "linux")
elseif(OS_IS_FBSD)
    set(LIB_OS "linux")
elseif(OS_IS_MAC)
    set(LIB_OS "macos")
    list(LENGTH CMAKE_OSX_ARCHITECTURES arch_count)
    if(arch_count GREATER 1)
        set(ARCH "universal")
    endif()
endif()

set(LIB_ARCH ${ARCH})

if (BUILD_IS_RELEASE)
    set(LIB_BUILD_TYPE "release")
else()
    set(LIB_BUILD_TYPE "debug")
endif()

# MUSE_DEPS_URL (the deps repo root) is set by the manifest, below.
set(LOCAL_ROOT_PATH ${FETCHCONTENT_BASE_DIR})

# Consume scripts vendored here (by the `bundle_deps` target) are used offline,
# without any network fetch — this is what lets sandboxed distro SYSTEM builds
# work. Empty in a normal git checkout (scripts are fetched instead).
set(MUSE_DEPS_BUNDLE_DIR "${CMAKE_CURRENT_LIST_DIR}/deps_bundle")

# Pristine source cache (for source/REBUILD builds). Priority: an explicit
# -DMUSE_DEPS_CACHE, else a pre-existing $MUSE_DEPS_CACHE, else the in-tree
# vendored sources (offline release tarball), else build_dep_lib's ~/.cache
# default. Exported to the env so build_dep_lib picks it up without threading.
if (MUSE_DEPS_CACHE)
    set(ENV{MUSE_DEPS_CACHE} "${MUSE_DEPS_CACHE}")
elseif (NOT DEFINED ENV{MUSE_DEPS_CACHE} AND EXISTS "${MUSE_DEPS_BUNDLE_DIR}/sources")
    set(ENV{MUSE_DEPS_CACHE} "${MUSE_DEPS_BUNDLE_DIR}/sources")
endif()

# Make a dep's consume script (and, for source builds, the builder + recipe)
# available under local_path, then set CONSUME_SCRIPT to the script to include.
# Two modes:
#   bundle present (deps_bundle/<name>/<name>.cmake) -> offline/distro: use the
#     vendored script + copy the vendored builder/recipe into local_path so the
#     version-agnostic PopulateBuild finds them with no network.
#   no bundle -> dev: fetch the consume script fresh each configure (so muse_deps
#     changes propagate without clearing the build tree) and drop any stale
#     builder/recipe so PopulateBuild re-fetches them fresh too.
function(_prepare_dep_files name local_path)
    set(bundle "${MUSE_DEPS_BUNDLE_DIR}/${name}")
    file(MAKE_DIRECTORY "${local_path}")
    if (EXISTS "${bundle}/${name}.cmake")
        if (EXISTS "${MUSE_DEPS_BUNDLE_DIR}/buildtools/build_dep_lib.cmake")
            file(COPY "${MUSE_DEPS_BUNDLE_DIR}/buildtools/build_dep_lib.cmake" DESTINATION "${local_path}")
        endif()
        if (EXISTS "${bundle}/recipe")
            file(REMOVE_RECURSE "${local_path}/recipe")
            file(COPY "${bundle}/recipe" DESTINATION "${local_path}")
        endif()
        set(CONSUME_SCRIPT "${bundle}/${name}.cmake" PARENT_SCOPE)
    else()
        file(DOWNLOAD "${MUSE_DEPS_URL}/${name}/${name}.cmake" "${local_path}/${name}.cmake"
             HTTPHEADER "Cache-Control: no-cache")
        file(REMOVE "${local_path}/build_dep_lib.cmake")
        file(REMOVE_RECURSE "${local_path}/recipe")
        set(CONSUME_SCRIPT "${local_path}/${name}.cmake" PARENT_SCOPE)
    endif()
endfunction()

function(require_dep name)
    # Manifest entry forms (see DependencyManifest.cmake):
    #   require_dep(<name> <version>)          prebuilt, with source-build fallback
    #   require_dep(<name> <version> REBUILD)  always build from source
    #   require_dep(<name> SYSTEM)             system library (no version)
    set(version "")
    set(mode "prebuilt")
    if ("${ARGV1}" STREQUAL "SYSTEM")
        set(mode "system")
    else()
        set(version "${ARGV1}")
        if (ARGC GREATER 2 AND "${ARGV2}" STREQUAL "REBUILD")
            set(mode "rebuild")
        endif()
    endif()

    # Overrides: global MUSE_USE_SYSTEM_ALL / MUSE_BUILD_ALL, per-dep
    # MUSE_USE_SYSTEM_<NAME> / MUSE_BUILD_<NAME>.
    string(TOUPPER ${name} name_upper)
    if (MUSE_USE_SYSTEM_ALL OR MUSE_USE_SYSTEM_${name_upper})
        set(mode "system")
    elseif ((MUSE_BUILD_ALL OR MUSE_BUILD_${name_upper}) AND NOT "${version}" STREQUAL "")
        set(mode "rebuild")
    endif()

    if (NOT mode STREQUAL "system" AND "${version}" STREQUAL "")
        message(FATAL_ERROR "[deps] '${name}' needs a version (or SYSTEM) in DependencyManifest.cmake")
    endif()

    # Consume script is version-agnostic at <name>/<name>.cmake; the version
    # (when pinned) is passed to the populate functions for release/recipe paths.
    set(local_path ${LOCAL_ROOT_PATH}/${name})
    _prepare_dep_files(${name} ${local_path})
    include(${CONSUME_SCRIPT})

    # Resolution order: system -> forced source -> prebuilt -> auto source.
    if (mode STREQUAL "system")
        cmake_language(CALL ${name}_PopulateSystem)
    elseif (mode STREQUAL "rebuild")
        cmake_language(CALL ${name}_PopulateBuild ${local_path} ${LIB_OS} ${LIB_ARCH} ${LIB_BUILD_TYPE} ${version})
    else()
        set_property(GLOBAL PROPERTY ${name}_AVAILABLE TRUE)
        cmake_language(CALL ${name}_Populate ${local_path} ${LIB_OS} ${LIB_ARCH} ${LIB_BUILD_TYPE} ${version})
        get_property(prebuilt_available GLOBAL PROPERTY ${name}_AVAILABLE)
        if (NOT prebuilt_available)
            message(STATUS "[${name}] no prebuilt for ${LIB_OS}/${LIB_ARCH}, building from source")
            cmake_language(CALL ${name}_PopulateBuild ${local_path} ${LIB_OS} ${LIB_ARCH} ${LIB_BUILD_TYPE} ${version})
        endif()
    endif()

    get_property(include_dirs GLOBAL PROPERTY ${name}_INCLUDE_DIRS)
    get_property(libraries GLOBAL PROPERTY ${name}_LIBRARIES)
    get_property(instal_libraries GLOBAL PROPERTY ${name}_INSTALL_LIBRARIES)

    set(${name}_INCLUDE_DIRS ${include_dirs} PARENT_SCOPE)
    set(${name}_LIBRARIES ${libraries} PARENT_SCOPE)
    set(${name}_INSTALL_LIBRARIES ${instal_libraries} PARENT_SCOPE)

    if (OS_IS_MAC)
        install(FILES ${instal_libraries} DESTINATION "audacity.app/Contents/Frameworks")
    elseif(OS_IS_WIN)
        install(FILES ${instal_libraries} TYPE BIN)
    else()
        install(FILES ${instal_libraries} TYPE LIB)
    endif()

endfunction()

# Source-delivery deps: muse_deps ships a pinned source tree (no prebuilt lib,
# no system mode); the consumer compiles it in-tree. The manifest only records
# the pin here (cheap, unconditional); the actual fetch is deferred to
# populate_source_dep(), called from the consuming module — its build option may
# not be defined yet at manifest time, and we must not fetch when it is disabled.
function(require_source_dep name version)
    set_property(GLOBAL PROPERTY ${name}_PINNED_VERSION ${version})
endfunction()

# Fetches the source bundle for a dep declared via require_source_dep and exposes
# its extracted root as the ${name}_SOURCE_DIR global. Call from the module that
# uses it (guarded by that module's build option).
function(populate_source_dep name)
    get_property(version GLOBAL PROPERTY ${name}_PINNED_VERSION)
    if (NOT version)
        message(FATAL_ERROR "[deps] '${name}' has no require_source_dep() entry in DependencyManifest.cmake")
    endif()

    # Resolve the consume script (bundle-first offline, fresh in dev), then
    # populate the source into the build tree. The script fetches its sources
    # cache-first, so offline works when the cache is pre-populated.
    set(local_path ${LOCAL_ROOT_PATH}/${name})
    _prepare_dep_files(${name} ${local_path})
    include(${CONSUME_SCRIPT})

    cmake_language(CALL ${name}_PopulateSource ${local_path} ${version})
endfunction()

# The dependency set + versions + modes live in the manifest (require_dep calls).
include(${CMAKE_CURRENT_LIST_DIR}/DependencyManifest.cmake)

# Vendor the consume scripts into the source tree for offline / release builds.
# Run before source-packaging: `cmake --build <dir> --target bundle_deps`
# (deps_bundle/ is then picked up by require_dep, and included in the source pkg).
add_custom_target(bundle_deps
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/BundleDeps.cmake"
    COMMENT "Vendoring muse_deps consume scripts into deps_bundle/ (offline/release)"
    VERBATIM
)

# Pre-fetch pristine dependency sources into the cache (optional step 0): lets
# source/REBUILD builds run offline. Point it at the in-tree bundle to assemble
# an offline release source tree.
add_custom_target(prepare_deps_sources
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_LIST_DIR}/PrepareDepsSources.cmake"
    COMMENT "Pre-fetching dependency sources into the cache"
    VERBATIM
)

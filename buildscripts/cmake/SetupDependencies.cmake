

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
    if (NOT EXISTS ${local_path}/${name}.cmake)
        file(MAKE_DIRECTORY ${local_path})
        file(DOWNLOAD ${MUSE_DEPS_URL}/${name}/${name}.cmake ${local_path}/${name}.cmake
            HTTPHEADER "Cache-Control: no-cache"
        )
    endif()

    include(${local_path}/${name}.cmake)

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

# The dependency set + versions + modes live in the manifest (require_dep calls).
include(${CMAKE_CURRENT_LIST_DIR}/DependencyManifest.cmake)

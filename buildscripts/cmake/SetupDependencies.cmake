

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

set(REMOTE_ROOT_URL https://raw.githubusercontent.com/kryksyh/muse_deps_private/main)
set(LOCAL_ROOT_PATH ${FETCHCONTENT_BASE_DIR})

function(populate name remote_suffix)
    set(remote_url ${REMOTE_ROOT_URL}/${remote_suffix})
    set(local_path ${LOCAL_ROOT_PATH}/${name})

    if (NOT EXISTS ${local_path}/${name}.cmake)
        file(MAKE_DIRECTORY ${local_path})
        file(DOWNLOAD ${remote_url}/${name}.cmake ${local_path}/${name}.cmake
            HTTPHEADER "Cache-Control: no-cache"
        )
    endif()

    include(${local_path}/${name}.cmake)

    # Resolution order: system -> forced source -> prebuilt -> auto source.
    # Per-dep: MUSE_USE_SYSTEM_<NAME> (or the option named in the 3rd arg) and
    # MUSE_BUILD_<NAME>. Global: MUSE_USE_SYSTEM_ALL / MUSE_BUILD_ALL.
    string(TOUPPER ${name} name_upper)
    set(build_var "MUSE_BUILD_${name_upper}")

    set(use_system FALSE)
    if (MUSE_USE_SYSTEM_ALL)
        set(use_system TRUE)
    elseif (ARGC GREATER 2)
        set(use_system_var ${ARGV2})
        if (${use_system_var})
            set(use_system TRUE)
        endif()
    endif()

    set(force_build FALSE)
    if (MUSE_BUILD_ALL OR ${build_var})
        set(force_build TRUE)
    endif()

    if (use_system)
        cmake_language(CALL ${name}_PopulateSystem)
    elseif (force_build)
        cmake_language(CALL ${name}_PopulateBuild ${remote_url} ${local_path} ${LIB_OS} ${LIB_ARCH} ${LIB_BUILD_TYPE})
    else()
        set_property(GLOBAL PROPERTY ${name}_AVAILABLE TRUE)
        cmake_language(CALL ${name}_Populate ${remote_url} ${local_path} ${LIB_OS} ${LIB_ARCH} ${LIB_BUILD_TYPE})
        get_property(prebuilt_available GLOBAL PROPERTY ${name}_AVAILABLE)
        if (NOT prebuilt_available)
            message(STATUS "[${name}] no prebuilt for ${LIB_OS}/${LIB_ARCH}, building from source")
            cmake_language(CALL ${name}_PopulateBuild ${remote_url} ${local_path} ${LIB_OS} ${LIB_ARCH} ${LIB_BUILD_TYPE})
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

# Ordered so that a dependency is populated before anything that links it
# (matters when a dep is built from source: e.g. vorbis/flac/opusfile need ogg).
populate(expat "expat/2.0.5" MUSE_USE_SYSTEM_EXPAT)
populate(wxwidgets "wxwidgets/3.2.6" MUSE_USE_SYSTEM_WXWIDGETS)

if (NOT OS_IS_LIN)
    populate(zlib "zlib/1.2.13" MUSE_USE_SYSTEM_ZLIB)
endif()

if (NOT OS_IS_WIN)
    populate(openssl "openssl/1.1.1t" MUSE_USE_SYSTEM_OPENSSL)
endif()

if (AU_USE_LIBCURL)
    populate(libcurl "libcurl/8.17.0" MUSE_USE_SYSTEM_LIBCURL)
endif()

populate(ogg "ogg/1.3.5" MUSE_USE_SYSTEM_OGG)
populate(vorbis "vorbis/1.3.7" MUSE_USE_SYSTEM_VORBIS)
populate(flac "flac/1.4.2" MUSE_USE_SYSTEM_FLAC)
populate(opus "opus/1.5.2" MUSE_USE_SYSTEM_OPUS)
populate(opusfile "opusfile/0.12" MUSE_USE_SYSTEM_OPUSFILE)
populate(libmp3lame "libmp3lame/3.100" MUSE_USE_SYSTEM_LAME)
populate(mpg123 "mpg123/1.31.2" MUSE_USE_SYSTEM_MPG123)
populate(wavpack "wavpack/5.7.0" MUSE_USE_SYSTEM_WAVPACK)
populate(libsndfile "libsndfile/1.0.31" MUSE_USE_SYSTEM_SNDFILE)
populate(portaudio "portaudio/19.7.0" MUSE_USE_SYSTEM_PORTAUDIO)

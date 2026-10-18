#
# Audacity: A Digital Audio Editor
#
declare_module(lv2sdk)

get_property(lv2sdk_SOURCE_DIR GLOBAL PROPERTY lv2sdk_SOURCE_DIR)
set( lv2_SOURCE_DIR    ${lv2sdk_SOURCE_DIR}/lv2 )
set( lilv_SOURCE_DIR   ${lv2sdk_SOURCE_DIR}/lilv )
set( zix_SOURCE_DIR    ${lv2sdk_SOURCE_DIR}/zix )
set( sord_SOURCE_DIR   ${lv2sdk_SOURCE_DIR}/sord )
set( serd_SOURCE_DIR   ${lv2sdk_SOURCE_DIR}/serd )
set( sratom_SOURCE_DIR ${lv2sdk_SOURCE_DIR}/sratom )
set( suil_SOURCE_DIR   ${lv2sdk_SOURCE_DIR}/suil )

set( SOURCES

      # zix
      ${zix_SOURCE_DIR}/src/allocator.c
      ${zix_SOURCE_DIR}/src/bump_allocator.c
      ${zix_SOURCE_DIR}/src/digest.c
      ${zix_SOURCE_DIR}/src/errno_status.h
      ${zix_SOURCE_DIR}/src/hash.c
      ${zix_SOURCE_DIR}/src/path.c
      ${zix_SOURCE_DIR}/src/status.c
      ${zix_SOURCE_DIR}/src/system.c
      ${zix_SOURCE_DIR}/src/tree.c
      ${zix_SOURCE_DIR}/src/zix_config.h
      ${zix_SOURCE_DIR}/src/btree.c
      ${zix_SOURCE_DIR}/src/errno_status.c
      ${zix_SOURCE_DIR}/src/filesystem.c
      ${zix_SOURCE_DIR}/src/index_range.h
      ${zix_SOURCE_DIR}/src/path_iter.h
      ${zix_SOURCE_DIR}/src/ring.c
      ${zix_SOURCE_DIR}/src/string_view.c
      ${zix_SOURCE_DIR}/src/system.h

      # lilv

      ${lilv_SOURCE_DIR}/src/collections.c
      ${lilv_SOURCE_DIR}/src/dylib.c
      ${lilv_SOURCE_DIR}/src/dylib.h
      ${lilv_SOURCE_DIR}/src/instance.c
      ${lilv_SOURCE_DIR}/src/lib.c
      ${lilv_SOURCE_DIR}/src/lilv_config.h
      ${lilv_SOURCE_DIR}/src/lilv_internal.h
      ${lilv_SOURCE_DIR}/src/node.c
      ${lilv_SOURCE_DIR}/src/plugin.c
      ${lilv_SOURCE_DIR}/src/pluginclass.c
      ${lilv_SOURCE_DIR}/src/port.c
      ${lilv_SOURCE_DIR}/src/query.c
      ${lilv_SOURCE_DIR}/src/scalepoint.c
      ${lilv_SOURCE_DIR}/src/state.c
      ${lilv_SOURCE_DIR}/src/ui.c
      ${lilv_SOURCE_DIR}/src/util.c
      ${lilv_SOURCE_DIR}/src/world.c

      # serd

      ${serd_SOURCE_DIR}/src/attributes.h
      ${serd_SOURCE_DIR}/src/base64.c
      ${serd_SOURCE_DIR}/src/base64.h
      ${serd_SOURCE_DIR}/src/byte_sink.h
      ${serd_SOURCE_DIR}/src/byte_source.c
      ${serd_SOURCE_DIR}/src/byte_source.h
      ${serd_SOURCE_DIR}/src/env.c
      ${serd_SOURCE_DIR}/src/n3.c
      ${serd_SOURCE_DIR}/src/node.c
      ${serd_SOURCE_DIR}/src/reader.c
      ${serd_SOURCE_DIR}/src/reader.h
      ${serd_SOURCE_DIR}/src/serd_config.h
      ${serd_SOURCE_DIR}/src/serd_internal.h
      ${serd_SOURCE_DIR}/src/serdi.c
      ${serd_SOURCE_DIR}/src/stack.h
      ${serd_SOURCE_DIR}/src/string.c
      ${serd_SOURCE_DIR}/src/string_utils.h
      ${serd_SOURCE_DIR}/src/system.c
      ${serd_SOURCE_DIR}/src/system.h
      ${serd_SOURCE_DIR}/src/try.h
      ${serd_SOURCE_DIR}/src/uri.c
      ${serd_SOURCE_DIR}/src/uri_utils.h
      ${serd_SOURCE_DIR}/src/warnings.h
      ${serd_SOURCE_DIR}/src/writer.c

      # sord

      ${sord_SOURCE_DIR}/src/sord.c
      ${sord_SOURCE_DIR}/src/sord_config.h
      ${sord_SOURCE_DIR}/src/sord_internal.h
      ${sord_SOURCE_DIR}/src/sord_validate.c
      ${sord_SOURCE_DIR}/src/sordi.c
      ${sord_SOURCE_DIR}/src/syntax.c

      # sratom

      ${sratom_SOURCE_DIR}/src/sratom.c

      # suil

      ${suil_SOURCE_DIR}/src/host.c
      ${suil_SOURCE_DIR}/src/instance.c
      ${suil_SOURCE_DIR}/src/suil_internal.h
)

if (OS_IS_WIN)
    list(APPEND SOURCES
        ${zix_SOURCE_DIR}/src/win32/environment_win32.c
        ${zix_SOURCE_DIR}/src/win32/filesystem_win32.c
        ${zix_SOURCE_DIR}/src/win32/sem_win32.c
        ${zix_SOURCE_DIR}/src/win32/system_win32.c
        ${zix_SOURCE_DIR}/src/win32/thread_win32.c
        ${zix_SOURCE_DIR}/src/win32/win32_util.c
    )
else()
    list(APPEND SOURCES
        ${zix_SOURCE_DIR}/src/posix/environment_posix.c
        ${zix_SOURCE_DIR}/src/posix/filesystem_posix.c
        ${zix_SOURCE_DIR}/src/posix/system_posix.c
        ${zix_SOURCE_DIR}/src/posix/thread_posix.c
    )
    if (OS_IS_MAC)
        list(APPEND SOURCES ${zix_SOURCE_DIR}/src/darwin/sem_darwin.c)
    else()
        list(APPEND SOURCES ${zix_SOURCE_DIR}/src/posix/sem_posix.c)
    endif()
endif()

set( INCLUDES
   PRIVATE
      ${lilv_SOURCE_DIR}/src
      ${zix_SOURCE_DIR}/src
      ${serd_SOURCE_DIR}/src
      ${sord_SOURCE_DIR}/src
      ${sratom_SOURCE_DIR}/src
      ${suil_SOURCE_DIR}/src
   PUBLIC
      ${zix_SOURCE_DIR}/include
      ${lv2_SOURCE_DIR}/include
      ${lilv_SOURCE_DIR}/include
      ${serd_SOURCE_DIR}/include
      ${sord_SOURCE_DIR}/include
      ${sratom_SOURCE_DIR}/include
      ${suil_SOURCE_DIR}/include
)

set( SUIL_DEFS
   PRIVATE
      HAVE_X11=OFF
      SUIL_MODULE_DIR=""
)

set(MODULE_SRC ${SOURCES})
set(MODULE_DEF ${SUIL_DEFS})
set(MODULE_INCLUDE ${INCLUDES})
set(MODULE_USE_PCH OFF)
set(MODULE_USE_UNITY OFF)

setup_module()

# The SDK is linked into the host as a static library. Its public headers must
# use the same linkage in both the SDK sources and consumers on Windows.
if(OS_IS_WIN)
    target_compile_definitions(${MODULE} PUBLIC
        ZIX_STATIC LILV_STATIC SERD_STATIC SORD_STATIC SRATOM_STATIC SUIL_STATIC
    )
endif()

include(CheckSymbolExists)
check_symbol_exists(posix_fadvise "fcntl.h" LV2SDK_HAVE_POSIX_FADVISE)
target_compile_definitions(${MODULE} PRIVATE HAVE_POSIX_FADVISE=$<BOOL:${LV2SDK_HAVE_POSIX_FADVISE}>)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(${MODULE} PRIVATE _GNU_SOURCE)
endif()

target_no_warning(${MODULE} -w)

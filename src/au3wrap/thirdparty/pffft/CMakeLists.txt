
set(TARGET pffft)
get_property(_src GLOBAL PROPERTY pffft_SOURCE_DIR)
set(TARGET_ROOT ${_src}/pffft)

add_library( ${TARGET} STATIC )

set(SOURCES
      ${TARGET_ROOT}/pffft.c
      ${TARGET_ROOT}/pffft.h
      ${TARGET_ROOT}/pfsimd_macros.h
)

set(INCLUDES
      ${TARGET_ROOT}
)

target_sources( ${TARGET} PRIVATE ${SOURCES} )
target_include_directories( ${TARGET} PUBLIC ${INCLUDES} )

# ARM64EC defines _M_X64, so pffft would pull x86 SSE intrinsic headers
# (xmmintrin.h), which are invalid for the EC target. Build the scalar path.
if (LIB_ARCH STREQUAL "arm64ec")
    target_compile_definitions( ${TARGET} PRIVATE PFFFT_SIMD_DISABLE )
endif()

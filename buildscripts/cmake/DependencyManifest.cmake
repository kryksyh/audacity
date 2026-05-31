# Audacity dependency manifest — the single source of truth. One line per dep,
# declaring its version and how it is obtained:
#
#   require_dep(<name> <version>)           pinned version: prebuilt, else build from source
#   require_dep(<name> <version> REBUILD)   pinned version: always build from source
#   require_dep(<name> SYSTEM)              use the system-installed library (no version)
#
# Order matters: a dependency must precede anything that links it.
# Global overrides: -DMUSE_USE_SYSTEM_ALL=ON, -DMUSE_BUILD_ALL=ON, or per-dep
# -DMUSE_USE_SYSTEM_<NAME>=ON / -DMUSE_BUILD_<NAME>=ON.

# Where the dependency recipes/prebuilts come from (raw repo root at a ref).
set(MUSE_DEPS_URL "https://raw.githubusercontent.com/kryksyh/muse_deps_private/main")

require_dep(expat       2.0.5)

if (NOT OS_IS_LIN)
    require_dep(zlib    1.2.13)
endif()
if (NOT OS_IS_WIN)
    require_dep(openssl 1.1.1t)
endif()
if (AU_USE_LIBCURL)
    require_dep(libcurl 8.17.0)
endif()

require_dep(ogg         1.3.5)
require_dep(vorbis      1.3.7)
require_dep(flac        1.4.2)
require_dep(opus        1.5.2)
require_dep(opusfile    0.12)
require_dep(libmp3lame  3.100)

# mpg123's CMake port assembles its x86/x64 decoder with yasm. Build that tool
# first on Windows x64 (ARM64 uses NEON, no yasm; other OSes assemble via gas).
if (OS_IS_WIN AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64")
    require_tool(yasm 1.3.0)
endif()
require_dep(mpg123      1.31.2)
require_dep(wavpack     5.7.0)
require_dep(libsndfile  1.0.31)
require_dep(portaudio   19.7.0)

# wxwidgets last: no dependents among our deps, slowest/riskiest source build.
require_dep(wxwidgets   3.2.6)

# Source-delivered deps (pinned source compiled in-tree by the consumer; no
# prebuilt/system mode). Fetched lazily via populate_source_dep() from the
# consuming module, so the pin is recorded unconditionally here.
require_source_dep(lv2sdk 0.24.26)

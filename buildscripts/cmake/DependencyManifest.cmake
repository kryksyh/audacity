# Audacity dependency version manifest — the single source of truth for which
# version of each dependency this build uses. muse_deps hosts the recipe for
# each at <name>/<version>/; populate(<name>) resolves the version from here.
#
# To bump a dependency, change it here only (and ensure muse_deps has that
# <name>/<version>/ recipe).

set(DEP_VERSION_wxwidgets   "3.2.6")
set(DEP_VERSION_expat       "2.0.5")
set(DEP_VERSION_zlib        "1.2.13")
set(DEP_VERSION_openssl     "1.1.1t")
set(DEP_VERSION_libcurl     "8.17.0")
set(DEP_VERSION_ogg         "1.3.5")
set(DEP_VERSION_vorbis      "1.3.7")
set(DEP_VERSION_flac        "1.4.2")
set(DEP_VERSION_opus        "1.5.2")
set(DEP_VERSION_opusfile    "0.12")
set(DEP_VERSION_libmp3lame  "3.100")
set(DEP_VERSION_mpg123      "1.31.2")
set(DEP_VERSION_wavpack     "5.7.0")
set(DEP_VERSION_libsndfile  "1.0.31")
set(DEP_VERSION_portaudio   "19.7.0")

#!/usr/bin/env bash
# Fails if any dependency we build/bundle ourselves resolves to a system library
# — i.e. the build is not self-contained. Run after building, e.g.:
#
#   buildscripts/ci/linux/check_self_contained.sh build/main/audacity
#
# Uses the app's own RUNPATH (ldd resolves the full runtime closure), so it
# catches both direct and transitive leaks (e.g. libsndfile -> libvorbis.so.0).
# The strongest proof is still to run this in a container with no audio -dev
# packages installed.
set -euo pipefail

APP="${1:?usage: $0 <app-binary>}"

# Libraries muse_deps owns (built + bundled). zlib is intentionally the system
# one on Linux (not in our manifest there).
OWNED='libvorbis|libvorbisenc|libvorbisfile|libogg|libFLAC|libopus|libopusfile|libsndfile|libmpg123|libmp3lame|libwavpack|libwx_base|libexpat|libssl|libcrypto|libportaudio'

leaks="$(ldd "$APP" | grep -E "$OWNED" | grep -E '=> /usr|=> /lib' || true)"

if [ -n "$leaks" ]; then
    echo "NOT self-contained — these owned libraries resolve to system paths:" >&2
    echo "$leaks" >&2
    exit 1
fi

echo "OK: all owned dependencies resolve inside the build tree"

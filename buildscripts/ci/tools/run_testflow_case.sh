#!/usr/bin/env bash
#
# Runs one testflow test case against the AppImage in ./build.artifacts.

set -euo pipefail

SCRIPT="$1"

APPIMAGE=$(find ./build.artifacts -type f -name '*.AppImage' -print -quit)
test -n "$APPIMAGE" || { echo "AppImage not found in artifact"; exit 1; }

# Extracted rather than mounted, so the runner does not need libfuse2
chmod +x "$APPIMAGE"
"$APPIMAGE" --appimage-extract > /dev/null

SCRIPTS_DIR=$(find "$PWD/squashfs-root" -type d -name testflowscripts -print -quit)
test -n "$SCRIPTS_DIR" || { echo "no testflowscripts in the AppImage, was it a release build?"; exit 1; }
if test -f "$PWD/share/testflowscripts/$SCRIPT"; then
    # A reused build can exercise the checked-out test revision while the
    # normal build workflow tests the matching application and script commit.
    SCRIPTS_DIR="$PWD/share/testflowscripts"
fi

# A running instance would swallow the arguments and exit 0
export AU_ALLOW_MULTIPLE_PROCESSES=1
export MUSE_TESTFLOW_DATA_PATH="$PWD/testflow_data"
export MUSE_TESTFLOW_SCRIPTS_PATH="$SCRIPTS_DIR"
export MUSE_TESTFLOW_FILES_PATH="$SCRIPTS_DIR/data"
mkdir -p "$MUSE_TESTFLOW_DATA_PATH/diagnostics"

# Fresh profile, so nothing leaks in from a previous test case
SANDBOX=$(mktemp -d)
export XDG_CONFIG_HOME="$SANDBOX/config" XDG_DATA_HOME="$SANDBOX/data" \
       XDG_CACHE_HOME="$SANDBOX/cache" XDG_STATE_HOME="$SANDBOX/state"

# Give original-engine playback tests a real, deterministic server and PCM
# device on hosted runners. No microphone or physical speakers are accessed.
pulseaudio --start --exit-idle-time=-1
# PulseAudio's non-rewinding clocked sink uses a bounded 50 ms block instead
# of the default two-second virtual-device block (module-null-sink.c).
pactl load-module module-null-sink sink_name=audacity_ci rate=48000 channels=2 norewinds=true
pactl set-default-sink audacity_ci
pactl set-default-source audacity_ci.monitor
export PULSE_SINK=audacity_ci PULSE_SOURCE=audacity_ci.monitor
# libpulse's stream buffer override bounds the null sink's startup prebuffer.
export PULSE_LATENCY_MSEC=100

# Let a stalled test produce a crash dump for the uploaded diagnostics before
# killing its process group 15 seconds later.
set +e
timeout -s ABRT -k 15 300 xvfb-run -a -s "-screen 0 1600x1000x24" \
    bash "$PWD/buildscripts/ci/tools/run_testflow_display.sh" \
    "$PWD/squashfs-root/AppRun" --test-case "$SCRIPTS_DIR/$SCRIPT" --test-case-speed Fast \
    2>&1 | tee "$MUSE_TESTFLOW_DATA_PATH/console.log"
rc=${PIPESTATUS[0]}
set -e
if test "$rc" -ne 0; then
    find "$SANDBOX" -type f \( -name '*.dmp' -o -name '*.log' \) \
        -exec cp {} "$MUSE_TESTFLOW_DATA_PATH/diagnostics/" \;
fi

# Report titles contain punctuation (for example "TC3.9: ...") that GitHub's
# cross-platform artifact storage rejects in filenames.
python3 - "$MUSE_TESTFLOW_DATA_PATH" <<'PY'
import pathlib
import re
import sys
for entry in sorted(pathlib.Path(sys.argv[1]).rglob('*'), key=lambda path: len(path.parts), reverse=True):
    name = re.sub(r'[<>:"\\|?*\x00-\x1f]', '_', entry.name)
    if name != entry.name:
        entry.rename(entry.with_name(name))
PY

shopt -s nullglob
for report in "$MUSE_TESTFLOW_DATA_PATH"/reports/*.txt; do
    echo "--- $report"
    cat "$report"
done

test "$rc" -eq 124 && echo "timed out"
exit $rc

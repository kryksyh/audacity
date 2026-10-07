#!/bin/zsh
#
# Audacity: A Digital Audio Editor
#
# Runs latency_scenario.js and prints the per-action latency report.
#
# usage: run_latency_scenario.sh <label> [ENV=VALUE ...]
#   APP       app binary (default: build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity)
#   OUT       output directory (default: build/latency)
#   ENV=VALUE passed to the app:
#     AU_LAT_BUFFER_MS=10       buffer length preference for the run
#     AU_LAT_OUTPUT=<device>    output device name, as in the preferences page
#     AU_LAT_INPUT=<device>     input device name
#     AU_LAT_EFFECT=<effect id> AU_LAT_EFFECT_COUNT=<n>   realtime effects on the track, as load
set -u
SCRIPT_DIR=${0:A:h}
ROOT=${SCRIPT_DIR:h:h}
APP=${APP:-$ROOT/build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity}
OUT=${OUT:-$ROOT/build/latency}
label=$1; shift

mkdir -p "$OUT"
[ -f "$OUT/control.wav" ] || python3 "$SCRIPT_DIR/make_test_signals.py" "$OUT"
rm -f "$OUT/$label.json"

# Never hand the run over to an Audacity that is already open
env "$@" AU_ALLOW_MULTIPLE_PROCESSES=1 AU_LAT_SIGNAL="$OUT/control.wav" AU_LAT_TRACE="$OUT/$label.json" \
    "$APP" --test-case "$SCRIPT_DIR/latency_scenario.js" > "$OUT/$label.log" 2>&1 &
pid=$!
# The imported signal leaves an unsaved project, so quitting may wait on a
# save prompt: stop the app once the trace is written
for i in {1..180}; do
    kill -0 $pid 2>/dev/null || break
    [ -s "$OUT/$label.json" ] && { /bin/sleep 2; kill $pid; break; }
    /bin/sleep 1
done
if kill -0 $pid 2>/dev/null; then
    /bin/sleep 3
    kill -0 $pid 2>/dev/null && { echo "app did not exit, killing"; kill -9 $pid; }
fi

grep -h "configureAudio\|addRealtimeEffect\|audioEngineHealth" "$OUT/$label.log"
if [ -f "$OUT/$label.json" ]; then
    python3 "$SCRIPT_DIR/analyze_trace.py" "$OUT/$label.json"
else
    echo "No trace produced, see $OUT/$label.log"
fi

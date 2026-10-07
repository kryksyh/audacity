#!/bin/zsh
#
# Audacity: A Digital Audio Editor
#
# Runs stress_scenario.js and prints the engine health and the callback load.
#
# usage: run_stress.sh <label> [ENV=VALUE ...]
#   APP  app binary (default: build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity)
#   OUT  output directory (default: build/latency)
#   ENV=VALUE passed to the app:
#     AU_LAT_BUFFER_MS=5  AU_LAT_OUTPUT=<device>  as for run_latency_scenario.sh
#     AU_LAT_TRACKS=8     number of tracks
#     AU_LAT_EFFECTS=<id>,<id>  realtime effects on each track (default: Compressor, Reverb)
#     AU_LAT_SECONDS=120  play time
set -u
SCRIPT_DIR=${0:A:h}
ROOT=${SCRIPT_DIR:h:h}
APP=${APP:-$ROOT/build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity}
OUT=${OUT:-$ROOT/build/latency}
label=$1; shift

mkdir -p "$OUT"
[ -f "$OUT/stress.wav" ] || python3 "$SCRIPT_DIR/make_test_signals.py" "$OUT"
rm -f "$OUT/$label.json"

seconds=120
for arg in "$@"; do
    [[ $arg == AU_LAT_SECONDS=* ]] && seconds=${arg#AU_LAT_SECONDS=}
done

# Never hand the run over to an Audacity that is already open
env AU_LAT_EFFECTS="Effect_Audacity_Audacity_Compressor_Built-in Effect: Compressor,Effect_Audacity_Audacity_Reverb_Built-in Effect: Reverb" "$@" \
    AU_ALLOW_MULTIPLE_PROCESSES=1 AU_LAT_SIGNAL="$OUT/stress.wav" AU_LAT_TRACE="$OUT/$label.json" \
    "$APP" --test-case "$SCRIPT_DIR/stress_scenario.js" > "$OUT/$label.log" 2>&1 &
pid=$!
for i in {1..$(( seconds + 180 ))}; do
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
    python3 "$SCRIPT_DIR/analyze_trace.py" "$OUT/$label.json" | sed -n '/== Audio callback/,/^$/p'
else
    echo "No trace produced, see $OUT/$label.log"
fi

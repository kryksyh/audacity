#!/bin/zsh
#
# Audacity: A Digital Audio Editor
#
# Runs record_alignment_scenario.js and prints where the recorded bursts land.
# A positive offset means the recording sits late on the timeline.
#
# usage: run_record_alignment.sh <label> [ENV=VALUE ...]
#   APP, OUT  as in run_latency_scenario.sh
#   ENV=VALUE passed to the app: AU_LAT_BUFFER_MS, AU_LAT_OUTPUT, AU_LAT_INPUT,
#             AU_LAT_AUTO_COMPENSATION=0|1, AU_LAT_EFFECT=<effect id> (added to the played track)
set -u
SCRIPT_DIR=${0:A:h}
ROOT=${SCRIPT_DIR:h:h}
APP=${APP:-$ROOT/build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity}
OUT=${OUT:-$ROOT/build/latency}
PA_RTL=${PA_RTL:-$ROOT/build/pa_rtl/pa_rtl}
label=$1; shift

mkdir -p "$OUT"
python3 "$SCRIPT_DIR/make_test_signals.py" "$OUT"
rm -rf "$OUT/$label" "$OUT/$label.json"
mkdir -p "$OUT/$label"

env "$@" AU_LAT_SIGNAL="$OUT/bursts.wav" AU_LAT_TRACE="$OUT/$label.json" AU_LAT_EXPORT_DIR="$OUT/$label" \
    "$APP" --test-case "$SCRIPT_DIR/record_alignment_scenario.js" > "$OUT/$label.log" 2>&1 &
pid=$!
for i in {1..120}; do
    kill -0 $pid 2>/dev/null || break
    grep -q "exportTracks" "$OUT/$label.log" && { /bin/sleep 2; kill $pid 2>/dev/null; break; }
    /bin/sleep 1
done
if kill -0 $pid 2>/dev/null; then
    /bin/sleep 3
    kill -0 $pid 2>/dev/null && { echo "app did not exit, killing"; kill -9 $pid; }
fi

grep -h "configureAudio\|addRealtimeEffect\|exportTracks" "$OUT/$label.log"
ls "$OUT/$label"
take=$(ls "$OUT/$label"/*.wav(N) | tail -1)
[ -n "$take" ] && "$PA_RTL" --align "$OUT/bursts.wav" "$take"

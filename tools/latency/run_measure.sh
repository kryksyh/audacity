#!/bin/zsh
#
# Audacity: A Digital Audio Editor
#
# Runs the built-in loopback latency measurement once and prints the result.
#
# usage: run_measure.sh <label> [AU_LAT_BUFFER_MS=10] [AU_LAT_OUTPUT=<device>] [AU_LAT_INPUT=<device>]
#   APP, OUT  as in run_latency_scenario.sh
set -u
SCRIPT_DIR=${0:A:h}
ROOT=${SCRIPT_DIR:h:h}
APP=${APP:-$ROOT/build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity}
OUT=${OUT:-$ROOT/build/latency}
label=$1; shift

mkdir -p "$OUT"
# Never hand the run over to an Audacity that is already open
env "$@" AU_ALLOW_MULTIPLE_PROCESSES=1 \
    "$APP" --test-case "$SCRIPT_DIR/measure_scenario.js" > "$OUT/$label.log" 2>&1 &
pid=$!
for i in {1..60}; do
    kill -0 $pid 2>/dev/null || break
    grep -q "latencyMeasurement" "$OUT/$label.log" && { /bin/sleep 1; kill $pid 2>/dev/null; break; }
    /bin/sleep 1
done
if kill -0 $pid 2>/dev/null; then
    /bin/sleep 3
    kill -0 $pid 2>/dev/null && { echo "app did not exit, killing"; kill -9 $pid; }
fi

grep -h "configureAudio\|latencyMeasurement" "$OUT/$label.log"

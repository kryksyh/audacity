#!/bin/zsh
#
# Audacity: A Digital Audio Editor
#
# Runs effect_churn_scenario.js, by default on the ASan build, and reports any
# AddressSanitizer finding.
#
# usage: run_effect_churn.sh <label> [AU_LAT_ROUNDS=100] [AU_LAT_EFFECT=<effect id>] [AU_LAT_OUTPUT=<device>]
#   APP  app binary (default: build/asan/src/app/audacity.app/Contents/MacOS/audacity)
#   OUT  output directory (default: build/latency)
set -u
SCRIPT_DIR=${0:A:h}
ROOT=${SCRIPT_DIR:h:h}
APP=${APP:-$ROOT/build/asan/src/app/audacity.app/Contents/MacOS/audacity}
OUT=${OUT:-$ROOT/build/latency}
label=$1; shift

# Test signals must never reach the system default output (e.g. the laptop speakers)
[[ " $* " == *" AU_LAT_OUTPUT="* ]] || { echo "AU_LAT_OUTPUT=<device> is required"; exit 2; }

mkdir -p "$OUT"
[ -f "$OUT/control.wav" ] || python3 "$SCRIPT_DIR/make_test_signals.py" "$OUT"

# Never hand the run over to an Audacity that is already open
env AU_LAT_EFFECT="Effect_Audacity_Audacity_Compressor_Built-in Effect: Compressor" "$@" \
    AU_ALLOW_MULTIPLE_PROCESSES=1 AU_LAT_SIGNAL="$OUT/control.wav" \
    ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=0" \
    "$APP" --test-case "$SCRIPT_DIR/effect_churn_scenario.js" > "$OUT/$label.log" 2>&1 &
pid=$!
for i in {1..600}; do
    kill -0 $pid 2>/dev/null || break
    # The app would otherwise play on the system default output
    grep -q "configureAudio FAILED" "$OUT/$label.log" && { echo "configureAudio failed, app stopped"; kill -9 $pid; break; }
    grep -q "effectChurn" "$OUT/$label.log" && { /bin/sleep 2; kill $pid 2>/dev/null; break; }
    /bin/sleep 1
done
if kill -0 $pid 2>/dev/null; then
    echo "TIMEOUT, killing"
    kill -9 $pid
fi

grep -h "configureAudio\\|effectChurn" "$OUT/$label.log" | sed 's/.*| "//'
if grep -q "AddressSanitizer" "$OUT/$label.log"; then
    echo "ASAN FINDING:"
    grep -A30 "ERROR: AddressSanitizer" "$OUT/$label.log" | head -40
    exit 1
fi
grep -q "effectChurn" "$OUT/$label.log" && echo "no AddressSanitizer report" || { echo "run did not finish, see $OUT/$label.log"; exit 2; }

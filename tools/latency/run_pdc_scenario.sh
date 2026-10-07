#!/bin/zsh
#
# Audacity: A Digital Audio Editor
#
# Runs pdc_scenario.js and prints the distance between the two click onsets
# for each case; 0 frames off means the tracks are aligned.
#
# usage: run_pdc_scenario.sh <label> [ENV=VALUE ...]
#   APP  app binary (default: build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity)
#   OUT  output directory (default: build/latency)
#   ENV=VALUE passed to the app, as for run_latency_scenario.sh; AU_LAT_EFFECT
#   defaults to the built-in Compressor (3 ms look-ahead)
set -u
SCRIPT_DIR=${0:A:h}
ROOT=${SCRIPT_DIR:h:h}
APP=${APP:-$ROOT/build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity}
OUT=${OUT:-$ROOT/build/latency}
label=$1; shift

mkdir -p "$OUT"
[ -f "$OUT/click_a.wav" ] || python3 "$SCRIPT_DIR/make_test_signals.py" "$OUT"
rm -f "$OUT/$label.json"

# Never hand the run over to an Audacity that is already open
env AU_LAT_EFFECT="Effect_Audacity_Audacity_Compressor_Built-in Effect: Compressor" "$@" \
    AU_ALLOW_MULTIPLE_PROCESSES=1 AU_LAT_SIGNAL_A="$OUT/click_a.wav" AU_LAT_SIGNAL_B="$OUT/click_b.wav" \
    AU_LAT_TRACE="$OUT/$label.json" \
    "$APP" --test-case "$SCRIPT_DIR/pdc_scenario.js" > "$OUT/$label.log" 2>&1 &
pid=$!
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
[ -f "$OUT/$label.json" ] || { echo "No trace produced, see $OUT/$label.log"; exit 1; }
python3 - "$OUT/$label.json" <<'PY'
import json, sys
events = json.load(open(sys.argv[1]))["traceEvents"]
items = []
for e in events:
    if e.get("ph") == "X" and e["name"].startswith("pdc: "):
        items.append((e["ts"], "mark", e["name"][5:]))
    elif e.get("ph") == "C" and e["name"] == "output onset frame":
        items.append((e["ts"], "onset", e["args"]["value"]))
items.sort()
cases, current = [], None
for _, kind, value in items:
    if kind == "mark":
        current = (value, [])
        cases.append(current)
    elif current:
        current[1].append(value)
print("== Click onsets, track B with the latent effect (0.5 s apart when aligned)")
for name, onsets in cases:
    if len(onsets) != 2:
        print("  %-28s %d onsets found, expected 2" % (name, len(onsets)))
        continue
    distance = onsets[1] - onsets[0]
    expected = min((22050, 24000), key=lambda e: abs(e - distance))
    print("  %-28s %+5d frames off" % (name, distance - expected))
PY

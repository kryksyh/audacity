#!/bin/zsh
#
# Audacity: A Digital Audio Editor
#
# Runs the timeline zoom/scroll benchmark on a copy of a project and prints per-phase frame stats.
#
# usage: run_timeline_bench.sh <project.aup4> <label> [ENV=VALUE ...]
#   APP       app binary (default: build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity)
#   OUT       output directory (default: build/perf)
#   APP_ARGS  extra app arguments, e.g. -d to let Qt debug output reach the log
#   ENV=VALUE passed to the app, e.g. AU_BENCH_PHASES=pinch to run only the matching phases
set -u
SCRIPT_DIR=${0:A:h}
ROOT=${SCRIPT_DIR:h:h}
APP=${APP:-$ROOT/build/audacity-release/src/app/audacity.app/Contents/MacOS/audacity}
OUT=${OUT:-$ROOT/build/perf}
project=$1; label=$2; shift 2

mkdir -p "$OUT"
cp "$project" "$OUT/run.aup4"
rm -f "$OUT"/run.aup4-*(N) "$OUT/$label.json"

env "$@" AU_BENCH_PROJECT="$OUT/run.aup4" AU_BENCH_TRACE="$OUT/$label.json" \
    "$APP" ${=APP_ARGS:-} --test-case "$SCRIPT_DIR/timeline_zoom_scroll.js" > "$OUT/$label.log" 2>&1 &
pid=$!
for i in {1..240}; do
    kill -0 $pid 2>/dev/null || break
    /bin/sleep 1
done
if kill -0 $pid 2>/dev/null; then
    echo "TIMEOUT after 240s, killing"
    kill $pid
fi

if [ -f "$OUT/$label.json" ]; then
    python3 "$SCRIPT_DIR/bench_report.py" "$OUT/$label.json"
else
    echo "No trace produced, see $OUT/$label.log"
fi

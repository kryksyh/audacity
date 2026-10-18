#!/usr/bin/env bash
# Provide normal X11 focus and modal-window behavior on the virtual display.
set -euo pipefail

openbox > "$MUSE_TESTFLOW_DATA_PATH/diagnostics/window-manager.log" 2>&1 &
window_manager_pid=$!
trap 'kill "$window_manager_pid" 2>/dev/null || true' EXIT

window_manager_ready=false
for ((attempt=0; attempt<50; ++attempt)); do
    if xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id #'; then
        window_manager_ready=true
        break
    fi
    kill -0 "$window_manager_pid"
    sleep 0.1
done
if ! "$window_manager_ready"; then
    echo "Window manager did not initialize"
    exit 1
fi

"$@"

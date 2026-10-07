#
# Audacity: A Digital Audio Editor
#
# Latency report for a trace from latency_scenario.js (or any trace with the
# "Audio callback" lane).
#
# Control latency: each "expect sound: <action>" / "expect silence: <action>"
# mark is followed by the first audio callback whose output peak reaches that
# level, which marks when the change left the app. Adding the output latency that PortAudio reports
# estimates when it became audible; the loopback harness measures how true that
# report is.
#
# usage: analyze_trace.py <trace.json>
#
import bisect, json, sys

# Relative to the loudest output peak, so the playback volume setting does not matter
LOUD_FRACTION = 0.3
SILENT_FRACTION = 0.01
# Longer callback periods are pauses between transport runs, not jitter
MAX_PERIOD_MS = 1000
LOOKAHEAD_US = 3_000_000
STREAM_GAP_US = 500_000
TONE_END_S = 10.0
JUMP_MS = 50
ACTION_SETTLE_US = 300_000
NEW_PLAYBACK_GAP_US = 200_000

PA_FLAGS = ["input underflow", "input overflow", "output underflow", "output overflow", "priming output"]

START_PHASES = ["AudioIO::StartStream", "StopMonitoring", "StartPortAudioStream", "AllocateBuffers",
                "TransportState (realtime effects init)", "SequenceBufferExchange (prime)",
                "wait for ring buffer prime", "Pa_StartStream", "WaitForAudioThreadStarted"]
STOP_PHASES = ["AudioIO::StopStream", "fade-out sleep", "Pa_AbortStream + Pa_CloseStream",
               "WaitForAudioThreadStopped", "WaitWhileBusy"]


def pct(values, q):
    if not values:
        return float("nan")
    s = sorted(values)
    return s[min(int(q * len(s)), len(s) - 1)]


def summary(values, unit="ms"):
    if not values:
        return "none"
    return "n=%d  p50 %.2f  p90 %.2f  max %.2f %s" % (len(values), pct(values, 0.5), pct(values, 0.9), max(values), unit)


def main():
    events = json.load(open(sys.argv[1]))["traceEvents"]
    zones = [e for e in events if e.get("ph") == "X"]
    counters = {}
    for e in events:
        if e.get("ph") == "C":
            counters.setdefault(e["name"], []).append((e["ts"], e["args"]["value"]))
    for series in counters.values():
        series.sort()

    def zones_named(name):
        return sorted((e for e in zones if e["name"] == name), key=lambda e: e["ts"])

    peaks = counters.get("output peak", [])
    peak_ts = [t for t, _ in peaks]
    max_peak = max((v for _, v in peaks), default=0.0)
    LOUD = LOUD_FRACTION * max_peak
    SILENT = SILENT_FRACTION * max_peak
    out_latency = counters.get("pa output latency ms", [])
    out_latency_ts = [t for t, _ in out_latency]

    def reported_output_latency_ms(t):
        i = bisect.bisect_right(out_latency_ts, t) - 1
        return out_latency[i][1] if i >= 0 else 0.0

    print("== Control latency (mark -> first callback with the expected output level)")
    print("   'audible' adds the output latency PortAudio reported for the stream")
    print("   loudest output peak %.4f" % max_peak)
    results = {}
    for mark in sorted((e for e in zones if e["name"].startswith("expect ")), key=lambda e: e["ts"]):
        expectation, action = mark["name"][len("expect "):].split(": ", 1)
        rising = expectation == "sound"
        t0 = mark["ts"]
        i = bisect.bisect_left(peak_ts, t0)
        hit = None
        for t, value in peaks[i:]:
            if t - t0 > LOOKAHEAD_US:
                break
            if (rising and value >= LOUD) or (not rising and value <= SILENT):
                hit = t
                break
        if hit is None and not rising and i > 1:
            # A stopped stream reports no silent callback: the sound ends after the
            # last callback of the run, which can start before the mark
            j = i - 1
            while j + 1 < len(peaks) and peaks[j + 1][0] - peaks[j][0] <= STREAM_GAP_US:
                j += 1
            hit = peaks[j][0] + (peaks[j][0] - peaks[j - 1][0])
        entry = results.setdefault(action, {"app": [], "audible": [], "missed": 0})
        if hit is None:
            entry["missed"] += 1
            continue
        app_ms = (hit - t0) / 1000.0
        entry["app"].append(app_ms)
        entry["audible"].append(app_ms + reported_output_latency_ms(hit))

    for action, entry in results.items():
        print("  %s" % action)
        print("    app      %s" % summary(entry["app"]))
        print("    audible  %s" % summary(entry["audible"]))
        if entry["missed"]:
            print("    expected level not reached for %d action(s)" % entry["missed"])

    print("\n== Display sync (tone ends at %.1f s in control.wav)" % TONE_END_S)
    playhead = counters.get("playhead s", [])
    for k in range(1, len(playhead)):
        (t_prev, p_prev), (t, p) = playhead[k - 1], playhead[k]
        if p_prev < TONE_END_S <= p and p - p_prev < 0.5:
            crossing = t_prev + (t - t_prev) * (TONE_END_S - p_prev) / (p - p_prev)
            # The loud -> silent callback step nearest to the crossing
            i = max(bisect.bisect_left(peak_ts, crossing - LOOKAHEAD_US), 1)
            j = bisect.bisect_left(peak_ts, crossing + LOOKAHEAD_US)
            steps = [peaks[n][0] for n in range(i, j) if peaks[n - 1][1] >= LOUD and peaks[n][1] <= SILENT]
            silent = min(steps, key=lambda st: abs(st - crossing)) if steps else None
            if silent is None:
                print("  playhead crossed %.1f s, but no tone-to-silence step found" % TONE_END_S)
                continue
            audible = silent + reported_output_latency_ms(silent) * 1000.0
            print("  playhead crosses %.1f s %+.1f ms vs the audible end of the tone (+ = playhead late)"
                  % (TONE_END_S, (crossing - audible) / 1000.0))
            print("  playhead update interval  %s" % summary([(playhead[j][0] - playhead[j - 1][0]) / 1000.0
                                                             for j in range(max(1, k - 20), k + 1)]))

    # Forward jumps not caused by an action: the playhead must not move faster than the clock
    actions = sorted(e["ts"] for e in zones if e["name"].startswith("control: "))
    jumps = []
    for k in range(1, len(playhead)):
        (t_prev, p_prev), (t, p) = playhead[k - 1], playhead[k]
        j = bisect.bisect_right(actions, t) - 1
        if j >= 0 and t - actions[j] < ACTION_SETTLE_US:
            continue
        # The first update of a new playback starts from wherever the last one stopped
        if t - t_prev > NEW_PLAYBACK_GAP_US:
            continue
        excess_ms = 1000.0 * (p - p_prev) - (t - t_prev) / 1000.0
        if excess_ms > JUMP_MS:
            jumps.append(excess_ms)
    print("  playhead jumps ahead of the clock: %d%s" % (len(jumps), (" (" + ", ".join("%+.0f ms" % v for v in jumps) + ")") if jumps else ""))

    print("\n== GUI thread time inside control handlers")
    for name in sorted(set(e["name"] for e in zones if e["name"].startswith("control: "))):
        print("  %-28s %s" % (name[len("control: "):], summary([e["dur"] / 1000.0 for e in zones_named(name)])))

    print("\n== Transport phases (GUI thread unless noted)")
    for title, names in (("start", START_PHASES), ("stop", STOP_PHASES)):
        print("  %s" % title)
        for name in names:
            durations = [e["dur"] / 1000.0 for e in zones_named(name)]
            if durations:
                print("    %-40s %s" % (name, summary(durations)))

    print("\n== Stream as opened")
    for name in ("suggested latency ms", "pa output latency ms", "pa input latency ms"):
        values = sorted(set(round(v, 3) for _, v in counters.get(name, [])))
        print("  %-24s %s" % (name, ", ".join("%g" % v for v in values) or "none"))

    print("\n== Audio callback")
    callbacks = zones_named("audio callback")
    print("  duration       %s" % summary([e["dur"] / 1000.0 for e in callbacks]))
    print("  load           %s" % summary([v for _, v in counters.get("callback load %", [])], "%"))
    periods = [v for _, v in counters.get("callback period ms", []) if v < MAX_PERIOD_MS]
    print("  period         %s" % summary(periods))
    flags = [int(v) for _, v in counters.get("pa status flags", [])]
    flag_counts = {name: sum(1 for f in flags if f & (1 << bit)) for bit, name in enumerate(PA_FLAGS)}
    print("  status flags   %s" % (", ".join("%s %d" % kv for kv in flag_counts.items() if kv[1]) or "none"))
    underruns = counters.get("ring underrun frames", [])
    print("  ring underruns %d callbacks, %d frames zero-padded" % (len(underruns), sum(v for _, v in underruns)))
    lost = counters.get("callback records lost", [])
    if lost:
        print("  records lost   %d (drain fell behind)" % sum(v for _, v in lost))

    print("\n== Producer thread")
    print("  SequenceBufferExchange %s" % summary([e["dur"] / 1000.0 for e in zones_named("SequenceBufferExchange")]))
    queue = [v for _, v in counters.get("playback queue ms", [])]
    if queue:
        print("  playback queue          min %.1f  p50 %.1f  max %.1f ms" % (min(queue), pct(queue, 0.5), max(queue)))


if __name__ == "__main__":
    main()

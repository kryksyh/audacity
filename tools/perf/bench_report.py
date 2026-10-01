#
# Audacity: A Digital Audio Editor
#
# Per-phase frame stats for a trace written by tools/perf/timeline_zoom_scroll.js.
# A frame interval counts as slow only if the GUI thread was busy for most of it;
# otherwise it is a gap where nothing needed rendering.
#
import json, sys, bisect, statistics as st

d = json.load(open(sys.argv[1]))["traceEvents"]
X = [e for e in d if e["ph"] == "X"]
phases = sorted([e for e in X if e["name"].startswith("bench: ")], key=lambda e: e["ts"])
fi = sorted((e["ts"], e["args"]["value"]) for e in d if e.get("name") == "frame interval ms")
fts = [t for t, _ in fi]

def zones(name):
    return sorted([e for e in X if e["name"] == name], key=lambda e: e["ts"])

busy, rend, sync, upd = zones("GUI busy"), zones("SG render"), zones("SG sync"), zones("TrackItemsListModel::updateItemsMetrics")
passes = zones("SG pass")
bts = [e["ts"] for e in busy]
waves = sorted(e["ts"] for e in X if e["name"] == "WaveView::paint")

def in_win(lst, a, b):
    return [e for e in lst if a <= e["ts"] < b]

def busy_in(a, b):
    i = max(bisect.bisect_left(bts, a) - 1, 0)
    total = 0
    for e in busy[i:]:
        if e["ts"] >= b:
            break
        total += max(0, min(b, e["ts"] + e["dur"]) - max(a, e["ts"]))
    return total

def pct(v, q):
    return sorted(v)[min(int(q * len(v)), len(v) - 1)] if v else 0

# a long interval with an idle GUI thread is "nothing to render", not a dropped frame
def real_intervals(a, b):
    out = []
    for t, v in fi[bisect.bisect_left(fts, a):bisect.bisect_left(fts, b)]:
        start = t - v * 1000
        if v <= 17 or busy_in(start, t) >= 0.5 * v * 1000:
            out.append(v)
    return out

print(f"{'phase':20s} {'frames':>6s} {'p50':>5s} {'p95':>6s} {'p99':>6s} {'max':>6s} {'>17ms':>6s} | {'maxGUI':>7s} {'GUI%':>5s} {'metrics':>8s} | {'render':>6s} {'pass':>5s} {'sync':>5s} {'waves':>6s}")
all_iv, worst_gui = [], 0.0
for p in phases:
    a, b = p["ts"], p["ts"] + p["dur"]
    iv = real_intervals(a, b)
    gslices = [e["dur"] / 1000 for e in in_win(busy, a, b)]
    if "idle" not in p["name"]:
        all_iv += iv
        worst_gui = max([worst_gui] + gslices)
    if not iv:
        print(f"{p['name'][7:]:20s} {'-':>6s}")
        continue
    gui = busy_in(a, b) / p["dur"] * 100
    met = sum(e["dur"] for e in in_win(upd, a, b)) / 1000 / len(iv)
    r = in_win(rend, a, b); s = in_win(sync, a, b); pp = in_win(passes, a, b)
    nw = bisect.bisect_left(waves, b) - bisect.bisect_left(waves, a)
    print(f"{p['name'][7:]:20s} {len(iv):6d} {st.median(iv):5.1f} {pct(iv, .95):6.1f} {pct(iv, .99):6.1f} {max(iv):6.1f} {sum(v > 17 for v in iv):6d} | "
          f"{max(gslices) if gslices else 0:6.1f}  {gui:5.0f} {met:7.2f}ms | {st.mean(e['dur'] for e in r) / 1000 if r else 0:6.2f} {st.mean(e['dur'] for e in pp) / 1000 if pp else 0:5.2f} {st.mean(e['dur'] for e in s) / 1000 if s else 0:5.2f} {nw / len(iv):6.1f}")
if all_iv:
    m17 = sum(v > 17 for v in all_iv)
    print(f"\nACTIVE: frames {len(all_iv)}, p50 {st.median(all_iv):.1f} p95 {pct(all_iv, .95):.1f} p99 {pct(all_iv, .99):.1f} max {max(all_iv):.1f} ms, "
          f">17ms {m17} ({100 * m17 / len(all_iv):.1f}%), longest GUI slice {worst_gui:.1f} ms")
    print("TARGET (no frame > 17 ms):", "MET" if m17 == 0 else "NOT MET")

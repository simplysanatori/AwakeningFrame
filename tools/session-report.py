#!/usr/bin/env python3
"""One table from the timing logs of a VR session (standard library only).

  tools/session-report.py LOG [LOG ...]       named logs, one column each
  tools/session-report.py DIR                 current and previous session in a data folder

A LOG is one of
  - the app's log (or its xr timing log) with `xrt` per-frame lines, see docs/af/TOOLING.md;
    the 10-second summary lines GalaxyFrame logs ("vr: N of M game frames shown twice", ...) are
    read too when present;
  - the mock runtime's timing file (MOCK_XR_TIMING of tools/xrmock, a CSV with a '# frame,...' header);
  - a vrtiming.log written by tools/vrtiming.py (SteamVR's compositor view; the file name must
    contain "vrt"), which adds the compositor rows to the column of the log before it.
In DIR mode the files are xr_timing.log / lasrecomp.log with vrtiming.log, and their .prev
copies as "previous".  DIR defaults to $AWAKENINGFRAME_HOME or ${XDG_DATA_HOME:-~/.local/share}/lasrecomp.

A hitch is a moment over 100 ms: a gap over 100 ms between frames, a GPU time over 100 ms, a
client frame interval over 100 ms in the compositor's log.  "Refreshes lost" is, for each gap
between two frames, round(gap / display period) - 1.  "Late" is an xrEndFrame less than half a
period before the predicted display time (or the app's own late=1 flag).
"""
import os
import re
import sys

HITCH_MS = 100.0
APP_LOGS = ("xr_timing.log", "lasrecomp.log")


def secs(stamp):
    h, m, s = stamp.split(":")
    return int(h) * 3600 + int(m) * 60 + float(s)


def new_session(name):
    return {"name": name, "frames": [], "first": None, "last": None, "shown_twice": 0, "game_frames": 0,
            "missed": 0, "sets_late": 0, "sets": 0, "eye_avg": [], "eye_max": 0.0, "hitch_notes": [], "hitch": 0}


KEYS = ("f", "t", "pdt", "period", "wait", "begin", "end", "gpu0", "gpu1", "hud", "late")


def parse_app_log(path):
    """xrt lines (key=value), the mock's CSV, and GalaxyFrame's summary lines."""
    r = new_session(os.path.basename(path))
    day, prev = 0, None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#") or not line.strip():
                continue
            m = re.match(r"(\d\d:\d\d:\d\d\.\d+) (.*)", line)
            msg = m.group(2) if m else line
            if m:
                t = secs(m.group(1))
                if prev is not None and t < prev - 3600:
                    day += 1
                prev = t
                t += day * 86400
                r["first"] = t if r["first"] is None else r["first"]
                r["last"] = t
            g = re.match(r"xrt (.*)", msg)
            if g:
                d = {}
                for tok in g.group(1).split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        try:
                            d[k] = float(v)
                        except ValueError:
                            pass
                if "t" in d:
                    r["frames"].append(d)
                continue
            if re.match(r"^\d+,", msg):  # mock CSV: frame,wait_call,wait_return,wait_block,predicted,period,begin,end,b2e,margin,layers,lost,late
                c = msg.split(",")
                if len(c) >= 13:
                    r["frames"].append({"f": float(c[0]), "t": float(c[2]), "wait": float(c[3]), "pdt": float(c[4]), "period": float(c[5]),
                                        "begin": float(c[6]), "end": float(c[7]), "late": float(c[12])})
                continue
            g = re.match(r"vr: (\d+) of (\d+) game frames shown twice", msg)
            if g:
                r["shown_twice"] += int(g.group(1))
                r["game_frames"] += int(g.group(2))
                continue
            g = re.match(r"vr: the frame loop missed (\d+) refreshes in the last [\d.]+ s \(\d+ with its own work late\); (\d+) of (\d+) sets not finished when due", msg)
            if g:
                r["missed"] += int(g.group(1))
                r["sets_late"] += int(g.group(2))
                r["sets"] += int(g.group(3))
                continue
            g = re.match(r"vr: render scale ([\d.]+)-([\d.]+) \(now [\d.]+, (\d+) changes\); eye GPU ([\d.]+) ms avg ([\d.]+) max", msg)
            if g:
                r["eye_avg"].append(float(g.group(4)))
                r["eye_max"] = max(r["eye_max"], float(g.group(5)))
    return r


def parse_vrtiming(path):
    r = {"seconds": 0, "frames": 0, "reused": 0, "mis": 0, "dropped": 0, "rp_cpu": 0, "rp_gpu": 0, "comp_gpu": [],
         "interval_max": 0.0, "interval_avg": [], "hitch": 0, "hitch_notes": []}
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("#"):
                continue
            g = re.match(r"(\d\d:\d\d:\d\d)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(-?[\d.]+)\s+([\d.]+)/([\d.]+)\s+([\d.]+)/([\d.]+)\s+([\d.]+)", line)
            if not g:
                continue
            v = [float(x) for x in g.groups()[1:]]
            r["seconds"] += 1
            r["frames"] += int(v[0]); r["reused"] += int(v[1]); r["mis"] += int(v[2]); r["dropped"] += int(v[3])
            r["rp_cpu"] += int(v[4]); r["rp_gpu"] += int(v[5])
            r["comp_gpu"].append(v[8]); r["interval_avg"].append(v[9]); r["interval_max"] = max(r["interval_max"], v[10])
            if v[10] > HITCH_MS:
                r["hitch"] += 1
                r["hitch_notes"].append("%s client interval max %.0f ms" % (g.group(1), v[10]))
    return r


def mean(xs):
    return sum(xs) / len(xs) if xs else None


def pct(xs, p):
    if not xs:
        return None
    s = sorted(xs)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))]


def fmt(v, spec="%.1f"):
    return "-" if v is None else spec % v


def derive(g):
    """Statistics of the xrt frames of one session."""
    fr = g["frames"]
    d = {"n": len(fr)}
    if len(fr) < 2:
        return d
    ts = [x["t"] for x in fr]
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    period = mean([x["period"] for x in fr if x.get("period", 0) > 0]) or 8.333
    d["period"] = period
    d["hz"] = 1000.0 * (len(fr) - 1) / (ts[-1] - ts[0]) if ts[-1] > ts[0] else None
    d["gap_avg"], d["gap_p99"], d["gap_max"] = mean(gaps), pct(gaps, 0.99), max(gaps)
    d["lost"] = sum(max(0, round(x / period) - 1) for x in gaps)
    d["hitches"] = [x for x in gaps if x > HITCH_MS]
    d["wait"] = mean([x["wait"] for x in fr if "wait" in x])
    be = [x["end"] - x["begin"] for x in fr if "end" in x and "begin" in x]
    d["b2e_avg"], d["b2e_max"] = mean(be), (max(be) if be else None)
    mar = [x["pdt"] - x["end"] for x in fr if "pdt" in x and "end" in x]
    d["margin_min"], d["margin_avg"] = (min(mar) if mar else None), mean(mar)
    late = 0
    for x in fr:
        if "late" in x and x["late"]:
            late += 1
        elif "late" not in x and "pdt" in x and "end" in x and x["pdt"] - x["end"] < period / 2:
            late += 1
    d["late"] = late
    for k in ("gpu0", "gpu1", "hud"):
        v = [x[k] for x in fr if x.get(k, -1) >= 0]
        d[k] = (mean(v), max(v), sum(1 for y in v if y > period)) if v else None
    return d


def build(sessions):
    rows = []

    def add(label, fn):
        cells = []
        for _, g, v in sessions:
            try:
                cells.append(fn(g, v))
            except Exception:
                cells.append("-")
        rows.append((label, cells))

    D = {id(s[1]): derive(s[1]) for s in sessions if s[1]}
    add("frames (xrt)", lambda g, v: str(D[id(g)]["n"]) if g and D[id(g)]["n"] else "-")
    add("display period (ms)", lambda g, v: fmt(D[id(g)]["period"], "%.2f") if g else "-")
    add("effective frame rate (Hz)", lambda g, v: fmt(D[id(g)]["hz"]) if g else "-")
    add("frame gap: avg / p99 / max ms", lambda g, v: "%s / %s / %s" % (fmt(D[id(g)]["gap_avg"], "%.2f"), fmt(D[id(g)]["gap_p99"], "%.2f"), fmt(D[id(g)]["gap_max"])) if g else "-")
    add("refreshes lost", lambda g, v: str(D[id(g)]["lost"]) if g and D[id(g)]["n"] > 1 else "-")
    add("xrWaitFrame blocked avg (ms)", lambda g, v: fmt(D[id(g)]["wait"], "%.2f") if g else "-")
    add("begin to end: avg / max ms", lambda g, v: "%s / %s" % (fmt(D[id(g)]["b2e_avg"], "%.2f"), fmt(D[id(g)]["b2e_max"])) if g else "-")
    add("end before display time: min / avg ms", lambda g, v: "%s / %s" % (fmt(D[id(g)]["margin_min"]), fmt(D[id(g)]["margin_avg"])) if g else "-")
    add("late frames", lambda g, v: "%d (%s%%)" % (D[id(g)]["late"], fmt(100.0 * D[id(g)]["late"] / D[id(g)]["n"])) if g and D[id(g)]["n"] else "-")
    for k, label in (("gpu0", "eye 0 GPU"), ("gpu1", "eye 1 GPU"), ("hud", "HUD GPU")):
        add(label + ": avg / max ms (over period)", lambda g, v, k=k: "%s / %s (%d)" % (fmt(D[id(g)][k][0], "%.2f"), fmt(D[id(g)][k][1]), D[id(g)][k][2]) if g and D[id(g)].get(k) else "-")
    add("game frames shown twice", lambda g, v: "%d of %d" % (g["shown_twice"], g["game_frames"]) if g and g["game_frames"] else "-")
    add("sets not finished when due", lambda g, v: "%d of %d" % (g["sets_late"], g["sets"]) if g and g["sets"] else "-")
    add("eye GPU 10 s summaries: avg / max ms", lambda g, v: "%s / %s" % (fmt(mean(g["eye_avg"]), "%.2f"), fmt(g["eye_max"])) if g and g["eye_avg"] else "-")
    add("compositor seconds logged", lambda g, v: str(v["seconds"]) if v else "no vrtiming.log")
    add("compositor frames", lambda g, v: str(v["frames"]) if v else "-")
    add("  app frames reused", lambda g, v: "%d (%s%%)" % (v["reused"], fmt(100.0 * v["reused"] / v["frames"])) if v and v["frames"] else "-")
    add("  mispresented / dropped", lambda g, v: "%d / %d" % (v["mis"], v["dropped"]) if v else "-")
    add("  reprojected (cpu / gpu flagged)", lambda g, v: "%d / %d" % (v["rp_cpu"], v["rp_gpu"]) if v else "-")
    add("compositor GPU ms avg", lambda g, v: fmt(mean([x for x in v["comp_gpu"] if x >= 0]), "%.2f") if v else "-")
    add("client frame interval: avg / max ms", lambda g, v: "%s / %s" % (fmt(mean(v["interval_avg"]), "%.2f"), fmt(v["interval_max"])) if v else "-")

    def hitches(g, v):
        n = len(D[id(g)].get("hitches", [])) if g else 0
        return str(n + (v["hitch"] if v else 0))
    add("hitches > %d ms" % HITCH_MS, hitches)
    return rows


def main(argv):
    if argv and argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    sessions = []
    if not argv or (len(argv) == 1 and os.path.isdir(argv[0])):
        d = argv[0] if argv else os.environ.get("AWAKENINGFRAME_HOME") or os.path.join(os.environ.get("XDG_DATA_HOME", os.path.expanduser("~/.local/share")), "lasrecomp")
        for label, suffix in (("current", ""), ("previous", ".prev")):
            lp = next((os.path.join(d, n + suffix) for n in APP_LOGS if os.path.exists(os.path.join(d, n + suffix))), None)
            vp = os.path.join(d, "vrtiming.log" + suffix)
            vp = vp if os.path.exists(vp) else None
            if lp or vp:
                sessions.append((label, lp, vp))
    else:
        i = 0
        while i < len(argv):
            lp, vp = argv[i], None
            if "vrt" in os.path.basename(lp):
                lp, vp = None, argv[i]
            elif i + 1 < len(argv) and "vrt" in os.path.basename(argv[i + 1]):
                vp = argv[i + 1]
                i += 1
            sessions.append((os.path.basename(lp or vp), lp, vp))
            i += 1
    if not sessions:
        print("no logs found", file=sys.stderr)
        return 1
    parsed = [(label, parse_app_log(lp) if lp else None, parse_vrtiming(vp) if vp else None) for label, lp, vp in sessions]
    rows = build(parsed)
    w0 = max(len(r[0]) for r in rows)
    ws = [max(len(parsed[k][0]), max(len(r[1][k]) for r in rows)) for k in range(len(parsed))]
    print("%-*s  %s" % (w0, "", "  ".join("%-*s" % (ws[k], parsed[k][0]) for k in range(len(parsed)))))
    for label, cells in rows:
        print("%-*s  %s" % (w0, label, "  ".join("%-*s" % (ws[k], cells[k]) for k in range(len(cells)))))
    notes = []
    for label, g, v in parsed:
        if v and v["hitch_notes"]:
            notes.append("%s (compositor): %s" % (label, "; ".join(v["hitch_notes"][:8])))
    if notes:
        print()
        print("\n".join(notes))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

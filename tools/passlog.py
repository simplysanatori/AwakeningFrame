#!/usr/bin/env python3
# passlog.py run.log [--frame N] [--md] [--counts]: the frame audit of a LARECOMP_PASSLOG run (runtime/nvn/vulkan/passlog.h).
# For each logged frame (the first of each logged window by default; --frame N for one, --all for every one):
#   - every render pass: eye, size, multiview or not, attachments with their load op, draws (draws that joined it with the
#     pass diet's merge included), clears in it, the fragment hashes of its draws and the textures they sample;
#   - every copy: kind, source, destination, size, and the destination's next use, followed into the next frames of the
#     log: read (by a draw, a dispatch, a copy, a pass that loads it, the eye post hook) or overwritten first;
#   - every dispatch; and the counts: passes, multiview passes, empty passes (no draw), passes that only clear, draws,
#     clears, copies, full-size copies, dispatches, full-size texture reads (a pass sampling a full-size texture, each
#     distinct texture once per pass; copies from a full-size texture).
# "Full size" is the eye image size (the largest twinned scalable target in the log).
# --counts: the counts only (one line a frame), for comparing two runs; --md: Markdown tables.
import re, sys
from collections import defaultdict, OrderedDict

VKF = {37: "RGBA8", 43: "RGBA8srgb", 44: "BGRA8", 50: "BGRA8srgb", 64: "A2B10G10R10", 76: "R16F", 83: "RG16F", 97: "RGBA16F",
       100: "R32F", 103: "RG32F", 109: "RGBA32F", 122: "B10G11R11F", 124: "D16", 125: "X8D24", 126: "D32F", 127: "S8", 129: "D24S8",
       130: "D32FS8", 9: "R8", 16: "RG8", 70: "R16", 77: "RG16", 91: "RGBA16", 98: "R32ui", 107: "RGBA32ui"}

LINE = re.compile(r"\[(?:pl|plog)\] f=(\d+) e=(-?\d+) (\S) ?(.*)$")
TEX = re.compile(r"\[(?:pl|plog)\] I (\w+) fmt=(\S+) g=(\d+)x(\d+) img=(\d+)x(\d+) vk=(\d+) lv=(\d+) ly=(\d) tw=(\d) sc=(\d) ao=(\d) sh=(\d) pres=(\d) \"(.*)\"")


def parse(path):
    tex, ev = {}, []
    for ln in open(path, errors="replace"):
        m = TEX.search(ln)
        if m:
            g = m.groups()
            tex[g[0]] = {"fmt": g[1], "gw": int(g[2]), "gh": int(g[3]), "w": int(g[4]), "h": int(g[5]), "vk": int(g[6]), "ly": int(g[8]),
                         "tw": g[9] == "1", "sc": g[10] == "1", "ao": g[11] == "1", "sh": g[12] == "1", "pres": g[13] == "1", "label": g[14]}
            continue
        m = LINE.search(ln)
        if m: ev.append((int(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))
    return tex, ev


def tname(tex, t):
    i = tex.get(t)
    if not i: return t
    return "%s %s %dx%d%s" % (t, VKF.get(i["vk"], str(i["vk"])), i["w"], i["h"], " L" if i["ly"] == 2 else "")


def eye_size(tex):
    best = (0, 0)
    for i in tex.values():
        if i["sc"] and i["tw"] and not i["ao"] and i["w"] * i["h"] > best[0] * best[1]: best = (i["w"], i["h"])
    return best


def frames_in(ev):
    out = OrderedDict()
    for k, e in enumerate(ev): out.setdefault(e[0], []).append(k)
    return out


def next_use(ev, k0, t, tex=None):
    """the destination t of the copy at ev[k0]: its next use from there on (all later events of the log)"""
    k, use = next_use_(ev, k0, t, tex)
    return k, use


def next_use_(ev, k0, t, tex):
    global note
    note = ""
    stencil = lambda t: tex is not None and t in tex and tex[t]["vk"] in (127, 128, 129, 130)
    r = _scan(ev, k0, t, stencil)
    return r[0], note + r[1]


def _scan(ev, k0, t, stencil):
    global note
    loaded = None  # (a pass that loads t: read at its first draw, unless a whole clear of t comes first)
    pass_att = {}
    for k in range(k0 + 1, len(ev)):
        f, e, kind, rest = ev[k]
        w = rest.split()
        if kind == "B":
            pass_att = dict(a.split("=", 1) for a in w[3:])
            loaded = None
            for slot, v in pass_att.items():
                tid, op = v.split(":")
                if tid == t: loaded = (slot, op)
            if loaded and loaded[1] == "C": return k, "overwritten (pass clears it)"
        elif kind == "K" and loaded:
            slot = loaded[0]
            if w[1] == "whole=1" and slot == "d" and w[0] == "ds": return k, "overwritten (whole clear)"
            if w[1] == "whole=1" and slot == "d" and w[0] == "d":
                if not stencil(t): return k, "overwritten (whole clear)"
                note = "depth cleared (stencil kept), then "   # (the stencil may still be read: follow it on)
                loaded = None
            elif w[1] == "whole=1" and w[0] == slot: return k, "overwritten (whole clear)"
        elif kind == "D":
            ts = w[1][2:].split(",")
            if loaded: return k, "read (pass loads it, draw %s)" % w[0][3:11]
            if t in ts: return k, "read (draw %s)" % w[0][3:11]
        elif kind == "E":
            loaded = None
        elif kind == "C":
            ts = [x.replace("I:", "") for x in w[2][2:].split(",")]
            if t in ts: return k, "read (dispatch %s)" % w[0][3:]
        elif kind == "X":
            if "ly=1" in w: continue  # (the second layer of a multiview copy: the same copy)
            s, d = w[1].split(">")
            if s == t: return k, "read (%s source)" % w[0]
            if d == t and "whole=1" in w: return k, "overwritten (%s)" % w[0]
        elif kind == "R" and w[0] == t: return k, "read (%s)" % " ".join(w[1:])
        elif kind == "W" and w[0] == t: return k, "overwritten (%s)" % " ".join(w[1:])
    return None, "not seen again in the log"


def audit(tex, ev, frame, md, counts_only):
    fr = frames_in(ev)
    ks = fr.get(frame, [])
    ew, eh = eye_size(tex)
    full = lambda t: t in tex and tex[t]["w"] == ew and tex[t]["h"] == eh
    passes, cur = [], None
    copies, disp, clears = [], [], []
    for k in ks:
        f, e, kind, rest = ev[k]
        w = rest.split()
        if kind == "B":
            att = [a.split("=", 1) for a in w[3:]]
            cur = {"seq": int(w[0]), "eye": e, "size": w[1], "mv": w[2] == "vm=3", "att": att, "draws": 0, "clears": [], "fs": OrderedDict(),
                   "samp": set(), "k": k}
            passes.append(cur)
        elif kind == "E" and cur:
            cur["draws_logged"] = int(w[1].split("=")[1])
        elif kind == "D" and cur:
            cur["draws"] += 1
            cur["fs"][w[0][3:11]] = cur["fs"].get(w[0][3:11], 0) + 1
            for t in w[1][2:].split(","):
                if t != "": cur["samp"].add(t)
        elif kind == "K" and cur:
            cur["clears"].append((w[0], w[1] == "whole=1", w[2] == "draw=1", int(w[3].split("=")[1])))
        elif kind == "X":
            if "ly=1" in w: continue
            s, d = w[1].split(">")
            nk, use = next_use(ev, k, d, tex) if w[0] != "snapshot" else (None, "(feedback snapshot: read by the draw that needs it)")
            where = ""
            if nk is not None:
                df = ev[nk][0] - f
                where = "same frame" if df == 0 else "next frame" if df == 1 else "%d frames later" % df
            copies.append({"kind": w[0], "eye": e, "src": s, "dst": d, "size": w[2], "whole": "whole=1" in w, "use": use, "where": where,
                           "full": full(d) or (w[0] == "snapshot" and full(s))})
        elif kind == "C":
            disp.append({"cs": w[0][3:], "grid": w[1], "tex": w[2][2:]})
    n = {"passes": len(passes), "multiview": sum(p["mv"] for p in passes), "empty": sum(1 for p in passes if not p["draws"]),
         "clear only": sum(1 for p in passes if not p["draws"] and p["clears"]), "draws": sum(p["draws"] for p in passes),
         "clears": sum(len(p["clears"]) for p in passes), "copies": len(copies), "full-size copies": sum(c["full"] for c in copies),
         "dispatches": len(disp),
         "full-size reads": sum(sum(1 for t in p["samp"] if full(t)) for p in passes) + sum(1 for c in copies if full(c["src"])),
         "full-size passes": sum(1 for p in passes if p["att"] and any(full(a[1].split(":")[0]) for a in p["att"]))}
    if counts_only:
        print("frame %d: " % frame + ", ".join("%s %d" % kv for kv in n.items()))
        return n
    print("## frame %d (eye images %dx%d)\n" % (frame, ew, eh))
    print("Counts: " + ", ".join("%s %d" % kv for kv in n.items()) + "\n")
    print("| # | eye | size | MV | attachments (load) | draws | clears (whole, by draw) | fragment shaders (draws) | samples |")
    print("|---|---|---|---|---|---|---|---|---|")
    for p in passes:
        att = "; ".join("%s %s %s" % (s, tname(tex, v.split(":")[0]), {"L": "LOAD", "C": "CLEAR"}[v.split(":")[1]]) for s, v in p["att"])
        cl = ", ".join("%s%s%s" % (c[0], " whole" if c[1] else " part", " draw" if c[2] else "") for c in p["clears"]) or "-"
        fs = ", ".join("%s x%d" % kv for kv in list(p["fs"].items())[:4]) + (" ..." if len(p["fs"]) > 4 else "")
        sm = ", ".join(tname(tex, t) for t in sorted(p["samp"])[:5]) + (" ..." if len(p["samp"]) > 5 else "")
        print("| %d | %d | %s | %s | %s | %d | %s | %s | %s |" % (p["seq"], p["eye"], p["size"], "yes" if p["mv"] else "no", att, p["draws"], cl,
                                                              fs or "-", sm or "-"))
    print("\n| copy | eye | source | destination | size | next use of the destination | when |")
    print("|---|---|---|---|---|---|---|")
    for c in copies:
        print("| %s | %d | %s | %s | %s | %s | %s |" % (c["kind"], c["eye"], tname(tex, c["src"]), tname(tex, c["dst"]) if c["dst"] != "000000" else "-",
                                                c["size"], c["use"], c["where"]))
    print("\n| dispatch | grid | textures (I: image) |")
    print("|---|---|---|")
    for d in disp:
        print("| %s | %s | %s |" % (d["cs"], d["grid"], ", ".join(tname(tex, t.replace("I:", "")) + (" (image)" if t.startswith("I:") else "")
                                                            for t in d["tex"].split(",") if t and t != "-")))
    print()
    return n


def main():
    a = sys.argv[1:]
    path = a[0]
    md, counts, allf = "--md" in a, "--counts" in a, "--all" in a
    tex, ev = parse(path)
    fr = list(frames_in(ev))
    if "--frame" in a: want = [int(a[a.index("--frame") + 1])]
    elif allf: want = fr
    else:  # the first frame of each logged window
        want = [f for i, f in enumerate(fr) if i == 0 or f != fr[i - 1] + 1]
    for f in want: audit(tex, ev, f, md, counts)


if __name__ == "__main__":
    main()

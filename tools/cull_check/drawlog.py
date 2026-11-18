#!/usr/bin/env python3
"""drawlog.py <run.log> [--frame N] [--md] [--radii 21.8,20,18,16,14]: what a frame draws against what the eyes can see.

Reads the executor's draw log (LARECOMP_VR_DRAWLOG=<frame>[,...], docs/af/PASSES.md "Draws the eyes cannot see"): every
draw of eye 0's run of those frames with its pass, count, instances, blend and world box, and per frame the table's
centre, the round fade's circle (Table Edge Fade) and both eyes' positions and view-projections. Per logged frame (the
first of each run of consecutive frames, or --frame):

  - per pass (shadow casters, the eyes' full-size depth pre-pass and colour pass, the eyes' smaller passes, other shared
    passes): draws, indices (count x instances), and how many have no world box (instanced, unplaceable) or are blended;
  - the eyes' full-size draws and the casters by the distance of their box from the fade's centre (histogram);
  - how many lie wholly past the fade's black radius, wholly past its clear radius, outside both eyes' views, and the
    shadow casters whose box swept along the light reaches nothing the eyes see (the executor's tests, offline);
  - the trade: the same counts for smaller black radii (the eye passes' draws past r, and the share of draws wholly
    outside the square of half-size r + 1.5, an estimate of what a culling rectangle following the fade would leave out).
The tests are the executor's (exec.c seen_class_eye / seen_class_shadow): boxes padded by 0.25 units (2 for a skinned
mesh), the fade margin 1 unit, a box reaching below the floor plane taken where the eyes' rays through its bottom meet it.
"""
import argparse, math, re, sys
from collections import defaultdict

PAD, PAD_SKIN, MARGIN, WIDEN = 0.25, 2.0, 1.0, 0.3


def parse(path):
    frames = defaultdict(lambda: {'d': [], 'eyes': {}, 'blocks': {}})
    for line in open(path, errors='replace'):
        if not line.startswith('[drawlog] f'):
            continue
        m = re.match(r'\[drawlog\] f(\d+) (\S+)', line)
        f, what = int(m[1]), m[2]
        F = frames[f]
        if what == 'head:':
            h = re.search(r'seen (\d+) anchor (\d) (\S+) (\S+) (\S+) fade (\d) off (\S+) (\S+) black (\S+) light (\d) (\S+) (\S+) (\S+) ylo (\S+) scene (\d+)', line)
            F['seen'] = int(h[1]); F['anchor'] = tuple(float(h[i]) for i in (3, 4, 5)); F['fade'] = int(h[6])
            F['off'] = (float(h[7]), float(h[8])); F['black'] = float(h[9]); F['scene'] = int(h[15])
        elif what.startswith('eye'):
            e = re.search(r'pos (\S+) (\S+) (\S+) vp (.*)$', line)
            F['eyes'][int(what[3])] = (tuple(map(float, e.group(1, 2, 3))), list(map(float, e[4].split())))
        elif what == 'pass':
            b = re.search(r'pass (\d+)x\d+ block (\d+) size \d+: (.*)$', line)
            vals = [float(x) for x in b[3].replace('/', ' ').split()]
            F['blocks'].setdefault((int(b[1]), int(b[2])), vals)
        elif what == 'd':
            d = re.search(r'k(\d) w(\d+) nc(\d) fs (\S+) inst (\d+) n (\d+) ix \d bl (\d) ds \d+ s4 \d+ box (.*?)(?: class (\d+))?$', line.rstrip())
            box = None
            if d[8] != '-':
                v = d[8].split()
                box = (int(v[0]), tuple(map(float, v[1:4])), tuple(map(float, v[4:7])))
            F['d'].append(dict(k=int(d[1]), w=int(d[2]), nc=int(d[3]), fs=d[4], inst=int(d[5]), n=int(d[6]), bl=int(d[7]), box=box,
                               cls=int(d[9]) if d[9] else None))
    return frames


def planes(vp, widen=0.0):
    pl = []
    for i in range(4):
        row, sg = i // 2, (-1 if i & 1 else 1)
        pl.append([(1 + widen) * vp[12 + j] + sg * vp[row * 4 + j] for j in range(4)])
    pl.append([vp[12 + j] for j in range(4)])
    return pl


def out_of(pls, lo, hi):  # the box outside every frustum in pls
    for pl in pls:
        if not any(p[0] * (hi[0] if p[0] >= 0 else lo[0]) + p[1] * (hi[1] if p[1] >= 0 else lo[1]) + p[2] * (hi[2] if p[2] >= 0 else lo[2]) + p[3] < 0
                   for p in pl):
            return False
    return True


def rect_dist(c, lx, lz, hx, hz):
    dx = max(lx - c[0], c[0] - hx, 0.0); dz = max(lz - c[1], c[1] - hz, 0.0)
    return math.hypot(dx, dz)


def fade_dist(c, floor_y, eyes, lo, hi, rays=True):  # the box's nearest distance as the fade sees it
    lx, lz, hx, hz = lo[0], lo[2], hi[0], hi[2]
    if rays and lo[1] < floor_y:
        for E in eyes:
            for cx in (lo[0], hi[0]):
                for cz in (lo[2], hi[2]):
                    t = (E[1] - floor_y) / (E[1] - lo[1])
                    px, pz = E[0] + t * (cx - E[0]), E[2] + t * (cz - E[2])
                    lx, hx, lz, hz = min(lx, px), max(hx, px), min(lz, pz), max(hz, pz)
    return rect_dist(c, lx, lz, hx, hz)


MIRROR = False  # (--mirror) the fade as the edge pass draws it under a lower-left origin: each eye's rays mirrored vertically


def eye_mirror(E, vp):  # world point -> where that eye's fade pass puts it with its rays mirrored (y -> -y + 2 P6/P5 depth)
    dot = lambda a, b: sum(x * y for x, y in zip(a, b))
    r2 = [-x for x in vp[12:15]]
    p2, p6 = dot(vp[0:3], r2), dot(vp[4:7], r2)
    r0 = [a - p2 * b for a, b in zip(vp[0:3], r2)]; p0 = math.sqrt(dot(r0, r0)); r0 = [x / p0 for x in r0]
    r1 = [a - p6 * b for a, b in zip(vp[4:7], r2)]; p5 = math.sqrt(dot(r1, r1)); r1 = [x / p5 for x in r1]
    def f(w):
        d = [a - b for a, b in zip(w, E)]
        e0, e1, e2 = dot(r0, d), dot(r1, d), dot(r2, d)
        e1 = -e1 + 2.0 * p6 / p5 * -e2
        return [E[i] + e0 * r0[i] + e1 * r1[i] + e2 * r2[i] for i in range(3)]
    return f


def drawn_dist(F, c, floor_y, lo, hi, rays=True):  # fade_dist as the fade is drawn (--mirror: the nearer of the eyes')
    eyes = [F['eyes'][k][0] for k in (0, 1) if k in F['eyes']]
    if not MIRROR:
        return fade_dist(c, floor_y, eyes, lo, hi, rays)
    best = 1e30
    for k in (0, 1):
        if k not in F['eyes']:
            continue
        E, vp = F['eyes'][k]
        f = eye_mirror(E, vp)
        pts = [f((x, y, z)) for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])]
        ml = [min(p[i] for p in pts) for i in range(3)]; mh = [max(p[i] for p in pts) for i in range(3)]
        best = min(best, fade_dist(c, floor_y, [E], ml, mh, rays))
    return best


def padded(box):  # box[0]: 1 through the bone palette, 2 at the world's origin (placed some other way)
    p = PAD_SKIN if box[0] & 1 else PAD
    lo = [x - p for x in box[1]]
    lo[1] = box[1][1] - (PAD if box[0] & 1 else 0.0)
    return tuple(lo), tuple(x + p for x in box[2])


def light_dir(F):
    for (w, b), v in F['blocks'].items():
        if b == 1 and w >= 1024 and len(v) >= 16 and v[12:16] == [0, 0, 0, 1]:
            r = v[8:11]; n = math.sqrt(sum(x * x for x in r))
            d = [x / n for x in r]
            return [-x for x in d] if d[1] > 0 else d
    return None


def clear_of(black, width=12.0):
    w = min(max(black * 0.4, 3.0), width)
    return black - w


def analyse(f, F, radii, md):
    a, off = F['anchor'], F['off']
    c = (a[0] + off[0], a[2] + off[1]); floor_y = a[1]; black = F['black']; clear = clear_of(black)
    eyes = [F['eyes'][k][0] for k in (0, 1) if k in F['eyes']]
    pls = [planes(F['eyes'][k][1]) for k in (0, 1) if k in F['eyes']]
    spls = [planes(F['eyes'][k][1], WIDEN) for k in (0, 1) if k in F['eyes']]
    scene = F['scene']
    L = light_dir(F)
    inside = all(math.hypot(E[0] - c[0], E[2] - c[1]) < black - 0.5 and E[1] > floor_y + 0.1 for E in eyes)
    cats = {'casters': [], 'eye depth': [], 'eye colour': [], 'eye small': [], 'shared other': []}
    for d in F['d']:
        if d['k'] == 0:
            cats['casters' if not d['nc'] and d['w'] >= 1024 else 'shared other'].append(d)
        else:
            cats['eye small' if d['w'] != scene else 'eye depth' if d['k'] == 1 else 'eye colour'].append(d)
    out = []
    P = lambda s='': out.append(s)
    P(f'### frame {f}')
    P()
    P(f'table centre ({a[0]:.1f}, {a[1]:.1f}, {a[2]:.1f}); fade {"on" if F["fade"] else "off"}: centre ({c[0]:.1f}, {c[1]:.1f}), clear to {clear:.1f}, '
      f'black past {black:.1f}; eyes {"inside" if inside else "NOT inside"} the black circle '
      f'(at {", ".join("%.1f" % math.hypot(E[0] - c[0], E[2] - c[1]) for E in eyes)} units, {", ".join("%.1f" % (E[1] - floor_y) for E in eyes)} above the floor); '
      f'light {"(%.2f, %.2f, %.2f), %.0f deg up" % (L[0], L[1], L[2], math.degrees(math.asin(-L[1]))) if L else "unknown"}; seen flags {F.get("seen")}')
    P()
    P('| pass | draws | indices (k) | no box | instanced | blended | class 4/5 in the log |')
    P('|---|---|---|---|---|---|---|')
    for name, ds in cats.items():
        P(f'| {name} | {len(ds)} | {sum(d["n"] * max(d["inst"], 1) for d in ds) / 1000:.0f} | {sum(d["box"] is None for d in ds)} | '
          f'{sum(d["inst"] > 1 for d in ds)} | {sum(d["bl"] and d["k"] == 2 for d in ds)} | {sum((d["cls"] or 0) >= 4 for d in ds)} |')
    P()
    # the eyes' full-size draws: what the executor can place (box, not instanced, not blended in colour)
    ylo = 1e30
    rows = {}
    for name in ('eye depth', 'eye colour'):
        ds = cats[name]
        placeable = [d for d in ds if d['box'] and not d['box'][0] & 2 and d['inst'] <= 1 and not (d['k'] == 2 and d['bl'])]
        res = dict(total=len(ds), idx=sum(d['n'] * max(d['inst'], 1) for d in ds), placeable=len(placeable))
        past_black = past_clear = view = either = 0; idx_either = 0
        hist = defaultdict(int)
        for d in placeable:
            lo, hi = padded(d['box'])
            fd = drawn_dist(F, c, floor_y, lo, hi)
            pb = inside and fd > black + MARGIN
            pc = fd > clear
            ov = out_of(pls, lo, hi)
            past_black += pb; past_clear += pc; view += ov; either += pb or ov
            if pb or ov:
                idx_either += d['n']
            elif lo[1] < ylo and not (inside and F['fade'] and drawn_dist(F, c, floor_y, lo, hi, rays=False) > black + MARGIN):
                ylo = lo[1]
            dd = rect_dist(c, lo[0], lo[2], hi[0], hi[2])
            hist[0 if dd < min(5, clear) else 1 if dd < min(10, clear) else 2 if dd < clear else 3 if dd < black else 4 if dd < black + MARGIN else 5 if dd < 30 else 6] += 1
        res.update(past_black=past_black, past_clear=past_clear, view=view, either=either, idx_either=idx_either, hist=hist)
        rows[name] = res
    P(f'Eyes\' full-size passes (placeable = a world box, not instanced, not blended): by the nearest distance of the box from the fade\'s centre')
    P()
    b5, b10 = min(5, clear), min(10, clear)
    bins = [f'<{b5:g}', f'{b5:g}-{b10:.1f}', f'{b10:.1f}-{clear:.1f}', f'{clear:.1f}-{black:.1f}', f'{black:.1f}-{black + MARGIN:.1f}', f'{black + MARGIN:.1f}-30', '>30']
    P('| pass | draws | placeable | ' + ' | '.join(bins) + ' | wholly past clear | wholly past black+1 | outside both views | either (draws, indices k) |')
    P('|---|---|---|' + '---|' * len(bins) + '---|---|---|---|')
    for name, r in rows.items():
        P(f'| {name} | {r["total"]} | {r["placeable"]} | ' + ' | '.join(str(r['hist'][i]) for i in range(7)) +
          f' | {r["past_clear"]} | {r["past_black"]} | {r["view"]} | {r["either"]} ({r["idx_either"] / 1000:.0f} of {r["idx"] / 1000:.0f}) |')
    P()
    # shadow casters
    ds = cats['casters']
    placeable = [d for d in ds if d['box'] and not d['box'][0] & 2 and d['inst'] <= 1]
    sh_fade = sh_view = sh_either = 0; sidx = 0
    hist = defaultdict(int)
    for d in placeable:
        lo, hi = padded(d['box'])
        dd = rect_dist(c, lo[0], lo[2], hi[0], hi[2])
        hist[0 if dd < clear else 1 if dd < black else 2 if dd < black + MARGIN else 3] += 1
        if not L or ylo > 1e29:
            continue
        s = [lo[0], min(lo[1], ylo - 1.0), lo[2]]; t = list(hi)
        k = (t[1] - s[1]) / -L[1]
        for i in (0, 2):
            s[i] += min(k * L[i], 0.0); t[i] += max(k * L[i], 0.0)
        pf = inside and F['fade'] and drawn_dist(F, c, floor_y, s, t, rays=False) > black + MARGIN
        pv = out_of(spls, s, t)
        sh_fade += pf; sh_view += pv; sh_either += pf or pv
        if pf or pv:
            sidx += d['n']
    P(f'Shadow casters: {len(ds)} draws, {len(placeable)} placeable; by distance <{clear:.1f}: {hist[0]}, to {black:.1f}: {hist[1]}, to {black + MARGIN:.1f}: '
      f'{hist[2]}, past: {hist[3]}. Swept along the light down to {ylo - 1:.1f} (the lowest box the eyes see, less 1): past the fade {sh_fade}, '
      f'outside both views (widened {WIDEN}) {sh_view}, either {sh_either} ({sidx / 1000:.0f}k of {sum(d["n"] for d in ds) / 1000:.0f}k indices)')
    P()
    # the trade: smaller black radii
    P('Smaller black radii (the clear radius follows: 60% of it); eye passes = depth + colour full-size draws left out (past r + 1 or outside both '
      'views); square = draws (all eye full-size, casters apart) wholly outside the square of half-size r + 1.5 about the centre')
    P()
    P('| black r | clear | eye passes left out | of which past the fade | casters past the fade (swept) | eye draws outside the square | casters outside the square |')
    P('|---|---|---|---|---|---|---|')
    for r in [black] + [x for x in radii if x < black - 0.05]:
        ex = pf_ = sq = 0
        for name in ('eye depth', 'eye colour'):
            for d in cats[name]:
                if not (d['box'] and not d['box'][0] & 2 and d['inst'] <= 1 and not (d['k'] == 2 and d['bl'])):
                    continue
                lo, hi = padded(d['box'])
                pb = inside and drawn_dist(F, c, floor_y, lo, hi) > r + MARGIN
                ov = out_of(pls, lo, hi)
                ex += pb or ov; pf_ += pb
        for name in ('eye depth', 'eye colour'):
            for d in cats[name]:
                if d['box']:
                    lo, hi = padded(d['box'])
                    sq += max(lo[0] - c[0], c[0] - hi[0], lo[2] - c[1], c[1] - hi[2]) > r + 1.5
        csq = cpf = 0
        for d in placeable:
            lo, hi = padded(d['box'])
            csq += max(lo[0] - c[0], c[0] - hi[0], lo[2] - c[1], c[1] - hi[2]) > r + 1.5
            if L and ylo < 1e29:
                s = [lo[0], min(lo[1], ylo - 1.0), lo[2]]; t = list(hi)
                k = (t[1] - s[1]) / -L[1]
                for i in (0, 2):
                    s[i] += min(k * L[i], 0.0); t[i] += max(k * L[i], 0.0)
                cpf += inside and drawn_dist(F, c, floor_y, s, t, rays=False) > r + MARGIN
        P(f'| {r:.1f} | {clear_of(r):.1f} | {ex} | {pf_} | {cpf} | {sq} | {csq} |')
    P()
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('log')
    ap.add_argument('--frame', type=int, action='append')
    ap.add_argument('--all', action='store_true', help='every logged frame, not the first of each run')
    ap.add_argument('--mirror', action='store_true', help='the fade as drawn with a lower-left origin (rays mirrored vertically)')
    ap.add_argument('--radii', default='20,18,16,14,12', help='black radii below the frame\'s own to count for (the trade)')
    a = ap.parse_args()
    global MIRROR
    MIRROR = a.mirror
    frames = parse(a.log)
    pick = sorted(frames) if a.all else a.frame or [f for f in sorted(frames) if f - 1 not in frames]
    radii = [float(x) for x in a.radii.split(',')]
    for f in pick:
        if f in frames and 'anchor' in frames[f]:
            print(analyse(f, frames[f], radii, True))


if __name__ == '__main__':
    main()

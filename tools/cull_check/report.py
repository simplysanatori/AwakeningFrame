#!/usr/bin/env python3
"""Cull check report: report.py <test dir> <reference dir> <base dir> [--tsv out.tsv]
Per view case and eye: the share of pixels whose floor point lies inside a footprint that are black (void), and the
share that are black here but drawn in the reference run (resident geometry not drawn). Three footprints about the
table's centre (the anchor), along the game camera's ground axes (right u, forward v):
  code  the old fit's floor quadrilateral (the stock screen's trapezoid x 1.2 x 1), from <base dir>'s log
  sym   that quadrilateral's widest half-width and far reach on every side (view-independent)
  table the table camera's footprint (1.5 x the stock screen's widest half-width, 2 x its far reach)
A dump counts only if the cull log has the same case for the 25 frames before it and a fit within 10 frames (a stalled
camera latch stops the log)."""
import glob, math, os, re, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cases as cc

G = 216  # analysis grid


def parse(d):
    geom, eyes, rect, table, vk, pace = {}, {}, {}, {}, [], []
    cur = 0
    for line in open(os.path.join(d, 'game.log'), errors='replace'):
        if line.startswith('[vrcull] f'):
            m = re.match(r'\[vrcull\] f(\d+) (\w+): (.*)', line)
            if not m: continue
            f, kind, rest = int(m[1]), m[2], m[3]
            nums = [float(x) for x in re.findall(r'-?\d+\.?\d*(?:e[-+]?\d+)?', rest)]
            if kind == 'geom': geom[f] = nums; cur = f
            elif kind == 'rect': rect[f] = nums[:8]
            elif kind in ('eye0', 'eye1'): eyes[(f, int(kind[3]))] = nums
            elif kind == 'table': table[f] = nums
        elif line.startswith('[vk] t='):
            m = re.match(r'\[vk\] t=[\d.]+ frame (\d+) .*?\((\d+) draws', line)
            if m: vk.append((int(m[1]), int(m[2])))
        elif line.startswith('[pace] period'):
            m = re.search(r'busy ([\d.]+) .*?cpu ([\d.]+), render cpu ([\d.]+)', line)
            if m: pace.append((cur, float(m[1]), float(m[2]), float(m[3])))
    return dict(geom=geom, eyes=eyes, rect=rect, table=table, vk=vk, pace=pace)


CASES = cc.cases()
REL = [cc.eye_rel(*c) for c in CASES]


def case_at(L, f):
    g, e0, e1 = L['geom'].get(f), L['eyes'].get((f, 0)), L['eyes'].get((f, 1))
    if not (g and e0 and e1): return None
    anchor = g[9:12]
    p0, p1 = e0[0:3], e1[0:3]
    mid = [(p0[i] + p1[i]) / 2 - anchor[i] for i in range(3)]
    best = min(range(len(CASES)), key=lambda k: sum((mid[i] - REL[k][i]) ** 2 for i in range(3)))
    dist = math.sqrt(sum((mid[i] - REL[best][i]) ** 2 for i in range(3)))
    return best if dist < 0.6 else None


def frames_case(L):
    return {f: case_at(L, f) for f in sorted(L['geom'])}


def dump_case(fc, F):
    # the same case from 25 frames before the dump to the first fit after it (within 15 frames): the head was still
    fs = [f for f in fc if F - 25 <= f <= F + 15]
    if len(fs) < 3 or max(fs) <= F or not any(F - 10 <= f <= F for f in fs): return None
    cs = {fc[f] for f in fs}
    return cs.pop() if len(cs) == 1 else None


def uv_axes(g):
    fx, fz = g[3], g[5]
    n = math.hypot(fx, fz)
    v = (fx / n, fz / n)       # forward on the ground, in (x, z)
    u = (-v[1], v[0])          # right
    return u, v


def floor_uv(L, f, eye, flip):
    """per grid pixel: (u, v) of the eye ray's floor point about the anchor, and whether the ray reaches the floor"""
    g, e = L['geom'][f], L['eyes'][(f, eye)]
    pos, vp = np.array(e[0:3]), np.array(e[6:22]).reshape(4, 4)
    inv = np.linalg.inv(vp)
    xs = (np.arange(G) + 0.5) / G * 2 - 1
    nx, ny = np.meshgrid(xs, xs)
    if flip: ny = -ny
    far = np.stack([nx, ny, np.full_like(nx, 0.5), np.ones_like(nx)], -1) @ inv.T
    far = far[..., :3] / far[..., 3:4]
    d = far - pos
    # (the unprojected point may lie behind the eye when w flips: take the direction that agrees with the eye's forward)
    fwd = np.array(e[3:6])
    d = d * np.sign((d * fwd).sum(-1, keepdims=True))
    fy = g[10]
    with np.errstate(divide='ignore', invalid='ignore'):
        t = (fy - pos[1]) / d[..., 1]
    ok = (d[..., 1] < -1e-6) & (t > 0)
    hx, hz = pos[0] + t * d[..., 0] - g[9], pos[2] + t * d[..., 2] - g[11]
    (ux, uz), (vx, vz) = uv_axes(g)
    return hx * ux + hz * uz, hx * vx + hz * vz, ok


def black(path, level=8):
    im = np.asarray(Image.open(path).convert('RGB').resize((G, G), Image.BOX))
    return im.max(-1) < level


def code_quad(B):
    for f in sorted(B['rect']):
        if f < 1300 or f not in B['geom']: continue
        g, q = B['geom'][f], B['rect'][f]
        (ux, uz), (vx, vz) = uv_axes(g)
        out = []
        for i in range(4):
            dx, dz = q[2 * i] - g[9], q[2 * i + 1] - g[11]
            out.append((dx * ux + dz * uz, dx * vx + dz * vz))
        return out
    raise SystemExit('no rect line in the base log')


def in_quad(u, v, quad):
    quad = sorted(quad, key=lambda p: (p[1], p[0]))  # near pair, far pair
    (ua, va), (ub, vb), (uc, vc), (ud, vd) = quad
    vn, vf = (va + vb) / 2, (vc + vd) / 2
    wn, wf = abs(ub - ua) / 2, abs(ud - uc) / 2
    s = np.clip((v - vn) / (vf - vn), 0, 1)
    return (v >= vn) & (v <= vf) & (np.abs(u) <= wn + (wf - wn) * s)


def main():
    T, R, B = sys.argv[1], sys.argv[2], sys.argv[3]
    tsv = sys.argv[sys.argv.index('--tsv') + 1] if '--tsv' in sys.argv else None
    LT, LR, LB = parse(T), parse(R), parse(B)
    quad = code_quad(LB)
    U, V = max(abs(p[0]) for p in quad), max(abs(p[1]) for p in quad)
    TU, TV = 1.5 * U / 1.2, 2.0 * V
    fct, fcr = frames_case(LT), frames_case(LR)
    def dumps(d, fc):
        out = {}
        for p in sorted(glob.glob(os.path.join(d, 'stereo_*_eye0.png'))):
            F = int(re.search(r'stereo_(\d+)_eye0', p)[1])
            c = dump_case(fc, F)
            if c is not None: out.setdefault(c, []).append(F)
        return out
    dt, dr = dumps(T, fct), dumps(R, fcr)
    # image orientation: the one that puts more drawn pixels inside the table footprint
    score = [0, 0]
    for c in list(dt)[:8]:
        F = dt[c][0]
        f = max(x for x in fct if x <= F)
        b = black(os.path.join(T, 'stereo_%05d_eye0.png' % F))
        for flip in (0, 1):
            u, v, ok = floor_uv(LT, f, 0, flip)
            ins = ok & (np.abs(u) <= U) & (np.abs(v) <= V)
            score[flip] += (~b & ins).sum() - (b & ins).sum()
    flip = int(score[1] > score[0])
    # draws and game CPU per case: [vk] intervals and [pace] lines that lie inside one case
    draws, cpu = {}, {}
    prev = None
    for f, d in LT['vk']:
        if prev is not None and f > prev:
            fs = [x for x in fct if prev < x <= f]
            cs = {fct[x] for x in fs}
            if fs and len(cs) == 1 and None not in cs: draws.setdefault(cs.pop(), []).append(d / (f - prev))
        prev = f
    for f, busy, c, rc in LT['pace']:
        k = fct.get(f)
        if k is not None: cpu.setdefault(k, []).append((busy, c, rc))
    rows = []
    for k, case in enumerate(CASES):
        row = dict(yaw=case[0], tilt=case[1], head=case[2], valid=k in dt and k in dr)
        if k in draws: row['draws'] = float(np.median(draws[k]))
        if k in cpu:
            a = np.array(cpu[k]); row['busy'], row['cpu'], row['rcpu'] = [float(x) for x in np.median(a, 0)]
        if row['valid']:
            F, FR = dt[k][-1], dr[k][-1]
            f = max(x for x in fct if x <= F)
            fr = max(x for x in fcr if x <= FR)
            da = max(abs(LT['geom'][f][9 + i] - LR['geom'][fr][9 + i]) for i in range(3))
            if da > 1.0:  # (Link stands elsewhere in the reference run: the pictures can't be compared)
                row['valid'] = False; row['why'] = 'the anchor differs by %.1f units from the reference run' % da
                rows.append(row); continue
            row['frame'], row['ref_frame'] = F, FR
            if f in LT['table']: row['culled'] = LT['table'][f][2:6]
            for eye in (0, 1):
                b = black(os.path.join(T, 'stereo_%05d_eye%d.png' % (F, eye)))
                br = black(os.path.join(R, 'stereo_%05d_eye%d.png' % (FR, eye)), 24)  # (drawn in the reference: clearly not black)
                u, v, ok = floor_uv(LT, f, eye, flip)
                for name, ins in (('code', ok & in_quad(u, v, quad)), ('sym', ok & (np.abs(u) <= U) & (np.abs(v) <= V)),
                                  ('table', ok & (np.abs(u) <= TU) & (np.abs(v) <= TV))):
                    n = int(ins.sum())
                    row['%s_n%d' % (name, eye)] = n
                    row['%s_void%d' % (name, eye)] = 100.0 * (b & ins).sum() / n if n else float('nan')
                    row['%s_miss%d' % (name, eye)] = 100.0 * (b & ~br & ins).sum() / n if n else float('nan')
                row['void%d' % eye] = 100.0 * b.mean()
        rows.append(row)
    print('# test %s  ref %s  flip %d' % (T, R, flip))
    print('# footprints about the anchor (u right, v forward): code quad %s; sym +-%.2f x +-%.2f; table +-%.2f x +-%.2f'
          % (' '.join('(%.1f,%.1f)' % p for p in quad), U, V, TU, TV))
    hdr = 'yaw tilt head frame draws cpu | missing %% of footprint pixels, worse eye: code sym table | void %%: code sym table | eye void'
    print(hdr)
    for r in rows:
        if not r['valid']:
            print('%3d %2d %s INVALID (%s)' % (r['yaw'], r['tilt'], r['head'], r.get('why', 'no settled dump in the test or the reference'))); continue
        w = lambda key: max(r[key % 0], r[key % 1])
        print('%3d %2d %s %5d %6.0f %5.2f | %5.1f %5.1f %5.1f | %5.1f %5.1f %5.1f | %4.1f' % (
            r['yaw'], r['tilt'], r['head'], r['frame'], r.get('draws', float('nan')), r.get('cpu', float('nan')),
            w('code_miss%d'), w('sym_miss%d'), w('table_miss%d'), w('code_void%d'), w('sym_void%d'), w('table_void%d'), w('void%d')))
    if tsv:
        keys = sorted({k for r in rows for k in r})
        with open(tsv, 'w') as fh:
            fh.write('\t'.join(keys) + '\n')
            for r in rows: fh.write('\t'.join(str(r.get(k, '')) for k in keys) + '\n')


if __name__ == "__main__":
    main()

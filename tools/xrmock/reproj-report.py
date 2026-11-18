#!/usr/bin/env python3
# What each refresh shows against the head at its display time, measured from the pixels (xrmock MOCK_XR_SHIFT with
# MOCK_XR_SHIFT_DUMP; docs/af/STEREO.md "Re-shown refreshes"). Eye 0 only.
#
# The images of the set's own swapchains (made before the session began) are taken as rendered for the pose their layer
# claims (STEREO.md "Frame end": 0.001 degrees rms). Every other image the layers name (a set shown again, an image drawn
# for a later refresh) is registered against the latest of those before it (its set's own), and its content is measured, not claimed:
#   --motion turn   the rotation that maps it onto that image (a pure rotation is the same at every depth), so its
#                   content's orientation; per refresh against the head's true eye 0 at the display time ("pinned": what
#                   a compositor shows that puts the layer up as it is) and against the layer's own pose ("reprojected":
#                   what is left once a compositor corrects by the layer's pose)
#   --motion slide  the shift of a centre patch (the near table) against the set's own image, against the shift the
#                   head's true position at the display time calls for: interpolated between the shifts of consecutive
#                   rendered images (rendered images only, no depth or claim of the image under test is used)
#   --motion follow as slide, by display time instead of the head's position: a still head while the table follows
#                   Link (the world slides at a steady rate between game frames)
# A registration check, the same measure between consecutive rendered images against their claimed poses, gives the
# floor of the measurement. Each refresh is also sorted by the display time its layer's pose was located for (the mock
# matches it): this refresh's ("on time": within half a period), or another's (a set shown again, or a late set).
# Usage: reproj-report.py shift.txt dumpdir --motion turn|slide|follow [--from S] [--to S] [--lines]
import sys, math
import numpy as np

def kv(line):
    d = {}
    for tok in line.split()[1:]:
        if '=' in tok:
            k, v = tok.split('=', 1)
            try: d[k] = float(v)
            except ValueError: d[k] = v
    return d

a = sys.argv[1:]
path, ddir = a[0], a[1]
motion = a[a.index('--motion') + 1] if '--motion' in a else 'turn'
t_from = float(a[a.index('--from') + 1]) if '--from' in a else 0.0
t_to = float(a[a.index('--to') + 1]) if '--to' in a else 1e9

chains, imgs, disps, dumps = {}, {}, [], {}
for line in open(path):
    if line.startswith('chain '):
        d = kv(line); chains[int(d['sc'])] = d
    elif line.startswith('img '):
        d = kv(line)
        if int(d['eye']) == 0 and 'lqw' in d: imgs[(int(d['sc']), int(d['rel']))] = d
    elif line.startswith('disp '):
        d = kv(line)
        if 'sc0' in d and t_from <= d['t'] <= t_to: disps.append(d)
    elif line.startswith('dump '):
        d = kv(line)
        if int(d['eye']) == 0: dumps[(int(d['sc']), int(d['rel']))] = '%s/f%05d_e0.pgm' % (ddir, int(d['frame']))
own = {sc for sc, c in chains.items() if c['t'] < 0}

def pgm(p):
    b = open(p, 'rb').read()
    parts = b.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return np.frombuffer(parts[3][:w * h], np.uint8).reshape(h, w).astype(np.float32)

def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
def rotvec(R):  # the rotation's axis * angle (radians)
    c = max(-1.0, min(1.0, (np.trace(R) - 1) / 2)); ang = math.acos(c)
    v = np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]])
    n = np.linalg.norm(v)
    return v / n * ang if n > 1e-12 else np.zeros(3)
def expm(w):
    th = np.linalg.norm(w)
    if th < 1e-12: return np.eye(3)
    k = w / th; K = np.array([[0, -k[2], k[1]], [k[2], 0, -k[0]], [-k[1], k[0], 0]])
    return np.eye(3) + math.sin(th) * K + (1 - math.cos(th)) * K @ K
def claimR(d): return qmat((d['lqx'], d['lqy'], d['lqz'], d['lqw']))
def deg(x): return x * 180 / math.pi

def blur(im, s):
    if s <= 0: return im
    r = int(3 * s); k = np.exp(-0.5 * (np.arange(-r, r + 1) / s) ** 2); k /= k.sum()
    im = np.apply_along_axis(lambda v: np.convolve(np.pad(v, r, mode='edge'), k, 'valid'), 1, im)
    return np.apply_along_axis(lambda v: np.convolve(np.pad(v, r, mode='edge'), k, 'valid'), 0, im)
def bilinear(im, u, v):
    h, w = im.shape
    ok = (u >= 0) & (v >= 0) & (u <= w - 1.001) & (v <= h - 1.001)
    u = np.clip(u, 0, w - 1.001); v = np.clip(v, 0, h - 1.001)
    i, j = np.floor(u).astype(int), np.floor(v).astype(int); fu, fv = u - i, v - j
    r = im[j, i] * (1 - fu) * (1 - fv) + im[j, i + 1] * fu * (1 - fv) + im[j + 1, i] * (1 - fu) * fv + im[j + 1, i + 1] * fu * fv
    return r, ok

class Geo:  # a dump pixel <-> its ray in the eye's frame (the probe: the centre 2/3, 2x2 boxes)
    def __init__(s, d, shape):
        s.W, s.H = d['w'], d['h']; s.tl, s.tr, s.tu, s.td = (math.tan(d[k]) for k in ('fl', 'fr', 'fu', 'fd'))
        s.x0, s.y0 = (s.W - s.W * 2 // 3) // 2, (s.H - s.H * 2 // 3) // 2
        s.h, s.w = shape
    def rays(s, u, v):
        X, Y = s.x0 + 2 * u + 1, s.y0 + 2 * v + 1
        return np.stack([s.tl + X / s.W * (s.tr - s.tl), s.tu - Y / s.H * (s.tu - s.td), -np.ones_like(u)])
    def pix(s, d):
        tx, ty = d[0] / -d[2], d[1] / -d[2]
        return ((tx - s.tl) / (s.tr - s.tl) * s.W - s.x0 - 1) / 2, ((s.tu - ty) / (s.tu - s.td) * s.H - s.y0 - 1) / 2
    def fpx(s): return s.W / (s.tr - s.tl)  # pixels a radian at the centre

cache = {}
def image(key):
    if key not in cache: cache[key] = pgm(dumps[key])
    return cache[key]

def register_rot(A, B, g, R0):
    # the rotation D (rays of B's frame into A's) minimising |A(pix(D ray_B)) - B|, Gauss-Newton, from R0
    h, w = B.shape
    vv, uu = np.mgrid[h // 8:h - h // 8:2, w // 8:w - w // 8:2].astype(np.float64)
    rb = g.rays(uu.ravel(), vv.ravel())
    D = R0.copy()
    for s in (3.0, 1.0, 0.0):
        As, Bs = blur(A, s), blur(B, s)
        b, _ = bilinear(Bs, uu.ravel(), vv.ravel())
        for _ in range(6):
            r0, ok = bilinear(As, *g.pix(D @ rb))
            J = []
            for k in range(3):
                e = np.zeros(3); e[k] = 2e-4
                rk, _ = bilinear(As, *g.pix(expm(e) @ D @ rb))
                J.append((rk - r0) / 2e-4)
            J = np.stack(J, 1)
            res = r0 - b
            m = ok & (np.abs(res) < np.percentile(np.abs(res[ok]), 90) + 1e-6)  # (game animation: the worst tenth left out)
            dx = np.linalg.lstsq(J[m], -res[m], rcond=None)[0]
            D = expm(dx) @ D
            if np.linalg.norm(dx) < 1e-6: break
    return D

def shift_patch(A, B):  # the shift (px of the dump, x and y) of B's centre patch against A's, phase correlation
    h, w = A.shape
    a = A[h // 4:3 * h // 4, w // 4:3 * w // 4]; b = B[h // 4:3 * h // 4, w // 4:3 * w // 4]
    win = np.outer(np.hanning(a.shape[0]), np.hanning(a.shape[1]))
    Fa, Fb = np.fft.fft2((a - a.mean()) * win), np.fft.fft2((b - b.mean()) * win)
    c = np.fft.ifft2(Fb * np.conj(Fa) / (np.abs(Fb * np.conj(Fa)) + 1e-9)).real
    j, i = np.unravel_index(np.argmax(c), c.shape)
    def sub(m, l, r): d = l - 2 * m + r; return 0.5 * (l - r) / d if d < 0 else 0.0
    dy = j + sub(c[j, i], c[j - 1, i], c[(j + 1) % c.shape[0], i]); dx = i + sub(c[j, i], c[j, i - 1], c[j, (i + 1) % c.shape[1]])
    if dy > c.shape[0] / 2: dy -= c.shape[0]
    if dx > c.shape[1] / 2: dx -= c.shape[1]
    return dx, dy

def stats(v):
    if not v: return 'n/a'
    v2 = sorted(abs(x) for x in v); rms = math.sqrt(sum(x * x for x in v) / len(v))
    return 'rms %.3f p95 %.3f max %.3f, %d of %d within 0.1' % (rms, v2[int(0.95 * (len(v2) - 1))], v2[-1], sum(x <= 0.1 for x in v2), len(v))

shown = [k for k in dict.fromkeys((int(d['sc0']), int(d['rel0'])) for d in disps) if k in dumps and k in imgs]
mains = sorted([k for k in shown if k[0] in own], key=lambda k: imgs[k]['t'])
if len(mains) < 3:
    print('%s: too few dumped images of the set\'s own swapchains in %.1f-%.1f s' % (path, t_from, t_to)); sys.exit(1)
mt = np.array([imgs[k]['t'] for k in mains])
g = Geo(imgs[mains[0]], image(mains[0]).shape)
print('%s: %d refreshes, %d images shown (%d the sets\' own), %.1f-%.1f s, motion %s' % (path, len(disps), len(shown), len(mains),
      t_from, min(t_to, 9999), motion))

def nearest_main(t, before=False):
    i = int(np.searchsorted(mt, t + 1e-6)) - 1
    if before: return max(i, 0)
    j = min(max(i, 0) + 1, len(mains) - 1)
    return i if i >= 0 and (abs(mt[i] - t) <= abs(mt[j] - t)) else j

lines = '--lines' in a
half = 0.5 * float(np.median(np.diff([d['t'] for d in disps]))) if len(disps) > 2 else 0.0042
def on_time(d): return d.get('loc_ms', -1) >= 0 and abs(d['t'] - d['loc_ms'] / 1e3) < half
def split(rows):  # rows: (refresh, error); all of them, those on time and the others
    return ('all %s; on time %s; for another refresh %s' % (stats([e for _, e in rows]), stats([e for d, e in rows if on_time(d)]),
            stats([e for d, e in rows if not on_time(d)])))
if motion == 'turn':
    # the measurement's floor: consecutive rendered images registered against each other, against their claims
    chk = []
    for i in range(1, min(len(mains), 60)):
        A, B = mains[i - 1], mains[i]
        R0 = claimR(imgs[A]).T @ claimR(imgs[B])
        D = register_rot(image(A), image(B), g, R0)
        chk.append(deg(np.linalg.norm(rotvec(R0.T @ D))))
    print('registration check (consecutive rendered images against their claims): %s deg' % stats(chk))
    content = {}
    for k in shown:
        if k[0] in own: content[k] = claimR(imgs[k]); continue
        A = mains[nearest_main(imgs[k]['t'], before=True)]  # (its set's own: the next set can be another scene)
        R0 = claimR(imgs[A]).T @ claimR(imgs[k])
        content[k] = claimR(imgs[A]) @ register_rot(image(A), image(k), g, R0)
    first, again = {'pin': [], 'rep': []}, {'pin': [], 'rep': []}
    rows = []
    seen = set()
    for d in disps:
        k = (int(d['sc0']), int(d['rel0']))
        if k not in content: continue
        T = qmat((d['tqx'], d['tqy'], d['tqz'], d['tqw']))
        pin = deg(np.linalg.norm(rotvec(T.T @ content[k])))
        rep = deg(np.linalg.norm(rotvec(claimR(imgs[k]).T @ content[k])))
        grp = first if (k[0] in own and k not in seen) else again
        seen.add(k)
        grp['pin'].append(pin); grp['rep'].append(rep); rows.append((d, pin))
        if lines: print('  t=%.4f sc%d rel%d %s pinned %.3f reprojected %.3f%s' % (d['t'], k[0], k[1], 'first' if grp is first else 'again', pin, rep,
                                                                                  '' if on_time(d) else ' (pose for another refresh)'))
    print('a set\'s first refresh: content against the head at display (pinned) %s deg; against its layer pose %s deg' % (stats(first['pin']), stats(first['rep'])))
    print('other refreshes:        content against the head at display (pinned) %s deg; against its layer pose %s deg' % (stats(again['pin']), stats(again['rep'])))
    print('every refresh, pinned, by its pose\'s display time: %s deg' % split(rows))
else:
    fpx = g.fpx() / 2  # (dump pixels a radian)
    if motion == 'follow': pos = lambda d: np.array([d['t'], 0.0, 0.0])
    else: pos = lambda d: np.array([d['lpx'], d['lpy'], d['lpz']])
    # shift of each rendered image against the one before
    step = [None] + [shift_patch(image(mains[i - 1]), image(mains[i])) for i in range(1, len(mains))]
    chk = []
    for i in range(2, len(mains)):  # (floor: a rendered image predicted from its neighbours' shifts, by position)
        p0, p1, p2 = pos(imgs[mains[i - 2]]), pos(imgs[mains[i - 1]]), pos(imgs[mains[i]])
        if np.linalg.norm(p1 - p0) < 1e-6: continue
        lam = np.dot(p2 - p1, p1 - p0) / np.dot(p1 - p0, p1 - p0)
        chk.append(deg((step[i][0] - lam * step[i - 1][0]) / fpx))
    print('measurement check (a rendered image\'s patch shift against its neighbours\', by position): %s deg' % stats(chk))
    first, again = [], []
    rows = []
    seen = set()
    for d in disps:
        k = (int(d['sc0']), int(d['rel0']))
        if k not in imgs or k not in dumps: continue
        i = nearest_main(d['t'], before=True)
        if i + 1 >= len(mains): continue
        A, Bn = mains[i], mains[i + 1]
        pa, pb = pos(imgs[A]), pos(imgs[Bn])
        pt = np.array([d['t'], 0.0, 0.0]) if motion == 'follow' else np.array([d['tpx'], d['tpy'], d['tpz']])
        if np.linalg.norm(pb - pa) < 1e-6: continue
        lam = np.dot(pt - pa, pb - pa) / np.dot(pb - pa, pb - pa)
        want = lam * step[i + 1][0]
        got = shift_patch(image(A), image(k))[0] if k != A else 0.0
        err = deg((got - want) / fpx)
        grp = first if (k[0] in own and k not in seen) else again
        seen.add(k)
        grp.append(err); rows.append((d, err))
        if lines: print('  t=%.4f sc%d rel%d %s want %+.2f got %+.2f px err %+.3f deg%s' % (d['t'], k[0], k[1], 'first' if grp is first else 'again', want, got, err,
                                                                                       '' if on_time(d) else ' (pose for another refresh)'))
    print('a set\'s first refresh: near-geometry shift against the head at display %s deg' % stats(first))
    print('other refreshes:        near-geometry shift against the head at display %s deg' % stats(again))
    print('every refresh, by its pose\'s display time: %s deg' % split(rows))

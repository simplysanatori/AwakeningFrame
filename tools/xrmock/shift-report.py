#!/usr/bin/env python3
# Reads xrmock's MOCK_XR_SHIFT file and reports, per eye, how far the eye images' content is from the pose their
# projection layer claims (the error a reprojecting compositor cannot see: it shows up as the image jumping when the
# head moves), and per refresh how far the layer's pose is from the head's at the display time (what the compositor
# has to correct). Usage: shift-report.py shift.txt [--axis pitch|yaw] [--from S] [--to S] [--q MIN_CORRELATION] [--lines]
import sys, math

def kv(line):
    d = {}
    for tok in line.split()[1:]:
        if '=' in tok:
            k, v = tok.split('=', 1)
            try: d[k] = float(v)
            except ValueError: d[k] = v
    return d

args = sys.argv[1:]
path = args[0]
axis = args[args.index('--axis') + 1] if '--axis' in args else 'pitch'
t_from = float(args[args.index('--from') + 1]) if '--from' in args else 0.0
t_to = float(args[args.index('--to') + 1]) if '--to' in args else 1e9
QMIN = float(args[args.index('--q') + 1]) if '--q' in args else 0.3
imgs, disps = [], []
for line in open(path):
    if line.startswith('img '): imgs.append(kv(line))
    elif line.startswith('disp '): disps.append(kv(line))
imgs = [d for d in imgs if t_from <= d['t'] <= t_to]
disps = [d for d in disps if t_from <= d['t'] <= t_to]

def stats(v):
    if not v: return 'n/a'
    v2 = sorted(abs(x) for x in v)
    rms = math.sqrt(sum(x * x for x in v) / len(v))
    return 'rms %.3f p95 %.3f max %.3f deg (n %d)' % (rms, v2[int(0.95 * (len(v2) - 1))], v2[-1], len(v))

print('%s: %d eye images, %d refreshes, %.1f-%.1f s, axis %s' % (path, len(imgs), len(disps), t_from, min(t_to, 9999), axis))
for eye in (0, 1):
    e = [d for d in imgs if int(d['eye']) == eye and d['qy' if axis == 'pitch' else 'qx'] > QMIN]
    if len(e) < 3:
        print('eye %d: too few images' % eye); continue
    e.sort(key=lambda d: d['frame'])
    x = [d['layer_d' + axis] for d in e]
    y = [d['img_d' + axis] for d in e]
    n = len(x); mx = sum(x) / n; my = sum(y) / n
    sxx = sum((a - mx) ** 2 for a in x); sxy = sum((a - mx) * (b - my) for a, b in zip(x, y))
    a = sxy / sxx if sxx > 0 else 0.0
    b = my - a * mx
    # the content against its own layer's pose, and against the previous / next image's layer pose (a lag of a set)
    res = [yy - (a * xx + b) for xx, yy in zip(x, y)]
    raw = [yy - xx for xx, yy in zip(x, y)]
    prev = [y[i] - (a * x[i - 1] + b) for i in range(1, n)]
    nxt = [y[i] - (a * x[i + 1] + b) for i in range(n - 1)]
    q = sorted(d['qy' if axis == 'pitch' else 'qx'] for d in e)
    print('eye %d: content = %.3f x layer %+.3f deg; content - layer: %s' % (eye, a, b, stats(res)))
    print('       (slope 1, no offset: %s)' % stats(raw))
    print('       against the previous image\'s pose: %s; the next\'s: %s; peak correlation median %.2f' % (stats(prev), stats(nxt), q[len(q) // 2]))
    if '--lines' in args:
        for d, r in zip(e, res):
            print('   f%d rel%d t=%.3f layer %+.3f content %+.3f err %+.3f' % (d['frame'], d['rel'], d['t'], d['layer_d' + axis], d['img_d' + axis], r))
# both eyes of a set: content difference between the two eyes against their layers' difference
by = {}
for d in imgs:
    by.setdefault((int(d['frame'])), {})[int(d['eye'])] = d
pair = [(v[0], v[1]) for v in by.values() if 0 in v and 1 in v]
if pair:
    k = 'd' + axis
    dd = [(r['img_' + k] - l['img_' + k]) - (r['layer_' + k] - l['layer_' + k]) for l, r in pair]
    print('eyes of one set: (right - left content) - (right - left layer): %s' % stats(dd))
if disps:
    new = [d for i, d in enumerate(disps) if i and (d['rel0'] != disps[i - 1]['rel0'])]
    old = [d for i, d in enumerate(disps) if i and (d['rel0'] == disps[i - 1]['rel0'])]
    print('refreshes: layer pose vs the head at display: new image %s; image shown again %s' % (stats([d['rot_err'] for d in new]), stats([d['rot_err'] for d in old])))
    ages = [d['t'] * 1000 - d['loc_ms'] for d in disps if d['loc_ms'] > 0]
    if ages:
        ages.sort()
        print('poses located for: display time - locate time avg %.2f ms, min %.2f, max %.2f' % (sum(ages) / len(ages), ages[0], ages[-1]))
    pos = [d['pos_err_mm'] for d in disps]
    print('layer position vs the head at display: avg %.2f mm, max %.2f' % (sum(pos) / len(pos), max(pos)))

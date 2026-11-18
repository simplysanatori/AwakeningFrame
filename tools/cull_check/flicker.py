#!/usr/bin/env python3
"""flicker.py <session dir> ...: a moving head (cull log every 2 frames, eye dumps at consecutive frames).
1. From the log: how far each edge of what the culling takes in on the floor moves between two fits
   (the table camera's rectangle from the 'table:' lines, or the game camera's frustum floor from the 'drawn:' lines).
2. From the dumps: floor cells (1 unit, inside the table's footprint, by each pixel's floor point) that are seen drawn in
   some dumps and seen black in others: geometry that came and went with the head."""
import glob, os, re, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report as cr

for d in sys.argv[1:]:
    L = cr.parse(d)
    fs = sorted(f for f in L['geom'] if f >= 1500)
    rows = []
    for f in fs:
        g = L['geom'][f]
        (ux, uz), (vx, vz) = cr.uv_axes(g)
        if f in L['table']:
            rows.append(L['table'][f][2:6])                      # right lo, hi; forward lo, hi (about the anchor)
        else:
            q = None
    kind = 'table rectangle (right lo/hi, forward lo/hi)'
    if not rows:  # the game camera's frustum: its floor quad about the anchor, as the near / far reach and the far half-width
        kind = "game camera's frustum floor (south reach, north reach, half-width at the north edge)"
        for line in open(os.path.join(d, 'game.log'), errors='replace'):
            m = re.match(r'\[vrcull\] f(\d+) drawn: (.*)', line)
            if m and int(m[1]) in L['geom'] and int(m[1]) >= 1500:
                q = [float(x) for x in m[2].split()]
                g = L['geom'][int(m[1])]
                zs = sorted(set(round(q[i], 3) for i in (1, 3, 5, 7)))
                xs_far = [q[i] for i in (0, 2, 4, 6) if abs(q[i + 1] - zs[0]) < 0.01]
                rows.append([zs[-1] - g[11], g[11] - zs[0], (max(xs_far) - min(xs_far)) / 2])
    a = np.array(rows)
    step = np.abs(np.diff(a, axis=0))
    print('%s: %d fits; %s' % (os.path.basename(d.rstrip('/')), len(a), kind))
    print('   range over the run: %s' % '  '.join('%.2f..%.2f' % (a[:, i].min(), a[:, i].max()) for i in range(a.shape[1])))
    print('   largest step between two fits (2 frames): %s units; 99th percentile %s' % (
        '  '.join('%.2f' % step[:, i].max() for i in range(a.shape[1])), '  '.join('%.2f' % np.percentile(step[:, i], 99) for i in range(a.shape[1]))))
    # the dumps
    dumps = sorted(int(re.search(r'stereo_(\d+)_eye0', p)[1]) for p in glob.glob(os.path.join(d, 'stereo_*_eye0.png')))
    N = 26
    seen_d = np.zeros((2 * N, 2 * N), int); seen_b = np.zeros((2 * N, 2 * N), int)
    used = 0
    for F in dumps:
        f = max((x for x in L['geom'] if x <= F), default=None)
        if f is None or F - f > 4 or (f, 0) not in L['eyes']: continue
        used += 1
        b = cr.black(os.path.join(d, 'stereo_%05d_eye0.png' % F))
        u, v, ok = cr.floor_uv(L, f, 0, 1)
        iu, iv = np.floor(u).astype(int) + N, np.floor(v).astype(int) + N
        ok = ok & (iu >= 0) & (iu < 2 * N) & (iv >= 0) & (iv < 2 * N)
        cb = np.zeros((2 * N, 2 * N), int); cd = np.zeros((2 * N, 2 * N), int)
        np.add.at(cb, (iu[ok & b], iv[ok & b]), 1); np.add.at(cd, (iu[ok & ~b], iv[ok & ~b]), 1)
        tot = cb + cd
        seen_b += (tot >= 4) & (cb > 0.8 * tot); seen_d += (tot >= 4) & (cd > 0.8 * tot)
    uu, vv = np.meshgrid(np.arange(-N, N) + 0.5, np.arange(-N, N) + 0.5, indexing='ij')
    foot = (np.abs(uu) <= 22.27) & (np.abs(vv) <= 23.62)
    flick = (seen_b >= 2) & (seen_d >= 2) & foot
    print('   %d dumps: %d floor cells of the footprint seen in two or more; %d of them drawn in two or more dumps and black in two or more'
          % (used, int((((seen_b + seen_d) >= 2) & foot).sum()), int(flick.sum())))
    if flick.any():
        idx = np.argwhere(flick)
        print('   those cells lie at right %.0f..%.0f, forward %.0f..%.0f about the table centre' % (
            idx[:, 0].min() - N, idx[:, 0].max() - N + 1, idx[:, 1].min() - N, idx[:, 1].max() - N + 1))

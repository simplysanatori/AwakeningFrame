#!/usr/bin/env python3
"""cost.py <session dir> ...: per spot window of cost.sh sessions, draws per game frame ([vk] lines) and the
game thread's busy / cpu and the render thread's cpu ([pace] lines, medians), one row per session; then per group
(the directory name without its trailing _<n>) the mean and the spread over its sessions."""
import os, re, sys
import numpy as np
SPOTS = (('Mabe 11C', 1400, 2400), ('woods edge 09C', 2900, 3900), ('forest 06C', 4400, 5400))


def session(d):
    vk, pace, cur = [], [], 0
    for line in open(os.path.join(d, 'game.log'), errors='replace'):
        m = re.match(r'\[vk\] t=[\d.]+ frame (\d+) ([\d.]+) fps.*?\((\d+) draws', line)
        if m:
            vk.append((int(m[1]), int(m[3]), float(m[2]))); cur = int(m[1]); continue
        if line.startswith('[pace] period'):
            m = re.search(r'busy ([\d.]+) .*?cpu ([\d.]+), render cpu ([\d.]+)', line)
            if m: pace.append((cur, float(m[1]), float(m[2]), float(m[3])))
    out = []
    for name, f0, f1 in SPOTS:
        dr = [(vk[i][1] / (vk[i][0] - vk[i - 1][0])) for i in range(1, len(vk)) if f0 <= vk[i - 1][0] and vk[i][0] <= f1 and vk[i][0] > vk[i - 1][0]]
        fps = [v[2] for v in vk if f0 < v[0] <= f1]
        p = np.array([x[1:] for x in pace if f0 <= x[0] < f1 - 120])
        out.append((np.median(dr) if dr else float('nan'), *(np.median(p, 0) if len(p) else [float('nan')] * 3), np.median(fps) if fps else float('nan')))
    return out


groups = {}
print('%-28s %-16s %8s %6s %6s %6s %5s' % ('session', 'spot', 'draws', 'busy', 'cpu', 'rcpu', 'fps'))
for d in sys.argv[1:]:
    s = session(d)
    g = re.sub(r'_\d+$', '', os.path.basename(d.rstrip('/')))
    groups.setdefault(g, []).append(s)
    for (name, _, _), r in zip(SPOTS, s):
        print('%-28s %-16s %8.1f %6.2f %6.2f %6.2f %5.1f' % (os.path.basename(d.rstrip('/')), name, *r))
print()
print('%-22s %-16s %-22s %-18s %-18s %-18s' % ('group (n)', 'spot', 'draws mean (min-max)', 'busy ms', 'cpu ms', 'render cpu ms'))
for g, ss in groups.items():
    a = np.array(ss)  # sessions x spots x 5
    for i, (name, _, _) in enumerate(SPOTS):
        f = lambda k: '%.2f (%.2f-%.2f)' % (a[:, i, k].mean(), a[:, i, k].min(), a[:, i, k].max())
        print('%-22s %-16s %-22s %-18s %-18s %-18s' % ('%s (%d)' % (g, len(ss)), name, '%.0f (%.0f-%.0f)' % (a[:, i, 0].mean(), a[:, i, 0].min(), a[:, i, 0].max()), f(1), f(2), f(3)))

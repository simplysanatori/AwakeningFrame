#!/usr/bin/env python3
"""views_cost.py <base session> <new session>: sessions that walk the view cases with no dumps (the list several
times). Per case: draws per game frame ([vk] intervals that lie inside one hold of the case) and the game thread's cpu
([pace] lines), each hold a sample; mean and range over the holds, and the change."""
import os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report as cr


def stats(d):
    L = cr.parse(d); fc = cr.frames_case(L)
    draws, cpu = {}, {}
    prev = None
    for f, n in L['vk']:
        if prev is not None and f > prev:
            fs = [x for x in fc if prev < x <= f]
            cs = {fc[x] for x in fs}
            if len(fs) >= 6 and len(cs) == 1 and None not in cs: draws.setdefault(cs.pop(), []).append(n / (f - prev))
        prev = f
    # cpu: one sample per run of consecutive frames in a case
    run, last = [], None
    for f, busy, c, rc in L['pace']:
        k = fc.get(f)
        if k != last:
            if last is not None and len(run) > 60: cpu.setdefault(last, []).append(float(np.median(run)))
            run, last = [], k
        if k is not None: run.append(c)
    if last is not None and len(run) > 60: cpu.setdefault(last, []).append(float(np.median(run)))
    return draws, cpu


(db, cb), (dn, cn) = stats(sys.argv[1]), stats(sys.argv[2])
print('yaw tilt head | draws before (min-max, n) | after | change | game cpu ms before | after')
worst = []
for k, case in enumerate(cr.CASES):
    if k not in db or k not in dn:
        print('%3d %2d %s | no sample' % case); continue
    b, n = np.array(db[k]), np.array(dn[k])
    ch = 100 * (n.mean() / b.mean() - 1)
    worst.append((ch, case))
    f = lambda a: '%4.0f (%.0f-%.0f, %d)' % (a.mean(), a.min(), a.max(), len(a))
    g = lambda c: '%.2f (%.2f-%.2f)' % (np.mean(c[k]), min(c[k]), max(c[k])) if k in c else '-'
    print('%3d %2d %s | %s | %s | %+5.1f%% | %s | %s' % (*case, f(b), f(n), ch, g(cb), g(cn)))
worst.sort()
print('change in draws: least %+.1f%% (%s), most %+.1f%% (%s); %d of %d views over +25%%, %d over +10%%' % (
    worst[0][0], worst[0][1], worst[-1][0], worst[-1][1], sum(1 for w in worst if w[0] > 25), len(worst), sum(1 for w in worst if w[0] > 10)))

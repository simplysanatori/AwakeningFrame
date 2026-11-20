#!/usr/bin/env python3
# Which refreshes show a set and which are dark, from xrmock's MOCK_XR_SHIFT file (docs/af/STEREO.md "Black Frame
# Insertion").
# Per ended frame the mock writes an "end" line (its projection and quad layer counts); a frame with a projection layer
# also has a "disp" line (the image each eye shows: its swapchain and release; the display time its poses were located
# for; the layer's eye 0 pose against the head's true eye 0 at the display time). Reported over a window of display times:
#   - refreshes lit (a projection layer) and dark (none), and the quad layers (HUD, laser) on each kind
#   - per set (eye 0's swapchain and release): on how many refreshes it is lit; lit refreshes in a row; the same image on
#     two lit refreshes in a row
#   - the gaps between flashes (refreshes from one lit refresh to the next) and the lit refreshes' phase (their refresh
#     number's parity) and how often it changes
#   - each lit refresh against the display time its poses were located for: on it, or 1, 2, 3+ refreshes after it
#   - the layer's rotation error against the head (rot_err, degrees): on the lit refreshes, and from one flash to the next
#     (doubling.py's measure: what a compositor that shows a layer as it is turns into a double image)
# Without the option every refresh is lit: the same report is the base to compare with.
# Usage: bfi-report.py shift.txt [--from S] [--to S] [--lines]
import sys, math, collections

def kv(line):
    d = {}
    for tok in line.split()[1:]:
        if '=' in tok:
            k, v = tok.split('=', 1)
            try: d[k] = float(v)
            except ValueError: d[k] = v
    return d

a = sys.argv[1:]
path = a[0]
t_from = float(a[a.index('--from') + 1]) if '--from' in a else 0.0
t_to = float(a[a.index('--to') + 1]) if '--to' in a else 1e9
lines = '--lines' in a

ends, disps = [], {}
for line in open(path):  # (a line cut short where the app exited is skipped)
    if line.startswith('end '):
        d = kv(line)
        if {'frame', 't', 'proj', 'quads'} <= d.keys() and t_from <= d['t'] <= t_to: ends.append(d)
    elif line.startswith('disp '):
        d = kv(line)
        if {'frame', 't', 'sc0', 'rel0', 'rot_err', 'true_pitch'} <= d.keys(): disps[int(d['frame'])] = d
if len(ends) < 3:
    sys.exit('no "end" lines in the window (an xrmock without them, or MOCK_XR_SHIFT unset)')

# (the display times are written to 0.1 ms: the period is the mean of the single steps, and each refresh's number is
# counted step by step, so the rounding never drifts over a run)
dts = sorted(b['t'] - a_['t'] for a_, b in zip(ends, ends[1:]))
med = dts[len(dts) // 2]
one = [x for x in dts if x < 1.5 * med]
per = sum(one) / len(one)
t0 = ends[0]['t']
num, k = {}, 0
for i, e in enumerate(ends):
    if i: k += max(1, int(round((e['t'] - ends[i - 1]['t']) / per)))
    num[e['frame']] = k
def rn(e): return num[e['frame']]

def stat(v):
    if not v: return 'none'
    v = sorted(v); n = len(v)
    return 'rms %.3f p50 %.3f p95 %.3f max %.3f' % (math.sqrt(sum(x * x for x in v) / n), v[n // 2], v[min(n - 1, int(.95 * n))], v[-1])

lit, dark = [], []
quads_lit, quads_dark = collections.Counter(), collections.Counter()
lost = 0
prev_n = None
for e in ends:
    n = rn(e)
    if prev_n is not None and n - prev_n > 1: lost += n - prev_n - 1
    prev_n = n
    if int(e['proj']) > 0:
        d = disps.get(int(e['frame']))
        if not d: continue
        lit.append((n, e, d))
        quads_lit[int(e['quads'])] += 1
    else:
        dark.append((n, e))
        quads_dark[int(e['quads'])] += 1

print('%s: %.1f s (%.3f to %.3f), period %.3f ms: %d refreshes ended (%d lost), %d lit, %d dark' %
      (path, ends[-1]['t'] - t0, t0, ends[-1]['t'], per * 1e3, len(ends), lost, len(lit), len(dark)))
print('quad layers on lit refreshes %s; on dark refreshes %s' %
      (dict(sorted(quads_lit.items())), dict(sorted(quads_dark.items())) if dark else 'none'))
blk = sum(1 for _, e in dark if int(e.get('black', 0)) > 0)
print('dark refreshes with the black projection layer (LARECOMP_XR_BFI_DARK=black): %d of %d; lit refreshes with one too: %d' %
      (blk, len(dark), sum(1 for _, e, _ in lit if int(e.get('black', 0)) > 0)))

# sets: eye 0's image (swapchain, release)
img = lambda d: (int(d['sc0']), int(d['rel0']))
per_set = collections.Counter(img(d) for _, _, d in lit)
hist = collections.Counter(per_set.values())
print('sets: %d, lit on 1 / 2 / 3 / 4+ refreshes: %d / %d / %d / %d' %
      (len(per_set), hist[1], hist[2], hist[3], sum(v for k, v in hist.items() if k >= 4)))

gaps = collections.Counter()
row = same_row = same_any = 0
phase_changes = 0
par_prev = None
for (n0, _, d0), (n1, _, d1) in zip(lit, lit[1:]):
    g = n1 - n0
    gaps[min(g, 6)] += 1
    if g == 1:
        row += 1
        if img(d0) == img(d1): same_row += 1
    if img(d0) == img(d1): same_any += 1
for n, _, _ in lit:
    par = n & 1
    if par_prev is not None and par != par_prev: phase_changes += 1
    par_prev = par
print('gaps between flashes 1 / 2 / 3 / 4 / 5 / 6+ refreshes: %s' % ' / '.join(str(gaps[k]) for k in range(1, 7)))
print('lit refreshes in a row: %d (%d with the same image); the same image on two flashes in a row: %d; phase changes '
      '(the lit refresh number\'s parity): %d' % (row, same_row, same_any, phase_changes))

# the display time each lit refresh's poses were located for
late = collections.Counter()
on_err, all_err = [], []
for n, e, d in lit:
    loc = d.get('loc_ms', -1.0)
    if loc < 0:
        late['?'] += 1
        continue
    k = int(round((d['t'] * 1e3 - loc) / (per * 1e3)))
    late[k if k <= 2 and k >= 0 else ('3+' if k > 2 else '<0')] += 1
    all_err.append(d['rot_err'])
    if k == 0: on_err.append(d['rot_err'])
print('lit refreshes against the display time their poses were located for: on it %d, 1 after %d, 2 after %d, 3+ %d, '
      'before %d, no locate %d' % (late[0], late[1], late[2], late['3+'], late['<0'], late['?']))
print('layer rotation against the head (deg), lit refreshes: %s; within 0.1: %d of %d' %
      (stat(all_err), sum(x <= 0.1 for x in all_err), len(all_err)))
print('  those on the display time their poses were located for: %s; within 0.1: %d of %d' %
      (stat(on_err), sum(x <= 0.1 for x in on_err), len(on_err)))
# flash to flash: the change of (layer - head) in yaw / pitch between consecutive lit refreshes 2 or fewer apart
ch = []
for (n0, _, d0), (n1, _, d1) in zip(lit, lit[1:]):
    if n1 - n0 > 2: continue
    e0 = (d0['layer_yaw'] - d0['true_yaw'], d0['layer_pitch'] - d0['true_pitch'])
    e1 = (d1['layer_yaw'] - d1['true_yaw'], d1['layer_pitch'] - d1['true_pitch'])
    ch.append(math.hypot(e1[0] - e0[0], e1[1] - e0[1]))
print('flash-to-flash change of the layer-vs-head error (deg; flashes up to 2 refreshes apart): %s; over 0.1: %d of %d' %
      (stat(ch), sum(x > 0.1 for x in ch), len(ch)))
if lines:
    for n, e, d in lit:
        print('lit n=%d t=%.4f sc0=%d rel0=%d loc_ms=%.3f rot_err=%.4f quads=%d' % (n, e['t'], int(d['sc0']), int(d['rel0']),
              d.get('loc_ms', -1), d['rot_err'], int(e['quads'])))

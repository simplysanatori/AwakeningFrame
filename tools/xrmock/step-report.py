#!/usr/bin/env python3
# Ghost check after head steps (xrmock MOCK_XR_POSE with instant pose steps, MOCK_XR_SHIFT + MOCK_XR_SHIFT_DUMP):
# within each hold of one pose, every eye image is compared with the hold's last image (rendered after many frames at
# that pose).  A renderer whose images depend only on the pose they claim gives the same difference right after a
# step as later in the hold (game animation alone); content left from the frame before the step (a pass that reads an
# earlier frame's image, or an eye drawn with an older camera) shows as a larger difference in the first images.
# Usage: step-report.py shift.txt dumpdir [--diffs outdir]
import sys, os

def kv(line):
    d = {}
    for tok in line.split()[1:]:
        k, v = tok.split('=', 1)
        d[k] = float(v)
    return d

def pgm(path):
    b = open(path, 'rb').read()
    parts = b.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]

shift, ddir = sys.argv[1], sys.argv[2]
out = sys.argv[sys.argv.index('--diffs') + 1] if '--diffs' in sys.argv else None
dumps = [kv(l) for l in open(shift) if l.startswith('dump ')]
for eye in (0, 1):
    seq = sorted([d for d in dumps if int(d['eye']) == eye], key=lambda d: d['frame'])
    holds, cur = [], []
    for d in seq:
        if cur and abs(d['pitch'] - cur[-1]['pitch']) > 0.5: holds.append(cur); cur = []
        cur.append(d)
    if cur: holds.append(cur)
    holds = [h for h in holds[1:-1] if len(h) >= 20]  # whole holds only
    by_pos = {}
    for h in holds:
        ref = pgm('%s/f%05d_e%d.pgm' % (ddir, int(h[-1]['frame']), eye))[2]
        for j, d in enumerate(h[:-1]):
            im = pgm('%s/f%05d_e%d.pgm' % (ddir, int(d['frame']), eye))[2]
            diff = sum(abs(a - b) for a, b in zip(im, ref)) / len(im)
            big = sum(1 for a, b in zip(im, ref) if abs(a - b) > 24) / len(im)
            k = j if j < 4 else 4
            by_pos.setdefault(k, []).append((diff, big))
            if out and j < 2 and h is holds[0]:
                os.makedirs(out, exist_ok=True)
                w, hh, _ = pgm('%s/f%05d_e%d.pgm' % (ddir, int(d['frame']), eye))
                with open('%s/diff_e%d_j%d.pgm' % (out, eye, j), 'wb') as f:
                    f.write(b'P5\n%d %d\n255\n' % (w, hh))
                    f.write(bytes(min(255, abs(a - b) * 4) for a, b in zip(im, ref)))
    print('eye %d: %d holds' % (eye, len(holds)))
    for k in sorted(by_pos):
        v = by_pos[k]
        print('  %s: mean |diff| to the hold\'s last image %.2f / 255, pixels off by > 24: %.2f%%  (n %d)' % (
            'image %d after the step' % (k + 1) if k < 4 else 'later images      ', sum(a for a, _ in v) / len(v), 100 * sum(b for _, b in v) / len(v), len(v)))

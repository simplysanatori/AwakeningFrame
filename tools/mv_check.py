#!/usr/bin/env python3
"""mv_check.py: the comparator of tools/mv_check.sh (promoted from the mv lane's mvcmp.py).

mv_check.py OUT [--limit PCT] [--thr N] [--mode LABEL]

OUT is a mv_check.sh output folder: spots.tsv (index, name, dump frame), and per session (base, cand, optionally
noise) a folder <session>/ holding eyes/stereo_<frame>_{eye0,eye1,hud}.png and run.log; sessions.tsv names them
(label, env text). Compares cand with base per spot and eye: the share of pixels where any channel differs by more
than THR (24/255), the noise floor (base vs a second base run) when there is one, eye 0 vs eye 1 inside cand (a copy
of eye 0 in eye 1 is a FAIL: the eyes must differ), the HUD quad; writes heat maps (base | cand | diff) to OUT/heat/,
greps the run logs, prints a pass/fail table and OUT/results.md. Exit status 1 on any FAIL. PIL only.
"""
import os, re, sys
from PIL import Image, ImageChops, ImageFilter, ImageStat

args = sys.argv[1:]
def opt(name, default):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return type(default)(v)
    return default
LIMIT = opt('--limit', 1.5)      # % of pixels allowed to differ (the floor below it is the run-to-run noise)
THR = opt('--thr', 24)           # a channel difference over this (of 255) counts
MODE = opt('--mode', 'v1 vs MV')
OUT = args[0]

def rd(name):
    p = os.path.join(OUT, name)
    return [l.rstrip('\n').split('\t') for l in open(p)] if os.path.exists(p) else []

spots = [(int(r[0]), r[1], r[2]) for r in rd('spots.tsv')]
sess = {r[0]: r[1] if len(r) > 1 else '' for r in rd('sessions.tsv')}
BASE, CAND, NOISE = 'base', 'cand', 'noise'

def img(sn, frame, what):
    p = f'{OUT}/{sn}/eyes/stereo_{frame:05d}_{what}.png'
    return Image.open(p).convert('RGB') if os.path.exists(p) else None

def maxdiff(x, y):   # the largest channel difference per pixel, as an L image
    m = None
    for c in ImageChops.difference(x, y).split():
        m = c if m is None else ImageChops.lighter(m, c)
    return m

def share(x, y):     # (% of pixels over THR, mean difference) or None for a size mismatch
    if x.size != y.size: return None
    m = maxdiff(x, y)
    h = m.histogram()
    return 100.0 * sum(h[THR + 1:]) / (x.size[0] * x.size[1]), ImageStat.Stat(ImageChops.difference(x, y)).mean

def masked(x, y, n):  # base x vs cand y outside the pixels where x differs from the second base run n (dilated):
    # (% of the unmasked pixels over THR, % of all pixels masked): the game is not deterministic run to run
    mask = maxdiff(x, n).point(lambda v: 255 if v > THR else 0).filter(ImageFilter.MaxFilter(7))
    d = ImageChops.subtract(maxdiff(x, y).point(lambda v: 255 if v > THR else 0), mask)
    tot = x.size[0] * x.size[1]
    keep = mask.histogram()[0]
    return 100.0 * d.histogram()[255] / max(1, keep), 100.0 * (tot - keep) / tot

def heat(x, y, path):  # base | cand | diff: the diff in red (over THR, brighter = larger) on a dim base
    m = maxdiff(x, y)
    dim = x.convert('L').point(lambda v: v // 3).convert('RGB')
    hot = Image.merge('RGB', (m.point(lambda v: 0 if v <= THR else min(255, 120 + v)),
                              m.point(lambda v: 0 if v <= THR else max(0, v - 128) * 2), m.point(lambda v: 0)))
    mask = m.point(lambda v: 255 if v > THR else 0)
    d = Image.composite(hot, dim, mask)
    w, h = x.size
    sheet = Image.new('RGB', (3 * (w // 2), h // 2))
    for i, im in enumerate((x, y, d)): sheet.paste(im.resize((w // 2, h // 2)), (i * (w // 2), 0))
    sheet.save(path)

os.makedirs(f'{OUT}/heat', exist_ok=True)
rows, fails = [], 0
have_noise = os.path.isdir(f'{OUT}/{NOISE}')
for idx, name, frame in spots:
    frame = int(frame); why = []; weak = []
    cell = {}; noise_cols = {}
    for e in (0, 1):
        a, b = img(BASE, frame, f'eye{e}'), img(CAND, frame, f'eye{e}')
        if a is None or b is None:
            cell[e] = 'missing'; why.append(f'eye{e} dump missing ({"base" if a is None else "cand"})'); continue
        r = share(a, b)
        if r is None: cell[e] = 'size'; why.append(f'eye{e} size differs {a.size} vs {b.size}'); continue
        p = r[0]
        nz = ms = None
        if have_noise:
            n = img(NOISE, frame, f'eye{e}')
            if n is not None and n.size == a.size: nz = share(a, n)[0]; ms = masked(a, b, n)
        # with a second base run the verdict is the share outside the pixels that moved between base runs
        cell[e] = f'{p:.2f}%' + (f' | {ms[0]:.2f}% outside noise ({ms[1]:.0f}% masked)' if ms else '')
        if ms:
            noise_cols[e] = f'{nz:.2f}%'
            if ms[0] > LIMIT: why.append(f'eye{e} {ms[0]:.2f}% > {LIMIT:.2f}% (outside the noise mask)')
            elif ms[1] > 40: weak.append(f'eye{e} {ms[1]:.0f}% of the image masked as noisy')
        elif p > LIMIT: why.append(f'eye{e} {p:.2f}% > {LIMIT:.2f}%')
        heat(a, b, f'{OUT}/heat/{idx}_{re.sub(r"[^A-Za-z0-9]+", "_", name)}_eye{e}.png')
    # eye 0 vs eye 1 inside cand (and base for reference): the eyes must differ
    c0, c1, b0, b1 = img(CAND, frame, 'eye0'), img(CAND, frame, 'eye1'), img(BASE, frame, 'eye0'), img(BASE, frame, 'eye1')
    ee = 'n/a'
    if c0 is not None and c1 is not None and c0.size == c1.size:
        cs = share(c0, c1)[0]
        bs = share(b0, b1)[0] if b0 is not None and b1 is not None and b0.size == b1.size else None
        ee = f'{cs:.1f}%' + (f' (base {bs:.1f})' if bs is not None else '')
        if cs < 0.1 or (bs is not None and bs > 1 and cs < 0.3 * bs): why.append(f'cand eyes ~identical ({cs:.2f}%): eye 1 is a copy')
    # the HUD quad
    ha, hb = img(BASE, frame, 'hud'), img(CAND, frame, 'hud')
    hud = '-'
    if ha is not None and hb is not None and ha.size == hb.size:
        hp = share(ha, hb)[0]; hud = f'{hp:.2f}%'
        if hp > LIMIT: why.append(f'hud {hp:.2f}% > {LIMIT:.2f}%')
    elif (ha is None) != (hb is None): hud = 'missing'; why.append('hud dump only in one run')
    fails += bool(why)
    rows.append((name, frame, cell.get(0, '?'), cell.get(1, '?'), ee, hud, noise_cols.get(0, '-'),
                 ('PASS' + (' (weak: ' + '; '.join(weak) + ')' if weak else '')) if not why else 'FAIL: ' + '; '.join(why)))

# ---- the logs, per session
def logcheck(sn):
    p = f'{OUT}/{sn}/run.log'
    if not os.path.exists(p): return None
    t = open(p, errors='replace').read().splitlines()
    bad = {'pose': 0, 'split': 0}; nxr = 0; mv = None; mv_on = None; val = []; flt = []
    for l in t:
        if l.startswith('[xr]') and 'pose mismatches' in l:
            nxr += 1
            m = re.search(r'pose mismatches (\d+) of', l)
            if m and int(m.group(1)): bad['pose'] += int(m.group(1))
            m = re.search(r'(\d+) sets with eyes of two frames', l)
            if m and int(m.group(1)): bad['split'] += int(m.group(1))
        if l.startswith('[mv] 5 s:'): mv = l
        if l.startswith('[mv] multiview'): mv_on = l
        if re.match(r'^Validation (Error|Warning)|.*VUID-|\[vk\] stopping at the first validation', l): val.append(l)
        if re.search(r'DEVICE_LOST|device lost|GPU fault|VK_ERROR_|\[crash\]|^FATAL|Segmentation fault|Aborted \(core|vkQueue\w+ failed', l): flt.append(l)
    rc = next((l for l in reversed(t) if l.startswith('rc=')), 'rc=?')
    return dict(n=nxr, bad=bad, mv=mv, mv_on=mv_on, val=val, flt=flt, rc=rc)

logrows, lfails = [], 0
for sn in (BASE, CAND, NOISE):
    if sn not in sess: continue
    L = logcheck(sn); why = []
    if L is None: logrows.append((sn, 'no run.log', 'FAIL')); lfails += 1; continue
    if L['n'] == 0: why.append('no [xr] stats line')
    if L['bad']['pose']: why.append(f'pose mismatches {L["bad"]["pose"]}')
    if L['bad']['split']: why.append(f'{L["bad"]["split"]} sets with eyes of two frames')
    if L['val']: why.append(f'{len(L["val"])} validation messages')
    if L['flt']: why.append(f'{len(L["flt"])} fault lines')
    if not L['rc'] in ('rc=0',): why.append(L['rc'])
    mvwanted = 'LARECOMP_MV=1' in sess[sn] and 'LARECOMP_MV_PASSES=0' not in sess[sn]
    if mvwanted:
        if L['mv_on'] is None: why.append('no "[mv] multiview" line: MV did not start')
        elif 'not supported' in L['mv_on']: why.append('multiview not supported: MV not exercised')
        if L['mv'] is None: why.append('no [mv] 5 s stats line')
    mvtxt = L['mv'].replace('[mv] 5 s: ', '') if L['mv'] else '-'
    info = f'{L["n"]} [xr] lines, pose mismatches {L["bad"]["pose"]}, eyes-of-two-frames {L["bad"]["split"]}, validation {len(L["val"])}, faults {len(L["flt"])}, {L["rc"]}'
    if mvwanted or L['mv']: info += f'; [mv] {mvtxt[:170]}'
    for l in (L['val'] + L['flt'])[:3]: info += f'\n      > {l[:200]}'
    logrows.append((f'{sn} ({sess[sn]})', info, 'PASS' if not why else 'FAIL: ' + '; '.join(why)))
    lfails += bool(why)

# ---- the report
o = [f'# mv_check: {MODE}', '', f'threshold: a channel difference over {THR}/255; allowed share {LIMIT:.2f}% of pixels'
     + ('; with a second base run, only the pixels that did not move between the two base runs count (the game is not deterministic run to run)' if have_noise else ''), '',
     '| spot | frame | eye 0 diff | eye 1 diff | cand eye0 vs eye1 | hud diff | noise (base vs base, eye 0) | result |', '|---|---|---|---|---|---|---|---|']
for r in rows: o.append('| ' + ' | '.join(str(x) for x in r) + ' |')
o += ['', '| session | log checks | result |', '|---|---|---|']
for r in logrows: o.append('| ' + ' | '.join(r) + ' |')
tot = fails + lfails
o += ['', f'heat maps (base | cand | diff, half size): {OUT}/heat/', f'RESULT: {"PASS" if not tot else "FAIL"} ({fails} of {len(rows)} spots, {lfails} log sessions failed)']
txt = '\n'.join(o)
open(f'{OUT}/results.md', 'w').write(txt + '\n')
# plain table for the terminal
w = max([len(r[0]) for r in rows] + [4])
print(f'mv_check: {MODE} (diff = % of pixels with a channel off by > {THR}/255; allowed {LIMIT:.2f}%)')
print(f'{"spot":<{w}}  {"eye0 diff":<44}  {"eye1 diff":<44}  {"cand e0-vs-e1":<18}  {"hud":>6}  {"noise":>6}  result')
for r in rows: print(f'{r[0]:<{w}}  {r[2]:<44}  {r[3]:<44}  {r[4]:<18}  {r[5]:>6}  {r[6]:>6}  {r[7]}')
print()
for r in logrows: print(f'log {r[0]}: {r[2]}\n    {r[1]}')
print(f'\nRESULT: {"PASS" if not tot else "FAIL"} ({fails} of {len(rows)} spots, {lfails} log sessions failed)   details: {OUT}/results.md')
sys.exit(1 if tot else 0)

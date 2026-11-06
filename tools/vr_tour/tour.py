#!/usr/bin/env python3
"""VR tour analysis (tools/vr_tour.sh, docs/af/TOUR.md): turns the raw sessions of one tour into report.md, sheet.png,
data.json and spots/, or compares two tours.

    tour.py report <tour dir> [--keep-raw] [--compare <older tour dir>]
    tour.py compare <older tour dir> <newer tour dir>

Run with python -I and PIL (no numpy).  Reads <dir>/meta.tsv, spots.tsv, sessions.tsv and raw/<session>/{run.log (every
line with its wall-clock time), timing.csv, shots/}.
"""
import csv
import glob
import json
import math
import os
import re
import statistics as st
import sys

from PIL import Image, ImageChops, ImageDraw, ImageStat

# ---- thresholds (docs/af/TOUR.md explains each)
SETTLE_S = 4.0           # seconds after a warp the timing window starts at the latest (else 1 s after the level's request object: loaded)
SETTLE_LOADED_S = 1.0
SHOT_SETTLE_S = 5.0      # a screenshot must be taken at least this long after the warp
SLOW_FPS = 0.92          # game fps below this share of nominal (the game runs at 30 fps below 100 Hz, else 60)
SLOW_SETS = 0.90         # sets shown per second below this share of the nominal game fps
SLOW_MISSED = 2.0        # percent of refreshes missed
SLOW_HITCHES = 3         # game frames over 1.5x nominal in a window
SLOW_GPU = 0.90          # both eyes' GPU time above this share of the game frame period
BLACK_LUMA = 1.0         # mean luma below this: a black eye
BLACK_FRAC = 0.985       # or this share of pixels under luma 8
HUD_MIN = 0.0003         # a HUD quad with a smaller share of drawn pixels is missing (three hearts are 0.17%)
EYE_RES_MAX = 0.6        # the two eyes disagree: difference at the best shift over the image's brightness (good pairs 0.2-0.3)
DISP_MIN_CONF = 1.25     # eye disparity: the best shift must beat the zero shift by this ratio to count
FPS_DROP = 0.10          # --compare: fps / sets per second drop
CMP_COLOUR = 60.0        # --compare: distance of the mean colour (0..255 per channel, euclidean)
CMP_HIST = 0.55          # --compare: histogram distance 0..1
CMP_MAD = 80.0           # --compare: mean absolute difference of the 48 px thumbnails
CMP_BLACK = 0.35         # --compare: change of the black share
CMP_HUD = 0.5            # --compare: the HUD's drawn share fell under this share of before (a banner can add to it, so only a loss counts)

STAMP = re.compile(r'^(\d+\.\d+)\t(.*)$')


def slug(s):
    return re.sub(r'[^A-Za-z0-9]+', '_', s).strip('_')


def read_tsv(p):
    if not os.path.exists(p):
        return []
    return [l.rstrip('\n').split('\t') for l in open(p, encoding='utf-8', errors='replace') if l.strip()]


def read_log(p):
    out = []
    if not os.path.exists(p):
        return out
    for l in open(p, encoding='utf-8', errors='replace'):
        m = STAMP.match(l.rstrip('\n'))
        if m:
            out.append((float(m.group(1)), m.group(2)))
    return out


def mean(v):
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else None


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(len(v) * p))] if v else None


def f1(x, nd=1):
    return '-' if x is None else ('%.*f' % (nd, x))


# ------------------------------------------------------------------------------------------------ log parsing
RE_PACE = re.compile(r'^\[pace\] period (\d.*)$')
RE_KV = re.compile(r'([a-z][a-z ]*?) (-?\d+(?:\.\d+)?)')
RE_VK = re.compile(r'^\[vk\] t=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms level (\S*) \((.*)\)$')
RE_GPU = re.compile(r'^\[stereo\] GPU per game frame: eye 0 ([\d.]+) ms, eye 1 ([\d.]+) ms, both ([\d.]+) ms \(max ([\d.]+)\) over (\d+) frames')
RE_XR = re.compile(r'^\[xr\] (\d+) s: (\d+) refreshes at (\d+) Hz \((\d+) missed\), (\d+) vsyncs, (\d+) sets shown \(([\d.]+) ms after submission\)(.*)$')
RE_JUMP = re.compile(r'^\[warp\] jumping to (\S+) / (\S+)')
CRASH = re.compile(r'^(FATAL:|\*\*\* |\[crash\]|.*Segmentation fault|.*Aborted \(core)')


def parse_pace(line):
    m = RE_PACE.match(line)
    if not m or '(no gameplay)' in line:
        return None
    kv = {k.strip(): float(v) for k, v in RE_KV.findall('period ' + m.group(1))}
    return kv if 'period' in kv else None


def parse_xr(line):
    m = RE_XR.match(line)
    if not m:
        return None
    d = {'secs': float(m.group(1)), 'refreshes': int(m.group(2)), 'hz': int(m.group(3)), 'missed': int(m.group(4)),
         'vsyncs': int(m.group(5)), 'sets': int(m.group(6)), 'after_ms': float(m.group(7))}
    rest = m.group(8)
    for key, pat in (('reshown', r'(\d+) refreshes re-showed a set'), ('gpu_late', r'(\d+) waited for the GPU'), ('no_layers', r'(\d+) without layers'),
                     ('skipped', r'(\d+) frames skipped')):
        mm = re.search(pat, rest)
        d[key] = int(mm.group(1)) if mm else None
    mm = re.search(r'pose mismatches (\d+) of (\d+) refreshes', rest)
    d['pose_bad'], d['pose_of'] = (int(mm.group(1)), int(mm.group(2))) if mm else (None, None)
    mm = re.search(r'(\d+) sets with eyes of two frames', rest)
    d['eye_split'] = int(mm.group(1)) if mm else None
    mm = re.search(r'latch to display ([\d.]+) ms', rest)
    d['latch_ms'] = float(mm.group(1)) if mm else None
    return d


def parse_vk_stats(s):
    d = {}
    for pat, key in ((r'(\d+) draws', 'draws'), (r'(\d+) passes', 'passes'), (r'(\d+) flushes', 'flushes'), (r'compile (\d+)', 'compile'),
                     (r'wait (\d+)ms', 'wait_ms'), (r'upload ([\d.]+)MB', 'upload_mb')):
        m = re.search(pat, s)
        d[key] = float(m.group(1)) if m else None
    return d


def analyse_session(row, spots):
    """One session's log: when each spot was warped to, the lines of every spot's window, the session's totals."""
    name, kind, hz, dwell, step, rc, t0, t1, d = row
    hz, rc, t0, t1 = int(hz), int(rc), float(t0), float(t1)
    log = read_log(os.path.join(d, 'run.log'))
    S = {'name': name, 'kind': kind, 'hz': hz, 'dwell': float(dwell), 'rc': rc, 'dir': d, 'wall_s': t1 - t0, 'lines': len(log)}
    byloc = {(s['level'], s['loc']): i for i, s in enumerate(spots)}
    jumps = {}
    for t, l in log:
        m = RE_JUMP.match(l)
        if m and (m.group(1), m.group(2)) in byloc:
            jumps.setdefault(byloc[(m.group(1), m.group(2))], t)
    S['jumps'] = jumps
    t_end = log[-1][0] if log else t1
    S['crash'] = None
    for t, l in log:
        if CRASH.match(l):
            S['crash'] = (t, l.strip()[:160])
            break
    if S['crash'] is None and (rc >= 128 or rc == 124):
        S['crash'] = (t_end, 'the session ended with exit status %d%s' % (rc, ' (timed out)' if rc == 124 else ''))
    S['mock_errors'] = [(t, l) for t, l in log if l.startswith('xrmock: ERROR')]
    for t, l in log:
        m = re.match(r'^xrmock: summary: (\d+) errors, (\d+) warnings; (\d+) frames ended', l)
        if m:
            S['mock_summary'] = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
        if l.startswith('xrmock: timing:') and 'xrWaitFrame interval' in l:
            S['mock_wait'] = l[len('xrmock: timing: '):]
        if l.startswith('xrmock: timing:') and 'begin to xrEndFrame' in l.replace('xrBeginFrame to ', 'begin to '):
            S['mock_end'] = l[len('xrmock: timing: '):]
    S['vuids'] = [(t, l) for t, l in log if 'VUID-' in l or 'Validation Error' in l]
    S['errors_game'] = [(t, l) for t, l in log if re.match(r'^\[xr\] .*(failed|lacks|cannot)', l)]
    # the session's own totals from the mock's timing CSV
    tc = os.path.join(d, 'timing.csv')
    if os.path.exists(tc):
        n = late = lost = nolayers = 0
        for r in csv.reader(l for l in open(tc) if not l.startswith('#')):
            try:
                n += 1
                nolayers += int(r[10]) == 0
                lost += int(r[11])
                late += int(r[12])
            except (ValueError, IndexError):
                n -= 1
        S['csv'] = {'frames': n, 'late': late, 'lost': lost, 'no_layers': nolayers}
    # windows
    order = sorted(jumps.items(), key=lambda kv: kv[1])
    S['windows'] = {}
    reqs = [t for t, l in log if l.startswith('[warp] level-jump request object')]
    for k, (i, t) in enumerate(order):
        end = order[k + 1][1] if k + 1 < len(order) else t_end
        loaded = [r for r in reqs if t <= r < t + 12.0]
        a = min(t + SETTLE_S, loaded[0] + SETTLE_LOADED_S) if loaded else t + SETTLE_S
        S['windows'][i] = (t, max(a, t + 1.5), end)
    S['t_end'] = t_end
    S['log'] = log
    return S


def window_metrics(S, i, nominal):
    """Per-spot numbers of one session: only lines stamped inside [warp + settle, next warp)."""
    if i not in S['windows']:
        return None
    t_warp, a, b = S['windows'][i]
    m = {'secs': max(0.0, b - a)}
    pace = []
    vk, gpu, xr = [], [], []
    mism_lines = 0
    for t, l in S['log']:
        if t < a - 0.0 or t >= b:
            continue
        if l.startswith('[pace]'):
            p = parse_pace(l)
            if p:
                pace.append(p)
        elif l.startswith('[vk] t='):
            mm = RE_VK.match(l)
            if mm:
                d = parse_vk_stats(mm.group(6))
                d.update(fps=float(mm.group(3)), max_ms=float(mm.group(4)), level=mm.group(5), pp=mm.group(6), t=t)
                vk.append(d)
        elif l.startswith('[stereo] GPU per game frame'):
            mm = RE_GPU.match(l)
            if mm:
                gpu.append({'e0': float(mm.group(1)), 'e1': float(mm.group(2)), 'both': float(mm.group(3)), 'max': float(mm.group(4)), 'n': int(mm.group(5))})
        elif l.startswith('[xr] ') and ' s: ' in l[:12]:
            d = parse_xr(l)
            if d:
                d['t'] = t
                xr.append(d)
        elif l.startswith("[stereo] set ") and 'eye images' in l:
            mism_lines += 1
    m['frames'] = len(pace)
    if pace:
        per = [p['period'] for p in pace]
        m['period'] = mean(per)
        m['fps'] = 1000.0 / m['period']
        m['period_p95'] = pct(per, 0.95)
        m['period_max'] = max(per)
        m['hitches'] = sum(1 for x in per if x > 1.5 * 1000.0 / nominal)
        for key, out in (('busy', 'busy'), ('render cpu', 'render_cpu'), ('cpu', 'cpu'), ('gpu', 'gpu'), ('gpu wait', 'gpu_wait'), ('queue wait', 'queue_wait'),
                         ('blocked', 'blocked')):
            m[out] = mean([p.get(key) for p in pace])
        m['busy_p95'] = pct([p['busy'] for p in pace if 'busy' in p], 0.95)
    if vk:
        m['vk_fps'] = mean([v['fps'] for v in vk])
        m['vk_max_ms'] = max(v['max_ms'] for v in vk)
        m['compile'] = sum(v['compile'] or 0 for v in vk)
        for key in ('draws', 'passes', 'flushes', 'wait_ms', 'upload_mb'):
            m[key] = mean([v[key] for v in vk])
        m['level'] = vk[-1]['level']
        m['pp'] = vk[-1]['pp']
    if gpu:
        w = sum(g['n'] for g in gpu) or 1
        for key in ('e0', 'e1', 'both'):
            m['gpu_' + key] = sum(g[key] * g['n'] for g in gpu) / w
        m['gpu_max'] = max(g['max'] for g in gpu)
    # the [xr] summaries: lines whose 5 s lie inside the window count; else the one overlapping most (marked)
    inside = [x for x in xr if x['t'] - x['secs'] >= a - 0.6]
    if not inside:
        best, ov = None, 0.0
        for t, l in S['log']:
            if l.startswith('[xr] ') and ' s: ' in l[:12]:
                d = parse_xr(l)
                if d:
                    o = min(t, b) - max(t - d['secs'], a)
                    if o > ov:
                        best, ov = d, o
        inside = [best] if best and ov > 2.0 else []
        m['xr_partial'] = bool(inside)
    if inside:
        secs = sum(x['secs'] for x in inside)
        m['sets_per_s'] = sum(x['sets'] for x in inside) / secs
        refr = sum(x['refreshes'] for x in inside)
        m['missed_pct'] = 100.0 * sum(x['missed'] for x in inside) / refr if refr else None
        m['gpu_late_pct'] = 100.0 * sum(x['gpu_late'] or 0 for x in inside) / refr if refr else None
        m['after_ms'] = mean([x['after_ms'] for x in inside])
        pb = [x['pose_bad'] for x in inside if x['pose_bad'] is not None]
        po = [x['pose_of'] for x in inside if x['pose_of'] is not None]
        m['pose_bad'] = sum(pb) if pb else None
        m['pose_of'] = sum(po) if po else None
        es = [x['eye_split'] for x in inside if x['eye_split'] is not None]
        m['eye_split'] = sum(es) if es else None
        m['latch_ms'] = mean([x['latch_ms'] for x in inside])
        m['xr_lines'] = len(inside)
    m['pose_log_lines'] = mism_lines
    m['mock_errors'] = sum(1 for t, l in S['mock_errors'] if a <= t < b)
    m['vuids'] = sum(1 for t, l in S['vuids'] if t_warp <= t < b)
    return m


# ------------------------------------------------------------------------------------------------ images
def thumb_stats(im):
    """Colour numbers of an eye or HUD image (RGB, any size)."""
    small = im.convert('RGB').resize((96, max(1, round(96 * im.height / im.width))))
    st_ = ImageStat.Stat(small)
    g = small.convert('L')
    px = g.tobytes()
    black = sum(1 for p in px if p < 8) / len(px)
    hist = []
    for ch in small.split():
        h = ch.resize((64, 64)).histogram()
        b = [sum(h[k * 16:(k + 1) * 16]) for k in range(16)]
        tot = float(sum(b)) or 1.0
        hist += [x / tot for x in b]
    return {'mean_rgb': [round(x, 2) for x in st_.mean], 'luma': round(ImageStat.Stat(g).mean[0], 2), 'black': round(black, 4),
            'hist': [round(x, 4) for x in hist]}


def disparity(left, right):
    """Horizontal disparity of the pair in pixels of the full-size image and how sure: d = x_left - x_right of matching
    content (positive for anything nearer than infinity, as OpenXR's parallel eyes give), by the shift that makes the two
    grayscale images agree best.  conf = (difference at no shift) / (difference at the best shift)."""
    W = 320
    sc = W / left.width
    H = max(8, round(left.height * sc))
    a = left.convert('L').resize((W, H), Image.BILINEAR)
    b = right.convert('L').resize((W, H), Image.BILINEAR)
    maxs = 30
    y0, y1 = H // 8, H - H // 8
    x0, x1 = maxs + 2, W - maxs - 2
    ref = b.crop((x0, y0, x1, y1))
    best, bs, d0 = None, 0, None
    for s in range(-maxs, maxs + 1):   # b(x) == a(x + s)  ->  disparity d = s
        cand = a.crop((x0 + s, y0, x1 + s, y1))
        v = ImageStat.Stat(ImageChops.difference(ref, cand)).mean[0]
        if s == 0:
            d0 = v
        if best is None or v < best:
            best, bs = v, s
    conf = (d0 + 0.25) / (best + 0.25)
    res = best / (ImageStat.Stat(ref).mean[0] + 0.5)
    return bs / sc, conf, d0, res


def hud_share(im):
    """Share of the HUD quad's pixels that are drawn (alpha when it has one, else not black)."""
    im = im.convert('RGBA')
    a = im.getchannel('A')
    ap = a.tobytes()
    if min(ap) < 250:   # transparent where nothing is drawn
        return sum(1 for p in ap if p > 12) / len(ap)
    g = im.convert('L').resize((480, max(1, round(480 * im.height / im.width)))).tobytes()
    return sum(1 for p in g if p > 14) / len(g)


def on_bg(im, bg=(34, 36, 44)):
    im = im.convert('RGBA')
    base = Image.new('RGBA', im.size, bg + (255,))
    return Image.alpha_composite(base, im).convert('RGB')


SHOT_END_MARGIN_S = 1.5  # dumps this close to the next warp (or the end of the session) are mid-transition: not used
MAX_CANDIDATES = 4


def pick_shots(S, i):
    """The screenshots (frames with both eyes) taken inside spot i's window, latest last, at most MAX_CANDIDATES."""
    if i not in S['windows']:
        return []
    t_warp, _, end = S['windows'][i]
    frames = {}
    for p in glob.glob(os.path.join(S['dir'], 'shots', 'f*_proj*_eye[01].png')):
        m = re.match(r'f(\d+)_proj(\d+)_eye([01])\.png$', os.path.basename(p))
        if m:
            frames.setdefault((int(m.group(1)), int(m.group(2))), {})[int(m.group(3))] = p
    out = []
    for (f, layer), eyes in sorted(frames.items()):
        if 0 not in eyes or 1 not in eyes:
            continue
        mt = max(os.path.getmtime(eyes[0]), os.path.getmtime(eyes[1]))
        if mt < t_warp + SHOT_SETTLE_S or mt >= end - SHOT_END_MARGIN_S:
            continue
        quads = sorted(glob.glob(os.path.join(S['dir'], 'shots', 'f%04d_quad*.png' % f)))
        out.append({'frame': f, 'e0': eyes[0], 'e1': eyes[1], 'hud': quads[-1] if quads else None, 'mtime': mt})
    return out[-MAX_CANDIDATES:]


def stats_shot(shot):
    r = {'frame': shot['frame']}
    e0, e1 = Image.open(shot['e0']), Image.open(shot['e1'])
    r['size'] = [e0.width, e0.height]
    r['eye0'], r['eye1'] = thumb_stats(e0), thumb_stats(e1)
    d, conf, d0, res = disparity(e0, e1)
    r['disparity_px'], r['disparity_conf'], r['eye_diff0'], r['eye_res'] = round(d, 1), round(conf, 2), round(d0, 2), round(res, 3)
    if shot['hud']:
        hu = Image.open(shot['hud'])
        r['hud_share'] = round(hud_share(hu), 5)
        r['hud_size'] = [hu.width, hu.height]
        r['hud_stats'] = thumb_stats(on_bg(hu))
    return r


def save_shot(shot, r, spot_idx, spot_name, out):
    e0, e1 = Image.open(shot['e0']), Image.open(shot['e1'])
    w = 480   # each eye 480 wide: the sheet and --compare read these
    h = round(w * e0.height / e0.width)
    pair = Image.new('RGB', (2 * w, h))
    pair.paste(e0.convert('RGB').resize((w, h), Image.LANCZOS), (0, 0))
    pair.paste(e1.convert('RGB').resize((w, h), Image.LANCZOS), (w, 0))
    base = '%02d_%s' % (spot_idx, slug(spot_name))
    pair.save(os.path.join(out, 'spots', base + '_eyes.png'), optimize=True)
    r['eyes_png'] = 'spots/%s_eyes.png' % base
    if shot['hud']:
        hu = Image.open(shot['hud'])
        on_bg(hu).resize((640, max(1, round(640 * hu.height / hu.width))), Image.LANCZOS).save(os.path.join(out, 'spots', base + '_hud.png'), optimize=True)
        r['hud_png'] = 'spots/%s_hud.png' % base


def is_black(e):
    return e['luma'] < BLACK_LUMA or e['black'] > BLACK_FRAC


def analyse_spot_shots(S, i, spot_name, out):
    """All candidate screenshots of the spot; the one shown is the brightest with both eyes drawn.  'black_eyes' lists the
    eyes black in every candidate (then it is a verdict), 'flicker' the eyes black in some."""
    cands = pick_shots(S, i)
    if not cands:
        return None
    rs = [stats_shot(c) for c in cands]
    ok = [k for k, r in enumerate(rs) if not is_black(r['eye0']) and not is_black(r['eye1'])]
    # (the one shown: the pair whose eyes agree best; the game camera glides after a warp or a walk and a pair then shows
    # two latches of it, which is what the other candidates are for)
    best = min(ok, key=lambda k: rs[k]['eye_res']) if ok else max(range(len(rs)), key=lambda k: rs[k]['eye0']['luma'] + rs[k]['eye1']['luma'])
    r = rs[best]
    r['candidates'] = len(rs)
    r['eyes_disagree'] = bool(ok) and all(rs[k]['eye_res'] > EYE_RES_MAX for k in ok)
    r['black_eyes'] = [nm for nm, key in (('left', 'eye0'), ('right', 'eye1')) if all(is_black(x[key]) for x in rs)]
    r['flicker'] = [nm for nm, key in (('left', 'eye0'), ('right', 'eye1')) if any(is_black(x[key]) for x in rs) and nm not in r['black_eyes']]
    save_shot(cands[best], r, i, spot_name, out)
    return r


# ------------------------------------------------------------------------------------------------ the report
def verdict_for(spot, S_all, nominal_of):
    notes, broken, slow = [], [], []
    img = spot.get('img')
    if img is None:
        broken.append('no screenshot' + (' (the session crashed)' if any(S['crash'] for S in S_all if S['kind'] == 'shots') else ''))
    else:
        l0, l1 = img['eye0'], img['eye1']
        for nm in img.get('black_eyes', []):
            e = l0 if nm == 'left' else l1
            broken.append('%s eye black%s (luma %.1f, %.0f%% black)' % (nm, ' in all %d screenshots' % img['candidates'] if img['candidates'] > 1 else '', e['luma'], 100 * e['black']))
        for nm in img.get('flicker', []):
            notes.append('%s eye black in one of %d screenshots (a transition?)' % (nm, img['candidates']))
        if not img.get('black_eyes') and (l0['luma'] > 12 or l1['luma'] > 12) and abs(l0['luma'] - l1['luma']) > 0.6 * max(l0['luma'], l1['luma']):
            broken.append('the eyes differ a lot in brightness (%.0f vs %.0f)' % (l0['luma'], l1['luma']))
        if img.get('eyes_disagree'):
            msg = 'the eyes show different views in %s (residual %.2f after the best shift)' % ('every screenshot' if img['candidates'] > 1 else 'the screenshot', img['eye_res'])
            (broken if img['candidates'] > 1 else notes).append(msg)
        elif img['disparity_conf'] >= DISP_MIN_CONF:
            if img['disparity_px'] < -1.5:
                broken.append('eyes swapped? disparity %+.0f px (should be positive)' % img['disparity_px'])
            elif abs(img['disparity_px']) < 1.0 and img['eye_diff0'] < 0.8:
                notes.append('no disparity between the eyes (mono?)')
        if 'hud_share' not in img:
            broken.append('no HUD quad in the screenshot frame')
        elif img['hud_share'] < HUD_MIN:
            broken.append('HUD quad empty (%.2f%% drawn)' % (100 * img['hud_share']))
    for S in S_all:
        if S['crash']:
            when = S['crash'][0]
            w = S['windows'].get(spot['idx'])
            last = max(S['windows'].values(), key=lambda x: x[0])[0] if S['windows'] else 0
            if w is None or w[0] <= when <= w[2] or (spot['idx'] not in S['jumps']):
                broken.append('%s: %s' % (S['name'], S['crash'][1]))
        if spot['idx'] not in S['jumps']:
            if not S['crash']:
                broken.append('%s: the warp was not taken' % S['name'])
    for hz, m in spot['hz'].items():
        nom = nominal_of[hz]
        if m is None or not m.get('frames'):
            if not any(('timing_%s' % hz) in b for b in broken):
                broken.append('timing_%s: no gameplay frames in the window' % hz)
            continue
        if m['fps'] < SLOW_FPS * nom:
            slow.append('%s Hz: game %.1f fps (of %d)' % (hz, m['fps'], nom))
        if m.get('sets_per_s') is not None and m['sets_per_s'] < SLOW_SETS * nom:
            slow.append('%s Hz: %.1f sets shown a second (of %d)' % (hz, m['sets_per_s'], nom))
        if m.get('missed_pct') is not None and m['missed_pct'] > SLOW_MISSED:
            slow.append('%s Hz: %.1f%% of refreshes missed' % (hz, m['missed_pct']))
        if m['hitches'] >= SLOW_HITCHES:
            slow.append('%s Hz: %d game frames over 1.5x the period (worst %.0f ms)' % (hz, m['hitches'], m['period_max']))
        if m.get('gpu_both') and m['gpu_both'] > SLOW_GPU * 1000.0 / nom:
            slow.append('%s Hz: GPU %.1f ms for both eyes (period %.1f)' % (hz, m['gpu_both'], 1000.0 / nom))
        if m.get('pose_bad'):
            notes.append('%s Hz: %d pose mismatches of %d refreshes' % (hz, m['pose_bad'], m['pose_of'] or 0))
        if m.get('mock_errors'):
            broken.append('%s Hz: %d xrmock ERROR lines' % (hz, m['mock_errors']))
        if m.get('vuids'):
            broken.append('%s Hz: %d validation messages' % (hz, m['vuids']))
        if m.get('level') and m['level'] not in ('-', spot['level']):
            notes.append('%s Hz: ended in level %s, not %s (the walk left the spot, or a warp was late)' % (hz, m['level'], spot['level']))
        if m.get('xr_partial'):
            notes.append('%s Hz: XR summary only partly in the window' % hz)
        if m.get('compile'):
            notes.append('%s Hz: %d shader compiles' % (hz, m['compile']))
    for t, l in []:
        pass
    v = 'BROKEN' if broken else 'SLOW' if slow else 'OK'
    return v, broken, slow, notes


def build_report(out, keep_raw=False):
    meta = {r[0]: r[1] for r in read_tsv(os.path.join(out, 'meta.tsv'))}
    spots = [{'idx': int(r[0]), 'name': r[1], 'level': r[2], 'loc': r[3]} for r in read_tsv(os.path.join(out, 'spots.tsv'))]
    sess = [analyse_session(r, spots) for r in read_tsv(os.path.join(out, 'sessions.tsv'))]
    shot_S = [S for S in sess if S['kind'] == 'shots']
    timing_S = [S for S in sess if S['kind'] == 'timing']
    vvl_S = [S for S in sess if S['kind'] == 'vvl']
    hzs = []
    for S in timing_S:
        if str(S['hz']) not in hzs:
            hzs.append(str(S['hz']))
    nominal_of = {h: 30 if int(h) < 100 else 60 for h in hzs}
    for spot in spots:
        i = spot['idx']
        spot['hz'] = {}
        for S in timing_S:
            spot['hz'][str(S['hz'])] = window_metrics(S, i, nominal_of[str(S['hz'])])
        spot['vvl'] = None
        for S in vvl_S:
            m = window_metrics(S, i, 30 if S['hz'] < 100 else 60)
            spot['vvl'] = {'vuids': m['vuids'] if m else None, 'unique': sorted({re.search(r'VUID-[\w-]+', l).group(0) for t, l in S['vuids']
                                                                                 if re.search(r'VUID-[\w-]+', l)
                                                                                 and S['windows'].get(i) and S['windows'][i][0] <= t < S['windows'][i][2]})}
        spot['img'] = None
        for S in shot_S:
            r = analyse_spot_shots(S, i, spot['name'], out)
            if r:
                spot['img'] = r
        spot['verdict'], spot['broken'], spot['slow'], spot['notes'] = verdict_for(spot, sess if not vvl_S else [S for S in sess if S['kind'] != 'vvl'], nominal_of)
        if spot['vvl'] and spot['vvl']['vuids']:
            spot['broken'].append('validation: %d messages (%s)' % (spot['vvl']['vuids'], ', '.join(spot['vvl']['unique'][:3])))
            spot['verdict'] = 'BROKEN'
    # the data file
    data = {'meta': meta, 'hzs': hzs, 'nominal': nominal_of,
            'sessions': [{k: (S[k] if k not in ('crash',) else (list(S[k]) if S[k] else None)) for k in
                          ('name', 'kind', 'hz', 'rc', 'wall_s', 'lines') if k in S} | {
                              'crash': list(S['crash']) if S['crash'] else None, 'mock_summary': S.get('mock_summary'), 'csv': S.get('csv'),
                              'mock_wait': S.get('mock_wait'), 'mock_end': S.get('mock_end'),
                              'mock_errors': len(S['mock_errors']), 'vuids': len(S['vuids']),
                              'mock_error_text': sorted({re.sub(r'f\d+', 'f#', l)[:200] for t, l in S['mock_errors']})[:8]} for S in sess],
            'spots': spots}
    json.dump(data, open(os.path.join(out, 'data.json'), 'w'), indent=1)
    sheet(out, spots)
    write_md(out, meta, spots, sess, hzs, nominal_of)
    if not keep_raw:
        for S in sess:
            for p in glob.glob(os.path.join(S['dir'], 'shots', '*.png')):
                os.remove(p)
    return data


def sheet(out, spots):
    tw, th_ = 360, 190
    cells = []
    for s in spots:
        img = s.get('img')
        cell = Image.new('RGB', (1080, th_ * 2 + 22), (18, 18, 22))
        d = ImageDraw.Draw(cell)
        col = {'OK': (110, 230, 120), 'SLOW': (255, 200, 70), 'BROKEN': (255, 90, 90)}[s['verdict']]
        fps = ' '.join('%sHz %s' % (h, f1((m or {}).get('sets_per_s') if (m or {}).get('sets_per_s') is not None else (m or {}).get('fps'))) for h, m in s['hz'].items())
        d.text((6, 5), '%s   %s   %s' % (s['name'], s['verdict'], fps), fill=col)
        if img:
            e = Image.open(os.path.join(out, img['eyes_png']))
            e = e.resize((2 * tw, round(2 * tw * e.height / e.width)))
            cell.paste(e.crop((0, 0, 2 * tw, th_ * 2)), (0, 20))
            if img.get('hud_png'):
                h = Image.open(os.path.join(out, img['hud_png'])).resize((tw, round(tw * 9 / 16)))
                cell.paste(h, (2 * tw, 20))
                d.text((2 * tw + 4, 24 + h.height), 'HUD %.1f%% drawn  disp %+.0f px' % (100 * img.get('hud_share', 0), img['disparity_px']), fill=(200, 200, 200))
        else:
            d.text((10, 60), 'no screenshot', fill=(255, 90, 90))
        cells.append(cell)
    cols = 2 if len(cells) > 1 else 1
    rows = (len(cells) + cols - 1) // cols
    ch = th_ * 2 + 22
    sheet_ = Image.new('RGB', (1080 * cols, ch * rows), (18, 18, 22))
    for k, c in enumerate(cells):
        sheet_.paste(c, ((k % cols) * 1080, (k // cols) * ch))
    sheet_.save(os.path.join(out, 'sheet.png'), optimize=True)


def write_md(out, meta, spots, sess, hzs, nominal_of):
    L = []
    worst = 'BROKEN' if any(s['verdict'] == 'BROKEN' for s in spots) else 'SLOW' if any(s['verdict'] == 'SLOW' for s in spots) else 'OK'
    cnt = {v: sum(1 for s in spots if s['verdict'] == v) for v in ('OK', 'SLOW', 'BROKEN')}
    L.append('# VR tour: %s' % worst)
    L.append('')
    L.append('%s @ %s%s, binary of %s, %s. %d spots: %d OK, %d SLOW, %d BROKEN. Eye %s, mock head pose 35 degrees down with a yaw sway, hearts cheat.' % (
        meta.get('rev', '?'), meta.get('branch', '?'), ' (+%s modified files)' % meta['dirty'] if meta.get('dirty', '0') != '0' else '', meta.get('binary', '?'),
        meta.get('date', '?')[:19], len(spots), cnt['OK'], cnt['SLOW'], cnt['BROKEN'], meta.get('eye', '?')))
    L.append('')
    # session table
    L.append('## Sessions')
    L.append('')
    L.append('| session | exit | wall s | xrmock errors | validation msgs | late / lost refreshes (mock) | crash |')
    L.append('|---|---|---|---|---|---|---|')
    for S in sess:
        c = S.get('csv') or {}
        L.append('| %s | %d | %.0f | %d | %d | %s | %s |' % (S['name'], S['rc'], S['wall_s'], len(S['mock_errors']), len(S['vuids']),
                                                         '%d / %d of %d' % (c['late'], c['lost'], c['frames']) if c else '-', S['crash'][1] if S['crash'] else 'no'))
    for S in sess:
        if S.get('mock_wait'):
            L.append('')
            L.append('%s: %s' % (S['name'], S['mock_wait']))
        for e in sorted({re.sub(r'f\d+', 'f#', l)[:200] for t, l in S['mock_errors']})[:6]:
            L.append('')
            L.append('`%s`' % e)
    L.append('')
    L.append('## Spots')
    L.append('')
    head = ['spot', 'verdict']
    for h in hzs:
        head += ['%s Hz fps (sets/s)' % h, 'missed %', 'GPU ms e0+e1', 'render CPU ms', 'busy ms', 'pose mism.']
    head += ['HUD %', 'disp px']
    L.append('| ' + ' | '.join(head) + ' |')
    L.append('|' + '---|' * len(head))
    for s in spots:
        row = [s['name'], '**%s**' % s['verdict']]
        for h in hzs:
            m = s['hz'].get(h)
            if not m or not m.get('frames'):
                row += ['-'] * 6
                continue
            row += ['%s (%s)' % (f1(m['fps']), f1(m.get('sets_per_s')) + ('~' if m.get('xr_partial') else '')),
                    f1(m.get('missed_pct')),
                    '%s+%s=%s' % (f1(m.get('gpu_e0'), 2), f1(m.get('gpu_e1'), 2), f1(m.get('gpu_both'), 2)) if m.get('gpu_both') is not None else '-',
                    f1(m.get('render_cpu'), 2), f1(m.get('busy'), 2),
                    '%s/%s' % (m['pose_bad'], m['pose_of']) if m.get('pose_bad') is not None else '-']
        img = s.get('img')
        row += [f1(100 * img['hud_share']) if img and 'hud_share' in img else '-', '%+.0f' % img['disparity_px'] if img else '-']
        L.append('| ' + ' | '.join(row) + ' |')
    L.append('')
    L.append('Verdicts: BROKEN = crash, warp not taken, a black eye, swapped eyes, missing HUD, xrmock ERROR or validation message; SLOW = game fps under %.0f%%, '
             'sets shown under %.0f%% of nominal (30 fps below 100 Hz, else 60), more than %.0f%% refreshes missed, %d+ hitches or GPU over %.0f%% of the period.' % (
                 100 * SLOW_FPS, 100 * SLOW_SETS, SLOW_MISSED, SLOW_HITCHES, 100 * SLOW_GPU))
    L.append('')
    L.append('## Why, per spot')
    L.append('')
    for s in spots:
        L.append('### %s: %s' % (s['name'], s['verdict']))
        for b in s['broken']:
            L.append('- BROKEN: %s' % b)
        for b in s['slow']:
            L.append('- SLOW: %s' % b)
        for b in s['notes']:
            L.append('- note: %s' % b)
        for h in hzs:
            m = s['hz'].get(h)
            if m and m.get('frames'):
                ex = ''
                if m.get('draws') is not None:
                    ex = '; per second %.0f draws, %.0f passes, %.0f flushes, %.0f ms waiting, level %s' % (m['draws'] / 2, m['passes'] / 2, m['flushes'] / 2, (m.get('wait_ms') or 0) / 2, m.get('level'))
                L.append('- %s Hz: %d frames in %.0f s; period avg %.2f p95 %.2f max %.1f ms; cpu %s, render cpu %s, gpu(pace) %s, gpu wait %s, game queue wait %s; latch to display %s ms%s' % (
                    int(h), m['frames'], m['secs'], m['period'], m['period_p95'], m['period_max'], f1(m.get('cpu'), 2), f1(m.get('render_cpu'), 2), f1(m.get('gpu'), 2),
                    f1(m.get('gpu_wait'), 2), f1(m.get('queue_wait'), 2), f1(m.get('latch_ms')), ex))
        if s.get('img'):
            i = s['img']
            L.append('- eyes: luma %.0f / %.0f, black %.0f%% / %.0f%%, disparity %+.1f px (confidence %.2f), eye residual %.2f; HUD %s' % (
                i['eye0']['luma'], i['eye1']['luma'], 100 * i['eye0']['black'], 100 * i['eye1']['black'], i['disparity_px'], i['disparity_conf'], i['eye_res'],
                '%.2f%% drawn' % (100 * i['hud_share']) if 'hud_share' in i else 'missing'))
        L.append('')
    L.append('![sheet](sheet.png)')
    L.append('')
    open(os.path.join(out, 'report.md'), 'w').write('\n'.join(L) + '\n')


# ------------------------------------------------------------------------------------------------ compare
def hist_dist(a, b):
    return 0.5 * sum(abs(x - y) for x, y in zip(a, b)) / 3.0


def thumb_gray(path, half):
    im = Image.open(path)
    w = im.width // 2
    im = im.crop((0, 0, w, im.height)) if half == 0 else im.crop((w, 0, 2 * w, im.height))
    return im.convert('L').resize((48, 50), Image.BILINEAR)


def compare(old, new):
    od, nd = json.load(open(os.path.join(old, 'data.json'))), json.load(open(os.path.join(new, 'data.json')))
    oldsp = {s['name']: s for s in od['spots']}
    flags, lines = [], []
    rank = {'OK': 0, 'SLOW': 1, 'BROKEN': 2}
    for s in nd['spots']:
        o = oldsp.get(s['name'])
        if not o:
            lines.append('%s: not in the older tour' % s['name'])
            continue
        f = []
        if rank[s['verdict']] > rank[o['verdict']]:
            f.append('verdict %s -> %s' % (o['verdict'], s['verdict']))
        for h, m in s['hz'].items():
            om = o['hz'].get(h)
            if not om or not m or not om.get('fps') or not m.get('fps'):
                continue
            if m['fps'] < om['fps'] * (1 - FPS_DROP):
                f.append('%s Hz game fps %.1f -> %.1f (%+.0f%%)' % (h, om['fps'], m['fps'], 100 * (m['fps'] / om['fps'] - 1)))
            if om.get('sets_per_s') and m.get('sets_per_s') and m['sets_per_s'] < om['sets_per_s'] * (1 - FPS_DROP):
                f.append('%s Hz sets shown %.1f -> %.1f a second (%+.0f%%)' % (h, om['sets_per_s'], m['sets_per_s'], 100 * (m['sets_per_s'] / om['sets_per_s'] - 1)))
            if om.get('gpu_both') and m.get('gpu_both') and m['gpu_both'] > om['gpu_both'] * 1.25 and m['gpu_both'] - om['gpu_both'] > 0.5:
                f.append('%s Hz GPU both eyes %.2f -> %.2f ms' % (h, om['gpu_both'], m['gpu_both']))
            if (m.get('pose_bad') or 0) > (om.get('pose_bad') or 0) + 5:
                f.append('%s Hz pose mismatches %s -> %s' % (h, om.get('pose_bad'), m['pose_bad']))
        oi, ni = o.get('img'), s.get('img')
        if oi and ni:
            for k in ('eye0', 'eye1'):
                if abs(ni[k]['black'] - oi[k]['black']) > CMP_BLACK:
                    f.append('%s eye black share %.0f%% -> %.0f%%' % ('left' if k == 'eye0' else 'right', 100 * oi[k]['black'], 100 * ni[k]['black']))
                dc = math.dist(ni[k]['mean_rgb'], oi[k]['mean_rgb'])
                if dc > CMP_COLOUR:
                    f.append('%s eye mean colour moved %.0f (%s -> %s)' % ('left' if k == 'eye0' else 'right', dc, oi[k]['mean_rgb'], ni[k]['mean_rgb']))
                hd = hist_dist(ni[k]['hist'], oi[k]['hist'])
                if hd > CMP_HIST:
                    f.append('%s eye colour histogram distance %.2f' % ('left' if k == 'eye0' else 'right', hd))
            try:
                for half in (0, 1):
                    a, b = thumb_gray(os.path.join(old, oi['eyes_png']), half), thumb_gray(os.path.join(new, ni['eyes_png']), half)
                    mad = ImageStat.Stat(ImageChops.difference(a, b)).mean[0]
                    if mad > CMP_MAD:
                        f.append('%s eye image differs a lot (mean abs difference %.0f of 255 at 48 px)' % ('left' if half == 0 else 'right', mad))
            except OSError:
                pass
            if oi['disparity_conf'] >= DISP_MIN_CONF and ni['disparity_conf'] >= DISP_MIN_CONF and oi['disparity_px'] * ni['disparity_px'] < 0 \
                    and max(abs(oi['disparity_px']), abs(ni['disparity_px'])) > 1.5:
                f.append('eye order flipped: disparity %+.1f -> %+.1f px' % (oi['disparity_px'], ni['disparity_px']))
            if 'hud_share' in oi:
                if 'hud_share' not in ni:
                    f.append('HUD quad gone')
                elif oi['hud_share'] > HUD_MIN:
                    r = ni['hud_share'] / oi['hud_share']
                    if r < CMP_HUD:
                        f.append('HUD drawn share %.2f%% -> %.2f%%' % (100 * oi['hud_share'], 100 * ni['hud_share']))
        elif oi and not ni:
            f.append('screenshot missing now')
        if f:
            flags.append((s['name'], f))
            lines.append('- **%s** (%s -> %s): %s' % (s['name'], o['verdict'], s['verdict'], '; '.join(f)))
        else:
            lines.append('- %s: no big change (%s -> %s)' % (s['name'], o['verdict'], s['verdict']))
    head = 'Compared with %s (%s @ %s): %s' % (old, od['meta'].get('rev'), od['meta'].get('branch'), 'FLAGGED %d spot(s)' % len(flags) if flags else 'nothing flagged')
    txt = '## Compare\n\n%s\n\n%s\n' % (head, '\n'.join(lines))
    open(os.path.join(new, 'compare.md'), 'w').write(txt)
    rp = os.path.join(new, 'report.md')
    if os.path.exists(rp):
        body = open(rp).read()
        body = re.sub(r'\n## Compare\n.*?(?=\n!\[sheet\])', '\n', body, flags=re.S)
        body = body.replace('\n![sheet](sheet.png)', '\n' + txt + '\n![sheet](sheet.png)')
        open(rp, 'w').write(body)
    print(txt)
    return 1 if flags else 0


def main(argv):
    if len(argv) >= 3 and argv[1] == 'report':
        out = argv[2].rstrip('/')
        keep = '--keep-raw' in argv
        cmp_ = argv[argv.index('--compare') + 1] if '--compare' in argv else None
        data = build_report(out, keep)
        worst = 'BROKEN' if any(s['verdict'] == 'BROKEN' for s in data['spots']) else 'SLOW' if any(s['verdict'] == 'SLOW' for s in data['spots']) else 'OK'
        print('vr_tour: %s: %s' % (worst, ', '.join('%s %s' % (s['name'], s['verdict']) for s in data['spots'])))
        rc = 0
        if cmp_:
            rc = compare(cmp_.rstrip('/'), out)
        print('vr_tour: report %s/report.md, sheet %s/sheet.png' % (out, out))
        return rc
    if len(argv) == 4 and argv[1] == 'compare':
        return compare(argv[2].rstrip('/'), argv[3].rstrip('/'))
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))

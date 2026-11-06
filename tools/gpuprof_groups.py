#!/usr/bin/env python3
"""gpuprof_groups.py run.log [frame#]: a LARECOMP_GPUPROF_AT frame's [gpuprof] rows (VR mode: e0 / e1 labels) summed
into pass groups per eye (docs/af/PASSES.md). frame#: which profiled frame of the log (0 = first; default all, averaged).
Groups go by the first draw's fragment shader hash (the outdoor field's post chain); rows without a draw by their kind."""
import re, sys
from collections import defaultdict

FS = {
    'd3e83e0301de7e42': 'shadow', '01b5fc5ff41fbb99': 'pre-scene 704x396', '6804ba6e5340c6f9': 'pre-scene 704x396',
    'd853b93d3755364b': 'depth prepass', 'f0b2d796cbb7ffce': 'main colour', 'a0045fbabd2b2f1d': 'main colour',
    '2a22219a6c942b1e': 'main colour', 'd7fe0c3e3c480af7': 'AO', 'bbb2410a6d696a78': 'AO apply',
    '7b537cfc9b0c1937': 'DOF (tilt-shift)', '86a3fa6a2a33df54': 'DOF (tilt-shift)', 'f2f5df1c1be118a0': 'bloom',
    '6d74240865f058e4': 'bloom', '5bcc5bba74542b30': 'bloom composite', 'ca3fbe0c31ec2974': 'tonemap -> eye output',
}

def group(label, ms):
    m = re.search(r'first fs ([0-9a-f]{16})', label)
    if m:
        h = m.group(1)
        if h == '589f0038ffc39255':  # separable blur: AO's (R8, fmt 0x1) or bloom's (fmt 0x3f)
            return 'AO' if 'fmt 0x1,' in label else 'bloom'
        if 'fmt 0x29' in label: return 'HUD'
        return FS.get(h, 'other draws')
    if label.startswith('dispatch') or ' dispatch' in label: return 'compute (eye 0)'
    if 'copy texture' in label:
        return 'AO' if '-> 960x540' in label or '-> 480x270' in label else 'scene copies'
    if 'feedback snapshot' in label: return 'AO'
    if 'submission start' in label: return None
    if 'pass:' in label: return 'idle gap' if ms > 0.5 else 'empty passes (clears)'
    return 'other'

yield_list = []
cur = None
for ln in open(sys.argv[1], errors='replace'):
    if '[gpuprof]' not in ln: continue
    if re.search(r'\[gpuprof\] \d+ marks', ln): cur = []; yield_list.append(cur); continue
    m = re.match(r'\[gpuprof\]\s+([\d.]+) ms\s+[\d.]+%\s+(.*)', ln.strip())
    if m and cur is not None: cur.append((float(m.group(1)), m.group(2)))
sel = yield_list if len(sys.argv) < 3 else [yield_list[int(sys.argv[2])]]
tot = defaultdict(lambda: [0.0, 0.0, 0, 0])  # ms e0, ms e1, passes e0, passes e1
eye = 0
for fr in sel:
    eye = 0
    for ms, lb in fr:
        m = re.match(r'e(-?\d) ', lb)
        if m: eye = 1 if m.group(1) == '1' else 0
        g = group(lb, ms)
        if g is None: continue
        t = tot[g]
        t[eye] += ms / len(sel)
        if 'pass:' in lb: t[2 + eye] += 1 / len(sel)
order = sorted(tot, key=lambda g: -(tot[g][0] + tot[g][1]))
print(f"{'group':24} {'eye0 ms':>8} {'eye1 ms':>8} {'both':>7} {'passes e0':>9} {'e1':>4}")
s = [0, 0, 0, 0]
for g in order:
    t = tot[g]
    print(f"{g:24} {t[0]:8.2f} {t[1]:8.2f} {t[0]+t[1]:7.2f} {t[2]:9.0f} {t[3]:4.0f}")
    for i in range(4): s[i] += t[i]
print(f"{'total':24} {s[0]:8.2f} {s[1]:8.2f} {s[0]+s[1]:7.2f} {s[2]:9.0f} {s[3]:4.0f}  ({len(sel)} frame(s))")

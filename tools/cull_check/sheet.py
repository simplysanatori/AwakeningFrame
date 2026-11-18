#!/usr/bin/env python3
"""sheet.py <out.png> <yaw> <tilt> <head> <eye> <dir> [<dir> ...]: one row of eye images of the same view case, one per
session directory (each session's own dump of that case), labelled with the directory name."""
import os, sys
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report as cr, glob, re
out, yaw, tilt, head, eye = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
k = cr.CASES.index((yaw, tilt, head))
ims = []
for d in sys.argv[6:]:
    L = cr.parse(d); fc = cr.frames_case(L)
    fs = [int(re.search(r'stereo_(\d+)_eye0', p)[1]) for p in sorted(glob.glob(os.path.join(d, 'stereo_*_eye0.png')))]
    fs = [F for F in fs if cr.dump_case(fc, F) == k]
    if not fs:
        ims.append((d, None)); continue
    ims.append((d, Image.open(os.path.join(d, 'stereo_%05d_eye%d.png' % (fs[-1], eye))).convert('RGB').resize((640, 640), Image.LANCZOS)))
W = 648 * len(ims) - 8
s = Image.new('RGB', (W, 664), (40, 40, 40))
dr = ImageDraw.Draw(s)
for i, (d, im) in enumerate(ims):
    if im: s.paste(im, (i * 648, 24))
    dr.text((i * 648 + 4, 6), '%s  (table yaw %d, tilt %d, head %s, eye %d)%s' % (os.path.basename(d.rstrip('/')), yaw, tilt, head, eye, '' if im else '  NO DUMP'), fill=(255, 255, 255))
s.save(out)

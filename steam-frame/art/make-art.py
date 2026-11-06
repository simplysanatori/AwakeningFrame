#!/usr/bin/env python3
"""Builds the Frame version of the library art from the SteamGridDB originals listed in SOURCES.md.
   python3 make-art.py SRC OUT      (needs Pillow)
 SRC holds the downloaded originals named p.png, wide.png, hero.png, logo.png, icon.png (the Nintendo artwork is not
 committed here, see SOURCES.md). OUT gets the same names, ready for set-art.py or for steam-frame/art/ before install.sh.
 The Frame touch: a "FRAME" pill on the portrait and the wide capsule, and a letterspaced FRAME line under the logo
 (the same place the other ports' logos carry PRIME, LEGEND, COMBAT EVOLVED). Hero and icon stay as they are."""
import os, subprocess, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter

SRC, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)
FB = subprocess.check_output(["fc-match", "-f", "%{file}", "DejaVu Sans:bold"]).decode()

def flat(path):
    im = Image.open(path)
    if im.mode == "RGBA":
        bg = Image.new("RGB", im.size, (0, 0, 0)); bg.paste(im, mask=im.getchannel("A")); return bg
    return im.convert("RGB")

def spaced(d, xy, txt, font, fill, sp):
    ws = [d.textlength(c, font=font) for c in txt]
    x = xy[0] - (sum(ws) + sp * (len(txt) - 1)) / 2
    for c, w in zip(txt, ws):
        d.text((x, xy[1]), c, font=font, fill=fill, anchor="lm"); x += w + sp
    return sum(ws) + sp * (len(txt) - 1)

def badge(im, cx, cy, k):
    im = im.convert("RGBA"); layer = Image.new("RGBA", im.size, (0, 0, 0, 0)); d = ImageDraw.Draw(layer)
    f = ImageFont.truetype(FB, int(30 * k)); sp = int(11 * k)
    tw = sum(d.textlength(c, font=f) for c in "FRAME") + sp * 4
    pw, ph = tw + 54 * k, 58 * k
    box = (cx - pw / 2, cy - ph / 2, cx + pw / 2, cy + ph / 2)
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((box[0], box[1] + 4 * k, box[2], box[3] + 4 * k), radius=ph / 2, fill=(0, 0, 0, 110))
    im = Image.alpha_composite(im, sh.filter(ImageFilter.GaussianBlur(6 * k)))
    d.rounded_rectangle(box, radius=ph / 2, fill=(12, 34, 56, 215), outline=(255, 255, 255, 235), width=max(2, int(2.5 * k)))
    spaced(d, (cx, cy + k), "FRAME", f, (255, 255, 255, 255), sp)
    return Image.alpha_composite(im, layer).convert("RGB")

p = flat(os.path.join(SRC, "p.png")); badge(p, p.width / 2, p.height - 52, 1.0).save(os.path.join(OUT, "p.png"))
w = flat(os.path.join(SRC, "wide.png")); badge(w, w.width - 100, w.height - 40, 0.75).save(os.path.join(OUT, "wide.png"))
flat(os.path.join(SRC, "hero.png")).save(os.path.join(OUT, "hero.png"))
flat(os.path.join(SRC, "icon.png")).resize((512, 512), Image.LANCZOS).save(os.path.join(OUT, "icon.png"))

lg = Image.open(os.path.join(SRC, "logo.png")).convert("RGBA"); W0, H0 = lg.size
cv = Image.new("RGBA", (W0, H0 + 260), (0, 0, 0, 0)); cv.paste(lg, (0, 0))
cy = H0 + 130; f = ImageFont.truetype(FB, 150)
def line(layer, col, dy):
    d = ImageDraw.Draw(layer); tw = spaced(d, (W0 / 2, cy + dy), "FRAME", f, col, 70)
    for s in (-1, 1):
        d.line([(W0 / 2 + s * (tw / 2 + 70), cy + dy), (W0 / 2 + s * (W0 / 2 - 30), cy + dy)], fill=col, width=12)
sh = Image.new("RGBA", cv.size, (0, 0, 0, 0)); line(sh, (10, 25, 45, 220), 4)
line(cv, (255, 255, 255, 255), 0)
Image.alpha_composite(sh.filter(ImageFilter.GaussianBlur(9)), cv).save(os.path.join(OUT, "logo.png"))
print("art written to " + OUT)

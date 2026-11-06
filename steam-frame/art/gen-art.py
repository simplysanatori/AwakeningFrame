#!/usr/bin/env python3
"""Placeholder library art for the Steam shortcut: original shapes and the app's name only, no game artwork.
Writes p.svg (600x900 portrait), wide.svg (920x430), hero.svg (3840x1240), logo.svg (1280x720, transparent),
icon.svg (512x512, full bleed) and, when rsvg-convert is there, the .png of each.   python3 gen-art.py"""
import os, shutil, subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
BG = ('<defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#101c33"/>'
      '<stop offset="1" stop-color="#1f4a52"/></linearGradient>'
      '<radialGradient id="glow" cx="0.5" cy="0.6" r="0.6"><stop offset="0" stop-color="#4fd1b5" stop-opacity="0.35"/>'
      '<stop offset="1" stop-color="#4fd1b5" stop-opacity="0"/></radialGradient></defs>')
FONT = 'font-family="DejaVu Sans, Helvetica, Arial, sans-serif" font-weight="bold"'

def plate(cx, cy, w):
    """An isometric tabletop slab with a few blocks and a round tree on it, centred at cx, cy, w wide."""
    h = w / 2          # top face height
    t = w * 0.12       # slab thickness
    def P(*pts): return " ".join("%.1f,%.1f" % p for p in pts)
    L, R, T, B = (cx - w / 2, cy), (cx + w / 2, cy), (cx, cy - h / 2), (cx, cy + h / 2)
    out = ['<polygon points="%s" fill="#14352f"/>' % P(L, B, (B[0], B[1] + t), (L[0], L[1] + t)),
           '<polygon points="%s" fill="#0d2420"/>' % P(R, B, (B[0], B[1] + t), (R[0], R[1] + t)),
           '<polygon points="%s" fill="#3fae7a"/>' % P(L, T, R, B)]
    def pt(u, v):
        x = cx + (u - v) * w / 2
        y = cy - h / 2 + (u + v) * h / 2
        return (x, y)
    def block(u, v, s, ht, top, left, right):
        a, b, c, d = pt(u, v), pt(u + s, v), pt(u + s, v + s), pt(u, v + s)
        up = lambda p: (p[0], p[1] - ht)
        out.append('<polygon points="%s" fill="%s"/>' % (P(d, c, up(c), up(d)), left))
        out.append('<polygon points="%s" fill="%s"/>' % (P(c, b, up(b), up(c)), right))
        out.append('<polygon points="%s" fill="%s"/>' % (P(up(a), up(b), up(c), up(d)), top))
    block(0.18, 0.55, 0.22, w * 0.10, "#e8d9a8", "#c9b77f", "#a8975f")
    block(0.62, 0.22, 0.18, w * 0.06, "#d9e3ea", "#aebcc8", "#8fa0ae")
    tx, ty = pt(0.6, 0.62)
    out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#6b4a2b"/>' % (tx - w * 0.012, ty - w * 0.10, w * 0.024, w * 0.10))
    out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#1f7a4d"/>' % (tx, ty - w * 0.15, w * 0.085))
    out.append('<circle cx="%.1f" cy="%.1f" r="%.1f" fill="#2b9a62"/>' % (tx - w * 0.02, ty - w * 0.17, w * 0.06))
    return "\n".join(out)

def svg(w, h, body, bg=True):
    return ('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s%s%s</svg>\n'
            % (w, h, w, h, BG, ('<rect width="%d" height="%d" fill="url(#g)"/><rect width="%d" height="%d" fill="url(#glow)"/>' % (w, h, w, h)) if bg else "", body))

def title(x, y, size, anchor="middle", sub=None):
    s = ('<text x="%d" y="%d" %s font-size="%d" text-anchor="%s" fill="#f4f7fa">Awakening<tspan fill="#6fe3c1">Frame</tspan></text>'
         % (x, y, FONT, size, anchor))
    if sub:
        s += '<text x="%d" y="%d" font-family="DejaVu Sans, Helvetica, Arial, sans-serif" font-size="%d" text-anchor="%s" fill="#b9c9d6" letter-spacing="%d">%s</text>' % (
            x, y + size * 0.55, size * 0.34, anchor, size * 0.05, sub)
    return s

files = {
    "p": svg(600, 900, plate(300, 470, 520) + title(300, 190, 58, sub="A VR DIORAMA")),
    "wide": svg(920, 430, plate(720, 270, 300) + title(50, 215, 56, "start", "A VR DIORAMA")),
    "hero": svg(3840, 1240, plate(2900, 760, 1300) + '<rect width="3840" height="1240" fill="#0a1424" opacity="0.15"/>'),
    "logo": svg(1280, 720, title(640, 360, 190, sub="A VR DIORAMA"), bg=False),
    "icon": svg(512, 512, plate(256, 300, 400) + title(256, 118, 56)),
}
rsvg = shutil.which("rsvg-convert")
for name, text in files.items():
    path = os.path.join(HERE, name + ".svg")
    open(path, "w").write(text)
    if rsvg:
        subprocess.run([rsvg, path, "-o", os.path.join(HERE, name + ".png")], check=True)
print("wrote", ", ".join(sorted(files)), "" if rsvg else "(no rsvg-convert: SVG only)")

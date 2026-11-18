#!/usr/bin/env python3
"""Flicker in a run of consecutive eye images (xrmock MOCK_XR_FLICK; docs/af/TOOLING.md "Flicker").

    tools/flicker.py footprint <dump dir> <out.npz>         the table's footprint from a run with Surroundings and Table
                                                             Edge off (where the eye is drawn at all), with the pose it is in
    tools/flicker.py score <dump dir> --footprint <npz> [--out DIR] [--eye 0] [--thr 24] [--json FILE]

Flicker is a pixel that changes and comes back: within three consecutive images a, b, c of one eye, its brightness
(luma, 0-255) goes up by more than --thr from a to b and down by more than --thr from b to c, or down and then up. A
steady picture changes smoothly, even under head motion; flicker reverses. Each image is first brought into the
footprint's view by a rotation from the pose it was submitted with (rotation only: the eyes' few centimetres of travel
are left, which is why the static-head runs are the ones to trust and a moving run is only compared with another
moving run of the same trace). Pixels outside an image's view, or past the eye where any of the three is missing, do
not count; triples across a dropped image are not formed.

Regions, in the footprint's view: edge (a band along the footprint's outline, --inner of the image width inside it and
--ring outside: a fade darkens the table inside the outline, Surroundings draw past it), table (the footprint inside that
band), far (the rest). Per region: the share of its pixels that flicker, averaged over the triples (mean), its 99th percentile and
maximum over the triples, and the same share counted against the region's drawn pixels only (not black in any of the
three: a black void cannot flicker, so a mode that draws the far area would otherwise look worse for drawing at all).
Also per region: the share of it drawn at all (not black), mean, least and most over the images: a fill that
comes and goes, or comes out in fragments, shows there and not as flicker when it changes slower than three images.
--out writes strip.png: the worst triples (a | b | c | b with its flickering pixels in red), and series.tsv.
"""
import argparse
import json
import math
import os
import sys

import numpy as np
from PIL import Image, ImageDraw


def read_index(d):
    imgs, drops, meta = {}, [], {}
    with open(os.path.join(d, "index.txt")) as f:
        for line in f:
            w = line.split()
            if not w:
                continue
            kv = dict(x.split("=", 1) for x in w[1:] if "=" in x)
            if w[0] == "img":
                if len(kv.get("fov", "").split(",")) != 4 or not line.endswith("\n"):
                    continue  # (a line cut short where the app ended)
                e, s = int(kv["eye"]), int(kv["seq"])
                imgs.setdefault(e, {})[s] = {
                    "t": float(kv["t"]), "frame": int(kv["frame"]), "w": int(kv["w"]), "h": int(kv["h"]),
                    "pos": [float(v) for v in kv["pos"].split(",")], "quat": [float(v) for v in kv["quat"].split(",")],
                    "fov": [float(v) for v in kv["fov"].split(",")],
                    "file": os.path.join(d, "e%d_%06d.pgm" % (e, s))}
            elif w[0] == "drop":
                drops.append((int(kv["eye"]), int(kv["seq"])))
            elif w[0] in ("begin", "end"):
                meta.update(kv)
    return imgs, drops, meta


def load(p):
    return np.asarray(Image.open(p), dtype=np.float32)


def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])


def rays(fov, w, h):
    """Each pixel centre's view-space direction (x right, y up, -z forward), rows from the top."""
    l, r, u, d = (math.tan(a) for a in fov)
    x = l + (np.arange(w) + 0.5) / w * (r - l)
    y = u - (np.arange(h) + 0.5) / h * (u - d)
    X, Y = np.meshgrid(x, y)
    return np.stack([X, Y, -np.ones_like(X)], -1)


class Reg:
    """Warps images into a reference view (rotation only), cached per pose."""

    def __init__(self, ref):
        self.ref = ref
        self.R = qmat(ref["quat"])
        self.d = rays(ref["fov"], ref["w"], ref["h"]) @ self.R.T  # reference pixels' world directions
        self.cache = {}

    def coords(self, im):
        key = tuple(im["quat"]) + tuple(im["fov"]) + (im["w"], im["h"])
        if key in self.cache:
            return self.cache[key]
        if np.allclose(im["quat"], self.ref["quat"], atol=1e-6) and im["fov"] == self.ref["fov"] and (im["w"], im["h"]) == (self.ref["w"], self.ref["h"]):
            c = None  # the same view: no warp
        else:
            v = self.d @ qmat(im["quat"])  # world -> this image's view (R^T applied to row vectors)
            l, r, u, dn = (math.tan(a) for a in im["fov"])
            with np.errstate(divide="ignore", invalid="ignore"):
                tx, ty = v[..., 0] / -v[..., 2], v[..., 1] / -v[..., 2]
            px = (tx - l) / (r - l) * im["w"] - 0.5
            py = (u - ty) / (u - dn) * im["h"] - 0.5
            ok = (v[..., 2] < 0) & (px >= 0) & (py >= 0) & (px <= im["w"] - 1) & (py <= im["h"] - 1)
            c = (np.where(ok, px, 0), np.where(ok, py, 0), ok)
        if len(self.cache) > 4096:
            self.cache.clear()
        self.cache[key] = c
        return c

    def warp(self, im, a):
        c = self.coords(im)
        if c is None:
            return a, np.ones(a.shape, bool)
        px, py, ok = c
        x0, y0 = np.floor(px).astype(int), np.floor(py).astype(int)
        x1, y1 = np.minimum(x0 + 1, a.shape[1] - 1), np.minimum(y0 + 1, a.shape[0] - 1)
        fx, fy = px - x0, py - y0
        v = (a[y0, x0] * (1 - fx) * (1 - fy) + a[y0, x1] * fx * (1 - fy) + a[y1, x0] * (1 - fx) * fy + a[y1, x1] * fx * fy)
        return np.where(ok, v, 0), ok


def dilate(m, r):
    """Binary dilation by a disc of radius r (pixels)."""
    out = m.copy()
    H, W = m.shape
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            if dx * dx + dy * dy > r * r:
                continue
            src = m[max(0, -dy):H - max(0, dy), max(0, -dx):W - max(0, dx)]
            out[max(0, dy):H - max(0, -dy), max(0, dx):W - max(0, -dx)] |= src
    return out


def fill_holes(m):
    im = Image.fromarray(np.where(m, 255, 0).astype(np.uint8)).copy()  # (a copy: floodfill does not write through to the array)
    H, W = m.shape
    ImageDraw.floodfill(im, (0, 0), 128)  # the outside, reached from a corner (the footprint does not touch it)
    for c in ((W - 1, 0), (0, H - 1), (W - 1, H - 1)):
        if im.getpixel(c) == 0:
            ImageDraw.floodfill(im, c, 128)
    return np.asarray(im) != 128


def footprint(args):
    imgs, _, _ = read_index(args.dir)
    seq = sorted(imgs.get(args.eye, {}))
    if not seq:
        sys.exit("flicker: no images of eye %d in %s" % (args.eye, args.dir))
    ref = imgs[args.eye][seq[0]]
    reg = Reg(ref)
    drawn = np.zeros((ref["h"], ref["w"]), np.float32)
    seen = np.zeros_like(drawn)
    for s in seq:
        im = imgs[args.eye][s]
        a, ok = reg.warp(im, load(im["file"]))
        drawn += (a > args.black) & ok
        seen += ok
    share = drawn / np.maximum(seen, 1)
    m = share > 0.5
    # the table is one blob: keep its outline (small holes, a black roof, a shadow are still the table)
    m = fill_holes(dilate(m, 2))
    np.savez_compressed(args.out_npz, mask=m, quat=np.array(ref["quat"]), fov=np.array(ref["fov"]), w=ref["w"], h=ref["h"],
                        pos=np.array(ref["pos"]))
    Image.fromarray(np.where(m, 255, 0).astype(np.uint8)).save(os.path.splitext(args.out_npz)[0] + ".png")
    print("flicker: footprint %.1f%% of the eye (%d images)" % (100 * m.mean(), len(seq)))


def score(args):
    imgs, drops, meta = read_index(args.dir)
    fp = np.load(args.footprint)
    ref = {"quat": list(fp["quat"]), "fov": list(fp["fov"]), "w": int(fp["w"]), "h": int(fp["h"])}
    foot = fp["mask"].astype(bool)
    ring = max(2, int(round(args.ring * ref["w"])))
    inner = max(2, int(round(args.inner * ref["w"])))
    # the edge straddles the footprint's outline: a fade darkens the table inside it, Surroundings draw past it
    table = ~dilate(~foot, inner)
    edge = dilate(foot, ring) & ~table
    far = ~(table | edge)
    regions = {"table": table, "edge": edge, "far": far}
    reg = Reg(ref)
    ims = imgs.get(args.eye, {})
    seq = sorted(ims)
    if args.skip:
        t0 = ims[seq[0]]["t"] if seq else 0
        seq = [s for s in seq if ims[s]["t"] >= t0 + args.skip]
    thr = args.thr
    cache = {}

    def get(s):
        if s not in cache:
            im = ims[s]
            if (im["w"], im["h"]) != (ref["w"], ref["h"]) and reg.coords(im) is None:
                raise SystemExit("flicker: image size differs from the footprint's")
            cache[s] = reg.warp(im, load(im["file"]))
            for k in [k for k in cache if k < s - 2]:
                del cache[k]
        return cache[s]

    rows, worst = [], []
    for s in seq:
        if s + 1 not in ims or s + 2 not in ims:
            continue
        (a, oa), (b, ob), (c, oc) = get(s), get(s + 1), get(s + 2)
        ok = oa & ob & oc
        up = (b - a > thr) & (b - c > thr)
        dn = (a - b > thr) & (c - b > thr)
        fl = (up | dn) & ok
        lit = (np.maximum(np.maximum(a, b), c) > args.black) & ok
        row = {"seq": s + 1, "t": ims[s + 1]["t"], "frame": ims[s + 1]["frame"]}
        for name, m in regions.items():
            v, d = (m & ok).sum(), (m & lit).sum()
            n = (fl & m).sum()
            row[name] = n / v if v else float("nan")
            row[name + "_drawn"] = n / d if d else float("nan")
            row[name + "_px"] = int(n)
            row[name + "_lit"] = float((m & ok & (b > args.black)).sum() / v) if v else float("nan")  # (coverage: drawn in b)
        rows.append(row)
        tot = row["table_px"] + row["edge_px"] + row["far_px"]
        worst.append((tot, s, fl))
        worst.sort(key=lambda x: -x[0])
        del worst[args.strip:]
    if not rows:
        sys.exit("flicker: no three consecutive images of eye %d in %s" % (args.eye, args.dir))
    res = {"dir": args.dir, "eye": args.eye, "images": len(seq), "triples": len(rows), "drops": len(drops),
           "seconds": rows[-1]["t"] - rows[0]["t"], "thr": thr, "ring_px": ring,
           "area": {k: float(m.mean()) for k, m in regions.items()}}
    for name in regions:
        v = np.array([r[name] for r in rows], float)
        dv = np.array([r[name + "_drawn"] for r in rows], float)
        v, dv = v[~np.isnan(v)], dv[~np.isnan(dv)]
        lv = np.array([r[name + "_lit"] for r in rows], float)
        lv = lv[~np.isnan(lv)]
        res[name] = {"mean": float(v.mean()) if len(v) else None, "p99": float(np.percentile(v, 99)) if len(v) else None,
                     "max": float(v.max()) if len(v) else None,
                     "drawn_mean": float(dv.mean()) if len(dv) else None,
                     "frames_over_0.1pct": int((v > 0.001).sum()),
                     # how much of the region is drawn at all (a Backdrop fill that comes and goes, or fragments)
                     "lit_mean": float(lv.mean()) if len(lv) else None, "lit_min": float(lv.min()) if len(lv) else None,
                     "lit_max": float(lv.max()) if len(lv) else None}
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        with open(os.path.join(args.out, "series.tsv"), "w") as f:
            keys = list(rows[0])
            f.write("\t".join(keys) + "\n")
            for r in rows:
                f.write("\t".join(("%.6g" % r[k]) if isinstance(r[k], float) else str(r[k]) for k in keys) + "\n")
        tiles = []
        for tot, s, fl in sorted(worst, key=lambda x: x[1]):
            (a, _), (b, _), (c, _) = (reg.warp(ims[k], load(ims[k]["file"])) for k in (s, s + 1, s + 2))
            red = np.stack([b, b, b], -1)
            red[fl] = (255, 0, 0)
            edge_line = dilate(foot, 1) & ~foot
            red[edge_line & ~fl] = (0, 160, 255)  # (the footprint's outline in blue)
            row = np.concatenate([np.stack([x] * 3, -1) for x in (a, b, c)] + [red], 1)
            tiles.append((row, "seq %d-%d  t %.2f s  %d px flicker" % (s, s + 2, ims[s + 1]["t"], tot)))
        if tiles:
            H = tiles[0][0].shape[0] + 14
            strip = Image.new("RGB", (tiles[0][0].shape[1], H * len(tiles)))
            dr = ImageDraw.Draw(strip)
            for i, (t, label) in enumerate(tiles):
                strip.paste(Image.fromarray(np.clip(t, 0, 255).astype(np.uint8)), (0, i * H + 14))
                dr.text((4, i * H + 1), label + "   (a | b | c | b, flickering pixels red, footprint outline blue)", fill=(255, 255, 0))
            strip.save(os.path.join(args.out, "strip.png"))
    pc = lambda v: "-" if v is None else "%.4f%%" % (100 * v)
    line = "  ".join("%s %s (p99 %s, max %s; of drawn %s; drawn %s, %s-%s)" % (
        k, pc(res[k]["mean"]), pc(res[k]["p99"]), pc(res[k]["max"]), pc(res[k]["drawn_mean"]), pc(res[k]["lit_mean"]), pc(res[k]["lit_min"]),
        pc(res[k]["lit_max"])) for k in regions)
    print("flicker: eye %d, %d images, %d triples over %.1f s, %d dropped:  %s" % (args.eye, res["images"], res["triples"],
                                                                                   res["seconds"], res["drops"], line))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(res, f, indent=1)
    return res


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("footprint")
    a.add_argument("dir")
    a.add_argument("out_npz")
    a.add_argument("--eye", type=int, default=0)
    a.add_argument("--black", type=float, default=6, help="luma at or below this is the void (6)")
    a = sub.add_parser("score")
    a.add_argument("dir")
    a.add_argument("--footprint", required=True)
    a.add_argument("--eye", type=int, default=0)
    a.add_argument("--thr", type=float, default=24, help="the change that counts, luma of 255 (24)")
    a.add_argument("--black", type=float, default=6, help="luma at or below this is the void (6)")
    a.add_argument("--ring", type=float, default=0.05, help="the edge band's width outside the footprint, share of the image width (0.05)")
    a.add_argument("--inner", type=float, default=0.08, help="... and inside it (0.08)")
    a.add_argument("--skip", type=float, default=0, help="leave out the first S seconds of images")
    a.add_argument("--strip", type=int, default=4, help="worst triples in strip.png (4)")
    a.add_argument("--out")
    a.add_argument("--json")
    args = ap.parse_args()
    footprint(args) if args.cmd == "footprint" else score(args)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""A head on a neck, for xrmock's MOCK_XR_POSE=file: and MOCK_XR_GAZE=file (docs/af/TOOLING.md).

    tools/xrmock/headpose.py out.pose [--seed N] [--seconds S] [--still] [--base x,y,z,yaw,pitch]

Writes "t x y z yaw pitch roll gaze_yaw gaze_pitch" lines (seconds, metres, degrees; the gaze relative to the head) at
--rate lines a second, the format the game's LARECOMP_XR_POSE_LOG records, so a recording and a made-up head play the same
way. The mock's default sway is slow and smooth; a head is not:

- never still: a tremor of a fraction of a degree (filtered noise, 2-12 Hz) and a postural sway of the body of a few
  millimetres to a centimetre (0.1-0.6 Hz);
- turns: every second or so a turn to look somewhere else on or around the table, with a minimum-jerk profile whose peak
  speed is drawn up to --peak degrees a second (200), now and then a wide fast look away and back;
- nods: a quick down-and-up of a few degrees every few seconds;
- the eyes sit --neck metres in front of and --up metres above the pivot in the neck, so every rotation moves them too.

The gaze fixates points in the world (it counter-rotates while the head turns, as eyes do) and jumps to a new one with a
saccade every 0.2-0.5 s, with a small fixation jitter. --still: the head held at --base (no tremor), only the gaze moving.
Seeded: the same arguments give the same file.
"""
import argparse
import math
import random


def min_jerk(s):
    s = min(max(s, 0.0), 1.0)
    return s * s * s * (10 - 15 * s + 6 * s * s)


class Noise:
    """Band-limited noise: white noise through two one-pole low-passes, minus a slower one (a band-pass)."""

    def __init__(self, rng, lo_hz, hi_hz, rate):
        self.rng = rng
        self.a_hi = 1 - math.exp(-2 * math.pi * hi_hz / rate)
        self.a_lo = 1 - math.exp(-2 * math.pi * lo_hz / rate)
        self.h1 = self.h2 = self.l = 0.0

    def step(self):
        x = self.rng.gauss(0, 1)
        self.h1 += self.a_hi * (x - self.h1)
        self.h2 += self.a_hi * (self.h1 - self.h2)
        self.l += self.a_lo * (self.h2 - self.l)
        return self.h2 - self.l


def calibrate(make, n, rate):
    """The RMS of a noise source, to scale it to an amplitude."""
    src = make()
    xs = [src.step() for _ in range(int(n * rate))]
    return math.sqrt(sum(x * x for x in xs) / len(xs)) or 1.0


def rot(yaw, pitch, roll, v):
    """v rotated by yaw about +y, then pitch about x, then roll about z (xrmock's qeuler order; degrees)."""
    y, p, r = (math.radians(a) for a in (yaw, pitch, roll))
    x0, y0, z0 = v
    x1, y1 = x0 * math.cos(r) - y0 * math.sin(r), x0 * math.sin(r) + y0 * math.cos(r)  # roll about z
    y2, z2 = y1 * math.cos(p) - z0 * math.sin(p), y1 * math.sin(p) + z0 * math.cos(p)  # pitch about x
    return (x1 * math.cos(y) + z2 * math.sin(y), y2, -x1 * math.sin(y) + z2 * math.cos(y))  # yaw about y


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--rate", type=float, default=240, help="lines a second (240)")
    ap.add_argument("--base", default="0,1.6,0,0,-30", help="the eyes' rest pose: x,y,z (m), yaw, pitch (degrees)")
    ap.add_argument("--still", action="store_true", help="the head held at --base, only the gaze moving")
    ap.add_argument("--neck", type=float, default=0.10, help="eyes in front of the neck's pivot (m)")
    ap.add_argument("--up", type=float, default=0.07, help="eyes above the neck's pivot (m)")
    ap.add_argument("--peak", type=float, default=200, help="fastest turn (degrees a second)")
    ap.add_argument("--reach", default="30,14", help="how far turns go from the rest, yaw,pitch degrees")
    ap.add_argument("--tremor", type=float, default=0.25, help="tremor, RMS degrees")
    ap.add_argument("--sway", type=float, default=0.004, help="body sway, RMS metres")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    bx, by, bz, byaw, bpitch = (float(v) for v in a.base.split(","))
    ryaw, rpitch = (float(v) for v in a.reach.split(","))
    rate, n = a.rate, int(a.seconds * a.rate) + 1
    dt = 1.0 / rate
    off = (0.0, a.up, -a.neck)  # the eyes from the pivot, in the head's frame
    o = rot(byaw, bpitch, 0, off)
    neck = (bx - o[0], by - o[1], bz - o[2])  # the pivot, placed so the eyes are at --base at rest

    # noise sources, scaled to their RMS amplitudes (the calibration uses its own generator: the file stays seeded)
    crng = random.Random(a.seed + 7919)
    tr_rms = calibrate(lambda: Noise(crng, 2.0, 12.0, rate), 20, rate)
    sw_rms = calibrate(lambda: Noise(crng, 0.1, 0.6, rate), 60, rate)
    tremor = [Noise(rng, 2.0, 12.0, rate) for _ in range(3)]
    sway = [Noise(rng, 0.1, 0.6, rate) for _ in range(3)]

    # the turn schedule: (start, duration, from yaw, from pitch, to yaw, to pitch), offsets from the rest
    turns, t, cy, cp = [], 0.5, 0.0, 0.0
    while t < a.seconds:
        if rng.random() < 0.15:  # a wide fast look away, then back
            ty, tp = rng.choice((-1, 1)) * rng.uniform(ryaw, 1.8 * ryaw), rng.uniform(-rpitch, rpitch * 0.5)
        else:
            ty, tp = rng.uniform(-ryaw, ryaw), rng.uniform(-rpitch, rpitch)
        amp = math.hypot(ty - cy, tp - cp)
        vpk = rng.uniform(0.35, 1.0) * a.peak  # peak speed of this turn
        dur = max(0.12, 1.875 * amp / vpk)  # (a minimum-jerk move peaks at 1.875 x its mean speed)
        turns.append((t, dur, cy, cp, ty, tp))
        cy, cp = ty, tp
        t += dur + rng.expovariate(1 / 0.9)
    nods, t = [], rng.uniform(1, 4)
    while t < a.seconds:
        f = rng.uniform(1.5, 3.0)
        nods.append((t, rng.randint(1, 2) / f, f, rng.uniform(3, 8)))  # start, length, Hz, degrees
        t += rng.uniform(2.5, 6.0)

    # the gaze: world fixation points (yaw, pitch about the rest), a saccade every 0.2-0.5 s
    fix, t = [], 0.0
    while t < a.seconds + 1:
        fix.append((t, rng.uniform(-ryaw - 8, ryaw + 8), rng.uniform(-rpitch - 8, rpitch + 6)))
        t += rng.uniform(0.2, 0.5)
    jit = Noise(rng, 5.0, 40.0, rate)
    jit_rms = calibrate(lambda: Noise(crng, 5.0, 40.0, rate), 20, rate)

    ti, ni, fi = 0, 0, 0
    with open(a.out, "w") as f:
        f.write("# tools/xrmock/headpose.py seed %d%s: t x y z yaw pitch roll gaze_yaw gaze_pitch (s, m, degrees; gaze "
                "relative to the head)\n" % (a.seed, " still" if a.still else ""))
        gy_w = gp_w = 0.0
        for i in range(n):
            t = i * dt
            # head offset from the rest
            while ti + 1 < len(turns) and turns[ti + 1][0] <= t:
                ti += 1
            s0, d0, fy, fp, ty, tp = turns[ti] if turns else (0, 1, 0, 0, 0, 0)
            k = min_jerk((t - s0) / d0) if t >= s0 else 0.0
            hy, hp = fy + k * (ty - fy), fp + k * (tp - fp)
            while ni + 1 < len(nods) and nods[ni + 1][0] <= t:
                ni += 1
            if nods and nods[ni][0] <= t < nods[ni][0] + nods[ni][1]:
                _, ln, hz, deg = nods[ni]
                hp -= deg * math.sin(math.pi * hz * (t - nods[ni][0])) ** 2  # (down and back up, smooth at both ends)
            tr = [x.step() / tr_rms * a.tremor for x in tremor]
            sw = [x.step() / sw_rms * a.sway for x in sway]
            if a.still:
                yaw, pitch, roll = byaw, bpitch, 0.0
                pos = (bx, by, bz)
            else:
                yaw, pitch, roll = byaw + hy + tr[0], bpitch + hp + tr[1], 0.4 * tr[2]
                e = rot(yaw, pitch, roll, off)
                pos = (neck[0] + sw[0] + e[0], neck[1] + 0.3 * sw[1] + e[1], neck[2] + sw[2] + e[2])
            # gaze: the current world fixation, relative to the head, with jitter; limited to what eyes can turn
            while fi + 1 < len(fix) and fix[fi + 1][0] <= t:
                fi += 1
            ts, wy, wp = fix[fi]
            sk = min(1.0, (t - ts) / 0.04)  # (a saccade of 40 ms from the last point)
            py, pp = (fix[fi - 1][1], fix[fi - 1][2]) if fi else (wy, wp)
            gy_w, gp_w = py + sk * (wy - py), pp + sk * (wp - pp)
            j = jit.step() / jit_rms * 0.15
            gy = max(-30.0, min(30.0, byaw + gy_w - yaw + j))
            gp = max(-25.0, min(25.0, bpitch + gp_w - pitch + 0.7 * j))
            f.write("%.5f %.5f %.5f %.5f %.4f %.4f %.4f %.3f %.3f\n" % (t, pos[0], pos[1], pos[2], yaw, pitch, roll, gy, gp))


if __name__ == "__main__":
    main()

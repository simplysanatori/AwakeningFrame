#!/usr/bin/env python3
"""The view cases of the cull check and the mock's head pose file that walks through them.

The rig runs with tilt 30 and yaw 0; a case's table yaw and tilt are made by moving the head instead:
head' = A + Rx(30) Ry(-yaw) Rx(-tilt) (head - A), which gives the eyes the same place and direction in the game
world as a table turned by yaw and tilted by tilt with the head left alone (RIG.md: app = A + s R (world - anchor),
R = Rx(tilt) Ry(yaw))."""
import math, sys

DIST, PITCH, SCALE, TILT0 = 0.8, 15.0, 0.067, 30.0
A = (0.0, -DIST * math.sin(math.radians(PITCH)), -DIST * math.cos(math.radians(PITCH)))
HEADS = {'seat': (0.0, 0.0, 0.0), 'lean': (0.0, -0.10, -0.20)}
YAWS, TILTS = (0, 90, 180, 270), (0, 15, 30, 50)
import os
HOLD, START = float(os.environ.get("CASE_HOLD", "4.5")), float(os.environ.get("CASE_START", "24"))


def rx(a):
    c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
    return [[1, 0, 0], [0, c, -s], [0, s, c]]


def ry(a):
    c, s = math.cos(math.radians(a)), math.sin(math.radians(a))
    return [[c, 0, s], [0, 1, 0], [-s, 0, c]]


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def app(m, v):
    return tuple(sum(m[i][k] * v[k] for k in range(3)) for i in range(3))


def cases():
    out = []
    for head in HEADS:
        for tilt in TILTS:
            for yaw in YAWS:
                out.append((yaw, tilt, head))
    return out


def head_pose(yaw, tilt, head):
    """(position in app space, yaw, pitch, roll in degrees) of the mock's head for a case"""
    p = HEADS[head]
    d = tuple(A[i] - p[i] for i in range(3))
    look = math.degrees(math.atan2(d[1], -d[2]))  # the head looks at the table's centre
    M = mul(mul(rx(TILT0), ry(-yaw)), rx(-tilt))
    rel = app(M, tuple(p[i] - A[i] for i in range(3)))
    pos = tuple(A[i] + rel[i] for i in range(3))
    m = mul(M, rx(look))
    pitch = math.degrees(math.asin(max(-1.0, min(1.0, -m[1][2]))))
    y = math.degrees(math.atan2(m[0][2], m[2][2]))
    r = math.degrees(math.atan2(m[1][0], m[1][1]))
    return pos, y, pitch, r


def eye_rel(yaw, tilt, head):
    """where the head sits in the game world, from the anchor, in game units"""
    p = HEADS[head]
    R = mul(rx(tilt), ry(yaw))
    Rt = [[R[j][i] for j in range(3)] for i in range(3)]
    return app(Rt, tuple((p[i] - A[i]) / SCALE for i in range(3)))


if __name__ == '__main__':
    # the pose file: the start pose (the app space's origin), then each case held HOLD seconds, the list twice
    print('0.0 0 1.6 0 0 0 0')
    print('%.2f 0 1.6 0 0 0 0' % (START - 0.5))
    t = START
    for rep in range(int(sys.argv[1]) if len(sys.argv) > 1 else 2):
        for c in cases():
            pos, y, p, r = head_pose(*c)
            for tt in (t, t + HOLD - 0.1):
                print('%.2f %.4f %.4f %.4f %.3f %.3f %.3f' % (tt, pos[0], 1.6 + pos[1], pos[2], y, p, r))
            t += HOLD

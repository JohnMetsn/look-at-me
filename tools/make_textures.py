#!/usr/bin/env python3
"""Generate resources/data/eye.bin for Look at me!

Layout (all uint8):
  IRIS  96*96  low 6 bits luminance (0..63), bit6 heterochromia sector, bit7 inner (hazel) zone.
               Texture space: centre (48,48), radius 48 = iris edge, rest pupil radius PR0.
  VEIN  96*96  vein strength 0..255 on the eyeball in local (eye-fixed) coordinates, x,y in [-1,1].
  NOISE 64*64  tileable skin noise 0..255.
Same noise functions as concept/eye-sketches-v1.html so the watch matches the sketch.
"""
import math, os, struct

PR0 = 0.45
TEX = 96


def imul(a, b):
    return ((a & 0xFFFFFFFF) * (b & 0xFFFFFFFF)) & 0xFFFFFFFF


def s32(v):
    v &= 0xFFFFFFFF
    return v - 0x100000000 if v & 0x80000000 else v


def h32(x, y):
    h = s32(imul(x, 374761393) + imul(y, 668265263))
    h = imul((h ^ ((h & 0xFFFFFFFF) >> 13)) & 0xFFFFFFFF, 1274126177)
    h ^= h >> 16
    return (h & 0xFFFFFFFF) / 4294967296.0


def vn(x, y, P=0, Py=0):
    xi, yi = math.floor(x), math.floor(y)
    xf, yf = x - xi, y - yi
    u, v = xf * xf * (3 - 2 * xf), yf * yf * (3 - 2 * yf)
    x0, x1 = xi, xi + 1
    if P:
        x0, x1 = x0 % P, x1 % P
    y0, y1 = yi, yi + 1
    if Py:
        y0, y1 = y0 % Py, y1 % Py
    a, b, c, d = h32(x0, y0), h32(x1, y0), h32(x0, y1), h32(x1, y1)
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v


def clamp(v, a, b):
    return a if v < a else b if v > b else v


def smooth(e0, e1, x):
    t = clamp((x - e0) / (e1 - e0), 0, 1)
    return t * t * (3 - 2 * t)


def iris_value(t, phi, pr):
    ph = ((phi / (2 * math.pi)) % 1 + 1) % 1
    f = vn(ph * 110, t * 5.5, 110)
    f2 = vn(ph * 22, t * 1.7 + 7, 22)
    f3 = vn(ph * 52, t * 8 + 3, 52)
    v = 0.52 + 0.62 * (f - 0.5) + 0.40 * (f2 - 0.5)
    coll = pr + 0.15 + 0.07 * (f2 - 0.5)
    cg = math.exp(-((t - coll) / 0.07) ** 2)
    if f3 > 0.72 and coll < t < 0.8:
        v *= 0.55
    v *= 1 - 0.78 * smooth(0.78, 1.0, t)
    v *= 0.72 + 0.28 * smooth(pr, pr + 0.09, t)
    v = clamp(v, 0, 1)
    v = v + (1 - v) * cg * 0.5          # collarette pulled toward the light end of the ramp
    inner = t < coll + 0.02 + 0.05 * (f - 0.5)
    sector = 0.06 < ph < 0.31
    return v, inner, sector


def main():
    out = bytearray()
    for j in range(TEX):
        for i in range(TEX):
            x = (i + 0.5 - TEX / 2) / (TEX / 2)
            y = (j + 0.5 - TEX / 2) / (TEX / 2)
            t = math.hypot(x, y)
            phi = math.atan2(y, x)
            if t > 1.02:
                out.append(0)
                continue
            v, inner, sector = iris_value(min(t, 1.0), phi, PR0)
            b = int(round(v * 63)) & 63
            if sector:
                b |= 0x40
            if inner:
                b |= 0x80
            out.append(b)
    for j in range(TEX):
        for i in range(TEX):
            x2 = (i + 0.5 - TEX / 2) / (TEX / 2)
            y2 = (j + 0.5 - TEX / 2) / (TEX / 2)
            r = 1 - abs(2 * vn(x2 * 15 + 3, y2 * 9 + 1) - 1)
            r2 = 1 - abs(2 * vn(x2 * 30 + 11, y2 * 28 + 5) - 1)
            vv = max(smooth(0.86, 0.97, r), 0.7 * smooth(0.9, 0.985, r2))
            out.append(int(round(clamp(vv, 0, 1) * 255)))
    for j in range(64):
        for i in range(64):
            # tileable: wrap noise period 16 in x, sample y through a circle-free repeat of period 16
            n = vn(i * 0.25, j * 0.25, 16, 16)
            out.append(int(round(n * 255)))
    path = os.path.join(os.path.dirname(__file__), '..', 'resources', 'data', 'eye.bin')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as fh:
        fh.write(out)
    print('wrote', os.path.normpath(path), len(out), 'bytes')


if __name__ == '__main__':
    main()

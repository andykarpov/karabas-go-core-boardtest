#!/usr/bin/env python3
"""The frame the RTL should show (bt_tiles + menu_compose), bit for bit, from the state of the screen the CPU
wrote (a .state file of sim/soc/tb_bt.cpp: the gradient, the cursor, the palette, the cells) and the atlases as
loaded (roms/atlas1.bin, atlas4.bin). Compared with the frame of the simulation by sim/soc/run_sim.sh.

  frame_model.py STATE ROMS_DIR [OUT.ppm] [SIM.ppm]

With SIM.ppm: prints the number of pixels that differ (and the first ones) and exits 1 if any does."""
import sys

W, H, V_GRAD = 800, 600, 450
CUR = [0x000001, 0x000005, 0x000019, 0x000069, 0x0001A9, 0x0006A9, 0x001AA9, 0x006AA9, 0x01AAA9, 0x06AAA9,
       0x1AAAA9, 0x556AA9, 0x0069A9, 0x01A469, 0x01A419, 0x069005, 0x069001, 0x1A4000, 0x054000]
BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]


def read_state(path):
    st = {"pal": [0] * 16, "cells": [0] * 4096}
    for line in open(path):
        f = line.split()
        if f[0] == "grad":
            st["top"], st["bot"] = int(f[1], 16), int(f[2], 16)
        elif f[0] == "cursor":
            st["cursor"] = int(f[1], 16)
        elif f[0] == "pal":
            st["pal"][int(f[1])] = int(f[2], 16)
        elif f[0] == "cell":
            st["cells"][int(f[1])] = int(f[2], 16)
    return st


def ram(path, size):
    d = bytearray(open(path, "rb").read())
    return d + bytearray(size - len(d))


def gradient_line(top, bot, y):
    """menu_compose: the colour of line y in 8.4 fixed point, per channel."""
    tp = (y * 146) & 0x3FFFF
    t = 256 if (y >= V_GRAD or tp >> 16 & 1) else (tp >> 8) & 0xFF
    out = []
    for sh in (16, 8, 0):
        a, b = top >> sh & 255, bot >> sh & 255
        d = (b - a) * t                      # signed
        s = (a << 4) + (d >> 4)              # >>> 4 (arithmetic)
        out.append(s & 0xFFF)
    return out


def dith(v, th):
    s = v + th
    return 0xFF if s >> 12 & 1 else (s >> 4) & 0xFF


def tile_rgba(st, a1, a4, x, y):
    c = st["cells"][(y // 20) * 80 + x // 10]
    tile, fg, bg = c & 0x3FF, c >> 10 & 15, c >> 14 & 15
    pos = (tile & 0x1FF) * 200 + (y % 20) * 10 + x % 10
    if tile & 0x200:
        b = a4[(pos >> 1) & 0xFFF]
        a = b >> 4 if pos & 1 else b & 15
    else:
        a = 15 if a1[(pos >> 3) & 0x1FFF] >> (pos & 7) & 1 else 0
    w = a + (a >> 3)
    pf, pb = st["pal"][fg], st["pal"][bg]
    out = ((pb >> 24 & 15) * (16 - w) + (pf >> 24 & 15) * w) >> 4 << 24
    for sh in (16, 8, 0):
        out |= (((pb >> sh & 255) * (16 - w) + (pf >> sh & 255) * w) >> 4) << sh
    cur = st["cursor"]
    if cur >> 31 & 1:
        dx, dy = x - (cur & 0x7FF), y - (cur >> 16 & 0x3FF)
        if 0 <= dx < 12 and 0 <= dy < 19:
            p = CUR[dy] >> (2 * dx) & 3
            if p == 1:
                return 0xF000000
            if p == 2:
                return 0xFFFFFFF
    return out


def model(st, a1, a4):
    img = [0] * (W * H)
    for y in range(H):
        line = gradient_line(st["top"], st["bot"], y)
        for x in range(W):
            th = BAYER[(y & 3) * 4 + (x & 3)]
            g = [dith(v, th) for v in line]
            px = tile_rgba(st, a1, a4, x, y)
            al = px >> 24 & 15
            k = 16 if al == 15 else al
            rgb = 0
            for i, sh in enumerate((16, 8, 0)):
                rgb |= (((px >> sh & 255) * k + g[i] * (16 - k)) >> 4 & 0xFF) << sh
            img[y * W + x] = rgb
    return img


def write_ppm(path, img):
    with open(path, "wb") as f:
        f.write(b"P6 800 600 255\n")
        f.write(bytes(b for p in img for b in (p >> 16 & 255, p >> 8 & 255, p & 255)))


def read_ppm(path):
    d = open(path, "rb").read()
    body = d[d.index(b"255\n") + 4:]
    return [body[i] << 16 | body[i + 1] << 8 | body[i + 2] for i in range(0, W * H * 3, 3)]


if __name__ == "__main__":
    st = read_state(sys.argv[1])
    a1 = ram(sys.argv[2] + "/atlas1.bin", 8192)
    a4 = ram(sys.argv[2] + "/atlas4.bin", 4096)
    img = model(st, a1, a4)
    if len(sys.argv) > 3:
        write_ppm(sys.argv[3], img)
    if len(sys.argv) > 4:
        sim = read_ppm(sys.argv[4])
        bad = [(i % W, i // W, img[i], sim[i]) for i in range(W * H) if img[i] != sim[i]]
        print(f"frame_model: {len(bad)} pixels differ")
        for x, y, m, s in bad[:8]:
            print(f"  ({x}, {y}) model {m:06x} sim {s:06x}")
        sys.exit(1 if bad else 0)

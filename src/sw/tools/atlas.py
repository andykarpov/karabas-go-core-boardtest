#!/usr/bin/env python3
"""The tile atlas of the board test screen (docs/PLAN.md, "Видео: слой знакомест").

The screen is a grid of cells (CW x CH pixels, 10 x 20: 80 x 30 on 800 x 600). A cell names a tile
of the atlas and two colours of a 16 entry palette (fg, bg); a tile pixel is a 4-bit coverage a
(0..15): colour = bg + (fg - bg) * a / 15, alpha included, then menu_compose blends it over the
gradient. The atlas holds:

- the font: the 8 x 8 font of the firmware OSD (CP866, assets/osd_font_866.bin), each row twice
  (8 x 16), centred in the cell, ASCII 32..126 (user 2026-11-22: rather than Noto Sans Mono);
- the big digits of the clock: the same x2 again (16 x 32), 2 x 2 tiles per character;
- shapes (anti-aliased): solid, rounded ends of buttons, panel corners and edges, lines, meter
  fills, marks;
- the mono logo of the Mini (assets/logo_mini_56x16.bin = firmware ui2.c logo_full with the m and
  n of the colour logo, firmware PR #165), x4 = 224 x 64, cut into tiles (deduplicated).

The font, the digits and the logo are 1 bit (coverage 0 or 15), only the shapes use the 16 levels.

Atlas().tiles is a list of bytes objects (cw * ch coverages each, row by row); .id[name] the tile
index of a name ("ch:A", "big:1:0".."big:1:3" left-top, right-top, left-bottom, right-bottom,
shape names, "logo" -> (cols, rows, [indexes row by row])).
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "..", "assets")
BIG_CHARS = "0123456789:/ "  # the clock and the date only
SS = 4  # supersampling of the shapes


def logo_bits():
    b = open(os.path.join(ASSETS, "logo_mini_56x16.bin"), "rb").read()
    return [[(b[y * 7 + x // 8] >> (7 - x % 8)) & 1 for x in range(56)] for y in range(16)]


def osd_font():
    """The 8 x 8 font of the firmware OSD (CP866, 256 x 8 bytes, the left pixel in bit 7)."""
    return open(os.path.join(ASSETS, "osd_font_866.bin"), "rb").read()


class Atlas:
    def __init__(self, cw=10, ch=20, logo_xy=(8, 8), logo_scale=4):
        self.cw, self.ch = cw, ch
        self.tiles, self.id, self._idx = [], {}, {}
        self.add("empty", bytes(cw * ch))
        self.add("solid", bytes([15]) * (cw * ch))
        self._osd_font()
        self._shapes()
        self._logo(logo_xy, logo_scale)

    # a tile from an L image (0..255) of cw x ch, deduplicated
    def add(self, name, data):
        if isinstance(data, Image.Image):
            data = bytes((v * 15 + 127) // 255 for v in data.getdata())
        i = self._idx.get(data)
        if i is None:
            i = len(self.tiles)
            self.tiles.append(data)
            self._idx[data] = i
        if name:
            self.id[name] = i
        return i

    def _osd_font(self):
        """The OSD font, each row twice (8 x 16), centred in the cell, 1 bit; the big digits x2 more."""
        f = osd_font()
        def glyph(code, sx, sy):
            img = Image.new("L", (8 * sx, 8 * sy), 0)
            for y in range(8):
                for x in range(8):
                    if f[code * 8 + y] >> (7 - x) & 1:
                        img.paste(255, (x * sx, y * sy, (x + 1) * sx, (y + 1) * sy))
            return img
        for c in range(32, 127):
            img = Image.new("L", (self.cw, self.ch), 0)
            img.paste(glyph(c, 1, 2), ((self.cw - 8) // 2, (self.ch - 16) // 2))
            self.add("ch:" + chr(c), img)
        W, H = 2 * self.cw, 2 * self.ch
        for c in BIG_CHARS:
            img = Image.new("L", (W, H), 0)
            img.paste(glyph(ord(c), 2, 4), ((W - 16) // 2, (H - 32) // 2))
            for k, (x, y) in enumerate(((0, 0), (1, 0), (0, 1), (1, 1))):
                self.add(f"big:{c}:{k}", img.crop((x * self.cw, y * self.ch, (x + 1) * self.cw, (y + 1) * self.ch)))

    def _shape(self, name, draw_fn):
        big = Image.new("L", (self.cw * SS, self.ch * SS), 0)
        draw_fn(ImageDraw.Draw(big), self.cw * SS, self.ch * SS)
        self.add(name, big.resize((self.cw, self.ch), Image.LANCZOS))

    def _shapes(self):
        r = 4 * SS
        # button ends (a button = one row of cells: end_l, text cells with bg = button, end_r)
        self._shape("end_l", lambda d, w, h: d.rounded_rectangle((0, 0, 2 * w, h - 1), r, fill=255))
        self._shape("end_r", lambda d, w, h: d.rounded_rectangle((-w, 0, w - 1, h - 1), r, fill=255))
        self._shape("end_lr", lambda d, w, h: d.rounded_rectangle((0, 0, w - 1, h - 1), r, fill=255))
        # panel: corners and edges of a frame (line 2 px) with the inside transparent
        lw = 2 * SS
        for nm, box in (("p_tl", (0, 0, 3, 3)), ("p_tr", (-2, 0, 1, 3)), ("p_bl", (0, -2, 3, 1)), ("p_br", (-2, -2, 1, 1))):
            self._shape(nm, lambda d, w, h, box=box: d.rounded_rectangle(
                (box[0] * w, box[1] * h, box[2] * w - 1, box[3] * h - 1), 2 * r, outline=255, width=lw))
        self._shape("p_t", lambda d, w, h: d.rectangle((0, 0, w, lw - 1), fill=255))
        self._shape("p_b", lambda d, w, h: d.rectangle((0, h - lw, w, h), fill=255))
        self._shape("p_l", lambda d, w, h: d.rectangle((0, 0, lw - 1, h), fill=255))
        self._shape("p_r", lambda d, w, h: d.rectangle((w - lw, 0, w, h), fill=255))
        # the title underline (2 px, low in the cell), a thin line in the middle
        self._shape("uline", lambda d, w, h: d.rectangle((0, h - 3 * SS, w, h - SS - 1), fill=255))
        self._shape("hline", lambda d, w, h: d.rectangle((0, h // 2 - SS // 2, w, h // 2 + SS // 2 - 1), fill=255))
        # meter: fills of 0..cw columns (a bar with the middle 60 % of the height)
        # (the filled part opaque, the rest of the track at 1/3: one colour for the whole meter)
        for k in range(self.cw + 1):
            self._shape(f"bar{k}", lambda d, w, h, k=k: (
                d.rectangle((0, h * 3 // 10, w, h * 7 // 10 - 1), fill=85),
                k and d.rectangle((0, h * 3 // 10, k * SS - 1, h * 7 // 10 - 1), fill=255)))
        # marks: a dot, a ring, a tick, a cross (status), arrows
        cx, cy = self.cw * SS // 2, self.ch * SS // 2
        rr = min(self.cw, self.ch) * SS * 3 // 8
        self._shape("dot", lambda d, w, h: d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), fill=255))
        sr = SS * 5 // 2   # the small dot of a status on a button (as the Setup bullet of the menu)
        self._shape("sdot", lambda d, w, h: d.ellipse((cx - sr, cy - sr, cx + sr, cy + sr), fill=255))
        self._shape("ring", lambda d, w, h: d.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), outline=255, width=SS * 3 // 2))
        self._shape("tick", lambda d, w, h: d.line(((w * 0.12, cy), (w * 0.4, cy + rr * 0.9), (w * 0.9, cy - rr * 1.1)), fill=255, width=2 * SS))
        self._shape("cross", lambda d, w, h: (d.line(((w * .15, cy - rr), (w * .85, cy + rr)), fill=255, width=2 * SS),
                                             d.line(((w * .15, cy + rr), (w * .85, cy - rr)), fill=255, width=2 * SS)))

    def _logo(self, xy, scale):
        bits = logo_bits()
        lw, lh = 56 * scale, 16 * scale
        cols = (xy[0] + lw + self.cw - 1) // self.cw
        rows = (xy[1] + lh + self.ch - 1) // self.ch
        img = Image.new("L", (cols * self.cw, rows * self.ch), 0)
        for y in range(16):
            for x in range(56):
                if bits[y][x]:
                    img.paste(255, (xy[0] + x * scale, xy[1] + y * scale, xy[0] + (x + 1) * scale, xy[1] + (y + 1) * scale))
        idx = []
        for r in range(rows):
            for c in range(cols):
                idx.append(self.add(None, img.crop((c * self.cw, r * self.ch, (c + 1) * self.cw, (r + 1) * self.ch))))
        self.id["logo"] = (cols, rows, idx)

    def bytes_used(self):
        return len(self.tiles) * self.cw * self.ch // 2


if __name__ == "__main__":
    a = Atlas()
    print(f"{len(a.tiles)} tiles, {a.bytes_used()} bytes at 4 bits, logo {a.id['logo'][:2]}")

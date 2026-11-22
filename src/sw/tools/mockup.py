#!/usr/bin/env python3
"""Mock-up frames of the board test screen, pixel for pixel as the hardware will draw them
(bt_tiles + menu_compose): the gradient, the cells of the atlas with their fg / bg colours.

  mockup.py OUT_DIR

Writes main_<variant>.png and sram_<variant>.png for the cell / font variants of atlas.py."""
import os
import sys
from PIL import Image
from atlas import Atlas

W, H, V_GRAD = 800, 600, 450

# the palette (16 entries, alpha 0..15 + RGB): the Ocean theme of the menu (karabas.ini [ft812])
GRAD_TOP, GRAD_BOT = 0x006a80, 0x000608
PAL = {
    "none": (0, 0),
    "text": (15, 0xe0fcff), "text_a": (15, 0xffffff), "foot": (15, 0x5a8088), "dim": (15, 0x8fb0b8),
    "button": (15, 0x0e2228), "active": (15, 0x00a0b8),
    "pass": (15, 0x2fbf5a), "fail": (15, 0xe0303c), "run": (15, 0xe0b000), "idle": (15, 0x3a5058), "na": (15, 0x3a5058),
    "line": (15, 0x6a8a92), "panel": (11, 0x061418), "logo": (15, 0xffffff),
}
NAMES = list(PAL)
assert len(NAMES) <= 16


def rgb(c):
    return (c >> 16) & 255, (c >> 8) & 255, c & 255


class Screen:
    def __init__(self, atlas, ox=0, oy=0):
        self.a = atlas
        self.cols, self.rows = W // atlas.cw, H // atlas.ch
        self.ox, self.oy = ox, oy
        self.cells = [[(0, "none", "none")] * self.cols for _ in range(self.rows)]

    def put(self, c, r, tile, fg="text", bg="none"):
        if 0 <= c < self.cols and 0 <= r < self.rows:
            t = self.a.id[tile] if isinstance(tile, str) else tile
            self.cells[r][c] = (t, fg, bg)

    def text(self, c, r, s, fg="text", bg="none"):
        for i, ch in enumerate(s):
            self.put(c + i, r, "ch:" + ch, fg, bg)

    def fill(self, c, r, w, bg):
        for i in range(w):
            self.put(c + i, r, "empty", "text", bg)

    def button(self, c, r, w, s, color="button", fg="text", align="center", dot=False):
        self.put(c, r, "end_l", color)
        self.put(c + w - 1, r, "end_r", color)
        self.fill(c + 1, r, w - 2, color)
        # with a status column (dot is not False) the text keeps off the first inner cell
        c0, n = (c + 2, w - 3) if dot is not False else (c + 1, w - 2)
        s = s[:n - (0 if align == "center" else 1)]
        x = c0 + ((n - len(s)) // 2 if align == "center" else 1)
        self.text(x, r, s, fg, color)
        if dot:  # the status on the button, as the bullet of the Setup sections of the boot menu
            self.put(c + 1, r, "sdot", dot, color)

    def pill(self, c, r, s, color):
        self.button(c, r, len(s) + 2, s, color, "text_a")

    def uline(self, c, r, w, fg="line"):
        for i in range(w):
            self.put(c + i, r, "uline", fg)

    def meter(self, c, r, w, frac, fg="active"):
        n = round(frac * w * self.a.cw)
        for i in range(w):
            self.put(c + i, r, f"bar{max(0, min(self.a.cw, n - i * self.a.cw))}", fg)

    def logo(self, c, r, fg="logo", go=False):
        cols, rows, idx = self.a.id["logo_go" if go else "logo"]
        for y in range(rows):
            for x in range(cols):
                self.put(c + x, r + y, idx[y * cols + x], fg)

    def render(self):
        img = Image.new("RGB", (W, H))
        px = img.load()
        top, bot = rgb(GRAD_TOP), rgb(GRAD_BOT)
        cw, ch = self.a.cw, self.a.ch
        for y in range(H):
            t = min(1.0, y / V_GRAD)
            g = tuple(top[i] + (bot[i] - top[i]) * t for i in range(3))
            for x in range(W):
                c, r = (x - self.ox) // cw, (y - self.oy) // ch
                out = g
                if 0 <= c < self.cols and 0 <= r < self.rows and x >= self.ox and y >= self.oy:
                    tile, fg, bg = self.cells[r][c]
                    a = self.a.tiles[tile][((y - self.oy) % ch) * cw + (x - self.ox) % cw]
                    fa, fc = PAL[fg]
                    ba, bc = PAL[bg]
                    fr, br = rgb(fc), rgb(bc)
                    al = (ba * (15 - a) + fa * a) / 15          # alpha of the cell pixel 0..15
                    col = tuple((br[i] * (15 - a) + fr[i] * a) / 15 for i in range(3))
                    k = 16 if al >= 14.5 else al                # menu_compose: 15 = opaque
                    out = tuple(col[i] * k / 16 + g[i] * (16 - k) / 16 for i in range(3))
                px[x, y] = tuple(int(v) for v in out)
        return img


GROUPS = [("FPGA / MCU", "pass", "DNA 0x012345"), ("SRAM", "fail", "MD7 open"),
          ("SDRAM", "pass", "166 MHz"), ("SPI flash", "na", "not fitted"),
          ("SD cards", "pass", "SD2 7.4 GB"), ("CF / IDE", "idle", "no card"),
          ("FT812", "pass", "ID 7C"), ("Wi-Fi", "pass", "AT 1.7.4"), ("RTC / EEPROM", "pass", "24C08"),
          ("Audio", "run", "L / R tones"), ("Tape", "idle", "not run"),
          ("MIDI", "idle", "not run"), ("Video", "pass", "HDMI 800x600"),
          ("Input", "pass", "kbd, mouse"), ("Buttons / LEDs", "idle", "not run"),
          ("LED matrix", "idle", "not run"), ("Floppy", "idle", "Go only")]
STATUS = {"pass": "PASS", "fail": "FAIL", "run": "RUN", "idle": "--", "na": "N/A"}


def header(s):
    s.logo(0, 0)


def main_screen(a):
    s = Screen(a)
    header(s)
    s.text(1, 5, "Karabas Board Test", "text_a")
    s.text(s.cols - 1 - len("Mini G  fw 26112119"), 5, "Mini G  fw 26112119", "dim")
    s.uline(1, 5 + 1, s.cols - 2)
    for i, (name, st, info) in enumerate(GROUPS):
        col, row = (1 if i < 9 else 40), 8 + 2 * (i % 9)
        s.button(col, row, 17, name, "active" if i == 1 else "button", "text_a", "left")
        s.pill(col + 18, row, f"{STATUS[st]:^4}", st)
        s.text(col + 25, row, info[:13 + (col != 1)], "dim")
    s.text(1, s.rows - 2, "SRAM: data line MD7 does not toggle (stuck at 1)", "foot")
    s.text(1, s.rows - 1, "Arrows: select  Enter: open  R: run all  Ctrl+Alt+Backspace: exit", "foot")
    return s


def sram_screen(a):
    s = Screen(a)
    header(s)
    for i, (name, st, _) in enumerate(GROUPS[:10]):
        s.button(1, 5 + 2 * i, 15, name, "active" if i == 1 else "button", "text_a",
                 dot=st if st in ("pass", "fail", "run") else None)
    x = 18
    s.text(x, 5, "SRAM", "text_a")
    s.pill(x + 6, 5, "FAIL", "fail")
    s.uline(x, 6, s.cols - x - 1)
    rows = [("Size", "4 MB (2 x 2M x 8)"), ("Mode", "march C-, then random"),
            ("Passes", "3"), ("Errors", "1 237"), ("First", "0x000014  bit 7 read 1")]
    for i, (k, v) in enumerate(rows):
        s.text(x, 8 + i, k, "dim")
        s.text(x + 10, 8 + i, v)
    s.text(x, 14, "Data", "dim")
    for b in range(16):
        s.put(x + 10 + b, 14, "dot", "fail" if b == 7 else "pass")
    s.text(x, 15, "", "dim")
    for b in range(16):
        s.text(x + 10 + b, 15, "%X" % b, "dim")
    s.text(x, 17, "Address", "dim")
    for b in range(21):
        s.put(x + 10 + b, 17, "dot", "pass")
    for b in range(21):
        s.text(x + 10 + b, 18, str(b % 10), "dim")
    s.text(x, 20, "Progress", "dim")
    s.meter(x + 10, 20, 30, 0.63)
    s.text(x + 41, 20, "63%")
    s.text(1, s.rows - 2, "MD7 stuck at 1: check the pin of U3 and its solder", "foot")
    s.text(1, s.rows - 1, "Up/Down: group  Enter: run  Space: stop  Ctrl+Alt+Backspace: exit", "foot")
    return s


if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    a = Atlas()
    main_screen(a).render().save(os.path.join(out, "main.png"))
    sram_screen(a).render().save(os.path.join(out, "sram.png"))
    print(len(a.tiles), "tiles")

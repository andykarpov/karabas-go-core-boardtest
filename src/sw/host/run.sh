#!/bin/sh
# The frames of the pages (host build of the program, a fake board): out/<scenario>.png and out/sheet.png
# (all of them, for the approval of the look). Seconds.
set -e
cd "$(dirname "$0")"
mkdir -p out
gcc -DHOST -O1 -Wall -Wno-unused-result -I.. -o out/host ../main.c ../tests.c ../mem.c ../per.c ../scr.c ../util.c host.c per_dev.c
S="${S:-main fpga sram sram_fail sdram sdram_slow sdram_fail rtc rtc_oldfw leds leds_oldfw matrix audio tape midi video video_bars video_ramp input sd sd_none cf cf_none ft ft_fail wifi wifi_none}"
for s in $S; do
  ./out/host $s out/$s.state
  python3 ../tools/frame_model.py out/$s.state ../../roms out/$s.ppm
done
python3 - $S <<'PY'
import sys
from PIL import Image, ImageDraw
names = sys.argv[1:]
cols = 3
rows = (len(names) + cols - 1) // cols
sheet = Image.new("RGB", (cols * 800, rows * 620), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, n in enumerate(names):
    im = Image.open(f"out/{n}.ppm")
    im.save(f"out/{n}.png")
    x, y = (i % cols) * 800, (i // cols) * 620
    sheet.paste(im, (x, y + 20))
    d.text((x + 6, y + 4), n, fill=(255, 255, 0))
sheet.save("out/sheet.png")
PY
echo "host: out/sheet.png"

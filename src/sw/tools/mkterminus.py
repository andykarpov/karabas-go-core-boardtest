#!/usr/bin/env python3
"""Extract the printable ASCII (32..126) of Terminus Font Bold 10 x 20 (console-setup Uni2-TerminusBold20x10.psf.gz,
SIL OFL 1.1, assets/OFL-Terminus.txt) into assets/terminus_bold_10x20.bin: 95 glyphs x 20 rows x 2 bytes, big
endian, the left pixel in bit 15 (10 bits used).

  tools/mkterminus.py [/usr/share/consolefonts/Uni2-TerminusBold20x10.psf.gz] [assets/terminus_bold_10x20.bin]
"""
import gzip, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def main(src="/usr/share/consolefonts/Uni2-TerminusBold20x10.psf.gz",
         out=os.path.join(HERE, "..", "assets", "terminus_bold_10x20.bin")):
    b = gzip.open(src).read()
    assert b[:4] == b"\x72\xb5\x4a\x86", "not PSF2"
    hs, fl, n, cs, h, w = struct.unpack("<6I", b[8:32])
    assert (w, h) == (10, 20) and fl & 1, (w, h, fl)
    g, tab = b[hs:hs + n * cs], b[hs + n * cs:]
    uni = {}
    for i, chunk in enumerate(tab.split(b"\xff")[:n]):
        for ch in chunk.split(b"\xfe")[0].decode("utf-8"):
            uni.setdefault(ch, i)
    o = bytearray()
    for c in range(32, 127):
        d = g[uni[chr(c)] * cs:(uni[chr(c)] + 1) * cs]
        for y in range(20):
            o += d[y * 2:y * 2 + 2]
    open(out, "wb").write(o)
    print(f"{out}: {len(o)} bytes")


if __name__ == "__main__":
    main(*sys.argv[1:3])

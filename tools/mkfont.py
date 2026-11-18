#!/usr/bin/env python3
"""Make src/drivers/font8x16.c, the console font, from unscii-16.

    python3 tools/mkfont.py unscii-16.hex > src/drivers/font8x16.c

unscii-16.hex is Viznut's unscii, 8x16 variant, in GNU Unifont's .hex
format (http://viznut.fi/unscii/). That variant is in the public domain
(the "-full" one is not: it borrows from Unifont). The font is laid out
in code page 437, the PC's own character set, which is what the console
and the programs write: byte 0xB3 is a box-drawing line, 0xDB a full
block. unscii has all but four of the 256; those are drawn here.
"""
import sys

# Code page 437: what each byte shows, as Unicode.
LOW = "\u0000☺☻♥♦♣♠•◘○◙♂♀♪♫☼" \
      "►◄↕‼¶§▬↨↑↓→←∟↔▲▼"
CP437 = list(LOW) + [chr(i) for i in range(32, 127)] + ["⌂"] + list(bytes(range(128, 256)).decode("cp437"))

# The four unscii lacks, as 16 rows of 8 pixels (bit 7 is the left).
DRAWN = {
    0x0F: [0x00, 0x00, 0x00, 0x18, 0x18, 0xDB, 0x3C, 0xE7, 0x3C, 0xDB, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00],  # sun
    0x7F: [0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x6C, 0xC6, 0xC6, 0xC6, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00],  # house
    0xA9: [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0xC0, 0xC0, 0xC0, 0xC0, 0x00, 0x00, 0x00, 0x00],  # reversed not
    0xF9: [0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00],  # bullet operator
}


def main():
    glyphs = {}
    with open(sys.argv[1]) as f:
        for line in f:
            code, bits = line.strip().split(":")
            if len(bits) == 32:  # 8 pixels wide; 16-wide ones do not fit a cell
                glyphs[int(code, 16)] = bytes.fromhex(bits)
    out = ["// The console font: 256 glyphs of 8x16 pixels in code page 437, one",
           "// byte per row, bit 7 the leftmost pixel. Made by tools/mkfont.py from",
           "// Viznut's unscii-16, which is in the public domain. Do not edit.",
           "#include <stdint.h>", "",
           "const uint8_t font8x16[256 * 16] = {"]
    for i, ch in enumerate(CP437):
        rows = bytes(DRAWN[i]) if i in DRAWN else glyphs.get(ord(ch))
        if rows is None:
            sys.exit(f"unscii has no glyph for byte {i:#04x} ({ch!r})")
        out.append("    " + ", ".join(f"0x{b:02X}" for b in rows) + f",  // {i:#04x}")
    out.append("};")
    print("\n".join(out))


if __name__ == "__main__":
    main()

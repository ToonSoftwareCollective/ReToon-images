#!/usr/bin/env python3
"""Make splash-logo.h: the word "ReToon" as a run-length coded coverage map
for toon-splash.c. Run by hand when the logo changes (needs Pillow):
    python3 mklogo.py ../fonts/Inter-SemiBold.ttf > splash-logo.h
"""
import sys
from PIL import Image, ImageDraw, ImageFont

font = ImageFont.truetype(sys.argv[1], 104)
x0, y0, x1, y1 = font.getbbox("ReToon")
size = (x1 - x0, y1 - y0)
word = Image.new("L", size, 0)
ImageDraw.Draw(word).text((-x0, -y0), "ReToon", font=font, fill=255)
first = Image.new("L", size, 0)		# which pixels belong to "Re"
ImageDraw.Draw(first).text((-x0, -y0), "Re", font=font, fill=255)
# one byte per pixel: coverage 0..127, top bit set for the "Re" part
data = bytes((c >> 1) | (0x80 if c and f else 0) for c, f in zip(word.tobytes(), first.tobytes()))
runs, i = [], 0
while i < len(data):
    j = i
    while j < len(data) and data[j] == data[i] and j - i < 255:
        j += 1
    runs += [j - i, data[i]]
    i = j
print("/* made by mklogo.py: \"ReToon\" in Inter SemiBold; pairs of (count, value), */")
print("/* value = coverage 0..127, plus 128 for the \"Re\" part */")
print(f"#define LOGO_W {word.width}\n#define LOGO_H {word.height}")
print("static const unsigned char logo_rle[] = {")
for k in range(0, len(runs), 24):
    print("\t" + ",".join(map(str, runs[k:k + 24])) + ",")
print("};")

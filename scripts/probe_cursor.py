#!/usr/bin/env python3
"""probe_cursor.py — печатает RGB в заданной позиции PPM, и
рисует ASCII-карту 14x21 области вокруг этой позиции."""
import sys

def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6"
    pos = 2
    nums = []
    while len(nums) < 3:
        while pos < len(data) and data[pos:pos+1] in (b" ", b"\n", b"\r", b"\t"):
            pos += 1
        if data[pos:pos+1] == b"#":
            while data[pos:pos+1] != b"\n":
                pos += 1
            continue
        s = pos
        while pos < len(data) and data[pos:pos+1] not in (b" ", b"\n", b"\r", b"\t"):
            pos += 1
        nums.append(int(data[s:pos]))
    pos += 1
    return nums[0], nums[1], data[pos:]

def get_px(pix, x, y, w):
    off = (y * w + x) * 3
    return pix[off], pix[off+1], pix[off+2]

def ascii_box(pix, cx, cy, w, h, span=15):
    print(f"--- box around ({cx},{cy}) ---")
    for dy in range(-2, span):
        y = cy + dy
        if y < 0 or y >= h: continue
        line = ""
        for dx in range(-2, span):
            x = cx + dx
            if x < 0 or x >= w:
                line += "?"
                continue
            r, g, b = get_px(pix, x, y, w)
            if r < 30 and g < 30 and b < 30:
                line += "X"
            elif r > 220 and g > 220 and b > 220:
                line += "."
            else:
                line += f"·"
        print(f"  y={y:4d}: {line}")
    print()

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("usage: probe_cursor.py <ppm> <x> <y>")
        sys.exit(1)
    path = sys.argv[1]
    cx = int(sys.argv[2])
    cy = int(sys.argv[3])
    w, h, pix = load_ppm(path)
    print(f"image {w}x{h}")
    print(f"px at ({cx},{cy}) = {get_px(pix, cx, cy, w)}")
    ascii_box(pix, cx, cy, w, h)

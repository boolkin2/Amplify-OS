#!/usr/bin/env python3
"""find_cursor.py — найти позицию AMPLIFY-OS курсора в PPM."""
import sys

def load_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6"
    # Парсим header: 3 числа разделённые whitespace, потом newline.
    pos = 2
    nums = []
    while len(nums) < 3:
        # skip whitespace
        while pos < len(data) and data[pos:pos+1] in (b" ", b"\n", b"\r", b"\t"):
            pos += 1
        # skip comments
        if data[pos:pos+1] == b"#":
            while data[pos:pos+1] != b"\n":
                pos += 1
            continue
        # read number
        s = pos
        while pos < len(data) and data[pos:pos+1] not in (b" ", b"\n", b"\r", b"\t"):
            pos += 1
        nums.append(int(data[s:pos]))
    # после последнего числа должен быть один whitespace
    pos += 1
    w, h, mv = nums
    return w, h, data[pos:]

def get_px(pix, x, y, w):
    off = (y * w + x) * 3
    return pix[off], pix[off+1], pix[off+2]

def find_cursor(path):
    w, h, pix = load_ppm(path)
    # Курсор 12x19. Уникальный паттерн строки 3 (индекс): "X..X"
    candidates = []
    for y in range(0, h - 19):
        for x in range(0, w - 12):
            r1, g1, b1 = get_px(pix, x, y + 3, w)
            r2, g2, b2 = get_px(pix, x + 1, y + 3, w)
            r3, g3, b3 = get_px(pix, x + 2, y + 3, w)
            r4, g4, b4 = get_px(pix, x + 3, y + 3, w)
            # X..X
            if (r1 < 30 and g1 < 30 and b1 < 30 and
                r2 > 200 and g2 > 200 and b2 > 200 and
                r3 > 200 and g3 > 200 and b3 > 200 and
                r4 < 30 and g4 < 30 and b4 < 30):
                # row 0: X (only)
                r0a, g0a, b0a = get_px(pix, x, y, w)
                r0b, g0b, b0b = get_px(pix, x + 1, y, w)
                if r0a < 30 and r0b > 50:
                    # row 1: XX
                    rA, _, _ = get_px(pix, x, y + 1, w)
                    rB, _, _ = get_px(pix, x + 1, y + 1, w)
                    if rA < 30 and rB < 30:
                        candidates.append((x, y))
    return w, h, candidates

if __name__ == "__main__":
    for path in sys.argv[1:]:
        try:
            w, h, cands = find_cursor(path)
            if cands:
                print(f"{path}: matches: {cands}")
            else:
                print(f"{path}: cursor NOT FOUND ({w}x{h})")
        except Exception as e:
            print(f"{path}: ERROR {type(e).__name__}: {e}")

#!/usr/bin/env python3
"""
patch_font_with_cyrillic.py
============================
Патчит kernel/gui/font.c, заменяя байты глифов в позициях:
  0xA8 (Ё), 0xB8 (ё), 0xC0..0xFF (А-Я, а-я)
на отрендеренные из системного TTF (DejaVu Sans Mono Bold).

Это решение, при компиляции с -finput-charset=UTF-8 -fexec-charset=CP1251,
позволяет писать русский текст прямо в C-исходниках, и font_draw_char
будет печатать его глифами из обновлённого font_8x16.
"""

import os
import re
import sys
from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_C = os.path.join(ROOT, "kernel", "gui", "font.c")

FONT_PATH_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMonoBold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
]

GLYPH_MAP = {0xA8: "Ё", 0xB8: "ё"}
RU_UPPER = "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
RU_LOWER = "абвгдежзийклмнопрстуфхцчшщъыьэюя"
for i, ch in enumerate(RU_UPPER):
    GLYPH_MAP[0xC0 + i] = ch
for i, ch in enumerate(RU_LOWER):
    GLYPH_MAP[0xE0 + i] = ch

# Дополнительные знаки пунктуации, типичные при наборе русских строк
# в редакторе UTF-8 (после -fexec-charset=CP1251):
GLYPH_MAP[0x84] = "„"   # нижняя левая кавычка
GLYPH_MAP[0x91] = "‘"
GLYPH_MAP[0x92] = "’"
GLYPH_MAP[0x93] = "“"
GLYPH_MAP[0x94] = "”"
GLYPH_MAP[0x95] = "•"   # круглый буллет
GLYPH_MAP[0x96] = "–"   # короткое тире
GLYPH_MAP[0x97] = "—"   # длинное тире
GLYPH_MAP[0xAB] = "«"
GLYPH_MAP[0xBB] = "»"
GLYPH_MAP[0xB0] = "°"
GLYPH_MAP[0xB7] = "·"
GLYPH_MAP[0xA0] = " "   # неразрывный пробел -> обычный


def find_ttf():
    for p in FONT_PATH_CANDIDATES:
        if os.path.exists(p):
            return p
    raise SystemExit("DejaVu/FreeMono TTF не найден")


def render_glyph(font, ch):
    img = Image.new("L", (W, H), 0)
    draw = ImageDraw.Draw(img)
    try:
        bbox = font.getbbox(ch)
        gw = bbox[2] - bbox[0]
        gh = bbox[3] - bbox[1]
        ox = (W - gw) // 2 - bbox[0]
        oy = (H - gh) // 2 - bbox[1]
    except Exception:
        ox, oy = 0, 0
    draw.text((ox, oy), ch, fill=255, font=font)

    rows = []
    for y in range(H):
        byte = 0
        for x in range(W):
            v = img.getpixel((x, y))
            if v >= 96:
                byte |= (1 << (7 - x))
        rows.append(byte)
    return rows


def parse_font_array(text):
    """Находит начало и конец массива font_8x16 = { ... }; и парсит все байты."""
    m = re.search(r"const\s+uint8_t\s+font_8x16\s*\[[^\]]*\]\s*=\s*\{",
                  text)
    if not m:
        raise SystemExit("font_8x16 не найден в font.c")
    start = m.end()

    # Найдём соответствующую закрывающую '};' учитывая вложенность.
    depth = 1
    i = start
    while i < len(text) and depth > 0:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    end = i  # позиция после '}'
    # пропустим ';'
    while end < len(text) and text[end] in " \t":
        end += 1
    body = text[start:end - 1]   # без последней '}'

    # Парсим все шестнадцатеричные числа.
    bytes_ = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", body)]
    if len(bytes_) != 256 * H:
        raise SystemExit(f"Ожидалось {256*H} байт, найдено {len(bytes_)}")
    return start, end - 1, bytes_


def emit_array(bytes_):
    """Превращает 256*16 байт в красивый C-литерал."""
    lines = []
    for i in range(256):
        row = bytes_[i * H:(i + 1) * H]
        lines.append("    " + ", ".join("0x%02x" % b for b in row) + ",")
    return "\n".join(lines) + "\n"


def main():
    ttf_path = find_ttf()
    print(f"[+] TTF: {ttf_path}")
    font = ImageFont.truetype(ttf_path, 13)

    with open(FONT_C, "r", encoding="utf-8") as f:
        text = f.read()

    start, end, bytes_ = parse_font_array(text)
    print(f"[+] Распарсил {len(bytes_)} байт массива font_8x16")

    patched = 0
    for code, ch in GLYPH_MAP.items():
        rows = render_glyph(font, ch)
        for y in range(H):
            bytes_[code * H + y] = rows[y]
        patched += 1
    print(f"[+] Заменено глифов: {patched}")

    new_body = "\n" + emit_array(bytes_)
    new_text = text[:start] + new_body + text[end:]

    with open(FONT_C, "w", encoding="utf-8") as f:
        f.write(new_text)
    print(f"[+] {FONT_C} обновлён.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
gen_cyrillic_font.py
====================
Рендерит глифы кириллицы в формате 8x16 bitmap (1 байт на строку, MSB-слева)
из системного TTF-шрифта и патчит файл kernel/gui/font.c.

Целевые позиции в массиве font_8x16 (256 глифов × 16 байт):
  0xA8 = 168 — 'Ё'
  0xB8 = 184 — 'ё'
  0xC0..0xFF = 192..255 — 'А'..'я'  (в порядке CP1251)

Нумерация совпадает с CP1251, поэтому при компиляции исходников
с -fexec-charset=CP1251 строковые литералы автоматически попадут
по нужным индексам массива.
"""

import sys
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 8, 16
FONT_PATH_CANDIDATES = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/truetype/freefont/FreeMono.ttf",
]

# Карта (CP1251 индекс) -> Unicode-символ.
GLYPH_MAP = {}
GLYPH_MAP[0xA8] = "Ё"
GLYPH_MAP[0xB8] = "ё"
# А-Я: 0xC0..0xDF
RU_UPPER = "АБВГДЕЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ"
for i, ch in enumerate(RU_UPPER):
    GLYPH_MAP[0xC0 + i] = ch
# а-я: 0xE0..0xFF
RU_LOWER = "абвгдежзийклмнопрстуфхцчшщъыьэюя"
for i, ch in enumerate(RU_LOWER):
    GLYPH_MAP[0xE0 + i] = ch


def find_font():
    for p in FONT_PATH_CANDIDATES:
        if os.path.exists(p):
            return p
    raise RuntimeError("DejaVu/FreeMono TTF не найден")


def render_glyph(font, ch):
    """Возвращает 16 байт (по одному на строку, MSB слева)."""
    img = Image.new("L", (W, H), 0)
    draw = ImageDraw.Draw(img)
    # Подбираем смещение и размер под наш бокс 8x16. Используем 14pt
    # как близкое к высоте 16 px при моноширине 8.
    # Используем bbox чтобы центрировать.
    try:
        bbox = font.getbbox(ch)
        gw = bbox[2] - bbox[0]
        gh = bbox[3] - bbox[1]
        ox = (W - gw) // 2 - bbox[0]
        oy = (H - gh) // 2 - bbox[1] + 1   # +1 — чуть ниже base line
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


def main():
    font_path = find_font()
    # Размер шрифта подбирается экспериментально для 8x16 bitmap. 14 норм.
    font = ImageFont.truetype(font_path, 14)

    out = {}
    for code, ch in GLYPH_MAP.items():
        out[code] = render_glyph(font, ch)

    # Генерируем C-фрагмент: для каждого кода печатаем строку
    # с комментарием.
    parts = []
    parts.append("// =========================================================")
    parts.append("// Кириллические глифы (CP1251). Сгенерировано автоматически:")
    parts.append("//   tools/gen_cyrillic_font.py")
    parts.append("// =========================================================")
    parts.append("")
    parts.append("typedef struct { uint8_t code; uint8_t bytes[16]; } cyr_glyph_t;")
    parts.append("static const cyr_glyph_t cyr_glyphs[] = {")
    for code in sorted(out.keys()):
        ch = GLYPH_MAP[code]
        bs = out[code]
        b_str = ", ".join("0x%02x" % b for b in bs)
        parts.append(f"    {{ 0x{code:02X}, {{ {b_str} }} }}, // {ch}")
    parts.append("};")
    parts.append("static const int cyr_glyph_count = sizeof(cyr_glyphs) / sizeof(cyr_glyphs[0]);")

    print("\n".join(parts))


if __name__ == "__main__":
    main()

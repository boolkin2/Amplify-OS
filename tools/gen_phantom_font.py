#!/usr/bin/env python3
# ============================================================================
# gen_phantom_font.py - шрифт текстового экрана гостя phantom (этап 50.5).
# ----------------------------------------------------------------------------
# Пишет kernel/arch/x86_64/phantom_font_data.c: моноширинная гарнитура DejaVu
# Sans Mono (то же семейство, что и шрифт интерфейса ОС - DejaVu Sans) в
# нескольких размерах знакоместа и в кодировке CP437 - той, в которой гость
# пишет в видеопамять B8000. Шрифты интерфейса (ui_font_data.c) не годятся:
# у них кодировка CP1251 и только два моноширинных размера (8 и 10 px), а
# экран 80x25 на мониторе 1920x1080 хочется видеть крупно и без растяжения.
#
# Знакоместо - cell_w x 2*cell_w (экран 80x25 получается 8:5, как 640x400).
# Глиф хранится как у ui_font: 4-битное покрытие, обрезанное по рамке.
# Псевдографика рамок и блоков (B0h..DFh) сюда НЕ входит - её окно гостя
# рисует линиями, чтобы рамки сходились пиксель в пиксель.
#
# Запуск (Windows-python с PIL, из Git Bash; шрифт взять из Debian-пакета
# fonts-dejavu-core, в WSL это /usr/share/fonts/truetype/dejavu/):
#   python tools/gen_phantom_font.py <путь к DejaVuSansMono.ttf>
# ============================================================================
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_C = os.path.join(ROOT, 'kernel', 'arch', 'x86_64', 'phantom_font_data.c')

# Ширины знакоместа. Кегль подбирается так, чтобы шаг гарнитуры (0.602 em)
# не превышал ширину, а строка (1.164 em) - высоту 2*w.
CELLS = [8, 10, 12, 14, 16, 20, 24]

# Первые 32 кода CP437 - не управляющие символы, а значки.
LOW = ('\u0000☺☻♥♦♣♠•◘○◙♂♀♪♫☼'
       '►◄↕‼¶§▬↨↑↓→←∟↔▲▼')


def cp437_char(code):
    if code == 0 or code == 0x20 or code == 0xFF:
        return None                     # пустые знакоместа
    if code < 0x20:
        return LOW[code]
    if code == 0x7F:
        return '⌂'
    if 0xB0 <= code <= 0xDF:
        return None                     # рамки и блоки рисует ядро
    return bytes([code]).decode('cp437')


def pack4(rows, w, h):
    out = bytearray()
    for y in range(h):
        x = 0
        while x < w:
            hi = (rows[y][x] + 8) // 17
            lo = (rows[y][x + 1] + 8) // 17 if x + 1 < w else 0
            out.append((min(hi, 15) << 4) | min(lo, 15))
            x += 2
    return bytes(out)


def build_face(path, cw):
    ch = cw * 2
    px = int(cw / 0.602)
    font = ImageFont.truetype(path, px)
    ascent, descent = font.getmetrics()
    adv = font.getlength('M')
    # Строка по центру знакоместа; глиф - по центру своего шага.
    base = (ch - (ascent + descent)) // 2 + ascent
    xoff = (cw - adv) / 2.0
    pad = px * 2
    glyphs, blob = [], bytearray()
    for code in range(256):
        c = cp437_char(code)
        g = dict(w=0, h=0, left=0, top=0, off=len(blob))
        if c is not None:
            img = Image.new('L', (pad * 2 + cw * 2, pad * 2 + ch), 0)
            ImageDraw.Draw(img).text((pad + xoff, pad + base), c, font=font, fill=255, anchor='ls')
            # Всё, что вылезло за знакоместо, отрезаем: соседей оно не трогает.
            img = img.crop((pad, pad, pad + cw, pad + ch))
            box = img.getbbox()
            if box:
                x0, y0, x1, y1 = box
                w, h = x1 - x0, y1 - y0
                data = list(img.crop(box).getdata())
                rows = [data[y * w:(y + 1) * w] for y in range(h)]
                g = dict(w=w, h=h, left=x0, top=y0, off=len(blob))
                blob += pack4(rows, w, h)
        glyphs.append(g)
    return dict(cw=cw, ch=ch, px=px, glyphs=glyphs, blob=bytes(blob))


def main():
    if len(sys.argv) < 2 or not os.path.exists(sys.argv[1]):
        print('usage: gen_phantom_font.py <DejaVuSansMono.ttf>')
        return 1
    faces = [build_face(sys.argv[1], cw) for cw in CELLS]
    L = []
    L.append('// ============================================================================')
    L.append('// phantom_font_data.c - СГЕНЕРИРОВАНО tools/gen_phantom_font.py, руками не править.')
    L.append('// Шрифт текстового экрана гостя phantom: DejaVu Sans Mono, кодировка CP437,')
    L.append('// знакоместа %s px шириной (высота вдвое больше).' % ', '.join(str(c) for c in CELLS))
    L.append('// Глиф: 4-битное покрытие по строкам, два пикселя в байте, строка добита до байта.')
    L.append('// ============================================================================')
    L.append('#include "phantom_font.h"')
    L.append('')
    for f in faces:
        n = f['cw']
        L.append('static const uint8_t pf_blob_%d[%d] = {' % (n, max(1, len(f['blob']))))
        b = f['blob'] or b'\0'
        for i in range(0, len(b), 24):
            L.append('    ' + ','.join(str(x) for x in b[i:i + 24]) + ',')
        L.append('};')
        L.append('static const pf_glyph_t pf_glyphs_%d[256] = {' % n)
        for g in f['glyphs']:
            L.append('    { %d, %d, %d, %d, %d },' % (g['w'], g['h'], g['left'], g['top'], g['off']))
        L.append('};')
        L.append('')
    L.append('const pf_face_t phantom_font_faces[PHANTOM_FONT_FACES] = {')
    for f in faces:
        L.append('    { %d, %d, pf_glyphs_%d, pf_blob_%d },' % (f['cw'], f['ch'], f['cw'], f['cw']))
    L.append('};')
    L.append('')
    with open(OUT_C, 'w', encoding='utf-8', newline='\n') as fo:
        fo.write('\n'.join(L))
    total = sum(len(f['blob']) for f in faces)
    print('ok: %d faces, %d bytes of coverage -> %s' % (len(faces), total, OUT_C))
    for f in faces:
        print('  cell %2dx%2d  font %2d px  blob %6d' % (f['cw'], f['ch'], f['px'], len(f['blob'])))
    return 0


if __name__ == '__main__':
    sys.exit(main())

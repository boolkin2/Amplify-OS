# ============================================================================
# gen_rune_icon.py — значок приложения Rune (подэтап 51.4).
# ----------------------------------------------------------------------------
# Исходные PNG значков Pearl (tools/gen_ui_icons.py берёт их из /data/assets/px)
# на этой машине нет, поэтому новый значок строится из того, что есть в дереве:
#   плитка  — самый тёмный пиксель из уже существующих значков из ui_icons.c
#             (у всех одна и та же градиентная плитка, различается только знак,
#             а знак всегда светлее плитки);
#   знак    — руна «райдо» белой линией той же толщины, что у остальных
#             (рисуется в 8-кратном размере и уменьшается — края сглажены).
# Запуск (Windows-python, нужен PIL):  python tools/gen_rune_icon.py
# Результат: kernel/arch/x86_64/ui_icon_rune.c и build/fonts/ic_rune.png (посмотреть).
# ============================================================================
import os
import re
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'kernel', 'arch', 'x86_64', 'ui_icons.c')
OUT = os.path.join(ROOT, 'kernel', 'arch', 'x86_64', 'ui_icon_rune.c')
PNG = os.path.join(ROOT, 'build', 'fonts', 'ic_rune.png')
TILES = ['terminal', 'settings', 'browser', 'files', 'notes', 'music', 'store', 'mail', 'calendar']
N = 48
K = 8                                   # во сколько раз крупнее рисуем знак

text = open(SRC, encoding='utf-8', errors='replace').read()


def icon(name):
    m = re.search(r'ic_%s_px\[(\d+)\] = \{(.*?)\};' % name, text, re.S)
    return [int(v, 16) for v in re.findall(r'0x[0-9A-Fa-f]{8}', m.group(2))]


icons = [icon(n) for n in TILES]
tile = []
for i in range(N * N):
    # Знак у всех белый, то есть светлее плитки: из девяти значков берём тот,
    # где этот пиксель самый тёмный, — там знака нет. (Медиана не годится:
    # в середине плитки знаки перекрываются у большинства значков.)
    v = min((ic[i] for ic in icons),
            key=lambda p: ((p >> 16) & 255) + ((p >> 8) & 255) + (p & 255) if (p >> 24) else 1 << 20)
    alphas = sorted(ic[i] >> 24 for ic in icons)
    tile.append((alphas[len(alphas) // 2], (v >> 16) & 255, (v >> 8) & 255, v & 255))

# Плитки значков чуть различаются, и в середине «самый тёмный» оставляет
# тени знаков. Середину поэтому считаем заново: градиент плитки линейный —
# плоскость по каждому каналу, подобранная (наименьшие квадраты) по кольцу у
# края, куда знаки не заходят. Сам край (сглаженные углы) остаётся как был.
import numpy as np
RING = 8
pts = [(x, y) for y in range(N) for x in range(N)
       if tile[y * N + x][0] == 255 and (x < RING or y < RING or x >= N - RING or y >= N - RING)]
A = np.array([[x, y, 1.0] for x, y in pts])
plane = []
for ch in (1, 2, 3):
    coef, *_ = np.linalg.lstsq(A, np.array([tile[y * N + x][ch] for x, y in pts], dtype=float), rcond=None)
    plane.append(coef)
for y in range(RING, N - RING):
    for x in range(RING, N - RING):
        rgb = [int(max(0, min(255, round(c[0] * x + c[1] * y + c[2])))) for c in plane]
        tile[y * N + x] = (255, rgb[0], rgb[1], rgb[2])

# Знак: стойка, угловатая «дуга» и нога — руна райдо, она же буква R.
mask = Image.new('L', (N * K, N * K), 0)
d = ImageDraw.Draw(mask)
W = int(3.5 * K)


def line(pts):
    pts = [(x * K, y * K) for x, y in pts]
    d.line(pts, fill=255, width=W, joint='curve')
    for x, y in pts:                    # скруглённые концы и стыки
        d.ellipse((x - W / 2, y - W / 2, x + W / 2, y + W / 2), fill=255)


line([(17.5, 12), (17.5, 36)])
line([(17.5, 12), (30.5, 18.5), (17.5, 25)])
line([(22.5, 25.5), (31, 36)])
mask = mask.resize((N, N), Image.LANCZOS)
cov = list(mask.getdata())

px = []
for i in range(N * N):
    a, r, g, b = tile[i]
    c = cov[i]
    # Белый знак поверх плитки; прозрачность плитки (скруглённые углы) сохраняется.
    r = (r * (255 - c) + 255 * c) // 255
    g = (g * (255 - c) + 255 * c) // 255
    b = (b * (255 - c) + 255 * c) // 255
    px.append((a << 24) | (r << 16) | (g << 8) | b)

rows = []
for i in range(0, len(px), 12):
    rows.append('    ' + ','.join('0x%08X' % v for v in px[i:i + 12]) + ',')
body = '''// ============================================================================
// ui_icon_rune.c - значок приложения Rune (48x48 ARGB), подэтап 51.4.
// ----------------------------------------------------------------------------
// Файл создан tools/gen_rune_icon.py - руками не править. Лежит отдельно от
// ui_icons.c: тот собирается из исходных PNG, которых в дереве нет, а этот -
// из самих значков Pearl (плитка) и нарисованного знака (руна райдо).
// ============================================================================
#include <stdint.h>

const uint32_t ic_rune_px[%d] = {
%s
};
''' % (len(px), '\n'.join(rows))
with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write(body)

os.makedirs(os.path.dirname(PNG), exist_ok=True)
im = Image.new('RGBA', (N, N))
im.putdata([((v >> 16) & 255, (v >> 8) & 255, v & 255, v >> 24) for v in px])
sheet = Image.new('RGBA', (N * 8 + 16, N * 4 + 16), (40, 36, 46, 255))
sheet.alpha_composite(im.resize((N * 4, N * 4), Image.NEAREST), (8, 8))
sheet.alpha_composite(im, (N * 4 + 40, 8))
sheet.save(PNG)
print('ok:', OUT)

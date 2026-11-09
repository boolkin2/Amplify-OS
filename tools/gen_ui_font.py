#!/usr/bin/env python3
# ============================================================================
# gen_ui_font.py - offline generator of smoothed (anti-aliased) font atlases
# for AMPLIFY-OS "Pearl" (Stage F typography).
# ----------------------------------------------------------------------------
# Produces kernel/arch/x86_64/ui_font_data.c:
#   * proportional sans face (initrd_content/default.ttf) at 12/14/16/20/28/44
#   * monospace face (Liberation Mono) at 14/16 for console/terminal surfaces
# Every glyph is stored as 4-bit (16 level) coverage, bbox-trimmed, together
# with bearings and a 1/4 px advance. Kerning pairs are extracted through the
# Raqm layout engine and stored as a sorted sparse table.
# Charset: CP1251 0x20..0xFF (matches -fexec-charset=CP1251 used by the build).
# ============================================================================
import os
import sys
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SANS = os.path.join(ROOT, 'initrd_content', 'default.ttf')
MONO_CANDIDATES = [
    '/usr/share/fonts/liberation-mono/LiberationMono-Regular.ttf',
    '/usr/share/fonts/liberation/LiberationMono-Regular.ttf',
    '/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf',
]
OUT_C = os.path.join(ROOT, 'kernel', 'arch', 'x86_64', 'ui_font_data.c')

FIRST, LAST = 0x20, 0xFF
COUNT = LAST - FIRST + 1

# (enum name, ttf, pixel size, monospace advance in px or 0)
FACES = [
    ('UI_FONT_12',      'sans', 12, 0),
    ('UI_FONT_14',      'sans', 14, 0),
    ('UI_FONT_16',      'sans', 16, 0),
    ('UI_FONT_20',      'sans', 20, 0),
    ('UI_FONT_28',      'sans', 28, 0),
    ('UI_FONT_44',      'sans', 44, 0),
    ('UI_FONT_MONO_14', 'mono', 14, 8),
    ('UI_FONT_MONO_16', 'mono', 16, 10),
]


def cp1251_char(code):
    try:
        return bytes([code]).decode('cp1251')
    except UnicodeDecodeError:
        return None


def load(path, size):
    try:
        return ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.RAQM)
    except Exception:
        return ImageFont.truetype(path, size)


def pack4(rows, w, h):
    """Pack an 8-bit coverage matrix into 4-bit nibbles, row major."""
    out = bytearray()
    for y in range(h):
        x = 0
        while x < w:
            hi = (rows[y][x] + 8) // 17
            lo = 0
            if x + 1 < w:
                lo = (rows[y][x + 1] + 8) // 17
            out.append(((hi if hi < 16 else 15) << 4) | (lo if lo < 16 else 15))
            x += 2
    return bytes(out)


def build_face(kind, path, size, mono_adv):
    font = load(path, size)
    ascent, descent = font.getmetrics()
    pad = size * 2
    glyphs = []
    blob = bytearray()
    for code in range(FIRST, LAST + 1):
        ch = cp1251_char(code)
        entry = dict(w=0, h=0, left=0, top=0, adv=0, off=0)
        if ch is None or ch == '\u00a0':
            if ch == '\u00a0':
                entry['adv'] = int(round(font.getlength(' ') * 4))
            glyphs.append(entry)
            continue
        adv_q = int(round(font.getlength(ch) * 4))
        if mono_adv:
            adv_q = mono_adv * 4
        entry['adv'] = adv_q
        im = Image.new('L', (pad * 2 + size * 2, pad * 2 + size * 2), 0)
        d = ImageDraw.Draw(im)
        d.text((pad, pad), ch, font=font, fill=255)
        bbox = im.getbbox()
        if bbox is None:
            glyphs.append(entry)
            continue
        x0, y0, x1, y1 = bbox
        w, h = x1 - x0, y1 - y0
        crop = im.crop(bbox)
        px = list(crop.getdata())
        rows = [px[r * w:(r + 1) * w] for r in range(h)]
        left = x0 - pad
        top = (pad + ascent) - y0
        if mono_adv:
            # centre the glyph inside the fixed cell
            ink_adv = font.getlength(ch)
            left += int(round((mono_adv - ink_adv) / 2.0))
        entry.update(w=w, h=h, left=left, top=top, off=len(blob))
        blob += pack4(rows, w, h)
        glyphs.append(entry)

    kerns = []
    if not mono_adv:
        pairs = [chr(c) for c in range(0x20, 0x7F)]
        for a in pairs:
            la = font.getlength(a)
            for b in pairs:
                dq = int(round((font.getlength(a + b) - la - font.getlength(b)) * 4))
                if dq:
                    kerns.append((ord(a), ord(b), max(-128, min(127, dq))))
    return dict(ascent=ascent, descent=descent, line=ascent + descent,
                glyphs=glyphs, blob=bytes(blob), kerns=kerns, size=size,
                mono=mono_adv)


def emit_blob(name, blob):
    out = ['static const unsigned char %s[%d] = {' % (name, len(blob))]
    for i in range(0, len(blob), 24):
        out.append('    ' + ','.join('%d' % b for b in blob[i:i + 24]) + ',')
    out.append('};')
    return '\n'.join(out)


def main():
    mono_path = None
    for c in MONO_CANDIDATES:
        if os.path.exists(c):
            mono_path = c
            break
    if not os.path.exists(SANS):
        sys.exit('missing %s' % SANS)
    if not mono_path:
        sys.exit('no monospace ttf found')

    chunks = ['''// ============================================================================
// ui_font_data.c - GENERATED by tools/gen_ui_font.py. Do not edit by hand.
// Anti-aliased glyph atlases (4-bit coverage) for the Pearl type scale.
// Sans: DejaVu Sans (initrd_content/default.ttf). Mono: Liberation Mono.
// Charset: CP1251 0x20..0xFF, matching -fexec-charset=CP1251.
// ============================================================================
#include "ui_font.h"
''']
    faces_meta = []
    total = 0
    for enum_name, kind, size, mono_adv in FACES:
        path = SANS if kind == 'sans' else mono_path
        face = build_face(kind, path, size, mono_adv)
        tag = enum_name.lower()
        total += len(face['blob'])
        chunks.append(emit_blob(tag + '_blob', face['blob']))
        rows = []
        for g in face['glyphs']:
            rows.append('    {%d,%d,%d,%d,%d,%d},' % (
                g['w'], g['h'], g['left'], g['top'], g['adv'], g['off']))
        chunks.append('static const ui_glyph_t %s_glyphs[%d] = {\n%s\n};'
                      % (tag, COUNT, '\n'.join(rows)))
        if face['kerns']:
            kr = ['    {%d,%d,%d},' % k for k in face['kerns']]
            chunks.append('static const ui_kern_t %s_kern[%d] = {\n%s\n};'
                          % (tag, len(face['kerns']), '\n'.join(kr)))
            kern_ref = '%s_kern, %d' % (tag, len(face['kerns']))
        else:
            kern_ref = '0, 0'
        faces_meta.append((enum_name, tag, face, kern_ref))

    entries = []
    for enum_name, tag, face, kern_ref in faces_meta:
        entries.append('    [%s] = { %s_glyphs, %s_blob, %s, %d, %d, %d, %d },'
                       % (enum_name, tag, tag, kern_ref, face['size'],
                          face['ascent'], face['line'], face['mono']))
    chunks.append('const ui_face_t ui_font_faces[UI_FONT_COUNT] = {\n%s\n};'
                  % '\n'.join(entries))
    with open(OUT_C, 'w') as f:
        f.write('\n\n'.join(chunks) + '\n')
    print('wrote %s (%d bytes of glyph data)' % (OUT_C, total))
    for enum_name, tag, face, _ in faces_meta:
        print('  %-16s size=%2d ascent=%2d line=%2d blob=%6d kern=%4d'
              % (enum_name, face['size'], face['ascent'], face['line'],
                 len(face['blob']), len(face['kerns'])))


if __name__ == '__main__':
    main()

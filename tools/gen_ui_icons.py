from PIL import Image
import os

SRC = '/data/assets/px'
OUT_C = '/data/sys/OS2/kernel/arch/x86_64/ui_icons.c'
OUT_H = '/data/sys/OS2/kernel/arch/x86_64/ui_icons.h'

ICONS = ['amplify','files','folder','browser','mail','terminal','settings',
         'gallery','music','calendar','notes','store','trash']

def load(path):
    return Image.open(path).convert('RGBA')

def c_array(name, im):
    w, h = im.size
    px = list(im.getdata())
    vals = ['0x%02X%02X%02X%02X' % (a, r, g, b) for (r, g, b, a) in px]
    lines = []
    for i in range(0, len(vals), 12):
        lines.append('    ' + ','.join(vals[i:i+12]) + ',')
    return (w, h, 'static const uint32_t %s_px[%d] = {\n%s\n};\n' % (name, w*h, '\n'.join(lines)))

parts = []
meta = []
for n in ICONS:
    w, h, arr = c_array('ic_' + n, load(f'{SRC}/{n}.png'))
    parts.append(arr)
    meta.append((n, 'ic_' + n, w, h))

w, h, arr = c_array('logo_big', load(f'{SRC}/logo220.png'))
parts.append(arr); logo_big = (w, h)
w2, h2, arr2 = c_array('logo_small', load(f'{SRC}/logo24.png'))
parts.append(arr2); logo_small = (w2, h2)

header = '''// ============================================================================
// ui_icons.h \u2014 \u0440\u0430\u0441\u0442\u0440\u043e\u0432\u044b\u0435 \u0438\u043a\u043e\u043d\u043a\u0438 \u0434\u0438\u0437\u0430\u0439\u043d-\u0441\u0438\u0441\u0442\u0435\u043c\u044b AMPLIFY-OS "Pearl".
// ----------------------------------------------------------------------------
// 48x48 ARGB-\u0438\u043a\u043e\u043d\u043a\u0438 \u043f\u0440\u0438\u043b\u043e\u0436\u0435\u043d\u0438\u0439 + \u043b\u043e\u0433\u043e\u0442\u0438\u043f AMPLIFY (\u0431\u043e\u043b\u044c\u0448\u043e\u0439 \u0438 \u043c\u0430\u043b\u044b\u0439).
// ============================================================================
#ifndef AOS_X86_64_UI_ICONS_H
#define AOS_X86_64_UI_ICONS_H

#include <stdint.h>
#include "graphics.h"

#define UI_ICON_SIZE 48

typedef enum {
%s    UI_ICON_COUNT
} ui_icon_id_t;

typedef struct {
    const uint32_t* px;
    int w;
    int h;
} ui_bitmap_t;

const ui_bitmap_t* ui_icon(ui_icon_id_t id);
ui_icon_id_t ui_icon_by_name(const char* name);
const ui_bitmap_t* ui_logo_big(void);
const ui_bitmap_t* ui_logo_small(void);

void ui_draw_bitmap(const gfx_target_t* t, int x, int y, const ui_bitmap_t* bmp);
void ui_draw_bitmap_scaled(const gfx_target_t* t, int x, int y,
                           int dst_w, int dst_h, const ui_bitmap_t* bmp);
void ui_draw_bitmap_alpha(const gfx_target_t* t, int x, int y,
                          int dst_w, int dst_h, const ui_bitmap_t* bmp,
                          uint8_t alpha);

#endif
''' % ''.join('    UI_ICON_%s,\n' % n.upper() for n in ICONS)

names_tbl = ''.join('    { "%s", UI_ICON_%s },\n' % (n, n.upper()) for n in ICONS)
alias = '''    { "About",             UI_ICON_AMPLIFY  },
    { "About AMPLIFY-OS",  UI_ICON_AMPLIFY  },
    { "Info",              UI_ICON_SETTINGS },
    { "System Info",       UI_ICON_SETTINGS },
    { "Amply",             UI_ICON_STORE    },
    { "Amply Center",      UI_ICON_STORE    },
    { "Calc",              UI_ICON_CALENDAR },
    { "Term",              UI_ICON_TERMINAL },
    { "Terminal",          UI_ICON_TERMINAL },
    { "Paint",             UI_ICON_GALLERY  },
    { "Embers",            UI_ICON_MUSIC    },
    { "Sandbox",           UI_ICON_FOLDER   },
    { "Sandbox Manager",   UI_ICON_FOLDER   },
    { "Task Manager",      UI_ICON_SETTINGS },
    { "TaskMgr",           UI_ICON_SETTINGS },
    { "Clock",             UI_ICON_CALENDAR },
    { "Matrix",            UI_ICON_TERMINAL },
    { "Reboot",            UI_ICON_SETTINGS },
    { "Shutdown",          UI_ICON_TRASH    },
    { "Web",               UI_ICON_BROWSER  },
    { "3D\u043a\u0443\u0431",  UI_ICON_GALLERY  },
'''

body = '''// ============================================================================
// ui_icons.c \u2014 \u0434\u0430\u043d\u043d\u044b\u0435 \u0440\u0430\u0441\u0442\u0440\u043e\u0432\u044b\u0445 \u0438\u043a\u043e\u043d\u043e\u043a \u0442\u0435\u043c\u044b "Pearl" + \u0431\u043b\u0438\u0442\u0442\u0435\u0440.
// ============================================================================
#include "ui_icons.h"
#include "graphics.h"
#include <stdbool.h>

'''
body += '\n'.join(parts)

body += '''
static const ui_bitmap_t g_icons[UI_ICON_COUNT] = {
%s};

static const ui_bitmap_t g_logo_big   = { logo_big_px,   %d, %d };
static const ui_bitmap_t g_logo_small = { logo_small_px, %d, %d };

typedef struct { const char* name; ui_icon_id_t id; } ui_icon_name_t;

static const ui_icon_name_t g_names[] = {
%s%s};

// Сравнение без учёта регистра и пробелов (для "3D \u043a\u0443\u0431 (GLES)" и т.п.).
static bool ui_str_eq_ci(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

const ui_bitmap_t* ui_icon(ui_icon_id_t id) {
    if ((int)id < 0 || (int)id >= (int)UI_ICON_COUNT) return &g_icons[0];
    return &g_icons[id];
}

// Совпадение по префиксу: "3D \u043a\u0443\u0431 (GLES)" → "3D\u043a\u0443\u0431", пробелы игнорируются.
static bool ui_str_prefix_ci(const char* text, const char* prefix) {
    while (*prefix) {
        while (*text == ' ') text++;
        while (*prefix == ' ') prefix++;
        if (!*prefix) break;
        char ca = *text, cb = *prefix;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
        text++; prefix++;
    }
    return true;
}

ui_icon_id_t ui_icon_by_name(const char* name) {
    if (!name) return UI_ICON_AMPLIFY;
    for (unsigned i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        if (ui_str_eq_ci(name, g_names[i].name)) return g_names[i].id;
    }
    for (unsigned i = 0; i < sizeof(g_names) / sizeof(g_names[0]); i++) {
        if (ui_str_prefix_ci(name, g_names[i].name)) return g_names[i].id;
    }
    return UI_ICON_AMPLIFY;
}

const ui_bitmap_t* ui_logo_big(void)   { return &g_logo_big; }
const ui_bitmap_t* ui_logo_small(void) { return &g_logo_small; }

void ui_draw_bitmap(const gfx_target_t* t, int x, int y, const ui_bitmap_t* bmp) {
    if (!bmp) return;
    ui_draw_bitmap_scaled(t, x, y, bmp->w, bmp->h, bmp);
}

void ui_draw_bitmap_scaled(const gfx_target_t* t, int x, int y,
                           int dst_w, int dst_h, const ui_bitmap_t* bmp) {
    ui_draw_bitmap_alpha(t, x, y, dst_w, dst_h, bmp, 255);
}

void ui_draw_bitmap_alpha(const gfx_target_t* t, int x, int y,
                          int dst_w, int dst_h, const ui_bitmap_t* bmp,
                          uint8_t alpha) {
    if (!bmp || !bmp->px || dst_w <= 0 || dst_h <= 0) return;
    if (!t) t = gfx_screen();
    if (!t || !t->pixels) return;
    int stride = t->pitch / 4;
    for (int dy = 0; dy < dst_h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= t->height) continue;
        int sy = (dy * bmp->h) / dst_h;
        for (int dx = 0; dx < dst_w; dx++) {
            int pxx = x + dx;
            if (pxx < 0 || pxx >= t->width) continue;
            int sx = (dx * bmp->w) / dst_w;
            uint32_t src = bmp->px[sy * bmp->w + sx];
            uint32_t a = (src >> 24) & 0xFF;
            if (!a) continue;
            if (alpha != 255) a = (a * alpha) / 255;
            if (!a) continue;
            src = (a << 24) | (src & 0x00FFFFFFu);
            uint32_t dst = t->pixels[py * stride + pxx];
            gfx_put_pixel(t, pxx, py, gfx_blend(dst, src));
        }
    }
}
''' % (''.join('    { %s_px, %d, %d },\n' % (m[1], m[2], m[3]) for m in meta),
       logo_big[0], logo_big[1], logo_small[0], logo_small[1],
       names_tbl, alias)

open(OUT_H, 'w').write(header)
open(OUT_C, 'w').write(body)
print('wrote', os.path.getsize(OUT_C), 'bytes')

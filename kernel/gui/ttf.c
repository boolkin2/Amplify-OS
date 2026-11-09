// ============================================================================
// ttf.c - Рендеринг TrueType-шрифтов (.ttf) для MyOS (Этап 21)
// ============================================================================
// Использует stb_truetype для генерации anti-aliased глифов и блитит их
// в back-буфер (двойная буферизация). Цвет пикселя считается альфа-смешением:
//
//     out = src * (alpha/255) + dst * (1 - alpha/255)
//
// где alpha — покрытие глифа (0..255), src — цвет шрифта, dst — текущий пиксель.
// ============================================================================

#include "ttf.h"
#include "stb_truetype.h"
#include "framebuffer.h"
#include "doublebuf.h"
#include "graphics.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../fs/vfs.h"
#include "../vga.h"

// ============================================================================
// Внутренняя структура шрифта
// ============================================================================

struct ttf_font {
    stbtt_fontinfo info;   // Структура stb_truetype (заполняется по буферу)
    uint8_t*       data;   // Копия содержимого .ttf (владеется нами)
    size_t         size;   // Размер буфера
};

// Системный шрифт (загружается в ttf_system_init)
static ttf_font_t* g_system_font = NULL;

// ============================================================================
// Низкоуровневые утилиты
// ============================================================================

// Клэмп в диапазон [0..255]
static inline uint8_t clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// Альфа-смешение: наложить цвет src с покрытием cov (0..255) на back-буфер
// в точке (x, y). Быстро — прямой доступ к буферу.
static inline void blend_pixel(int32_t x, int32_t y, uint32_t src, uint8_t cov) {
    if (cov == 0) return;

    // Границы
    uint32_t w = framebuffer_get_width();
    uint32_t h = framebuffer_get_height();
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) return;

    uint32_t* buf = doublebuf_get_buffer();
    if (!buf) {
        // Нет двойного буфера — пишем прямо во фреймбуфер
        framebuffer_put_pixel(x, y, src);
        return;
    }

    uint32_t pitch_px = framebuffer_get_pitch() / 4;
    uint32_t* p = &buf[(uint32_t)y * pitch_px + (uint32_t)x];

    if (cov == 255) {
        *p = src;
        return;
    }

    uint32_t dst = *p;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db = (dst      ) & 0xFF;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >>  8) & 0xFF;
    uint32_t sb = (src      ) & 0xFF;
    uint32_t inv = 255 - cov;

    uint32_t r = (sr * cov + dr * inv) / 255;
    uint32_t g = (sg * cov + dg * inv) / 255;
    uint32_t b = (sb * cov + db * inv) / 255;

    *p = (r << 16) | (g << 8) | b;
}

// ============================================================================
// Загрузка шрифта
// ============================================================================

ttf_font_t* ttf_load(const uint8_t* data, size_t size) {
    if (!data || size < 4) return NULL;

    ttf_font_t* f = (ttf_font_t*)kmalloc(sizeof(ttf_font_t));
    if (!f) return NULL;

    f->data = (uint8_t*)kmalloc(size);
    if (!f->data) {
        kfree(f);
        return NULL;
    }
    memcpy(f->data, data, size);
    f->size = size;

    // Инициализация stb_truetype с первым шрифтом (offset 0)
    int off = stbtt_GetFontOffsetForIndex(f->data, 0);
    if (off < 0) {
        kfree(f->data);
        kfree(f);
        return NULL;
    }
    if (!stbtt_InitFont(&f->info, f->data, off)) {
        kfree(f->data);
        kfree(f);
        return NULL;
    }

    return f;
}

ttf_font_t* ttf_load_from_path(const char* path) {
    int fd = vfs_open_file(path, VFS_O_RDONLY);
    if (fd < 0) return NULL;

    int sz = vfs_get_filesize(fd);
    if (sz <= 0) {
        vfs_close_file(fd);
        return NULL;
    }

    uint8_t* buf = (uint8_t*)kmalloc(sz);
    if (!buf) {
        vfs_close_file(fd);
        return NULL;
    }

    int rd = vfs_read_file(fd, buf, sz);
    vfs_close_file(fd);
    if (rd != sz) {
        kfree(buf);
        return NULL;
    }

    ttf_font_t* f = ttf_load(buf, sz);
    kfree(buf);  // ttf_load копирует
    return f;
}

void ttf_free(ttf_font_t* font) {
    if (!font) return;
    if (font->data) kfree(font->data);
    kfree(font);
}

// ============================================================================
// Инициализация системного шрифта
// ============================================================================

bool ttf_system_init(void) {
    // Пытаемся стандартные пути (по приоритету)
    static const char* const candidates[] = {
        "/fonts/default.ttf",
        "/default.ttf",
        "/disk/fonts/default.ttf",
        NULL,
    };

    for (int i = 0; candidates[i]; i++) {
        ttf_font_t* f = ttf_load_from_path(candidates[i]);
        if (f) {
            g_system_font = f;
            vga_puts("[TTF] Systemnyj shrift zagruzhen: ");
            vga_println(candidates[i]);
            return true;
        }
    }

    vga_println("[TTF] Sistemnyj shrift ne najden (TrueType otklyuchen)");
    return false;
}

ttf_font_t* ttf_get_system_font(void) {
    return g_system_font;
}

// ============================================================================
// Рендеринг
// ============================================================================

int ttf_draw_char(ttf_font_t* font, int32_t x, int32_t y,
                  uint32_t codepoint, float pixel_height, uint32_t color)
{
    if (!font) return 0;

    float scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);

    int advance, lsb;
    stbtt_GetCodepointHMetrics(&font->info, codepoint, &advance, &lsb);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, codepoint, scale, scale,
                                &x0, &y0, &x1, &y1);

    int gw = x1 - x0;
    int gh = y1 - y0;

    if (gw > 0 && gh > 0) {
        // stb_truetype сама вызовет STBTT_malloc/free
        int out_w, out_h, xoff, yoff;
        uint8_t* bitmap = stbtt_GetCodepointBitmap(
            &font->info, 0, scale, codepoint, &out_w, &out_h, &xoff, &yoff);

        if (bitmap) {
            for (int py = 0; py < out_h; py++) {
                for (int px = 0; px < out_w; px++) {
                    uint8_t cov = bitmap[py * out_w + px];
                    if (cov) {
                        blend_pixel(x + xoff + px, y + yoff + py, color, cov);
                    }
                }
            }
            // stb_truetype распределяет bitmap через STBTT_malloc -> kmalloc
            kfree(bitmap);
        }
    }

    return (int)((float)advance * scale);
}

int ttf_draw_string(ttf_font_t* font, int32_t x, int32_t y,
                    const char* str, float pixel_height, uint32_t color)
{
    if (!font || !str) return 0;

    float scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);
    int32_t cx = x;

    while (*str) {
        uint32_t cp = (uint8_t)*str;  // Только ASCII пока

        if (cp == '\n') {
            // Перевод строки
            int a, d, g;
            stbtt_GetFontVMetrics(&font->info, &a, &d, &g);
            y += (int)((a - d + g) * scale);
            cx = x;
            str++;
            continue;
        }

        int adv = ttf_draw_char(font, cx, y, cp, pixel_height, color);

        // Кернинг с предыдущим символом
        if (str[1]) {
            int kern = stbtt_GetCodepointKernAdvance(&font->info, cp, (uint8_t)str[1]);
            adv += (int)((float)kern * scale);
        }

        cx += adv;
        str++;
    }

    return cx - x;
}

int ttf_draw_string_top(ttf_font_t* font, int32_t x, int32_t y,
                        const char* str, float pixel_height, uint32_t color)
{
    if (!font) return 0;
    float scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    int baseline = y + (int)(ascent * scale);
    return ttf_draw_string(font, x, baseline, str, pixel_height, color);
}

int ttf_string_width(ttf_font_t* font, const char* str, float pixel_height) {
    if (!font || !str) return 0;

    float scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);
    int width = 0;

    while (*str) {
        uint32_t cp = (uint8_t)*str;
        if (cp == '\n') { str++; continue; }

        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font->info, cp, &adv, &lsb);
        width += (int)((float)adv * scale);

        if (str[1]) {
            int kern = stbtt_GetCodepointKernAdvance(&font->info, cp, (uint8_t)str[1]);
            width += (int)((float)kern * scale);
        }

        str++;
    }

    return width;
}

int ttf_line_height(ttf_font_t* font, float pixel_height) {
    if (!font) return 0;
    float scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    return (int)((ascent - descent + line_gap) * scale);
}

// ============================================================================
// Callback-рендеринг: один проход по глифам, вызов cb для каждого
// видимого пикселя. Используется, например, для splash-экрана с
// градиентом+бликом на всю строку.
// ============================================================================
int ttf_draw_string_cb(ttf_font_t* font, int32_t x, int32_t y,
                       const char* str, float pixel_height,
                       ttf_pixel_cb_t cb, void* user)
{
    if (!font || !str || !cb) return 0;

    float scale = stbtt_ScaleForPixelHeight(&font->info, pixel_height);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    int baseline = y + (int)(ascent * scale);

    int32_t cx = x;
    while (*str) {
        uint32_t cp = (uint8_t)*str;
        if (cp == '\n') {
            baseline += (int)((ascent - descent + line_gap) * scale);
            cx = x;
            str++;
            continue;
        }

        int adv, lsb;
        stbtt_GetCodepointHMetrics(&font->info, cp, &adv, &lsb);

        int out_w, out_h, xoff, yoff;
        uint8_t* bitmap = stbtt_GetCodepointBitmap(
            &font->info, 0, scale, cp, &out_w, &out_h, &xoff, &yoff);
        if (bitmap) {
            for (int py = 0; py < out_h; py++) {
                for (int px = 0; px < out_w; px++) {
                    uint8_t cov = bitmap[py * out_w + px];
                    if (cov) {
                        cb(cx + xoff + px, baseline + yoff + py, cov, user);
                    }
                }
            }
            kfree(bitmap);
        }

        int step = (int)((float)adv * scale);
        if (str[1]) {
            int kern = stbtt_GetCodepointKernAdvance(&font->info, cp, (uint8_t)str[1]);
            step += (int)((float)kern * scale);
        }
        cx += step;
        str++;
    }

    return cx - x;
}

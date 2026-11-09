// ============================================================================
// compositor.c -- композитор окон AMPLIFY-OS x86_64 (этап 42).
// ----------------------------------------------------------------------------
// Реализует тройную буферизацию, per-pixel alpha compositing, улучшенные
// тени (3-pass box blur), скруглённые углы с AA, анимации открытия/закрытия
// и dirty-rect tracking для partial GPU flush.
// ============================================================================
#include "compositor.h"
#include "doublebuf.h"
#include "drm.h"
#include "fb.h"
#include "heap.h"
#include "pit.h"
#include "timer.h"
#include "ui_fx.h"
#include "ui_motion.h"
#include "serial.h"
#include "ui_theme.h"
#include "window.h"

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Внутреннее состояние
// ============================================================================

// Тройная буферизация: compose-буфер -- промежуточный слой, куда WM рисует
// текущий кадр. После завершения кадра он копируется в doublebuf (back),
// а doublebuf_swap отправляет на GPU/VESA (front).
// Зачем третий буфер: позволяет делать alpha-blend окон поверх фона без
// артефактов от предыдущего кадра.
static gfx_target_t g_compose = {0, 0, 0, 0};
static bool         g_active  = false;

// Shadow scratch buffer -- временный буфер для box-blur теней.
// Размер = максимальный размер тени (окно + blur padding).
// Аллоцируется один раз при init, переиспользуется.
#define SHADOW_MAX_W  400
#define SHADOW_MAX_H  400
static uint8_t* g_shadow_buf  = 0;   // alpha-only буфер для blur
static uint8_t* g_shadow_tmp  = 0;   // временный буфер для промежуточного pass

// Dirty rects.
static dirty_rect_t g_dirty[COMPOSITOR_MAX_DIRTY];
static int          g_dirty_count = 0;
static bool         g_full_dirty  = false;

// Анимации: храним состояние для каждого окна через массив (до 32 окон).
#define ANIM_MAX_WINDOWS 32
typedef struct {
    struct window* win;
    anim_state_t   state;
} anim_entry_t;

static anim_entry_t g_anims[ANIM_MAX_WINDOWS];
static int          g_anim_count = 0;

// Статистика.
static compositor_stats_t g_stats = {0};

// ============================================================================
// Инициализация
// ----------------------------------------------------------------------------
// HOTFIX (15.05.2026, реакция на регрессию мыши после этапа 42):
// Отказались от compose-буфера. Причина:
//   * desktop_draw() полностью перерисовывает экран каждый кадр (фон + звёзды
//     + логотип + иконки + статус-строка), поэтому третий буфер не даёт ни partial
//     flush, ни tearing-protection — он лишь добавляет 3 МБ memcpy на каждый
//     кадр (compose -> doublebuf), что на QEMU TCG растягивало кадр до
//     ~150 мс. За это время PS/2 8042 (single-byte input buffer) терял
//     пакеты мыши: курсор замирал после первого движения, в то время как
//     часы в статус-строке шли (они рисуются из RTC прямо в кадре).
//   * Старый partial-flush через dirty rects уже был выключен (см. коммент
//     в compositor_end_frame), а full-copy compose -> doublebuf — чистый
//     overhead.
// Теперь compositor_target() возвращает doublebuf_target(), а
// compositor_end_frame() сразу делает doublebuf_swap(). g_compose не
// используется и не аллоцируется. Это снижает CPU-нагрузку рендера примерно
// в 2 раза без потери визуального качества.
//
// Shadow scratch буферы оставлены — они нужны функции box-blur (доступна
// для будущего GPU-шейдера), но по умолчанию compositor_draw_shadow теперь
// уходит в быстрый gfx_drop_shadow (см. ниже).
// ============================================================================
bool compositor_init(void) {
    const gfx_target_t* db = doublebuf_target();
    if (!db) {
        serial_puts("[compositor] doublebuf not ready\n");
        return false;
    }

    // Compose-буфер БОЛЬШЕ НЕ ВЫДЕЛЯЕТСЯ — рисуем прямо в doublebuf.
    g_compose.pixels = 0;
    g_compose.width  = db->width;
    g_compose.height = db->height;
    g_compose.pitch  = db->pitch;

    // Shadow scratch buffers (оставлены под опциональный box-blur fallback).
    // Если этих буферов не хватит — compositor_draw_shadow пойдёт по
    // быстрому пути через gfx_drop_shadow, и это нормально.
    size_t shadow_sz = (size_t)SHADOW_MAX_W * SHADOW_MAX_H;
    g_shadow_buf = (uint8_t*)kmalloc(shadow_sz);
    g_shadow_tmp = (uint8_t*)kmalloc(shadow_sz);
    if (!g_shadow_buf || !g_shadow_tmp) {
        serial_puts("[compositor] shadow scratch alloc failed (non-fatal)\n");
    }

    g_active = true;
    g_anim_count = 0;

    serial_puts("[compositor] init OK: ");
    serial_print_dec(db->width); serial_puts("x"); serial_print_dec(db->height);
    serial_puts(" direct-doublebuf + fast shadow + AA\n");
    return true;
}

bool compositor_active(void) { return g_active; }

// [41.2] Буфер кадра сменил размер (drm_set_mode) — обновить границы обрезки.
void compositor_resize(int w, int h) {
    g_compose.width  = w;
    g_compose.height = h;
    g_compose.pitch  = w * 4;
}

// Compositor рисует напрямую в doublebuf — это исключает 3 МБ memcpy
// compose -> doublebuf каждый кадр (см. HOTFIX-комментарий выше).
const gfx_target_t* compositor_target(void) {
    return doublebuf_target();
}

// ============================================================================
// Dirty-rect tracking
// ============================================================================
void compositor_begin_frame(void) {
    g_dirty_count = 0;
    g_full_dirty  = false;
}

void compositor_mark_dirty(int x, int y, int w, int h) {
    if (g_full_dirty) return;
    if (w <= 0 || h <= 0) return;

    // Clip к экрану.
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_compose.width)  w = g_compose.width  - x;
    if (y + h > g_compose.height) h = g_compose.height - y;
    if (w <= 0 || h <= 0) return;

    if (g_dirty_count >= COMPOSITOR_MAX_DIRTY) {
        g_full_dirty = true;
        return;
    }
    g_dirty[g_dirty_count].x = x;
    g_dirty[g_dirty_count].y = y;
    g_dirty[g_dirty_count].w = w;
    g_dirty[g_dirty_count].h = h;
    g_dirty_count++;
}

void compositor_mark_full_dirty(void) {
    g_full_dirty = true;
}

// Объединить dirty-rect'ы в один bounding box (простая стратегия).
// [41.2] Используется compositor_frame_bbox для частичного вывода.
static void merge_dirty(int* rx, int* ry, int* rw, int* rh) {
    if (g_dirty_count == 0) { *rx = *ry = *rw = *rh = 0; return; }
    int x0 = g_dirty[0].x, y0 = g_dirty[0].y;
    int x1 = x0 + g_dirty[0].w, y1 = y0 + g_dirty[0].h;
    for (int i = 1; i < g_dirty_count; i++) {
        int dx0 = g_dirty[i].x, dy0 = g_dirty[i].y;
        int dx1 = dx0 + g_dirty[i].w, dy1 = dy0 + g_dirty[i].h;
        if (dx0 < x0) x0 = dx0;
        if (dy0 < y0) y0 = dy0;
        if (dx1 > x1) x1 = dx1;
        if (dy1 > y1) y1 = dy1;
    }
    *rx = x0; *ry = y0; *rw = x1 - x0; *rh = y1 - y0;
}

void compositor_end_frame(void) {
    // HOTFIX: compose-буфер больше не используется. Рендер шёл прямо в
    // doublebuf, поэтому здесь только swap. Это убирает 3 МБ memcpy/кадр.
    doublebuf_swap();
    g_stats.frames_total++;
    g_stats.frames_full++;
    g_stats.dirty_rects_total += (uint64_t)g_dirty_count;
}

// ----------------------------------------------------------------------------
// [41.2] Частичный вывод. WM сам решает, что изменилось на экране:
//   compositor_frame_bbox  — прямоугольник, накрывающий всё, что пометили
//                            compositor_mark_dirty за кадр (окна с тенями);
//   compositor_present_*   — вывести на экран весь буфер или его часть
//                            (virtio-gpu: TRANSFER+FLUSH области; VESA: копия);
//   compositor_frame_done  — учёт кадра в статистике compstat.
// Полный вывод 1920x1080 — это 8 МБ на TRANSFER_TO_HOST_2D (3-4 мс под KVM),
// поэтому всё, что можно, выводится прямоугольниками.
// ----------------------------------------------------------------------------
bool compositor_frame_bbox(int* x, int* y, int* w, int* h) {
    if (g_full_dirty) return false;
    merge_dirty(x, y, w, h);
    return true;
}

void compositor_present_full(void) { doublebuf_swap(); }

void compositor_present_rect(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_compose.width)  w = g_compose.width  - x;
    if (y + h > g_compose.height) h = g_compose.height - y;
    if (w <= 0 || h <= 0) return;
    doublebuf_swap_rect(x, y, w, h);
}

void compositor_frame_done(bool partial) {
    g_stats.frames_total++;
    if (partial) g_stats.frames_partial++; else g_stats.frames_full++;
    g_stats.dirty_rects_total += (uint64_t)g_dirty_count;
}

compositor_stats_t compositor_get_stats(void) { return g_stats; }

// ============================================================================
// Тени: 3-pass box blur (приближение Gaussian)
// ============================================================================
// HOTFIX: box-blur используется только при сборке с COMPOSITOR_HEAVY_SHADOW.
// По умолчанию (быстрый путь) эти функции не компилируются — чтобы GCC не
// сыпал «defined but not used» под -Wall -Wextra.
#ifdef COMPOSITOR_HEAVY_SHADOW

// Горизонтальный box-blur pass на alpha-буфере.
static void box_blur_h(uint8_t* src, uint8_t* dst, int w, int h, int r) {
    if (r <= 0) { for (int i = 0; i < w * h; i++) dst[i] = src[i]; return; }
    int diam = 2 * r + 1;
    for (int y = 0; y < h; y++) {
        int sum = 0;
        uint8_t* srow = src + y * w;
        uint8_t* drow = dst + y * w;
        // Начальная сумма: первые r+1 элементов (с зеркальным отражением).
        for (int i = -r; i <= r; i++) {
            int idx = i < 0 ? 0 : (i >= w ? w - 1 : i);
            sum += srow[idx];
        }
        drow[0] = (uint8_t)(sum / diam);
        for (int x = 1; x < w; x++) {
            int add_idx = x + r; if (add_idx >= w) add_idx = w - 1;
            int rem_idx = x - r - 1; if (rem_idx < 0) rem_idx = 0;
            sum += srow[add_idx] - srow[rem_idx];
            drow[x] = (uint8_t)(sum / diam);
        }
    }
}

// Вертикальный box-blur pass.
static void box_blur_v(uint8_t* src, uint8_t* dst, int w, int h, int r) {
    if (r <= 0) { for (int i = 0; i < w * h; i++) dst[i] = src[i]; return; }
    int diam = 2 * r + 1;
    for (int x = 0; x < w; x++) {
        int sum = 0;
        for (int i = -r; i <= r; i++) {
            int idx = i < 0 ? 0 : (i >= h ? h - 1 : i);
            sum += src[idx * w + x];
        }
        dst[x] = (uint8_t)(sum / diam);
        for (int y = 1; y < h; y++) {
            int add_idx = y + r; if (add_idx >= h) add_idx = h - 1;
            int rem_idx = y - r - 1; if (rem_idx < 0) rem_idx = 0;
            sum += src[add_idx * w + x] - src[rem_idx * w + x];
            dst[y * w + x] = (uint8_t)(sum / diam);
        }
    }
}

void compositor_draw_shadow(const gfx_target_t* tgt,
                            int x, int y, int w, int h,
                            int blur_r, int off_x, int off_y,
                            uint32_t shadow_color) {
    if (!tgt || w <= 0 || h <= 0) return;

    int pad = blur_r * 2;
    int sw = w + pad * 2;
    int sh = h + pad * 2;

    if (!g_shadow_buf || !g_shadow_tmp || sw > SHADOW_MAX_W || sh > SHADOW_MAX_H) {
        gfx_drop_shadow(tgt, x + off_x, y + off_y, w, h, blur_r, shadow_color);
        return;
    }

    int total = sw * sh;
    for (int i = 0; i < total; i++) g_shadow_buf[i] = 0;

    for (int sy = 0; sy < h; sy++) {
        for (int sx = 0; sx < w; sx++) {
            g_shadow_buf[(sy + pad) * sw + (sx + pad)] = 255;
        }
    }

    int pass_r = blur_r / 3;
    if (pass_r < 1) pass_r = 1;

    box_blur_h(g_shadow_buf, g_shadow_tmp, sw, sh, pass_r);
    box_blur_v(g_shadow_tmp, g_shadow_buf, sw, sh, pass_r);
    box_blur_h(g_shadow_buf, g_shadow_tmp, sw, sh, pass_r);
    box_blur_v(g_shadow_tmp, g_shadow_buf, sw, sh, pass_r);
    box_blur_h(g_shadow_buf, g_shadow_tmp, sw, sh, pass_r);
    box_blur_v(g_shadow_tmp, g_shadow_buf, sw, sh, pass_r);

    uint32_t sr = (shadow_color >> 16) & 0xFF;
    uint32_t sg = (shadow_color >>  8) & 0xFF;
    uint32_t sb =  shadow_color        & 0xFF;
    uint32_t sa = (shadow_color >> 24) & 0xFF;

    int dx = x + off_x - pad;
    int dy = y + off_y - pad;

    for (int sy = 0; sy < sh; sy++) {
        int ty = dy + sy;
        if ((unsigned)ty >= (unsigned)tgt->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)ty * tgt->pitch);
        for (int sx = 0; sx < sw; sx++) {
            int tx = dx + sx;
            if ((unsigned)tx >= (unsigned)tgt->width) continue;
            uint8_t alpha = g_shadow_buf[sy * sw + sx];
            if (alpha == 0) continue;
            uint32_t a = ((uint32_t)alpha * sa) / 255;
            if (a == 0) continue;
            uint32_t src_c = (a << 24) | (sr << 16) | (sg << 8) | sb;
            row[tx] = gfx_blend(row[tx], src_c);
        }
    }
}

#else  /* !COMPOSITOR_HEAVY_SHADOW — быстрый путь по умолчанию */

// HOTFIX (15.05.2026): по умолчанию используем gfx_drop_shadow вместо
// 6-проходного box blur. На QEMU TCG это сокращает per-окно стоимость
// тени с ~30–50 мс до ~200 мкс. Визуально drop_shadow (несколько
// концентрических прямоугольников с убывающей альфой) близок к мягкой
// тени Material/Win11; реальный Gaussian заработает, когда тень станет
// GPU-шейдером в будущих этапах фазы 3.
void compositor_draw_shadow(const gfx_target_t* tgt,
                            int x, int y, int w, int h,
                            int blur_r, int off_x, int off_y,
                            uint32_t shadow_color) {
    if (!tgt || w <= 0 || h <= 0) return;
    gfx_drop_shadow(tgt, x + off_x, y + off_y, w, h, blur_r, shadow_color);
}

#endif  /* COMPOSITOR_HEAVY_SHADOW */

// ============================================================================
// Скруглённые углы с антиалиасингом (subpixel coverage)
// ============================================================================

// Вычислить coverage пикселя на краю круга радиуса r.
// dx, dy -- расстояние от центра угла до центра пикселя.
// Возвращает 0..255 (0 = полностью вне, 255 = полностью внутри).
static uint8_t corner_coverage(int dx, int dy, int r) {
    // Расстояние от центра угла до центра пикселя (в 256-х долях).
    // Используем 4x4 supersampling для антиалиасинга.
    int inside = 0;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            // Субпиксельная позиция (в 1/8 пикселя от верхнего-левого угла).
            int px = dx * 8 + sx * 2 + 1;
            int py = dy * 8 + sy * 2 + 1;
            // Расстояние^2 от центра (в единицах 1/8 пикселя).
            int r8 = r * 8;
            if (px * px + py * py <= r8 * r8) inside++;
        }
    }
    return (uint8_t)((inside * 255) / 16);
}

void compositor_fill_round_rect_aa(const gfx_target_t* tgt,
                                   int x, int y, int w, int h,
                                   int radius, uint32_t color) {
    if (!tgt || w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    uint8_t ca = (color >> 24) & 0xFF;
    if (ca == 0) return;

    // Центральная полоса (без углов) -- полностью непрозрачная.
    for (int yy = y + radius; yy < y + h - radius; yy++) {
        if ((unsigned)yy >= (unsigned)tgt->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)yy * tgt->pitch);
        for (int xx = x; xx < x + w; xx++) {
            if ((unsigned)xx >= (unsigned)tgt->width) continue;
            if (ca == 255) row[xx] = color;
            else row[xx] = gfx_blend(row[xx], color);
        }
    }

    // Верхняя и нижняя полосы (без углов).
    for (int yy = y; yy < y + radius; yy++) {
        if ((unsigned)yy >= (unsigned)tgt->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)yy * tgt->pitch);
        for (int xx = x + radius; xx < x + w - radius; xx++) {
            if ((unsigned)xx >= (unsigned)tgt->width) continue;
            if (ca == 255) row[xx] = color;
            else row[xx] = gfx_blend(row[xx], color);
        }
    }
    for (int yy = y + h - radius; yy < y + h; yy++) {
        if ((unsigned)yy >= (unsigned)tgt->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)yy * tgt->pitch);
        for (int xx = x + radius; xx < x + w - radius; xx++) {
            if ((unsigned)xx >= (unsigned)tgt->width) continue;
            if (ca == 255) row[xx] = color;
            else row[xx] = gfx_blend(row[xx], color);
        }
    }

    // 4 угла с антиалиасингом.
    // Центры углов:
    int cx[4] = { x + radius,     x + w - 1 - radius, x + radius,     x + w - 1 - radius };
    int cy[4] = { y + radius,     y + radius,          y + h - 1 - radius, y + h - 1 - radius };
    // Направления (от центра к углу):
    int sx[4] = { -1, 1, -1, 1 };
    int sy[4] = { -1, -1, 1, 1 };

    for (int corner = 0; corner < 4; corner++) {
        for (int dy = 0; dy < radius; dy++) {
            int py = cy[corner] + sy[corner] * dy;
            if ((unsigned)py >= (unsigned)tgt->height) continue;
            uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)py * tgt->pitch);
            for (int dx = 0; dx < radius; dx++) {
                int px = cx[corner] + sx[corner] * dx;
                if ((unsigned)px >= (unsigned)tgt->width) continue;
                uint8_t cov = corner_coverage(dx, dy, radius);
                if (cov == 0) continue;
                // Модулируем alpha цвета на coverage.
                uint32_t a = ((uint32_t)ca * cov) / 255;
                uint32_t blended = (a << 24) | (color & 0x00FFFFFFu);
                row[px] = gfx_blend(row[px], blended);
            }
        }
    }
}

void compositor_stroke_round_rect_aa(const gfx_target_t* tgt,
                                     int x, int y, int w, int h,
                                     int radius, uint32_t color) {
    if (!tgt || w <= 0 || h <= 0) return;
    if (radius < 0) radius = 0;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;

    // Верхняя и нижняя горизонтальные линии (без углов).
    for (int xx = x + radius; xx < x + w - radius; xx++) {
        if ((unsigned)xx >= (unsigned)tgt->width) continue;
        if ((unsigned)y < (unsigned)tgt->height) {
            uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)y * tgt->pitch);
            row[xx] = gfx_blend(row[xx], color);
        }
        int by = y + h - 1;
        if ((unsigned)by < (unsigned)tgt->height) {
            uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)by * tgt->pitch);
            row[xx] = gfx_blend(row[xx], color);
        }
    }
    // Левая и правая вертикальные линии (без углов).
    for (int yy = y + radius; yy < y + h - radius; yy++) {
        if ((unsigned)yy >= (unsigned)tgt->height) continue;
        uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)yy * tgt->pitch);
        if ((unsigned)x < (unsigned)tgt->width)
            row[x] = gfx_blend(row[x], color);
        int rx = x + w - 1;
        if ((unsigned)rx < (unsigned)tgt->width)
            row[rx] = gfx_blend(row[rx], color);
    }

    // Углы: рисуем дугу через circle-test с AA.
    int cx_arr[4] = { x + radius, x + w - 1 - radius, x + radius, x + w - 1 - radius };
    int cy_arr[4] = { y + radius, y + radius, y + h - 1 - radius, y + h - 1 - radius };

    for (int corner = 0; corner < 4; corner++) {
        int ccx = cx_arr[corner];
        int ccy = cy_arr[corner];
        for (int dy = -radius; dy <= 0; dy++) {
            for (int dx = -radius; dx <= 0; dx++) {
                // Проверяем, лежит ли пиксель на окружности (толщина ~1px).
                int d2 = dx * dx + dy * dy;
                int r2_inner = (radius - 1) * (radius - 1);
                int r2_outer = (radius + 1) * (radius + 1);
                if (d2 < r2_inner || d2 > r2_outer) continue;

                // Простой AA: расстояние от идеальной окружности.
                // dist = |sqrt(d2) - radius|, coverage = 1 - dist.
                // Аппроксимация без sqrt: используем линейную интерполяцию.
                int r2 = radius * radius;
                int diff = d2 - r2;
                if (diff < 0) diff = -diff;
                int max_diff = radius * 2;
                if (max_diff == 0) max_diff = 1;
                int alpha_scale = 255 - (diff * 255) / max_diff;
                if (alpha_scale < 0) alpha_scale = 0;
                if (alpha_scale > 255) alpha_scale = 255;

                uint32_t a = ((color >> 24) & 0xFF);
                a = (a * (uint32_t)alpha_scale) / 255;
                uint32_t c = (a << 24) | (color & 0x00FFFFFFu);

                // Зеркалим по 4 квадрантам.
                int px, py;
                // Определяем позицию в зависимости от угла.
                if (corner == 0) { px = ccx + dx; py = ccy + dy; }
                else if (corner == 1) { px = ccx - dx; py = ccy + dy; }
                else if (corner == 2) { px = ccx + dx; py = ccy - dy; }
                else { px = ccx - dx; py = ccy - dy; }

                if ((unsigned)px >= (unsigned)tgt->width) continue;
                if ((unsigned)py >= (unsigned)tgt->height) continue;
                uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)py * tgt->pitch);
                row[px] = gfx_blend(row[px], c);
            }
        }
    }
}

// ============================================================================
// Анимации
// ============================================================================

static anim_entry_t* find_anim(struct window* w) {
    for (int i = 0; i < g_anim_count; i++) {
        if (g_anims[i].win == w) return &g_anims[i];
    }
    return 0;
}

static anim_entry_t* alloc_anim(struct window* w) {
    // Ищем существующий слот.
    anim_entry_t* e = find_anim(w);
    if (e) return e;
    // Ищем свободный (done).
    for (int i = 0; i < g_anim_count; i++) {
        if (g_anims[i].state.done) {
            g_anims[i].win = w;
            return &g_anims[i];
        }
    }
    // Новый слот.
    if (g_anim_count >= ANIM_MAX_WINDOWS) return 0;
    g_anims[g_anim_count].win = w;
    return &g_anims[g_anim_count++];
}

void compositor_anim_open(struct window* w) {
    if (!w) return;
    if (ui_theme_reduced_motion()) { w->opacity = 255; return; }
    anim_entry_t* e = alloc_anim(w);
    if (!e) return;
    e->state.type       = ANIM_OPEN;
    e->state.start_ms   = timer_get_uptime_ms();
    e->state.duration_ms = (w->chrome == WINDOW_CHROME_CLASSIC) ? ANIM_OPEN_DURATION_MS
                                                                : ANIM_WS_OPEN_DURATION_MS;
    e->state.duration_ms *= ui_motion_slow();
    e->state.progress   = 0;
    e->state.done       = false;
    e->state.snapped    = false;
    e->state.frames     = 0;
    g_stats.anims_started++;
}

void compositor_anim_close(struct window* w) {
    if (!w) return;
    if (ui_theme_reduced_motion()) {
        w->visible = false;
        w->close_requested = true;
        return;
    }
    anim_entry_t* e = alloc_anim(w);
    if (!e) return;
    e->state.type       = ANIM_CLOSE;
    e->state.start_ms   = timer_get_uptime_ms();
    e->state.duration_ms = (w->chrome == WINDOW_CHROME_CLASSIC) ? ANIM_CLOSE_DURATION_MS
                                                                : ANIM_WS_CLOSE_DURATION_MS;
    e->state.duration_ms *= ui_motion_slow();
    e->state.progress   = 0;
    e->state.done       = false;
    e->state.snapped    = false;
    e->state.frames     = 0;
    g_stats.anims_started++;
}

bool compositor_update_animations(void) {
    bool any_active = false;
    uint64_t now = timer_get_uptime_ms();

    for (int i = 0; i < g_anim_count; i++) {
        anim_entry_t* e = &g_anims[i];
        if (e->state.done || e->state.type == ANIM_NONE) continue;

        uint32_t elapsed_ms = (uint32_t)(now - e->state.start_ms);
        uint32_t dur = e->state.duration_ms;

        if (elapsed_ms >= dur) {
            e->state.progress = 255;
            e->state.done = true;
            g_stats.anims_completed++;
            if (e->win)
                fx_log_anim("window", e->win->title,
                            e->state.type == ANIM_OPEN ? "open" : "close",
                            e->state.frames, elapsed_ms);

            // Для close-анимации: скрываем окно после завершения.
            if (e->state.type == ANIM_CLOSE && e->win) {
                e->win->visible = false;
                e->win->close_requested = true;
            }
        } else {
            e->state.progress = (uint8_t)((elapsed_ms * 255) / dur);
            any_active = true;
        }
    }
    return any_active;
}

bool compositor_is_animating(struct window* w) {
    anim_entry_t* e = find_anim(w);
    if (!e) return false;
    return !e->state.done && e->state.type != ANIM_NONE;
}

// Read-only обход слотов: главный цикл WM использует эту функцию, чтобы
// решить «нужен ли redraw из-за анимации», не двигая progress.
// До HOTFIX'а wm_main_loop вызывал compositor_update_animations() и в
// своём цикле, и внутри render_frame — анимации шли в 2x скорости.
bool compositor_peek_animation_active(void) {
    for (int i = 0; i < g_anim_count; i++) {
        const anim_entry_t* e = &g_anims[i];
        if (e->state.done || e->state.type == ANIM_NONE) continue;
        return true;
    }
    return false;
}

uint8_t compositor_anim_progress(struct window* w) {
    anim_entry_t* e = find_anim(w);
    if (!e) return 255;
    if (e->state.done) return 255;
    return e->state.progress;
}

// Ease-out cubic: t -> 1 - (1-t)^3. Вход/выход 0..255.
static uint8_t ease_out(uint8_t t) {
    uint32_t inv = 255 - t;
    uint32_t inv3 = (inv * inv * inv) / (255 * 255);
    return (uint8_t)(255 - inv3);
}

// [48.2] Ease-out-back: значение перелетает цель на ~10 % и возвращается —
// окно «выпрыгивает» чуть больше своего размера и садится на место.
// Вход 0..255, выход в тысячных (0..~1100):
//   f(x) = 1 + c3*(x-1)^3 + c1*(x-1)^2,  c1 = 1.70158, c3 = c1 + 1.
static int ease_out_back_1000(uint8_t t) {
    int u = (int)t - 255;                               // (x-1) * 255, -255..0
    int64_t u2 = (int64_t)u * u;                        // * 255^2
    int64_t u3 = u2 * u;                                // * 255^3
    int64_t v = 1000 + (2702 * u3) / (255LL * 255 * 255) + (1702 * u2) / (255LL * 255);
    return (int)v;
}

// ============================================================================
// Рендеринг окна с эффектами
// ============================================================================

// Blit буфера окна в target с учётом opacity (per-window alpha).
static void blit_with_opacity(const gfx_target_t* dst, int dx, int dy,
                              const gfx_target_t* src, int sw, int sh,
                              uint8_t opacity) {
    if (!dst || !src) return;

    // HOTFIX (19.05.2026, KVM perf): прежний код делал bounds-check per-pixel
    // (60000 проверок на 300x200 окно). Pre-clip убирает их вне inner loop.
    // Плюс fast-path для opacity=255 + alpha=255 — это 95% случаев (обычные
    // непрозрачные окна), сводится к простому row-копированию.
    int x0 = dx, y0 = dy, x1 = dx + sw, y1 = dy + sh;
    int sx0 = 0,  sy0 = 0;
    if (x0 < 0) { sx0 = -x0; x0 = 0; }
    if (y0 < 0) { sy0 = -y0; y0 = 0; }
    if (x1 > dst->width)  x1 = dst->width;
    if (y1 > dst->height) y1 = dst->height;
    if (x1 <= x0 || y1 <= y0) return;
    int cw = x1 - x0;
    int ch = y1 - y0;

    if (opacity == 255) {
        // Fast path: opacity=255 → блит зависит только от per-pixel alpha.
        for (int yy = 0; yy < ch; yy++) {
            uint32_t* drow = (uint32_t*)((uint8_t*)dst->pixels +
                                         (size_t)(y0 + yy) * dst->pitch) + x0;
            uint32_t* srow = (uint32_t*)((uint8_t*)src->pixels +
                                         (size_t)(sy0 + yy) * src->pitch) + sx0;
            for (int xx = 0; xx < cw; xx++) {
                uint32_t s = srow[xx];
                uint32_t sa = (s >> 24);
                if (sa == 0) continue;
                if (sa == 255) drow[xx] = s;
                else drow[xx] = gfx_blend(drow[xx], s);
            }
        }
        return;
    }

    // Slow path: модулируем alpha на opacity окна.
    for (int yy = 0; yy < ch; yy++) {
        uint32_t* drow = (uint32_t*)((uint8_t*)dst->pixels +
                                     (size_t)(y0 + yy) * dst->pitch) + x0;
        uint32_t* srow = (uint32_t*)((uint8_t*)src->pixels +
                                     (size_t)(sy0 + yy) * src->pitch) + sx0;
        for (int xx = 0; xx < cw; xx++) {
            uint32_t s = srow[xx];
            uint32_t sa = (s >> 24);
            if (sa == 0) continue;
            sa = (sa * (uint32_t)opacity) / 255;
            s  = (sa << 24) | (s & 0x00FFFFFFu);
            if (sa == 255) drow[xx] = s;
            else drow[xx] = gfx_blend(drow[xx], s);
        }
    }
}

// [48.2] Заморозка содержимого (см. compositor.h). Анимация оверлея или
// соседнего окна — это до 20 кадров сцены подряд; перерисовывать в каждом из
// них содержимое неподвижных окон (workspace-окно на 1080p — 6–8 мс) значит
// выпасть из 16 мс на кадр.
static bool g_frozen = false;
void compositor_set_frozen(bool frozen) { g_frozen = frozen; }

static void draw_content(struct window* w) {
    if (!w->on_draw) return;
    if (g_frozen && w->rendered && !w->wants_continuous_redraw) return;
    w->on_draw(w, &w->target);
    w->rendered = true;
}

void compositor_render_window(struct window* w, const gfx_target_t* tgt) {
    if (!w || !w->visible || !tgt) return;

    int title_h = window_title_height(w);
    int total_h = title_h + w->height;
    int win_x = w->x;
    int win_y = w->y;
    int win_w = w->width;
    int win_h = total_h;
    uint8_t opacity = w->opacity;

    // Анимация: модифицируем позицию, размер и opacity.
    anim_entry_t* anim = find_anim(w);
    bool animating = (anim && !anim->state.done && anim->state.type != ANIM_NONE);

    if (animating) {
        uint8_t raw_t = anim->state.progress;
        uint8_t t = ease_out(raw_t);

        if (anim->state.type == ANIM_OPEN) {
            // [48.2] Масштаб 0.84 -> 1.0 с небольшим перелётом (ease-out-back),
            // прозрачность набирается за первую половину пути.
            int scale = 840 + 160 * ease_out_back_1000(raw_t) / 1000;
            int new_w = (win_w * scale) / 1000;
            int new_h = (win_h * scale) / 1000;
            win_x += (win_w - new_w) / 2;
            win_y += (win_h - new_h) / 2;
            win_w = new_w;
            win_h = new_h;
            uint32_t op = (uint32_t)t * 2;
            if (op > 255) op = 255;
            opacity = (uint8_t)(((uint32_t)w->opacity * op) / 255);
        } else if (anim->state.type == ANIM_CLOSE) {
            // Scale: 1.0 -> 0.92, opacity: window.opacity -> 0
            uint8_t inv_t = 255 - t;
            uint32_t scale = 920 + ((uint32_t)inv_t * 80) / 255;  // 1000..920
            int new_w = (win_w * (int)scale) / 1000;
            int new_h = (win_h * (int)scale) / 1000;
            win_x += (win_w - new_w) / 2;
            win_y += (win_h - new_h) / 2;
            win_w = new_w;
            win_h = new_h;
            opacity = (uint8_t)(((uint32_t)w->opacity * inv_t) / 255);
        }
    }

    // Stage B: borderless/workspace surfaces bypass legacy blue title bars.
    // They still participate in the same ease-out scale/fade animation.
    if (w->chrome != WINDOW_CHROME_CLASSIC) {
        if (animating) {
            // [48.2] Workspace-окно на 1080p рисует себя 6–8 мс. Гонять on_draw
            // на каждом шаге — значит не уложиться в кадр, поэтому содержимое
            // снимается один раз (буфер окна и есть снимок), а дальше
            // анимируется готовая картинка: окно поднимается снизу, проявляясь
            // (при закрытии — опускается и тает). Без масштабирования: текст
            // остаётся чётким, а смешивание — простой SSE-проход по строкам.
            if (!anim->state.snapped) {
                if (w->on_draw) { w->on_draw(w, &w->target); w->rendered = true; }
                anim->state.snapped = true;
            }
            anim->state.frames++;
            uint8_t e = ease_out(anim->state.progress);
            int travel = tgt->height / 18;                 // 60 px на 1080p
            int dy; uint32_t op;
            if (anim->state.type == ANIM_OPEN) {
                dy = travel * (255 - e) / 255;
                op = (uint32_t)e * 5 / 3;                  // к 60 % пути окно уже плотное
            } else {
                dy = (travel * 2 / 3) * e / 255;
                op = (uint32_t)(255 - e);
            }
            if (op > 255) op = 255;
            op = op * w->opacity / 255;
            compositor_mark_full_dirty();
            fx_blit(tgt, w->x, w->y + dy, w->buffer, w->width, w->width, w->height,
                    (uint8_t)op, false);
            return;
        }
        compositor_mark_dirty(win_x - 8, win_y - 8, win_w + 16, win_h + 16);
        draw_content(w);
        gfx_target_t content_tgt = { w->buffer, w->width, w->height, w->width * 4 };
        blit_with_opacity(tgt, win_x, win_y, &content_tgt,
                          w->width, w->height, opacity);
        return;
    }
    if (animating) anim->state.frames++;

    // Mark dirty: тень + окно.
    int shadow_pad = 16;
    compositor_mark_dirty(win_x - shadow_pad, win_y - shadow_pad,
                          win_w + shadow_pad * 2, win_h + shadow_pad * 2 + 8);

    // 1) Тень (Gaussian-подобная).
    if (opacity > 30) {
        uint32_t shadow_alpha = w->focused ? 180 : 120;
        shadow_alpha = (shadow_alpha * opacity) / 255;
        compositor_draw_shadow(tgt, win_x, win_y, win_w, win_h,
                               8, 3, 5,
                               GFX_ARGB((uint8_t)shadow_alpha, 0, 0, 0));
    }

    // Если анимация масштабирует -- рисуем через scaled blit.
    if (animating && (win_w != w->width || win_h != total_h)) {
        // Рисуем окно во временный буфер (используем window buffer + title).
        // Для простоты: рисуем title bar + content в compose, потом scale.
        // Упрощение: рисуем title bar прямо в target со scale.

        // Title bar (скруглённые верхние углы).
        uint32_t title_bg = w->focused ? w->accent_color : 0xFF24345A;
        uint32_t title_with_alpha = (((uint32_t)opacity) << 24) | (title_bg & 0x00FFFFFFu);
        int title_h_scaled = (WINDOW_TITLE_HEIGHT * win_h) / total_h;
        compositor_fill_round_rect_aa(tgt, win_x, win_y, win_w, title_h_scaled,
                                      UI_RADIUS_M, title_with_alpha);

        // Content area (скруглённые нижние углы).
        int content_y = win_y + title_h_scaled;
        int content_h = win_h - title_h_scaled;
        uint32_t bg_with_alpha = (((uint32_t)opacity) << 24) | (w->bg_color & 0x00FFFFFFu);
        compositor_fill_round_rect_aa(tgt, win_x, content_y, win_w, content_h,
                                      UI_RADIUS_M, bg_with_alpha);

        // Blit content scaled. [48.2] Содержимое снимается один раз за
        // анимацию; масштабирование — быстрым fx_blit_scaled.
        if (!anim->state.snapped) {
            if (w->on_draw) { w->on_draw(w, &w->target); w->rendered = true; }
            anim->state.snapped = true;
        }
        fx_blit_scaled(tgt, win_x, content_y, win_w, content_h,
                       w->buffer, w->width, w->width, w->height, opacity, false);

        // Border.
        uint32_t border_c = GFX_ARGB(opacity, 0x60, 0xD0, 0xF0);
        compositor_stroke_round_rect_aa(tgt, win_x, win_y, win_w, win_h,
                                        UI_RADIUS_M, border_c);
        return;
    }

    // Нормальный рендеринг (без масштабирования).

    // 2) Title bar со скруглёнными верхними углами.
    uint32_t title_bg = w->focused ? w->accent_color : 0xFF24345A;
    if (opacity < 255) {
        title_bg = (((uint32_t)opacity) << 24) | (title_bg & 0x00FFFFFFu);
    }

    // Верхняя часть (title) -- скруглённые верхние углы.
    // Рисуем полный скруглённый rect, потом перекрываем нижнюю часть content'ом.
    compositor_fill_round_rect_aa(tgt, win_x, win_y, win_w, WINDOW_TITLE_HEIGHT + UI_RADIUS_M,
                                  UI_RADIUS_M, title_bg);
    // Перекрываем нижнюю часть title (чтобы углы были только сверху).
    uint32_t title_solid = title_bg;
    gfx_fill_rect(tgt, win_x, win_y + WINDOW_TITLE_HEIGHT - 2, win_w, UI_RADIUS_M + 2, title_solid);

    // 3) Title text.
    if (opacity > 128) {
        uint32_t text_fg = GFX_ARGB(opacity, 0xFF, 0xFF, 0xFF);
        gfx_draw_string(tgt, win_x + 10, win_y + 6, w->title, text_fg, title_bg);
    }

    // 4) Close button (красный круг с крестом).
    int bx = win_x + win_w - WINDOW_CLOSE_BTN - 8;
    int by = win_y + (WINDOW_TITLE_HEIGHT - WINDOW_CLOSE_BTN) / 2;
    uint32_t close_bg = GFX_ARGB(opacity, 0xFF, 0x50, 0x50);
    compositor_fill_round_rect_aa(tgt, bx, by, WINDOW_CLOSE_BTN, WINDOW_CLOSE_BTN,
                                  WINDOW_CLOSE_BTN / 2, close_bg);
    // Крест.
    if (opacity > 100) {
        uint32_t cross_c = GFX_ARGB(opacity, 0xFF, 0xFF, 0xFF);
        for (int i = 3; i < WINDOW_CLOSE_BTN - 3; i++) {
            gfx_put_pixel(tgt, bx + i, by + i, cross_c);
            gfx_put_pixel(tgt, bx + WINDOW_CLOSE_BTN - 1 - i, by + i, cross_c);
        }
    }

    // 5) Content area (скруглённые нижние углы).
    int content_y = win_y + WINDOW_TITLE_HEIGHT;
    uint32_t bg_c = w->bg_color;
    if (opacity < 255) bg_c = (((uint32_t)opacity) << 24) | (bg_c & 0x00FFFFFFu);

    // Нижняя часть -- скруглённые нижние углы.
    compositor_fill_round_rect_aa(tgt, win_x, content_y - UI_RADIUS_M,
                                  win_w, w->height + UI_RADIUS_M,
                                  UI_RADIUS_M, bg_c);
    // Перекрываем верхнюю часть content (чтобы углы были только снизу).
    gfx_fill_rect(tgt, win_x, content_y, win_w, UI_RADIUS_M, bg_c);

    // 6) Application content.
    draw_content(w);

    // Blit content buffer с opacity.
    gfx_target_t content_tgt = { w->buffer, w->width, w->height, w->width * 4 };
    blit_with_opacity(tgt, win_x, content_y, &content_tgt, w->width, w->height, opacity);

    // 7) Border (скруглённый, с AA).
    uint32_t border_c = w->focused
        ? GFX_ARGB(opacity, 0x60, 0xD0, 0xF0)
        : GFX_ARGB((uint8_t)(opacity / 2), 0x40, 0x60, 0x90);
    compositor_stroke_round_rect_aa(tgt, win_x, win_y, win_w, total_h,
                                    UI_RADIUS_M, border_c);
}

// ----------------------------------------------------------------------------
// Этап F: WM теперь реально удаляет закрытые окна (window_destroy).
// Композитор держит указатели window в слотах анимаций — сбрасываем.
// ----------------------------------------------------------------------------
void compositor_forget_window(struct window* w) {
    if (!w) return;
    for (int i = 0; i < g_anim_count; i++) {
        if (g_anims[i].win != w) continue;
        g_anims[i].win            = 0;
        g_anims[i].state.type     = ANIM_NONE;
        g_anims[i].state.done     = true;
        g_anims[i].state.progress = 255;
    }
}

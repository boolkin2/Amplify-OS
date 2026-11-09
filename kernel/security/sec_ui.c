// ============================================================================
// sec_ui.c - Реализация мини-UI-toolkit'а Этапа 32
// ============================================================================
// Использует gfx_*/font_*/doublebuf_* из ядра. Не запускает wm_update();
// рисует напрямую в back-буфер и презентует через doublebuf_swap().
// ============================================================================

#include "sec_ui.h"
#include "../gui/graphics.h"
#include "../gui/font.h"
#include "../gui/framebuffer.h"
#include "../gui/doublebuf.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"

// ----------------------------------------------------------------------------
// Строковые помощники
// ----------------------------------------------------------------------------

int secui_strlen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

int secui_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void secui_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// ----------------------------------------------------------------------------
// Фон: вертикальный градиент + редкий точечный «звёздный» паттерн.
// ----------------------------------------------------------------------------

void secui_draw_background(int frame_phase) {
    int32_t w = framebuffer_get_width();
    int32_t h = framebuffer_get_height();
    gfx_fill_rect_gradient(0, 0, w, h, SECUI_BG_TOP, SECUI_BG_BOTTOM);

    // Слабые «звёзды» — детерминированные точки, лёгкое мерцание от phase.
    for (int i = 0; i < 200; i++) {
        int x = ( (i * 1103515245 + 12345) & 0x7FFFFFFF ) % w;
        int y = ( (i * 22695477   + 1)     & 0x7FFFFFFF ) % h;
        uint8_t a = (uint8_t)(40 + ((i + frame_phase) & 31));
        uint32_t c = (uint32_t)a;
        c |= c << 8;
        c |= c << 16;
        c &= 0x00FFFFFFu;
        gfx_put_pixel(x, y, c);
    }
}

// ----------------------------------------------------------------------------
// Панель с тенью и закруглением (имитированным рамкой 1px).
// ----------------------------------------------------------------------------

void secui_draw_panel(int32_t x, int32_t y, int32_t w, int32_t h,
                      const char* title, const char* subtitle) {
    // Мягкая тень (3 уровня).
    for (int d = 1; d <= 6; d++) {
        uint32_t c = 0x00000010u + (uint32_t)(d * 0x040404u);
        gfx_fill_rect(x + d, y + d, w, h, c);
    }
    gfx_fill_rect(x, y, w, h, SECUI_PANEL);
    gfx_draw_rect(x, y, w, h, SECUI_PANEL_BORDER);
    // Внутренняя «штриховая» рамка.
    gfx_draw_rect(x + 2, y + 2, w - 4, h - 4, 0xFF2A3868u);

    if (title) {
        font_draw_string(x + 24, y + 18, title, SECUI_TITLE_FG, 0xFF000000);
        gfx_hline(x + 24, y + 18 + FONT_HEIGHT + 4, w - 48, SECUI_PANEL_BORDER);
    }
    if (subtitle) {
        font_draw_string(x + 24, y + 18 + FONT_HEIGHT + 12,
                         subtitle, SECUI_TEXT_DIM, 0xFF000000);
    }
}

// ----------------------------------------------------------------------------
// Кнопка: градиент + текст + подсветка при hover.
// ----------------------------------------------------------------------------

void secui_button_draw(const secui_button_t* b, bool hovered) {
    if (!b) return;

    uint32_t top = b->enabled ? SECUI_BTN_TOP    : SECUI_BTN_DISABLED;
    uint32_t bot = b->enabled ? SECUI_BTN_BOTTOM : SECUI_BTN_DISABLED;
    if (hovered && b->enabled) {
        // Чуть светлее.
        top = 0xFF60C0FFu;
        bot = 0xFF2080E0u;
    }
    gfx_fill_rect_gradient(b->x, b->y, b->w, b->h, top, bot);
    gfx_draw_rect(b->x, b->y, b->w, b->h, 0xFF002040u);

    // Лейбл по центру.
    int lw = b->label ? secui_strlen(b->label) * FONT_WIDTH : 0;
    int lx = b->x + (b->w - lw) / 2;
    int ly = b->y + (b->h - FONT_HEIGHT) / 2;
    font_draw_string(lx, ly, b->label ? b->label : "",
                     SECUI_BTN_FG, 0xFF000000);
}

// ----------------------------------------------------------------------------
// Поле ввода
// ----------------------------------------------------------------------------

void secui_field_draw(const secui_field_t* f) {
    if (!f) return;
    gfx_fill_rect(f->x, f->y, f->w, f->h, SECUI_INPUT_BG);
    gfx_draw_rect(f->x, f->y, f->w, f->h,
                  f->focused ? SECUI_ACCENT : SECUI_INPUT_BORDER);

    // Текст.
    int tx = f->x + 8;
    int ty = f->y + (f->h - FONT_HEIGHT) / 2;

    if (f->cursor == 0 && f->placeholder && !f->focused) {
        font_draw_string(tx, ty, f->placeholder, SECUI_TEXT_DIM, 0xFF000000);
    } else {
        // Если password — печатаем '*' столько же, сколько символов.
        int max_chars = (f->w - 16) / FONT_WIDTH;
        int len = f->cursor;
        if (len > max_chars) len = max_chars;

        for (int i = 0; i < len; i++) {
            char ch = (f->kind == SECUI_FIELD_PASSWORD) ? '*' : f->buffer[i];
            font_draw_char(tx + i * FONT_WIDTH, ty, ch,
                           SECUI_INPUT_FG, 0xFF000000);
        }

        // Каретка (если фокус).
        if (f->focused) {
            int cx = tx + len * FONT_WIDTH;
            gfx_vline(cx, ty - 2, FONT_HEIGHT + 4, SECUI_ACCENT);
        }
    }
}

bool secui_field_handle(secui_field_t* f, int ch) {
    if (!f || !f->focused) return false;

    if (ch == 0) return false;
    if (ch == '\r' || ch == '\n') {
        return true;
    }
    if (ch == '\b' || ch == 0x7F) {
        if (f->cursor > 0) {
            f->cursor--;
            f->buffer[f->cursor] = '\0';
        }
        return false;
    }
    if (ch >= 0x20 && ch < 0x7F) {
        if (f->cursor < f->buffer_size - 1) {
            f->buffer[f->cursor++] = (char)ch;
            f->buffer[f->cursor]   = '\0';
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// Презентация кадра
// ----------------------------------------------------------------------------

void secui_present(void) {
    doublebuf_swap();
}

// ----------------------------------------------------------------------------
// Опрос клавиатуры (неблокирующий).
// keyboard_getchar_nonblock возвращает 0 если нет символа.
// ----------------------------------------------------------------------------

int secui_kb_poll(void) {
    char c = keyboard_getchar_nonblock();
    return (int)(unsigned char)c;
}

// ----------------------------------------------------------------------------
// Спиннер: 3 точки, мигают по очереди.
// ----------------------------------------------------------------------------

void secui_draw_spinner(int32_t cx, int32_t cy, int frame_phase) {
    int phase = (frame_phase / 12) % 3;
    for (int i = 0; i < 3; i++) {
        uint32_t c = (i == phase) ? SECUI_ACCENT : SECUI_PANEL_BORDER;
        gfx_fill_circle(cx + (i - 1) * 14, cy, 4, c);
    }
}

// ----------------------------------------------------------------------------
// Centered text
// ----------------------------------------------------------------------------

void secui_text_centered(int32_t x, int32_t y, int32_t w, const char* s,
                         uint32_t fg) {
    int sw = secui_strlen(s) * FONT_WIDTH;
    font_draw_string(x + (w - sw) / 2, y, s, fg, 0xFF000000);
}

// ----------------------------------------------------------------------------
// Большой текст: используем bitmap-шрифт font_8x16 и масштабируем 2× (или N×).
// Делаем «руками», чтобы не привязываться к опциональному TTF.
// ----------------------------------------------------------------------------

void secui_text_big(int32_t x, int32_t y, const char* s, uint32_t fg, int scale) {
    if (!s || scale < 1) scale = 1;
    extern const uint8_t font_8x16[];
    int cx = x;
    for (int i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        const uint8_t* glyph = &font_8x16[ch * FONT_HEIGHT];
        for (int row = 0; row < FONT_HEIGHT; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_WIDTH; col++) {
                if (bits & (0x80 >> col)) {
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            gfx_put_pixel(cx + col * scale + dx,
                                          y  + row * scale + dy, fg);
                        }
                    }
                }
            }
        }
        cx += FONT_WIDTH * scale;
    }
}

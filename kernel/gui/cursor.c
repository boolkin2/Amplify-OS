// ============================================================================
// cursor.c - Реализация графического курсора мыши (Этап 22)
// ============================================================================
// Для устранения мерцания курсор теперь рисуется в back-buffer в рамках
// композитинга, а не напрямую в framebuffer из IRQ. IRQ мыши лишь обновляет
// координаты и просит WM перерисовать кадр (cursor_request_redraw).
// ============================================================================

#include "cursor.h"
#include "framebuffer.h"
#include "doublebuf.h"
#include "wm.h"

// ============================================================================
// Дизайн курсора (стандартная стрелка)
// ============================================================================

#define CURSOR_WIDTH 12
#define CURSOR_HEIGHT 19

// 0 = прозрачный, 1 = черная рамка, 2 = белая заливка
static const uint8_t cursor_bitmap[CURSOR_HEIGHT][CURSOR_WIDTH] = {
    {1,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,1,1,1,1,1,1},
    {1,2,2,1,2,2,1,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0},
    {1,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0},
    {0,0,0,0,0,0,1,2,1,0,0,0},
    {0,0,0,0,0,0,1,1,1,0,0,0}
};

// ============================================================================
// Внутреннее состояние
// ============================================================================

static int32_t cursor_x = 0;
static int32_t cursor_y = 0;
static bool cursor_initialized = false;

// ============================================================================
// Отрисовка курсора в произвольный 32-bpp буфер (back-buffer или прямо в
// framebuffer). Делает только запись небольшого количества пикселей (~100),
// поэтому дешевле любого цикла по всей сцене.
// ============================================================================
static void draw_cursor_to(uint32_t* buf, uint32_t pitch_px,
                           int32_t fbw, int32_t fbh,
                           int32_t x, int32_t y) {
    if (!buf) return;
    for (int32_t r = 0; r < CURSOR_HEIGHT; r++) {
        int32_t py = y + r;
        if (py < 0 || py >= fbh) continue;
        uint32_t* row = buf + (uint32_t)py * pitch_px;

        for (int32_t c = 0; c < CURSOR_WIDTH; c++) {
            int32_t px = x + c;
            if (px < 0 || px >= fbw) continue;

            uint8_t pixel = cursor_bitmap[r][c];
            if (pixel == 0) continue;
            uint32_t color = (pixel == 1) ? 0xFF000000 : 0xFFFFFFFF;
            row[px] = color;
        }
    }
}

// ============================================================================
// Внешний API
// ============================================================================

void cursor_init(void) {
    if (framebuffer_is_initialized()) {
        cursor_x = framebuffer_get_width() / 2;
        cursor_y = framebuffer_get_height() / 2;
        cursor_initialized = true;
    }
}

// Вызывается из прерывания мыши. НЕ рисует напрямую во фреймбуфер:
// только сохраняет координаты и помечает кадр как требующий презентации
// (НЕ полной пересборки сцены — это сделало бы мышь дёрганой). Композитор
// возьмёт кэш сцены и наложит курсор поверх за один memcpy.
void cursor_update(int32_t x, int32_t y) {
    if (!cursor_initialized) return;
    if (x == cursor_x && y == cursor_y) return;

    cursor_x = x;
    cursor_y = y;
    wm_require_present();
}

// Отрисовка курсора в compose-буфер (классический путь — используется при
// полной пересборке сцены).
void cursor_draw(void) {
    if (!cursor_initialized || !doublebuf_is_initialized()) return;
    framebuffer_t* fb = framebuffer_get();
    draw_cursor_to(doublebuf_get_buffer(), fb->pitch / 4,
                   (int32_t)fb->width, (int32_t)fb->height,
                   cursor_x, cursor_y);
}

// Отрисовка курсора прямо во фреймбуфер — используется для cursor-only
// обновлений через doublebuf_present_scene, чтобы избежать второго memcpy.
void cursor_draw_to_framebuffer(void) {
    if (!cursor_initialized || !framebuffer_is_initialized()) return;
    framebuffer_t* fb = framebuffer_get();
    draw_cursor_to((uint32_t*)fb->address, fb->pitch / 4,
                   (int32_t)fb->width, (int32_t)fb->height,
                   cursor_x, cursor_y);
}

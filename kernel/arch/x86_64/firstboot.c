// ============================================================================
// firstboot.c — красивый многоэкранный мастер первого запуска AMPLIFY-OS.
// ----------------------------------------------------------------------------
// 4 шага с собственной GUI-анимацией (не textmode):
//   1. WELCOME  — большой логотип + приветствие + кнопка «Далее»
//   2. POLICY   — политика безопасности (4 пункта) + кнопка «Согласен»
//   3. CREATE   — форма: имя пользователя + пароль + подтверждение
//   4. SUCCESS  — «Учётная запись создана» + анимация + автопереход
//
// Навигация: Enter — далее / подтвердить, Tab — следующее поле в форме.
// ============================================================================
#include "firstboot.h"
#include "users.h"
#include "fat32.h"
#include "graphics.h"
#include "doublebuf.h"
#include "keyboard.h"
#include "serial.h"
#include "timer.h"
#include "ui_theme.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// Helpers
// ----------------------------------------------------------------------------
static int str_len(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void copy_str(char* dst, const char* src, int cap) {
    int i;
    for (i = 0; i < cap - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
}

static bool str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// ----------------------------------------------------------------------------
// Общий рендер: тёмный фон + декоративные углы.
// ----------------------------------------------------------------------------
static void draw_background(const gfx_target_t* t, int frame) {
    int W = t->width, H = t->height;
    gfx_fill_rect_gradient(t, 0, 0, W, H, UI_BG_DEEP, UI_BG_DARK);

    // «Соты» — несколько полупрозрачных кругов-точек по краям.
    int phase = (frame * 3) % 256;
    for (int i = 0; i < 6; i++) {
        int x = 40 + i * 180;
        int y = H - 60;
        uint8_t t8 = (uint8_t)((phase + i * 40) & 0xFF);
        uint32_t c = ui_lerp(UI_ACCENT_ORANGE, UI_ACCENT_MAGENTA, t8);
        c = (c & 0x00FFFFFF) | ((uint32_t)90 << 24);
        int r = 3;
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx*dx + dy*dy <= r*r)
                    gfx_put_pixel(t, x + dx, y + dy, c);
    }

    // Заголовок в верхнем углу. «AMPLIFY-OS» — название продукта, оставляем
    // латиницей; подзаголовок — на русском.
    gfx_draw_string(t, 16, 10, "AMPLIFY-OS", UI_ACCENT_GOLD, UI_BG_DARK);
    gfx_draw_string(t, 16, 28, "Мастер первого запуска", UI_FG_DIM, UI_BG_DARK);
}

// Рисует панель с заголовком (скруглённые углы + shadow).
static void draw_panel(const gfx_target_t* t, int x, int y, int w, int h,
                       const char* title, const char* subtitle) {
    gfx_drop_shadow(t, x, y, w, h, 6, 0x60000000);
    gfx_fill_round_rect(t, x, y, w, h, UI_RADIUS_L, UI_BG_MID);
    // Верхняя полоса акцента (2px градиент).
    for (int i = 0; i < 3; i++) {
        uint32_t c = ui_lerp(UI_ACCENT_ORANGE, UI_ACCENT_MAGENTA,
                             (uint8_t)((i * 255) / 3));
        gfx_fill_rect(t, x + UI_RADIUS_L, y + 2 + i, w - UI_RADIUS_L * 2, 1, c);
    }
    // Заголовок.
    if (title) {
        int tx = x + 20, ty = y + 20;
        // Масштаб 2× для title.
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
                gfx_draw_string(t, tx + dx, ty + dy, title, UI_ACCENT_GOLD, UI_BG_MID);
    }
    if (subtitle) {
        gfx_draw_string(t, x + 20, y + 54, subtitle, UI_FG_SECONDARY, UI_BG_MID);
    }
    // Разделитель.
    gfx_fill_rect(t, x + 20, y + 74, w - 40, 1, UI_BORDER_SOFT);
}

// Кнопка с hover/эмфазой.
static void draw_button(const gfx_target_t* t, int x, int y, int w, int h,
                        const char* label, bool emphasis) {
    uint32_t top = emphasis ? UI_ACCENT_ORANGE : UI_BG_LIGHT;
    uint32_t bot = emphasis ? UI_ACCENT_MAGENTA : UI_BG_MID;
    gfx_drop_shadow(t, x, y, w, h, 3, 0x80000000);
    gfx_fill_round_rect(t, x, y, w, h, UI_RADIUS_M, bot);
    for (int i = 0; i < h - 2; i++) {
        uint32_t c = ui_lerp(top, bot, (uint8_t)((i * 255) / h));
        gfx_fill_rect(t, x + UI_RADIUS_M, y + 1 + i, w - UI_RADIUS_M * 2, 1, c);
    }
    // Рамка.
    gfx_stroke_rect(t, x, y, w, h, emphasis ? UI_ACCENT_GOLD : UI_BORDER_STRONG);
    // Текст по центру.
    int tw = str_len(label) * 8;
    int tx = x + (w - tw) / 2;
    int ty = y + (h - 16) / 2;
    gfx_draw_string(t, tx, ty, label, UI_FG_PRIMARY, 0);
}

// Поле ввода.
static void draw_field(const gfx_target_t* t, int x, int y, int w, int h,
                       const char* label, const char* value, bool focused,
                       bool password) {
    gfx_draw_string(t, x, y, label, UI_FG_SECONDARY, UI_BG_MID);
    int fy = y + 18;
    gfx_fill_round_rect(t, x, fy, w, h, UI_RADIUS_S,
                        focused ? UI_BG_DARK : UI_BG_DEEP);
    gfx_stroke_rect(t, x, fy, w, h,
                    focused ? UI_ACCENT_GOLD : UI_BORDER_SOFT);

    // Рендерим value (или маску).
    int vx = x + 8;
    int vy = fy + (h - 16) / 2;
    if (password) {
        for (int i = 0; value[i]; i++) {
            gfx_draw_char(t, vx + i * 8, vy, '*', UI_FG_PRIMARY, 0);
        }
    } else {
        gfx_draw_string(t, vx, vy, value, UI_FG_PRIMARY, 0);
    }
    // Каретка.
    if (focused) {
        int vlen = str_len(value);
        int cx = vx + vlen * 8;
        gfx_fill_rect(t, cx, vy, 1, 16, UI_ACCENT_GOLD);
    }
}

// ----------------------------------------------------------------------------
// Шаг 1: WELCOME
// ----------------------------------------------------------------------------
static void step_welcome(void) {
    for (int frame = 0;; frame++) {
        const gfx_target_t* t = doublebuf_target();
        if (!t) return;
        int W = t->width, H = t->height;

        draw_background(t, frame);

        // Крупный логотип (треугольник) слева.
        int logo_cx = W / 2 - 200;
        int logo_y  = H / 2 - 80;
        int size = 70, stroke = 10;
        int bx = logo_cx - size, by = logo_y + (size * 175 / 100);
        int cx2 = logo_cx + size, cy2 = by;
        // Градиентный треугольник (простой, без SSAA для скорости).
        for (int yy = logo_y; yy <= by; yy++) {
            for (int xx = logo_cx - size; xx <= logo_cx + size; xx++) {
                // Контур: внутри внешнего, но вне внутреннего.
                int in_outer =
                    ((bx - logo_cx) * (yy - logo_y) - (by - logo_y) * (xx - logo_cx)) >= 0 &&
                    ((cx2 - bx) * (yy - by) - (cy2 - by) * (xx - bx)) >= 0 &&
                    ((logo_cx - cx2) * (yy - cy2) - (logo_y - cy2) * (xx - cx2)) >= 0;
                int iax = logo_cx,             iay = logo_y + stroke;
                int ibx2 = logo_cx - size + stroke, iby2 = by - stroke / 2;
                int icx2 = logo_cx + size - stroke, icy2 = by - stroke / 2;
                int in_inner =
                    ((ibx2 - iax) * (yy - iay) - (iby2 - iay) * (xx - iax)) >= 0 &&
                    ((icx2 - ibx2) * (yy - iby2) - (icy2 - iby2) * (xx - ibx2)) >= 0 &&
                    ((iax - icx2) * (yy - icy2) - (iay - icy2) * (xx - icx2)) >= 0;
                if (in_outer && !in_inner) {
                    uint8_t tt = (uint8_t)(((yy - logo_y) * 255) / (by - logo_y + 1));
                    uint32_t c = ui_lerp(UI_ACCENT_ORANGE, UI_ACCENT_MAGENTA, tt);
                    gfx_put_pixel(t, xx, yy, c);
                }
            }
        }

        // Справа — приветственная панель.
        int pw = 540, ph = 320;
        int px = W / 2 - 80;
        int py = H / 2 - ph / 2;
        draw_panel(t, px, py, pw, ph, "Добро пожаловать!",
                   "AMPLIFY-OS  —  64-битная редакция Long Mode");

        int tx = px + 20;
        int ty = py + 96;
        int lh = 22;
        const char* lines[] = {
            "Это ваш первый запуск системы.",
            "",
            "За несколько шагов вы настроите:",
            "  * учётную запись администратора",
            "  * политику доступа (пароль + хеш)",
            "  * рабочий стол и приложения",
            "",
            "Нажмите [Enter], чтобы начать.",
            NULL
        };
        for (int i = 0; lines[i]; i++) {
            gfx_draw_string(t, tx, ty + i * lh, lines[i],
                            UI_FG_PRIMARY, UI_BG_MID);
        }

        // Кнопка.
        bool emphasis = ((frame / 8) & 1) != 0;
        draw_button(t, px + pw / 2 - 100, py + ph - 54, 200, 36,
                    "Начать (Enter)", emphasis);

        doublebuf_swap();
        timer_sleep(40);

        if (keyboard_has_char()) {
            char c = keyboard_wait_char();
            if (c == '\r' || c == '\n' || c == ' ') return;
        }
    }
}

// ----------------------------------------------------------------------------
// Шаг 2: POLICY
// ----------------------------------------------------------------------------
static void step_policy(void) {
    for (int frame = 0;; frame++) {
        const gfx_target_t* t = doublebuf_target();
        if (!t) return;
        int W = t->width, H = t->height;

        draw_background(t, frame);

        int pw = 720, ph = 440;
        int px = (W - pw) / 2;
        int py = (H - ph) / 2 - 20;
        draw_panel(t, px, py, pw, ph, "Политика безопасности",
                   "Прочтите и примите перед продолжением");

        int tx = px + 24;
        int ty = py + 96;
        int lh = 22;

        struct { uint32_t c; const char* s; } lines[] = {
            { UI_ACCENT_GOLD, "[1] Хеширование паролей" },
            { UI_FG_PRIMARY,  "    Все пароли сохраняются в виде FNV-1a 64-bit" },
            { UI_FG_PRIMARY,  "    хеша в /disk/users.txt. Сырой пароль никогда" },
            { UI_FG_PRIMARY,  "    не покидает сеансовую память."},
            { 0, "" },
            { UI_ACCENT_GOLD, "[2] Обязательная аутентификация" },
            { UI_FG_PRIMARY,  "    После создания admin-аккаунта guest-режим" },
            { UI_FG_PRIMARY,  "    отключается: каждый вход требует пароль."},
            { 0, "" },
            { UI_ACCENT_GOLD, "[3] Защита от подбора" },
            { UI_FG_PRIMARY,  "    После 5 неудачных попыток — пауза 2 секунды." },
            { 0, "" },
            { UI_ACCENT_GOLD, "[4] Требования к паролю" },
            { UI_FG_PRIMARY,  "    Минимум 4 символа. Латиница/цифры/знаки." },
        };
        int n = sizeof(lines) / sizeof(lines[0]);
        for (int i = 0; i < n; i++) {
            gfx_draw_string(t, tx, ty + i * lh, lines[i].s,
                            lines[i].c ? lines[i].c : UI_FG_PRIMARY, UI_BG_MID);
        }

        bool emphasis = ((frame / 8) & 1) != 0;
        draw_button(t, px + pw / 2 - 130, py + ph - 54, 260, 36,
                    "Принимаю (Enter)", emphasis);

        doublebuf_swap();
        timer_sleep(40);

        if (keyboard_has_char()) {
            char c = keyboard_wait_char();
            if (c == '\r' || c == '\n' || c == ' ') return;
        }
    }
}

// ----------------------------------------------------------------------------
// Шаг 3: CREATE ADMIN
// ----------------------------------------------------------------------------
typedef enum { F_USER = 0, F_PASS1, F_PASS2, F_COUNT } field_t;

static char fb_user [24];
static char fb_pass1[24];
static char fb_pass2[24];
static char fb_err  [128];

static bool validate_form(void) {
    fb_err[0] = 0;
    int ulen = str_len(fb_user);
    int p1   = str_len(fb_pass1);
    int p2   = str_len(fb_pass2);

    if (ulen == 0) { copy_str(fb_err, "Имя пользователя не может быть пустым", sizeof(fb_err)); return false; }
    for (int i = 0; i < ulen; i++) {
        char c = fb_user[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) { copy_str(fb_err, "Имя: только a-z, A-Z, 0-9, _, -", sizeof(fb_err)); return false; }
    }
    if (p1 < 4) { copy_str(fb_err, "Пароль должен быть не менее 4 символов", sizeof(fb_err)); return false; }
    if (p1 != p2 || !str_eq(fb_pass1, fb_pass2)) {
        copy_str(fb_err, "Пароли не совпадают", sizeof(fb_err));
        return false;
    }
    return true;
}

static void step_create(void) {
    field_t focus = F_USER;
    fb_user[0] = 0; fb_pass1[0] = 0; fb_pass2[0] = 0; fb_err[0] = 0;

    for (int frame = 0;; frame++) {
        const gfx_target_t* t = doublebuf_target();
        if (!t) return;
        int W = t->width, H = t->height;

        draw_background(t, frame);

        int pw = 560, ph = 410;
        int px = (W - pw) / 2;
        int py = (H - ph) / 2 - 20;
        draw_panel(t, px, py, pw, ph, "Учётная запись",
                   "Введите данные администратора");

        int fx = px + 30;
        int fw = pw - 60;
        int fh = 32;
        int gap = 70;
        int row_y = py + 100;

        draw_field(t, fx, row_y + 0 * gap, fw, fh,
                   "Имя пользователя:",         fb_user,
                   focus == F_USER, false);
        draw_field(t, fx, row_y + 1 * gap, fw, fh,
                   "Пароль:",                   fb_pass1,
                   focus == F_PASS1, true);
        draw_field(t, fx, row_y + 2 * gap, fw, fh,
                   "Повторите пароль:",         fb_pass2,
                   focus == F_PASS2, true);

        // Ошибка.
        if (fb_err[0]) {
            gfx_draw_string(t, fx, py + ph - 78, fb_err, UI_FG_ERROR, UI_BG_MID);
        }
        // Подсказка.
        gfx_draw_string(t, fx, py + ph - 60,
                        "Tab — следующее поле, Enter — создать",
                        UI_FG_DIM, UI_BG_MID);

        bool emphasis = ((frame / 8) & 1) != 0;
        draw_button(t, px + pw / 2 - 120, py + ph - 40, 240, 32,
                    "Создать (Enter)", emphasis);

        doublebuf_swap();
        timer_sleep(40);

        if (!keyboard_has_char()) continue;
        char c = keyboard_wait_char();

        if (c == '\t') {
            focus = (field_t)(((int)focus + 1) % F_COUNT);
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (focus != F_PASS2) {
                focus = (field_t)((int)focus + 1);
                continue;
            }
            if (!validate_form()) continue;
            int idx = users_add(fb_user, fb_pass1);
            if (idx < 0) {
                copy_str(fb_err, "Не удалось добавить пользователя", sizeof(fb_err));
                continue;
            }
            if (!users_save()) {
                copy_str(fb_err, "Создан, но НЕ сохранён на диск", sizeof(fb_err));
                // Продолжаем в SUCCESS, т.к. in-memory OK.
            }
            return;
        }
        if (c == '\b') {
            char* bufs[F_COUNT] = { fb_user, fb_pass1, fb_pass2 };
            int l = str_len(bufs[focus]);
            if (l > 0) bufs[focus][l - 1] = 0;
            continue;
        }
        if (c >= 32 && c < 127) {
            char* bufs[F_COUNT] = { fb_user, fb_pass1, fb_pass2 };
            int caps[F_COUNT]   = { sizeof(fb_user), sizeof(fb_pass1), sizeof(fb_pass2) };
            int l = str_len(bufs[focus]);
            if (l < caps[focus] - 1) {
                bufs[focus][l] = c;
                bufs[focus][l + 1] = 0;
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Шаг 4: SUCCESS
// ----------------------------------------------------------------------------
static void step_success(void) {
    for (int frame = 0; frame < 60; frame++) {
        const gfx_target_t* t = doublebuf_target();
        if (!t) return;
        int W = t->width, H = t->height;

        draw_background(t, frame);

        int pw = 520, ph = 220;
        int px = (W - pw) / 2;
        int py = (H - ph) / 2 - 20;
        draw_panel(t, px, py, pw, ph, "Готово!",
                   "Учётная запись успешно создана");

        gfx_draw_string(t, px + 30, py + 100,
                        "Сейчас откроется экран входа.",
                        UI_FG_PRIMARY, UI_BG_MID);
        gfx_draw_string(t, px + 30, py + 120,
                        "Используйте только что созданные учётные данные.",
                        UI_FG_SECONDARY, UI_BG_MID);

        // Анимированный progress-spinner.
        int bx = px + (pw - 200) / 2;
        int by = py + ph - 30;
        int fill = (frame * 200) / 60;
        gfx_fill_round_rect(t, bx, by, 200, 6, 3, UI_BORDER_SOFT);
        for (int x = 0; x < fill; x++) {
            uint8_t tt = (uint8_t)((x * 255) / 200);
            uint32_t c = ui_lerp(UI_ACCENT_ORANGE, UI_ACCENT_MAGENTA, tt);
            for (int y = 0; y < 6; y++) gfx_put_pixel(t, bx + x, by + y, c);
        }

        doublebuf_swap();
        timer_sleep(33);
    }
}

// ----------------------------------------------------------------------------
bool firstboot_run_if_needed(void) {
    if (users_count() > 0) return false;
    if (!fat32_is_mounted()) {
        serial_puts("[firstboot] skip: FAT32 not mounted\n");
        return false;
    }

    serial_puts("[firstboot] starting graphical wizard\n");
    step_welcome();
    step_policy();
    step_create();
    step_success();
    serial_puts("[firstboot] done\n");
    return true;
}

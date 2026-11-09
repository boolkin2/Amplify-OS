// ============================================================================
// login.c - Экран входа в систему (Этап 32)
// ============================================================================
// Полноэкранный login-prompt: список пользователей слева, поле пароля справа,
// сообщение об ошибке. Возвращает указатель на пользователя только после
// успешной аутентификации.
// ============================================================================

#include "security.h"
#include "sec_ui.h"
#include "../gui/graphics.h"
#include "../gui/font.h"
#include "../gui/framebuffer.h"
#include "../gui/doublebuf.h"
#include "../drivers/timer.h"
#include "../debug_serial.h"
#include <stddef.h>

extern void dbg_puts(const char* s);

// ----------------------------------------------------------------------------
// Состояние экрана.
// ----------------------------------------------------------------------------

static int  selected_user = 0;
static char login_pass[SEC_PASSWORD_MAX + 1];
static char login_error[128] = "";
static int  fail_count = 0;
static int  lockout_until_phase = 0;     // тики, до которых вход заблокирован
static int  global_phase = 0;

// ----------------------------------------------------------------------------
// Отрисовка.
// ----------------------------------------------------------------------------

static void draw_login_screen(void) {
    int32_t W = framebuffer_get_width();
    int32_t H = framebuffer_get_height();

    secui_draw_background(global_phase);

    // Шапка с логотипом.
    const char* brand = "AMPLIFY-OS";
    int bs = 3;
    int bw = secui_strlen(brand) * 8 * bs;
    secui_text_big((W - bw) / 2, 60, brand, 0xFFFFFFFFu, bs);
    secui_text_centered(0, 60 + 16 * bs + 8, W,
                        "Вход в систему", SECUI_ACCENT);

    // Главная панель (центральная).
    int pw = 760, ph = 420;
    int px = (W - pw) / 2;
    int py = 200;
    secui_draw_panel(px, py, pw, ph,
                     "Выберите пользователя",
                     "Стрелки вверх/вниз (или J/K) — выбор, Enter — вход");

    // Левая колонка — список пользователей.
    int list_x = px + 24;
    int list_y = py + 80;
    int list_w = 280;
    int row_h  = 56;

    int n = sec_users_count();
    if (n <= 0) {
        // Странная ситуация (но защищаемся): просто выходим.
        secui_text_centered(px, py + ph / 2, pw,
                            "Нет пользователей в системе!",
                            SECUI_ERROR_FG);
        return;
    }

    for (int i = 0; i < n; i++) {
        const sec_user_t* u = sec_users_get(i);
        int ry = list_y + i * (row_h + 8);
        bool sel = (i == selected_user);
        uint32_t bg_top = sel ? 0xFF285080u : 0xFF18244Cu;
        uint32_t bg_bot = sel ? 0xFF1A3868u : 0xFF101A40u;
        gfx_fill_rect_gradient(list_x, ry, list_w, row_h, bg_top, bg_bot);
        gfx_draw_rect(list_x, ry, list_w, row_h,
                      sel ? SECUI_ACCENT : 0xFF2A3868u);

        // Аватар-круг с первой буквой имени.
        int av_cx = list_x + 28;
        int av_cy = ry + row_h / 2;
        gfx_fill_circle(av_cx, av_cy, 18,
                        sel ? SECUI_ACCENT : 0xFF40A0FFu);
        char first[2] = { u->display[0] ? u->display[0] : u->name[0], 0 };
        if (first[0] >= 'a' && first[0] <= 'z') first[0] -= 32;
        font_draw_string(av_cx - FONT_WIDTH / 2, av_cy - FONT_HEIGHT / 2,
                         first, 0xFF000000, 0xFF000000);

        // Имя.
        font_draw_string(list_x + 60, ry + 10,
                         u->display[0] ? u->display : u->name,
                         SECUI_TITLE_FG, 0xFF000000);
        // Подпись «administrator» / «pol'zovatel'».
        const char* role =
            (u->uid == SEC_UID_ROOT || (u->flags & SEC_UFLAG_ADMIN))
            ? "администратор" : "пользователь";
        font_draw_string(list_x + 60, ry + 10 + FONT_HEIGHT + 4,
                         role, SECUI_TEXT_DIM, 0xFF000000);
    }

    // Правая колонка — поле пароля.
    int form_x = list_x + list_w + 32;
    int form_y = list_y;
    int form_w = pw - (form_x - px) - 24;

    const sec_user_t* u = sec_users_get(selected_user);
    if (!u) return;

    font_draw_string(form_x, form_y,
                     "Вы входите как:", SECUI_TEXT_DIM, 0xFF000000);
    font_draw_string(form_x, form_y + FONT_HEIGHT + 6,
                     u->display[0] ? u->display : u->name,
                     SECUI_TITLE_FG, 0xFF000000);

    font_draw_string(form_x, form_y + FONT_HEIGHT * 3 + 16,
                     "Введите пароль:", SECUI_TEXT_FG, 0xFF000000);

    secui_field_t pf = {
        .x = form_x,
        .y = form_y + FONT_HEIGHT * 4 + 18,
        .w = form_w - 8,
        .h = 40,
        .buffer = login_pass,
        .buffer_size = sizeof(login_pass),
        .cursor = secui_strlen(login_pass),
        .kind = SECUI_FIELD_PASSWORD,
        .focused = (global_phase < lockout_until_phase) ? false : true,
        .placeholder = "******"
    };
    secui_field_draw(&pf);

    // Ошибка.
    if (login_error[0]) {
        font_draw_string(form_x, form_y + FONT_HEIGHT * 4 + 18 + 50,
                         login_error, SECUI_ERROR_FG, 0xFF000000);
    }

    // Лимиты / lockout.
    if (global_phase < lockout_until_phase) {
        int left_frames = lockout_until_phase - global_phase;
        int left_seconds = (left_frames * 33) / 1000 + 1;
        char buf[96];
        const char* prefix = "Слишком много попыток. Подождите ";
        int p = 0;
        for (; prefix[p] && p < (int)sizeof(buf) - 8; p++) buf[p] = prefix[p];
        // Печатаем число секунд.
        if (left_seconds > 99) left_seconds = 99;
        if (left_seconds >= 10) buf[p++] = '0' + left_seconds / 10;
        buf[p++] = '0' + left_seconds % 10;
        const char* tail = " с.";
        for (int i = 0; tail[i] && p < (int)sizeof(buf) - 1; i++) buf[p++] = tail[i];
        buf[p] = '\0';
        font_draw_string(form_x, form_y + FONT_HEIGHT * 4 + 18 + 80,
                         buf, 0xFFFFA040u, 0xFF000000);
    }

    // Кнопка «Войти».
    secui_button_t btn = {
        form_x, form_y + FONT_HEIGHT * 4 + 18 + 110, form_w - 8, 44,
        "Войти (Enter)",
        global_phase >= lockout_until_phase
    };
    secui_button_draw(&btn, ((global_phase / 8) & 1) != 0);

    // Подвал — подсказки.
    secui_text_centered(0, H - FONT_HEIGHT - 20, W,
        "Tab / J / K: выбор пользователя   |   Enter: войти",
        SECUI_TEXT_DIM);
}

// ----------------------------------------------------------------------------
// Главный цикл логина.
// ----------------------------------------------------------------------------

const sec_user_t* sec_login_run(void) {
    selected_user = 0;
    login_pass[0] = '\0';
    login_error[0] = '\0';
    fail_count = 0;
    lockout_until_phase = 0;
    global_phase = 0;

    int n = sec_users_count();
    if (n <= 0) {
        // Сюда попасть не должны (security_run сначала запускает firstboot).
        dbg_puts("[sec][login] BUG: no users — running firstboot in fallback\n");
        sec_firstboot_run();
    }

    while (1) {
        draw_login_screen();
        secui_present();
        timer_sleep(33);
        global_phase++;

        int ch = secui_kb_poll();
        if (ch == 0) continue;

        // Если идёт lockout — игнорируем ввод (кроме навигации).
        bool locked = (global_phase < lockout_until_phase);

        if (ch == '\t') {
            if (!locked) {
                selected_user = (selected_user + 1) % sec_users_count();
                login_pass[0] = '\0';
                login_error[0] = '\0';
            }
            continue;
        }

        // Стрелки приходят как escape-последовательности через PS/2;
        // наш keyboard.c не делает этого для всех кейсов, поэтому продублируем
        // навигацию на 'j'/'k' и Tab. Просто и надёжно.
        if (!locked && (ch == 'j' || ch == 'J')) {
            selected_user = (selected_user + 1) % sec_users_count();
            login_pass[0] = '\0';
            login_error[0] = '\0';
            continue;
        }
        if (!locked && (ch == 'k' || ch == 'K')) {
            int n2 = sec_users_count();
            selected_user = (selected_user + n2 - 1) % n2;
            login_pass[0] = '\0';
            login_error[0] = '\0';
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (locked) continue;
            const sec_user_t* u = sec_users_get(selected_user);
            if (!u) continue;
            const sec_user_t* ok = sec_users_authenticate(u->name, login_pass);
            if (ok) {
                dbg_puts("[sec][login] success\n");
                // Победный кадр.
                for (int f = 0; f < 12; f++) {
                    draw_login_screen();
                    int32_t W = framebuffer_get_width();
                    int32_t H = framebuffer_get_height();
                    int alpha = (f * 200) / 12;
                    if (alpha > 200) alpha = 200;
                    uint32_t flash = 0xFF000000u | (uint32_t)alpha
                                   | ((uint32_t)alpha << 8)
                                   | ((uint32_t)alpha << 16);
                    (void)flash;
                    gfx_fill_rect(0, 0, W, H,
                                  (uint32_t)((alpha << 16) | (alpha << 8) | alpha) & 0x00FFFFFFu);
                    secui_present();
                    timer_sleep(20);
                }
                return ok;
            }
            // Неверный пароль.
            fail_count++;
            login_pass[0] = '\0';
            secui_strncpy(login_error,
                          "Неверный пароль. Попробуйте снова.",
                          sizeof(login_error));
            if (fail_count >= 3) {
                // Lockout: 5 секунд (~150 кадров по 33мс).
                lockout_until_phase = global_phase + 150;
                fail_count = 0;
                secui_strncpy(login_error,
                              "Слишком много неудачных попыток.",
                              sizeof(login_error));
            }
            continue;
        }

        if (ch == '\b' || ch == 0x7F) {
            int len = secui_strlen(login_pass);
            if (len > 0) login_pass[len - 1] = '\0';
            login_error[0] = '\0';
            continue;
        }

        if (!locked && ch >= 0x20 && ch < 0x7F) {
            int len = secui_strlen(login_pass);
            if (len < (int)sizeof(login_pass) - 1) {
                login_pass[len]     = (char)ch;
                login_pass[len + 1] = '\0';
            }
            login_error[0] = '\0';
        }
    }
    /* unreachable */
    return NULL;
}

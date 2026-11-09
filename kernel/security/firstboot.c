// ============================================================================
// firstboot.c - Мастер первого запуска (Этап 32)
// ============================================================================
// Полноэкранный многошаговый мастер: Welcome → Create Admin → Done.
// Не возвращается, пока не создан хотя бы один администратор.
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
// Состояние мастера
// ----------------------------------------------------------------------------

typedef enum {
    STEP_WELCOME = 0,
    STEP_CREATE_ADMIN,
    STEP_DONE
} fb_step_t;

// Поля формы Create Admin.
static char fb_username[SEC_USERNAME_MAX + 1];
static char fb_display [SEC_DISPLAYNAME_MAX + 1];
static char fb_pass1   [SEC_PASSWORD_MAX + 1];
static char fb_pass2   [SEC_PASSWORD_MAX + 1];

// Сообщение об ошибке (валидация).
static char fb_error[128] = "";

// ----------------------------------------------------------------------------
// Шаг 1: Welcome screen — крупный логотип + три абзаца + кнопка «Начать»
// ----------------------------------------------------------------------------

static void draw_welcome(int frame_phase) {
    int32_t W = framebuffer_get_width();
    (void)framebuffer_get_height();

    secui_draw_background(frame_phase);

    // Большой заголовок ОС (английская латиница — глифы стандартные).
    const char* title = "AMPLIFY-OS";
    int title_scale = 4;
    int tw = secui_strlen(title) * 8 * title_scale;
    secui_text_big((W - tw) / 2, 90, title, 0xFFFFFFFFu, title_scale);

    // Подзаголовок — на русском (CP1251).
    const char* sub = "Операционная Система — вторая фаза разработки";
    secui_text_centered(0, 90 + 16 * title_scale + 16, W, sub, SECUI_ACCENT);

    // Панель с описанием.
    int pw = 720, ph = 280;
    int px = (W - pw) / 2;
    int py = 240;
    secui_draw_panel(px, py, pw, ph,
                     "Добро пожаловать!",
                     "Этап 32: безопасность и многопользовательский режим");

    int tx = px + 32;
    int ty = py + 80;
    int line_h = FONT_HEIGHT + 6;

    const char* lines[] = {
        "Этот мастер поможет вам настроить систему.",
        "",
        "  • Вы создадите учётную запись администратора.",
        "  • Пароль будет храниться только в виде криптохеша + соли.",
        "  • При каждом запуске экран входа будет запрашивать пароль.",
        "  • Файлы получат права доступа в стиле Unix (rwxrwxrwx).",
        "",
        "После авторизации вам будет доступен полный рабочий стол",
        "Amplify-OS со всеми приложениями и сетевым стеком.",
        NULL
    };
    for (int i = 0; lines[i]; i++) {
        font_draw_string(tx, ty + i * line_h, lines[i],
                         SECUI_TEXT_FG, 0xFF000000);
    }

    // Кнопка «Начать».
    secui_button_t btn = { (W - 240) / 2, py + ph + 24, 240, 48,
                           "Начать (Enter)", true };
    secui_button_draw(&btn, ((frame_phase / 8) & 1) != 0);
}

// Возвращает true, когда пользователь нажал Enter / Space.
static bool process_welcome(void) {
    int phase = 0;
    while (1) {
        draw_welcome(phase);
        secui_present();
        timer_sleep(33);
        phase++;

        int ch = secui_kb_poll();
        if (ch == '\r' || ch == '\n' || ch == ' ') return true;
    }
}

// ----------------------------------------------------------------------------
// Шаг 2: Create Admin — форма с 4 полями
// ----------------------------------------------------------------------------

typedef enum {
    F_USER = 0, F_DISP, F_PASS1, F_PASS2, F_COUNT
} field_id_t;

static void draw_create_admin(int frame_phase, field_id_t focus) {
    int32_t W = framebuffer_get_width();
    (void)framebuffer_get_height();
    secui_draw_background(frame_phase);

    // Заголовок.
    const char* title = "Создание администратора";
    int tw = secui_strlen(title) * 8 * 2;
    secui_text_big((W - tw) / 2, 90, title, SECUI_TITLE_FG, 2);

    // Панель с формой.
    int pw = 600, ph = 420;
    int px = (W - pw) / 2;
    int py = 180;
    secui_draw_panel(px, py, pw, ph,
                     "Введите свои данные",
                     "Поле «Имя пользователя» — латинские буквы без пробелов.");

    // Размещение полей.
    int label_x = px + 32;
    int field_x = px + 220;
    int field_w = pw - 220 - 32;
    int field_h = 36;
    int row_y[F_COUNT];
    int y = py + 92;
    int gap = 56;
    for (int i = 0; i < F_COUNT; i++) { row_y[i] = y; y += gap; }

    const char* labels[F_COUNT] = {
        "Имя пользователя:",
        "Отображаемое имя:",
        "Пароль:",
        "Повтор пароля:"
    };

    secui_field_t fields[F_COUNT];
    char* bufs[F_COUNT] = { fb_username, fb_display, fb_pass1, fb_pass2 };
    int   bsz [F_COUNT] = { sizeof(fb_username), sizeof(fb_display),
                            sizeof(fb_pass1),    sizeof(fb_pass2) };
    secui_field_kind_t kinds[F_COUNT] = {
        SECUI_FIELD_TEXT, SECUI_FIELD_TEXT,
        SECUI_FIELD_PASSWORD, SECUI_FIELD_PASSWORD
    };
    const char* placeholders[F_COUNT] = {
        "например: admin", "например: Администратор",
        "(не менее 4 символов)", "повторите пароль"
    };

    for (int i = 0; i < F_COUNT; i++) {
        font_draw_string(label_x, row_y[i] + (field_h - FONT_HEIGHT) / 2,
                         labels[i], SECUI_TEXT_FG, 0xFF000000);

        fields[i].x = field_x;
        fields[i].y = row_y[i];
        fields[i].w = field_w;
        fields[i].h = field_h;
        fields[i].buffer = bufs[i];
        fields[i].buffer_size = bsz[i];
        fields[i].cursor = secui_strlen(bufs[i]);
        fields[i].kind = kinds[i];
        fields[i].focused = (i == (int)focus);
        fields[i].placeholder = placeholders[i];
        secui_field_draw(&fields[i]);
    }

    // Сообщение об ошибке (если есть).
    if (fb_error[0]) {
        secui_text_centered(px, py + ph - 80, pw, fb_error, SECUI_ERROR_FG);
    }

    // Подсказка по навигации.
    secui_text_centered(px, py + ph - 56, pw,
                        "Tab — следующее поле   |   Enter — создать",
                        SECUI_TEXT_DIM);

    // Кнопка «Создать».
    secui_button_t btn = { (W - 240) / 2, py + ph + 24, 240, 48,
                           "Создать (Enter)", true };
    secui_button_draw(&btn, ((frame_phase / 8) & 1) != 0);
}

static bool validate_admin_form(void) {
    fb_error[0] = '\0';

    int ulen = secui_strlen(fb_username);
    int dlen = secui_strlen(fb_display);
    int p1   = secui_strlen(fb_pass1);
    int p2   = secui_strlen(fb_pass2);

    if (ulen == 0)         { secui_strncpy(fb_error, "Имя пользователя не может быть пустым", sizeof(fb_error)); return false; }
    if (ulen > SEC_USERNAME_MAX) { secui_strncpy(fb_error, "Имя слишком длинное", sizeof(fb_error)); return false; }
    for (int i = 0; i < ulen; i++) {
        char c = fb_username[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) { secui_strncpy(fb_error, "Имя: только a-z, A-Z, 0-9, _ и -", sizeof(fb_error)); return false; }
    }
    if (dlen == 0) { secui_strncpy(fb_error, "Отображаемое имя не может быть пустым", sizeof(fb_error)); return false; }
    if (p1 < 4)    { secui_strncpy(fb_error, "Пароль должен быть не менее 4 символов", sizeof(fb_error)); return false; }
    if (p1 != p2 || secui_strcmp(fb_pass1, fb_pass2) != 0) {
        secui_strncpy(fb_error, "Пароли не совпадают", sizeof(fb_error));
        return false;
    }
    return true;
}

static void process_create_admin(void) {
    field_id_t focus = F_USER;
    int phase = 0;

    while (1) {
        draw_create_admin(phase, focus);
        secui_present();
        timer_sleep(33);
        phase++;

        int ch = secui_kb_poll();
        if (ch == 0) continue;

        if (ch == '\t') {
            // Циклически переключаем фокус.
            focus = (field_id_t)(((int)focus + 1) % F_COUNT);
            continue;
        }
        if (ch == 0x1B /* Esc */ ) {
            // Перепрыгиваем на первое поле.
            focus = F_USER;
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            // Если в фокусе не последнее поле — переходим дальше.
            if (focus != F_PASS2) {
                focus = (field_id_t)((int)focus + 1);
                continue;
            }
            // Иначе — пробуем создать.
            if (!validate_admin_form()) {
                continue;
            }
            int rc = sec_users_create(fb_username, fb_display, fb_pass1,
                                      /*is_admin=*/true);
            if (rc != 0) {
                secui_strncpy(fb_error,
                              "Не удалось создать учётную запись",
                              sizeof(fb_error));
                continue;
            }
            // Успех — выходим из формы.
            return;
        }

        // Делегируем символ в активное поле.
        secui_field_t fld;
        char* bufs[F_COUNT] = { fb_username, fb_display, fb_pass1, fb_pass2 };
        int   bsz [F_COUNT] = { sizeof(fb_username), sizeof(fb_display),
                                sizeof(fb_pass1),    sizeof(fb_pass2) };
        secui_field_kind_t kinds[F_COUNT] = {
            SECUI_FIELD_TEXT, SECUI_FIELD_TEXT,
            SECUI_FIELD_PASSWORD, SECUI_FIELD_PASSWORD
        };
        fld.x = 0; fld.y = 0; fld.w = 0; fld.h = 0;
        fld.buffer = bufs[focus];
        fld.buffer_size = bsz[focus];
        fld.cursor = secui_strlen(bufs[focus]);
        fld.kind = kinds[focus];
        fld.focused = true;
        fld.placeholder = NULL;
        secui_field_handle(&fld, ch);
    }
}

// ----------------------------------------------------------------------------
// Шаг 3: Done — короткий «успех» + автопереход.
// ----------------------------------------------------------------------------

static void draw_done(int frame_phase) {
    int32_t W = framebuffer_get_width();
    int32_t H = framebuffer_get_height();
    secui_draw_background(frame_phase);

    int pw = 560, ph = 200;
    int px = (W - pw) / 2;
    int py = (H - ph) / 2 - 40;
    secui_draw_panel(px, py, pw, ph,
                     "Готово!",
                     "Ваша учётная запись успешно создана");

    secui_text_centered(px, py + 90, pw,
                        "Сейчас вы будете перенаправлены на экран входа.",
                        SECUI_TEXT_FG);
    secui_text_centered(px, py + 90 + FONT_HEIGHT + 6, pw,
                        "Используйте созданные имя и пароль.",
                        SECUI_TEXT_DIM);

    secui_draw_spinner(W / 2, py + ph + 40, frame_phase);
}

static void process_done(void) {
    for (int i = 0; i < 60; i++) {
        draw_done(i);
        secui_present();
        timer_sleep(33);
    }
}

// ----------------------------------------------------------------------------
// Точка входа мастера.
// ----------------------------------------------------------------------------

void sec_firstboot_run(void) {
    fb_username[0] = '\0';
    fb_display [0] = '\0';
    fb_pass1   [0] = '\0';
    fb_pass2   [0] = '\0';
    fb_error   [0] = '\0';

    dbg_puts("[sec][firstboot] welcome\n");
    process_welcome();

    dbg_puts("[sec][firstboot] create admin\n");
    process_create_admin();

    dbg_puts("[sec][firstboot] done\n");
    process_done();
}

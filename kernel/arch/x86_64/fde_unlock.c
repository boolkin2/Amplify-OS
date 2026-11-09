// ============================================================================
// fde_unlock.c — экран разблокировки зашифрованного системного тома (этап 48).
// ----------------------------------------------------------------------------
// Показывается до amply/firstboot/login: /disk в этот момент ещё не
// смонтирован, поэтому ни тема пользователя, ни его имя недоступны —
// экран всегда светлый Pearl, как установщик, из которого система пришла.
//
// Проверка пароля — PBKDF2 на ~1 секунду (FDE_ITER_MS), поэтому перед
// вычислением кадр с «Unsealing...» рисуется заранее, иначе экран выглядел
// бы зависшим.  После трёх неверных попыток — пауза, растущая с каждой
// следующей: перебор «вживую» становится дороже, чем офлайн по образу.
// ============================================================================
#include "fde.h"
#include "luks.h"

#include "graphics.h"
#include "doublebuf.h"
#include "ui_theme.h"
#include "ui_layout.h"
#include "ui_icons.h"
#include "ui_font.h"
#include "keyboard.h"
#include "mouse.h"
#include "cursor.h"
#include "timer.h"
#include "serial.h"
#include "crypto.h"

#include <stdint.h>
#include <stdbool.h>

#define CX        800
#define FIELD_X   560
#define FIELD_W   480
#define FIELD_Y   474
#define FIELD_H   52
#define BTN_Y     552
#define BTN_H     52
#define GRAD_FROM 0xFFE69D40u
#define GRAD_TO   0xFFC45A79u

typedef enum { ST_IDLE, ST_WORKING, ST_WRONG, ST_ERROR, ST_DONE } unlock_state_t;

static char           g_pass[64];
static unlock_state_t g_state = ST_IDLE;
static int            g_attempts = 0;
static int            g_frame = 0;
static uint64_t       g_wait_until = 0;

static int s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

static uint32_t c_soft(void)  { const ui_runtime_theme_t* th = ui_theme_current(); return ui_lerp(th->muted, th->canvas, 50); }
static uint32_t c_faint(void) { const ui_runtime_theme_t* th = ui_theme_current(); return ui_lerp(th->muted, th->canvas, 150); }

static void fill(const gfx_target_t* t, int x, int y, int w, int h, uint32_t c) {
    gfx_fill_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), c);
}
static void frame_rect(const gfx_target_t* t, int x, int y, int w, int h, uint32_t c) {
    gfx_stroke_rect(t, ui_ux(x), ui_uy(y), ui_uw(w), ui_uh(h), c);
}
static void grad_rect(const gfx_target_t* t, int x, int y, int w, int h) {
    int sx = ui_ux(x), sy = ui_uy(y), sw = ui_uw(w), sh = ui_uh(h);
    for (int i = 0; i < sw; i++)
        gfx_fill_rect(t, sx + i, sy, 1, sh, ui_lerp(GRAD_FROM, GRAD_TO, (uint8_t)(sw > 1 ? i * 255 / (sw - 1) : 0)));
}
static int text_px(const char* s, ui_font_id_t f, int tracking, bool bold) {
    return ui_font_text_width_tracked(s, f, tracking) + (bold ? 1 : 0);
}
static void ctxt(const gfx_target_t* t, int cx, int base, const char* s, uint32_t c,
                 ui_font_id_t f, int tracking, bool bold) {
    int w = text_px(s, f, tracking, bold);
    ui_font_draw_baseline_ex(t, ui_ux(cx) - w / 2, ui_uy(base), s, c, f, tracking, bold);
}
static void txt(const gfx_target_t* t, int x, int base, const char* s, uint32_t c,
                ui_font_id_t f, int tracking, bool bold) {
    ui_font_draw_baseline_ex(t, ui_ux(x), ui_uy(base), s, c, f, tracking, bold);
}
static int logical_w(int px) {
    const ui_layout_t* l = ui_layout_get();
    return l->scale_fp > 0 ? (int)(((int64_t)px << 16) / l->scale_fp) : px;
}

static void draw(const gfx_target_t* t) {
    const ui_runtime_theme_t* th = ui_theme_current();
    gfx_clear(t, th->canvas);

    ui_draw_bitmap_scaled(t, ui_ux(CX - 45), ui_uy(196), ui_uw(90), ui_uh(88), ui_logo_big());
    ctxt(t, CX, 336, "SEALED VOLUME", th->accent_berry, UI_FONT_MONO_14, 2, false);
    ctxt(t, CX, 384, "Unlock AMPLIFY-OS", th->ink, UI_FONT_28, 0, true);
    ctxt(t, CX, 420, fde_volume_label(), c_soft(), UI_FONT_14, 0, false);

    // Поле пароля.
    txt(t, FIELD_X, FIELD_Y - 12, "PASSPHRASE", c_soft(), UI_FONT_12, 3, true);
    bool busy = (g_state == ST_WORKING || g_state == ST_DONE);
    fill(t, FIELD_X, FIELD_Y, FIELD_W, FIELD_H, th->surface);
    frame_rect(t, FIELD_X, FIELD_Y, FIELD_W, FIELD_H,
               g_state == ST_WRONG ? th->accent_coral : th->accent_berry);
    fill(t, FIELD_X, FIELD_Y + FIELD_H - 2, FIELD_W, 2,
         g_state == ST_WRONG ? th->accent_coral : th->accent_berry);

    static char dots[64];
    int n = s_len(g_pass);
    if (n > 40) n = 40;                                  // длина не должна вылезать за поле
    for (int i = 0; i < n; i++) dots[i] = (char)0x95;    // «•» в CP1251
    dots[n] = 0;
    txt(t, FIELD_X + 18, FIELD_Y + 33, dots, th->ink, UI_FONT_16, 1, false);
    if (!busy && ((g_frame / 12) & 1) == 0) {
        int wl = logical_w(text_px(dots, UI_FONT_16, 1, false));
        fill(t, FIELD_X + 20 + wl, FIELD_Y + 14, 2, 24, th->accent_berry);
    }

    // Кнопка.
    if (busy) fill(t, FIELD_X, BTN_Y, FIELD_W, BTN_H, th->surface_soft);
    else      grad_rect(t, FIELD_X, BTN_Y, FIELD_W, BTN_H);
    const char* label = g_state == ST_DONE ? "UNSEALED" : (busy ? "UNSEALING..." : "UNLOCK");
    ctxt(t, CX, BTN_Y + 32, label, busy ? c_soft() : th->on_accent, UI_FONT_14, 3, true);

    // Статус.
    static char status[96];
    const char* msg = "Enter the passphrase you chose during installation.";
    uint32_t col = c_soft();
    uint64_t now = timer_get_uptime_ms();
    switch (g_state) {
        case ST_WORKING: msg = "Deriving the key (PBKDF2-SHA256) - this takes about a second."; col = th->accent_berry; break;
        case ST_DONE:    msg = "Volume unsealed. Starting AMPLIFY-OS."; col = UI_FG_OK; break;
        case ST_ERROR:   msg = "The volume could not be opened - the drive may be damaged."; col = th->accent_coral; break;
        case ST_WRONG: {
            int p = 0;
            const char* a = "Wrong passphrase - attempt ";
            for (int i = 0; a[i]; i++) status[p++] = a[i];
            if (g_attempts >= 10) status[p++] = (char)('0' + (g_attempts / 10) % 10);
            status[p++] = (char)('0' + g_attempts % 10);
            if (now < g_wait_until) {
                const char* w = ". Wait a moment...";
                for (int i = 0; w[i]; i++) status[p++] = w[i];
            }
            status[p] = 0;
            msg = status;
            col = th->accent_coral;
            break;
        }
        default: break;
    }
    ctxt(t, CX, 644, msg, col, UI_FONT_14, 0, g_state == ST_WRONG);

    ctxt(t, CX, 930, "Enter unlocks  \xB7  Esc clears  \xB7  LUKS1  \xB7  AES-256-XTS",
         c_faint(), UI_FONT_12, 0, false);

    const mouse_state_t* m = mouse_get_state();
    if (m) cursor_draw(t, m->x, m->y);
}

static void present(void) {
    const gfx_target_t* t = doublebuf_target();
    if (!t) return;
    ui_layout_update(t->width, t->height);
    draw(t);
    doublebuf_swap();
}

static bool try_unlock(void) {
    if (!g_pass[0]) return false;
    if (timer_get_uptime_ms() < g_wait_until) return false;

    g_state = ST_WORKING;
    present();                               // кадр ДО долгого PBKDF2
    int r = fde_unlock(g_pass);
    crypto_wipe(g_pass, sizeof(g_pass));

    if (r == LUKS_OK) {
        g_state = ST_DONE;
        present();
        timer_sleep(450);
        return true;
    }
    if (r == LUKS_BAD_PASS) {
        g_attempts++;
        g_state = ST_WRONG;
        // С третьей ошибки — задержка 1, 2, 4... секунды (не больше 30).
        if (g_attempts >= 3) {
            uint64_t d = 1000ull << (g_attempts - 3 > 5 ? 5 : g_attempts - 3);
            if (d > 30000) d = 30000;
            g_wait_until = timer_get_uptime_ms() + d;
        }
    } else {
        g_state = ST_ERROR;
    }
    while (keyboard_has_char()) keyboard_get_char();   // нажатия во время PBKDF2 — в мусор
    return false;
}

void fde_unlock_screen(void) {
    if (!fde_pending()) return;
    serial_puts("[fde] unlock screen\n");
    ui_theme_set_dark(false);

    g_pass[0] = 0;
    g_state = ST_IDLE;
    g_attempts = 0;
    bool was_down = false;

    for (;;) {
        g_frame++;
        present();

        mouse_poll();
        const mouse_state_t* m = mouse_get_state();
        if (m) {
            bool down = (m->buttons & MOUSE_BTN_LEFT) != 0;
            if (down && !was_down &&
                ui_hit_logical(m->x, m->y, FIELD_X, BTN_Y, FIELD_W, BTN_H) &&
                try_unlock()) break;
            was_down = down;
        }

        if (keyboard_has_char()) {
            char c = keyboard_get_char();
            if (c == '\n' || c == '\r') {
                if (try_unlock()) break;
            } else if (c == KBD_KEY_ESC) {
                crypto_wipe(g_pass, sizeof(g_pass));
                if (g_state != ST_WORKING) g_state = ST_IDLE;
            } else if (c == '\b') {
                int n = s_len(g_pass);
                if (n) g_pass[n - 1] = 0;
            } else if (c >= 32 && c < 127) {
                int n = s_len(g_pass);
                if (n < (int)sizeof(g_pass) - 1) { g_pass[n] = c; g_pass[n + 1] = 0; }
                if (g_state == ST_WRONG && timer_get_uptime_ms() >= g_wait_until) g_state = ST_IDLE;
            }
        }
        timer_sleep(30);
    }
    serial_puts("[fde] unlock screen done\n");
}

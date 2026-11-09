// ============================================================================
// app_box.c — GUI Sandbox Manager для AMPLIFY-OS x86_64 (этап 46).
// ----------------------------------------------------------------------------
// Окно 620x420. Три таба сверху: [Boxes] [Namespaces] [Cgroups]. Снизу — лог
// последних операций + кнопки [Test] [Refresh] [Run hello] [Run strict].
//
// Run hello / Run strict — берут /hello_libc.elf из VFS, запускают через
// box_run с разными профилями. После запуска — task_yield (чтобы новая
// задача провернулась хотя бы один шаг).
//
// Используется тот же spawn_app_window() pattern, что и остальные apps;
// чтобы не делать его публичным API, мы дублируем минимальный helper.
// ============================================================================
#include "apps.h"
#include "wm.h"
#include "heap.h"
#include "graphics.h"
#include "ui_theme.h"
#include "timer.h"
#include "fb.h"
#include "serial.h"

#include "ns.h"
#include "cgroup.h"
#include "seccomp.h"
#include "box.h"
#include "task.h"

#include <stdint.h>
#include <stdbool.h>

// Из apps.c эти статики не доступны — реплицируем минимум.
static int g_box_spawn_x = 200;
static int g_box_spawn_y = 80;
static window_t* spawn_box_window(const char* title, int w, int h,
                                  uint32_t accent,
                                  window_draw_fn  draw_fn,
                                  window_click_fn click_fn,
                                  window_key_fn   key_fn) {
    window_t* win = window_create(title, g_box_spawn_x, g_box_spawn_y, w, h);
    if (!win) return 0;
    win->on_draw      = draw_fn;
    win->on_click     = click_fn;
    win->on_key       = key_fn;
    win->accent_color = accent ? accent : 0xFF38598C;
    win->bg_color     = 0xFF14182A;
    wm_add_window(win);
    g_box_spawn_x += 30; g_box_spawn_y += 30;
    if (g_box_spawn_x > 600) g_box_spawn_x = 200;
    if (g_box_spawn_y > 420) g_box_spawn_y = 80;
    return win;
}

// ---- String helpers -------------------------------------------------------
static int box_str_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static int box_utoa10(uint64_t v, char* buf) {
    int n = 0; char tmp[24]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
    while (ti) buf[n++] = tmp[--ti];
    buf[n] = 0;
    return n;
}
static void box_strcpy(char* dst, const char* src, int max) {
    int i = 0; while (i < max - 1 && src && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int box_strcmp_(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// ---- State ---------------------------------------------------------------
typedef enum {
    TAB_BOXES = 0,
    TAB_NS    = 1,
    TAB_CG    = 2,
} box_tab_t;

#define BOX_LOG_MAX     1800
#define BOX_BUTTON_W    100
#define BOX_BUTTON_H    26

typedef struct {
    box_tab_t tab;
    char      log[BOX_LOG_MAX];
    int       log_len;
    uint64_t  last_refresh_ms;
} app_box_state_t;

static void log_clear(app_box_state_t* st) { st->log[0] = 0; st->log_len = 0; }
static void log_append(app_box_state_t* st, const char* s) {
    if (!st || !s) return;
    int n = box_str_len(s);
    if (st->log_len + n + 1 >= BOX_LOG_MAX) {
        // Скользим окно: drop первую половину, перенесём вторую.
        int keep = BOX_LOG_MAX / 2;
        int drop = st->log_len - keep;
        if (drop > 0) {
            for (int i = 0; i < keep; i++) st->log[i] = st->log[drop + i];
            st->log_len = keep;
            st->log[st->log_len] = 0;
        } else {
            st->log_len = 0;
        }
    }
    for (int i = 0; i < n && st->log_len < BOX_LOG_MAX - 1; i++)
        st->log[st->log_len++] = s[i];
    st->log[st->log_len] = 0;
}

// ---- Tab buttons / action buttons ----------------------------------------
static const int TAB_X[3]    = { 14, 110, 206 };
static const int TAB_Y       = 8;
static const int TAB_W       = 90;
static const int TAB_H       = 22;

static const int BTN_X[4]    = { 14, 124, 234, 344 };
static const int BTN_Y       = 348;
static const char* BTN_LABEL[4] = { "Test", "Refresh", "Hello box", "Strict box" };

// ---- Drawing helpers ------------------------------------------------------
static void draw_button(const gfx_target_t* t, int x, int y, int w, int h,
                        const char* label, uint32_t fg, uint32_t bg, uint32_t border) {
    gfx_fill_rect (t, x, y, w, h, bg);
    gfx_stroke_rect(t, x, y, w, h, border);
    int tx = x + 8;
    int ty = y + (h - 16) / 2 + 1;
    gfx_draw_string(t, tx, ty, label, fg, bg);
}

static void draw_tabs(const gfx_target_t* t, app_box_state_t* st) {
    static const char* names[3] = { "Boxes", "Namespaces", "Cgroups" };
    for (int i = 0; i < 3; i++) {
        bool sel = (int)st->tab == i;
        uint32_t fg = sel ? UI_FG_PRIMARY : UI_FG_DIM;
        uint32_t bg = sel ? UI_BG_MID     : UI_BG_DARK;
        uint32_t br = sel ? UI_ACCENT_ORANGE : UI_BG_MID;
        draw_button(t, TAB_X[i], TAB_Y, TAB_W, TAB_H, names[i], fg, bg, br);
    }
}

static void draw_log(const gfx_target_t* t, const window_t* w, app_box_state_t* st) {
    int x0 = 14, y0 = 380;
    gfx_fill_rect(t, x0, y0, w->width - 28, 30, 0xFF080C18);
    gfx_stroke_rect(t, x0, y0, w->width - 28, 30, UI_BG_MID);
    // Печатаем последнюю строку лога (одну).
    int last_nl = -1;
    for (int i = st->log_len - 1; i >= 0; i--) {
        if (st->log[i] == '\n') { last_nl = i; break; }
    }
    int start = (last_nl >= 0) ? (last_nl + 1) : 0;
    if (start < st->log_len - 1) {
        // Найти предыдущий \n чтобы взять последнюю непустую строку.
        // Если последнее завершилось \n (start == st->log_len), берём предыдущую.
    }
    if (start >= st->log_len) {
        // Ищем непустую строку выше.
        for (int i = st->log_len - 2; i >= 0; i--) {
            if (st->log[i] == '\n') { start = i + 1; break; }
            if (i == 0) start = 0;
        }
    }
    char line[64]; int li = 0;
    for (int i = start; i < st->log_len && li < 60 && st->log[i] != '\n'; i++)
        line[li++] = st->log[i];
    line[li] = 0;
    gfx_draw_string(t, x0 + 4, y0 + 8, line, UI_FG_PRIMARY, 0xFF080C18);
}

static void draw_buttons(const gfx_target_t* t) {
    for (int i = 0; i < 4; i++) {
        draw_button(t, BTN_X[i], BTN_Y, BOX_BUTTON_W, BOX_BUTTON_H,
                    BTN_LABEL[i], UI_FG_PRIMARY, UI_BG_DARK, UI_ACCENT_GOLD);
    }
}

// ---- Tab content drawing --------------------------------------------------
static void draw_tab_boxes(const gfx_target_t* t, const window_t* w) {
    box_listing_t list[8];
    int n = box_list(list, 8);
    int y = 50;
    char buf[96]; int bp;
    gfx_draw_string(t, 14, y, "Active sandboxes (box_run):", UI_FG_OK, w->bg_color);
    y += 20;
    if (n == 0) {
        gfx_draw_string(t, 14, y, "(пусто — нажмите 'Hello box' ниже)", UI_FG_DIM, w->bg_color);
        return;
    }
    gfx_draw_string(t, 14, y, "tid  vpid cg  mem(B)  cpu(ms) blocked  name / path",
                    UI_FG_DIM, w->bg_color);
    y += 18;
    for (int i = 0; i < n && y < 330; i++) {
        bp = 0;
        bp += box_utoa10((uint64_t)list[i].tid, buf + bp);  while (bp < 4)  buf[bp++] = ' ';
        bp += box_utoa10((uint64_t)list[i].visible_pid, buf + bp); while (bp < 9)  buf[bp++] = ' ';
        bp += box_utoa10((uint64_t)list[i].cgroup_id, buf + bp);   while (bp < 13) buf[bp++] = ' ';
        bp += box_utoa10(list[i].mem_used, buf + bp);              while (bp < 21) buf[bp++] = ' ';
        bp += box_utoa10(list[i].cpu_total_ns / 1000000ULL, buf + bp); while (bp < 29) buf[bp++] = ' ';
        bp += box_utoa10((uint64_t)list[i].seccomp_blocked, buf + bp); while (bp < 38) buf[bp++] = ' ';
        // name
        for (int k = 0; list[i].name[k] && bp < 50; k++) buf[bp++] = list[i].name[k];
        buf[bp++] = ' ';
        for (int k = 0; list[i].path[k] && bp < 90; k++) buf[bp++] = list[i].path[k];
        buf[bp] = 0;
        gfx_draw_string(t, 14, y, buf, UI_FG_PRIMARY, w->bg_color);
        y += 16;
    }
}

static void draw_tab_ns(const gfx_target_t* t, const window_t* w) {
    ns_summary_t s[16];
    int n = ns_get_summaries(s, 16);
    int y = 50;
    char buf[80]; int bp;
    gfx_draw_string(t, 14, y, "Namespace registry:", UI_FG_OK, w->bg_color);
    y += 20;
    static const char* kinds[5] = { "PID", "UTS", "MNT", "NET", "USER" };
    gfx_draw_string(t, 14, y, "ns_id kind ref extra", UI_FG_DIM, w->bg_color);
    y += 18;
    for (int i = 0; i < n && y < 330; i++) {
        bp = 0;
        bp += box_utoa10((uint64_t)s[i].ns_id, buf + bp);    while (bp < 6)  buf[bp++] = ' ';
        const char* k = (s[i].kind >= 0 && s[i].kind < 5) ? kinds[s[i].kind] : "?";
        for (int kk = 0; k[kk] && bp < 11; kk++) { buf[bp++] = k[kk]; }
        while (bp < 11) buf[bp++] = ' ';
        bp += box_utoa10((uint64_t)s[i].refcount, buf + bp); while (bp < 15) buf[bp++] = ' ';
        bp += box_utoa10((uint64_t)s[i].extra,    buf + bp); buf[bp] = 0;
        uint32_t fg = (s[i].kind == 3) ? UI_ACCENT_MAGENTA : UI_FG_PRIMARY;
        gfx_draw_string(t, 14, y, buf, fg, w->bg_color);
        y += 16;
    }
}

static void draw_tab_cg(const gfx_target_t* t, const window_t* w) {
    int n = cgroup_count();
    int y = 50;
    char buf[96]; int bp;
    gfx_draw_string(t, 14, y, "Cgroups (cpu/mem/io/pids limits):", UI_FG_OK, w->bg_color);
    y += 20;
    gfx_draw_string(t, 14, y, "id name           pids   mem cur/max     cpu(ms)  thrott",
                    UI_FG_DIM, w->bg_color);
    y += 18;
    for (int i = 0; i < n && y < 330; i++) {
        cgroup_t* c = cgroup_at(i);
        if (!c) continue;
        bp = 0;
        bp += box_utoa10((uint64_t)c->id, buf + bp); while (bp < 4) buf[bp++] = ' ';
        for (int k = 0; c->name[k] && bp < 18; k++) { buf[bp++] = c->name[k]; }
        while (bp < 18) buf[bp++] = ' ';
        bp += box_utoa10((uint64_t)c->pids_current, buf + bp); buf[bp++] = '/';
        bp += box_utoa10((uint64_t)c->pids_max, buf + bp);     while (bp < 27) buf[bp++] = ' ';
        bp += box_utoa10(c->mem_current, buf + bp); buf[bp++] = '/';
        bp += box_utoa10(c->mem_max_bytes, buf + bp); while (bp < 47) buf[bp++] = ' ';
        bp += box_utoa10(c->cpu_total_ns / 1000000ULL, buf + bp); while (bp < 56) buf[bp++] = ' ';
        bp += box_utoa10(c->cpu_throttles + c->mem_oom + c->io_throttles + c->pids_denied, buf + bp);
        buf[bp] = 0;
        gfx_draw_string(t, 14, y, buf, UI_FG_PRIMARY, w->bg_color);
        y += 16;
    }
}

// ---- Drawing root ---------------------------------------------------------
static void box_draw(window_t* w, const gfx_target_t* t) {
    gfx_clear(t, w->bg_color);
    app_box_state_t* st = (app_box_state_t*)w->user_data;
    if (!st) return;
    draw_tabs(t, st);
    switch (st->tab) {
    case TAB_BOXES: draw_tab_boxes(t, w); break;
    case TAB_NS:    draw_tab_ns(t, w);    break;
    case TAB_CG:    draw_tab_cg(t, w);    break;
    }
    draw_buttons(t);
    draw_log(t, w, st);
}

// ---- Action handlers ------------------------------------------------------
static void action_test(app_box_state_t* st) {
    log_clear(st);
    log_append(st, "[box] running self-test...\n");

    cgroup_t* c = cgroup_create(cgroup_root(), "guitest");
    if (!c) { log_append(st, "[error] cgroup_create failed\n"); return; }
    cgroup_set_limit_mem(c, 1024);
    int r1 = cgroup_charge_mem(c, 512);
    int r2 = cgroup_charge_mem(c, 1000);
    bool cg_ok = (r1 == 0 && r2 == -1 && c->mem_oom == 1);
    cgroup_charge_mem(c, -512);
    cgroup_destroy(c);

    seccomp_filter_t* f = seccomp_profile("strict");
    int sr1 = seccomp_check(f, 0x101);    // WRITE allowed
    int sr2 = seccomp_check(f, 0x103);    // OPEN blocked
    bool sc_ok = (sr1 == 0 && sr2 == -2);
    seccomp_release(f);

    ns_set_t* nw = ns_unshare(ns_root(), AOS_CLONE_NEWUTS | AOS_CLONE_NEWPID);
    bool ns_ok = (nw && nw->uts != ns_root()->uts && nw->pid != ns_root()->pid);
    if (nw) ns_release(nw);

    if (cg_ok && sc_ok && ns_ok) log_append(st, "[ok] self-test PASS (cgroup+seccomp+ns)\n");
    else                          log_append(st, "[error] self-test FAIL\n");
}

static void action_refresh(app_box_state_t* st) {
    log_append(st, "[box] refresh\n");
    st->last_refresh_ms = timer_get_uptime_ms();
}

static void action_run(app_box_state_t* st, const char* profile) {
    box_spec_t spec;
    box_spec_default(&spec, "/hello_libc.elf", profile);
    box_strcpy(spec.seccomp_profile, profile, sizeof(spec.seccomp_profile));
    if (box_strcmp_(profile, "net") == 0) spec.clone_flags &= ~AOS_CLONE_NEWNET;
    char tmp[BOX_LOG_MAX]; tmp[0] = 0;
    int tid = box_run(&spec, tmp, sizeof(tmp));
    log_append(st, tmp);
    if (tid < 0) log_append(st, "[error] box_run returned <0\n");
    else {
        log_append(st, "[ok] spawned, yielding for one tick\n");
        task_yield();
    }
}

static void box_click(window_t* w, int mx, int my) {
    app_box_state_t* st = (app_box_state_t*)w->user_data;
    if (!st) return;
    int lx = mx - w->x;
    int ly = my - w->y - WINDOW_TITLE_HEIGHT;
    // Tabs.
    for (int i = 0; i < 3; i++) {
        if (lx >= TAB_X[i] && lx < TAB_X[i] + TAB_W &&
            ly >= TAB_Y    && ly < TAB_Y    + TAB_H) {
            st->tab = (box_tab_t)i;
            return;
        }
    }
    // Buttons.
    for (int i = 0; i < 4; i++) {
        if (lx >= BTN_X[i] && lx < BTN_X[i] + BOX_BUTTON_W &&
            ly >= BTN_Y    && ly < BTN_Y    + BOX_BUTTON_H) {
            switch (i) {
            case 0: action_test(st);            break;
            case 1: action_refresh(st);         break;
            case 2: action_run(st, "default");  break;
            case 3: action_run(st, "strict");   break;
            }
            return;
        }
    }
}

static void box_key(window_t* w, char c) {
    app_box_state_t* st = (app_box_state_t*)w->user_data;
    if (!st) return;
    if (c == 'T' || c == 't') action_test(st);
    else if (c == 'R' || c == 'r') action_refresh(st);
    else if (c == 'H' || c == 'h') action_run(st, "default");
    else if (c == 'S' || c == 's') action_run(st, "strict");
    else if (c == '1') st->tab = TAB_BOXES;
    else if (c == '2') st->tab = TAB_NS;
    else if (c == '3') st->tab = TAB_CG;
}

window_t* app_box_open(void) {
    app_box_state_t* st = (app_box_state_t*)kmalloc(sizeof(app_box_state_t));
    if (!st) return 0;
    st->tab = TAB_BOXES;
    st->log[0] = 0; st->log_len = 0;
    st->last_refresh_ms = 0;
    log_append(st, "Sandbox Manager (этап 46): namespaces + cgroups + seccomp.\n");
    log_append(st, "Tabs: 1=Boxes 2=Namespaces 3=Cgroups | T=test R=refresh H=hello S=strict\n");

    window_t* w = spawn_box_window("Sandbox Manager", 620, 420, UI_ACCENT_MAGENTA,
                                   box_draw, box_click, box_key);
    if (!w) { kfree(st); return 0; }
    w->user_data = st;
    w->owns_user_data = true;
    return w;
}

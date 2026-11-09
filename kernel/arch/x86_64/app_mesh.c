// ============================================================================
// app_mesh.c — Mesh: настройка сети AMPLIFY-OS (подэтап 48.2).
// ----------------------------------------------------------------------------
// До 48.2 это был нарисованный макет «топологии устройств» с тремя клавишами
// (R/T/F) и без обработчика кликов — настроить сеть было нечем. Теперь это
// рабочий экран настроек:
//   слева  — карточка интерфейса (имя, драйвер, MAC, состояние, счётчики) и
//            действующий адрес (откуда он взят, срок аренды DHCP);
//   справа — режим (Авто / DHCP / Статика), поля IP / маска / шлюз / DNS с
//            вводом и проверкой, кнопки «Применить», «Обновить DHCP»,
//            «Проверить DNS», «Сбросить кэш», строка состояния.
// Настройку применяет и хранит netcfg.c (/disk/Sanctum/network.conf).
//
// Раскладка считается одной функцией mesh_layout() — ею пользуются и
// отрисовка, и обработчик кликов. Координаты элементов один раз печатаются в
// serial (`[mesh] layout ...`, в пикселях экрана): тест кликает по ним через
// QMP и не зависит от того, как раскладка изменится завтра.
// ============================================================================
#include "apps.h"
#include "app_stage_e.h"
#include "ui_kit.h"
#include "netif.h"
#include "netcfg.h"
#include "dhcp.h"
#include "dns.h"
#include "arp.h"
#include "wm.h"
#include "heap.h"
#include "keyboard.h"
#include "timer.h"
#include "serial.h"

enum { F_IP = 0, F_MASK, F_GW, F_DNS, F_DNS2, F_COUNT };
enum { B_APPLY = 0, B_RENEW, B_TEST, B_FLUSH, B_COUNT };
enum { ST_INFO = 0, ST_OK, ST_ERR };

typedef struct {
    netcfg_mode_t mode;                 // выбранный (ещё не применённый) режим
    char     txt[F_COUNT][16];
    bool     bad[F_COUNT];
    int      focus;                     // поле с курсором, -1 — нет
    char     status[160];
    int      status_kind;
    bool     dirty;                     // есть неприменённые правки
    bool     busy;                      // идёт проверка DNS
    bool     layout_logged;
} mesh_t;

typedef struct {
    pk_rect_t iface, addr;              // левая колонка
    pk_rect_t mode, seg[3];             // правая: режим
    pk_rect_t fields, field[F_COUNT];   // поля
    pk_rect_t btn[B_COUNT];
    pk_rect_t status;
    int       label_x;                  // x подписей полей
} mesh_layout_t;

static const char* const k_field_label[F_COUNT] = {
    "IP-адрес", "Маска подсети", "Шлюз", "DNS-сервер", "Второй DNS"
};
static const char* const k_field_name[F_COUNT] = { "ip", "mask", "gateway", "dns", "dns2" };
static const char* const k_btn_label[B_COUNT] = {
    "Применить", "Обновить DHCP", "Проверить DNS", "Сбросить кэш"
};
static const char* const k_btn_name[B_COUNT] = { "apply", "renew", "testdns", "flush" };
static const char* const k_seg_label[3] = { "Авто", "DHCP", "Статика" };
static const netcfg_mode_t k_seg_mode[3] = { NETCFG_AUTO, NETCFG_DHCP, NETCFG_STATIC };

// ---------------------------------------------------------------------------
static void m_copy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static void m_cat(char* d, const char* s, int cap) {
    int n = 0;
    while (d[n]) n++;
    for (int i = 0; s && s[i] && n < cap - 1; i++) d[n++] = s[i];
    d[n] = 0;
}
static void m_cat_num(char* d, uint64_t v, int cap) {
    char t[24];
    se_num(t, v);
    m_cat(d, t, cap);
}
static void set_status(mesh_t* s, int kind, const char* text) {
    m_copy(s->status, text, sizeof(s->status));
    s->status_kind = kind;
}

static void mesh_layout(const window_t* w, mesh_layout_t* L) {
    int x0 = 78, x1 = w->width - 70;
    int content = x1 - x0;
    int lw = content * 38 / 100;
    if (lw < 380) lw = 380;
    if (lw > 560) lw = 560;
    int rx = x0 + lw + 24;
    int rw = x1 - rx;
    if (rw > 860) rw = 860;
    int top = 104;

    L->iface = (pk_rect_t){ x0, top, lw, 230 };
    L->addr  = (pk_rect_t){ x0, top + 230 + 18, lw, 252 };

    L->mode = (pk_rect_t){ rx, top, rw, 126 };
    int sw = (rw - 56 - 16) / 3;
    for (int i = 0; i < 3; i++)
        L->seg[i] = (pk_rect_t){ rx + 28 + i * (sw + 8), top + 46, sw, 40 };

    L->fields = (pk_rect_t){ rx, top + 126 + 18, rw, 286 };
    L->label_x = rx + 28;
    int fx = rx + 200;
    int fw = rw - 200 - 28;
    if (fw > 320) fw = 320;
    for (int i = 0; i < F_COUNT; i++)
        L->field[i] = (pk_rect_t){ fx, L->fields.y + 52 + i * 44, fw, 36 };

    int by = L->fields.y + L->fields.h + 18;
    int bw = (rw - 3 * 12) / 4;
    for (int i = 0; i < B_COUNT; i++)
        L->btn[i] = (pk_rect_t){ rx + i * (bw + 12), by, bw, 40 };

    L->status = (pk_rect_t){ rx, by + 40 + 16, rw, 40 };
}

// Координаты элементов — в serial, в пикселях ЭКРАНА (центры).
static void log_rect(const window_t* w, const char* name, pk_rect_t r) {
    serial_puts(" "); serial_puts(name); serial_puts("=");
    serial_print_dec((uint64_t)(w->x + r.x + r.w / 2)); serial_puts(",");
    serial_print_dec((uint64_t)(w->y + r.y + r.h / 2));
}
static void log_layout(const window_t* w, const mesh_layout_t* L) {
    serial_puts("[mesh] layout");
    log_rect(w, "auto",   L->seg[0]);
    log_rect(w, "dhcp",   L->seg[1]);
    log_rect(w, "static", L->seg[2]);
    for (int i = 0; i < F_COUNT; i++) log_rect(w, k_field_name[i], L->field[i]);
    for (int i = 0; i < B_COUNT; i++) log_rect(w, k_btn_name[i], L->btn[i]);
    serial_puts("\n");
}

// Заполнить форму из действующей настройки. Статические поля пусты (их ещё
// не задавали) — подставляем текущие значения интерфейса: чтобы перейти с
// DHCP на тот же адрес статикой, достаточно нажать «Статика» и «Применить».
static void mesh_load(mesh_t* s) {
    const netcfg_t* c = netcfg_get();
    net_interface_t* n = netif_get_default();
    uint32_t v[F_COUNT] = { c->ip, c->mask, c->gateway, c->dns, c->dns2 };
    if (c->ip == 0 && n && n->ip4_addr) {
        v[F_IP] = n->ip4_addr; v[F_MASK] = n->ip4_netmask; v[F_GW] = n->ip4_gateway;
        v[F_DNS] = dns_get_server(); v[F_DNS2] = dns_get_server2();
    }
    s->mode = c->mode;
    for (int i = 0; i < F_COUNT; i++) {
        if (v[i]) netcfg_format_ip(v[i], s->txt[i]); else s->txt[i][0] = 0;
        s->bad[i] = false;
    }
    s->focus = -1;
    s->dirty = false;
}

static bool fields_enabled(const mesh_t* s) { return s->mode != NETCFG_DHCP; }

// ---------------------------------------------------------------------------
// Действия.
// ---------------------------------------------------------------------------
static void mesh_apply(mesh_t* s) {
    netcfg_t c = { s->mode, 0, 0, 0, 0, 0 };
    uint32_t* dst[F_COUNT] = { &c.ip, &c.mask, &c.gateway, &c.dns, &c.dns2 };
    bool any_bad = false;
    for (int i = 0; i < F_COUNT; i++) {
        s->bad[i] = false;
        if (!s->txt[i][0]) continue;                       // пусто = не задано
        if (!netcfg_parse_ip(s->txt[i], dst[i])) { s->bad[i] = true; any_bad = true; }
    }
    if (any_bad && fields_enabled(s)) {
        for (int i = 0; i < F_COUNT; i++) if (s->bad[i]) {
            char m[160];
            m_copy(m, "Поле «", sizeof(m)); m_cat(m, k_field_label[i], sizeof(m));
            m_cat(m, "»: это не адрес вида 192.168.1.10", sizeof(m));
            set_status(s, ST_ERR, m);
            s->focus = i;
            break;
        }
        serial_puts("[mesh] apply rejected: bad field\n");
        return;
    }
    if (c.mode == NETCFG_STATIC && c.ip && !c.mask) {
        s->bad[F_MASK] = true; s->focus = F_MASK;
        set_status(s, ST_ERR, "Укажите маску подсети (обычно 255.255.255.0)");
        serial_puts("[mesh] apply rejected: no mask\n");
        return;
    }
    const char* why = "";
    if (!netcfg_apply(&c, &why)) {
        // Какое поле виновато, netcfg сообщает текстом; подсветим адрес —
        // с него начинается любая правка.
        if (c.ip == 0) { s->bad[F_IP] = true; s->focus = F_IP; }
        set_status(s, ST_ERR, why);
        serial_puts("[mesh] apply rejected: validation\n");
        return;
    }
    bool saved = netcfg_save();
    s->dirty = false;
    char m[160];
    m_copy(m, c.mode == NETCFG_STATIC ? "Статический адрес применён"
            : c.mode == NETCFG_DHCP   ? "Режим DHCP включён, запрос отправлен"
            :                           "Авто: DHCP, при его молчании — сохранённый адрес", sizeof(m));
    m_cat(m, saved ? " \xB7 сохранено в network.conf" : " \xB7 не сохранено (диск недоступен)", sizeof(m));
    set_status(s, saved ? ST_OK : ST_INFO, m);
    serial_puts("[mesh] apply ok mode="); serial_puts(netcfg_mode_name(c.mode));
    serial_puts(saved ? " saved\n" : " not-saved\n");
}

static void mesh_renew(mesh_t* s) {
    if (!netif_is_ready()) { set_status(s, ST_ERR, "Сетевая карта не найдена"); return; }
    if (netcfg_get()->mode == NETCFG_STATIC) {
        set_status(s, ST_INFO, "В режиме «Статика» DHCP выключен — выберите «Авто» или «DHCP» и примените");
        serial_puts("[mesh] renew ignored: static mode\n");
        return;
    }
    dhcp_start();
    set_status(s, ST_INFO, dhcp_is_bound() ? "Запрошено продление аренды DHCP"
                                           : "Запрос DHCP отправлен, жду ответ сервера");
    serial_puts("[mesh] dhcp renew requested\n");
}

static void mesh_test_dns(mesh_t* s) {
    net_interface_t* n = netif_get_default();
    if (!n)            { set_status(s, ST_ERR, "Сетевая карта не найдена"); return; }
    if (!n->ip4_addr)  { set_status(s, ST_ERR, "Нет адреса — сначала получите его по DHCP или задайте вручную"); return; }
    static const char host[] = "example.com";
    set_status(s, ST_INFO, "Проверяю DNS: example.com ...");
    s->busy = true;
    // Запрос идёт до 7 с. Чтобы экран не замирал, сетевые ожидания дёргают
    // насос кадров WM; Esc во время проверки её отменяет.
    dns_cache_flush();
    net_cancel_clear();
    net_set_idle_hook(wm_pump);
    uint64_t t0 = timer_get_uptime_ms();
    uint32_t ip = dns_resolve(host);
    uint64_t ms = timer_get_uptime_ms() - t0;
    net_set_idle_hook(0);
    s->busy = false;

    char m[160], a[16];
    if (ip) {
        netcfg_format_ip(ip, a);
        m_copy(m, "DNS работает: example.com = ", sizeof(m)); m_cat(m, a, sizeof(m));
        m_cat(m, " \xB7 ", sizeof(m)); m_cat_num(m, ms, sizeof(m)); m_cat(m, " мс", sizeof(m));
        set_status(s, ST_OK, m);
        serial_puts("[mesh] dns test ok\n");
    } else if (net_cancelled()) {
        set_status(s, ST_INFO, "Проверка DNS отменена");
        serial_puts("[mesh] dns test cancelled\n");
    } else {
        set_status(s, ST_ERR, dns_last_failure() == DNS_FAIL_NXDOMAIN
                   ? "DNS-сервер отвечает, но имя example.com не нашёл"
                   : "DNS-серверы не отвечают — проверьте адрес DNS и шлюз");
        serial_puts("[mesh] dns test failed\n");
    }
    net_cancel_clear();
}

static void mesh_flush(mesh_t* s) {
    dns_cache_flush();
    arp_cache_flush();
    set_status(s, ST_OK, "Кэш DNS и таблица соседей (ARP) очищены");
    serial_puts("[mesh] caches flushed\n");
}

static void mesh_button(mesh_t* s, int b) {
    switch (b) {
        case B_APPLY: mesh_apply(s);    break;
        case B_RENEW: mesh_renew(s);    break;
        case B_TEST:  mesh_test_dns(s); break;
        case B_FLUSH: mesh_flush(s);    break;
    }
}

static void set_mode(mesh_t* s, netcfg_mode_t m) {
    if (s->mode == m) return;
    s->mode = m;
    s->dirty = true;
    if (!fields_enabled(s)) s->focus = -1;
    set_status(s, ST_INFO, m == NETCFG_STATIC ? "Статика: адрес из полей ниже, DHCP выключен. Нажмите «Применить»"
                         : m == NETCFG_DHCP   ? "DHCP: адрес выдаёт сервер сети. Нажмите «Применить»"
                         :                      "Авто: DHCP, а если он не ответит за 3 с — адрес из полей ниже. Нажмите «Применить»");
    serial_puts("[mesh] mode selected: "); serial_puts(netcfg_mode_name(m)); serial_puts("\n");
}

// ---------------------------------------------------------------------------
// Отрисовка.
// ---------------------------------------------------------------------------
static void row(const gfx_target_t* t, int x, int y, int value_x, int max_w,
                const char* label, const char* value, uint32_t color) {
    const ui_runtime_theme_t* h = ui_theme_current();
    pk_text(t, x, y, label, h->muted, UI_FONT_14);
    pk_text_clip(t, value_x, y, max_w, value, color, UI_FONT_14, true);
}

static void fmt_bytes(char* out, int cap, uint64_t b) {
    out[0] = 0;
    if (b >= 10ull * 1024 * 1024) { m_cat_num(out, b / (1024 * 1024), cap); m_cat(out, " МБ", cap); }
    else if (b >= 10 * 1024)      { m_cat_num(out, b / 1024, cap);          m_cat(out, " КБ", cap); }
    else                          { m_cat_num(out, b, cap);                 m_cat(out, " Б", cap); }
}

static void fmt_lease(char* out, int cap, uint32_t sec) {
    out[0] = 0;
    if (sec >= 3600) { m_cat_num(out, sec / 3600, cap); m_cat(out, " ч ", cap); m_cat_num(out, (sec % 3600) / 60, cap); m_cat(out, " мин", cap); }
    else if (sec >= 60) { m_cat_num(out, sec / 60, cap); m_cat(out, " мин ", cap); m_cat_num(out, sec % 60, cap); m_cat(out, " с", cap); }
    else { m_cat_num(out, sec, cap); m_cat(out, " с", cap); }
}

static void mesh_draw(window_t* w, const gfx_target_t* t) {
    mesh_t* s = w->user_data;
    const ui_runtime_theme_t* h = ui_theme_current();
    mesh_layout_t L;
    mesh_layout(w, &L);
    if (!s->layout_logged) { s->layout_logged = true; log_layout(w, &L); }

    gfx_clear(t, h->canvas);
    se_header(t, "MESH - NETWORK", "Сеть", false);

    net_interface_t* n = netif_get_default();
    netcfg_source_t src = netcfg_source();
    char buf[96], ip[16];

    // ---- интерфейс ---------------------------------------------------------
    pk_card(t, L.iface);
    {
        int x = L.iface.x + 28, y = L.iface.y + 22;
        pk_label(t, x, y, "ИНТЕРФЕЙС");
        gfx_fill_rect_gradient(t, x, y + 30, 56, 56, h->accent_orange, h->accent_berry);
        ui_draw_bitmap_scaled(t, x + 16, y + 46, 24, 24, ui_icon(UI_ICON_AMPLIFY));
        pk_text_bold(t, x + 74, y + 30, n ? n->name : "нет карты", h->ink, UI_FONT_20);
        pk_text(t, x + 74, y + 60, n ? netif_driver_name() : "драйвер не загружен", h->muted, UI_FONT_14);

        const char* st; uint32_t dot;
        if (!n)                               { st = "Нет сетевой карты";   dot = h->accent_berry; }
        else if (src == NETCFG_SRC_DHCP)      { st = "Подключено \xB7 DHCP";   dot = 0xFF46A171; }
        else if (src == NETCFG_SRC_STATIC)    { st = "Подключено \xB7 статика"; dot = 0xFF46A171; }
        else if (src == NETCFG_SRC_FALLBACK)  { st = "Запасной адрес \xB7 DHCP молчит"; dot = h->accent_orange; }
        else if (dhcp_is_enabled())           { st = "Ищу DHCP-сервер";     dot = h->accent_orange; }
        else                                  { st = "Адрес не задан";      dot = h->accent_berry; }
        pk_chip(t, x, y + 104, st, dot);

        int vx = x + 150, mw = L.iface.w - 150 - 56;
        if (n) {
            static const char hx[] = "0123456789ABCDEF";
            int p = 0;
            for (int i = 0; i < 6; i++) { buf[p++] = hx[n->mac[i] >> 4]; buf[p++] = hx[n->mac[i] & 15]; if (i < 5) buf[p++] = ':'; }
            buf[p] = 0;
        } else m_copy(buf, "—", sizeof(buf));
        row(t, x, y + 146, vx, mw, "MAC", buf, h->ink);

        char rx[32], tx[32];
        fmt_bytes(rx, sizeof(rx), n ? n->rx_bytes : 0);
        fmt_bytes(tx, sizeof(tx), n ? n->tx_bytes : 0);
        m_copy(buf, rx, sizeof(buf)); m_cat(buf, " / ", sizeof(buf)); m_cat(buf, tx, sizeof(buf));
        row(t, x, y + 172, vx, mw, "Принято / отдано", buf, h->ink);
    }

    // ---- действующий адрес -------------------------------------------------
    pk_card(t, L.addr);
    {
        int x = L.addr.x + 28, y = L.addr.y + 22;
        int vx = x + 150, mw = L.addr.w - 150 - 56;
        pk_label(t, x, y, "ДЕЙСТВУЮЩИЙ АДРЕС");
        if (n && n->ip4_addr) netcfg_format_ip(n->ip4_addr, ip); else m_copy(ip, "нет", sizeof(ip));
        pk_text_bold(t, x, y + 26, ip, h->ink, UI_FONT_28);

        int ry = y + 76;
        if (n && n->ip4_addr) netcfg_format_ip(n->ip4_netmask, buf); else m_copy(buf, "—", sizeof(buf));
        row(t, x, ry, vx, mw, "Маска", buf, h->ink);
        if (n && n->ip4_gateway) netcfg_format_ip(n->ip4_gateway, buf); else m_copy(buf, "—", sizeof(buf));
        row(t, x, ry + 26, vx, mw, "Шлюз", buf, h->ink);
        if (dns_get_server()) {
            netcfg_format_ip(dns_get_server(), buf);
            if (dns_get_server2()) { char b2[16]; netcfg_format_ip(dns_get_server2(), b2); m_cat(buf, ", ", sizeof(buf)); m_cat(buf, b2, sizeof(buf)); }
        } else m_copy(buf, "публичные (8.8.8.8, 1.1.1.1)", sizeof(buf));
        row(t, x, ry + 52, vx, mw, "DNS", buf, h->ink);

        const char* from = src == NETCFG_SRC_DHCP     ? "DHCP-сервер"
                         : src == NETCFG_SRC_STATIC   ? "задан вручную"
                         : src == NETCFG_SRC_FALLBACK ? "запасной (DHCP не ответил)"
                         :                              "—";
        if (src == NETCFG_SRC_DHCP) {
            char sv[16]; netcfg_format_ip(dhcp_get_server(), sv);
            m_copy(buf, "DHCP-сервер ", sizeof(buf)); m_cat(buf, sv, sizeof(buf));
            from = buf;
        }
        row(t, x, ry + 78, vx, mw, "Источник", from, h->ink);

        if (src == NETCFG_SRC_DHCP) {
            char ls[40];
            if (dhcp_lease_left_s()) fmt_lease(ls, sizeof(ls), dhcp_lease_left_s());
            else m_copy(ls, "бессрочно", sizeof(ls));
            row(t, x, ry + 104, vx, mw, "Аренда, осталось", ls, h->ink);
        } else if (dhcp_is_enabled()) {
            m_copy(buf, "DHCP: ", sizeof(buf)); m_cat(buf, dhcp_state_name(), sizeof(buf));
            row(t, x, ry + 104, vx, mw, "Клиент", buf, h->muted);
        }
    }

    // ---- режим -------------------------------------------------------------
    pk_card(t, L.mode);
    pk_label(t, L.mode.x + 28, L.mode.y + 22, "РЕЖИМ");
    for (int i = 0; i < 3; i++)
        pk_segment(t, L.seg[i], k_seg_label[i], s->mode == k_seg_mode[i],
                   pk_pointer_state(w, L.seg[i], false));
    pk_text_clip(t, L.mode.x + 28, L.mode.y + 96, L.mode.w - 56,
                 s->mode == NETCFG_STATIC ? "Адрес задаётся вручную, DHCP выключен."
               : s->mode == NETCFG_DHCP   ? "Адрес выдаёт DHCP-сервер; запросы повторяются, пока он не ответит."
               :                            "Сначала DHCP; если сервер молчит — адрес из полей ниже.",
                 h->muted, UI_FONT_12, false);

    // ---- поля --------------------------------------------------------------
    pk_card(t, L.fields);
    pk_label(t, L.fields.x + 28, L.fields.y + 22,
             s->mode == NETCFG_AUTO ? "ЗАПАСНОЙ СТАТИЧЕСКИЙ АДРЕС" : "СТАТИЧЕСКИЙ АДРЕС");
    bool en = fields_enabled(s);
    for (int i = 0; i < F_COUNT; i++) {
        pk_text(t, L.label_x, L.field[i].y + 9, k_field_label[i], en ? h->ink : h->muted, UI_FONT_14);
        pk_field(t, L.field[i], s->txt[i],
                 i == F_MASK ? "255.255.255.0" : i >= F_GW ? "не задан" : "192.168.1.10",
                 en && s->focus == i, en && s->bad[i], !en, UI_FONT_16);
    }

    // ---- кнопки и состояние ------------------------------------------------
    for (int i = 0; i < B_COUNT; i++) {
        bool dis = s->busy;
        pk_button(t, L.btn[i], k_btn_label[i], i == B_APPLY, pk_pointer_state(w, L.btn[i], dis));
    }
    if (s->status[0]) {
        uint32_t c = s->status_kind == ST_ERR ? h->accent_berry
                   : s->status_kind == ST_OK  ? 0xFF2F8A5B : h->muted;
        gfx_fill_round_rect(t, L.status.x, L.status.y, 4, L.status.h, 2, c);
        pk_text_clip(t, L.status.x + 16, L.status.y + 11, L.status.w - 16, s->status,
                     s->status_kind == ST_INFO ? h->ink : c, UI_FONT_14, s->status_kind != ST_INFO);
    }
    pk_text(t, 78, w->height - 34,
            "Tab - следующее поле   Enter - применить   R - обновить DHCP   T - проверить DNS   Esc - закрыть",
            h->muted, UI_FONT_12);
}

// ---------------------------------------------------------------------------
// Ввод.
// ---------------------------------------------------------------------------
static void mesh_click(window_t* w, int lx, int ly) {
    mesh_t* s = w->user_data;
    if (s->busy) return;
    mesh_layout_t L;
    mesh_layout(w, &L);
    for (int i = 0; i < 3; i++)
        if (pk_hit(L.seg[i], lx, ly)) { set_mode(s, k_seg_mode[i]); return; }
    for (int i = 0; i < F_COUNT; i++)
        if (pk_hit(L.field[i], lx, ly)) {
            if (fields_enabled(s)) {
                s->focus = i;
                serial_puts("[mesh] focus "); serial_puts(k_field_name[i]); serial_puts("\n");
            }
            return;
        }
    for (int i = 0; i < B_COUNT; i++)
        if (pk_hit(L.btn[i], lx, ly)) { mesh_button(s, i); return; }
    s->focus = -1;                       // клик мимо — снять курсор с поля
}

static void mesh_key(window_t* w, char c) {
    mesh_t* s = w->user_data;
    if (s->busy) return;
    if (c == 27) { se_close(w); return; }
    bool en = fields_enabled(s);
    if (c == '\n') { mesh_apply(s); return; }
    if (c == '\t' || c == KBD_KEY_DOWN) { if (en) s->focus = (s->focus + 1) % F_COUNT; return; }
    if (c == KBD_KEY_BACKTAB || c == KBD_KEY_UP) { if (en) s->focus = (s->focus + F_COUNT - (s->focus < 0 ? 0 : 1)) % F_COUNT; return; }
    if (en && s->focus >= 0) {
        char* f = s->txt[s->focus];
        int n = se_len(f);
        if (c == '\b') { if (n > 0) { f[n - 1] = 0; s->bad[s->focus] = false; s->dirty = true; } return; }
        if ((c >= '0' && c <= '9') || c == '.') {
            if (n < 15) { f[n] = c; f[n + 1] = 0; s->bad[s->focus] = false; s->dirty = true; }
            return;
        }
    }
    // Буквы в адрес не входят — ими остаются быстрые команды прежнего Mesh.
    if (c == 'r' || c == 'R') mesh_renew(s);
    else if (c == 't' || c == 'T') mesh_test_dns(s);
    else if (c == 'f' || c == 'F') mesh_flush(s);
    else if (c == 'a' || c == 'A') set_mode(s, NETCFG_AUTO);
    else if (c == 'd' || c == 'D') set_mode(s, NETCFG_DHCP);
    else if (c == 's' || c == 'S') set_mode(s, NETCFG_STATIC);
}

window_t* app_mesh_open(void) {
    mesh_t* s = kmalloc(sizeof(*s));
    if (!s) return 0;
    for (size_t i = 0; i < sizeof(*s); i++) ((uint8_t*)s)[i] = 0;
    mesh_load(s);
    set_status(s, ST_INFO, netif_is_ready() ? "" : "Сетевая карта не найдена - настройки можно задать, но применить будет некуда");
    window_t* w = se_workspace("Mesh", mesh_draw, mesh_click, mesh_key, s, false);
    if (!w) kfree(s);
    else serial_puts("[mesh] open\n");
    return w;
}

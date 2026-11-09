// ============================================================================
// app_browser.c — браузер AMPLIFY-OS в стиле Pearl (подэтап 48.2).
// ----------------------------------------------------------------------------
// Было (48.1, apps.c): классическое окно 760x520, моноширинный текст 8x16 одним
// цветом, синие кнопки, ответ не больше 160 КБ, а на время загрузки весь
// интерфейс замирал, потому что net_fetch блокировал цикл WM. Об ошибке
// сообщала строка «см. serial».
//
// Стало: workspace-приложение.
//   * панель: Назад / Вперёд / Обновить (во время загрузки — Стоп) / Домой,
//     адресная строка с индикатором соединения (HTTPS проверен / HTTPS без
//     проверки сертификата / HTTP), переключатель «HTML» (исходный текст);
//   * полоса загрузки и строка этапа (поиск адреса, соединение, TLS, тело);
//   * стартовая страница с плитками избранного;
//   * типографика: документ разбирается в блоки (заголовки h1-h3, абзацы,
//     списки с маркерами и номерами, цитаты, pre) и стилевые отрезки (жирный,
//     код, ссылка); строки верстаются пропорциональным шрифтом ui_font один
//     раз на страницу, а не на каждый кадр;
//   * прокрутка: стрелки, PgUp/PgDn, Home/End, пробел, колесо, полоса справа;
//   * история назад/вперёд с запоминанием места на странице;
//   * страницы ошибок: нет сети, нет адреса, DNS, нет ответа, отказ, TLS,
//     сброс, таймаут, код HTTP — с кнопками «Повторить» и «Настройки сети».
//
// Загрузка по-прежнему синхронная (TCP-сокет в системе один, потоков нет), но
// интерфейс не замирает: сетевые ожидания зовут net_idle(), а она — насос
// кадров wm_pump: курсор двигается, полоса загрузки идёт, Esc отменяет.
// Остальной ввод на время загрузки игнорируется.
//
// «Интернет до конца» (48.2, настоящие сайты):
//   * в адресной строке можно писать не только адрес: текст с пробелом или
//     без точки уходит в поиск (DuckDuckGo без скриптов), а ссылки выдачи
//     вида duckduckgo.com/l/?uddg=<адрес> раскрываются в сам адрес;
//   * поле поиска на странице (<form> с текстовым <input>) показывается
//     ссылкой «[ ... ]»: щелчок — и запрос набирается в адресной строке;
//   * <meta http-equiv="refresh"> выполняется как перенаправление;
//   * скрытые элементы (hidden, aria-hidden, display:none) не показываются;
//   * страница открывается с основного содержимого (<main>, <article>,
//     role="main"), меню сайта остаётся выше — Home возвращает к началу;
//   * HTML узнаётся по содержимому, а не только по Content-Type;
//   * адрес с %D0%.. показывается буквами, пока строка не в фокусе.
//
// Вне рамок (честно): картинки, CSS, JavaScript, формы кроме поля поиска,
// таблицы (ячейки идут текстом через « | »), курсив, кодировки кроме UTF-8 и
// Windows-1251, TLS 1.3, HTML больше 4 МБ (обрезается с пометкой), текст
// страницы больше 512 КБ.
// ============================================================================
#include "apps.h"
#include "app_stage_e.h"
#include "ui_kit.h"
#include "ui_icons.h"
#include "http.h"
#include "https.h"
#include "netif.h"
#include "dns.h"
#include "wm.h"
#include "heap.h"
#include "keyboard.h"
#include "mouse.h"
#include "timer.h"
#include "serial.h"

#define BR_URL        NET_URL_MAX
#define BR_RAW_CAP    (4u * 1024 * 1024)       // предел HTML (было 2 МБ: dzen.ru — 3.2 МБ, текст в конце)
#define BR_TEXT_CAP   (512u * 1024)            // текст после разбора
#define BR_STYLE_MAX  32768
#define BR_BLOCK_MAX  16384
#define BR_LINE_MAX   40000
#define BR_LINK_MAX   2048
#define BR_HREF_POOL  (160u * 1024)
#define BR_HIST       32
#define BR_TITLE      128
#define BR_START_URL  "amplify://start"
#define BR_FORM_PFX   "amplify-form:"                    // псевдоссылка поля поиска
#define BR_SEARCH_URL "https://html.duckduckgo.com/html/?q="
#define BR_REFRESH_MAX 4                                 // <meta refresh> подряд

enum { BT_P = 0, BT_H1, BT_H2, BT_H3, BT_LI, BT_PRE, BT_HR };
enum { SF_BOLD = 1, SF_CODE = 2, SF_MUTED = 4 };
enum { VIEW_START = 0, VIEW_PAGE, VIEW_ERROR, VIEW_LOADING };
enum { TB_BACK = 0, TB_FWD, TB_RELOAD, TB_HOME, TB_COUNT };

typedef struct { uint32_t pos; uint16_t link; uint8_t flags; } br_style_t;   // link: 0 — нет, иначе номер+1
typedef struct { uint32_t start, end; uint16_t num; uint8_t type, depth, quote; } br_block_t;
typedef struct {
    uint32_t start, end;        // отрезок текста
    uint32_t sidx;              // стилевая точка, действующая в начале строки
    int32_t  y;
    uint16_t x, h, num;
    uint8_t  type, first, quote, font;
} br_line_t;

typedef struct {
    // --- документ (буферы выделяются один раз) ---
    char*       raw;
    char*       text;
    br_style_t* styles;
    br_block_t* blocks;
    br_line_t*  lines;
    char*       hrefs;
    uint32_t*   href_off;
    int raw_len, text_len, n_styles, n_blocks, n_lines, n_links, href_used;
    int doc_h, layout_w;
    char title[BR_TITLE];
    bool text_overflow;

    // --- навигация ---
    char url[BR_URL];
    char edit[BR_URL];
    int  edit_len;
    bool addr_focus, edit_fresh;
    char hist[BR_HIST][BR_URL];
    int  hist_scroll[BR_HIST];
    int  hist_n, hist_pos;

    int  view;
    bool show_source;
    int  scroll;
    net_fetch_t f;
    net_error_t err;
    dns_failure_t dns_fail;
    bool loading;
    uint64_t load_t0;
    uint32_t load_ms;

    bool drag_scroll;
    int  drag_off;
    bool layout_logged, links_logged;
    window_t* win;

    // --- 48.2: настоящие сайты ---
    int  main_pos;              // позиция текста, с которой идёт основное содержимое (-1 — нет)
    bool jump_main;             // после вёрстки прокрутить к нему
    char refresh[BR_URL];       // цель <meta http-equiv="refresh">
    char form_prefix[BR_URL];   // «адрес формы + имя поля=»: к нему допишется запрос
} browser_t;

typedef struct {
    pk_rect_t tb[TB_COUNT], addr, src, bar, card, vp, track, status;
    int col_x, col_w;                  // колонка текста внутри vp
} br_layout_t;

static browser_t* g_br = 0;

// ---------------------------------------------------------------------------
// Строки.
// ---------------------------------------------------------------------------
static int  b_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void b_copy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static void b_cat(char* d, const char* s, int cap) {
    int n = b_len(d);
    for (int i = 0; s && s[i] && n < cap - 1; i++) d[n++] = s[i];
    d[n] = 0;
}
static void b_cat_num(char* d, uint64_t v, int cap) { char t[24]; se_num(t, v); b_cat(d, t, cap); }
static bool b_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static bool b_starts(const char* s, const char* p) { while (*p) if (*s++ != *p++) return false; return true; }
static char b_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static void fmt_size(char* out, int cap, uint64_t b) {
    out[0] = 0;
    if (b >= 1024 * 1024) {
        b_cat_num(out, b / (1024 * 1024), cap); b_cat(out, ".", cap);
        b_cat_num(out, (b % (1024 * 1024)) * 10 / (1024 * 1024), cap); b_cat(out, " МБ", cap);
    } else if (b >= 1024) { b_cat_num(out, b / 1024, cap); b_cat(out, " КБ", cap); }
    else                  { b_cat_num(out, b, cap);        b_cat(out, " Б", cap); }
}

static void serial_hex32(uint32_t v) {
    static const char hx[] = "0123456789abcdef";
    char o[9];
    for (int i = 0; i < 8; i++) o[i] = hx[(v >> (28 - 4 * i)) & 15];
    o[8] = 0;
    serial_puts(o);
}

// ---------------------------------------------------------------------------
// HTML: низкоуровневые помощники.
// ---------------------------------------------------------------------------
static bool tag_is(const char* p, int len, const char* name) {
    int i = 0;
    for (; i < len; i++) {
        if (!name[i]) return false;
        if (b_lower(p[i]) != name[i]) return false;
    }
    return name[i] == 0;
}
static bool tag_in(const char* p, int len, const char* const* list) {
    for (int i = 0; list[i]; i++) if (tag_is(p, len, list[i])) return true;
    return false;
}

// Unicode → CP1251 (шрифты и строки ядра — CP1251).
static unsigned char uni_to_cp1251(uint32_t u) {
    if (u < 0x80) return (unsigned char)u;
    if (u >= 0x410 && u <= 0x44F) return (unsigned char)(0xC0 + (u - 0x410));
    switch (u) {
        case 0x401: return 0xA8;   case 0x451: return 0xB8;   // Ё ё
        case 0x404: return 0xAA;   case 0x454: return 0xBA;
        case 0x406: return 0xB2;   case 0x456: return 0xB3;
        case 0x407: return 0xAF;   case 0x457: return 0xBF;
        case 0x490: return 0xA5;   case 0x491: return 0xB4;
        case 0x00A0: return ' ';
        case 0x00AB: return 0xAB;  case 0x00BB: return 0xBB;   // « »
        case 0x00B0: return 0xB0;  case 0x00B7: return 0xB7;
        case 0x00A9: return 0xA9;  case 0x00AE: return 0xAE;   // (c) (R)
        case 0x00A7: return 0xA7;  case 0x00B1: return 0xB1;
        case 0x2013: return 0x96;  case 0x2014: return 0x97;   // тире
        case 0x2212: return '-';
        case 0x2018: return 0x91;  case 0x2019: return 0x92;
        case 0x201A: return 0x82;  case 0x201C: return 0x93;
        case 0x201D: return 0x94;  case 0x201E: return 0x84;
        case 0x2022: return 0x95;  case 0x2026: return 0x85;
        case 0x2122: return 0x99;  case 0x20AC: return 0x88;   // TM, евро
        case 0x2116: return 0xB9;                              // №
        case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0x00AD: return 0;  // невидимые
        default: break;
    }
    // Латиница с диакритикой — базовой буквой (читаемо, хоть и без значков).
    if (u >= 0xC0 && u <= 0xFF) {
        static const char base[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
        return (unsigned char)base[u - 0xC0];
    }
    return '?';
}

static int utf8_decode(const char* p, int len, int i, uint32_t* out) {
    unsigned char c = (unsigned char)p[i];
    if (c < 0x80) { *out = c; return 1; }
    if ((c & 0xE0) == 0xC0 && i + 1 < len) {
        *out = ((uint32_t)(c & 0x1F) << 6) | (uint32_t)((unsigned char)p[i+1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < len) {
        *out = ((uint32_t)(c & 0x0F) << 12) |
               ((uint32_t)((unsigned char)p[i+1] & 0x3F) << 6) |
               (uint32_t)((unsigned char)p[i+2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < len) {
        *out = 0xFFFD;                       // вне BMP (эмодзи) — у нас глифов нет
        return 4;
    }
    *out = '?';
    return 1;
}

// HTML-сущности. Возвращает число съеденных байт (0 — не сущность).
static int parse_entity(const char* p, int len, int i, uint32_t* out) {
    if (p[i] != '&') return 0;
    int j = i + 1;
    if (j < len && p[j] == '#') {
        j++;
        uint32_t v = 0;
        bool hex = false;
        if (j < len && (p[j] == 'x' || p[j] == 'X')) { hex = true; j++; }
        int digits = 0;
        while (j < len && digits < 8) {
            char c = p[j];
            int d = -1;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            if (d < 0) break;
            v = v * (hex ? 16u : 10u) + (uint32_t)d;
            j++; digits++;
        }
        if (!digits) return 0;
        if (j < len && p[j] == ';') j++;
        *out = v;
        return j - i;
    }
    static const struct { const char* name; uint32_t cp; } ents[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' },
        { "apos", '\'' }, { "nbsp", 0x00A0 }, { "mdash", 0x2014 },
        { "ndash", 0x2013 }, { "laquo", 0x00AB }, { "raquo", 0x00BB },
        { "hellip", 0x2026 }, { "copy", 0x00A9 }, { "reg", 0x00AE },
        { "trade", 0x2122 }, { "deg", 0x00B0 }, { "middot", 0x00B7 },
        { "bull", 0x2022 }, { "rsquo", 0x2019 }, { "lsquo", 0x2018 },
        { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bdquo", 0x201E },
        { "euro", 0x20AC }, { "sect", 0x00A7 }, { "plusmn", 0x00B1 },
        { "times", 'x' }, { "minus", '-' }, { "shy", 0x00AD }, { "thinsp", ' ' },
        { "ensp", ' ' }, { "emsp", ' ' }, { "rarr", '>' }, { "larr", '<' }, { 0, 0 }
    };
    for (int k = 0; ents[k].name; k++) {
        int n = b_len(ents[k].name);
        if (j + n < len) {
            bool m = true;
            for (int q = 0; q < n; q++) if (p[j+q] != ents[k].name[q]) { m = false; break; }
            if (m && p[j+n] == ';') { *out = ents[k].cp; return (j + n + 1) - i; }
        }
    }
    return 0;
}

// Пропустить содержимое элемента целиком (script/style/…): до </name>.
static int skip_element(const char* p, int len, int from, const char* name, int nlen) {
    for (int i = from; i + 1 < len; i++) {
        if (p[i] != '<' || p[i+1] != '/') continue;
        int j = i + 2;
        bool match = true;
        for (int q = 0; q < nlen; q++)
            if (j + q >= len || b_lower(p[j+q]) != b_lower(name[q])) { match = false; break; }
        if (!match) continue;
        j += nlen;
        while (j < len && p[j] != '>') j++;
        return (j < len) ? j + 1 : len;
    }
    return len;
}

// То же, но с учётом вложенности одноимённых тегов (<div> в <div>).
// Возвращает -1, если элемент так и не закрыт: тогда его лучше не пропускать
// вовсе, чем потерять остаток страницы.
static int skip_nested(const char* p, int len, int from, const char* name, int nlen) {
    int depth = 1;
    for (int i = from; i + 1 < len; i++) {
        if (p[i] != '<') continue;
        int j = i + 1;
        bool closing = false;
        if (p[j] == '/') { closing = true; j++; }
        bool match = true;
        for (int q = 0; q < nlen; q++)
            if (j + q >= len || b_lower(p[j+q]) != b_lower(name[q])) { match = false; break; }
        if (!match) continue;
        char e = (j + nlen < len) ? p[j + nlen] : '>';
        if (!(e == '>' || e == ' ' || e == '\n' || e == '\t' || e == '\r' || e == '/')) continue;
        if (!closing) { depth++; continue; }
        if (--depth == 0) {
            while (j < len && p[j] != '>') j++;
            return (j < len) ? j + 1 : len;
        }
    }
    return -1;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// Скрыт ли элемент: атрибут hidden, aria-hidden="true" или style с
// display:none. Смотрим ИМЕНА атрибутов (class="hidden" — это уже CSS сайта,
// его мы не знаем).
// Исключение — <div hidden id="S:0">: так React (Next.js и др.) при потоковой
// отдаче страницы присылает НАСТОЯЩЕЕ содержимое, которое скрипт потом
// переставляет на место заглушки <template id="B:0">. Скриптов у нас нет —
// такое «скрытое» и есть страница (avito.ru: без исключения текста ноль).
static bool tag_hidden(const char* p, int len) {
    bool hidden = false, react_slot = false;
    int i = 0;
    while (i < len) {
        while (i < len && (is_ws(p[i]) || p[i] == '/')) i++;
        int ns = i;
        while (i < len && p[i] != '=' && !is_ws(p[i]) && p[i] != '/') i++;
        int nl = i - ns;
        if (nl == 0) { i++; continue; }
        while (i < len && is_ws(p[i])) i++;
        int vs = i, vl = 0;
        if (i < len && p[i] == '=') {
            i++;
            while (i < len && is_ws(p[i])) i++;
            if (i < len && (p[i] == '"' || p[i] == '\'')) {
                char q = p[i++];
                vs = i;
                while (i < len && p[i] != q) i++;
                vl = i - vs;
                if (i < len) i++;
            } else {
                vs = i;
                while (i < len && !is_ws(p[i])) i++;
                vl = i - vs;
            }
        }
        if (tag_is(p + ns, nl, "hidden") && !(vl == 5 && tag_is(p + vs, vl, "false"))) hidden = true;
        if (tag_is(p + ns, nl, "aria-hidden") && vl == 4 && tag_is(p + vs, vl, "true")) hidden = true;
        if (tag_is(p + ns, nl, "id") && vl >= 3 && p[vs] == 'S' && p[vs + 1] == ':') react_slot = true;
        if (tag_is(p + ns, nl, "style")) {
            for (int k = vs; k + 12 <= vs + vl; k++) {
                if (!tag_is(p + k, 7, "display")) continue;
                int m = k + 7;
                while (m < vs + vl && (p[m] == ' ' || p[m] == ':')) m++;
                if (m + 4 <= vs + vl && tag_is(p + m, 4, "none")) hidden = true;
            }
        }
    }
    return hidden && !react_slot;
}

// %XX и «+» -> байты (адрес из параметра ссылки-обёртки, показ адреса).
static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static int url_unescape(const char* in, int n, char* out, int cap, bool plus_space) {
    int o = 0;
    for (int i = 0; i < n && o < cap - 1; i++) {
        if (in[i] == '%' && i + 2 < n && hexval(in[i+1]) >= 0 && hexval(in[i+2]) >= 0) {
            out[o++] = (char)(hexval(in[i+1]) * 16 + hexval(in[i+2]));
            i += 2;
        } else if (in[i] == '+' && plus_space) out[o++] = ' ';
        else out[o++] = in[i];
    }
    out[o] = 0;
    return o;
}

// Текст для строки запроса: буквы, цифры и -_.~ как есть, пробел — «+»,
// остальное — %XX.
static void url_escape_cat(char* d, const char* in, int cap) {
    static const char hx[] = "0123456789ABCDEF";
    int n = b_len(d);
    for (int i = 0; in[i] && n < cap - 4; i++) {
        unsigned char c = (unsigned char)in[i];
        bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                     c == '-' || c == '_' || c == '.' || c == '~';
        if (plain) d[n++] = (char)c;
        else if (c == ' ') d[n++] = '+';
        else { d[n++] = '%'; d[n++] = hx[c >> 4]; d[n++] = hx[c & 15]; }
    }
    d[n] = 0;
}

// Поисковая выдача DuckDuckGo ведёт не на сайт, а на свою пересылку
// //duckduckgo.com/l/?uddg=<адрес>&rut=...: раскрываем её на месте, чтобы
// щелчок шёл сразу на сайт (и адрес в строке состояния был настоящим).
static void unwrap_redirect(char* url, int cap) {
    const char* host = 0;
    for (int i = 0; url[i]; i++) if (b_starts(url + i, "duckduckgo.com/l/?")) { host = url + i; break; }
    if (!host) return;
    const char* q = 0;
    for (int i = 0; host[i]; i++) if (b_starts(host + i, "uddg=")) { q = host + i + 5; break; }
    if (!q) return;
    int n = 0;
    while (q[n] && q[n] != '&') n++;
    static char dec[BR_URL];
    url_unescape(q, n, dec, sizeof(dec), false);
    if (b_starts(dec, "http://") || b_starts(dec, "https://")) b_copy(url, dec, cap);
}

// Значение атрибута из текста атрибутов тега.
static bool get_attr(const char* p, int len, const char* name, char* out, int cap) {
    int nlen = b_len(name);
    for (int i = 0; i + nlen < len; i++) {
        bool boundary = (i == 0) || p[i-1] == ' ' || p[i-1] == '\t' || p[i-1] == '\n' || p[i-1] == '\r';
        if (!boundary) continue;
        bool m = true;
        for (int q = 0; q < nlen; q++) if (b_lower(p[i+q]) != name[q]) { m = false; break; }
        if (!m) continue;
        int j = i + nlen;
        while (j < len && (p[j] == ' ' || p[j] == '\t')) j++;
        if (j >= len || p[j] != '=') continue;
        j++;
        while (j < len && (p[j] == ' ' || p[j] == '\t')) j++;
        char quote = 0;
        if (j < len && (p[j] == '"' || p[j] == '\'')) { quote = p[j]; j++; }
        int o = 0;
        while (j < len && o < cap - 1) {
            char c = p[j];
            if (quote && c == quote) break;
            if (!quote && (c == ' ' || c == '\t' || c == '>' || c == '\n')) break;
            uint32_t u = 0;
            int adv = parse_entity(p, len, j, &u);
            if (adv > 0) {
                unsigned char cc = uni_to_cp1251(u);
                if (cc) out[o++] = (char)cc;
                j += adv;
                continue;
            }
            out[o++] = c;
            j++;
        }
        out[o] = 0;
        return o > 0;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Разбор HTML в блоки и стилевые точки.
// ---------------------------------------------------------------------------
typedef struct {
    browser_t* s;
    bool utf8;
    int  blk;                          // открытый блок или -1
    int  depth, quote, pre;
    int  bold, code, muted, heading;
    uint16_t link;
    struct { bool ordered; int n; } lists[8];
    bool in_form, form_used;           // 48.2: поле поиска — одно на форму
    int  link_end;                     // позиция текста, где закрылась последняя ссылка (-1 — нет)
    bool main_strong;                  // начало содержимого найдено по точной примете
} parse_t;

static char g_form_action[BR_URL];     // адрес текущей <form> (static: стек ядра мал)

static void p_style(parse_t* ps) {
    browser_t* s = ps->s;
    uint8_t fl = (uint8_t)((ps->bold > 0 ? SF_BOLD : 0) | (ps->code > 0 ? SF_CODE : 0) |
                           (ps->muted > 0 ? SF_MUTED : 0));
    if (s->n_styles > 0) {
        br_style_t* last = &s->styles[s->n_styles - 1];
        if (last->flags == fl && last->link == ps->link) return;
        if (last->pos == (uint32_t)s->text_len) { last->flags = fl; last->link = ps->link; return; }
    } else if (fl == 0 && ps->link == 0) return;
    if (s->n_styles >= BR_STYLE_MAX) return;
    s->styles[s->n_styles++] = (br_style_t){ (uint32_t)s->text_len, ps->link, fl };
}

static void p_close(parse_t* ps) {
    browser_t* s = ps->s;
    if (ps->blk < 0) return;
    br_block_t* b = &s->blocks[ps->blk];
    while ((uint32_t)s->text_len > b->start &&
           (s->text[s->text_len-1] == ' ' || s->text[s->text_len-1] == '\n')) s->text_len--;
    b->end = (uint32_t)s->text_len;
    if (b->end == b->start && b->type != BT_HR) s->n_blocks--;     // пустой блок не нужен
    ps->blk = -1;
}

static void p_open(parse_t* ps, uint8_t type) {
    browser_t* s = ps->s;
    p_close(ps);
    if (s->n_blocks >= BR_BLOCK_MAX) return;
    br_block_t* b = &s->blocks[s->n_blocks];
    b->start = b->end = (uint32_t)s->text_len;
    b->type  = type;
    b->depth = (uint8_t)(ps->depth > 7 ? 7 : ps->depth);
    b->quote = (uint8_t)(ps->quote > 3 ? 3 : ps->quote);
    b->num   = 0;
    ps->blk  = s->n_blocks++;
}

static bool p_ensure(parse_t* ps) {
    if (ps->blk >= 0) return true;
    p_open(ps, ps->pre > 0 ? BT_PRE : ps->heading ? (uint8_t)(BT_H1 + ps->heading - 1) : BT_P);
    return ps->blk >= 0;
}

static void p_char(parse_t* ps, char c) {
    browser_t* s = ps->s;
    if (!p_ensure(ps)) return;
    if (s->text_len >= (int)BR_TEXT_CAP - 1) { s->text_overflow = true; return; }
    s->text[s->text_len++] = c;
}

static void p_space(parse_t* ps) {
    browser_t* s = ps->s;
    if (ps->pre > 0) { p_char(ps, ' '); return; }
    if (ps->blk < 0) return;                                   // блок не начинают пробелом
    if ((uint32_t)s->text_len == s->blocks[ps->blk].start) return;
    char last = s->text[s->text_len - 1];
    if (last == ' ' || last == '\n') return;
    p_char(ps, ' ');
}

static void p_break(parse_t* ps) {
    browser_t* s = ps->s;
    if (ps->blk < 0) return;
    uint32_t st = s->blocks[ps->blk].start;
    while ((uint32_t)s->text_len > st && s->text[s->text_len-1] == ' ') s->text_len--;
    // Не больше одной пустой строки подряд.
    if ((uint32_t)s->text_len >= st + 2 && s->text[s->text_len-1] == '\n' && s->text[s->text_len-2] == '\n') return;
    if ((uint32_t)s->text_len == st) return;
    p_char(ps, '\n');
}

static void p_uni(parse_t* ps, uint32_t u) {
    if (ps->pre > 0) {
        if (u == '\r') return;
        if (u == '\n') { p_char(ps, '\n'); return; }
        if (u == '\t') { for (int i = 0; i < 4; i++) p_char(ps, ' '); return; }
    } else if (u == '\n' || u == '\t' || u == '\r' || u == ' ') { p_space(ps); return; }
    unsigned char c = uni_to_cp1251(u);
    if (c == 0) return;
    if (c == ' ') { p_space(ps); return; }
    p_char(ps, (char)c);
}

static void p_str(parse_t* ps, const char* t) { while (*t) p_char(ps, *t++); }

// Значение атрибута для показа (alt, placeholder): из кодировки страницы в
// CP1251, на месте (результат не длиннее исходного).
static void attr_to_cp1251(const parse_t* ps, char* v) {
    if (!ps->utf8) return;
    int n = b_len(v), o = 0;
    for (int i = 0; i < n; ) {
        uint32_t u = 0;
        i += utf8_decode(v, n, i, &u);
        unsigned char c = (u == '\n' || u == '\r' || u == '\t') ? ' ' : uni_to_cp1251(u);
        if (c) v[o++] = (char)c;
    }
    v[o] = 0;
}

// Добавить ссылку с готовым адресом; возвращает её номер+1 или 0.
static uint16_t add_href(browser_t* s, const char* abs) {
    int al = b_len(abs) + 1;
    if (s->n_links >= BR_LINK_MAX || s->href_used + al > (int)BR_HREF_POOL) return 0;
    s->href_off[s->n_links] = (uint32_t)s->href_used;
    for (int q = 0; q < al; q++) s->hrefs[s->href_used + q] = abs[q];
    s->href_used += al;
    return (uint16_t)(++s->n_links);
}

static void doc_reset(browser_t* s) {
    s->text_len = 0; s->n_styles = 0; s->n_blocks = 0; s->n_lines = 0;
    s->n_links = 0; s->href_used = 0; s->title[0] = 0;
    s->doc_h = 0; s->layout_w = 0; s->text_overflow = false;
    s->links_logged = false;
    s->main_pos = -1; s->refresh[0] = 0;
}

static void html_parse(browser_t* s, const char* html, int len, bool utf8) {
    // <noscript> НЕ пропускаем: скриптов у нас нет, и его содержимое написано
    // как раз для нас (запасные ссылки, перенаправление на версию без JS).
    // <dialog> без open и <button> (без скриптов кнопка — мёртвая подпись) — мимо.
    static const char* const skip_tags[] = { "script", "style", "svg", "iframe", "canvas", "template",
                                             "select", "textarea", "object", "dialog", "button", 0 };
    static const char* const block_tags[] = { "p", "div", "section", "article", "header", "footer",
        "nav", "aside", "main", "form", "fieldset", "address", "figure", "figcaption", "table",
        "thead", "tbody", "tfoot", "tr", "dl", "dt", "dd", "center", "details", "summary",
        "body", "html", "caption", "option", 0 };
    static const char* const bold_tags[] = { "b", "strong", "th", 0 };
    static const char* const code_tags[] = { "code", "kbd", "tt", "samp", 0 };
    // Теги без содержимого: «скрытый» такой тег пропускается сам по себе.
    static const char* const void_tags[] = { "area", "base", "br", "col", "embed", "hr", "img",
                                             "input", "link", "meta", "source", "track", "wbr", 0 };
    static const char* const main_ids[] = { "content", "main", "main-content", "maincontent",
                                            "mw-content-text", "bodycontent", "article", 0 };

    doc_reset(s);
    parse_t ps;
    for (size_t k = 0; k < sizeof(ps); k++) ((uint8_t*)&ps)[k] = 0;
    ps.s = s; ps.utf8 = utf8; ps.blk = -1; ps.link_end = -1;
    const char* base = s->f.final_url[0] ? s->f.final_url : s->url;
    int i = 0;

    while (i < len) {
        char c = html[i];
        if (c != '<') {
            uint32_t u = 0;
            int adv = parse_entity(html, len, i, &u);
            if (adv > 0) { p_uni(&ps, u); i += adv; continue; }
            if ((unsigned char)c >= 0x80) {
                if (utf8) { adv = utf8_decode(html, len, i, &u); p_uni(&ps, u); i += adv; }
                else { p_char(&ps, c); i++; }                   // уже CP1251
                continue;
            }
            p_uni(&ps, (unsigned char)c);
            i++;
            continue;
        }

        // Комментарии, <!doctype>, <![CDATA[...]]>.
        if (i + 1 < len && html[i+1] == '!') {
            if (i + 3 < len && html[i+2] == '-' && html[i+3] == '-') {
                i += 4;
                while (i + 2 < len && !(html[i] == '-' && html[i+1] == '-' && html[i+2] == '>')) i++;
                i = (i + 3 <= len) ? i + 3 : len;
            } else {
                while (i < len && html[i] != '>') i++;
                if (i < len) i++;
            }
            continue;
        }
        int  j = i + 1;
        bool closing = false;
        if (j < len && html[j] == '/') { closing = true; j++; }
        int ns = j;
        while (j < len && ((html[j] >= 'a' && html[j] <= 'z') || (html[j] >= 'A' && html[j] <= 'Z') ||
                           (html[j] >= '0' && html[j] <= '9'))) j++;
        int nlen = j - ns;
        if (nlen == 0) {                                        // «<» как обычный знак (a < b)
            if (j < len && (html[j] == ' ' || (html[j] >= '0' && html[j] <= '9') || html[j] == '=')) {
                p_uni(&ps, '<'); i++; continue;
            }
            while (j < len && html[j] != '>') j++;
            i = (j < len) ? j + 1 : len;
            continue;
        }
        int attr_start = j;
        while (j < len && html[j] != '>') {
            if (html[j] == '"' || html[j] == '\'') {
                char q = html[j++];
                while (j < len && html[j] != q) j++;
            }
            if (j < len) j++;
        }
        int attr_end = j;
        int tag_end  = (j < len) ? j + 1 : len;
        const char* tn = html + ns;

        if (!closing && tag_in(tn, nlen, skip_tags)) { i = skip_element(html, len, tag_end, tn, nlen); continue; }

        // Скрытый элемент — целиком мимо (вместе с вложенными). <html>/<body>
        // не трогаем: «скрытое» тело раскрывает скрипт, которого у нас нет.
        if (!closing && attr_end > attr_start && !tag_is(tn, nlen, "body") && !tag_is(tn, nlen, "html") &&
            tag_hidden(html + attr_start, attr_end - attr_start)) {
            if (tag_in(tn, nlen, void_tags)) { i = tag_end; continue; }
            int after = skip_nested(html, len, tag_end, tn, nlen);
            if (after > 0) { i = after; continue; }
        }

        // Где начинается основное содержимое: <main>, <article>, role="main"
        // или привычные id. Первое найденное место.
        if (!closing && s->main_pos < 0) {
            bool is_main = tag_is(tn, nlen, "main") || tag_is(tn, nlen, "article");
            if (!is_main && attr_end > attr_start &&
                (tag_is(tn, nlen, "div") || tag_is(tn, nlen, "section") || tag_is(tn, nlen, "td"))) {
                static char av[48];
                if (get_attr(html + attr_start, attr_end - attr_start, "role", av, sizeof(av)) && tag_is(av, b_len(av), "main"))
                    is_main = true;
                else if (get_attr(html + attr_start, attr_end - attr_start, "id", av, sizeof(av)) && tag_in(av, b_len(av), main_ids))
                    is_main = true;
            }
            if (is_main) { p_close(&ps); s->main_pos = s->text_len; }
        }
        // MediaWiki (Википедия): внутри <main> перед статьёй лежит список из
        // трёхсот языков (в обычном браузере он свёрнут стилями). Тело статьи
        // — <div id="mw-content-text">: эта примета точнее, она и побеждает.
        if (!closing && !ps.main_strong && nlen == 3 && tag_is(tn, nlen, "div") && attr_end > attr_start) {
            static char mid[32];
            if (get_attr(html + attr_start, attr_end - attr_start, "id", mid, sizeof(mid)) &&
                tag_is(mid, b_len(mid), "mw-content-text")) {
                p_close(&ps);
                s->main_pos = s->text_len;
                ps.main_strong = true;
            }
        }

        // <meta http-equiv="refresh" content="N; url=...">: перенаправление.
        if (!closing && tag_is(tn, nlen, "meta") && !s->refresh[0]) {
            static char he[24], mc[BR_URL];
            if (get_attr(html + attr_start, attr_end - attr_start, "http-equiv", he, sizeof(he)) &&
                tag_is(he, b_len(he), "refresh") &&
                get_attr(html + attr_start, attr_end - attr_start, "content", mc, sizeof(mc))) {
                int k = 0, delay = 0;
                while (mc[k] >= '0' && mc[k] <= '9') { if (delay < 100000) delay = delay * 10 + (mc[k] - '0'); k++; }
                while (mc[k] && !(b_lower(mc[k]) == 'u' && b_lower(mc[k+1]) == 'r' && b_lower(mc[k+2]) == 'l')) k++;
                if (mc[k]) {
                    k += 3;
                    while (mc[k] == ' ' || mc[k] == '=') k++;
                    if (mc[k] == '"' || mc[k] == '\'') k++;
                    int e = b_len(mc);
                    while (e > k && (mc[e-1] == '"' || mc[e-1] == '\'' || mc[e-1] == ' ')) e--;
                    // Долгий таймер — это «обновлять страницу раз в N минут», а не переход.
                    if (e > k && delay <= 15) http_resolve_url(base, mc + k, (size_t)(e - k), s->refresh, sizeof(s->refresh));
                }
            }
        }

        // Форма: помним адрес, первое текстовое поле станет ссылкой-поиском.
        if (tag_is(tn, nlen, "form")) {
            ps.in_form = !closing; ps.form_used = false;
            if (!closing) {
                static char act[BR_URL];
                if (!get_attr(html + attr_start, attr_end - attr_start, "action", act, sizeof(act)) ||
                    !http_resolve_url(base, act, (size_t)b_len(act), g_form_action, sizeof(g_form_action))) {
                    // Без action форма шлёт запрос на адрес самой страницы (без её query).
                    b_copy(g_form_action, base, sizeof(g_form_action));
                    for (int q = 0; g_form_action[q]; q++) if (g_form_action[q] == '?') { g_form_action[q] = 0; break; }
                }
            }
        }
        if (!closing && tag_is(tn, nlen, "input") && ps.in_form && !ps.form_used) {
            static char ty[24], nm[64], lab[80], fh[BR_URL];
            const char* at = html + attr_start; int an = attr_end - attr_start;
            bool texty = !get_attr(at, an, "type", ty, sizeof(ty)) || tag_is(ty, b_len(ty), "text") ||
                         tag_is(ty, b_len(ty), "search");
            if (texty && get_attr(at, an, "name", nm, sizeof(nm))) {
                if (!get_attr(at, an, "placeholder", lab, sizeof(lab)) &&
                    !get_attr(at, an, "aria-label", lab, sizeof(lab)) &&
                    !get_attr(at, an, "title", lab, sizeof(lab))) b_copy(lab, "поиск", sizeof(lab));
                else attr_to_cp1251(&ps, lab);
                b_copy(fh, BR_FORM_PFX, sizeof(fh));
                b_cat(fh, g_form_action, sizeof(fh));
                bool has_q = false;
                for (int q = 0; g_form_action[q]; q++) if (g_form_action[q] == '?') has_q = true;
                b_cat(fh, has_q ? "&" : "?", sizeof(fh));
                b_cat(fh, nm, sizeof(fh));
                b_cat(fh, "=", sizeof(fh));
                uint16_t fl = add_href(s, fh);
                if (fl) {
                    uint16_t keep = ps.link;
                    p_space(&ps);
                    ps.link = fl; p_style(&ps);
                    p_str(&ps, "[ "); p_str(&ps, lab); p_str(&ps, " ]");
                    ps.link = keep; p_style(&ps);
                    p_space(&ps);
                    ps.form_used = true;
                }
            }
        }

        if (!closing && tag_is(tn, nlen, "title")) {
            int k = tag_end, o = 0;
            while (k < len && html[k] != '<' && o < BR_TITLE - 1) {
                uint32_t u = 0;
                int adv = parse_entity(html, len, k, &u);
                if (adv <= 0) {
                    if (utf8) adv = utf8_decode(html, len, k, &u);
                    else { u = (unsigned char)html[k]; adv = 1; }
                }
                unsigned char cc = (u == '\n' || u == '\r' || u == '\t') ? ' '
                                 : (!utf8 && u >= 0x80) ? (unsigned char)u : uni_to_cp1251(u);
                if (cc && !(cc == ' ' && (o == 0 || s->title[o-1] == ' '))) s->title[o++] = (char)cc;
                k += adv;
            }
            while (o > 0 && s->title[o-1] == ' ') o--;
            s->title[o] = 0;
            i = skip_element(html, len, tag_end, tn, nlen);
            continue;
        }

        if (tag_is(tn, nlen, "a")) {
            if (closing) { ps.link = 0; p_style(&ps); ps.link_end = s->text_len; }
            else {
                static char href[BR_URL], abs[BR_URL];
                ps.link = 0;
                // Две ссылки вплотную (<a>Почта</a><a>Облако</a> — отступ между
                // ними делает CSS) без пробела сливаются в одно слово.
                if (ps.link_end == s->text_len && ps.link_end >= 0) { p_style(&ps); p_space(&ps); }
                if (get_attr(html + attr_start, attr_end - attr_start, "href", href, sizeof(href)) &&
                    http_resolve_url(base, href, (size_t)b_len(href), abs, sizeof(abs))) {
                    unwrap_redirect(abs, sizeof(abs));
                    ps.link = add_href(s, abs);
                }
                p_style(&ps);
            }
        }
        else if (tag_in(tn, nlen, bold_tags)) { ps.bold += closing ? -1 : 1; if (ps.bold < 0) ps.bold = 0; p_style(&ps);
                                                if (tag_is(tn, nlen, "th") && !closing && ps.blk >= 0 &&
                                                    (uint32_t)s->text_len > s->blocks[ps.blk].start) { p_space(&ps); p_str(&ps, "| "); } }
        else if (tag_in(tn, nlen, code_tags)) { ps.code += closing ? -1 : 1; if (ps.code < 0) ps.code = 0; p_style(&ps); }
        else if (nlen == 2 && b_lower(tn[0]) == 'h' && tn[1] >= '1' && tn[1] <= '6') {
            p_close(&ps);
            ps.heading = closing ? 0 : (tn[1] <= '3' ? tn[1] - '0' : 3);
        }
        else if (tag_is(tn, nlen, "br")) p_break(&ps);
        else if (tag_is(tn, nlen, "hr")) { if (!closing) { p_open(&ps, BT_HR); p_close(&ps); } }
        else if (tag_is(tn, nlen, "ul") || tag_is(tn, nlen, "ol")) {
            p_close(&ps);
            if (!closing) {
                if (ps.depth < 8) { ps.lists[ps.depth].ordered = (b_lower(tn[0]) == 'o'); ps.lists[ps.depth].n = 0; }
                ps.depth++;
            } else if (ps.depth > 0) ps.depth--;
        }
        else if (tag_is(tn, nlen, "li")) {
            p_close(&ps);
            if (!closing) {
                if (ps.depth == 0) ps.depth = 1;                // <li> без списка
                p_open(&ps, BT_LI);
                int d = ps.depth - 1;
                if (ps.blk >= 0 && d < 8 && ps.lists[d].ordered) s->blocks[ps.blk].num = (uint16_t)(++ps.lists[d].n);
            }
        }
        else if (tag_is(tn, nlen, "pre")) {
            p_close(&ps);
            if (!closing) { ps.pre++; p_open(&ps, BT_PRE); }
            else if (ps.pre > 0) ps.pre--;
        }
        else if (tag_is(tn, nlen, "blockquote")) {
            p_close(&ps);
            if (!closing) ps.quote++; else if (ps.quote > 0) ps.quote--;
        }
        else if (tag_is(tn, nlen, "td")) {
            // Таблиц нет: ячейки строки идут подряд через « | ».
            if (!closing && ps.blk >= 0 && (uint32_t)s->text_len > s->blocks[ps.blk].start) { p_space(&ps); p_str(&ps, "| "); }
        }
        else if (tag_is(tn, nlen, "img")) {
            static char alt[96];
            if (!closing && get_attr(html + attr_start, attr_end - attr_start, "alt", alt, sizeof(alt)) && alt[0]) {
                attr_to_cp1251(&ps, alt);
                ps.muted++; p_style(&ps);
                p_char(&ps, '[');
                for (int q = 0; alt[q]; q++) p_char(&ps, alt[q] == '\n' ? ' ' : alt[q]);
                p_char(&ps, ']');
                ps.muted--; p_style(&ps);
                p_space(&ps);
            }
        }
        else if (tag_in(tn, nlen, block_tags)) p_close(&ps);
        // остальное (span, i, em, font, ...) — оформление не меняет

        i = tag_end;
    }
    p_close(&ps);
    s->text[s->text_len] = 0;
}

// Простой текст и просмотр исходного кода — один блок моноширинным шрифтом.
static void plain_parse(browser_t* s, const char* src, int len, bool utf8) {
    doc_reset(s);
    parse_t ps;
    for (size_t k = 0; k < sizeof(ps); k++) ((uint8_t*)&ps)[k] = 0;
    ps.s = s; ps.utf8 = utf8; ps.blk = -1; ps.pre = 1;
    int i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x80 && utf8) { uint32_t u; i += utf8_decode(src, len, i, &u); p_uni(&ps, u); continue; }
        if (c >= 0x80) { p_char(&ps, (char)c); i++; continue; }
        if (c == 0) { i++; continue; }
        p_uni(&ps, c);
        i++;
    }
    p_close(&ps);
    s->text[s->text_len] = 0;
}

// Кодировка: charset из Content-Type, иначе <meta charset> в начале документа.
static bool sniff_utf8(const browser_t* s) {
    const char* ct = s->f.content_type;
    for (int i = 0; ct[i]; i++) {
        if (b_lower(ct[i]) == 'c' && b_starts(ct + i + 1, "harset=")) {
            const char* v = ct + i + 8;
            if (b_lower(v[0]) == 'u') return true;
            return false;                                        // windows-1251 и прочее
        }
    }
    const char* p = s->raw;
    int lim = s->raw_len < 4096 ? s->raw_len : 4096;
    for (int i = 0; i + 7 < lim; i++) {
        if (b_lower(p[i]) != 'c' || b_lower(p[i+1]) != 'h' || b_lower(p[i+2]) != 'a' ||
            b_lower(p[i+3]) != 'r' || b_lower(p[i+4]) != 's' || b_lower(p[i+5]) != 'e' || b_lower(p[i+6]) != 't') continue;
        for (int j = i + 7; j < lim && j < i + 40; j++) {
            if (p[j] == '1' && j + 3 < lim && p[j+1] == '2' && p[j+2] == '5' && p[j+3] == '1') return false;
            if (b_lower(p[j]) == 'k' && j + 2 < lim && b_lower(p[j+1]) == 'o' && b_lower(p[j+2]) == 'i') return false;
            if (b_lower(p[j]) == 'u' && j + 2 < lim && b_lower(p[j+1]) == 't' && b_lower(p[j+2]) == 'f') return true;
        }
    }
    return true;                                                 // сегодня UTF-8 — разумное умолчание
}

static bool sniff_html(const browser_t* s) {
    const char* ct = s->f.content_type;
    for (int i = 0; ct[i]; i++) if (b_lower(ct[i]) == 'h' && b_starts(ct + i + 1, "tml")) return true;
    if (b_starts(ct, "text/plain") || b_starts(ct, "application/json")) return false;
    const char* p = s->raw;
    int lim = s->raw_len < 2048 ? s->raw_len : 2048;
    static const char* const marks[] = { "html", "!doctype", "head", "body", "div", "p>", "meta", "title", "a ", "h1", 0 };
    for (int i = 0; i + 1 < lim; i++) {
        if (p[i] != '<') continue;
        for (int k = 0; marks[k]; k++) {
            int n = b_len(marks[k]);
            if (lim - i - 1 < n) continue;
            bool m = true;
            for (int q = 0; q < n; q++) if (b_lower(p[i+1+q]) != marks[k][q]) { m = false; break; }
            if (m) return true;
        }
    }
    return false;
}

// Умеем ли показать содержимое этого типа.
static bool content_supported(const browser_t* s) {
    const char* ct = s->f.content_type;
    if (!ct[0]) return true;
    if (b_starts(ct, "text/")) return true;
    for (int i = 0; ct[i]; i++) {
        if (b_starts(ct + i, "html") || b_starts(ct + i, "xml") || b_starts(ct + i, "json") ||
            b_starts(ct + i, "javascript")) return true;
    }
    // Тип назван «никаким» (application/octet-stream) или неверно — смотрим
    // на само тело: без нулевых байт и с HTML-разметкой в начале — страница.
    if (s->raw_len == 0) return true;                      // пустой ответ — пустая страница
    int lim = s->raw_len < 1024 ? s->raw_len : 1024;
    for (int i = 0; i < lim; i++) if (s->raw[i] == 0) return false;
    return sniff_html(s);
}

static void doc_build(browser_t* s) {
    bool utf8 = sniff_utf8(s);
    if (s->show_source)      plain_parse(s, s->raw, s->raw_len, utf8);
    else if (sniff_html(s))  html_parse(s, s->raw, s->raw_len, utf8);
    else                     plain_parse(s, s->raw, s->raw_len, utf8);
}

// ---------------------------------------------------------------------------
// Вёрстка: блоки → строки.
// ---------------------------------------------------------------------------
static ui_font_id_t block_font(uint8_t type) {
    switch (type) {
        case BT_H1:  return UI_FONT_28;
        case BT_H2:  return UI_FONT_20;
        case BT_PRE: return UI_FONT_MONO_14;
        default:     return UI_FONT_16;
    }
}

// Ширина отрезка текста [a, b) шрифтом f.
static int span_width(const browser_t* s, uint32_t a, uint32_t b, ui_font_id_t f) {
    static char buf[512];
    int w = 0;
    while (a < b) {
        uint32_t n = b - a;
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        for (uint32_t i = 0; i < n; i++) buf[i] = s->text[a + i];
        buf[n] = 0;
        w += ui_font_text_width(buf, f);
        a += n;
    }
    return w;
}

static void add_line(browser_t* s, const br_block_t* b, uint32_t a, uint32_t e, int x, int* y,
                     int lh, bool first, uint32_t* sidx) {
    if (s->n_lines >= BR_LINE_MAX) return;
    while (*sidx + 1 < (uint32_t)s->n_styles && s->styles[*sidx + 1].pos <= a) (*sidx)++;
    br_line_t* ln = &s->lines[s->n_lines++];
    ln->start = a; ln->end = e; ln->sidx = *sidx;
    ln->y = *y; ln->x = (uint16_t)x; ln->h = (uint16_t)lh;
    ln->num = b->num; ln->type = b->type; ln->first = first ? 1 : 0;
    ln->quote = b->quote; ln->font = (uint8_t)block_font(b->type);
    *y += lh;
}

static void doc_layout(browser_t* s, int col_w) {
    s->n_lines = 0;
    s->layout_w = col_w;
    int y = 28;
    uint32_t sidx = 0;
    for (int bi = 0; bi < s->n_blocks; bi++) {
        const br_block_t* b = &s->blocks[bi];
        ui_font_id_t f = block_font(b->type);
        int flh = ui_font_line_height(f);
        int lh, mt = 0, mb;
        switch (b->type) {
            case BT_H1:  lh = flh + 8; mt = 18; mb = 12; break;
            case BT_H2:  lh = flh + 6; mt = 16; mb = 8;  break;
            case BT_H3:  lh = flh + 6; mt = 12; mb = 6;  break;
            case BT_LI:  lh = flh + 6; mb = 5;  break;
            case BT_PRE: lh = flh + 3; mt = 4;  mb = 14; break;
            case BT_HR:  lh = 1;       mt = 10; mb = 18; break;
            default:     lh = flh + 7; mb = 14; break;
        }
        int indent = b->depth * 28 + b->quote * 24;
        if (indent > col_w / 2) indent = col_w / 2;
        int maxw = col_w - indent;
        if (bi > 0) y += mt;

        if (b->type == BT_HR) { add_line(s, b, b->start, b->start, indent, &y, lh, true, &sidx); y += mb; continue; }

        if (b->type == BT_PRE) {
            // Моноширинный блок: пробелы и переводы строк как в источнике;
            // длинная строка переносится по символам.
            int adv = ui_font_text_width("0", f);
            if (adv < 1) adv = 8;
            int per = (maxw - 24) / adv;
            if (per < 8) per = 8;
            uint32_t p = b->start;
            bool first = true;
            while (p < b->end) {
                uint32_t e = p;
                while (e < b->end && s->text[e] != '\n' && (int)(e - p) < per) e++;
                add_line(s, b, p, e, indent, &y, lh, first, &sidx);
                first = false;
                p = (e < b->end && s->text[e] == '\n') ? e + 1 : e;
            }
            y += mb;
            continue;
        }

        int space_w = ui_font_text_width(" ", f);
        uint32_t p = b->start;
        bool first = true;
        while (p < b->end) {
            uint32_t line_start = p, i = p, line_end = p;
            int x = 0;
            bool done = false;
            while (i < b->end && !done) {
                if (s->text[i] == '\n') { line_end = i; p = i + 1; done = true; break; }
                uint32_t j = i;
                while (j < b->end && s->text[j] != ' ' && s->text[j] != '\n') j++;
                int ww = span_width(s, i, j, f);
                if (b->type != BT_P && b->type != BT_LI) ww += (int)(j - i) / 8 + 1;   // жирный чуть шире
                if (x + ww > maxw && x > 0) { line_end = i; p = i; done = true; break; }
                if (ww > maxw) {
                    // Слово длиннее строки (адрес, хеш): режем по символам.
                    uint32_t k = i + 1;
                    while (k < j && span_width(s, i, k + 1, f) <= maxw) k++;
                    line_end = k; p = k; done = true; break;
                }
                x += ww;
                i = j;
                line_end = i;
                while (i < b->end && s->text[i] == ' ') { x += space_w; i++; }
            }
            if (!done) p = b->end;
            // Хвостовые пробелы строке не нужны.
            while (line_end > line_start && s->text[line_end - 1] == ' ') line_end--;
            add_line(s, b, line_start, line_end, indent, &y, lh, first, &sidx);
            first = false;
            while (p < b->end && s->text[p] == ' ') p++;
        }
        y += mb;
    }
    s->doc_h = y + 48;
}

// Пройти по стилевым отрезкам строки: нарисовать (t != 0) и/или найти ссылку
// под точкой hit_x. ox — левый край колонки, top — верх строки на экране.
static uint16_t line_walk(const browser_t* s, const br_line_t* ln, const gfx_target_t* t,
                          int ox, int top, int hit_x, int* link_x) {
    const ui_runtime_theme_t* th = ui_theme_current();
    static char buf[512];
    uint16_t hit = 0;
    int x = ox + ln->x;
    uint32_t si = ln->sidx, p = ln->start;
    bool heading = (ln->type == BT_H1 || ln->type == BT_H2 || ln->type == BT_H3);
    while (p < ln->end) {
        while (si + 1 < (uint32_t)s->n_styles && s->styles[si + 1].pos <= p) si++;
        bool active = s->n_styles > 0 && s->styles[si].pos <= p;
        // Отрезок тянется до следующей стилевой точки. Пока не дошли даже до
        // первой (текст до первой ссылки на странице), «следующая» — она сама.
        uint32_t e = ln->end;
        uint32_t nx = active ? si + 1 : si;
        if (nx < (uint32_t)s->n_styles && s->styles[nx].pos < e) e = s->styles[nx].pos;
        if (e - p > sizeof(buf) - 1) e = p + sizeof(buf) - 1;
        uint8_t fl = active ? s->styles[si].flags : 0;
        uint16_t link = active ? s->styles[si].link : 0;
        for (uint32_t i = 0; i < e - p; i++) buf[i] = s->text[p + i];
        buf[e - p] = 0;

        ui_font_id_t f = (ui_font_id_t)ln->font;
        if ((fl & SF_CODE) && ln->type != BT_PRE && !heading) f = UI_FONT_MONO_14;
        bool bold = heading || (fl & SF_BOLD);
        int w = ui_font_text_width(buf, f);
        if (t) {
            uint32_t col = link ? th->accent_berry : (fl & SF_MUTED) ? th->muted : th->ink;
            int base = top + (ln->h - ui_font_line_height((ui_font_id_t)ln->font)) / 2 + ui_font_ascent((ui_font_id_t)ln->font);
            if ((fl & SF_CODE) && f == UI_FONT_MONO_14 && ln->type != BT_PRE)
                gfx_fill_round_rect(t, x - 2, top + 2, w + 4, ln->h - 4, 4, th->surface_soft);
            ui_font_draw_baseline_ex(t, x, base, buf, col, f, 0, bold);
            if (link) gfx_fill_rect(t, x, base + 2, w, 1, col);
        }
        if (link) {
            if (link_x && *link_x < 0) *link_x = x + (w > 16 ? 8 : w / 2);
            if (hit_x >= x && hit_x < x + w) hit = link;
        }
        x += w;
        p = e;
    }
    return hit;
}

// Первая строка, нижний край которой ниже y (двоичный поиск: строки по y растут).
static int line_at(const browser_t* s, int y) {
    int lo = 0, hi = s->n_lines;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (s->lines[mid].y + s->lines[mid].h <= y) lo = mid + 1; else hi = mid;
    }
    return lo;
}

// ---------------------------------------------------------------------------
// Раскладка окна.
// ---------------------------------------------------------------------------
static void br_layout(const window_t* w, br_layout_t* L) {
    int pad = 28, W = w->width, H = w->height;
    for (int i = 0; i < TB_COUNT; i++) L->tb[i] = (pk_rect_t){ pad + i * 52, 16, 44, 44 };
    int ax = pad + TB_COUNT * 52 + 8;
    L->src  = (pk_rect_t){ W - pad - 76, 16, 76, 44 };
    L->addr = (pk_rect_t){ ax, 16, L->src.x - 12 - ax, 44 };
    L->bar  = (pk_rect_t){ pad, 68, W - 2 * pad, 3 };
    L->card = (pk_rect_t){ pad, 82, W - 2 * pad, H - 82 - 46 };
    L->vp   = (pk_rect_t){ L->card.x + 1, L->card.y + 1, L->card.w - 2, L->card.h - 2 };
    L->track = (pk_rect_t){ L->vp.w - 14, 10, 8, L->vp.h - 20 };        // в координатах vp
    L->status = (pk_rect_t){ pad, H - 34, W - 2 * pad, 20 };
    int avail = L->vp.w - 2 * 40 - 20;
    L->col_w = avail > 980 ? 980 : avail;
    L->col_x = (L->vp.w - 20 - L->col_w) / 2;
}

static int max_scroll(const browser_t* s, const br_layout_t* L) {
    int m = s->doc_h - L->vp.h;
    return m > 0 ? m : 0;
}
static void clamp_scroll(browser_t* s, const br_layout_t* L) {
    int m = max_scroll(s, L);
    if (s->scroll > m) s->scroll = m;
    if (s->scroll < 0) s->scroll = 0;
}

// ---------------------------------------------------------------------------
// Избранное на стартовой странице.
// ---------------------------------------------------------------------------
typedef struct { const char* title; const char* note; const char* url; char letter; } br_fav_t;
static const br_fav_t k_favs[] = {
    { "Example",        "example.com",           "https://example.com/",          'E' },
    { "Первый сайт",    "info.cern.ch",          "http://info.cern.ch/",          'W' },
    { "Википедия",      "ru.wikipedia.org",      "https://ru.wikipedia.org/",     'W' },
    { "Hacker News",    "news.ycombinator.com",  "https://news.ycombinator.com/", 'Y' },
    { "NPR \xB7 текст",    "text.npr.org",          "https://text.npr.org/",         'N' },
    { "CNN Lite",       "lite.cnn.com",          "https://lite.cnn.com/",         'C' },
    { "Поиск",          "DuckDuckGo без скриптов", "https://html.duckduckgo.com/html/", 'D' },
    { "Настройки сети", "приложение Mesh",       "amplify://mesh",                'M' },
};
#define BR_FAVS ((int)(sizeof(k_favs) / sizeof(k_favs[0])))

static pk_rect_t fav_rect(const br_layout_t* L, int i) {
    int cols = L->col_w >= 760 ? 4 : 2;
    int gap = 16;
    int tw = (L->col_w - (cols - 1) * gap) / cols;
    return (pk_rect_t){ L->col_x + (i % cols) * (tw + gap), 250 + (i / cols) * (96 + gap), tw, 96 };
}

// Кнопки страницы ошибки (координаты vp).
static pk_rect_t err_btn(const br_layout_t* L, int i) {
    return (pk_rect_t){ L->col_x + i * 196, 300, 180, 42 };
}

// ---------------------------------------------------------------------------
// Навигация и загрузка.
// ---------------------------------------------------------------------------
static void set_edit(browser_t* s, const char* url) {
    b_copy(s->edit, b_eq(url, BR_START_URL) ? "" : url, BR_URL);
    s->edit_len = b_len(s->edit);
}

// Кадры, нарисованные за время загрузки: по ним тест видит, что интерфейс
// не замирал.
static uint32_t g_pump_frames = 0;
static void br_pump(void) { g_pump_frames++; wm_pump(); }

// jump — новая страница (не «назад» и не «обновить»): открыть с основного
// содержимого, если оно начинается не на первом экране.
static void br_load_once(browser_t* s, const char* url, int restore_scroll, bool jump) {
    b_copy(s->url, url, BR_URL);
    set_edit(s, url);
    s->addr_focus = false;
    s->drag_scroll = false;
    s->form_prefix[0] = 0;
    s->refresh[0] = 0;
    s->jump_main = false;

    if (b_eq(url, BR_START_URL)) {
        s->view = VIEW_START;
        s->scroll = 0;
        s->addr_focus = true; s->edit_fresh = false;
        serial_puts("[browser] start page\n");
        return;
    }

    serial_puts("[browser] navigate "); serial_puts(url); serial_puts("\n");
    s->loading = true;
    s->load_t0 = timer_get_uptime_ms();
    net_progress_set(NET_STAGE_DNS, 0, -1);
    // Прежняя страница остаётся на экране, пока грузится новая (как в любом
    // браузере), но прежняя СТРАНИЦА ОШИБКИ — нет: её текст относился к
    // другому адресу («Такого сайта нет» под уже новым адресом сбивает с толку).
    if (s->view == VIEW_ERROR) s->view = VIEW_LOADING;
    // Кадр «загрузка началась» — сразу, до первого сетевого ожидания; дальше
    // кадры идут из net_idle.
    g_pump_frames = 0;
    br_pump();
    net_set_idle_hook(br_pump);
    s->f.body = s->raw; s->f.body_cap = BR_RAW_CAP + 1;
    bool ok = net_fetch_ex(url, &s->f, 8, false);
    net_set_idle_hook(0);
    net_cancel_clear();
    s->loading = false;
    s->load_ms = (uint32_t)(timer_get_uptime_ms() - s->load_t0);
    s->layout_w = 0;

    if (!ok) {
        s->view = VIEW_ERROR;
        s->err = s->f.error;
        s->dns_fail = dns_last_failure();
        s->scroll = 0;
        serial_puts("[browser] error "); serial_puts(net_error_name(s->err));
        serial_puts(" ms="); serial_print_dec(s->load_ms);
        serial_puts(" frames="); serial_print_dec(g_pump_frames);
        serial_puts(" url="); serial_puts(url); serial_puts("\n");
        return;
    }

    s->raw_len = (int)s->f.body_len;
    s->raw[s->raw_len] = 0;
    // Адрес после перенаправлений — в строку и в историю.
    if (s->f.final_url[0]) {
        b_copy(s->url, s->f.final_url, BR_URL);
        set_edit(s, s->url);
        if (s->hist_pos >= 0) b_copy(s->hist[s->hist_pos], s->url, BR_URL);
    }
    s->show_source = false;
    if (!content_supported(s)) {
        doc_reset(s);
        s->view = VIEW_ERROR;
        s->err = NET_ERR_PROTOCOL;
        s->f.status = -1;                       // признак «тип не поддерживается»
        serial_puts("[browser] unsupported content type "); serial_puts(s->f.content_type); serial_puts("\n");
        return;
    }
    doc_build(s);
    s->view = VIEW_PAGE;
    s->scroll = restore_scroll;
    s->jump_main = jump && s->main_pos > 0 && !s->show_source;

    uint32_t h = 2166136261u;                    // FNV-1a тела: тест сверяет с файлом на сервере
    for (int i = 0; i < s->raw_len; i++) { h ^= (uint8_t)s->raw[i]; h *= 16777619u; }
    serial_puts("[browser] page http="); serial_print_dec((uint64_t)s->f.status);
    serial_puts(" bytes=");     serial_print_dec((uint64_t)s->raw_len);
    serial_puts(" fnv=");       serial_hex32(h);
    serial_puts(" links=");     serial_print_dec((uint64_t)s->n_links);
    serial_puts(" blocks=");    serial_print_dec((uint64_t)s->n_blocks);
    serial_puts(" truncated="); serial_print_dec(s->f.truncated ? 1 : 0);
    serial_puts(" complete=");  serial_print_dec(s->f.complete ? 1 : 0);
    serial_puts(" ms=");        serial_print_dec(s->load_ms);
    serial_puts(" frames=");    serial_print_dec(g_pump_frames);
    serial_puts("\n");
}

// Загрузка с выполнением <meta http-equiv="refresh">: страница-прокладка
// («сейчас вы будете перенаправлены») сама ведёт дальше, но не без конца.
static void br_load(browser_t* s, const char* url, int restore_scroll, bool jump) {
    br_load_once(s, url, restore_scroll, jump);
    for (int hop = 0; hop < BR_REFRESH_MAX; hop++) {
        if (s->view != VIEW_PAGE || !s->refresh[0] || b_eq(s->refresh, s->url)) break;
        static char next[BR_URL];
        b_copy(next, s->refresh, BR_URL);
        serial_puts("[browser] meta refresh -> "); serial_puts(next); serial_puts("\n");
        if (s->hist_pos >= 0) b_copy(s->hist[s->hist_pos], next, BR_URL);
        br_load_once(s, next, 0, jump);
    }
}

// Похоже ли набранное на адрес (иначе это поисковый запрос).
static bool looks_like_url(const char* t) {
    if (b_starts(t, "http://") || b_starts(t, "https://") || b_starts(t, "amplify://")) return true;
    bool dot = false;
    for (int i = 0; t[i]; i++) {
        if (t[i] == ' ') return false;
        if (t[i] == '.' || t[i] == ':' || t[i] == '/') dot = true;
    }
    return dot || b_starts(t, "localhost");
}

static void hist_save_scroll(browser_t* s) {
    if (s->hist_pos >= 0 && s->hist_pos < s->hist_n) s->hist_scroll[s->hist_pos] = s->scroll;
}

static void br_navigate(browser_t* s, const char* url) {
    if (!url || !url[0]) return;
    if (b_eq(url, "amplify://mesh")) { (void)app_mesh_open(); return; }
    static char target[BR_URL];
    b_copy(target, url, BR_URL);                 // url может указывать в s->edit
    hist_save_scroll(s);
    s->hist_n = s->hist_pos + 1;                 // «вперёд» после нового перехода пропадает
    if (s->hist_n >= BR_HIST) {
        for (int i = 1; i < BR_HIST; i++) { b_copy(s->hist[i-1], s->hist[i], BR_URL); s->hist_scroll[i-1] = s->hist_scroll[i]; }
        s->hist_n = BR_HIST - 1;
    }
    b_copy(s->hist[s->hist_n], target, BR_URL);
    s->hist_scroll[s->hist_n] = 0;
    s->hist_pos = s->hist_n++;
    br_load(s, target, 0, true);
}

// Enter в адресной строке: запрос формы, адрес или поиск.
static void br_go_typed(browser_t* s) {
    static char url[BR_URL];
    if (s->form_prefix[0]) {
        b_copy(url, s->form_prefix, BR_URL);
        url_escape_cat(url, s->edit, BR_URL);
        serial_puts("[browser] form query -> "); serial_puts(url); serial_puts("\n");
    } else {
        if (!s->edit[0]) return;
        // Пробелы по краям — не часть адреса.
        const char* t = s->edit;
        while (*t == ' ') t++;
        static char trimmed[BR_URL];
        b_copy(trimmed, t, BR_URL);
        int n = b_len(trimmed);
        while (n > 0 && trimmed[n-1] == ' ') trimmed[--n] = 0;
        if (!n) return;
        if (looks_like_url(trimmed)) b_copy(url, trimmed, BR_URL);
        else {
            b_copy(url, BR_SEARCH_URL, BR_URL);
            url_escape_cat(url, trimmed, BR_URL);
            serial_puts("[browser] search -> "); serial_puts(url); serial_puts("\n");
        }
    }
    br_navigate(s, url);
}

// Щелчок по ссылке: обычная — переход, «поле поиска» — набор запроса в
// адресной строке (своих полей ввода на странице у браузера нет).
static void br_follow(browser_t* s, const char* href) {
    if (b_starts(href, BR_FORM_PFX)) {
        b_copy(s->form_prefix, href + b_len(BR_FORM_PFX), BR_URL);
        s->edit[0] = 0; s->edit_len = 0;
        s->addr_focus = true; s->edit_fresh = false;
        serial_puts("[browser] form field -> "); serial_puts(s->form_prefix); serial_puts("\n");
        return;
    }
    br_navigate(s, href);
}

static bool can_back(const browser_t* s)    { return s->hist_pos > 0; }
static bool can_forward(const browser_t* s) { return s->hist_pos + 1 < s->hist_n; }

static void br_step(browser_t* s, int dir) {
    if (dir < 0 ? !can_back(s) : !can_forward(s)) return;
    hist_save_scroll(s);
    s->hist_pos += dir;
    serial_puts(dir < 0 ? "[browser] back -> " : "[browser] forward -> ");
    serial_puts(s->hist[s->hist_pos]); serial_puts("\n");
    static char target[BR_URL];
    b_copy(target, s->hist[s->hist_pos], BR_URL);
    br_load(s, target, s->hist_scroll[s->hist_pos], false);
}

static void br_reload(browser_t* s) {
    static char target[BR_URL];
    b_copy(target, s->url, BR_URL);
    br_load(s, target, s->view == VIEW_PAGE ? s->scroll : 0, false);
}

static void br_home(browser_t* s) { br_navigate(s, BR_START_URL); }

static void br_toggle_source(browser_t* s) {
    if (s->view != VIEW_PAGE) return;
    s->show_source = !s->show_source;
    doc_build(s);
    s->layout_w = 0;
    s->scroll = 0;
    serial_puts(s->show_source ? "[browser] view source\n" : "[browser] view page\n");
}

// ---------------------------------------------------------------------------
// Отрисовка.
// ---------------------------------------------------------------------------
static void icon_rect(const gfx_target_t* t, int x, int y, int w, int h, uint32_t c) { gfx_fill_rect(t, x, y, w, h, c); }

static void draw_tb_icon(const gfx_target_t* t, pk_rect_t r, int which, bool stop, uint32_t c, uint32_t bg) {
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (which == TB_BACK) {
        for (int i = 0; i <= 7; i++) { icon_rect(t, cx + 3 - i, cy - 7 + i, 3, 2, c); icon_rect(t, cx + 3 - i, cy + 6 - i, 3, 2, c); }
    } else if (which == TB_FWD) {
        for (int i = 0; i <= 7; i++) { icon_rect(t, cx - 5 + i, cy - 7 + i, 3, 2, c); icon_rect(t, cx - 5 + i, cy + 6 - i, 3, 2, c); }
    } else if (which == TB_RELOAD && stop) {
        for (int i = -6; i <= 6; i++) { icon_rect(t, cx + i - 1, cy + i, 3, 2, c); icon_rect(t, cx - i - 1, cy + i, 3, 2, c); }
    } else if (which == TB_RELOAD) {
        // Кольцо с разрывом справа вверху и стрелкой.
        for (int dy = -9; dy <= 9; dy++)
            for (int dx = -9; dx <= 9; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 >= 44 && d2 <= 76 && !(dx > 1 && dy < -1)) gfx_put_pixel(t, cx + dx, cy + dy, c);
            }
        for (int i = 0; i < 6; i++) icon_rect(t, cx + 3 + i, cy - 10 + i, 6 - i, 1, c);
    } else if (which == TB_HOME) {
        for (int i = 0; i <= 8; i++) icon_rect(t, cx - i, cy - 9 + i, 2 * i + 1, 1, c);
        icon_rect(t, cx - 6, cy, 13, 8, c);
        icon_rect(t, cx - 2, cy + 3, 4, 5, bg);
    }
}

static void draw_toolbar(window_t* w, const gfx_target_t* t, browser_t* s, const br_layout_t* L) {
    const ui_runtime_theme_t* th = ui_theme_current();
    for (int i = 0; i < TB_COUNT; i++) {
        bool dis = (i == TB_BACK && !can_back(s)) || (i == TB_FWD && !can_forward(s));
        if (s->loading) dis = (i != TB_RELOAD);
        pk_state_t st = pk_pointer_state(w, L->tb[i], dis);
        uint32_t bg = st == PK_HOVER ? th->surface : st == PK_ACTIVE ? th->border : th->surface_soft;
        gfx_fill_round_rect(t, L->tb[i].x, L->tb[i].y, L->tb[i].w, L->tb[i].h, 12, th->border);
        gfx_fill_round_rect(t, L->tb[i].x + 1, L->tb[i].y + 1, L->tb[i].w - 2, L->tb[i].h - 2, 11, bg);
        draw_tb_icon(t, L->tb[i], i, s->loading, dis ? th->border : th->ink, bg);
    }

    // Адресная строка: слева — индикатор соединения.
    const char* chip = 0; uint32_t chip_col = th->muted;
    bool form_mode = s->form_prefix[0] && s->addr_focus;
    if (form_mode) { chip = "ЗАПРОС"; chip_col = th->accent_berry; }
    else if (s->view == VIEW_PAGE && !s->loading) {
        if (s->f.https && s->f.cert_trusted) { chip = "HTTPS";  chip_col = 0xFF2F8A5B; }
        else if (s->f.https)                 { chip = "HTTPS ?"; chip_col = th->accent_orange; }
        else                                 { chip = "HTTP";   chip_col = th->muted; }
    }
    pk_rect_t a = L->addr;
    uint32_t border = s->addr_focus ? th->accent_coral : th->border;
    int bw = s->addr_focus ? 2 : 1;
    gfx_fill_round_rect(t, a.x, a.y, a.w, a.h, 12, border);
    gfx_fill_round_rect(t, a.x + bw, a.y + bw, a.w - 2 * bw, a.h - 2 * bw, 12 - bw, th->surface);
    int tx = a.x + 16;
    if (chip) {
        int cw = ui_font_text_width(chip, UI_FONT_12) + 30;
        gfx_fill_round_rect(t, a.x + 8, a.y + 9, cw, 26, 13, th->surface_soft);
        int lx = a.x + 17, ly = a.y + 16;
        if (form_mode) {
            // Лупа: кольцо и ручка.
            gfx_fill_round_rect(t, lx - 1, ly + 1, 9, 9, 4, chip_col);
            gfx_fill_round_rect(t, lx + 1, ly + 3, 5, 5, 2, th->surface_soft);
            gfx_fill_rect(t, lx + 7, ly + 9, 2, 2, chip_col);
            gfx_fill_rect(t, lx + 8, ly + 10, 2, 2, chip_col);
        } else if (s->f.https) {
            // Замок: дужка и корпус.
            gfx_fill_rect(t, lx + 1, ly, 6, 2, chip_col);
            gfx_fill_rect(t, lx + 1, ly, 2, 5, chip_col);
            gfx_fill_rect(t, lx + 5, ly, 2, 5, chip_col);
            gfx_fill_round_rect(t, lx - 1, ly + 5, 10, 8, 2, chip_col);
        } else {
            // Без шифрования замка нет — пустой кружок.
            gfx_fill_round_rect(t, lx - 1, ly + 2, 10, 10, 5, chip_col);
            gfx_fill_round_rect(t, lx + 1, ly + 4, 6, 6, 3, th->surface_soft);
        }
        ui_font_draw_baseline_ex(t, a.x + 30, a.y + 9 + (26 - ui_font_line_height(UI_FONT_12)) / 2 + ui_font_ascent(UI_FONT_12),
                                 chip, chip_col, UI_FONT_12, 0, true);
        tx = a.x + 8 + cw + 10;
    }
    ui_font_id_t f = UI_FONT_16;
    int ty = a.y + (a.h - ui_font_line_height(f)) / 2;
    int max_w = a.x + a.w - 16 - tx;
    const char* shown = s->edit;
    // Пока строка не в фокусе, %D0%9B... показываем буквами (в фокусе — как
    // есть: правится то, что уйдёт серверу).
    static char pretty[BR_URL];
    if (!s->addr_focus && s->edit[0]) {
        static char raw[BR_URL];
        int rn = url_unescape(s->edit, s->edit_len, raw, sizeof(raw), false);
        int o = 0;
        for (int i = 0; i < rn && o < BR_URL - 1; ) {
            uint32_t u = 0;
            i += utf8_decode(raw, rn, i, &u);
            unsigned char c = (u < 0x20) ? '?' : uni_to_cp1251(u);
            if (c) pretty[o++] = (char)c;
        }
        pretty[o] = 0;
        shown = pretty;
    }
    while (*shown && ui_font_text_width(shown, f) > max_w) shown++;      // виден хвост
    int tw = ui_font_text_width(shown, f);
    if (!s->edit[0] && form_mode) {
        pk_text(t, tx, ty, "Что найти? Введите запрос и нажмите Enter", th->muted, f);
    } else if (!s->edit[0] && !s->addr_focus) {
        pk_text(t, tx, ty, "Введите адрес или запрос для поиска", th->muted, f);
    } else {
        // Свежий фокус: адрес «выделен» — первая клавиша заменит его целиком.
        if (s->addr_focus && s->edit_fresh && tw > 0)
            gfx_fill_round_rect(t, tx - 3, a.y + 9, tw + 6, a.h - 18, 4, th->surface_soft);
        pk_text(t, tx, ty, shown, th->ink, f);
    }
    if (s->addr_focus && ((timer_get_uptime_ms() / 500) & 1) == 0)
        gfx_fill_rect(t, tx + tw + 1, a.y + 11, 2, a.h - 22, th->accent_coral);

    pk_state_t ss = pk_pointer_state(w, L->src, s->view != VIEW_PAGE || s->loading);
    if (s->show_source && ss != PK_DISABLED) ss = PK_ACTIVE;
    pk_button(t, L->src, "HTML", false, ss);

    // Полоса загрузки: известна длина — доля, иначе бегущий отрезок.
    if (s->loading) {
        gfx_fill_rect(t, L->bar.x, L->bar.y, L->bar.w, L->bar.h, th->border);
        int64_t total = net_progress_total();
        uint64_t done = net_progress_done();
        if (net_progress_stage() == NET_STAGE_BODY && total > 0) {
            int fw = (int)((uint64_t)L->bar.w * (done > (uint64_t)total ? (uint64_t)total : done) / (uint64_t)total);
            gfx_fill_rect_gradient(t, L->bar.x, L->bar.y, fw, L->bar.h, th->accent_orange, th->accent_berry);
        } else {
            int seg = L->bar.w / 5;
            int pos = (int)((timer_get_uptime_ms() / 2) % (uint64_t)(L->bar.w + seg)) - seg;
            int x0 = pos < 0 ? 0 : pos, x1 = pos + seg > L->bar.w ? L->bar.w : pos + seg;
            if (x1 > x0) gfx_fill_rect_gradient(t, L->bar.x + x0, L->bar.y, x1 - x0, L->bar.h, th->accent_orange, th->accent_berry);
        }
    }
}

static void draw_start(window_t* w, const gfx_target_t* vp, const br_layout_t* L) {
    const ui_runtime_theme_t* th = ui_theme_current();
    const ui_bitmap_t* logo = ui_logo_big();
    if (logo) ui_draw_bitmap_alpha(vp, vp->width - 420, vp->height - 420, 520, 520, logo, 20);
    int x = L->col_x;
    pk_label(vp, x, 56, "AMPLIFY BROWSER");
    pk_text_bold(vp, x, 78, "Куда отправимся?", th->ink, UI_FONT_44);
    pk_text(vp, x, 146, "Введите в строке сверху адрес (example.com) или слова для поиска и нажмите Enter.", th->muted, UI_FONT_16);
    pk_text(vp, x, 170, "Схему можно не писать: сначала пробуется https, затем http.", th->muted, UI_FONT_16);
    pk_label(vp, x, 220, "ИЗБРАННОЕ");
    for (int i = 0; i < BR_FAVS; i++) {
        pk_rect_t r = fav_rect(L, i);
        // Указатель — в координатах окна, плитка — в координатах vp.
        pk_rect_t wr = { r.x + L->vp.x, r.y + L->vp.y, r.w, r.h };
        pk_state_t st = pk_pointer_state(w, wr, false);
        gfx_fill_round_rect(vp, r.x, r.y, r.w, r.h, 14, st == PK_IDLE ? th->border : th->accent_coral);
        gfx_fill_round_rect(vp, r.x + 1, r.y + 1, r.w - 2, r.h - 2, 13, st == PK_ACTIVE ? th->surface_soft : th->canvas);
        gfx_fill_round_rect(vp, r.x + 16, r.y + 22, 52, 52, 12, th->accent_berry);
        gfx_fill_rect_gradient(vp, r.x + 18, r.y + 24, 48, 48, th->accent_orange, th->accent_berry);
        char l[2] = { k_favs[i].letter, 0 };
        int lw = ui_font_text_width(l, UI_FONT_28);
        pk_text_bold(vp, r.x + 42 - lw / 2, r.y + 30, l, th->on_accent, UI_FONT_28);
        pk_text_clip(vp, r.x + 82, r.y + 26, r.w - 94, k_favs[i].title, th->ink, UI_FONT_16, true);
        pk_text_clip(vp, r.x + 82, r.y + 52, r.w - 94, k_favs[i].note, th->muted, UI_FONT_12, false);
    }
    pk_rect_t last = fav_rect(L, BR_FAVS - 1);
    pk_text(vp, x, last.y + last.h + 28,
            "Показываются текст и ссылки: без картинок, стилей и скриптов. Сайты, которым нужен TLS 1.3, не откроются.",
            th->muted, UI_FONT_12);
}

typedef struct { const char* title; const char* l1; const char* l2; } err_text_t;

static err_text_t err_describe(const browser_t* s) {
    err_text_t e = { "Страница не загрузилась", "", "" };
    if (s->f.status == -1 && s->err == NET_ERR_PROTOCOL && s->f.content_type[0]) {
        e.title = "Этот тип содержимого не показать";
        e.l1 = "Сервер прислал не текст и не HTML (картинку, архив, видео).";
        e.l2 = "Браузер AMPLIFY-OS показывает только текстовые страницы.";
        return e;
    }
    switch (s->err) {
    case NET_ERR_NO_IFACE:
        e.title = "Нет сетевой карты";
        e.l1 = "Система не нашла сетевой адаптер (virtio-net, e1000, rtl8139).";
        e.l2 = "Проверьте, что у виртуальной машины есть сетевое устройство."; break;
    case NET_ERR_NO_ADDR:
        e.title = "Нет подключения к сети";
        e.l1 = "У компьютера нет сетевого адреса: DHCP-сервер не ответил, а статический адрес не задан.";
        e.l2 = "Откройте настройки сети и задайте адрес вручную или повторите запрос DHCP."; break;
    case NET_ERR_DNS:
        if (s->dns_fail == DNS_FAIL_NXDOMAIN) {
            e.title = "Такого сайта нет";
            e.l1 = "DNS-сервер ответил, что имя не существует.";
            e.l2 = "Проверьте, нет ли опечатки в адресе.";
        } else {
            e.title = "DNS не отвечает";
            e.l1 = "Не удалось узнать адрес сайта: ни один DNS-сервер не ответил.";
            e.l2 = "Проверьте шлюз и DNS в настройках сети (кнопка «Проверить DNS» в Mesh).";
        }
        break;
    case NET_ERR_UNREACHABLE:
        e.title = "Узел недоступен";
        e.l1 = "Шлюз не отвечает в локальной сети, либо маршрутизатор сообщил, что пути к серверу нет.";
        e.l2 = "Проверьте адрес шлюза и маску в настройках сети; возможно, у сети нет выхода в интернет."; break;
    case NET_ERR_REFUSED:
        e.title = "Сервер отклонил соединение";
        e.l1 = "Компьютер по этому адресу есть, но на нужном порту никто не слушает.";
        e.l2 = "Проверьте адрес и порт; возможно, сайт выключен."; break;
    case NET_ERR_CONNECT:
        e.title = "Сервер не отвечает";
        e.l1 = "Запрос на соединение остался без ответа (4 попытки за 6 секунд).";
        e.l2 = "Сайт недоступен или у компьютера нет выхода в интернет."; break;
    case NET_ERR_TLS:
    case NET_ERR_TLS_TRUST:
        e.title = "Защищённое соединение не установлено";
        e.l1 = "Не удалось договориться с сервером о шифровании.";
        e.l2 = "AMPLIFY-OS умеет TLS 1.2; сайты, требующие только TLS 1.3, не откроются."; break;
    case NET_ERR_TLS_SILENT:
        e.title = "Сервер молчит в ответ на приветствие";
        e.l1 = "Соединение установлено, но на приветствие TLS сервер не ответил ни байтом за 10 секунд.";
        e.l2 = "Так выглядит блокировка сайта по имени на пути к нему; сам сайт при этом может работать."; break;
    case NET_ERR_RESET:
        e.title = "Соединение сброшено";
        e.l1 = "Сервер оборвал соединение посреди ответа.";
        e.l2 = "Попробуйте повторить запрос."; break;
    case NET_ERR_TIMEOUT:
        e.title = "Сервер замолчал";
        e.l1 = "Соединение установлено, но ответ не пришёл за 30 секунд.";
        e.l2 = "Попробуйте повторить запрос позже."; break;
    case NET_ERR_REDIRECTS:
        e.title = "Слишком много перенаправлений";
        e.l1 = "Сайт пересылает запрос по кругу (больше 8 переходов подряд).";
        e.l2 = ""; break;
    case NET_ERR_CANCELLED:
        e.title = "Загрузка остановлена";
        e.l1 = "Вы прервали загрузку страницы.";
        e.l2 = ""; break;
    case NET_ERR_URL:
        e.title = "Адрес не разобрать";
        e.l1 = "В адресе ошибка. Пример правильного адреса: https://example.com/page";
        e.l2 = ""; break;
    default:
        e.title = "Сервер ответил непонятно";
        e.l1 = "Ответ не похож на HTTP.";
        e.l2 = ""; break;
    }
    return e;
}

static bool err_wants_mesh(const browser_t* s) {
    return s->err == NET_ERR_NO_ADDR || s->err == NET_ERR_DNS || s->err == NET_ERR_UNREACHABLE ||
           s->err == NET_ERR_NO_IFACE || s->err == NET_ERR_CONNECT;
}

static void draw_error(window_t* w, const gfx_target_t* vp, browser_t* s, const br_layout_t* L) {
    const ui_runtime_theme_t* th = ui_theme_current();
    err_text_t e = err_describe(s);
    int x = L->col_x;
    // Знак: круг с восклицательным знаком в цветах темы.
    gfx_fill_round_rect(vp, x, 64, 64, 64, 32, th->accent_berry);
    gfx_fill_round_rect(vp, x + 3, 67, 58, 58, 29, th->surface);
    gfx_fill_round_rect(vp, x + 29, 80, 6, 24, 3, th->accent_berry);
    gfx_fill_round_rect(vp, x + 29, 109, 6, 6, 3, th->accent_berry);
    pk_text_bold(vp, x, 150, e.title, th->ink, UI_FONT_28);
    pk_text_clip(vp, x, 198, L->col_w, e.l1, th->ink, UI_FONT_16, false);
    pk_text_clip(vp, x, 224, L->col_w, e.l2, th->muted, UI_FONT_16, false);
    pk_text_clip(vp, x, 256, L->col_w, s->url, th->muted, UI_FONT_MONO_14, false);

    pk_rect_t r0 = err_btn(L, 0), r1 = err_btn(L, 1);
    pk_rect_t w0 = { r0.x + L->vp.x, r0.y + L->vp.y, r0.w, r0.h };
    pk_rect_t w1 = { r1.x + L->vp.x, r1.y + L->vp.y, r1.w, r1.h };
    pk_button(vp, r0, "Повторить", true, pk_pointer_state(w, w0, false));
    if (err_wants_mesh(s)) pk_button(vp, r1, "Настройки сети", false, pk_pointer_state(w, w1, false));

    char d[96];
    b_copy(d, "Код причины: ", sizeof(d)); b_cat(d, net_error_name(s->err), sizeof(d));
    pk_text(vp, x, 368, d, th->muted, UI_FONT_12);
}

static void draw_page(window_t* w, const gfx_target_t* vp, browser_t* s, const br_layout_t* L) {
    const ui_runtime_theme_t* th = ui_theme_current();
    if (s->layout_w != L->col_w) { doc_layout(s, L->col_w); clamp_scroll(s, L); }
    if (s->jump_main) {
        // Первая строка основного содержимого: если она ниже первого экрана,
        // начинаем с неё (меню сайта остаётся выше, Home — к началу).
        s->jump_main = false;
        int lo = 0, hi = s->n_lines;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if ((int)s->lines[mid].start < s->main_pos) lo = mid + 1; else hi = mid;
        }
        if (lo < s->n_lines && s->scroll == 0 && s->lines[lo].y > L->vp.h - 80) {
            s->scroll = s->lines[lo].y - 24;
            clamp_scroll(s, L);
            serial_puts("[browser] start at main content y="); serial_print_dec((uint64_t)s->scroll); serial_puts("\n");
        }
    }

    // Перетаскивание ползунка: позицию берём у мыши, пока кнопка нажата.
    int ms = max_scroll(s, L);
    int th_h = L->track.h;
    if (s->doc_h > L->vp.h) { th_h = (int)((int64_t)L->track.h * L->vp.h / s->doc_h); if (th_h < 36) th_h = 36; }
    if (s->drag_scroll) {
        int lx, ly; bool down;
        pk_pointer(w, &lx, &ly, &down);
        if (!down) { s->drag_scroll = false; w->wants_continuous_redraw = false; }
        else if (ms > 0 && L->track.h > th_h) {
            int rel = ly - L->vp.y - L->track.y - s->drag_off;
            s->scroll = (int)((int64_t)rel * ms / (L->track.h - th_h));
            clamp_scroll(s, L);
        }
    }

    int first = line_at(s, s->scroll);
    for (int i = first; i < s->n_lines; i++) {
        const br_line_t* ln = &s->lines[i];
        int top = ln->y - s->scroll;
        if (top >= L->vp.h) break;
        int ox = L->col_x;
        if (ln->quote) gfx_fill_rect(vp, ox + ln->x - 14, top, 3, ln->h, th->border);
        if (ln->type == BT_HR) { gfx_fill_rect(vp, ox + ln->x, top, L->col_w - ln->x, 1, th->border); continue; }
        if (ln->type == BT_PRE) gfx_fill_rect(vp, ox + ln->x, top, L->col_w - ln->x, ln->h, th->surface_soft);
        if (ln->type == BT_LI && ln->first) {
            if (ln->num) {
                char nb[12]; se_num(nb, ln->num); b_cat(nb, ".", sizeof(nb));
                int nw = ui_font_text_width(nb, UI_FONT_16);
                pk_text(vp, ox + ln->x - 8 - nw, top + (ln->h - ui_font_line_height(UI_FONT_16)) / 2, nb, th->muted, UI_FONT_16);
            } else {
                gfx_fill_round_rect(vp, ox + ln->x - 16, top + ln->h / 2 - 3, 6, 6, 3, th->accent_coral);
            }
        }
        line_walk(s, ln, vp, ox + (ln->type == BT_PRE ? 12 : 0), top, -1, 0);
    }

    // Обрезанная страница — пометка в конце документа.
    if (s->f.truncated || s->text_overflow || !s->f.complete) {
        int ny = s->doc_h - 40 - s->scroll;
        if (ny < L->vp.h && ny > -20)
            pk_text(vp, L->col_x, ny, s->f.truncated || s->text_overflow
                    ? "Страница больше предела браузера (4 МБ HTML, 512 КБ текста) — показано начало."
                    : "Сервер закрыл соединение раньше конца страницы — показано то, что пришло.",
                    th->accent_berry, UI_FONT_12);
    }

    // Полоса прокрутки.
    if (ms > 0) {
        gfx_fill_round_rect(vp, L->track.x, L->track.y, L->track.w, L->track.h, 4, th->surface_soft);
        int ty = L->track.y + (int)((int64_t)(L->track.h - th_h) * s->scroll / ms);
        gfx_fill_round_rect(vp, L->track.x, ty, L->track.w, th_h, 4, s->drag_scroll ? th->accent_coral : th->border);
    }
}

// Экранные координаты элементов — в serial (для теста, см. app_mesh.c).
static void log_pt(const window_t* w, const char* name, pk_rect_t r) {
    serial_puts(" "); serial_puts(name); serial_puts("=");
    serial_print_dec((uint64_t)(w->x + r.x + r.w / 2)); serial_puts(",");
    serial_print_dec((uint64_t)(w->y + r.y + r.h / 2));
}

static void log_links(const window_t* w, browser_t* s, const br_layout_t* L) {
    serial_puts("[browser] links:");
    uint16_t seen = 0;
    int shown = 0;
    for (int i = line_at(s, s->scroll); i < s->n_lines && shown < 4; i++) {
        const br_line_t* ln = &s->lines[i];
        int top = ln->y - s->scroll;
        if (top + ln->h > L->vp.h) break;
        // Найти в строке ссылки с номером больше уже показанных.
        uint32_t si = ln->sidx;
        for (uint32_t p = ln->start; p < ln->end && shown < 4; ) {
            while (si + 1 < (uint32_t)s->n_styles && s->styles[si + 1].pos <= p) si++;
            bool active = s->n_styles > 0 && s->styles[si].pos <= p;
            uint32_t e = ln->end;
            uint32_t nx = active ? si + 1 : si;
            if (nx < (uint32_t)s->n_styles && s->styles[nx].pos < e) e = s->styles[nx].pos;
            uint16_t link = active ? s->styles[si].link : 0;
            if (link && link > seen) {
                seen = link;
                int lx = L->col_x + ln->x + span_width(s, ln->start, p, (ui_font_id_t)ln->font) + 6;
                serial_puts(" ");
                serial_print_dec((uint64_t)(w->x + L->vp.x + lx)); serial_puts(",");
                serial_print_dec((uint64_t)(w->y + L->vp.y + top + ln->h / 2));
                shown++;
            }
            p = e;
        }
    }
    serial_puts(" doc_h="); serial_print_dec((uint64_t)s->doc_h);
    serial_puts(" lines="); serial_print_dec((uint64_t)s->n_lines);
    serial_puts("\n");
}

static void browser_draw(window_t* w, const gfx_target_t* t) {
    browser_t* s = (browser_t*)w->user_data;
    const ui_runtime_theme_t* th = ui_theme_current();
    br_layout_t L;
    br_layout(w, &L);
    if (!s->layout_logged) {
        s->layout_logged = true;
        serial_puts("[browser] layout");
        log_pt(w, "back", L.tb[TB_BACK]); log_pt(w, "forward", L.tb[TB_FWD]);
        log_pt(w, "reload", L.tb[TB_RELOAD]); log_pt(w, "home", L.tb[TB_HOME]);
        log_pt(w, "addr", L.addr); log_pt(w, "source", L.src);
        serial_puts("\n");
    }

    gfx_clear(t, th->canvas);
    draw_toolbar(w, t, s, &L);
    pk_card(t, L.card);

    // Область страницы — отдельная «мишень» внутри буфера окна: всё, что
    // рисуется за её краями (строка, наполовину ушедшая под панель), режется
    // границами мишени само.
    gfx_target_t vp = { (uint32_t*)((uint8_t*)t->pixels + (size_t)L.vp.y * t->pitch) + L.vp.x,
                        L.vp.w, L.vp.h, t->pitch };
    if (s->view == VIEW_START)      draw_start(w, &vp, &L);
    else if (s->view == VIEW_ERROR) draw_error(w, &vp, s, &L);
    else if (s->view == VIEW_LOADING) {
        pk_text_bold(&vp, L.col_x, 150, "Загружаю страницу", th->ink, UI_FONT_28);
        pk_text_clip(&vp, L.col_x, 204, L.col_w, s->url, th->muted, UI_FONT_MONO_14, false);
    }
    else                            draw_page(w, &vp, s, &L);
    if (s->view == VIEW_PAGE && !s->links_logged && !s->loading) { s->links_logged = true; log_links(w, s, &L); }

    // Строка состояния.
    char st[200];
    st[0] = 0;
    if (s->loading) {
        char sz[32];
        switch (net_progress_stage()) {
            case NET_STAGE_DNS:     b_copy(st, "Ищу адрес сайта...", sizeof(st)); break;
            case NET_STAGE_CONNECT: b_copy(st, "Соединяюсь с сервером...", sizeof(st)); break;
            case NET_STAGE_TLS:     b_copy(st, "Устанавливаю защищённое соединение (TLS)...", sizeof(st)); break;
            case NET_STAGE_REQUEST: b_copy(st, "Запрос отправлен, жду ответ...", sizeof(st)); break;
            default:
                b_copy(st, "Загружаю: ", sizeof(st));
                fmt_size(sz, sizeof(sz), net_progress_done()); b_cat(st, sz, sizeof(st));
                if (net_progress_total() > 0) { b_cat(st, " из ", sizeof(st)); fmt_size(sz, sizeof(sz), (uint64_t)net_progress_total()); b_cat(st, sz, sizeof(st)); }
                break;
        }
        b_cat(st, "   Esc - остановить", sizeof(st));
        pk_text(t, L.status.x, L.status.y, st, th->accent_berry, UI_FONT_12);
    } else {
        // Ссылка под указателем — её адрес; иначе сведения о странице.
        uint16_t hover = 0;
        if (s->view == VIEW_PAGE && w->focused) {
            int lx, ly;
            pk_pointer(w, &lx, &ly, 0);
            lx -= L.vp.x; ly -= L.vp.y;
            if (lx >= 0 && ly >= 0 && lx < L.vp.w - 20 && ly < L.vp.h) {
                int i = line_at(s, s->scroll + ly);
                if (i < s->n_lines && s->lines[i].y <= s->scroll + ly)
                    hover = line_walk(s, &s->lines[i], 0, L.col_x, 0, lx, 0);
            }
        }
        if (hover) {
            pk_text_clip(t, L.status.x, L.status.y, L.status.w, s->hrefs + s->href_off[hover - 1], th->accent_berry, UI_FONT_12, false);
        } else if (s->view == VIEW_PAGE) {
            char sz[32];
            b_copy(st, "HTTP ", sizeof(st)); b_cat_num(st, (uint64_t)s->f.status, sizeof(st));
            b_cat(st, "  \xB7  ", sizeof(st)); fmt_size(sz, sizeof(sz), (uint64_t)s->raw_len); b_cat(st, sz, sizeof(st));
            b_cat(st, "  \xB7  ссылок: ", sizeof(st)); b_cat_num(st, (uint64_t)s->n_links, sizeof(st));
            b_cat(st, "  \xB7  ", sizeof(st)); b_cat_num(st, s->load_ms, sizeof(st)); b_cat(st, " мс", sizeof(st));
            if (s->title[0]) { b_cat(st, "  \xB7  ", sizeof(st)); b_cat(st, s->title, sizeof(st)); }
            pk_text_clip(t, L.status.x, L.status.y, L.status.w - 80, st, th->muted, UI_FONT_12, false);
            int ms = max_scroll(s, &L);
            char pc[16]; pc[0] = 0;
            b_cat_num(pc, ms ? (uint64_t)s->scroll * 100 / (uint64_t)ms : 100, sizeof(pc)); b_cat(pc, "%", sizeof(pc));
            pk_text(t, L.status.x + L.status.w - ui_font_text_width(pc, UI_FONT_12), L.status.y, pc, th->muted, UI_FONT_12);
        } else {
            pk_text(t, L.status.x, L.status.y,
                    "L - адрес   B/F - назад/вперёд   R - обновить   H - домой   U - исходный текст   Esc - закрыть",
                    th->muted, UI_FONT_12);
        }
    }
}

// ---------------------------------------------------------------------------
// Ввод.
// ---------------------------------------------------------------------------
static void focus_addr(browser_t* s) { s->addr_focus = true; s->edit_fresh = s->edit[0] != 0; }

static void scroll_by(browser_t* s, window_t* w, int dy) {
    if (s->view != VIEW_PAGE) return;
    br_layout_t L;
    br_layout(w, &L);
    s->scroll += dy;
    clamp_scroll(s, &L);
}

static void browser_click(window_t* w, int lx, int ly) {
    browser_t* s = (browser_t*)w->user_data;
    if (s->loading) return;
    br_layout_t L;
    br_layout(w, &L);

    if (pk_hit(L.tb[TB_BACK], lx, ly))   { br_step(s, -1); return; }   // (форма сбрасывается в br_load)
    if (pk_hit(L.tb[TB_FWD], lx, ly))    { br_step(s, +1); return; }
    if (pk_hit(L.tb[TB_RELOAD], lx, ly)) { br_reload(s); return; }
    if (pk_hit(L.tb[TB_HOME], lx, ly))   { br_home(s); return; }
    if (pk_hit(L.src, lx, ly))           { br_toggle_source(s); return; }
    if (pk_hit(L.addr, lx, ly))          { if (!s->form_prefix[0]) focus_addr(s); return; }
    if (s->form_prefix[0]) { s->form_prefix[0] = 0; set_edit(s, s->url); }   // запрос не набрали
    s->addr_focus = false;

    if (!pk_hit(L.vp, lx, ly)) return;
    int vx = lx - L.vp.x, vy = ly - L.vp.y;

    if (s->view == VIEW_START) {
        for (int i = 0; i < BR_FAVS; i++)
            if (pk_hit(fav_rect(&L, i), vx, vy)) { br_navigate(s, k_favs[i].url); return; }
        return;
    }
    if (s->view == VIEW_ERROR) {
        if (pk_hit(err_btn(&L, 0), vx, vy)) { br_reload(s); return; }
        if (err_wants_mesh(s) && pk_hit(err_btn(&L, 1), vx, vy)) { (void)app_mesh_open(); return; }
        return;
    }

    // Полоса прокрутки: ползунок — тащим, дорожка — страница вверх/вниз.
    int ms = max_scroll(s, &L);
    if (ms > 0 && vx >= L.track.x - 6) {
        int th_h = (int)((int64_t)L.track.h * L.vp.h / s->doc_h);
        if (th_h < 36) th_h = 36;
        int ty = L.track.y + (int)((int64_t)(L.track.h - th_h) * s->scroll / ms);
        if (vy >= ty && vy < ty + th_h) {
            s->drag_scroll = true;
            s->drag_off = vy - ty;
            w->wants_continuous_redraw = true;          // кадры идут, пока тащим
        } else {
            s->scroll += (vy < ty ? -1 : 1) * (L.vp.h - 60);
            clamp_scroll(s, &L);
        }
        return;
    }

    int i = line_at(s, s->scroll + vy);
    if (i < s->n_lines && s->lines[i].y <= s->scroll + vy) {
        uint16_t link = line_walk(s, &s->lines[i], 0, L.col_x, 0, vx, 0);
        if (link) br_follow(s, s->hrefs + s->href_off[link - 1]);
    }
}

static void browser_scroll(window_t* w, int notches) {
    scroll_by((browser_t*)w->user_data, w, -notches * 72);
}

static void browser_key(window_t* w, char c) {
    browser_t* s = (browser_t*)w->user_data;
    if (s->loading) return;
    br_layout_t L;
    br_layout(w, &L);
    int page = L.vp.h - 60;

    // Прокрутка работает всегда, в том числе при курсоре в адресной строке.
    if (c == KBD_KEY_PGUP) { scroll_by(s, w, -page); return; }
    if (c == KBD_KEY_PGDN) { scroll_by(s, w, page); return; }
    if (c == KBD_KEY_UP)   { scroll_by(s, w, -56); return; }
    if (c == KBD_KEY_DOWN) { scroll_by(s, w, 56); return; }
    if (c == KBD_KEY_HOME && !s->addr_focus) { s->scroll = 0; return; }
    if (c == KBD_KEY_END && !s->addr_focus)  { s->scroll = max_scroll(s, &L); return; }
    if (c == 12) { s->form_prefix[0] = 0; set_edit(s, s->url); focus_addr(s); return; }   // Ctrl+L
    if (c == 18) { br_reload(s); return; }                       // Ctrl+R

    if (s->addr_focus) {
        if (c == 27) { s->addr_focus = false; s->form_prefix[0] = 0; set_edit(s, s->url); return; }
        if (c == '\n') { br_go_typed(s); return; }
        if (c == '\b') {
            if (s->edit_fresh) { s->edit[0] = 0; s->edit_len = 0; }
            else if (s->edit_len > 0) s->edit[--s->edit_len] = 0;
            s->edit_fresh = false;
            return;
        }
        if (c == KBD_KEY_END || c == KBD_KEY_RIGHT || c == KBD_KEY_LEFT || c == KBD_KEY_HOME) { s->edit_fresh = false; return; }
        if (c >= 32 && c < 127) {                                 // пробел — для поисковых запросов
            if (c == ' ' && (s->edit_len == 0 || s->edit_fresh)) return;
            if (s->edit_fresh) { s->edit_len = 0; s->edit_fresh = false; }
            if (s->edit_len < BR_URL - 1) { s->edit[s->edit_len++] = c; s->edit[s->edit_len] = 0; }
        }
        return;
    }

    if (c == 27) { se_close(w); return; }
    if (c == ' ') { scroll_by(s, w, page); return; }
    if (c == '\b' || c == 'b' || c == 'B' || c == KBD_KEY_LEFT) { br_step(s, -1); return; }
    if (c == 'f' || c == 'F' || c == KBD_KEY_RIGHT) { br_step(s, +1); return; }
    if (c == 'r' || c == 'R') { br_reload(s); return; }
    if (c == 'h' || c == 'H') { br_home(s); return; }
    if (c == 'u' || c == 'U') { br_toggle_source(s); return; }
    if (c == 'l' || c == 'L' || c == '/') { focus_addr(s); return; }
}

static void browser_destroy(window_t* w) {
    if (g_br && g_br->win == w) g_br->win = 0;
}

window_t* app_browser_open(void) {
    if (g_br && g_br->win) { wm_focus(g_br->win); g_br->win->visible = true; return g_br->win; }
    if (!g_br) {
        // Состояние и буферы — один раз на всю жизнь системы: закрытие окна
        // историю и страницу не теряет, а ~4 МБ не гуляют по куче туда-сюда.
        browser_t* s = (browser_t*)kmalloc(sizeof(*s));
        if (!s) return 0;
        for (size_t i = 0; i < sizeof(*s); i++) ((uint8_t*)s)[i] = 0;
        s->raw      = (char*)kmalloc(BR_RAW_CAP + 2);
        s->text     = (char*)kmalloc(BR_TEXT_CAP);
        s->styles   = (br_style_t*)kmalloc(sizeof(br_style_t) * BR_STYLE_MAX);
        s->blocks   = (br_block_t*)kmalloc(sizeof(br_block_t) * BR_BLOCK_MAX);
        s->lines    = (br_line_t*)kmalloc(sizeof(br_line_t) * BR_LINE_MAX);
        s->hrefs    = (char*)kmalloc(BR_HREF_POOL);
        s->href_off = (uint32_t*)kmalloc(sizeof(uint32_t) * BR_LINK_MAX);
        if (!s->raw || !s->text || !s->styles || !s->blocks || !s->lines || !s->hrefs || !s->href_off) {
            serial_puts("[browser] out of memory\n");
            return 0;
        }
        s->hist_pos = -1;
        g_br = s;
    }
    browser_t* s = g_br;
    window_t* w = se_workspace("Browser", browser_draw, browser_click, browser_key, s, false);
    if (!w) return 0;
    w->owns_user_data = false;                  // состояние переживает окно
    w->on_destroy = browser_destroy;
    w->on_scroll  = browser_scroll;
    s->win = w;
    s->layout_logged = false;
    s->links_logged = false;
    s->layout_w = 0;
    s->loading = false;
    s->drag_scroll = false;
    serial_puts("[browser] open\n");
    if (s->hist_n == 0) br_navigate(s, BR_START_URL);
    else if (s->view == VIEW_START) { s->addr_focus = true; s->edit_fresh = false; }
    return w;
}

// ============================================================================
// cookies.c — хранилище cookies браузера (см. cookies.h).
// ============================================================================
#include "cookies.h"
#include "serial.h"
#include <stdint.h>

#define CK_DOMAIN 72
#define CK_NAME   64
#define CK_VALUE  768
#define CK_PATH   64

typedef struct {
    bool used, secure, host_only;
    char domain[CK_DOMAIN];
    char name[CK_NAME];
    char value[CK_VALUE];
    char path[CK_PATH];
} cookie_t;

static cookie_t g_jar[COOKIE_MAX];
static int      g_next = 0;                 // кого вытеснять следующим

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }
static int  slen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static bool eq_ci(const char* a, const char* b) {
    for (;; a++, b++) {
        if (lower(*a) != lower(*b)) return false;
        if (!*a) return true;
    }
}
static bool eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
// name — имя атрибута в нижнем регистре; сравнение с отрезком [p, p+n).
static bool attr_is(const char* p, int n, const char* name) {
    int i = 0;
    for (; i < n; i++) { if (!name[i] || lower(p[i]) != name[i]) return false; }
    return name[i] == 0;
}

// host == domain либо host оканчивается на "." + domain.
static bool domain_match(const char* host, const char* domain) {
    int hl = slen(host), dl = slen(domain);
    if (dl == 0 || dl > hl) return false;
    for (int i = 0; i < dl; i++) if (lower(host[hl - dl + i]) != lower(domain[i])) return false;
    return hl == dl || host[hl - dl - 1] == '.';
}

void cookies_clear(void) { for (int i = 0; i < COOKIE_MAX; i++) g_jar[i].used = false; }
int  cookies_count(void) { int n = 0; for (int i = 0; i < COOKIE_MAX; i++) if (g_jar[i].used) n++; return n; }

// Одна строка Set-Cookie: "name=value; Attr=...; Attr".
static void store_one(const char* host, const char* v, int vlen) {
    cookie_t c;
    c.used = true; c.secure = false; c.host_only = true;
    c.path[0] = '/'; c.path[1] = 0;
    int i = 0;
    while (i < vlen && (v[i] == ' ' || v[i] == '\t')) i++;

    // имя=значение
    int n = 0;
    while (i < vlen && v[i] != '=' && v[i] != ';') { if (n >= CK_NAME - 1) return; c.name[n++] = v[i++]; }
    while (n > 0 && c.name[n - 1] == ' ') n--;
    c.name[n] = 0;
    if (n == 0 || i >= vlen || v[i] != '=') return;
    i++;
    n = 0;
    while (i < vlen && v[i] != ';') { if (n >= CK_VALUE - 1) return; c.value[n++] = v[i++]; }  // слишком длинную не храним вовсе
    while (n > 0 && c.value[n - 1] == ' ') n--;
    c.value[n] = 0;

    bool del = (n == 0);
    int k = 0;
    for (k = 0; host[k] && k < CK_DOMAIN - 1; k++) c.domain[k] = lower(host[k]);
    c.domain[k] = 0;

    // атрибуты
    while (i < vlen) {
        if (v[i] == ';') i++;
        while (i < vlen && v[i] == ' ') i++;
        int as = i;
        while (i < vlen && v[i] != '=' && v[i] != ';') i++;
        int an = i - as;
        int vs = i, vn = 0;
        if (i < vlen && v[i] == '=') { vs = ++i; while (i < vlen && v[i] != ';') i++; vn = i - vs; }

        if (attr_is(v + as, an, "secure")) c.secure = true;
        else if (attr_is(v + as, an, "max-age")) {
            if (vn > 0 && (v[vs] == '-' || (v[vs] == '0' && vn == 1))) del = true;
        } else if (attr_is(v + as, an, "path")) {
            if (vn > 0 && v[vs] == '/' && vn < CK_PATH) { for (int q = 0; q < vn; q++) c.path[q] = v[vs + q]; c.path[vn] = 0; }
        } else if (attr_is(v + as, an, "domain")) {
            if (vn > 0 && v[vs] == '.') { vs++; vn--; }
            if (vn > 0 && vn < CK_DOMAIN) {
                char d[CK_DOMAIN];
                for (int q = 0; q < vn; q++) d[q] = lower(v[vs + q]);
                d[vn] = 0;
                // Сайт может ставить cookie только себе или своему домену, и не
                // на домен верхнего уровня («.com»): в имени должна быть точка.
                bool has_dot = false;
                for (int q = 0; q < vn; q++) if (d[q] == '.') has_dot = true;
                if (!has_dot || !domain_match(host, d)) return;
                for (int q = 0; q <= vn; q++) c.domain[q] = d[q];
                c.host_only = false;
            }
        }
    }

    // Обновить существующую (тот же домен, имя, путь) или занять слот.
    int slot = -1;
    for (int j = 0; j < COOKIE_MAX; j++)
        if (g_jar[j].used && eq_ci(g_jar[j].domain, c.domain) && eq(g_jar[j].name, c.name) && eq(g_jar[j].path, c.path)) { slot = j; break; }
    if (del) { if (slot >= 0) g_jar[slot].used = false; return; }
    if (slot < 0) for (int j = 0; j < COOKIE_MAX; j++) if (!g_jar[j].used) { slot = j; break; }
    if (slot < 0) { slot = g_next; g_next = (g_next + 1) % COOKIE_MAX; }
    g_jar[slot] = c;
}

void cookies_store(const char* host, const char* hdrs, size_t hlen) {
    static const char key[] = "set-cookie:";
    int stored = 0;
    size_t i = 0;
    while (i < hlen) {
        size_t ls = i;
        while (i < hlen && hdrs[i] != '\n') i++;
        size_t le = i;                               // конец строки (без \n)
        if (i < hlen) i++;
        if (le > ls && hdrs[le - 1] == '\r') le--;
        if (le - ls < sizeof(key) - 1) continue;
        bool m = true;
        for (size_t k = 0; k < sizeof(key) - 1; k++) if (lower(hdrs[ls + k]) != key[k]) { m = false; break; }
        if (!m) continue;
        store_one(host, hdrs + ls + sizeof(key) - 1, (int)(le - ls - (sizeof(key) - 1)));
        stored++;
    }
    if (stored) {
        serial_puts("[cookies] "); serial_print_dec((uint64_t)stored);
        serial_puts(" set by "); serial_puts(host);
        serial_puts(", jar="); serial_print_dec((uint64_t)cookies_count()); serial_puts("\n");
    }
}

int cookies_header(const char* host, const char* path, bool https, char* out, int cap) {
    int n = 0;
    bool any = false;
    out[0] = 0;
    if (cap < 16) return 0;
    for (int j = 0; j < COOKIE_MAX; j++) {
        const cookie_t* c = &g_jar[j];
        if (!c->used) continue;
        if (c->secure && !https) continue;
        if (c->host_only ? !eq_ci(host, c->domain) : !domain_match(host, c->domain)) continue;
        // Путь cookie — префикс пути запроса.
        int pl = slen(c->path);
        bool pm = true;
        for (int q = 0; q < pl; q++) if (path[q] != c->path[q]) { pm = false; break; }
        if (!pm) continue;
        int need = slen(c->name) + 1 + slen(c->value) + 2;
        if (n + need + 12 >= cap) break;             // не влезает — остальные не шлём
        if (!any) { const char* h = "Cookie: "; while (*h) out[n++] = *h++; any = true; }
        else { out[n++] = ';'; out[n++] = ' '; }
        for (const char* p = c->name; *p; p++)  out[n++] = *p;
        out[n++] = '=';
        for (const char* p = c->value; *p; p++) out[n++] = *p;
    }
    if (any) { out[n++] = '\r'; out[n++] = '\n'; }
    out[n] = 0;
    return n;
}

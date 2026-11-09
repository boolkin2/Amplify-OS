// ============================================================================
// users.c — реализация базы пользователей.
// ============================================================================
#include "users.h"
#include "fat32.h"
#include "serial.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static user_record_t g_users[USERS_MAX_USERS];
static int           g_count = 0;

// ----------------------------------------------------------------------------
// FNV-1a 64-bit. Простая, fast, без зависимостей.
// ----------------------------------------------------------------------------
uint64_t users_hash(const char* str) {
    uint64_t h = 0xcbf29ce484222325ULL;        // FNV offset basis (64-bit)
    while (*str) {
        h ^= (uint8_t)*str++;
        h *= 0x100000001b3ULL;                  // FNV prime (64-bit)
    }
    return h;
}

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------
static void copy_name(char* dst, const char* src) {
    int i;
    for (i = 0; i < USERS_NAME_LEN - 1 && src[i] && src[i] != '\n' && src[i] != ':'; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex64_to_str(uint64_t v, char out[17]) {
    static const char H[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--) {
        out[i] = H[v & 0xF];
        v >>= 4;
    }
    out[16] = 0;
}

static uint64_t parse_hex64(const char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int d = hex_digit(p[i]);
        if (d < 0) break;
        v = (v << 4) | (uint64_t)d;
    }
    return v;
}

// ----------------------------------------------------------------------------
// Парсинг /disk/users.txt
// ----------------------------------------------------------------------------
static void parse_buffer(const char* buf, int len) {
    g_count = 0;
    int i = 0;
    while (i < len && g_count < USERS_MAX_USERS) {
        // Пропустить ведущие пустые строки.
        while (i < len && (buf[i] == '\n' || buf[i] == '\r')) i++;
        if (i >= len) break;

        const char* line_start = buf + i;
        while (i < len && buf[i] != '\n') i++;
        int line_end = i;
        if (i < len) i++; // съесть '\n'

        // Ищем ':'
        int sep = -1;
        for (int k = 0; k < line_end - (line_start - buf); k++) {
            if (line_start[k] == ':') { sep = k; break; }
        }
        if (sep < 0) continue;

        // Имя
        user_record_t* u = &g_users[g_count];
        copy_name(u->name, line_start);
        if (u->name[0] == 0) continue;

        // Hash (16 hex chars после ':')
        if (sep + 16 > line_end - (line_start - buf)) continue;
        u->pw_hash = parse_hex64(line_start + sep + 1);

        g_count++;
    }
}

void users_init(void) {
    g_count = 0;
    static char buf[1024];
    int64_t n = fat32_read_file("/disk/users.txt", buf, sizeof(buf) - 1);
    if (n <= 0) {
        // Попробуем без префикса /disk/.
        n = fat32_read_file("users.txt", buf, sizeof(buf) - 1);
    }
    if (n > 0) {
        buf[n] = 0;
        parse_buffer(buf, (int)n);
        serial_puts("[users] loaded ");
        serial_print_dec(g_count);
        serial_puts(" record(s) from /disk/users.txt\n");
    } else {
        serial_puts("[users] /disk/users.txt not found — empty database (guest mode)\n");
    }
}

bool users_save(void) {
    if (!fat32_is_mounted()) return false;

    static char buf[1024];
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        const user_record_t* u = &g_users[i];
        for (int k = 0; u->name[k] && n < (int)sizeof(buf) - 1; k++) buf[n++] = u->name[k];
        if (n < (int)sizeof(buf) - 1) buf[n++] = ':';
        char hex[17];
        hex64_to_str(u->pw_hash, hex);
        for (int k = 0; k < 16 && n < (int)sizeof(buf) - 1; k++) buf[n++] = hex[k];
        if (n < (int)sizeof(buf) - 1) buf[n++] = '\n';
    }
    buf[n] = 0;

    int64_t wr = fat32_save_file("/disk/users.txt", buf, (uint64_t)n);
    if (wr < 0) {
        serial_puts("[users] save failed (FAT32 save returned -1)\n");
        return false;
    }
    serial_puts("[users] saved ");
    serial_print_dec(g_count);
    serial_puts(" record(s) to /disk/users.txt\n");
    return true;
}

int users_count(void) { return g_count; }

const user_record_t* users_at(int idx) {
    if (idx < 0 || idx >= g_count) return 0;
    return &g_users[idx];
}

const user_record_t* users_find(const char* name) {
    if (!name) return 0;
    for (int i = 0; i < g_count; i++) {
        const user_record_t* u = &g_users[i];
        int k;
        for (k = 0; u->name[k] && name[k]; k++) if (u->name[k] != name[k]) break;
        if (!u->name[k] && !name[k]) return u;
    }
    return 0;
}

int users_add(const char* name, const char* password) {
    if (!name || !password) return -1;
    if (g_count >= USERS_MAX_USERS) return -1;
    if (users_find(name)) return -1;

    user_record_t* u = &g_users[g_count];
    copy_name(u->name, name);
    if (u->name[0] == 0) return -1;
    u->pw_hash = users_hash(password);

    int idx = g_count++;
    serial_puts("[users] added '");
    serial_puts(u->name);
    serial_puts("'\n");
    return idx;
}

bool users_verify(const char* name, const char* password) {
    // Пустая база — legacy guest mode (любой логин принимается).
    if (g_count == 0) return true;
    const user_record_t* u = users_find(name);
    if (!u) return false;
    return u->pw_hash == users_hash(password ? password : "");
}

// ============================================================================
// users.c - База пользователей и хеширование паролей (Этап 32)
// ============================================================================
// Хеш-функция: FNV-1a на 64 бит, потом 4096 раундов смешивания (стрейчинг).
// Генерация соли: счётчик RDTSC + системный таймер + порядковый номер.
// Хранилище: /disk/Sanctum/users.db (массив фиксированных записей).
// ============================================================================

#include "security.h"
#include "../debug_serial.h"
#include "../fs/vfs.h"
#include "../memory/memory.h"
#include "../drivers/timer.h"

extern void dbg_puts(const char* s);

// ----------------------------------------------------------------------------
// БД в памяти
// ----------------------------------------------------------------------------

static sec_user_t g_users[SEC_MAX_USERS];
static int        g_users_count = 0;
static uint32_t   g_next_uid    = 1000;   // не считая root (UID=0)

// ----------------------------------------------------------------------------
// Локальные строковые помощники
// ----------------------------------------------------------------------------

static int s_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static int s_strlen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}

static void s_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

// ----------------------------------------------------------------------------
// Безопасное сравнение в постоянное время (для хешей и паролей)
// ----------------------------------------------------------------------------

bool sec_const_time_eq(const void* a, const void* b, uint32_t len) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    uint8_t diff = 0;
    for (uint32_t i = 0; i < len; i++) diff |= (uint8_t)(pa[i] ^ pb[i]);
    return diff == 0;
}

// ----------------------------------------------------------------------------
// Соль — собирается из RDTSC, jiffies таймера и инкрементного счётчика.
// Не криптографическая, но достаточно непредсказуема, чтобы соли разных
// пользователей не совпадали.
// ----------------------------------------------------------------------------

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void sec_make_salt(uint8_t out[SEC_SALT_LEN]) {
    static uint32_t counter = 0xC0DECAFE;
    counter += 0x9E3779B9;

    uint64_t t  = rdtsc();
    uint32_t jf = (uint32_t)timer_get_ticks();

    // Простой PRNG: xorshift, инициализированный из tsc/jiffies/counter.
    uint64_t s = t ^ ((uint64_t)jf * 0x9E3779B97F4A7C15ULL) ^ counter;
    if (s == 0) s = 0xDEADBEEFCAFEBABEULL;

    for (int i = 0; i < SEC_SALT_LEN; i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        out[i] = (uint8_t)(s & 0xFF);
    }
}

// ----------------------------------------------------------------------------
// Хеш пароля. Стратегия:
//   1. seed = FNV-1a 64-бит по последовательности (salt || password || salt).
//   2. Растягиваем результат до SEC_HASH_LEN байт через 4096 раундов
//      smix (xorshift64 + mixing с предыдущим байтом).
// Это не SHA-256, но для учебной ОС адекватно: timing-устойчивое сравнение
// и невозможность восстановить пароль из дампа БД без брутфорса.
// ----------------------------------------------------------------------------

void sec_hash_password(const char* password, const uint8_t salt[SEC_SALT_LEN],
                       uint8_t out[SEC_HASH_LEN]) {
    const uint64_t FNV_OFFSET = 0xCBF29CE484222325ULL;
    const uint64_t FNV_PRIME  = 0x100000001B3ULL;
    uint64_t h = FNV_OFFSET;

    for (int i = 0; i < SEC_SALT_LEN; i++) {
        h ^= salt[i];
        h *= FNV_PRIME;
    }
    for (int i = 0; password && password[i]; i++) {
        h ^= (uint8_t)password[i];
        h *= FNV_PRIME;
    }
    for (int i = 0; i < SEC_SALT_LEN; i++) {
        h ^= salt[(SEC_SALT_LEN - 1) - i];
        h *= FNV_PRIME;
    }
    if (h == 0) h = 0xA5A5A5A5A5A5A5A5ULL;

    // Растягиваем h в 32 байта через xorshift с подмешиванием соли.
    uint64_t s = h;
    for (int i = 0; i < SEC_HASH_LEN; i++) out[i] = (uint8_t)(s >> ((i & 7) * 8));

    // 4096 раундов перемешивания (key stretching).
    for (int round = 0; round < 4096; round++) {
        for (int i = 0; i < SEC_HASH_LEN; i++) {
            s ^= s << 13; s ^= s >> 7; s ^= s << 17;
            uint8_t prev = out[(i + SEC_HASH_LEN - 1) % SEC_HASH_LEN];
            uint8_t saltb = salt[i % SEC_SALT_LEN];
            out[i] = (uint8_t)(out[i] ^ prev ^ saltb ^ (uint8_t)s);
        }
    }
}

// ============================================================================
// Инициализация / запросы к БД
// ============================================================================

void sec_users_init(void) {
    g_users_count = 0;
    g_next_uid    = 1000;
    for (int i = 0; i < SEC_MAX_USERS; i++) {
        memset(&g_users[i], 0, sizeof(g_users[i]));
    }
}

int sec_users_count(void) {
    return g_users_count;
}

const sec_user_t* sec_users_get(int index) {
    if (index < 0 || index >= g_users_count) return NULL;
    return &g_users[index];
}

const sec_user_t* sec_users_find_by_name(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < g_users_count; i++) {
        if (s_strcmp(g_users[i].name, name) == 0) return &g_users[i];
    }
    return NULL;
}

const sec_user_t* sec_users_find_by_uid(uint32_t uid) {
    for (int i = 0; i < g_users_count; i++) {
        if (g_users[i].uid == uid) return &g_users[i];
    }
    return NULL;
}

// ----------------------------------------------------------------------------
// Аутентификация
// ----------------------------------------------------------------------------

const sec_user_t* sec_users_authenticate(const char* name, const char* password) {
    if (!name || !password) return NULL;

    const sec_user_t* u = sec_users_find_by_name(name);
    // ВАЖНО: считаем хеш в любом случае (даже если пользователя нет), чтобы
    // не сливать «пользователь существует» через измерение времени.
    uint8_t hash_buf[SEC_HASH_LEN];
    uint8_t dummy_salt[SEC_SALT_LEN];
    if (u) {
        sec_hash_password(password, u->salt, hash_buf);
    } else {
        // Никаких for() с rng тут — просто детерминированная соль.
        for (int i = 0; i < SEC_SALT_LEN; i++) dummy_salt[i] = (uint8_t)(i * 17 + 0x42);
        sec_hash_password(password, dummy_salt, hash_buf);
        return NULL;
    }
    if (u->flags & SEC_UFLAG_DISABLED) return NULL;

    if (sec_const_time_eq(hash_buf, u->hash, SEC_HASH_LEN)) return u;
    return NULL;
}

// ----------------------------------------------------------------------------
// Создание пользователя
// ----------------------------------------------------------------------------

int sec_users_create(const char* name, const char* display,
                     const char* password, bool is_admin) {
    if (!name || !password || !display) return -1;
    if (s_strlen(name) == 0 || s_strlen(name) > SEC_USERNAME_MAX) return -1;
    if (s_strlen(password) == 0 || s_strlen(password) > SEC_PASSWORD_MAX) return -1;
    if (g_users_count >= SEC_MAX_USERS) return -2;
    if (sec_users_find_by_name(name) != NULL) return -3;

    sec_user_t* u = &g_users[g_users_count];
    memset(u, 0, sizeof(*u));
    u->magic = 0x554F4F41u; // 'AOSU'
    u->uid   = is_admin && g_users_count == 0 ? SEC_UID_ROOT : g_next_uid++;
    u->gid   = is_admin && g_users_count == 0 ? SEC_GID_ROOT : SEC_GID_USERS;
    u->flags = is_admin ? SEC_UFLAG_ADMIN : 0;
    s_strncpy(u->name,    name,    SEC_USERNAME_MAX + 1);
    s_strncpy(u->display, display, SEC_DISPLAYNAME_MAX + 1);

    sec_make_salt(u->salt);
    sec_hash_password(password, u->salt, u->hash);

    g_users_count++;

    // Пытаемся сохранить на диск — но даже если не получилось,
    // пользователь существует в памяти и логин будет работать в этом сеансе.
    if (security_disk_available()) {
        sec_users_save();
    }
    return 0;
}

// ============================================================================
// Сохранение и загрузка БД на FAT32 (через VFS)
// ============================================================================
// Основной путь: /disk/Sanctum/users.db (если каталог удалось создать).
// Fallback:      /disk/users.db          (корень FAT32, всегда доступен).
// При загрузке пробуем оба пути.
// ============================================================================

#define SEC_DB_PATH_PRIMARY  "/disk/Sanctum/users.db"
#define SEC_DB_PATH_FALLBACK "/disk/users.db"

// Пытается выбрать путь сохранения, предпочитая каталог Sanctum.
static const char* pick_db_save_path(void) {
    vfs_stat_info_t st;
    if (vfs_stat("/disk/Sanctum", &st) == 0) {
        return SEC_DB_PATH_PRIMARY;
    }
    return SEC_DB_PATH_FALLBACK;
}

int sec_users_save(void) {
    if (!security_disk_available()) return -1;

    const char* path = pick_db_save_path();
    dbg_puts("[sec] users_save: path=");
    dbg_puts(path);
    dbg_puts("\n");

    // Открываем (или создаём + truncate) файл.
    int fd = vfs_open_file(path,
                           VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        dbg_puts("[sec] users_save: cannot open users.db for write\n");
        return -2;
    }

    sec_db_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic      = SEC_DB_MAGIC;
    hdr.version    = SEC_DB_VERSION;
    hdr.user_count = (uint32_t)g_users_count;

    int n = vfs_write_file(fd, &hdr, sizeof(hdr));
    if (n != (int)sizeof(hdr)) {
        vfs_close_file(fd);
        dbg_puts("[sec] users_save: header write short\n");
        return -3;
    }

    for (int i = 0; i < g_users_count; i++) {
        n = vfs_write_file(fd, &g_users[i], sizeof(g_users[i]));
        if (n != (int)sizeof(g_users[i])) {
            vfs_close_file(fd);
            dbg_puts("[sec] users_save: record write short\n");
            return -4;
        }
    }

    vfs_close_file(fd);
    dbg_puts("[sec] users_save: OK\n");
    return 0;
}

int sec_users_load(void) {
    // Сначала пробуем основной путь, затем fallback.
    int fd = vfs_open_file(SEC_DB_PATH_PRIMARY, VFS_O_RDONLY);
    if (fd < 0) fd = vfs_open_file(SEC_DB_PATH_FALLBACK, VFS_O_RDONLY);
    if (fd < 0) return -1;     // файла нет — это первый запуск

    sec_db_header_t hdr;
    int n = vfs_read_file(fd, &hdr, sizeof(hdr));
    if (n != (int)sizeof(hdr) ||
        hdr.magic != SEC_DB_MAGIC ||
        hdr.version != SEC_DB_VERSION) {
        vfs_close_file(fd);
        dbg_puts("[sec] users_load: bad header — ignoring\n");
        return -2;
    }

    if (hdr.user_count > SEC_MAX_USERS) hdr.user_count = SEC_MAX_USERS;

    g_users_count = 0;
    g_next_uid    = 1000;
    for (uint32_t i = 0; i < hdr.user_count; i++) {
        sec_user_t* u = &g_users[g_users_count];
        n = vfs_read_file(fd, u, sizeof(*u));
        if (n != (int)sizeof(*u)) {
            dbg_puts("[sec] users_load: record short — stop\n");
            break;
        }
        g_users_count++;
        if (u->uid >= g_next_uid) g_next_uid = u->uid + 1;
    }
    vfs_close_file(fd);

    dbg_puts("[sec] users_load: OK\n");
    return 0;
}

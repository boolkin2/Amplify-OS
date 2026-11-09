// ============================================================================
// fat32.c — полнофункциональный FAT32 driver для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 45: расширен с минимальной (root + 8.3) до полнофункциональной версии.
// Поддерживает:
//
//   * Произвольные подкаталоги (path = "/disk/Sanctum/Amply/db.txt").
//   * Long File Names (LFN) — серия 0x0F-attr записей с UCS-2/UTF-16LE,
//     наша версия пишет/читает LFN, состоящие из кодов CP1251 (т.е. сразу
//     совместимо со строками, скомпилированными -fexec-charset=CP1251).
//   * Авто-расширение цепочки кластеров на write.
//   * mkdir, mkdir -p, удаление пустых каталогов.
//   * Итератор каталога fat32_iter_dir.
//
// Стратегия LFN: в FAT32 LFN — это серия 32-байтовых записей с attr=0x0F,
// каждая хранит 13 UCS-2 символов (5 + 6 + 2).  Перед короткой 8.3-записью
// идут LFN-записи в обратном порядке (последняя — с битом 0x40 в order).
// Контрольная сумма привязана к 8.3-имени: сумма с rotate-1 по 11 байтам.
//
// Для имени, не помещающегося в 8.3 (или содержащего нижний регистр /
// non-ASCII), мы:
//   1) генерируем 8.3 alias из первых 6 chars + "~N" (N=1..99);
//   2) пишем LFN-записи + 8.3-запись с этим alias.
// При чтении: если идёт серия LFN перед 8.3, собираем имя из неё; иначе —
// из 8.3.
//
// Поскольку наш userspace и kernel-консоль используют CP1251 (один байт на
// символ кириллицы), мы маппим UCS-2 ↔ CP1251 один-к-одному в утилитах
// utf16le_to_local() / local_to_utf16le().  Latin-1/CP1251 кодпойнты лежат
// прямо в low byte, то есть для нашей сборки преобразование тривиальное.
// ============================================================================
#include "fat32.h"
#include "disk_io.h"
#include "ata.h"
#include "partition.h"
#include "vfs.h"
#include "heap.h"
#include "memory.h"
#include "serial.h"

// ----------------------------------------------------------------------------
// Состояние смонтированной FAT32.
// ----------------------------------------------------------------------------
static fat32_bpb_t g_bpb;
static bool        g_mounted = false;
static uint64_t    g_first_data_sector;
static uint64_t    g_cluster_size_bytes;

// Буфер на один кластер (используется readers/writers последовательно;
// все операции — однопоточные на текущем кооперативном шедулере).
static uint8_t*    g_cluster_buf = 0;

// Кеш одного сектора FAT.
static uint8_t     g_fat_cache[512];
static uint64_t    g_fat_cache_lba = 0;
static bool        g_fat_cache_valid = false;

// [этап 49] Кэш позиции в цепочке кластеров: «файл (первый кластер) ->
// номер кластера -> кластер». fat32_read_file_at раньше при каждом вызове
// шёл по цепочке FAT с начала файла; для весов MIND (сотни МБ, чтение
// кусками по 1 МБ) это квадратичная работа. Цепочка меняется только через
// fat_set_entry, при монтировании и отвязке тома — там кэш и сбрасывается.
static uint32_t    g_seek_first;
static uint64_t    g_seek_index;
static uint32_t    g_seek_cluster;
static bool        g_seek_valid = false;

static void fat_caches_drop(void) {
    g_fat_cache_valid = false;
    g_seek_valid = false;
}

// ----------------------------------------------------------------------------
// [этап 57] Привязка тома: (устройство, LBA начала раздела).
// ----------------------------------------------------------------------------
// Раньше драйвер обращался к диску по абсолютным LBA, что работало только
// для dev-образа с FAT32 в секторе 0 на default-устройстве.  Теперь все
// внутренние обращения идут через vol_read/vol_write, которые прибавляют
// базу раздела — это позволяет установщику примонтировать свежий раздел на
// целевом диске и наполнить его тем же кодом (LFN, цепочки кластеров).
static int      g_vol_dev  = -1;
static uint64_t g_vol_base = 0;

static bool vol_read(uint64_t sector, uint64_t count, void* buf) {
    return disk_read_sectors_dev(g_vol_dev, g_vol_base + sector, count, buf);
}
static bool vol_write(uint64_t sector, uint64_t count, const void* buf) {
    return disk_write_sectors_dev(g_vol_dev, g_vol_base + sector, count, buf);
}
// Сброс кеша записи — один раз в конце каждой изменяющей публичной операции
// (не после каждого сектора: см. комментарий в ata.c:pio_transfer).
static void vol_flush(void) {
    if (g_vol_dev >= 0) disk_flush_dev(g_vol_dev);
}

// true — файлы тома светятся в VFS как /disk/...  Для тома, который
// установщик наполняет на целевом диске, это false: иначе в живой системе
// остались бы узлы /disk/..., указывающие на чужой раздел.
static bool g_vfs_attached = false;

// ----------------------------------------------------------------------------
// Минимальные string-helpers (kernel-mode, без libc).
// ----------------------------------------------------------------------------
static int s_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static int s_strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}
static void s_memcpy(void* d, const void* s, uint64_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (uint64_t i = 0; i < n; i++) dd[i] = ss[i];
}
static void s_memset(void* d, int v, uint64_t n) {
    uint8_t* dd = (uint8_t*)d;
    for (uint64_t i = 0; i < n; i++) dd[i] = (uint8_t)v;
}

// ----------------------------------------------------------------------------
// Helpers геометрии.
// ----------------------------------------------------------------------------
static uint64_t cluster_to_sector(uint32_t cluster) {
    return g_first_data_sector + (uint64_t)(cluster - 2) * g_bpb.sectors_per_cluster;
}

// Прочитать один FAT-entry (32 бита, маска 0x0FFFFFFF).
// Возвращает next-cluster (≥0x0FFFFFF8 = end-of-chain).
static uint32_t fat_next_cluster(uint32_t cluster) {
    uint64_t fat_offset_bytes = (uint64_t)cluster * 4;
    uint64_t fat_sector = (uint64_t)g_bpb.reserved_sectors + fat_offset_bytes / 512;
    uint32_t entry_offset = (uint32_t)(fat_offset_bytes % 512);

    if (!g_fat_cache_valid || g_fat_cache_lba != fat_sector) {
        if (!vol_read(fat_sector, 1, g_fat_cache)) return 0x0FFFFFFF;
        g_fat_cache_lba = fat_sector;
        g_fat_cache_valid = true;
    }
    uint32_t v = *(uint32_t*)(g_fat_cache + entry_offset);
    return v & 0x0FFFFFFF;
}

// Записать FAT-entry (во все копии FAT).
static bool fat_set_entry(uint32_t cluster, uint32_t value) {
    uint64_t fat_offset_bytes = (uint64_t)cluster * 4;
    uint64_t fat_sector = (uint64_t)g_bpb.reserved_sectors + fat_offset_bytes / 512;
    uint32_t entry_offset = (uint32_t)(fat_offset_bytes % 512);

    uint8_t sec[512];
    if (!vol_read(fat_sector, 1, sec)) return false;
    uint32_t* ep = (uint32_t*)(sec + entry_offset);
    *ep = (*ep & 0xF0000000u) | (value & 0x0FFFFFFFu);

    for (uint8_t f = 0; f < g_bpb.num_fats; f++) {
        uint64_t dst = fat_sector + (uint64_t)f * g_bpb.fat_size_32;
        if (!vol_write(dst, 1, sec)) return false;
    }
    fat_caches_drop();
    return true;
}

// Число адресуемых FAT-записей = кластеры тома + 2 служебные.  Таблица FAT
// почти всегда длиннее (её размер округлён вверх до сектора и считается с
// запасом), и хвостовые записи НЕ соответствуют реальным кластерам: кластер
// оттуда лёг бы за границей раздела — на соседний раздел диска.
static uint32_t fat_total_entries(void) {
    uint32_t by_fat = g_bpb.fat_size_32 * (512u / 4u);
    uint64_t total  = g_bpb.total_sectors_32 ? g_bpb.total_sectors_32
                                             : g_bpb.total_sectors_16;
    if (g_bpb.sectors_per_cluster && total > g_first_data_sector) {
        uint64_t entries = (total - g_first_data_sector) / g_bpb.sectors_per_cluster + 2;
        if (entries < by_fat) return (uint32_t)entries;
    }
    return by_fat;
}

// Найти свободный кластер.
static uint32_t fat_find_free_cluster(uint32_t start_hint) {
    uint32_t total = fat_total_entries();
    if (start_hint < 2) start_hint = 2;
    for (uint32_t c = start_hint; c < total; c++) {
        if (fat_next_cluster(c) == 0) return c;
    }
    for (uint32_t c = 2; c < start_hint; c++) {
        if (fat_next_cluster(c) == 0) return c;
    }
    return 0;
}

// Освободить цепочку.
static void fat_free_chain(uint32_t first) {
    uint32_t c = first;
    while (c >= 2 && c < 0x0FFFFFF8) {
        uint32_t next = fat_next_cluster(c);
        fat_set_entry(c, 0);
        c = next;
    }
}

// Обнулить только что выделенный кластер (важно для каталогов).
static bool zero_cluster(uint32_t cluster) {
    s_memset(g_cluster_buf, 0, g_cluster_size_bytes);
    return vol_write(cluster_to_sector(cluster),
                              g_bpb.sectors_per_cluster, g_cluster_buf);
}

// Расширить цепочку на 1 кластер.  Возвращает номер нового кластера или 0.
static uint32_t fat_extend_chain(uint32_t last_cluster) {
    uint32_t hint = last_cluster ? last_cluster + 1 : 2;
    uint32_t c = fat_find_free_cluster(hint);
    if (c == 0) return 0;
    if (!fat_set_entry(c, 0x0FFFFFFFu)) return 0;
    if (last_cluster) {
        if (!fat_set_entry(last_cluster, c)) {
            fat_set_entry(c, 0);
            return 0;
        }
    }
    return c;
}

// ----------------------------------------------------------------------------
// 8.3 имена + UTF-16 (UCS-2) ↔ local (CP1251) для LFN.
// ----------------------------------------------------------------------------

// Допустим ли символ в 8.3 без LFN-fallback?  Только uppercase ASCII / digits /
// '_' / '~' / '$'.  Любая русская/нижняя буква, пробел, длина — выгоняют в LFN.
static bool is_short_name_char(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '_' || c == '~' || c == '$' || c == '!' || c == '#' || c == '&' ||
        c == '\'' || c == '(' || c == ')' || c == '-' || c == '@' || c == '^' ||
        c == '`' || c == '{' || c == '}' || c == '~') return true;
    return false;
}

// Решить, нужно ли использовать LFN для имени.
static bool name_needs_lfn(const char* in) {
    int dot = -1;
    int len = 0;
    for (int i = 0; in[i]; i++) {
        len++;
        if (in[i] == '.') dot = i;
    }
    int basename_len = (dot >= 0) ? dot : len;
    int ext_len      = (dot >= 0) ? (len - dot - 1) : 0;
    if (basename_len == 0 || basename_len > 8) return true;
    if (ext_len > 3) return true;
    for (int i = 0; in[i]; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '.') continue;
        if (c >= 'a' && c <= 'z') return true;     // нижний регистр → LFN
        if (c >= 0x80) return true;                // не-ASCII → LFN
        if (!is_short_name_char(c)) return true;
    }
    return false;
}

// Сформировать 8.3 (uppercase, 11 байт padded ' ') из строки.  Если имя
// уже подходит, оно идёт как есть.  Иначе генерится "BASNAM~N.EXT".
static void to_short_name(const char* in, char out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, o = 0;
    while (in[i] && in[i] != '.' && o < 8) {
        char c = in[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        // Любой запрещённый символ заменяем на '_'.
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) c = '_';
        if (!is_short_name_char((unsigned char)c)) c = '_';
        out[o++] = c;
    }
    while (in[i] && in[i] != '.') i++;
    if (in[i] == '.') i++;
    int eo = 8;
    while (in[i] && eo < 11) {
        char c = in[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        if ((unsigned char)c < 0x20 || (unsigned char)c == 0x7F) c = '_';
        if (!is_short_name_char((unsigned char)c)) c = '_';
        out[eo++] = c;
    }
}

// 11-байтовое 8.3 → "HELLO.TXT".
static void name_from_83(const char dirname[11], char* out) {
    int o = 0;
    for (int i = 0; i < 8 && dirname[i] != ' '; i++) out[o++] = dirname[i];
    bool has_ext = false;
    for (int i = 8; i < 11; i++) if (dirname[i] != ' ') { has_ext = true; break; }
    if (has_ext) {
        out[o++] = '.';
        for (int i = 8; i < 11 && dirname[i] != ' '; i++) out[o++] = dirname[i];
    }
    out[o] = 0;
}

// Контрольная сумма 8.3 имени (для LFN-записей).
static uint8_t lfn_checksum(const char short83[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + (uint8_t)short83[i]);
    return sum;
}

// LFN-запись хранит 13 UCS-2 символов в полях name1[5], name2[6], name3[2].
typedef struct __attribute__((packed)) {
    uint8_t  order;            // sequence, 0x40 set on last
    uint16_t name1[5];
    uint8_t  attr;             // = 0x0F
    uint8_t  type;             // = 0
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t cluster_low;      // = 0
    uint16_t name3[2];
} fat32_lfn_t;

// CP1251/Latin-1 → UCS-2: байт без преобразования (в текущей сборке этого
// достаточно, т.к. CP1251 = 1 байт = единственный код-пойнт).
static uint16_t local_to_ucs2(unsigned char c) {
    return (uint16_t)c;
}
// UCS-2 → CP1251: при превышении 0xFF фолбэк '?'.
static char ucs2_to_local(uint16_t u) {
    return (u > 0xFF) ? '?' : (char)u;
}

// ----------------------------------------------------------------------------
// Чтение содержимого файла по cluster chain.
// ----------------------------------------------------------------------------
static int64_t read_cluster_chain(uint32_t first_cluster, uint64_t file_size,
                                  void* buf, uint64_t off, uint64_t len) {
    if (off >= file_size) return 0;
    if (off + len > file_size) len = file_size - off;
    uint64_t target = len;

    uint32_t cluster = first_cluster;
    uint64_t cluster_index = off / g_cluster_size_bytes;
    uint64_t k = 0;
    if (g_seek_valid && g_seek_first == first_cluster && g_seek_index <= cluster_index) {
        cluster = g_seek_cluster;
        k = g_seek_index;
    }
    for (; k < cluster_index; k++) {
        cluster = fat_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8) return -1;
    }
    uint64_t in_cluster_off = off % g_cluster_size_bytes;
    uint64_t copied = 0;
    uint64_t cur_index = cluster_index;       // номер кластера в переменной cluster

    while (cluster < 0x0FFFFFF8 && copied < target) {
        // [этап 49] Быстрый путь для больших файлов (веса AMPLIFY MIND —
        // сотни МБ): целые кластеры, лежащие на диске подряд, читаются ОДНОЙ
        // ATA-командой прямо в буфер вызывающего, без g_cluster_buf. Раньше
        // была команда на каждый кластер (4 КБ) — накладные расходы PIO
        // (выбор устройства, регистры, ожидание BSY) съедали пропускную
        // способность. 32 кластера по 8 секторов = 256 секторов = предел LBA28.
        if (in_cluster_off == 0 && target - copied >= g_cluster_size_bytes) {
            uint32_t run = 1;
            uint32_t next = fat_next_cluster(cluster);
            while (next == cluster + run && run < 32 &&
                   (uint64_t)(run + 1) * g_bpb.sectors_per_cluster <= 256 &&
                   (uint64_t)(run + 1) * g_cluster_size_bytes <= target - copied) {
                run++;
                next = fat_next_cluster(next);
            }
            if (!vol_read(cluster_to_sector(cluster), (uint64_t)run * g_bpb.sectors_per_cluster,
                          (uint8_t*)buf + copied)) return -1;
            copied += (uint64_t)run * g_cluster_size_bytes;
            cluster = next;
            cur_index += run;
            continue;
        }
        uint64_t sector = cluster_to_sector(cluster);
        if (!vol_read(sector, g_bpb.sectors_per_cluster, g_cluster_buf)) return -1;
        uint64_t avail = g_cluster_size_bytes - in_cluster_off;
        if (avail > target - copied) avail = target - copied;
        s_memcpy((uint8_t*)buf + copied, g_cluster_buf + in_cluster_off, avail);
        copied += avail;
        in_cluster_off = 0;
        cluster = fat_next_cluster(cluster);
        cur_index++;
    }
    // Запоминаем, где остановились: следующее чтение того же файла дальше
    // по смещению продолжит отсюда, а не с начала цепочки.
    if (first_cluster >= 2 && cluster >= 2 && cluster < 0x0FFFFFF8) {
        g_seek_first = first_cluster;
        g_seek_index = cur_index;
        g_seek_cluster = cluster;
        g_seek_valid = true;
    }
    return (int64_t)copied;
}

// ----------------------------------------------------------------------------
// Запись цепочки.  Авто-расширяет.  Возвращает количество записанных байт
// или -1.  Если new_size > file_size, цепочка удлиняется; если меньше,
// лишние кластеры освобождаются.
// ----------------------------------------------------------------------------
static int64_t write_cluster_chain_resize(uint32_t* first_cluster_io,
                                          uint64_t old_size, uint64_t new_size,
                                          const void* data) {
    uint32_t first_cluster = *first_cluster_io;
    (void)old_size;

    // Если файл был пуст (cluster=0) и new_size>0 — выделяем первый кластер.
    if (first_cluster == 0 && new_size > 0) {
        uint32_t c = fat_find_free_cluster(2);
        if (!c) return -1;
        if (!fat_set_entry(c, 0x0FFFFFFFu)) return -1;
        first_cluster = c;
        *first_cluster_io = c;
    }

    if (new_size == 0) {
        if (first_cluster) fat_free_chain(first_cluster);
        *first_cluster_io = 0;
        return 0;
    }

    // Сколько кластеров нужно?
    uint64_t need = (new_size + g_cluster_size_bytes - 1) / g_cluster_size_bytes;

    // Идём по цепочке, по дороге доступаем "хвост" если нужно;
    // если цепочка слишком длинная — обрезаем.
    uint32_t prev = 0;
    uint32_t cluster = first_cluster;
    uint64_t copied = 0;
    const uint8_t* src = (const uint8_t*)data;

    for (uint64_t k = 0; k < need; k++) {
        if (cluster >= 0x0FFFFFF8 || cluster < 2) {
            // Нужно расширить.
            uint32_t nc = fat_extend_chain(prev);
            if (!nc) return -1;
            cluster = nc;
        }
        uint64_t sector = cluster_to_sector(cluster);
        uint64_t chunk = g_cluster_size_bytes;
        if (copied + chunk > new_size) chunk = new_size - copied;
        for (uint64_t i = 0; i < chunk; i++) g_cluster_buf[i] = src[copied + i];
        for (uint64_t i = chunk; i < g_cluster_size_bytes; i++) g_cluster_buf[i] = 0;
        if (!vol_write(sector, g_bpb.sectors_per_cluster, g_cluster_buf)) return -1;
        copied += chunk;
        prev = cluster;
        cluster = fat_next_cluster(cluster);
    }

    // Если осталась лишняя часть цепочки — обрезаем.
    if (cluster >= 2 && cluster < 0x0FFFFFF8) {
        fat_free_chain(cluster);
        if (prev) fat_set_entry(prev, 0x0FFFFFFFu);
    }
    return (int64_t)copied;
}

// ----------------------------------------------------------------------------
// Поиск/создание dirent в произвольной директории (по cluster).
// ----------------------------------------------------------------------------

// Структура DirSlot — указывает, где в директории лежит конкретный
// логический dirent (8.3 + потенциальные LFN-записи перед ним).
typedef struct {
    uint32_t dir_first_cluster;   // first cluster директории
    uint64_t sector_lba;          // абсолютный LBA сектора с 8.3-записью
    uint32_t cluster;              // в каком кластере она лежит
    uint32_t entry_in_cluster;     // 32-байтовый индекс
    uint32_t lfn_count;            // сколько LFN-записей предшествует
    uint32_t lfn_first_index;      // первый индекс LFN-серии
} dir_slot_t;

// Построитель имени из последовательности LFN-записей.
typedef struct {
    char buf[FAT32_LONG_NAME_MAX + 1];
    int  pos;
    uint8_t expected_checksum;
    bool valid;
} lfn_collector_t;

static void lfn_collector_reset(lfn_collector_t* lc) {
    lc->pos = 0;
    lc->valid = false;
    lc->expected_checksum = 0;
    lc->buf[0] = 0;
}

// Принимаем LFN-запись, возвращаем true если серия пока ОК.
static bool lfn_collector_feed(lfn_collector_t* lc, const fat32_lfn_t* l) {
    uint8_t order = l->order & 0x3F;
    bool last = (l->order & 0x40) != 0;
    if (last) {
        // Начало новой серии — order — это total count записей.
        s_memset(lc->buf, 0, sizeof(lc->buf));
        lc->pos = 0;
        lc->expected_checksum = l->checksum;
        lc->valid = true;
    }
    if (!lc->valid) return false;
    if (lc->expected_checksum != l->checksum) { lc->valid = false; return false; }

    // Записать 13 ucs2-символов в позицию (order-1)*13.
    int base = ((int)order - 1) * 13;
    if (base < 0 || base + 13 > FAT32_LONG_NAME_MAX) { lc->valid = false; return false; }

    uint16_t chars[13];
    for (int i = 0; i < 5; i++) chars[i]      = l->name1[i];
    for (int i = 0; i < 6; i++) chars[5 + i]  = l->name2[i];
    for (int i = 0; i < 2; i++) chars[11 + i] = l->name3[i];

    int written_now = 0;
    for (int i = 0; i < 13; i++) {
        uint16_t u = chars[i];
        if (u == 0x0000 || u == 0xFFFF) break;
        lc->buf[base + i] = ucs2_to_local(u);
        written_now = i + 1;
    }
    if (base + written_now > lc->pos) lc->pos = base + written_now;
    lc->buf[lc->pos] = 0;
    return true;
}

// Прочитать кластер директории в g_cluster_buf и проверить валидность.
static bool read_dir_cluster(uint32_t cluster) {
    return vol_read(cluster_to_sector(cluster),
                             g_bpb.sectors_per_cluster, g_cluster_buf);
}

// Записать обратно.
static bool write_dir_cluster(uint32_t cluster) {
    return vol_write(cluster_to_sector(cluster),
                              g_bpb.sectors_per_cluster, g_cluster_buf);
}

// Итератор: проходит по кластеру директории, для каждой логической записи
// (8.3 + опционально LFN-серия) вызывает callback.  cb возвращает 0 для
// продолжения, не-0 для останова (значение возвращается наружу).
typedef int (*walk_cb_t)(const fat32_dirent_t* de, const char* long_name,
                         dir_slot_t* slot, void* user);

static int walk_dir_clusters(uint32_t dir_first_cluster, walk_cb_t cb, void* user) {
    if (dir_first_cluster < 2) return -1;
    uint32_t cluster = dir_first_cluster;
    uint32_t entries_per_cluster = (uint32_t)(g_cluster_size_bytes / sizeof(fat32_dirent_t));

    lfn_collector_t lc; lfn_collector_reset(&lc);
    uint32_t lfn_first_index = 0;
    uint32_t lfn_count = 0;

    while (cluster < 0x0FFFFFF8) {
        if (!read_dir_cluster(cluster)) return -1;
        fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;

        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first = (uint8_t)d[i].name[0];
            if (first == 0x00) {
                return 0;     // конец директории
            }
            if (first == 0xE5) {
                lfn_collector_reset(&lc);
                lfn_count = 0;
                continue;
            }
            // LFN-запись?
            if ((d[i].attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                if (lfn_count == 0) lfn_first_index = i;
                lfn_count++;
                lfn_collector_feed(&lc, (fat32_lfn_t*)&d[i]);
                continue;
            }
            // Обычная запись (8.3).  Если перед ней была валидная LFN-серия
            // и checksum совпадает, используем длинное имя.
            char long_name[FAT32_LONG_NAME_MAX + 1];
            const char* lname_ptr = NULL;
            if (lc.valid && lc.expected_checksum == lfn_checksum(d[i].name)) {
                for (int k = 0; k <= lc.pos && k <= FAT32_LONG_NAME_MAX; k++)
                    long_name[k] = lc.buf[k];
                lname_ptr = long_name;
            }

            dir_slot_t slot = {
                .dir_first_cluster = dir_first_cluster,
                .sector_lba = cluster_to_sector(cluster),
                .cluster = cluster,
                .entry_in_cluster = i,
                .lfn_count = (lname_ptr ? lfn_count : 0),
                .lfn_first_index = (lname_ptr ? lfn_first_index : 0),
            };

            int r = cb(&d[i], lname_ptr, &slot, user);
            if (r != 0) return r;

            lfn_collector_reset(&lc);
            lfn_count = 0;
        }
        cluster = fat_next_cluster(cluster);
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Поиск по имени в директории.
// ----------------------------------------------------------------------------
typedef struct {
    const char*    name;
    fat32_dirent_t found;
    dir_slot_t     slot;
    bool           hit;
} find_ctx_t;

static int find_cb(const fat32_dirent_t* de, const char* long_name,
                   dir_slot_t* slot, void* user) {
    find_ctx_t* ctx = (find_ctx_t*)user;
    if (de->attr & FAT32_ATTR_VOLLABEL) return 0;     // метка тома — не файл
    char short_name[16]; name_from_83(de->name, short_name);

    if (long_name && s_strcasecmp(long_name, ctx->name) == 0) {
        ctx->found = *de; ctx->slot = *slot; ctx->hit = true; return 1;
    }
    if (s_strcasecmp(short_name, ctx->name) == 0) {
        ctx->found = *de; ctx->slot = *slot; ctx->hit = true; return 1;
    }
    return 0;
}

static bool dir_find_entry(uint32_t dir_first_cluster, const char* name,
                           fat32_dirent_t* out_de, dir_slot_t* out_slot) {
    find_ctx_t ctx = { .name = name, .hit = false };
    walk_dir_clusters(dir_first_cluster, find_cb, &ctx);
    if (!ctx.hit) return false;
    if (out_de) *out_de = ctx.found;
    if (out_slot) *out_slot = ctx.slot;
    return true;
}

// ----------------------------------------------------------------------------
// Path resolution: "/disk/Sanctum/Amply/db.txt" → (dir_cluster, basename).
// Проходим по компонентам, начиная с root.
// ----------------------------------------------------------------------------

// Снять префикс "/disk/" если есть.
static const char* strip_disk_prefix(const char* path) {
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'i' && path[3] == 's' &&
        path[4] == 'k' && (path[5] == '/' || path[5] == 0)) return path + 5;
    return path;
}

// Найти директорию-родителя для path.  *out_dir_cluster — first cluster
// родительского каталога.  *out_basename — указатель на последний компонент
// (хранится в path, не копируется!).  Возвращает true если родитель найден.
static bool resolve_parent(const char* path, uint32_t* out_dir_cluster,
                           const char** out_basename) {
    const char* p = strip_disk_prefix(path);
    if (*p == '/') p++;   // лишний слэш

    uint32_t cur = g_bpb.root_cluster;

    // Найдём последний '/' в пути (от p).
    const char* last_slash = 0;
    for (const char* q = p; *q; q++) if (*q == '/') last_slash = q;
    if (!last_slash) {
        *out_dir_cluster = cur;
        *out_basename = p;
        return true;
    }

    // Нужно пройти все компоненты до last_slash.
    char comp[FAT32_LONG_NAME_MAX + 1];
    const char* cur_p = p;
    while (cur_p < last_slash) {
        int n = 0;
        while (cur_p < last_slash && *cur_p != '/' && n < FAT32_LONG_NAME_MAX) {
            comp[n++] = *cur_p++;
        }
        comp[n] = 0;
        if (cur_p < last_slash && *cur_p == '/') cur_p++;
        if (n == 0) continue;

        fat32_dirent_t de;
        if (!dir_find_entry(cur, comp, &de, 0)) return false;
        if (!(de.attr & FAT32_ATTR_DIR)) return false;
        cur = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
        if (cur < 2) return false;
    }
    *out_dir_cluster = cur;
    *out_basename = last_slash + 1;
    return true;
}

// Резолв полного пути → directory entry.  out_de + out_slot, опц.
static bool resolve_path(const char* path, fat32_dirent_t* out_de,
                         dir_slot_t* out_slot, uint32_t* out_parent_cluster,
                         const char** out_basename) {
    uint32_t parent;
    const char* base;
    if (!resolve_parent(path, &parent, &base)) return false;
    if (out_parent_cluster) *out_parent_cluster = parent;
    if (out_basename)       *out_basename       = base;
    if (*base == 0) {
        // "/disk/" — корень, нет dirent.
        return false;
    }
    return dir_find_entry(parent, base, out_de, out_slot);
}

// ----------------------------------------------------------------------------
// Запись dirent + LFN в директорию.  Если в текущей цепочке нет свободного
// места — расширяем кластер директории.
// ----------------------------------------------------------------------------

// Сколько dirent-слотов нужно для имени (1 для 8.3 + по 13 chars на LFN).
static int slots_needed(const char* long_name) {
    if (!name_needs_lfn(long_name)) return 1;
    int len = s_strlen(long_name);
    int lfn_count = (len + 12) / 13;
    return lfn_count + 1;
}

// Найти N подряд идущих свободных слотов (0x00 или 0xE5) в директории.
// Если не нашли — расширяем директорию на 1 кластер.  Возвращает (cluster,
// entry_in_cluster) первого слота.
typedef struct {
    uint32_t cluster;
    uint32_t entry_in_cluster;
} slot_pos_t;

static bool dir_alloc_slots(uint32_t dir_first_cluster, int need_slots,
                            slot_pos_t* out_pos) {
    uint32_t entries_per_cluster = (uint32_t)(g_cluster_size_bytes / sizeof(fat32_dirent_t));

    uint32_t cluster = dir_first_cluster;
    uint32_t prev = 0;
    int run = 0;
    slot_pos_t run_start = {0,0};

    while (cluster < 0x0FFFFFF8) {
        if (!read_dir_cluster(cluster)) return false;
        fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;
        for (uint32_t i = 0; i < entries_per_cluster; i++) {
            uint8_t first = (uint8_t)d[i].name[0];
            if (first == 0x00 || first == 0xE5) {
                if (run == 0) { run_start.cluster = cluster; run_start.entry_in_cluster = i; }
                run++;
                if (run >= need_slots) { *out_pos = run_start; return true; }
                if (first == 0x00) {
                    // Можно ли удлинить с этого места до конца кластера?
                    // Если до конца кластера + 1 кластер хватит — расширим.
                    // Иначе просто продолжим и расширим явно ниже.
                    continue;
                }
            } else {
                run = 0;
            }
        }
        prev = cluster;
        cluster = fat_next_cluster(cluster);
    }

    // Не хватает места — расширяем.
    uint32_t nc = fat_extend_chain(prev);
    if (!nc) return false;
    if (!zero_cluster(nc)) return false;

    if (run > 0) {
        // Серия начиналась в prev-кластере; продолжается в nc.
        *out_pos = run_start;
    } else {
        out_pos->cluster = nc;
        out_pos->entry_in_cluster = 0;
    }
    return true;
}

// Сгенерировать уникальный 8.3 alias для длинного имени: BASNAM~N + .EXT
static void make_short_alias(uint32_t parent_cluster, const char* long_name,
                             char out83[11]) {
    // Извлечь base + ext.
    char base[16] = {0};
    char ext[8]   = {0};
    int dot = -1;
    int n = 0;
    for (int i = 0; long_name[i]; i++) { if (long_name[i] == '.') dot = i; n++; }
    if (dot < 0) {
        for (int i = 0; i < 6 && long_name[i]; i++) {
            char c = long_name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!is_short_name_char((unsigned char)c)) c = '_';
            base[i] = c;
        }
    } else {
        for (int i = 0; i < dot && i < 6; i++) {
            char c = long_name[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!is_short_name_char((unsigned char)c)) c = '_';
            base[i] = c;
        }
        for (int i = 0; i < 3 && long_name[dot + 1 + i]; i++) {
            char c = long_name[dot + 1 + i];
            if (c >= 'a' && c <= 'z') c -= 32;
            if (!is_short_name_char((unsigned char)c)) c = '_';
            ext[i] = c;
        }
    }
    if (base[0] == 0) base[0] = '_';

    // Подбираем суффикс ~1, ~2, ..., пока не уникален.
    for (int n_suf = 1; n_suf <= 99; n_suf++) {
        char tmp[11];
        for (int i = 0; i < 11; i++) tmp[i] = ' ';
        // base + "~N"
        int bl = 0; while (bl < 6 && base[bl]) { tmp[bl] = base[bl]; bl++; }
        tmp[bl++] = '~';
        if (n_suf < 10) {
            tmp[bl++] = (char)('0' + n_suf);
        } else {
            tmp[bl++] = (char)('0' + n_suf / 10);
            tmp[bl++] = (char)('0' + n_suf % 10);
        }
        for (int i = 0; i < 3 && ext[i]; i++) tmp[8 + i] = ext[i];

        // Проверим в директории.
        char as_str[16]; name_from_83(tmp, as_str);
        fat32_dirent_t de;
        if (!dir_find_entry(parent_cluster, as_str, &de, 0)) {
            for (int i = 0; i < 11; i++) out83[i] = tmp[i];
            return;
        }
    }
    // Очень мало шансов; берём ~99.
    for (int i = 0; i < 11; i++) out83[i] = ' ';
    int bl = 0; while (bl < 5 && base[bl]) { out83[bl] = base[bl]; bl++; }
    out83[bl++] = '~'; out83[bl++] = '9'; out83[bl++] = '9';
    for (int i = 0; i < 3 && ext[i]; i++) out83[8 + i] = ext[i];
}

// Записать N dirent-слотов начиная с (cluster, entry).  Сначала LFN
// (в обратном порядке), затем 8.3.
static bool write_lfn_chain(slot_pos_t pos, const char* long_name,
                            const fat32_dirent_t* main_de) {
    int total_slots = slots_needed(long_name);
    int lfn_count = total_slots - 1;

    char short83[11];
    for (int i = 0; i < 11; i++) short83[i] = main_de->name[i];
    uint8_t cs = lfn_checksum(short83);

    // Соберём массив 13-character LFN-блоков.
    int len = s_strlen(long_name);
    int padded = (lfn_count) * 13;

    fat32_lfn_t lfns[ /*max*/ 20 ];
    if (lfn_count > 20) return false;

    for (int idx = 0; idx < lfn_count; idx++) {
        fat32_lfn_t* l = &lfns[idx];
        s_memset(l, 0, sizeof(*l));
        l->attr = 0x0F;
        l->type = 0;
        l->checksum = cs;
        l->cluster_low = 0;

        int base = idx * 13;
        for (int i = 0; i < 13; i++) {
            int p = base + i;
            uint16_t u;
            if (p < len) u = local_to_ucs2((unsigned char)long_name[p]);
            else if (p == len) u = 0x0000;       // null-terminator
            else u = 0xFFFF;                     // padding
            (void)padded;
            if (i < 5)       l->name1[i]      = u;
            else if (i < 11) l->name2[i - 5]  = u;
            else             l->name3[i - 11] = u;
        }
        l->order = (uint8_t)(idx + 1);
        if (idx == lfn_count - 1) l->order |= 0x40;
    }

    // Пишем LFN в обратном порядке (последний с битом 0x40 идёт первым в директории).
    uint32_t entries_per_cluster = (uint32_t)(g_cluster_size_bytes / sizeof(fat32_dirent_t));
    uint32_t cluster = pos.cluster;
    uint32_t entry = pos.entry_in_cluster;

    if (!read_dir_cluster(cluster)) return false;
    fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;

    for (int idx = lfn_count - 1; idx >= 0; idx--) {
        if (entry >= entries_per_cluster) {
            if (!write_dir_cluster(cluster)) return false;
            cluster = fat_next_cluster(cluster);
            if (cluster >= 0x0FFFFFF8) return false;
            if (!read_dir_cluster(cluster)) return false;
            d = (fat32_dirent_t*)g_cluster_buf;
            entry = 0;
        }
        s_memcpy(&d[entry], &lfns[idx], sizeof(fat32_dirent_t));
        entry++;
    }
    // 8.3 запись.
    if (entry >= entries_per_cluster) {
        if (!write_dir_cluster(cluster)) return false;
        cluster = fat_next_cluster(cluster);
        if (cluster >= 0x0FFFFFF8) return false;
        if (!read_dir_cluster(cluster)) return false;
        d = (fat32_dirent_t*)g_cluster_buf;
        entry = 0;
    }
    s_memcpy(&d[entry], main_de, sizeof(fat32_dirent_t));
    return write_dir_cluster(cluster);
}

// ----------------------------------------------------------------------------
// Очистить N слотов перед/включая dir_slot_t (помечаем 0xE5).
// ----------------------------------------------------------------------------
static bool dir_erase_slot(const dir_slot_t* slot) {
    uint32_t entries_per_cluster = (uint32_t)(g_cluster_size_bytes / sizeof(fat32_dirent_t));

    if (slot->lfn_count > 0) {
        // Очищаем LFN-серию.
        uint32_t cluster = slot->cluster;
        uint32_t e = slot->lfn_first_index;
        if (!read_dir_cluster(cluster)) return false;
        fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;
        for (uint32_t k = 0; k < slot->lfn_count; k++) {
            if (e >= entries_per_cluster) {
                write_dir_cluster(cluster);
                cluster = fat_next_cluster(cluster);
                if (cluster >= 0x0FFFFFF8) return false;
                if (!read_dir_cluster(cluster)) return false;
                d = (fat32_dirent_t*)g_cluster_buf;
                e = 0;
            }
            d[e].name[0] = (char)0xE5;
            e++;
        }
        write_dir_cluster(cluster);
    }

    // Очищаем 8.3.
    if (!read_dir_cluster(slot->cluster)) return false;
    fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;
    d[slot->entry_in_cluster].name[0] = (char)0xE5;
    return write_dir_cluster(slot->cluster);
}

// ----------------------------------------------------------------------------
// VFS-адаптер.
// ----------------------------------------------------------------------------
typedef struct {
    char     vfs_path[VFS_MAX_NAME];   // полное "/disk/.../name"
} fat32_node_priv_t;

static int64_t fat32_vfs_read(vfs_node_t* node, uint64_t off, uint64_t len, void* buf) {
    if (!node || !node->impl) return -1;
    fat32_node_priv_t* priv = (fat32_node_priv_t*)node->impl;
    fat32_dirent_t de;
    if (!resolve_path(priv->vfs_path, &de, 0, 0, 0)) return -1;
    if (de.attr & FAT32_ATTR_DIR) return -1;
    uint32_t first = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
    return read_cluster_chain(first, de.size, buf, off, len);
}

// ----------------------------------------------------------------------------
bool fat32_is_mounted(void) { return g_mounted; }

// ----------------------------------------------------------------------------
// Регистрация всех файлов в VFS, рекурсивно по всем подкаталогам.
// ----------------------------------------------------------------------------
typedef struct {
    char prefix[VFS_MAX_NAME];   // например "/disk" или "/disk/Sanctum"
    int  registered;
} register_ctx_t;

static int register_walk_cb(const fat32_dirent_t* de, const char* long_name,
                            dir_slot_t* slot, void* user);

static void register_dir(uint32_t dir_cluster, const char* prefix, int* registered) {
    register_ctx_t ctx;
    int n = 0;
    while (prefix[n] && n < VFS_MAX_NAME - 1) { ctx.prefix[n] = prefix[n]; n++; }
    ctx.prefix[n] = 0;
    ctx.registered = *registered;
    walk_dir_clusters(dir_cluster, register_walk_cb, &ctx);
    *registered = ctx.registered;
}

static int register_walk_cb(const fat32_dirent_t* de, const char* long_name,
                            dir_slot_t* slot, void* user) {
    (void)slot;
    register_ctx_t* ctx = (register_ctx_t*)user;
    if (de->attr & FAT32_ATTR_VOLLABEL) return 0;
    if (de->name[0] == '.') return 0;

    char short_name[16]; name_from_83(de->name, short_name);
    const char* nm = long_name ? long_name : short_name;

    char path[VFS_MAX_NAME];
    int p = 0;
    for (int i = 0; ctx->prefix[i] && p < VFS_MAX_NAME - 1; i++) path[p++] = ctx->prefix[i];
    if (p > 0 && path[p - 1] != '/' && p < VFS_MAX_NAME - 1) path[p++] = '/';
    for (int i = 0; nm[i] && p < VFS_MAX_NAME - 1; i++) path[p++] = nm[i];
    path[p] = 0;

    if (de->attr & FAT32_ATTR_DIR) {
        uint32_t cl = ((uint32_t)de->cluster_high << 16) | de->cluster_low;
        if (cl < 2) return 0;
        register_dir(cl, path, &ctx->registered);
        return 0;
    }

    fat32_node_priv_t* priv = (fat32_node_priv_t*)kmalloc(sizeof(*priv));
    if (!priv) return 0;
    int k; for (k = 0; path[k] && k < VFS_MAX_NAME - 1; k++) priv->vfs_path[k] = path[k];
    priv->vfs_path[k] = 0;

    vfs_node_t n = {0};
    for (k = 0; path[k] && k < VFS_MAX_NAME - 1; k++) n.name[k] = path[k];
    n.size = de->size;
    n.read = fat32_vfs_read;
    n.impl = priv;
    vfs_register(&n);

    serial_puts("[fat32]   "); serial_puts(path);
    serial_puts(" — "); serial_print_dec(de->size); serial_puts(" bytes\n");
    ctx->registered++;
    return 0;
}

// Прочитать BPB текущего привязанного тома и подготовить геометрию.
// register_vfs=false используется установщиком: том наполняется вручную,
// в VFS его светить не нужно.
static int mount_bound(bool register_vfs) {
    uint8_t boot[512];
    if (!vol_read(0, 1, boot)) { serial_puts("[fat32] sector 0 read failed\n"); return -1; }
    g_bpb = *(fat32_bpb_t*)boot;

    if (g_bpb.bytes_per_sector != 512) {
        serial_puts("[fat32] only 512-byte sectors supported\n");
        return -1;
    }
    if (g_bpb.fat_size_32 == 0) {
        serial_puts("[fat32] not FAT32 (fat_size_32 == 0)\n");
        return -1;
    }

    g_first_data_sector  = (uint64_t)g_bpb.reserved_sectors
                         + (uint64_t)g_bpb.num_fats * g_bpb.fat_size_32;
    g_cluster_size_bytes = (uint64_t)g_bpb.bytes_per_sector * g_bpb.sectors_per_cluster;

    // Буфер кластера переиспользуем, пока размер не вырос.
    static uint64_t g_cluster_buf_cap = 0;
    if (!g_cluster_buf || g_cluster_buf_cap < g_cluster_size_bytes) {
        g_cluster_buf = (uint8_t*)kmalloc(g_cluster_size_bytes);
        if (!g_cluster_buf) { serial_puts("[fat32] heap OOM for cluster buf\n"); return -1; }
        g_cluster_buf_cap = g_cluster_size_bytes;
    }

    fat_caches_drop();
    g_mounted = true;
    g_vfs_attached = register_vfs;

    serial_puts("[fat32] mounted ");
    char vol[12]; for (int i = 0; i < 11; i++) vol[i] = g_bpb.volume_label[i]; vol[11] = 0;
    serial_puts(vol);
    serial_puts(" / dev=");
    serial_print_dec((uint64_t)g_vol_dev);
    serial_puts(" / base_lba=");
    serial_print_dec(g_vol_base);
    serial_puts(" / cluster_size=");
    serial_print_dec(g_cluster_size_bytes);
    serial_puts(" / root_cluster=");
    serial_print_dec(g_bpb.root_cluster);
    serial_puts("\n");

    if (register_vfs) {
        int registered = 0;
        register_dir(g_bpb.root_cluster, "/disk", &registered);
        serial_puts("[fat32] registered ");
        serial_print_dec(registered);
        serial_puts(" files in /disk (recursive)\n");
    }
    return 0;
}

int fat32_mount(void) {
    // Ищем системный FAT32 по всем ATA-устройствам: сначала GPT/MBR-раздел,
    // затем — bare FAT32 в LBA 0 (наш dev-образ диска).
    for (int dev = 0; dev < ATA_MAX_DEVICES; dev++) {
        if (!disk_present_dev(dev)) continue;
        int64_t base = part_find_system_fat32(dev);
        if (base < 0) continue;
        g_vol_dev  = dev;
        g_vol_base = (uint64_t)base;
        if (mount_bound(true) == 0) return 0;
    }
    g_vol_dev = -1; g_vol_base = 0; g_mounted = false;
    serial_puts("[fat32] no FAT32 volume found\n");
    return -1;
}

// [этап 48] Смонтировать конкретный том как /disk (с регистрацией в VFS).
// Используется после разблокировки зашифрованного системного раздела:
// dev — виртуальное устройство blkcrypt, base_lba = 0.
int fat32_mount_volume(int dev, uint64_t base_lba) {
    g_vol_dev  = dev;
    g_vol_base = base_lba;
    g_mounted  = false;
    if (mount_bound(true) == 0) return 0;
    g_vol_dev = -1; g_vol_base = 0; g_mounted = false;
    return -1;
}

void fat32_get_volume(int* dev, uint64_t* base_lba) {
    if (dev)      *dev      = g_vol_dev;
    if (base_lba) *base_lba = g_vol_base;
}

int fat32_bind(int dev, uint64_t base_lba) {
    g_vol_dev  = dev;
    g_vol_base = base_lba;
    g_mounted  = false;
    return mount_bound(false);
}

void fat32_unbind(void) {
    g_vol_dev  = -1;
    g_vol_base = 0;
    g_mounted  = false;
    g_vfs_attached = false;
    fat_caches_drop();
}

void fat32_save_volume(fat32_volume_t* out) {
    if (!out) return;
    out->dev          = g_mounted ? g_vol_dev : -1;
    out->base_lba     = g_vol_base;
    out->vfs_attached = g_mounted && g_vfs_attached;
}

void fat32_restore_volume(const fat32_volume_t* v) {
    if (!v || v->dev < 0) { fat32_unbind(); return; }
    if (fat32_bind(v->dev, v->base_lba) != 0) { fat32_unbind(); return; }
    g_vfs_attached = v->vfs_attached;
}


// ----------------------------------------------------------------------------
// Public read.
// ----------------------------------------------------------------------------
int64_t fat32_read_file(const char* name, void* buf, uint64_t max_size) {
    if (!g_mounted) return -1;
    fat32_dirent_t de;
    if (!resolve_path(name, &de, 0, 0, 0)) return -1;
    if (de.attr & FAT32_ATTR_DIR) return -1;
    uint32_t first = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
    return read_cluster_chain(first, de.size, buf, 0, max_size);
}

int64_t fat32_read_file_at(const char* name, uint64_t off, void* buf, uint64_t len,
                           uint64_t* size_out) {
    if (!g_mounted) return -1;
    fat32_dirent_t de;
    if (!resolve_path(name, &de, 0, 0, 0)) return -1;
    if (de.attr & FAT32_ATTR_DIR) return -1;
    if (size_out) *size_out = de.size;
    uint32_t first = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
    return read_cluster_chain(first, de.size, buf, off, len);
}

// ----------------------------------------------------------------------------
// Patch dirent (size + first_cluster) на диске.
// ----------------------------------------------------------------------------
static bool patch_dirent(const dir_slot_t* slot, uint32_t first_cluster, uint32_t size) {
    if (!read_dir_cluster(slot->cluster)) return false;
    fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;
    d[slot->entry_in_cluster].cluster_high = (uint16_t)(first_cluster >> 16);
    d[slot->entry_in_cluster].cluster_low  = (uint16_t)(first_cluster & 0xFFFF);
    d[slot->entry_in_cluster].size         = size;
    return write_dir_cluster(slot->cluster);
}

// ----------------------------------------------------------------------------
// Public write/create/delete.
// ----------------------------------------------------------------------------
static bool create_entry(const char* name, uint32_t parent, const char* base,
                         uint32_t first_cluster, uint64_t size);

int64_t fat32_write_file(const char* name, const void* data, uint64_t size) {
    if (!g_mounted) return -1;
    fat32_dirent_t de;
    dir_slot_t slot;
    if (!resolve_path(name, &de, &slot, 0, 0)) return -1;
    if (de.attr & FAT32_ATTR_DIR) return -1;

    uint32_t first = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
    int64_t r = write_cluster_chain_resize(&first, de.size, size, data);
    if (r < 0) return -1;
    if (!patch_dirent(&slot, first, (uint32_t)size)) return -1;
    vol_flush();
    return r;
}

int64_t fat32_create_file(const char* name, const void* data, uint64_t size) {
    if (!g_mounted) return -1;
    uint32_t parent;
    const char* base;
    if (!resolve_parent(name, &parent, &base)) return -1;
    if (!*base) return -1;

    // Если уже есть — write.
    fat32_dirent_t existing;
    if (dir_find_entry(parent, base, &existing, 0)) {
        return fat32_write_file(name, data, size);
    }

    // Аллокация цепочки + запись данных.
    uint32_t first_cluster = 0;
    if (size > 0) {
        int64_t r = write_cluster_chain_resize(&first_cluster, 0, size, data);
        if (r < 0) return -1;
    }
    return create_entry(name, parent, base, first_cluster, size) ? (int64_t)size : -1;
}

// [51.2] Регистрировать ли новые файлы в реестре VFS. Реестр плоский, на 256
// записей, и записи из него не удаляются: программа WebAssembly, которая
// создаёт и стирает файлы сотнями, заполнила бы его за один запуск и оставила
// систему без места для её собственных файлов. Её файлы читаются прямо с
// тома (fat32_read_file_at), реестр им не нужен. Возвращает прежнее значение
// — вызывающий обязан вернуть его после своей записи.
static bool g_vfs_autoreg = true;
bool fat32_vfs_autoregister(bool on) { bool was = g_vfs_autoreg; g_vfs_autoreg = on; return was; }

// Запись каталога для файла с уже записанной цепочкой (first_cluster, size)
// + регистрация в VFS. Общая часть fat32_create_file и потоковой записи.
static bool create_entry(const char* name, uint32_t parent, const char* base,
                         uint32_t first_cluster, uint64_t size) {
    // Подготавливаем 8.3-имя (alias для длинных).
    char short83[11];
    bool needs_lfn = name_needs_lfn(base);
    if (needs_lfn) make_short_alias(parent, base, short83);
    else           to_short_name(base, short83);

    fat32_dirent_t nd; s_memset(&nd, 0, sizeof(nd));
    for (int i = 0; i < 11; i++) nd.name[i] = short83[i];
    nd.attr         = FAT32_ATTR_ARCHIVE;
    nd.cluster_high = (uint16_t)(first_cluster >> 16);
    nd.cluster_low  = (uint16_t)(first_cluster & 0xFFFF);
    nd.size         = (uint32_t)size;

    int slots = needs_lfn ? slots_needed(base) : 1;
    slot_pos_t pos;
    if (!dir_alloc_slots(parent, slots, &pos)) {
        if (first_cluster) fat_free_chain(first_cluster);
        return false;
    }

    bool ok;
    if (needs_lfn) ok = write_lfn_chain(pos, base, &nd);
    else {
        if (!read_dir_cluster(pos.cluster)) ok = false;
        else {
            fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;
            d[pos.entry_in_cluster] = nd;
            ok = write_dir_cluster(pos.cluster);
        }
    }
    if (!ok) {
        if (first_cluster) fat_free_chain(first_cluster);
        return false;
    }

    vol_flush();

    // Регистрация в VFS (только для тома живой системы).
    fat32_node_priv_t* priv = (g_vfs_attached && g_vfs_autoreg)
                            ? (fat32_node_priv_t*)kmalloc(sizeof(*priv)) : 0;
    if (priv) {
        // Нормализуем path к /disk/...
        char abs_path[VFS_MAX_NAME];
        int p = 0;
        const char prefix[] = "/disk/";
        for (int i = 0; i < (int)sizeof(prefix) - 1 && p < VFS_MAX_NAME - 1; i++)
            abs_path[p++] = prefix[i];
        const char* sn = strip_disk_prefix(name);
        if (*sn == '/') sn++;
        for (int i = 0; sn[i] && p < VFS_MAX_NAME - 1; i++) abs_path[p++] = sn[i];
        abs_path[p] = 0;
        for (int i = 0; abs_path[i] && i < VFS_MAX_NAME - 1; i++) priv->vfs_path[i] = abs_path[i];
        priv->vfs_path[VFS_MAX_NAME - 1] = 0;

        vfs_node_t vn = {0};
        for (int i = 0; abs_path[i] && i < VFS_MAX_NAME - 1; i++) vn.name[i] = abs_path[i];
        vn.size = (uint32_t)size;
        vn.read = fat32_vfs_read;
        vn.impl = priv;
        vfs_register(&vn);
    }
    return true;
}

// ----------------------------------------------------------------------------
// [49.1] Потоковая запись большого файла (веса AMPLIFY MIND, 1.8 ГБ).
// ----------------------------------------------------------------------------
// fat32_create_file пишет по одному кластеру и на КАЖДЫЙ кластер делает
// read-modify-write сектора FAT во всех копиях: для 1.8 ГБ (сотни тысяч
// кластеров) это миллион с лишним команд диска, плюс весь файл должен
// лежать в памяти. Здесь:
//   1) при открытии свободные кластеры собираются сериями (runs) одним
//      проходом по FAT — каждый сектор FAT читается один раз;
//   2) цепочка записывается в FAT посекторно: сектор читается, в нём
//      правятся все записи цепочки, и он пишется во все копии FAT один раз;
//   3) данные идут по мере поступления: целые кластеры, лежащие в серии
//      подряд, — одной командой прямо из буфера вызывающего, хвост
//      неполного кластера копится в tail;
//   4) запись каталога (с LFN) создаётся в конце, когда длина известна, —
//      обрыв посередине не оставит в каталоге файла с мусором (только
//      потерянные кластеры, их при ошибке освобождает fat32_stream_abort).
static int fat_scan_runs(fat32_stream_t* st, uint64_t need) {
    uint32_t total = fat_total_entries();
    st->nruns = 0;
    uint64_t got = 0;
    for (uint32_t c = 2; c < total && got < need; c++) {
        if (fat_next_cluster(c) != 0) continue;
        if (st->nruns > 0 && st->run_start[st->nruns - 1] + st->run_len[st->nruns - 1] == c) {
            st->run_len[st->nruns - 1]++;
        } else {
            if (st->nruns == FAT32_STREAM_RUNS) return -1;          // слишком фрагментирован
            st->run_start[st->nruns] = c;
            st->run_len[st->nruns] = 1;
            st->nruns++;
        }
        got++;
    }
    return got == need ? 0 : -1;
}

// Записать в FAT цепочку по сериям (link) или обнулить их (освобождение).
static bool fat_write_runs(const fat32_stream_t* st, bool link) {
    static uint8_t sec[512];
    uint64_t cur = (uint64_t)-1;
    for (int r = 0; r < st->nruns; r++) {
        for (uint32_t k = 0; k < st->run_len[r]; k++) {
            uint32_t c = st->run_start[r] + k;
            uint32_t v = 0;
            if (link) {
                if (k + 1 < st->run_len[r]) v = c + 1;
                else if (r + 1 < st->nruns) v = st->run_start[r + 1];
                else v = 0x0FFFFFFFu;
            }
            uint64_t fs = (uint64_t)g_bpb.reserved_sectors + (uint64_t)c * 4 / 512;
            if (fs != cur) {
                if (cur != (uint64_t)-1)
                    for (uint8_t f = 0; f < g_bpb.num_fats; f++)
                        if (!vol_write(cur + (uint64_t)f * g_bpb.fat_size_32, 1, sec)) return false;
                if (!vol_read(fs, 1, sec)) return false;
                cur = fs;
            }
            uint32_t* ep = (uint32_t*)(sec + (c * 4) % 512);
            *ep = (*ep & 0xF0000000u) | (v & 0x0FFFFFFFu);
        }
    }
    if (cur != (uint64_t)-1)
        for (uint8_t f = 0; f < g_bpb.num_fats; f++)
            if (!vol_write(cur + (uint64_t)f * g_bpb.fat_size_32, 1, sec)) return false;
    fat_caches_drop();
    return true;
}

int fat32_stream_open(fat32_stream_t* st, const char* path, uint64_t size) {
    s_memset(st, 0, sizeof(*st));
    if (!g_mounted || size >= 0xFFFFFFFFull) return -1;
    uint32_t parent;
    const char* base;
    if (!resolve_parent(path, &parent, &base) || !*base) return -1;
    fat32_dirent_t existing;
    if (dir_find_entry(parent, base, &existing, 0) && fat32_delete_file(path) != 0) return -1;
    uint64_t need = (size + g_cluster_size_bytes - 1) / g_cluster_size_bytes;
    if (need && fat_scan_runs(st, need) != 0) return -1;
    if (need && !fat_write_runs(st, true)) return -1;
    st->tail = (uint8_t*)kmalloc(g_cluster_size_bytes);
    if (!st->tail) { fat_write_runs(st, false); return -1; }
    int n = 0;
    while (path[n] && n + 1 < (int)sizeof(st->path)) { st->path[n] = path[n]; n++; }
    st->path[n] = 0;
    st->size = size;
    st->active = true;
    return 0;
}

// Номер кластера по позиции «серия / смещение».
static uint32_t stream_cluster(const fat32_stream_t* st) {
    return st->run_start[st->cur_run] + st->cur_off;
}

static void stream_advance(fat32_stream_t* st, uint32_t k) {
    st->cur_off += k;
    if (st->cur_run < st->nruns && st->cur_off >= st->run_len[st->cur_run]) {
        st->cur_run++;
        st->cur_off = 0;
    }
}

int64_t fat32_stream_write(fat32_stream_t* st, const void* data, uint64_t n) {
    if (!st->active || st->written + n > st->size) return -1;
    const uint8_t* src = (const uint8_t*)data;
    uint64_t done = 0;
    const uint64_t cs = g_cluster_size_bytes;
    while (done < n) {
        if (st->tail_n == 0 && n - done >= cs) {
            // Целые кластеры подряд в текущей серии — одной записью.
            uint64_t k = (n - done) / cs;
            uint64_t left = st->run_len[st->cur_run] - st->cur_off;
            if (k > left) k = left;
            if (k > 2048) k = 2048;
            if (!vol_write(cluster_to_sector(stream_cluster(st)), k * g_bpb.sectors_per_cluster, src + done))
                return -1;
            done += k * cs;
            stream_advance(st, (uint32_t)k);
            continue;
        }
        uint64_t take = cs - st->tail_n;
        if (take > n - done) take = n - done;
        s_memcpy(st->tail + st->tail_n, src + done, take);
        st->tail_n += (uint32_t)take;
        done += take;
        if (st->tail_n == cs) {
            if (!vol_write(cluster_to_sector(stream_cluster(st)), g_bpb.sectors_per_cluster, st->tail)) return -1;
            stream_advance(st, 1);
            st->tail_n = 0;
        }
    }
    st->written += n;
    return (int64_t)n;
}

int fat32_stream_close(fat32_stream_t* st) {
    if (!st->active) return -1;
    if (st->written != st->size) { fat32_stream_abort(st); return -1; }
    if (st->tail_n) {
        s_memset(st->tail + st->tail_n, 0, g_cluster_size_bytes - st->tail_n);
        if (!vol_write(cluster_to_sector(stream_cluster(st)), g_bpb.sectors_per_cluster, st->tail)) {
            fat32_stream_abort(st);
            return -1;
        }
    }
    uint32_t parent;
    const char* base;
    uint32_t first = st->nruns ? st->run_start[0] : 0;
    bool ok = resolve_parent(st->path, &parent, &base) && create_entry(st->path, parent, base, first, st->size);
    kfree(st->tail);
    st->tail = 0;
    st->active = false;
    vol_flush();
    return ok ? 0 : -1;
}

void fat32_stream_abort(fat32_stream_t* st) {
    if (!st->active) return;
    (void)fat_write_runs(st, false);
    if (st->tail) kfree(st->tail);
    st->tail = 0;
    st->active = false;
    vol_flush();
}

int fat32_delete_file(const char* name) {
    if (!g_mounted) return -1;
    fat32_dirent_t de;
    dir_slot_t slot;
    uint32_t parent_cluster;
    const char* base;
    if (!resolve_path(name, &de, &slot, &parent_cluster, &base)) return -1;

    if (de.attr & FAT32_ATTR_DIR) {
        // Проверяем, что директория пуста (только "." и "..").
        uint32_t cl = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
        if (cl < 2) return -1;
        if (!read_dir_cluster(cl)) return -1;
        fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;
        uint32_t epc = (uint32_t)(g_cluster_size_bytes / sizeof(fat32_dirent_t));
        for (uint32_t i = 0; i < epc; i++) {
            uint8_t f = (uint8_t)d[i].name[0];
            if (f == 0x00) break;
            if (f == 0xE5) continue;
            if (d[i].name[0] == '.') continue;     // "." и ".."
            return -1;     // не пуста
        }
    }

    uint32_t first = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
    if (first >= 2) fat_free_chain(first);
    bool ok = dir_erase_slot(&slot);
    vol_flush();
    return ok ? 0 : -1;
}

int64_t fat32_save_file(const char* name, const void* data, uint64_t size) {
    if (!g_mounted) return -1;
    fat32_dirent_t de;
    if (resolve_path(name, &de, 0, 0, 0)) {
        return fat32_write_file(name, data, size);
    }
    return fat32_create_file(name, data, size);
}

// ----------------------------------------------------------------------------
// mkdir.
// ----------------------------------------------------------------------------
int fat32_mkdir(const char* path) {
    if (!g_mounted) return -1;
    uint32_t parent;
    const char* base;
    if (!resolve_parent(path, &parent, &base)) return -1;
    if (!*base) return 1;

    fat32_dirent_t existing;
    if (dir_find_entry(parent, base, &existing, 0)) {
        return (existing.attr & FAT32_ATTR_DIR) ? 1 : -1;
    }

    // Выделяем кластер для новой директории.
    uint32_t dir_cluster = fat_find_free_cluster(2);
    if (!dir_cluster) return -1;
    if (!fat_set_entry(dir_cluster, 0x0FFFFFFFu)) return -1;
    if (!zero_cluster(dir_cluster)) { fat_free_chain(dir_cluster); return -1; }

    // Записываем "." и ".." в новый кластер.
    if (!read_dir_cluster(dir_cluster)) { fat_free_chain(dir_cluster); return -1; }
    fat32_dirent_t* d = (fat32_dirent_t*)g_cluster_buf;
    s_memset(&d[0], 0, sizeof(fat32_dirent_t));
    s_memset(&d[1], 0, sizeof(fat32_dirent_t));
    for (int i = 0; i < 11; i++) d[0].name[i] = ' ';
    d[0].name[0] = '.';
    d[0].attr = FAT32_ATTR_DIR;
    d[0].cluster_high = (uint16_t)(dir_cluster >> 16);
    d[0].cluster_low  = (uint16_t)(dir_cluster & 0xFFFF);
    for (int i = 0; i < 11; i++) d[1].name[i] = ' ';
    d[1].name[0] = '.'; d[1].name[1] = '.';
    d[1].attr = FAT32_ATTR_DIR;
    uint32_t parent_for_dotdot = (parent == g_bpb.root_cluster) ? 0 : parent;
    d[1].cluster_high = (uint16_t)(parent_for_dotdot >> 16);
    d[1].cluster_low  = (uint16_t)(parent_for_dotdot & 0xFFFF);
    if (!write_dir_cluster(dir_cluster)) { fat_free_chain(dir_cluster); return -1; }

    // Записываем dirent в parent.
    char short83[11];
    bool needs_lfn = name_needs_lfn(base);
    if (needs_lfn) make_short_alias(parent, base, short83);
    else           to_short_name(base, short83);

    fat32_dirent_t nd; s_memset(&nd, 0, sizeof(nd));
    for (int i = 0; i < 11; i++) nd.name[i] = short83[i];
    nd.attr         = FAT32_ATTR_DIR;
    nd.cluster_high = (uint16_t)(dir_cluster >> 16);
    nd.cluster_low  = (uint16_t)(dir_cluster & 0xFFFF);
    nd.size         = 0;

    int slots = needs_lfn ? slots_needed(base) : 1;
    slot_pos_t pos;
    if (!dir_alloc_slots(parent, slots, &pos)) {
        fat_free_chain(dir_cluster);
        return -1;
    }
    bool ok;
    if (needs_lfn) ok = write_lfn_chain(pos, base, &nd);
    else {
        if (!read_dir_cluster(pos.cluster)) ok = false;
        else {
            fat32_dirent_t* dd = (fat32_dirent_t*)g_cluster_buf;
            dd[pos.entry_in_cluster] = nd;
            ok = write_dir_cluster(pos.cluster);
        }
    }
    if (!ok) {
        fat_free_chain(dir_cluster);
        return -1;
    }
    vol_flush();
    serial_puts("[fat32] mkdir "); serial_puts(path); serial_puts("\n");
    return 0;
}

int fat32_mkdir_p(const char* path) {
    if (!g_mounted) return -1;
    const char* p = strip_disk_prefix(path);
    if (*p == '/') p++;

    char acc[VFS_MAX_NAME];
    int alen = 0;
    const char prefix[] = "/disk";
    for (int i = 0; prefix[i] && alen < VFS_MAX_NAME - 1; i++) acc[alen++] = prefix[i];
    acc[alen] = 0;

    while (*p) {
        if (alen < VFS_MAX_NAME - 1) acc[alen++] = '/';
        while (*p && *p != '/' && alen < VFS_MAX_NAME - 1) acc[alen++] = *p++;
        acc[alen] = 0;
        if (*p == '/') p++;
        int r = fat32_mkdir(acc);
        if (r < 0) return -1;
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Iterator.
// ----------------------------------------------------------------------------
typedef struct {
    fat32_dir_cb_t cb;
    void* user;
} iter_ctx_t;

static int iter_walk_cb(const fat32_dirent_t* de, const char* long_name,
                        dir_slot_t* slot, void* user) {
    (void)slot;
    iter_ctx_t* ctx = (iter_ctx_t*)user;
    if (de->attr & FAT32_ATTR_VOLLABEL) return 0;
    if (de->name[0] == '.') return 0;     // пропускаем "." и ".."

    fat32_dir_entry_t e; s_memset(&e, 0, sizeof(e));
    char short_name[16]; name_from_83(de->name, short_name);
    const char* nm = long_name ? long_name : short_name;
    int n; for (n = 0; nm[n] && n < FAT32_LONG_NAME_MAX - 1; n++) e.name[n] = nm[n];
    e.name[n] = 0;
    e.is_dir = (de->attr & FAT32_ATTR_DIR) ? true : false;
    e.size = de->size;
    e.first_cluster = ((uint32_t)de->cluster_high << 16) | de->cluster_low;
    return ctx->cb(&e, ctx->user);
}

int fat32_iter_dir(const char* path, fat32_dir_cb_t cb, void* user_data) {
    if (!g_mounted || !cb) return -1;
    uint32_t cluster;
    const char* p = strip_disk_prefix(path);
    if (*p == 0 || (*p == '/' && p[1] == 0)) {
        cluster = g_bpb.root_cluster;
    } else {
        fat32_dirent_t de;
        if (!resolve_path(path, &de, 0, 0, 0)) return -1;
        if (!(de.attr & FAT32_ATTR_DIR)) return -1;
        cluster = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
    }
    iter_ctx_t ctx = { cb, user_data };
    return walk_dir_clusters(cluster, iter_walk_cb, &ctx);
}

// [51.2] Что лежит по этому пути: файл или каталог, размер. 0 — есть (out
// заполнен), -1 — нет. Корень тома ("/disk") — каталог без имени.
int fat32_stat(const char* path, fat32_dir_entry_t* out) {
    if (!g_mounted || !out) return -1;
    s_memset(out, 0, sizeof(*out));
    const char* p = strip_disk_prefix(path);
    if (*p == 0 || (*p == '/' && p[1] == 0)) {
        out->is_dir = true;
        out->first_cluster = g_bpb.root_cluster;
        return 0;
    }
    fat32_dirent_t de;
    const char* base = 0;
    if (!resolve_path(path, &de, 0, 0, &base)) return -1;
    int n = 0;
    for (; base[n] && n < FAT32_LONG_NAME_MAX - 1; n++) out->name[n] = base[n];
    out->name[n] = 0;
    out->is_dir = (de.attr & FAT32_ATTR_DIR) ? true : false;
    out->size = de.size;
    out->first_cluster = ((uint32_t)de.cluster_high << 16) | de.cluster_low;
    return 0;
}

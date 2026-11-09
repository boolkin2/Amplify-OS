// ============================================================================
// pmm.c — реализация PMM для 64-битного ядра AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Логика взята из kernel/memory/pmm.c (i386), но переписана под uint64_t и под
// парсинг Multiboot2 memory map.
// ============================================================================
#include "pmm.h"
#include "memory.h"
#include "serial.h"
#include "paging.h"

// ----------------------------------------------------------------------------
// Глобальные структуры
// ----------------------------------------------------------------------------
// Bitmap: 1 бит = 1 страница 4 KB. 4 GB / 4 KB / 8 = 128 KB.
static uint8_t  pmm_bitmap[PMM_BITMAP_BYTES];

// Refcount: 1 байт на страницу. 4 GB / 4 KB = 1 MB.
// [38.1.0] Не в .bss: мегабайт нулей в образе ядра приближал его конец к
// памяти прошивки (ACPI NVS с адреса 0x800000, CLAUDE.md §5.50). Таблицу
// берём у самих себя в pmm_init (early_take), когда карта RAM уже разобрана.
// До этого момента указатель нулевой — pmm_release_region это учитывает.
static uint8_t* pmm_refcount;

static pmm_stats_t stats;

// Хинт для ускорения линейного поиска: индекс с которого начинать.
static uint64_t scan_hint = 0;

// [49.1] Память выше 4 ГБ: отсортированный список свободных экстентов.
// Выдаются только большие блоки (веса MIND и KV-кэш — единицы штук), так что
// простой first-fit со слиянием соседей при освобождении достаточен.
#define PMM_HIGH_MAX_EXT 64
typedef struct { uint64_t start, pages; } pmm_ext_t;
static pmm_ext_t g_high[PMM_HIGH_MAX_EXT];
static int       g_high_n;

// Вставка экстента с сохранением порядка и слиянием соседей.
static void high_insert(uint64_t start, uint64_t pages) {
    if (!pages) return;
    int i = 0;
    while (i < g_high_n && g_high[i].start < start) i++;
    if (i > 0 && g_high[i - 1].start + g_high[i - 1].pages * PAGE_SIZE == start) {
        g_high[i - 1].pages += pages;
        if (i < g_high_n && g_high[i - 1].start + g_high[i - 1].pages * PAGE_SIZE == g_high[i].start) {
            g_high[i - 1].pages += g_high[i].pages;
            for (int k = i; k + 1 < g_high_n; k++) g_high[k] = g_high[k + 1];
            g_high_n--;
        }
        return;
    }
    if (i < g_high_n && start + pages * PAGE_SIZE == g_high[i].start) {
        g_high[i].start = start;
        g_high[i].pages += pages;
        return;
    }
    if (g_high_n == PMM_HIGH_MAX_EXT) return;          // переполнение — память теряется
    for (int k = g_high_n; k > i; k--) g_high[k] = g_high[k - 1];
    g_high[i].start = start;
    g_high[i].pages = pages;
    g_high_n++;
}

// ----------------------------------------------------------------------------
// Внутренние bitmap-операции
// ----------------------------------------------------------------------------
static inline uint64_t addr_to_page(uint64_t phys) { return phys >> PAGE_SHIFT; }
static inline uint64_t page_to_addr(uint64_t page) { return page << PAGE_SHIFT; }

static inline void  bm_set  (uint64_t p) { pmm_bitmap[p >> 3] |=  (1u << (p & 7)); }
static inline void  bm_clear(uint64_t p) { pmm_bitmap[p >> 3] &= ~(1u << (p & 7)); }
static inline bool  bm_test (uint64_t p) { return pmm_bitmap[p >> 3] & (1u << (p & 7)); }

// ----------------------------------------------------------------------------
// Резервирование / освобождение регионов
// ----------------------------------------------------------------------------
void pmm_reserve_region(uint64_t start, uint64_t size) {
    uint64_t first = addr_to_page(PAGE_ALIGN_DOWN(start));
    uint64_t last  = addr_to_page(PAGE_ALIGN_UP(start + size) - 1);
    if (last >= PMM_MAX_PAGES) last = PMM_MAX_PAGES - 1;
    for (uint64_t p = first; p <= last; p++) {
        if (!bm_test(p)) {
            bm_set(p);
            if (stats.free_pages > 0) stats.free_pages--;
            stats.used_pages++;
        }
    }
}

void pmm_release_region(uint64_t start, uint64_t size) {
    uint64_t first = addr_to_page(PAGE_ALIGN_DOWN(start));
    uint64_t last  = addr_to_page(PAGE_ALIGN_UP(start + size) - 1);
    if (last >= PMM_MAX_PAGES) last = PMM_MAX_PAGES - 1;
    for (uint64_t p = first; p <= last; p++) {
        if (bm_test(p)) {
            bm_clear(p);
            stats.used_pages--;
            stats.free_pages++;
        }
        if (pmm_refcount) pmm_refcount[p] = 0;
    }
}

// ----------------------------------------------------------------------------
// [38.1.0] Занять `pages` страниц подряд, пока таблицы счётчиков ещё нет:
// обычный pmm_alloc_pages пишет в pmm_refcount, а она-то и выделяется.
// Ищем с 1 МБ: всё ниже зарезервировано. PMM_INVALID — не нашлось.
// ----------------------------------------------------------------------------
static uint64_t early_take(uint64_t pages) {
    uint64_t run = 0;
    for (uint64_t p = addr_to_page(1 * MB); p < PMM_MAX_PAGES; p++) {
        if (bm_test(p)) { run = 0; continue; }
        if (++run < pages) continue;
        uint64_t first = p + 1 - pages;
        for (uint64_t k = first; k <= p; k++) bm_set(k);
        stats.used_pages += pages;
        stats.free_pages -= pages;
        return page_to_addr(first);
    }
    return PMM_INVALID;
}

// ----------------------------------------------------------------------------
// Инициализация: разбираем Multiboot2 mmap
// ----------------------------------------------------------------------------
void pmm_init(const mb2_info_t* info) {
    // 1. Стартуем со ВСЕГО bitmap = занят (это безопасно: лучше не выдавать
    //    память, чем выдать чужое железо).
    for (uint64_t i = 0; i < PMM_BITMAP_BYTES; i++) pmm_bitmap[i] = 0xFF;
    pmm_refcount = 0;

    stats.total_pages = PMM_MAX_PAGES;
    stats.usable_pages = 0;
    stats.used_pages = PMM_MAX_PAGES;
    stats.free_pages = 0;

    // 2. Освобождаем все available (type=1) регионы из mmap.
    const mb2_tag_t* t = mb2_find_tag(info, MB2_TAG_MMAP);
    if (!t) {
        serial_puts("[pmm] FATAL: no Multiboot2 mmap tag — assuming 0 RAM.\n");
        return;
    }
    const mb2_tag_mmap_t* mmap = (const mb2_tag_mmap_t*)t;
    uint32_t entry_count = (mmap->size - 16) / mmap->entry_size;
    uint64_t total_avail = 0;

    for (uint32_t i = 0; i < entry_count; i++) {
        const mb2_mmap_entry_t* e =
            (const mb2_mmap_entry_t*)((const uint8_t*)mmap->entries + i * mmap->entry_size);

        // [38.1.1] Низ карты (до 16 МБ) — в журнал: там лежит всё, с чем
        // сталкивается ядро при загрузке (память прошивки с 0x800000 у OVMF,
        // чистые 1..16 МБ у BIOS). Типы — как в Multiboot2: 1 свободно,
        // 2 резерв, 3 ACPI (можно вернуть), 4 ACPI NVS, 5 сбойная.
        if (e->addr < 16 * MB) {
            serial_puts("[pmm] map ");
            serial_print_dec(e->type);
            serial_puts(" 0x");
            serial_print_hex(e->addr, 10);
            serial_puts("-0x");
            serial_print_hex(e->addr + e->len, 10);
            serial_puts("\n");
        }

        if (e->type != 1) continue;  // только available

        uint64_t end = e->addr + e->len;
        if (e->addr >= PMM_MAX_PHYS) continue;
        if (end > PMM_MAX_PHYS) end = PMM_MAX_PHYS;

        total_avail += (end - e->addr);
        pmm_release_region(e->addr, end - e->addr);
    }
    stats.total_phys  = total_avail;
    stats.usable_phys = total_avail;
    stats.usable_pages = total_avail / PAGE_SIZE;

    // 3. Резервируем 0..1 MB безусловно (BIOS, ROM, video memory).
    pmm_reserve_region(0, 1 * MB);

    // 4. Резервируем сам образ ядра. [38.1.1] Он лежит там, куда его положил
    //    загрузчик (kernel_phys_start), а не обязательно с 1 МБ; адрес
    //    _kernel_end — виртуальный, в память его переводит virt_to_phys.
    pmm_reserve_region(kernel_phys_start(), kernel_phys_end() - kernel_phys_start());

    // 5. Резервируем Multiboot info area + tags (если попали в mmap).
    pmm_reserve_region((uint64_t)(uintptr_t)info, info->total_size);

    // 6. Резервируем регион будущего kernel heap, чтобы pmm не отдал его
    //    в качестве произвольной страницы (heap собственноручно управляет
    //    всем своим диапазоном).
    pmm_reserve_region(KHEAP_START, KHEAP_SIZE);

    // 7. Резервируем ВСЕ Multiboot2 модули (этап 37.5a — initrd).
    //    Иначе pmm_alloc_page может отдать страницу с initrd-data,
    //    и elf64_load корруптит сам blob, который читает.
    {
        const uint8_t* p = (const uint8_t*)info + 8;
        const uint8_t* end_p = (const uint8_t*)info + info->total_size;
        while (p < end_p) {
            const mb2_tag_t* tg = (const mb2_tag_t*)p;
            if (tg->type == 0) break;
            if (tg->type == MB2_TAG_MODULE) {
                const mb2_tag_module_t* m = (const mb2_tag_module_t*)tg;
                pmm_reserve_region(m->mod_start, m->mod_end - m->mod_start);
                serial_puts("[pmm] reserved MB2 module 0x");
                serial_print_hex((uint64_t)m->mod_start, 8);
                serial_puts("..0x");
                serial_print_hex((uint64_t)m->mod_end, 8);
                serial_puts("\n");
            }
            p += (tg->size + 7) & ~7u;
        }
    }

    // 7а. [38.1.0] Таблица счётчиков ссылок — из только что размеченной
    //     памяти. Всё, что трогать нельзя (ядро, инфоблок, куча, модули), к
    //     этому месту уже зарезервировано. Без таблицы работать нельзя:
    //     каждый pmm_alloc_page пишет в неё.
    {
        uint64_t rc_pages = PAGE_ALIGN_UP(PMM_MAX_PAGES) / PAGE_SIZE;
        uint64_t rc = early_take(rc_pages);
        if (rc == PMM_INVALID) {
            serial_puts("[pmm] FATAL: no room for the refcount table\n");
            for (;;) __asm__ __volatile__("cli; hlt");
        }
        pmm_refcount = (uint8_t*)(uintptr_t)rc;
        for (uint64_t i = 0; i < PMM_MAX_PAGES; i++) pmm_refcount[i] = 0;
        for (uint64_t k = 0; k < rc_pages; k++) pmm_refcount[addr_to_page(rc) + k] = 1;
        serial_puts("[pmm] refcount table @ 0x");
        serial_print_hex(rc, 8);
        serial_puts("\n");
    }

    // 8. [49.1] RAM выше 4 ГБ: отобразить и завести экстенты.
    uint64_t top = 0;
    for (uint32_t i = 0; i < entry_count; i++) {
        const mb2_mmap_entry_t* e =
            (const mb2_mmap_entry_t*)((const uint8_t*)mmap->entries + i * mmap->entry_size);
        if (e->type == 1 && e->addr + e->len > PMM_MAX_PHYS && e->addr + e->len > top) top = e->addr + e->len;
    }
    if (top > PMM_MAX_PHYS) {
        uint64_t mapped = paging_map_high_identity(top);
        for (uint32_t i = 0; i < entry_count; i++) {
            const mb2_mmap_entry_t* e =
                (const mb2_mmap_entry_t*)((const uint8_t*)mmap->entries + i * mmap->entry_size);
            if (e->type != 1) continue;
            uint64_t s = e->addr, en = e->addr + e->len;
            if (s < PMM_MAX_PHYS) s = PMM_MAX_PHYS;
            if (en > mapped) en = mapped;
            s = PAGE_ALIGN_UP(s);
            en = PAGE_ALIGN_DOWN(en);
            if (en <= s) continue;
            high_insert(s, (en - s) / PAGE_SIZE);
            stats.high_total_pages += (en - s) / PAGE_SIZE;
        }
        stats.high_free_pages = stats.high_total_pages;
        serial_puts("[pmm] RAM above 4 GB: ");
        serial_print_dec(stats.high_total_pages * PAGE_SIZE / MB);
        serial_puts(" MB in ");
        serial_print_dec((uint64_t)g_high_n);
        serial_puts(" extent(s), identity-mapped up to 0x");
        serial_print_hex(mapped, 16);
        serial_puts("\n");
    }

    serial_puts("[pmm] usable RAM: ");
    {
        char buf[32]; int n = 0; uint64_t v = total_avail / (1024 * 1024);
        if (v == 0) buf[n++] = '0';
        else { char tmp[24]; int ti=0; while (v) { tmp[ti++]='0'+v%10; v/=10; }
               while (ti--) buf[n++] = tmp[ti]; }
        buf[n] = 0;
        serial_puts(buf);
    }
    serial_puts(" MB\n");
}

// ----------------------------------------------------------------------------
// [41.2] Вынести из диапазона кучи ядра то, что туда положил загрузчик.
//
// Куча — фиксированные KHEAP_START..KHEAP_END (16..144 МБ), heap_init
// размечает их, не глядя ни в карту памяти, ни на модули. Где окажутся initrd
// и инфоблок Multiboot2, решает загрузчик: наш BOOTX64.EFI кладёт их под
// верхнюю границу RAM, а GRUB в EFI-режиме (ISO, собранный там, где у
// grub-mkrescue есть модули x86_64-efi) — на 0x01600000, то есть ВНУТРЬ кучи.
// Первый же крупный kmalloc (doublebuf, 8 МБ) затирал заголовки initrd, и
// установщик падал с Page Fault в initrd_node_read (в регистрах 0xFF000000 —
// цвет пикселя). Резервирование модуля в bitmap (шаг 7 pmm_init) тут не
// помогает: куча не спрашивает pmm.
//
// Поэтому до heap_init переносим такие блоки в страницы, выданные pmm: он
// никогда не отдаёт диапазон кучи и сам модуль (оба зарезервированы), а его
// bitmap покрывает только первые 4 ГБ — новый адрес влезает в 32-битные
// mod_start/mod_end. Возвращает (возможно, новый) адрес инфоблока: дальше
// ядро должно пользоваться только им.
// ----------------------------------------------------------------------------
static bool in_kheap(uint64_t start, uint64_t end) {
    return start < KHEAP_END && end > KHEAP_START;
}

static uint64_t move_out_of_kheap(uint64_t src, uint64_t size, const char* what) {
    uint64_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t dst = pmm_alloc_pages(pages);
    if (dst == PMM_INVALID) {
        serial_puts("[pmm] FATAL: no room to move ");
        serial_puts(what);
        serial_puts(" out of the kernel heap range\n");
        return src;
    }
    // Источник зарезервирован, приёмник был свободен — области не пересекаются.
    const uint8_t* s = (const uint8_t*)(uintptr_t)src;
    uint8_t*       d = (uint8_t*)(uintptr_t)dst;
    for (uint64_t i = 0; i < size; i++) d[i] = s[i];
    serial_puts("[pmm] moved ");
    serial_puts(what);
    serial_puts(" out of the kernel heap range: 0x");
    serial_print_hex(src, 8);
    serial_puts(" -> 0x");
    serial_print_hex(dst, 8);
    serial_puts("\n");
    return dst;
}

const mb2_info_t* pmm_relocate_boot_data(const mb2_info_t* info) {
    // 1. Модули (initrd). Инфоблок лежит в обычной RAM — тэг правим на месте.
    uint8_t* p     = (uint8_t*)(uintptr_t)info + 8;
    uint8_t* end_p = (uint8_t*)(uintptr_t)info + info->total_size;
    while (p < end_p) {
        mb2_tag_t* tg = (mb2_tag_t*)p;
        if (tg->type == 0) break;
        if (tg->type == MB2_TAG_MODULE) {
            mb2_tag_module_t* m = (mb2_tag_module_t*)tg;
            uint64_t size = (uint64_t)m->mod_end - (uint64_t)m->mod_start;
            if (size && in_kheap(m->mod_start, m->mod_end)) {
                uint64_t dst = move_out_of_kheap(m->mod_start, size, "MB2 module");
                m->mod_start = (uint32_t)dst;
                m->mod_end   = (uint32_t)(dst + size);
            }
        }
        p += (tg->size + 7) & ~7u;
    }

    // 2. Сам инфоблок: его читают и после запуска кучи (ACPI RSDP, карта RAM).
    uint64_t ia = (uint64_t)(uintptr_t)info;
    if (in_kheap(ia, ia + info->total_size)) {
        info = (const mb2_info_t*)(uintptr_t)
               move_out_of_kheap(ia, info->total_size, "MB2 info block");
    }
    return info;
}

// ----------------------------------------------------------------------------
// Allocation
// ----------------------------------------------------------------------------
static uint64_t find_free_page_from(uint64_t hint) {
    // Сначала ищем с hint, потом с начала.
    for (uint64_t p = hint; p < PMM_MAX_PAGES; p++)
        if (!bm_test(p)) return p;
    for (uint64_t p = 0; p < hint; p++)
        if (!bm_test(p)) return p;
    return (uint64_t)-1;
}

uint64_t pmm_alloc_page(void) {
    uint64_t p = find_free_page_from(scan_hint);
    if (p == (uint64_t)-1) return PMM_INVALID;
    bm_set(p);
    pmm_refcount[p] = 1;
    stats.used_pages++;
    if (stats.free_pages > 0) stats.free_pages--;
    scan_hint = p + 1;
    return page_to_addr(p);
}

uint64_t pmm_alloc_pages(uint64_t count) {
    if (count == 0) return PMM_INVALID;
    if (count == 1) return pmm_alloc_page();

    // Линейный поиск count подряд свободных.
    for (uint64_t start = 0; start + count <= PMM_MAX_PAGES; start++) {
        bool ok = true;
        for (uint64_t k = 0; k < count; k++) {
            if (bm_test(start + k)) { ok = false; start += k; break; }
        }
        if (!ok) continue;

        for (uint64_t k = 0; k < count; k++) {
            bm_set(start + k);
            pmm_refcount[start + k] = 1;
        }
        stats.used_pages += count;
        if (stats.free_pages >= count) stats.free_pages -= count;
        else stats.free_pages = 0;
        return page_to_addr(start);
    }
    return PMM_INVALID;
}

void pmm_free_page(uint64_t phys) {
    uint64_t p = addr_to_page(phys);
    if (p >= PMM_MAX_PAGES) return;
    if (!bm_test(p)) return;

    if (pmm_refcount[p] > 1) {
        pmm_refcount[p]--;
        return;
    }
    pmm_refcount[p] = 0;
    bm_clear(p);
    stats.used_pages--;
    stats.free_pages++;
    if (p < scan_hint) scan_hint = p;
}

void pmm_free_pages(uint64_t phys, uint64_t count) {
    for (uint64_t i = 0; i < count; i++) pmm_free_page(phys + i * PAGE_SIZE);
}

void pmm_inc_ref(uint64_t phys) {
    uint64_t p = addr_to_page(phys);
    if (p < PMM_MAX_PAGES && pmm_refcount[p] < 0xFF) pmm_refcount[p]++;
}

uint8_t pmm_get_ref(uint64_t phys) {
    uint64_t p = addr_to_page(phys);
    return (p < PMM_MAX_PAGES) ? pmm_refcount[p] : 0;
}

bool pmm_is_free(uint64_t phys) {
    uint64_t p = addr_to_page(phys);
    if (p >= PMM_MAX_PAGES) return false;
    return !bm_test(p);
}

pmm_stats_t pmm_get_stats(void) { return stats; }

// ----------------------------------------------------------------------------
// [49.1] Большие блоки.
// ----------------------------------------------------------------------------
uint64_t pmm_alloc_big(uint64_t pages) {
    if (!pages) return PMM_INVALID;
    for (int i = 0; i < g_high_n; i++) {
        if (g_high[i].pages < pages) continue;
        uint64_t p = g_high[i].start;
        g_high[i].start += pages * PAGE_SIZE;
        g_high[i].pages -= pages;
        if (!g_high[i].pages) {
            for (int k = i; k + 1 < g_high_n; k++) g_high[k] = g_high[k + 1];
            g_high_n--;
        }
        stats.high_free_pages -= pages;
        return p;
    }
    return pmm_alloc_pages(pages);
}

void pmm_free_big(uint64_t phys, uint64_t pages) {
    if (!phys || !pages) return;
    if (phys >= PMM_MAX_PHYS) {
        high_insert(phys, pages);
        stats.high_free_pages += pages;
        return;
    }
    pmm_free_pages(phys, pages);
}

uint64_t pmm_big_largest_free(void) {
    uint64_t best = 0;
    for (int i = 0; i < g_high_n; i++) if (g_high[i].pages > best) best = g_high[i].pages;
    // Самый длинный пробег свободных страниц в bitmap (байт 0x00 = 8 свободных).
    uint64_t run = 0;
    for (uint64_t p = 0; p < PMM_MAX_PAGES;) {
        if (!(p & 7) && p + 8 <= PMM_MAX_PAGES && pmm_bitmap[p >> 3] == 0) { run += 8; p += 8; }
        else if (!bm_test(p)) { run++; p++; }
        else { run = 0; p++; }
        if (run > best) best = run;
    }
    return best * PAGE_SIZE;
}

uint64_t pmm_total_ram(void) { return (stats.usable_pages + stats.high_total_pages) * PAGE_SIZE; }
uint64_t pmm_free_ram(void)  { return (stats.free_pages + stats.high_free_pages) * PAGE_SIZE; }

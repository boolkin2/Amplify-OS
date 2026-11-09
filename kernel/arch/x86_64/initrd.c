// ============================================================================
// initrd.c — discovery + парсинг + регистрация файлов в VFS.
// ============================================================================
#include "initrd.h"
#include "vfs.h"
#include "serial.h"
#include "memory.h"

// Кеш базы initrd для read-callback'ов и initrd_get_file.
static const uint8_t*           initrd_base = 0;
static uint64_t                 initrd_size = 0;
static const initrd_header_t*   initrd_hdr  = 0;
static const initrd_file_header_t* initrd_files = 0;

// ----------------------------------------------------------------------------
// Local strncmp (мы без libc).
// ----------------------------------------------------------------------------
static int strncmp_local(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!a[i] || !b[i] || a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Read callback для VFS-узла initrd-файла.
// node->impl = указатель на initrd_file_header_t для этого файла.
// ----------------------------------------------------------------------------
static int64_t initrd_node_read(vfs_node_t* node, uint64_t off, uint64_t len, void* buf) {
    if (!node || !node->impl) return -1;
    const initrd_file_header_t* fh = (const initrd_file_header_t*)node->impl;
    if (off >= fh->size) return 0;
    uint64_t avail = fh->size - off;
    if (len > avail) len = avail;
    const uint8_t* src = initrd_base + fh->offset + off;
    uint8_t* dst = (uint8_t*)buf;
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return (int64_t)len;
}

// ----------------------------------------------------------------------------
int initrd_init_from_mb2(const mb2_info_t* info) {
    const mb2_tag_t* t = mb2_find_tag(info, MB2_TAG_MODULE);
    if (!t) {
        serial_puts("[initrd] no MB2 module tag — initrd not loaded\n");
        return -1;
    }
    const mb2_tag_module_t* mod = (const mb2_tag_module_t*)t;

    serial_puts("[initrd] module @ phys 0x");
    serial_print_hex(mod->mod_start, 8);
    serial_puts(" .. 0x");
    serial_print_hex(mod->mod_end, 8);
    serial_puts(" cmdline=\"");
    serial_puts(mod->cmdline);
    serial_puts("\"\n");

    // [41.2] Куча ядра — фиксированный диапазон KHEAP_START..KHEAP_END; если
    // модуль остался внутри, первые же крупные kmalloc (doublebuf) затрут
    // initrd, и упадёт это гораздо позже и непонятно где (так было с ISO,
    // загруженным через GRUB-EFI). Обычно модуль уже вынесен оттуда
    // pmm_relocate_boot_data; сюда попадаем, только если переносить было некуда.
    if ((uint64_t)mod->mod_start < KHEAP_END && (uint64_t)mod->mod_end > KHEAP_START) {
        serial_puts("[initrd] FATAL: module overlaps the kernel heap range - it will be overwritten\n");
    }

    initrd_base = (const uint8_t*)(uintptr_t)mod->mod_start;
    initrd_size = (uint64_t)(mod->mod_end - mod->mod_start);
    initrd_hdr  = (const initrd_header_t*)initrd_base;

    if (initrd_hdr->magic != INITRD_MAGIC) {
        serial_puts("[initrd] bad magic 0x");
        serial_print_hex(initrd_hdr->magic, 8);
        serial_puts(" expected 0x4D594F53 (\"MYOS\")\n");
        return -1;
    }
    if (initrd_hdr->num_files > INITRD_MAX_FILES) {
        serial_puts("[initrd] num_files exceeds INITRD_MAX_FILES\n");
        return -1;
    }

    initrd_files = (const initrd_file_header_t*)(initrd_base + sizeof(initrd_header_t));

    serial_puts("[initrd] OK: ");
    serial_print_dec(initrd_hdr->num_files);
    serial_puts(" files, total ");
    serial_print_dec(initrd_size);
    serial_puts(" bytes\n");

    // Регистрируем в VFS.
    for (uint32_t i = 0; i < initrd_hdr->num_files; i++) {
        const initrd_file_header_t* fh = &initrd_files[i];
        vfs_node_t n = {0};
        for (int j = 0; j < INITRD_MAX_FILENAME && j < VFS_MAX_NAME && fh->name[j]; j++) {
            n.name[j] = fh->name[j];
        }
        n.size = fh->size;
        n.read = initrd_node_read;
        n.impl = (void*)fh;
        vfs_register(&n);

        serial_puts("[initrd]   "); serial_puts(fh->name);
        serial_puts(" — ");          serial_print_dec(fh->size);
        serial_puts(" bytes @ off 0x"); serial_print_hex(fh->offset, 8);
        serial_puts("\n");
    }
    return 0;
}

// ----------------------------------------------------------------------------
const uint8_t* initrd_get_file(const char* name, uint64_t* size_out) {
    if (!initrd_files || !initrd_hdr) return 0;
    for (uint32_t i = 0; i < initrd_hdr->num_files; i++) {
        const initrd_file_header_t* fh = &initrd_files[i];
        if (strncmp_local(fh->name, name, INITRD_MAX_FILENAME) == 0) {
            if (size_out) *size_out = fh->size;
            return initrd_base + fh->offset;
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
const uint8_t* initrd_image(uint64_t* size_out) {
    if (!initrd_base) return 0;
    if (size_out) *size_out = initrd_size;
    return initrd_base;
}

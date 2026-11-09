// ============================================================================
// dynlink.c — динамический линкер x86_64 для AMPLIFY-OS (этап 40 группа E).
// ----------------------------------------------------------------------------
// Цель: загрузить ELF64 shared object (libc.so) в адресное пространство
// процесса, прочитать PT_DYNAMIC, применить relocations:
//   * R_X86_64_RELATIVE  — *addr = base + addend
//   * R_X86_64_GLOB_DAT  — *addr = symaddr
//   * R_X86_64_JUMP_SLOT — *addr = symaddr
//
// Модель:
//   * kernel сам выполняет relocations (а не userspace ld.so) — проще, не
//     требует отдельного .interp + /lib/ld.so.
//   * Поиск символов: DT_GNU_HASH (GNU_HASH есть в нашем libc.so) либо
//     DT_HASH как fallback.
//   * Зависимости DT_NEEDED: на 40-м не реализуем загрузку вложенных
//     зависимостей — только ведущая библиотека. Глобальной таблицы
//     символов нет — символ из libc.so ищется в той же библиотеке.
//   * Пути: dynlink_load_from_vfs(name) ищет в VFS как
//     "/libc.so", "/disk/nexus/lib/libc.so", "/libc.so.1".
//
// Интеграция:
//   * elf.c:elf64_load больше не ограничивается ET_EXEC без PT_INTERP —
//     если есть PT_INTERP, это динамическая программа, и dynlink_load_so
//     загружает указанную в DT_NEEDED библиотеку. Но на 40-м главный
//     userspace ELF у нас statically-linked, так что на runtime-пути
//     PT_INTERP обычно нет.  Функциональная роль dynlink_load_so —
//     явный entry point для тестов.
// ============================================================================
#include <stdint.h>
#include <stdbool.h>
#include "serial.h"
#include "pmm.h"
#include "vm.h"
#include "vfs.h"
#include "elf.h"
#include "heap.h"

// ---- x86_64 ELF64 расширения: динамика, relocations, gnu-hash -------------
#define DT_NULL         0
#define DT_NEEDED       1
#define DT_PLTRELSZ     2
#define DT_PLTGOT       3
#define DT_HASH         4
#define DT_STRTAB       5
#define DT_SYMTAB       6
#define DT_RELA         7
#define DT_RELASZ       8
#define DT_RELAENT      9
#define DT_STRSZ        10
#define DT_SYMENT       11
#define DT_INIT         12
#define DT_FINI         13
#define DT_SONAME       14
#define DT_RPATH        15
#define DT_SYMBOLIC     16
#define DT_REL          17
#define DT_RELSZ        18
#define DT_RELENT       19
#define DT_PLTREL       20
#define DT_DEBUG        21
#define DT_TEXTREL      22
#define DT_JMPREL       23
#define DT_GNU_HASH     0x6FFFFEF5

#define R_X86_64_NONE      0
#define R_X86_64_64        1
#define R_X86_64_PC32      2
#define R_X86_64_GOT32     3
#define R_X86_64_PLT32     4
#define R_X86_64_COPY      5
#define R_X86_64_GLOB_DAT  6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE  8

#define ELF64_R_SYM(i)    ((i) >> 32)
#define ELF64_R_TYPE(i)   ((i) & 0xFFFFFFFF)

typedef struct __attribute__((packed)) {
    int64_t  d_tag;
    uint64_t d_val;
} Elf64_Dyn;

typedef struct __attribute__((packed)) {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} Elf64_Rela;

typedef struct __attribute__((packed)) {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} Elf64_Sym;

// ---- Структура загруженной SO ---------------------------------------------
#define DYNLINK_MAX_LIBS 4

typedef struct {
    char     name[64];
    uint64_t base;
    uint64_t end;
    uint64_t dyn_tags[64];           // indexed by d_tag
    uint64_t strtab;
    uint64_t symtab;
    uint32_t strsz;
    uint32_t syment;
    uint64_t rela;
    uint32_t rela_size;
    uint64_t jmprel;
    uint32_t jmprel_size;
    uint32_t pltrel;                 // DT_RELA or DT_REL
    uint64_t hash;                   // DT_HASH
    uint64_t gnu_hash;               // DT_GNU_HASH
    bool     loaded;
} dl_lib_t;

static dl_lib_t g_libs[DYNLINK_MAX_LIBS];
static int      g_lib_count = 0;

// ---- string helpers --------------------------------------------------------
static int dl_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void dl_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src && src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// ---- Поиск символа в библиотеке -------------------------------------------
// Использует DT_GNU_HASH, если есть, иначе DT_HASH. Возвращает st_value,
// добавленный к base, либо 0 если не найден.
static uint64_t dl_lookup(dl_lib_t* lib, const char* name) {
    if (!lib || !name) return 0;
    const Elf64_Sym* syms = (const Elf64_Sym*)(uintptr_t)lib->symtab;
    const char* strs      = (const char*)(uintptr_t)lib->strtab;
    if (!syms || !strs) return 0;

    // На 40-м используем LINEAR SCAN — надёжнее чем GNU_HASH reader для
    // первой итерации. 256 символов * strcmp — несколько микросекунд.
    uint32_t sym_count = lib->syment ? (uint32_t)((lib->strtab - lib->symtab) / lib->syment) : 0;
    if (sym_count > 4096) sym_count = 4096;     // safety cap
    for (uint32_t i = 0; i < sym_count; i++) {
        const Elf64_Sym* s = &syms[i];
        if (s->st_name == 0 || s->st_shndx == 0) continue;
        const char* sname = strs + s->st_name;
        if (dl_strcmp(sname, name) == 0) return lib->base + s->st_value;
    }
    return 0;
}

// ---- Применение RELA-табицы -----------------------------------------------
static int dl_apply_rela(dl_lib_t* lib, uint64_t rela_addr, uint32_t rela_size) {
    if (!rela_addr || !rela_size) return 0;
    const Elf64_Rela* rels = (const Elf64_Rela*)(uintptr_t)rela_addr;
    uint32_t count = rela_size / sizeof(Elf64_Rela);
    int applied = 0;
    const Elf64_Sym* syms = (const Elf64_Sym*)(uintptr_t)lib->symtab;
    const char* strs      = (const char*)(uintptr_t)lib->strtab;
    for (uint32_t i = 0; i < count; i++) {
        const Elf64_Rela* r = &rels[i];
        uint64_t type  = ELF64_R_TYPE(r->r_info);
        uint64_t sym   = ELF64_R_SYM(r->r_info);
        uint64_t* tgt  = (uint64_t*)(uintptr_t)(lib->base + r->r_offset);
        uint64_t sym_val = 0;
        if (sym != 0 && syms) {
            const Elf64_Sym* s = &syms[sym];
            if (s->st_shndx != 0) {
                sym_val = lib->base + s->st_value;
            } else if (strs) {
                // UND symbol — интра-либ lookup (для libc.so всё внутри себя).
                sym_val = dl_lookup(lib, strs + s->st_name);
            }
        }
        switch (type) {
        case R_X86_64_RELATIVE:
            *tgt = lib->base + (uint64_t)r->r_addend;
            applied++;
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            if (sym_val) { *tgt = sym_val; applied++; }
            break;
        case R_X86_64_64:
            *tgt = sym_val + (uint64_t)r->r_addend;
            applied++;
            break;
        case R_X86_64_NONE:
            break;
        default:
            // Неизвестный тип — отмечаем, но не ломаемся.
            serial_puts("[dynlink] unknown reloc type ");
            serial_print_dec(type);
            serial_puts("\n");
            break;
        }
    }
    return applied;
}

// ---- Парсинг PT_DYNAMIC ---------------------------------------------------
static void dl_parse_dynamic(dl_lib_t* lib, const Elf64_Dyn* dyn) {
    while (dyn->d_tag != DT_NULL) {
        switch (dyn->d_tag) {
        case DT_STRTAB:    lib->strtab       = lib->base + dyn->d_val; break;
        case DT_SYMTAB:    lib->symtab       = lib->base + dyn->d_val; break;
        case DT_STRSZ:     lib->strsz        = (uint32_t)dyn->d_val; break;
        case DT_SYMENT:    lib->syment       = (uint32_t)dyn->d_val; break;
        case DT_RELA:      lib->rela         = lib->base + dyn->d_val; break;
        case DT_RELASZ:    lib->rela_size    = (uint32_t)dyn->d_val; break;
        case DT_JMPREL:    lib->jmprel       = lib->base + dyn->d_val; break;
        case DT_PLTRELSZ:  lib->jmprel_size  = (uint32_t)dyn->d_val; break;
        case DT_PLTREL:    lib->pltrel       = (uint32_t)dyn->d_val; break;
        case DT_HASH:      lib->hash         = lib->base + dyn->d_val; break;
        case DT_GNU_HASH:  lib->gnu_hash     = lib->base + dyn->d_val; break;
        default: break;
        }
        dyn++;
    }
}

// ============================================================================
// Публичный API
// ============================================================================
void dynlink_init(void) {
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        g_libs[i].loaded = false;
    }
    g_lib_count = 0;
    serial_puts("[dynlink] x86_64 dynamic linker initialized\n");
}

// Загрузить ELF-шарик как PIE-shared object в vm_space.
// Возвращает указатель на дескриптор библиотеки или NULL.
// base — куда грузить. vm — адресное пространство процесса.
dl_lib_t* dynlink_load_so(const char* name, const uint8_t* blob,
                          uint64_t blob_size, uint64_t base,
                          vm_space_t* vm) {
    if (g_lib_count >= DYNLINK_MAX_LIBS) return 0;
    if (blob_size < sizeof(Elf64_Ehdr)) return 0;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)blob;
    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) return 0;
    if (eh->e_type != ET_DYN) return 0;

    dl_lib_t* lib = &g_libs[g_lib_count++];
    lib->base = base;
    dl_strncpy(lib->name, name, 64);

    const Elf64_Phdr* phs = (const Elf64_Phdr*)(blob + eh->e_phoff);
    uint64_t max_end = 0;

    // 1) Загружаем PT_LOAD сегменты.
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = &phs[i];
        if (ph->p_type != PT_LOAD) continue;
        uint64_t va_start = (base + ph->p_vaddr) & ~0xFFFULL;
        uint64_t va_end   = (base + ph->p_vaddr + ph->p_memsz + 0xFFF) & ~0xFFFULL;
        bool writable = (ph->p_flags & PF_W) != 0;
        for (uint64_t va = va_start; va < va_end; va += 0x1000) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) return 0;
            uint8_t* pg = (uint8_t*)(uintptr_t)phys;
            for (int z = 0; z < 0x1000; z++) pg[z] = 0;
            // Копируем из ELF blob.
            int64_t off_in_seg = (int64_t)va - (int64_t)(base + ph->p_vaddr);
            uint64_t copy_dst = 0, copy_src = ph->p_offset, copy_len = 0;
            if (off_in_seg < 0) {
                copy_dst = (uint64_t)(-off_in_seg);
                copy_len = 0x1000 - copy_dst;
                if (copy_len > ph->p_filesz) copy_len = ph->p_filesz;
            } else if ((uint64_t)off_in_seg < ph->p_filesz) {
                copy_src = ph->p_offset + (uint64_t)off_in_seg;
                uint64_t left = ph->p_filesz - (uint64_t)off_in_seg;
                copy_len = (left < 0x1000) ? left : 0x1000;
            }
            for (uint64_t b = 0; b < copy_len; b++) {
                pg[copy_dst + b] = blob[copy_src + b];
            }
            if (!vm_map_user_page(vm, va, phys, writable)) return 0;
        }
        if (va_end > max_end) max_end = va_end;
    }
    lib->end = max_end;

    // 2) Парсим PT_DYNAMIC.
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = &phs[i];
        if (ph->p_type != PT_DYNAMIC) continue;
        const Elf64_Dyn* dyn = (const Elf64_Dyn*)(uintptr_t)(base + ph->p_vaddr);
        dl_parse_dynamic(lib, dyn);
        break;
    }

    // 3) Применяем relocations.
    int rela = dl_apply_rela(lib, lib->rela, lib->rela_size);
    int plt  = dl_apply_rela(lib, lib->jmprel, lib->jmprel_size);
    serial_puts("[dynlink] loaded ");
    serial_puts(lib->name);
    serial_puts(" base=0x");
    serial_print_hex(base, 16);
    serial_puts(" rela=");
    serial_print_dec(rela);
    serial_puts(" plt=");
    serial_print_dec(plt);
    serial_puts("\n");

    lib->loaded = true;
    return lib;
}

// Поиск символа по имени во всех загруженных библиотеках.
uint64_t dynlink_resolve(const char* name) {
    for (int i = 0; i < g_lib_count; i++) {
        if (!g_libs[i].loaded) continue;
        uint64_t a = dl_lookup(&g_libs[i], name);
        if (a) return a;
    }
    return 0;
}

// Список загруженных библиотек — для отладки.
int dynlink_list(dl_lib_t** out, int max) {
    int n = 0;
    for (int i = 0; i < g_lib_count && n < max; i++) {
        if (g_libs[i].loaded) out[n++] = &g_libs[i];
    }
    return n;
}

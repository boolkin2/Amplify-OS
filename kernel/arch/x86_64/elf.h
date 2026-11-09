// ============================================================================
// elf.h — минимальный ELF64 loader для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Поддерживает только то, что нужно для загрузки statically-linked ET_EXEC
// программ (наш userspace на 37.4b). PT_DYNAMIC и PT_INTERP игнорируются —
// dynamic linking появится в этапе 37.4c.
// ============================================================================
#ifndef AOS_X86_64_ELF_H
#define AOS_X86_64_ELF_H

#include <stdint.h>
#include <stdbool.h>
#include "vm.h"

// ----------------------------------------------------------------------------
// ELF64 header
// ----------------------------------------------------------------------------
#define EI_NIDENT        16
#define EI_MAG0          0
#define ELFMAG0          0x7F
#define ELFMAG1          'E'
#define ELFMAG2          'L'
#define ELFMAG3          'F'
#define EI_CLASS         4
#define ELFCLASS64       2
#define EI_DATA          5
#define ELFDATA2LSB      1

#define ET_EXEC          2
#define ET_DYN           3
#define EM_X86_64        62

typedef struct __attribute__((packed)) {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

// Program header
#define PT_NULL          0
#define PT_LOAD          1
#define PT_DYNAMIC       2
#define PT_INTERP        3
#define PT_NOTE          4
#define PT_PHDR          6
#define PT_TLS           7
#define PT_GNU_STACK     0x6474E551

#define PF_X             1
#define PF_W             2
#define PF_R             4

typedef struct __attribute__((packed)) {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

// ----------------------------------------------------------------------------
// Загрузка ELF64 в указанное vm_space.
// На входе: blob = указатель на ELF в kernel-памяти, blob_size = размер.
// На выходе: возвращает entry-point (e_entry из заголовка), либо 0 при ошибке.
// Параллельно маппит все PT_LOAD сегменты в vm с правильными флагами PG_*
// и копирует данные из blob в выделенные страницы.
// ----------------------------------------------------------------------------
uint64_t elf64_load(const uint8_t* blob, uint64_t blob_size, vm_space_t* vm);

#endif

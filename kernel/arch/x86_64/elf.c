// ============================================================================
// elf.c — реализация ELF64 загрузчика.
// ============================================================================
#include "elf.h"
#include "pmm.h"
#include "memory.h"
#include "serial.h"

static bool elf64_validate(const uint8_t* blob, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) return false;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)blob;

    if (eh->e_ident[0] != ELFMAG0 || eh->e_ident[1] != ELFMAG1 ||
        eh->e_ident[2] != ELFMAG2 || eh->e_ident[3] != ELFMAG3) {
        serial_puts("[elf] bad magic\n");
        return false;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) {
        serial_puts("[elf] not ELF64\n");
        return false;
    }
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) {
        serial_puts("[elf] not LSB\n");
        return false;
    }
    if (eh->e_machine != EM_X86_64) {
        serial_puts("[elf] not x86_64\n");
        return false;
    }
    if (eh->e_type != ET_EXEC) {
        serial_puts("[elf] not ET_EXEC\n");
        return false;
    }
    if (eh->e_phoff == 0 || eh->e_phnum == 0) {
        serial_puts("[elf] no program headers\n");
        return false;
    }
    if (eh->e_phoff + eh->e_phentsize * eh->e_phnum > size) {
        serial_puts("[elf] phdr table out of bounds\n");
        return false;
    }
    return true;
}

uint64_t elf64_load(const uint8_t* blob, uint64_t blob_size, vm_space_t* vm) {
    if (!elf64_validate(blob, blob_size)) return 0;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)blob;

    serial_puts("[elf] loading: entry=0x");
    serial_print_hex(eh->e_entry, 16);
    serial_puts(" phnum="); serial_print_dec(eh->e_phnum); serial_puts("\n");

    const Elf64_Phdr* phs = (const Elf64_Phdr*)(blob + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = &phs[i];
        if (ph->p_type != PT_LOAD) continue;

        // Проверка адресных границ — должно быть в user-range.
        if (ph->p_vaddr < USER_BASE) {
            serial_puts("[elf] segment vaddr below USER_BASE — refused\n");
            return 0;
        }

        bool writable = (ph->p_flags & PF_W) != 0;

        // Page-aligned диапазон [va_start, va_end) для этого сегмента.
        uint64_t va_start = ph->p_vaddr & ~PAGE_MASK;
        uint64_t va_end   = (ph->p_vaddr + ph->p_memsz + PAGE_MASK) & ~PAGE_MASK;

        serial_puts("[elf]   PT_LOAD vaddr=0x"); serial_print_hex(ph->p_vaddr, 16);
        serial_puts(" filesz="); serial_print_dec(ph->p_filesz);
        serial_puts(" memsz=");  serial_print_dec(ph->p_memsz);
        serial_puts(" flags=");
        if (ph->p_flags & PF_R) serial_putc('R'); else serial_putc('-');
        if (ph->p_flags & PF_W) serial_putc('W'); else serial_putc('-');
        if (ph->p_flags & PF_X) serial_putc('X'); else serial_putc('-');
        serial_puts("\n");

        for (uint64_t va = va_start; va < va_end; va += PAGE_SIZE) {
            uint64_t phys = pmm_alloc_page();
            if (!phys) {
                serial_puts("[elf] OOM during PT_LOAD\n");
                return 0;
            }

            uint8_t* page = (uint8_t*)(uintptr_t)phys;

            // 1) Зануляем целиком (BSS-часть остаётся нулями).
            for (uint64_t z = 0; z < PAGE_SIZE; z++) page[z] = 0;

            // 2) Копируем ту часть, которая в файле (file backed = filesz).
            //    page_offset_in_seg = va - p_vaddr (может быть отрицательным
            //    для первой страницы если p_vaddr не выровнен).
            int64_t page_off_in_seg = (int64_t)va - (int64_t)ph->p_vaddr;
            uint64_t copy_dst = 0;
            uint64_t copy_src_in_file = ph->p_offset;
            uint64_t copy_len = 0;

            if (page_off_in_seg < 0) {
                // p_vaddr не page-aligned — начало страницы перед сегментом.
                copy_dst = (uint64_t)(-page_off_in_seg);
                copy_src_in_file = ph->p_offset;
                copy_len = PAGE_SIZE - copy_dst;
                if (copy_len > ph->p_filesz) copy_len = ph->p_filesz;
            } else if ((uint64_t)page_off_in_seg < ph->p_filesz) {
                copy_dst = 0;
                copy_src_in_file = ph->p_offset + (uint64_t)page_off_in_seg;
                uint64_t left = ph->p_filesz - (uint64_t)page_off_in_seg;
                copy_len = (left < PAGE_SIZE) ? left : PAGE_SIZE;
            }

            if (copy_len > 0) {
                if (copy_src_in_file + copy_len > blob_size) {
                    serial_puts("[elf] segment file bytes outside blob\n");
                    pmm_free_page(phys);
                    return 0;
                }
                for (uint64_t b = 0; b < copy_len; b++) {
                    page[copy_dst + b] = blob[copy_src_in_file + b];
                }
            }

            // 3) Маппим в vm как user-page.
            if (!vm_map_user_page(vm, va, phys, writable)) {
                serial_puts("[elf] vm_map_user_page failed\n");
                pmm_free_page(phys);
                return 0;
            }
        }
    }

    return eh->e_entry;
}

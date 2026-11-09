// ============================================================================
// elf.c - ELF32 загрузчик для MyOS (Этап 18)
// ============================================================================
// Полная реализация парсинга и загрузки ELF32 файлов.
// Поддерживает:
//   - Валидацию заголовка (magic, class, architecture)
//   - Загрузку сегментов PT_LOAD в память с правильными правами
//   - Поиск символов и секций
//   - Вывод отладочной информации
// ============================================================================

#include "elf.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../memory/paging.h"
#include "../memory/pmm.h"
#include "../vga.h"

// ============================================================================
// Вспомогательные функции для строк (не зависим от libc)
// ============================================================================

// Сравнение строк
static int elf_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

// ============================================================================
// Валидация ELF заголовка
// ============================================================================

bool elf_validate(const void* data, uint32_t size) {
    // Проверяем минимальный размер (должен вмещать хотя бы заголовок)
    if (!data || size < sizeof(elf32_header_t)) {
        return false;
    }

    const elf32_header_t* hdr = (const elf32_header_t*)data;

    // Проверяем магическое число: 0x7F 'E' 'L' 'F'
    if (!ELF_CHECK_MAGIC(hdr)) {
        return false;
    }

    // Проверяем класс (должен быть 32-бит)
    if (hdr->e_ident[EI_CLASS] != ELFCLASS32) {
        return false;
    }

    // Проверяем порядок байтов (должен быть little-endian для x86)
    if (hdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }

    // Проверяем архитектуру (должна быть i386)
    if (hdr->e_machine != EM_386) {
        return false;
    }

    // Проверяем тип файла (допускаем EXEC и DYN)
    if (hdr->e_type != ET_EXEC && hdr->e_type != ET_DYN) {
        return false;
    }

    // Проверяем наличие программных заголовков
    if (hdr->e_phnum == 0 || hdr->e_phoff == 0) {
        return false;
    }

    // Проверяем что таблица программных заголовков не выходит за границы файла
    uint32_t ph_end = hdr->e_phoff + (uint32_t)hdr->e_phnum * hdr->e_phentsize;
    if (ph_end > size) {
        return false;
    }

    return true;
}

// ============================================================================
// Загрузка ELF файла в память
// ============================================================================

elf_load_result_t elf_load(const void* data, uint32_t size) {
    elf_load_result_t result;
    memset(&result, 0, sizeof(result));

    // Валидируем заголовок
    if (!elf_validate(data, size)) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Oshibka validatsii zagolovka");
        return result;
    }

    const elf32_header_t* hdr = (const elf32_header_t*)data;
    const uint8_t* file_data = (const uint8_t*)data;

    // Сохраняем точку входа
    result.entry_point = hdr->e_entry;
    result.base_addr = 0xFFFFFFFF;
    result.end_addr = 0;

    // Проходим по всем программным заголовкам
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        // Вычисляем указатель на i-й программный заголовок
        const elf32_phdr_t* phdr = (const elf32_phdr_t*)
            (file_data + hdr->e_phoff + i * hdr->e_phentsize);

        // Загружаем только сегменты типа PT_LOAD
        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        // Проверяем что сегмент не пустой
        if (phdr->p_memsz == 0) {
            continue;
        }

        // Проверяем что данные сегмента не выходят за пределы файла
        if (phdr->p_offset + phdr->p_filesz > size) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            vga_println("[ELF] Segment vyhodit za predely fajla");
            return result;
        }

        // Вычисляем флаги страниц для маппинга
        uint32_t page_flags = PTE_PRESENT | PTE_USER;
        if (phdr->p_flags & PF_W) {
            page_flags |= PTE_WRITABLE;
        }

        // Выравниваем адреса по границе страницы
        uint32_t seg_start = phdr->p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t seg_end = (phdr->p_vaddr + phdr->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        uint32_t num_pages = (seg_end - seg_start) / PAGE_SIZE;

        // Выделяем и маппим страницы для сегмента
        for (uint32_t p = 0; p < num_pages; p++) {
            uint32_t vaddr = seg_start + p * PAGE_SIZE;

            // Проверяем, не замаплена ли уже страница
            page_directory_t* cur_dir = paging_get_current_directory();
            if (!paging_is_mapped(cur_dir, vaddr)) {
                // Выделяем физическую страницу
                uint32_t phys = pmm_alloc_page();
                if (phys == 0) {
                    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    vga_println("[ELF] Net svobodnyh stranits pamyati");
                    return result;
                }
                // Маппим виртуальную страницу на физическую
                paging_map_page(cur_dir, vaddr, phys, page_flags);
            }
        }

        // Обнуляем всю область сегмента (важно для BSS!)
        memset((void*)phdr->p_vaddr, 0, phdr->p_memsz);

        // Копируем данные из файла в память
        if (phdr->p_filesz > 0) {
            memcpy((void*)phdr->p_vaddr,
                   file_data + phdr->p_offset,
                   phdr->p_filesz);
        }

        // Обновляем границы загруженного образа
        if (phdr->p_vaddr < result.base_addr) {
            result.base_addr = phdr->p_vaddr;
        }
        uint32_t seg_top = phdr->p_vaddr + phdr->p_memsz;
        if (seg_top > result.end_addr) {
            result.end_addr = seg_top;
        }

        // Сохраняем информацию о сегменте
        if (result.segment_count < ELF_MAX_SEGMENTS) {
            result.segments[result.segment_count].vaddr = phdr->p_vaddr;
            result.segments[result.segment_count].memsz = phdr->p_memsz;
            result.segments[result.segment_count].flags = phdr->p_flags;
            result.segment_count++;
        }
    }

    // Проверяем что хотя бы один сегмент загружен
    if (result.segment_count == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Net zagruzhaemyh segmentov (PT_LOAD)");
        return result;
    }

    result.valid = true;
    return result;
}

// ============================================================================
// Получение точки входа
// ============================================================================

uint32_t elf_get_entry(const void* data) {
    if (!data) return 0;
    const elf32_header_t* hdr = (const elf32_header_t*)data;
    if (!ELF_CHECK_MAGIC(hdr)) return 0;
    return hdr->e_entry;
}

// ============================================================================
// Получение типа ELF файла
// ============================================================================

uint16_t elf_get_type(const void* data) {
    if (!data) return 0;
    const elf32_header_t* hdr = (const elf32_header_t*)data;
    if (!ELF_CHECK_MAGIC(hdr)) return 0;
    return hdr->e_type;
}

// ============================================================================
// Поиск секции по типу
// ============================================================================

const elf32_shdr_t* elf_find_section(const void* data, uint32_t sh_type) {
    if (!data) return NULL;

    const elf32_header_t* hdr = (const elf32_header_t*)data;
    if (!ELF_CHECK_MAGIC(hdr)) return NULL;

    // Проверяем наличие заголовков секций
    if (hdr->e_shnum == 0 || hdr->e_shoff == 0) return NULL;

    const uint8_t* file_data = (const uint8_t*)data;

    // Проходим по всем секциям
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf32_shdr_t* shdr = (const elf32_shdr_t*)
            (file_data + hdr->e_shoff + i * hdr->e_shentsize);

        if (shdr->sh_type == sh_type) {
            return shdr;
        }
    }

    return NULL;
}

// ============================================================================
// Поиск секции по имени
// ============================================================================

const elf32_shdr_t* elf_find_section_by_name(const void* data, const char* name) {
    if (!data || !name) return NULL;

    const elf32_header_t* hdr = (const elf32_header_t*)data;
    if (!ELF_CHECK_MAGIC(hdr)) return NULL;
    if (hdr->e_shnum == 0 || hdr->e_shoff == 0) return NULL;
    if (hdr->e_shstrndx == 0) return NULL;

    const uint8_t* file_data = (const uint8_t*)data;

    // Получаем секцию строк заголовков секций (.shstrtab)
    const elf32_shdr_t* shstrtab = (const elf32_shdr_t*)
        (file_data + hdr->e_shoff + hdr->e_shstrndx * hdr->e_shentsize);
    const char* strtab = (const char*)(file_data + shstrtab->sh_offset);

    // Проходим по всем секциям
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf32_shdr_t* shdr = (const elf32_shdr_t*)
            (file_data + hdr->e_shoff + i * hdr->e_shentsize);

        const char* sec_name = strtab + shdr->sh_name;
        if (elf_strcmp(sec_name, name) == 0) {
            return shdr;
        }
    }

    return NULL;
}

// ============================================================================
// Поиск символа по имени
// ============================================================================

uint32_t elf_find_symbol(const void* data, const char* name) {
    if (!data || !name) return 0;

    const elf32_header_t* hdr = (const elf32_header_t*)data;
    if (!ELF_CHECK_MAGIC(hdr)) return 0;
    if (hdr->e_shnum == 0) return 0;

    const uint8_t* file_data = (const uint8_t*)data;

    // Ищем секцию таблицы символов (.symtab)
    const elf32_shdr_t* symtab_hdr = elf_find_section(data, SHT_SYMTAB);
    if (!symtab_hdr) {
        // Пробуем динамическую таблицу символов (.dynsym)
        symtab_hdr = elf_find_section(data, SHT_DYNSYM);
        if (!symtab_hdr) return 0;
    }

    // Получаем связанную таблицу строк
    if (symtab_hdr->sh_link == 0 || symtab_hdr->sh_link >= hdr->e_shnum) return 0;

    const elf32_shdr_t* strtab_hdr = (const elf32_shdr_t*)
        (file_data + hdr->e_shoff + symtab_hdr->sh_link * hdr->e_shentsize);
    const char* strtab = (const char*)(file_data + strtab_hdr->sh_offset);

    // Вычисляем количество символов
    uint32_t sym_count = symtab_hdr->sh_size / sizeof(elf32_sym_t);
    const elf32_sym_t* symbols = (const elf32_sym_t*)
        (file_data + symtab_hdr->sh_offset);

    // Ищем символ по имени
    for (uint32_t i = 0; i < sym_count; i++) {
        const char* sym_name = strtab + symbols[i].st_name;
        if (elf_strcmp(sym_name, name) == 0) {
            return symbols[i].st_value;
        }
    }

    return 0;
}

// ============================================================================
// Вывод информации о ELF файле (отладка)
// ============================================================================

void elf_print_info(const void* data) {
    if (!data) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Dannyye ne peredany");
        return;
    }

    const elf32_header_t* hdr = (const elf32_header_t*)data;

    if (!ELF_CHECK_MAGIC(hdr)) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Nevalidnoe magicheskoe chislo");
        return;
    }

    const uint8_t* file_data = (const uint8_t*)data;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("=== ELF Info ===");

    // Тип файла
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Tip:       ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    switch (hdr->e_type) {
        case ET_EXEC: vga_println("Ispolnyaemyj (ET_EXEC)"); break;
        case ET_DYN:  vga_println("Razdelyaemyj ob'ekt (ET_DYN)"); break;
        case ET_REL:  vga_println("Relotsiruemyj (ET_REL)"); break;
        default:      vga_println("Neizvestnyj"); break;
    }

    // Архитектура
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Arhit:     ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    switch (hdr->e_machine) {
        case EM_386:    vga_println("i386 (x86)"); break;
        case EM_X86_64: vga_println("x86_64"); break;
        default:
            vga_puts("Drugaya (");
            vga_print_dec(hdr->e_machine);
            vga_println(")");
            break;
    }

    // Точка входа
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Vhod:      0x");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print_hex(hdr->e_entry);
    vga_println("");

    // Количество программных заголовков
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Segmenty:  ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(hdr->e_phnum);
    vga_println("");

    // Количество секций
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Sektsii:   ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(hdr->e_shnum);
    vga_println("");

    // Информация о сегментах PT_LOAD
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("  --- Zagruzhaemye segmenty ---");

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const elf32_phdr_t* phdr = (const elf32_phdr_t*)
            (file_data + hdr->e_phoff + i * hdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;

        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("  [");
        vga_print_dec(i);
        vga_puts("] vaddr=0x");
        vga_print_hex(phdr->p_vaddr);
        vga_puts(" memsz=");
        vga_print_dec(phdr->p_memsz);
        vga_puts(" flags=");
        if (phdr->p_flags & PF_R) vga_puts("R");
        if (phdr->p_flags & PF_W) vga_puts("W");
        if (phdr->p_flags & PF_X) vga_puts("X");
        vga_println("");
    }
}

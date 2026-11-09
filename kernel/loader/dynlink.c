// ============================================================================
// dynlink.c - Реализация динамического линкера для MyOS (Этап 18)
// ============================================================================
// Управляет загрузкой разделяемых библиотек (.so) и разрешением символов.
//
// Алгоритм работы:
// 1. Библиотека загружается в память по фиксированному адресу
// 2. Парсятся секции .symtab/.dynsym для извлечения экспортируемых символов
// 3. Символы регистрируются в глобальной таблице
// 4. При загрузке программы применяются релокации из .rel.dyn/.rel.plt
// 5. Неразрешённые символы ищутся в глобальной таблице
// ============================================================================

#include "dynlink.h"
#include "elf.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../memory/paging.h"
#include "../memory/pmm.h"
#include "../fs/vfs.h"
#include "../vga.h"

// ============================================================================
// Вспомогательные функции для строк
// ============================================================================

static int dl_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static int dl_strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

// Безопасное копирование строки с ограничением длины
static void dl_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// ============================================================================
// Глобальные данные динамического линкера
// ============================================================================

// Массив загруженных библиотек
static dynlink_lib_t loaded_libs[DYNLINK_MAX_LIBS];

// Глобальная таблица символов (объединяет все библиотеки + ядро)
static dynlink_symbol_t global_symbols[DYNLINK_MAX_GLOBAL_SYMS];
static uint32_t global_symbol_count = 0;

// Следующий свободный адрес для загрузки библиотеки
static uint32_t next_lib_addr = DYNLINK_LIB_BASE;

// Флаг инициализации
static bool dynlink_initialized = false;

// Статистика
static uint32_t total_resolved = 0;
static uint32_t total_unresolved = 0;

// ============================================================================
// Инициализация
// ============================================================================

void dynlink_init(void) {
    // Обнуляем все структуры
    memset(loaded_libs, 0, sizeof(loaded_libs));
    memset(global_symbols, 0, sizeof(global_symbols));

    global_symbol_count = 0;
    next_lib_addr = DYNLINK_LIB_BASE;
    total_resolved = 0;
    total_unresolved = 0;
    dynlink_initialized = true;

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_println("[DYNLINK] Dinamicheskij linker inicializirovan");
}

// ============================================================================
// Регистрация символа в глобальной таблице
// ============================================================================

static bool register_global_symbol(const char* name, uint32_t address,
                                    uint8_t type, uint8_t bind) {
    if (global_symbol_count >= DYNLINK_MAX_GLOBAL_SYMS) {
        return false;
    }

    // Проверяем, не зарегистрирован ли уже такой символ
    for (uint32_t i = 0; i < global_symbol_count; i++) {
        if (global_symbols[i].in_use && dl_strcmp(global_symbols[i].name, name) == 0) {
            // Обновляем существующий символ (если новый — глобальный)
            if (bind == STB_GLOBAL) {
                global_symbols[i].address = address;
                global_symbols[i].type = type;
                global_symbols[i].bind = bind;
            }
            return true;
        }
    }

    // Регистрируем новый символ
    dynlink_symbol_t* sym = &global_symbols[global_symbol_count];
    dl_strncpy(sym->name, name, DYNLINK_MAX_SYM_NAME);
    sym->address = address;
    sym->type = type;
    sym->bind = bind;
    sym->in_use = true;
    global_symbol_count++;

    return true;
}

// ============================================================================
// Извлечение символов из ELF файла и регистрация в библиотеке
// ============================================================================

static uint32_t extract_symbols(const void* elf_data, uint32_t elf_size,
                                 dynlink_lib_t* lib, uint32_t base_offset) {
    (void)elf_size;
    (void)base_offset;
    const elf32_header_t* hdr = (const elf32_header_t*)elf_data;
    const uint8_t* file_data = (const uint8_t*)elf_data;
    uint32_t count = 0;

    if (hdr->e_shnum == 0 || hdr->e_shoff == 0) return 0;

    // Ищем таблицу символов (.symtab или .dynsym)
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf32_shdr_t* shdr = (const elf32_shdr_t*)
            (file_data + hdr->e_shoff + i * hdr->e_shentsize);

        // Нас интересуют только таблицы символов
        if (shdr->sh_type != SHT_SYMTAB && shdr->sh_type != SHT_DYNSYM) {
            continue;
        }

        // Получаем связанную таблицу строк
        if (shdr->sh_link == 0 || shdr->sh_link >= hdr->e_shnum) continue;

        const elf32_shdr_t* strtab_hdr = (const elf32_shdr_t*)
            (file_data + hdr->e_shoff + shdr->sh_link * hdr->e_shentsize);
        const char* strtab = (const char*)(file_data + strtab_hdr->sh_offset);

        // Количество символов
        uint32_t sym_count = shdr->sh_size / sizeof(elf32_sym_t);
        const elf32_sym_t* symbols = (const elf32_sym_t*)
            (file_data + shdr->sh_offset);

        for (uint32_t j = 0; j < sym_count; j++) {
            // Пропускаем неопределённые и локальные символы
            if (symbols[j].st_shndx == SHN_UNDEF) continue;
            if (ELF32_ST_BIND(symbols[j].st_info) == STB_LOCAL) continue;
            if (symbols[j].st_name == 0) continue;

            const char* sym_name = strtab + symbols[j].st_name;
            if (dl_strlen(sym_name) == 0) continue;

            // Вычисляем адрес символа с учётом базы загрузки
            uint32_t sym_addr = symbols[j].st_value;

            // Регистрируем в локальной таблице библиотеки
            if (lib->symbol_count < DYNLINK_MAX_SYMBOLS) {
                dynlink_symbol_t* lsym = &lib->symbols[lib->symbol_count];
                dl_strncpy(lsym->name, sym_name, DYNLINK_MAX_SYM_NAME);
                lsym->address = sym_addr;
                lsym->type = ELF32_ST_TYPE(symbols[j].st_info);
                lsym->bind = ELF32_ST_BIND(symbols[j].st_info);
                lsym->in_use = true;
                lib->symbol_count++;
            }

            // Регистрируем в глобальной таблице
            register_global_symbol(sym_name, sym_addr,
                ELF32_ST_TYPE(symbols[j].st_info),
                ELF32_ST_BIND(symbols[j].st_info));

            count++;
        }
    }

    return count;
}

// ============================================================================
// Загрузка библиотеки из памяти
// ============================================================================

bool dynlink_load_library(const char* name, const void* data, uint32_t size) {
    if (!dynlink_initialized) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[DYNLINK] Linker ne inicializirovan!");
        return false;
    }

    if (!name || !data || size == 0) {
        return false;
    }

    // Проверяем, не загружена ли уже
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (loaded_libs[i].loaded && dl_strcmp(loaded_libs[i].name, name) == 0) {
            loaded_libs[i].ref_count++;
            return true; // Уже загружена, увеличиваем счётчик ссылок
        }
    }

    // Ищем свободный слот
    int slot = -1;
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (!loaded_libs[i].loaded) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[DYNLINK] Net svobodnyh slotov dlya bibliotek");
        return false;
    }

    // Валидируем ELF
    if (!elf_validate(data, size)) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("[DYNLINK] Nevalidnyj ELF: ");
        vga_println(name);
        return false;
    }

    // Загружаем ELF (сегменты будут загружены по адресам из ELF)
    elf_load_result_t load_result = elf_load(data, size);
    if (!load_result.valid) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("[DYNLINK] Oshibka zagruzki ELF: ");
        vga_println(name);
        return false;
    }

    // Заполняем информацию о библиотеке
    dynlink_lib_t* lib = &loaded_libs[slot];
    dl_strncpy(lib->name, name, DYNLINK_MAX_LIB_NAME);
    lib->base_addr = load_result.base_addr;
    lib->end_addr = load_result.end_addr;
    lib->size = load_result.end_addr - load_result.base_addr;
    lib->ref_count = 1;
    lib->loaded = true;
    lib->symbol_count = 0;

    // Извлекаем символы из библиотеки
    uint32_t sym_count = extract_symbols(data, size, lib, 0);

    // Применяем релокации
    uint32_t reloc_count = dynlink_relocate(data, size, lib->base_addr);

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("[DYNLINK] Zagruzhena: ");
    vga_puts(name);
    vga_puts(" (simvolov: ");
    vga_print_dec(sym_count);
    vga_puts(", relok: ");
    vga_print_dec(reloc_count);
    vga_println(")");

    return true;
}

// ============================================================================
// Загрузка библиотеки из файла
// ============================================================================

bool dynlink_load_library_from_file(const char* path) {
    if (!path) return false;

    // Открываем файл через VFS
    int fd = vfs_open_file(path, VFS_O_RDONLY);
    if (fd < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("[DYNLINK] Fajl ne najden: ");
        vga_println(path);
        return false;
    }

    // Получаем размер файла
    int file_size = vfs_get_filesize(fd);
    if (file_size <= 0) {
        vfs_close_file(fd);
        return false;
    }

    // Выделяем буфер и читаем файл
    uint8_t* buffer = (uint8_t*)kmalloc(file_size);
    if (!buffer) {
        vfs_close_file(fd);
        return false;
    }

    int bytes_read = vfs_read_file(fd, buffer, file_size);
    vfs_close_file(fd);

    if (bytes_read != file_size) {
        kfree(buffer);
        return false;
    }

    // Извлекаем имя файла из пути
    const char* name = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }

    // Загружаем библиотеку
    bool result = dynlink_load_library(name, buffer, file_size);

    kfree(buffer);
    return result;
}

// ============================================================================
// Выгрузка библиотеки
// ============================================================================

bool dynlink_unload_library(const char* name) {
    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (loaded_libs[i].loaded && dl_strcmp(loaded_libs[i].name, name) == 0) {
            loaded_libs[i].ref_count--;

            // Выгружаем только когда счётчик ссылок достигает нуля
            if (loaded_libs[i].ref_count == 0) {
                // Удаляем символы из глобальной таблицы
                for (uint32_t j = 0; j < loaded_libs[i].symbol_count; j++) {
                    for (uint32_t k = 0; k < global_symbol_count; k++) {
                        if (global_symbols[k].in_use &&
                            dl_strcmp(global_symbols[k].name,
                                     loaded_libs[i].symbols[j].name) == 0) {
                            global_symbols[k].in_use = false;
                        }
                    }
                }

                // Помечаем как незагруженную
                loaded_libs[i].loaded = false;
                loaded_libs[i].symbol_count = 0;
                memset(loaded_libs[i].name, 0, DYNLINK_MAX_LIB_NAME);

                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                vga_puts("[DYNLINK] Vygruzhena: ");
                vga_println(name);
            }

            return true;
        }
    }

    return false;
}

// ============================================================================
// Разрешение символа
// ============================================================================

uint32_t dynlink_resolve(const char* name) {
    if (!name) return 0;

    // Ищем в глобальной таблице символов
    for (uint32_t i = 0; i < global_symbol_count; i++) {
        if (global_symbols[i].in_use && dl_strcmp(global_symbols[i].name, name) == 0) {
            total_resolved++;
            return global_symbols[i].address;
        }
    }

    total_unresolved++;
    return 0;
}

// ============================================================================
// Регистрация символа ядра
// ============================================================================

bool dynlink_register_kernel_symbol(const char* name, uint32_t address, uint8_t type) {
    return register_global_symbol(name, address, type, STB_GLOBAL);
}

// ============================================================================
// Применение релокаций
// ============================================================================

uint32_t dynlink_relocate(const void* elf_data, uint32_t elf_size, uint32_t base_addr) {
    (void)elf_size;
    if (!elf_data) return 0;

    const elf32_header_t* hdr = (const elf32_header_t*)elf_data;
    if (!ELF_CHECK_MAGIC(hdr)) return 0;
    if (hdr->e_shnum == 0) return 0;

    const uint8_t* file_data = (const uint8_t*)elf_data;
    uint32_t reloc_count = 0;

    // Проходим по всем секциям в поисках релокаций
    for (uint16_t i = 0; i < hdr->e_shnum; i++) {
        const elf32_shdr_t* shdr = (const elf32_shdr_t*)
            (file_data + hdr->e_shoff + i * hdr->e_shentsize);

        // Обрабатываем только секции релокаций
        if (shdr->sh_type != SHT_REL) continue;

        // Получаем связанную таблицу символов
        if (shdr->sh_link == 0 || shdr->sh_link >= hdr->e_shnum) continue;

        const elf32_shdr_t* symtab_hdr = (const elf32_shdr_t*)
            (file_data + hdr->e_shoff + shdr->sh_link * hdr->e_shentsize);
        const elf32_sym_t* symtab = (const elf32_sym_t*)
            (file_data + symtab_hdr->sh_offset);

        // Получаем таблицу строк для символов
        const elf32_shdr_t* strtab_hdr = (const elf32_shdr_t*)
            (file_data + hdr->e_shoff + symtab_hdr->sh_link * hdr->e_shentsize);
        const char* strtab = (const char*)(file_data + strtab_hdr->sh_offset);

        // Количество записей релокаций
        uint32_t rel_count = shdr->sh_size / sizeof(elf32_rel_t);
        const elf32_rel_t* rels = (const elf32_rel_t*)
            (file_data + shdr->sh_offset);

        // Применяем каждую релокацию
        for (uint32_t j = 0; j < rel_count; j++) {
            uint32_t sym_idx = ELF32_R_SYM(rels[j].r_info);
            uint8_t  rel_type = ELF32_R_TYPE(rels[j].r_info);
            uint32_t offset = rels[j].r_offset;

            // Получаем адрес символа
            uint32_t sym_value = 0;

            if (sym_idx != 0) {
                const elf32_sym_t* sym = &symtab[sym_idx];

                if (sym->st_shndx == SHN_UNDEF) {
                    // Символ не определён — ищем в глобальной таблице
                    const char* sym_name = strtab + sym->st_name;
                    sym_value = dynlink_resolve(sym_name);

                    if (sym_value == 0) {
                        // Символ не найден
                        continue;
                    }
                } else {
                    sym_value = sym->st_value;
                }
            }

            // Применяем релокацию в зависимости от типа
            uint32_t* target = (uint32_t*)offset;

            switch (rel_type) {
                case R_386_32:
                    // S + A: абсолютная релокация
                    *target += sym_value;
                    reloc_count++;
                    break;

                case R_386_PC32:
                    // S + A - P: относительная релокация
                    *target += sym_value - offset;
                    reloc_count++;
                    break;

                case R_386_GLOB_DAT:
                case R_386_JMP_SLOT:
                    // S: прямая подстановка адреса
                    *target = sym_value;
                    reloc_count++;
                    break;

                case R_386_RELATIVE:
                    // B + A: базовая релокация
                    *target += base_addr;
                    reloc_count++;
                    break;

                default:
                    // Неизвестный тип — пропускаем
                    break;
            }
        }
    }

    return reloc_count;
}

// ============================================================================
// Получение информации
// ============================================================================

dynlink_info_t dynlink_get_info(void) {
    dynlink_info_t info;
    memset(&info, 0, sizeof(info));

    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (loaded_libs[i].loaded) {
            info.loaded_libs++;
        }
    }

    info.total_symbols = global_symbol_count;
    info.resolved_count = total_resolved;
    info.unresolved_count = total_unresolved;
    info.next_load_addr = next_lib_addr;

    return info;
}

// ============================================================================
// Поиск библиотеки
// ============================================================================

const dynlink_lib_t* dynlink_find_library(const char* name) {
    if (!name) return NULL;

    for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
        if (loaded_libs[i].loaded && dl_strcmp(loaded_libs[i].name, name) == 0) {
            return &loaded_libs[i];
        }
    }

    return NULL;
}

// ============================================================================
// Вывод информации о библиотеках
// ============================================================================

void dynlink_print_info(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("=== Dinamicheskij Linker - Info ===");
    vga_println("");

    dynlink_info_t info = dynlink_get_info();

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Zagruzheno bibliotek: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(info.loaded_libs);
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Vsego simvolov:      ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(info.total_symbols);
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Razresheno:          ");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print_dec(info.resolved_count);
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Nerazresheno:        ");
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_print_dec(info.unresolved_count);
    vga_println("");

    // Список загруженных библиотек
    if (info.loaded_libs > 0) {
        vga_println("");
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_println("  --- Zagruzhennye biblioteki ---");

        for (int i = 0; i < DYNLINK_MAX_LIBS; i++) {
            if (!loaded_libs[i].loaded) continue;

            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts("  ");
            vga_puts(loaded_libs[i].name);

            int nlen = dl_strlen(loaded_libs[i].name);
            for (int j = nlen; j < 20; j++) vga_puts(" ");

            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            vga_puts("base=0x");
            vga_print_hex(loaded_libs[i].base_addr);
            vga_puts(" size=");
            vga_print_dec(loaded_libs[i].size);
            vga_puts(" syms=");
            vga_print_dec(loaded_libs[i].symbol_count);
            vga_puts(" refs=");
            vga_print_dec(loaded_libs[i].ref_count);
            vga_println("");
        }
    }
}

// ============================================================================
// Вывод символов библиотеки
// ============================================================================

void dynlink_print_symbols(const char* lib_name) {
    if (!lib_name) {
        // Выводим все глобальные символы
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        vga_println("=== Global'naya tablitsa simvolov ===");

        for (uint32_t i = 0; i < global_symbol_count; i++) {
            if (!global_symbols[i].in_use) continue;

            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts("  0x");
            vga_print_hex(global_symbols[i].address);
            vga_puts(" ");

            // Тип символа
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            switch (global_symbols[i].type) {
                case STT_FUNC:   vga_puts("FUNC "); break;
                case STT_OBJECT: vga_puts("OBJ  "); break;
                default:         vga_puts("UNK  "); break;
            }

            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_println(global_symbols[i].name);
        }
        return;
    }

    // Ищем конкретную библиотеку
    const dynlink_lib_t* lib = dynlink_find_library(lib_name);
    if (!lib) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("Biblioteka ne najdena: ");
        vga_println(lib_name);
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("=== Simvoly: ");
    vga_puts(lib_name);
    vga_println(" ===");

    for (uint32_t i = 0; i < lib->symbol_count; i++) {
        if (!lib->symbols[i].in_use) continue;

        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("  0x");
        vga_print_hex(lib->symbols[i].address);
        vga_puts(" ");
        vga_println(lib->symbols[i].name);
    }
}

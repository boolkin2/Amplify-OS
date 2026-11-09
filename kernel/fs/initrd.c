// ============================================================================
// initrd.c - Реализация Initial RAM Disk для MyOS
// ============================================================================
// Парсит образ InitRD, загруженный GRUB как Multiboot модуль,
// и создаёт VFS-узлы для каждого файла.
//
// Принцип работы:
//   1. GRUB загружает initrd.img в память как модуль
//   2. Адрес модуля передаётся через Multiboot info structure
//   3. initrd_init() парсит заголовок и создаёт VFS-дерево
//   4. Файлы доступны через стандартный VFS API (read, readdir, finddir)
// ============================================================================

#include "initrd.h"
#include "vfs.h"
#include "../vga.h"
#include "../memory/memory.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Указатель на заголовок InitRD в памяти
static initrd_header_t* initrd_header = NULL;

// Указатель на массив заголовков файлов
static initrd_file_header_t* file_headers = NULL;

// VFS-узлы для файлов InitRD
// Первый узел — корневая директория, остальные — файлы
static vfs_node_t initrd_root;
static vfs_node_t initrd_file_nodes[INITRD_MAX_FILES];

// Временная структура для возврата из readdir
static vfs_dirent_t initrd_dirent;

// Информация об InitRD
static initrd_info_t initrd_info;

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Копирование строки (локальная версия)
static void initrd_strcpy(char* dest, const char* src, size_t max_len) {
    size_t i;
    for (i = 0; i < max_len - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

// Сравнение строк (локальная версия)
static int initrd_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

// ============================================================================
// Функции VFS для InitRD (callbacks)
// ============================================================================

// Чтение данных из файла InitRD
// Параметры:
//   node   — VFS-узел файла
//   offset — смещение от начала файла
//   size   — количество байт для чтения
//   buffer — буфер для записи данных
// Возвращает количество прочитанных байт
static uint32_t initrd_read(vfs_node_t* node, uint32_t offset,
                            uint32_t size, uint8_t* buffer) {
    // Проверяем параметры
    if (!node || !buffer) {
        return 0;
    }

    // Получаем заголовок файла по inode
    uint32_t file_index = node->inode;
    if (file_index >= initrd_header->num_files) {
        return 0;
    }

    initrd_file_header_t* fh = &file_headers[file_index];

    // Проверяем выход за пределы файла
    if (offset >= fh->size) {
        return 0;  // Смещение за пределами файла
    }

    // Ограничиваем размер чтения
    if (offset + size > fh->size) {
        size = fh->size - offset;
    }

    // Вычисляем адрес данных в памяти
    // fh->offset — смещение от начала InitRD образа
    uint8_t* file_data = (uint8_t*)((uint32_t)initrd_header + fh->offset);

    // Копируем данные в буфер
    memcpy(buffer, file_data + offset, size);

    return size;
}

// Чтение записи директории InitRD (по индексу)
// Параметры:
//   node  — VFS-узел директории (корень InitRD)
//   index — индекс записи (0..num_files-1)
// Возвращает указатель на структуру dirent или NULL
static vfs_dirent_t* initrd_readdir(vfs_node_t* node, uint32_t index) {
    (void)node;  // Корневая директория — единственная

    // Проверяем индекс
    if (index >= initrd_header->num_files) {
        return NULL;
    }

    // Заполняем структуру dirent
    initrd_strcpy(initrd_dirent.name, file_headers[index].name, VFS_MAX_FILENAME);
    initrd_dirent.inode = index;

    return &initrd_dirent;
}

// Поиск файла в директории InitRD по имени
// Параметры:
//   node — VFS-узел директории
//   name — имя искомого файла
// Возвращает указатель на VFS-узел файла или NULL
static vfs_node_t* initrd_finddir(vfs_node_t* node, const char* name) {
    (void)node;  // Корневая директория — единственная

    if (!name) {
        return NULL;
    }

    // Перебираем все файлы
    for (uint32_t i = 0; i < initrd_header->num_files; i++) {
        if (initrd_strcmp(file_headers[i].name, name) == 0) {
            return &initrd_file_nodes[i];
        }
    }

    return NULL;  // Файл не найден
}

// ============================================================================
// Основные функции InitRD
// ============================================================================

// Инициализация InitRD
vfs_node_t* initrd_init(uint32_t location) {
    // Сохраняем указатель на заголовок
    initrd_header = (initrd_header_t*)location;

    // Проверяем магическое число
    if (initrd_header->magic != INITRD_MAGIC) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("  [OSHIBKA] InitRD: nevernoe magicheskoe chislo! ");
        vga_print_hex(initrd_header->magic);
        vga_println("");
        return NULL;
    }

    // Проверяем количество файлов
    if (initrd_header->num_files > INITRD_MAX_FILES) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("  [OSHIBKA] InitRD: slishkom mnogo fajlov!");
        return NULL;
    }

    // Указатель на массив заголовков файлов (сразу после главного заголовка)
    file_headers = (initrd_file_header_t*)((uint32_t)initrd_header +
                                            sizeof(initrd_header_t));

    // =====================================================================
    // Создаём корневую директорию (VFS-узел "/")
    // =====================================================================
    initrd_strcpy(initrd_root.name, "/", VFS_MAX_FILENAME);
    initrd_root.mask = 0;
    initrd_root.uid = 0;
    initrd_root.gid = 0;
    initrd_root.flags = VFS_DIRECTORY;
    initrd_root.inode = 0;
    initrd_root.length = initrd_header->num_files;
    initrd_root.impl = 0;
    initrd_root.read = NULL;
    initrd_root.write = NULL;
    initrd_root.open = NULL;
    initrd_root.close = NULL;
    initrd_root.readdir = initrd_readdir;
    initrd_root.finddir = initrd_finddir;
    initrd_root.ptr = NULL;
    // Корень ссылается сам на себя как parent — семантически
    // эквивалентно Unix: у "/" родитель — "/".
    initrd_root.parent = &initrd_root;

    // =====================================================================
    // Создаём VFS-узлы для каждого файла
    // =====================================================================
    for (uint32_t i = 0; i < initrd_header->num_files; i++) {
        initrd_strcpy(initrd_file_nodes[i].name, file_headers[i].name,
                      VFS_MAX_FILENAME);
        initrd_file_nodes[i].mask = 0;
        initrd_file_nodes[i].uid = 0;
        initrd_file_nodes[i].gid = 0;
        initrd_file_nodes[i].flags = VFS_FILE;
        initrd_file_nodes[i].inode = i;  // Индекс файла = inode
        initrd_file_nodes[i].length = file_headers[i].size;
        initrd_file_nodes[i].impl = 0;
        initrd_file_nodes[i].read = initrd_read;
        initrd_file_nodes[i].write = NULL;  // InitRD только для чтения
        initrd_file_nodes[i].open = NULL;
        initrd_file_nodes[i].close = NULL;
        initrd_file_nodes[i].readdir = NULL;
        initrd_file_nodes[i].finddir = NULL;
        initrd_file_nodes[i].ptr = NULL;
        // Все файлы initrd — непосредственные дети корня.
        initrd_file_nodes[i].parent = &initrd_root;
    }

    // =====================================================================
    // Заполняем информацию об InitRD
    // =====================================================================
    initrd_info.loaded = true;
    initrd_info.start_addr = location;
    initrd_info.num_files = initrd_header->num_files;

    // Вычисляем общий размер образа
    if (initrd_header->num_files > 0) {
        // Последний файл определяет конец образа
        uint32_t last_idx = initrd_header->num_files - 1;
        initrd_info.total_size = file_headers[last_idx].offset +
                                 file_headers[last_idx].size;
        initrd_info.end_addr = location + initrd_info.total_size;
    } else {
        initrd_info.total_size = sizeof(initrd_header_t);
        initrd_info.end_addr = location + initrd_info.total_size;
    }

    return &initrd_root;
}

// Получение информации об InitRD
initrd_info_t initrd_get_info(void) {
    return initrd_info;
}

// Получение количества файлов
uint32_t initrd_get_file_count(void) {
    if (!initrd_header) {
        return 0;
    }
    return initrd_header->num_files;
}

// Получение имени файла по индексу
const char* initrd_get_filename(uint32_t index) {
    if (!initrd_header || index >= initrd_header->num_files) {
        return NULL;
    }
    return file_headers[index].name;
}

// Получение размера файла по индексу
uint32_t initrd_get_filesize(uint32_t index) {
    if (!initrd_header || index >= initrd_header->num_files) {
        return 0;
    }
    return file_headers[index].size;
}

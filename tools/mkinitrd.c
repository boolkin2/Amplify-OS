// ============================================================================
// mkinitrd.c - Утилита для создания образа InitRD
// ============================================================================
// Это HOST-утилита (компилируется обычным gcc на Linux, НЕ кросс-компилятором)
// Читает список файлов и создаёт бинарный образ InitRD
//
// Использование:
//   ./mkinitrd <output.img> <file1> <file2> ... <fileN>
//
// Формат выходного файла:
//   [initrd_header_t]               — магическое число + количество файлов
//   [initrd_file_header_t * N]      — массив заголовков файлов
//   [данные файла 1]                — содержимое файла 1
//   [данные файла 2]                — содержимое файла 2
//   ...
//   [данные файла N]                — содержимое файла N
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ============================================================================
// Константы (должны совпадать с kernel/fs/initrd.h)
// ============================================================================

#define INITRD_MAGIC        0x4D594F53  // "MYOS"
#define INITRD_MAX_FILENAME 64
#define INITRD_MAX_FILES    256

// ============================================================================
// Структуры (должны совпадать с kernel/fs/initrd.h)
// ============================================================================

// Главный заголовок InitRD
typedef struct {
    uint32_t magic;
    uint32_t num_files;
} __attribute__((packed)) initrd_header_t;

// Заголовок файла
typedef struct {
    char name[INITRD_MAX_FILENAME];
    uint32_t offset;
    uint32_t size;
} __attribute__((packed)) initrd_file_header_t;

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Извлечение имени файла из полного пути
// "/path/to/file.txt" -> "file.txt"
static const char* basename(const char* path) {
    const char* last_slash = path;
    const char* p = path;

    while (*p) {
        if (*p == '/' || *p == '\\') {
            last_slash = p + 1;
        }
        p++;
    }

    // Если слешей не было, возвращаем весь путь
    if (last_slash == path && *path != '/' && *path != '\\') {
        return path;
    }

    return last_slash;
}

// Получение размера файла
static long get_file_size(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);

    return size;
}

// ============================================================================
// Главная функция
// ============================================================================

int main(int argc, char* argv[]) {
    printf("=== mkinitrd - Sozdanie obraza InitRD dlya MyOS ===\n\n");

    // Проверяем аргументы
    if (argc < 3) {
        printf("Ispol'zovanie: %s <output.img> <file1> [file2] ... [fileN]\n", argv[0]);
        printf("\nPrimer:\n");
        printf("  %s initrd.img hello.txt readme.txt about.txt\n", argv[0]);
        return 1;
    }

    const char* output_file = argv[1];
    int num_files = argc - 2;

    // Проверяем ограничение на количество файлов
    if (num_files > INITRD_MAX_FILES) {
        fprintf(stderr, "Oshibka: slishkom mnogo fajlov (max %d)\n", INITRD_MAX_FILES);
        return 1;
    }

    printf("Vyhodnoj fajl: %s\n", output_file);
    printf("Kolichestvo fajlov: %d\n\n", num_files);

    // =====================================================================
    // Шаг 1: Собираем информацию о файлах
    // =====================================================================

    initrd_header_t header;
    header.magic = INITRD_MAGIC;
    header.num_files = num_files;

    initrd_file_header_t file_headers[INITRD_MAX_FILES];
    memset(file_headers, 0, sizeof(file_headers));

    // Вычисляем смещение данных первого файла
    // Данные начинаются после главного заголовка и массива заголовков файлов
    uint32_t data_offset = sizeof(initrd_header_t) +
                           sizeof(initrd_file_header_t) * num_files;

    for (int i = 0; i < num_files; i++) {
        const char* filepath = argv[i + 2];
        const char* name = basename(filepath);

        // Получаем размер файла
        long fsize = get_file_size(filepath);
        if (fsize < 0) {
            fprintf(stderr, "Oshibka: ne mogu otkryt' fajl '%s'\n", filepath);
            return 1;
        }

        // Заполняем заголовок файла
        strncpy(file_headers[i].name, name, INITRD_MAX_FILENAME - 1);
        file_headers[i].name[INITRD_MAX_FILENAME - 1] = '\0';
        file_headers[i].offset = data_offset;
        file_headers[i].size = (uint32_t)fsize;

        printf("  [%d] %-30s  %6u bajt  (smeshchenie: 0x%08X)\n",
               i, name, file_headers[i].size, file_headers[i].offset);

        // Следующий файл начинается после текущего
        data_offset += (uint32_t)fsize;
    }

    printf("\nObshchij razmer obraza: %u bajt (%.2f KB)\n\n",
           data_offset, data_offset / 1024.0);

    // =====================================================================
    // Шаг 2: Записываем образ InitRD
    // =====================================================================

    FILE* out = fopen(output_file, "wb");
    if (!out) {
        fprintf(stderr, "Oshibka: ne mogu sozdat' fajl '%s'\n", output_file);
        return 1;
    }

    // Записываем главный заголовок
    fwrite(&header, sizeof(initrd_header_t), 1, out);

    // Записываем массив заголовков файлов
    fwrite(file_headers, sizeof(initrd_file_header_t), num_files, out);

    // Записываем данные каждого файла
    for (int i = 0; i < num_files; i++) {
        const char* filepath = argv[i + 2];

        FILE* in = fopen(filepath, "rb");
        if (!in) {
            fprintf(stderr, "Oshibka: ne mogu otkryt' '%s' dlya chteniya\n", filepath);
            fclose(out);
            return 1;
        }

        // Читаем и записываем содержимое файла
        uint8_t buffer[4096];
        size_t bytes_read;

        while ((bytes_read = fread(buffer, 1, sizeof(buffer), in)) > 0) {
            fwrite(buffer, 1, bytes_read, out);
        }

        fclose(in);
    }

    fclose(out);

    printf("Obraz InitRD uspeshno sozdan: %s\n", output_file);
    printf("Magic: 0x%08X\n", header.magic);
    printf("Fajlov: %u\n", header.num_files);

    return 0;
}

// ============================================================================
// scribe.c — «Писец»: файлы каталога программы (подэтап 51.4, для людей).
// ----------------------------------------------------------------------------
// Показывает, что программа видит ОДИН каталог диска (в окне Rune — поле
// «Каталог программы») и что с ним можно делать при разном доступе:
//   scribe               список каталога: имена по алфавиту, размеры
//   scribe <файл>        сколько в файле строк, слов и байт
//   scribe note <текст>  дописать строку в notes.txt и показать, сколько их
// При доступе «Чтение» запись получает отказ, при «Без файлов» каталога нет
// вовсе — программа говорит об этом сама.
// Порядок записей у файловых систем разный — имена сортируются здесь: вывод
// обязан совпадать с тем же исходником под gcc.
// ============================================================================
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __wasm__
// Строка для библиотеки окна Rune — пользовательская секция модуля «rune.about».
// Атрибут section у clang для этого не годится (данные уходят в обычный
// сегмент), секцию создаёт только ассемблерная вставка.
__asm__(".section .custom_section.rune.about,\"\",@\n"
        ".ascii \"Писец: список каталога, счёт строк, заметки\"\n"
        ".text\n");
#endif

#define MAX_NAMES 256
static char names[MAX_NAMES][128];

static int by_name(const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); }

static int list(void) {
    DIR* d = opendir(".");
    if (!d) { puts("Каталог программе не дан (или его не открыть)."); return 1; }
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n == MAX_NAMES || strlen(e->d_name) >= sizeof(names[0])) continue;
        strcpy(names[n++], e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    printf("В каталоге записей: %d\n", n);
    for (int i = 0; i < n; i++) {
        struct stat st;
        if (stat(names[i], &st) != 0) printf("  %-28s ?\n", names[i]);
        else if (S_ISDIR(st.st_mode)) printf("  %-28s <каталог>\n", names[i]);
        else printf("  %-28s %ld байт\n", names[i], (long)st.st_size);
    }
    return 0;
}

static int count(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("%s: не открыть (%s)\n", path, errno == ENOENT ? "нет такого файла" : "отказ"); return 1; }
    long lines = 0, words = 0, bytes = 0;
    int c, in_word = 0;
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        if (c == '\n') lines++;
        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') in_word = 0;
        else if (!in_word) { in_word = 1; words++; }
    }
    fclose(f);
    printf("%s: строк %ld, слов %ld, байт %ld\n", path, lines, words, bytes);
    return 0;
}

static int note(int argc, char** argv) {
    FILE* f = fopen("notes.txt", "a");
    if (!f) { puts("notes.txt: записать нельзя (каталог только для чтения или его нет)"); return 1; }
    for (int i = 2; i < argc; i++) fprintf(f, "%s%s", i > 2 ? " " : "", argv[i]);
    fputc('\n', f);
    if (fclose(f) != 0) { puts("notes.txt: запись не удалась"); return 1; }
    puts("Записано.");
    return count("notes.txt");
}

int main(int argc, char** argv) {
    if (argc == 1) return list();
    if (!strcmp(argv[1], "note")) {
        if (argc < 3) { puts("usage: scribe note <text>"); return 2; }
        return note(argc, argv);
    }
    return count(argv[1]);
}

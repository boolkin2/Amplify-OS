// ============================================================================
// files.c — проверочная программа: файлы и каталоги через WASI (этап 51.2).
// ----------------------------------------------------------------------------
// Обычный C со stdio: создаёт, дописывает, перезаписывает с середины, пишет
// «за конец», обрезает, переименовывает, перечисляет каталог, удаляет — и про
// каждый шаг печатает строку. Эталон — ТОТ ЖЕ исходник под gcc на хосте,
// запущенный в пустом каталоге: вывод обязан совпасть побайтно. Поэтому в
// выводе нет ничего, что у файловых систем разное: времени, номеров файлов,
// размеров каталогов, порядка записей (имена сортируются здесь).
// Работает в подкаталоге work (создаёт сам), в конце оставляет после себя
// один файл work/kept.txt — его тест читает с диска снаружи.
// ============================================================================
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char* ename(int e) {
    switch (e) {
    case 0:         return "OK";
    case ENOENT:    return "ENOENT";
    case EEXIST:    return "EEXIST";
    case ENOTDIR:   return "ENOTDIR";
    case EISDIR:    return "EISDIR";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EBADF:     return "EBADF";
    case EINVAL:    return "EINVAL";
    default:        return "other";
    }
}

static uint32_t crc32(uint32_t c, const uint8_t* p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static long size_of(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return S_ISDIR(st.st_mode) ? -2 : (long)st.st_size;
}

// Весь файл: размер, свёртка, число нулевых байт.
static void show(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("show %s: %s\n", path, ename(errno)); return; }
    static uint8_t buf[3000];
    uint32_t crc = 0;
    long total = 0, zeros = 0;
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        crc = crc32(crc, buf, n);
        for (size_t i = 0; i < n; i++) zeros += buf[i] == 0;
        total += (long)n;
    }
    fclose(f);
    printf("show %s: %ld bytes, crc %08x, zeros %ld, stat says %ld\n", path, total, (unsigned)crc, zeros, size_of(path));
}

static void lines(const char* path) {
    FILE* f = fopen(path, "r");
    char line[128];
    int n = 0;
    if (!f) { printf("lines %s: %s\n", path, ename(errno)); return; }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        printf("  %s:%d: [%s]\n", path, ++n, line);
    }
    fclose(f);
}

static void put(const char* path, const char* text) {
    FILE* f = fopen(path, "w");
    if (!f) { printf("put %s: %s\n", path, ename(errno)); return; }
    fputs(text, f);
    printf("put %s: %s\n", path, fclose(f) == 0 ? "OK" : "close failed");
}

static int by_name(const void* a, const void* b) { return strcmp(*(const char* const*)a, *(const char* const*)b); }

static void list(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) { printf("list %s: %s\n", dir, ename(errno)); return; }
    char* names[64];
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != 0 && n < 64) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof(names[0]), by_name);
    printf("list %s: %d entries\n", dir, n);
    for (int i = 0; i < n; i++) {
        char path[300];
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        long s = size_of(path);
        if (s == -2) printf("  %s/\n", names[i]);
        else printf("  %s (%ld)\n", names[i], s);
        free(names[i]);
    }
}

#define TRY(what, call) do { errno = 0; int r_ = (call); printf("%s: %s\n", what, r_ == 0 ? "OK" : ename(errno)); } while (0)
#define TRY_OPEN(path, mode) do { errno = 0; FILE* f_ = fopen(path, mode); printf("fopen(%s, %s): %s\n", path, mode, f_ ? "OK" : ename(errno)); if (f_) fclose(f_); } while (0)

int main(void) {
    FILE* f;
    // Каталог мог остаться от прошлого запуска — это не ошибка.
    if (mkdir("work", 0777) != 0 && errno != EEXIST) { printf("cannot create work: %s\n", ename(errno)); return 1; }

    puts("== text file ==");
    f = fopen("work/note.txt", "w");
    if (!f) { printf("cannot create work/note.txt: %s\n", ename(errno)); return 1; }
    fprintf(f, "first line\nsecond %d\n", 42);
    fclose(f);
    show("work/note.txt");
    lines("work/note.txt");

    puts("== append ==");
    f = fopen("work/note.txt", "a");
    fputs("third\n", f);
    printf("position after append: %ld\n", ftell(f));
    fclose(f);
    lines("work/note.txt");

    puts("== overwrite in the middle, write past the end ==");
    f = fopen("work/note.txt", "r+");
    fseek(f, 6, SEEK_SET);
    fputs("LINE", f);
    fseek(f, 0, SEEK_END);
    long end = ftell(f);
    fseek(f, end + 10, SEEK_SET);           // дыра в десять байт: должна читаться нулями
    fputc('!', f);
    printf("end was %ld, now %ld\n", end, ftell(f));
    fclose(f);
    show("work/note.txt");
    lines("work/note.txt");

    puts("== 200 KB in pieces ==");
    static uint8_t chunk[7001];
    uint32_t seed = 2026, want = 0;
    f = fopen("work/big.bin", "wb");
    for (int part = 0; part < 30; part++) {
        size_t n = 5000 + (size_t)(part * 67) % 2000;
        for (size_t i = 0; i < n; i++) { seed = seed * 1664525u + 1013904223u; chunk[i] = (uint8_t)(seed >> 24); }
        want = crc32(want, chunk, n);
        fwrite(chunk, 1, n, f);
    }
    printf("written %ld bytes, crc %08x\n", ftell(f), (unsigned)want);
    fclose(f);
    show("work/big.bin");
    f = fopen("work/big.bin", "rb");
    fseek(f, 123456, SEEK_SET);
    size_t got = fread(chunk, 1, 100, f);
    printf("100 bytes at 123456: read %zu, crc %08x\n", got, (unsigned)crc32(0, chunk, got));
    fseek(f, -50, SEEK_END);
    got = fread(chunk, 1, 100, f);          // до конца всего 50
    printf("100 bytes at end-50: read %zu, eof %d\n", got, feof(f) != 0);
    fclose(f);

    puts("== directories ==");
    TRY("mkdir work/box", mkdir("work/box", 0777));
    TRY("mkdir work/box again", mkdir("work/box", 0777));
    put("work/box/a.txt", "alpha\n");
    put("work/box/b.txt", "beta beta\n");
    TRY("mkdir work/box/sub", mkdir("work/box/sub", 0777));
    put("work/box/sub/deep.txt", "deep\n");
    list("work/box");
    list("work/box/sub");

    puts("== rename ==");
    TRY("rename a.txt -> c.txt", rename("work/box/a.txt", "work/box/c.txt"));
    printf("a.txt now: %ld, c.txt now: %ld\n", size_of("work/box/a.txt"), size_of("work/box/c.txt"));
    TRY("rename b.txt over c.txt", rename("work/box/b.txt", "work/box/c.txt"));
    lines("work/box/c.txt");
    list("work/box");

    puts("== errors ==");
    TRY_OPEN("work/missing.txt", "r");
    TRY_OPEN("work/note.txt/x", "r");
    TRY_OPEN("work/box", "w");
    TRY_OPEN("work/nodir/x.txt", "w");
    TRY("rmdir work/box (not empty)", rmdir("work/box"));
    TRY("unlink work/nothing", unlink("work/nothing"));
    TRY("mkdir work/nodir/x", mkdir("work/nodir/x", 0777));
    TRY("rmdir work/note.txt (a file)", rmdir("work/note.txt"));

    puts("== truncate ==");
    f = fopen("work/note.txt", "r+");
    TRY("ftruncate to 5", ftruncate(fileno(f), 5));
    fclose(f);
    show("work/note.txt");
    lines("work/note.txt");
    f = fopen("work/note.txt", "w");
    fclose(f);
    printf("after fopen(w): %ld bytes\n", size_of("work/note.txt"));

    puts("== twenty small files ==");
    for (int i = 0; i < 20; i++) {
        char path[64];
        snprintf(path, sizeof(path), "work/item-%02d.txt", i);
        f = fopen(path, "w");
        if (!f) { printf("cannot create %s: %s\n", path, ename(errno)); continue; }
        fprintf(f, "item %d\n", i * i);
        fclose(f);
    }
    list("work");
    int removed = 0;
    for (int i = 0; i < 20; i++) {
        char path[64];
        snprintf(path, sizeof(path), "work/item-%02d.txt", i);
        removed += unlink(path) == 0;
    }
    printf("removed %d of 20\n", removed);

    puts("== cleanup ==");
    TRY("unlink c.txt", unlink("work/box/c.txt"));
    TRY("unlink deep.txt", unlink("work/box/sub/deep.txt"));
    TRY("rmdir sub", rmdir("work/box/sub"));
    TRY("rmdir box", rmdir("work/box"));
    TRY("unlink note.txt", unlink("work/note.txt"));
    TRY("unlink big.bin", unlink("work/big.bin"));
    put("work/kept.txt", "kept by files.c\n");
    list("work");
    puts("files: done");
    return 0;
}

// ============================================================================
// jail.c — проверочная программа: граница каталога (этап 51.2).
// ----------------------------------------------------------------------------
// Программе открыт один каталог. Она ПЫТАЕТСЯ выйти из него всеми способами,
// какие даёт путь: «..», «..» после захода вглубь, абсолютный путь, каталог
// выше как каталог, переименование и создание снаружи. Каждая попытка обязана
// кончиться отказом. Законные пути с «..» внутри каталога — работать.
//   jail        — попытки выхода (каталог открыт на запись)
//   jail ro     — каталог открыт только для чтения: читать можно, менять нельзя
// Эталона на хосте у этой программы нет: без границы всё «получится».
// Тест ещё и смотрит снаружи, что вне каталога ничего не появилось.
// ============================================================================
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_total, g_blocked;

// Попытка, которая ДОЛЖНА быть отбита: ok — «у программы получилось».
static void must_fail(const char* what, int ok) {
    g_total++;
    if (!ok) g_blocked++;
    printf("%s: %s\n", what, ok ? "ESCAPED" : "blocked");
}
static void must_work(const char* what, int ok) {
    printf("%s: %s\n", what, ok ? "works" : "BROKEN");
}

static int can_open(const char* path, const char* mode) {
    FILE* f = fopen(path, mode);
    if (f) fclose(f);
    return f != 0;
}
static int can_list(const char* path) {
    DIR* d = opendir(path);
    if (d) closedir(d);
    return d != 0;
}

int main(int argc, char** argv) {
    struct stat st;
    if (argc > 1 && !strcmp(argv[1], "ro")) {
        // Каталог только для чтения. Файл seed.txt в нём кладёт тест.
        must_work("read seed.txt", can_open("seed.txt", "r"));
        must_work("list /", can_list("."));
        must_fail("create new.txt", can_open("new.txt", "w"));
        must_fail("append to seed.txt", can_open("seed.txt", "a"));
        must_fail("truncate seed.txt", can_open("seed.txt", "w"));
        must_fail("mkdir d", mkdir("d", 0777) == 0);
        must_fail("unlink seed.txt", unlink("seed.txt") == 0);
        must_fail("rename seed.txt", rename("seed.txt", "moved.txt") == 0);
        must_work("seed.txt is still there", stat("seed.txt", &st) == 0);
        printf("jail ro: %d of %d blocked\n", g_blocked, g_total);
        return g_blocked == g_total ? 0 : 1;
    }

    mkdir("inner", 0777);
    FILE* f = fopen("inner/mine.txt", "w");
    if (f) { fputs("mine\n", f); fclose(f); }

    must_work("inner/mine.txt", can_open("inner/mine.txt", "r"));
    must_work("inner/../inner/mine.txt", can_open("inner/../inner/mine.txt", "r"));
    must_work("./inner/./mine.txt", can_open("./inner/./mine.txt", "r"));
    must_work("/inner/mine.txt", can_open("/inner/mine.txt", "r"));

    must_fail("create ../escape.txt", can_open("../escape.txt", "w"));
    must_fail("create inner/../../escape.txt", can_open("inner/../../escape.txt", "w"));
    must_fail("create /../escape.txt", can_open("/../escape.txt", "w"));
    must_fail("create inner/../../../../escape.txt", can_open("inner/../../../../escape.txt", "w"));
    must_fail("read ../outside.txt", can_open("../outside.txt", "r"));
    must_fail("list ..", can_list(".."));
    must_fail("list inner/../..", can_list("inner/../.."));
    must_fail("stat ..", stat("..", &st) == 0);
    must_fail("mkdir ../dir", mkdir("../dir", 0777) == 0);
    must_fail("rename inner/mine.txt -> ../stolen.txt", rename("inner/mine.txt", "../stolen.txt") == 0);
    must_fail("rename ../outside.txt -> taken.txt", rename("../outside.txt", "taken.txt") == 0);
    must_fail("unlink ../outside.txt", unlink("../outside.txt") == 0);
    must_fail("rmdir ..", rmdir("..") == 0);
    must_fail("symlink to ..", symlink("..", "up") == 0);

    must_work("inner/mine.txt after all that", can_open("inner/mine.txt", "r"));
    unlink("inner/mine.txt");
    rmdir("inner");
    printf("jail: %d of %d blocked\n", g_blocked, g_total);
    return g_blocked == g_total ? 0 : 1;
}

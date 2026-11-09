// ============================================================================
// cwdtest/main.c — проверка chdir/getcwd + нормализации ".." (Стадия E)
// ============================================================================
// Сценарий:
//   1. Печатаем начальный cwd (должен быть "/").
//   2. chdir("/disk") — переход к смонтированной FAT32.
//   3. Печатаем cwd — ожидаем "/disk".
//   4. chdir("..") — должно вернуть в "/".
//   5. Пробуем chdir на несуществующий путь — ожидаем -1.
// ============================================================================

#include "../libmyos/myos.h"

static void show_cwd(const char* tag) {
    // Первый байт инициализирован явно, чтобы при ошибке получить "" а не
    // мусор со стека и чтобы -Wmaybe-uninitialized не ругался на buf.
    char buf[128];
    buf[0] = '\0';
    uint32_t n = myos_getcwd(buf, sizeof(buf));
    myos_print("[CWD] ");
    myos_print(tag);
    myos_print(": ");
    if (n == (uint32_t)-1) {
        myos_print("(getcwd error)");
    } else {
        myos_print(buf);
    }
    myos_print("\n");
}

int main(void) {
    myos_print("[CWD] cwdtest start\n");
    show_cwd("initial");

    if (myos_chdir("/disk") == 0) {
        show_cwd("after chdir(/disk)");
    } else {
        myos_print("[CWD] chdir(/disk) failed (mount missing?)\n");
    }

    if (myos_chdir("..") == 0) {
        show_cwd("after chdir(..)");
    } else {
        myos_print("[CWD] chdir(..) failed\n");
    }

    uint32_t rc = myos_chdir("/no/such/path");
    myos_print("[CWD] chdir(/no/such/path) = ");
    myos_print_int((int)rc);
    myos_print(" (expected -1)\n");

    return 0;
}

// ============================================================================
// exectest/main.c — проверка SYS_EXECVE в MyOS (Стадия D)
// ============================================================================
// Сценарий:
//   1. Программа печатает свой PID и идентификационную строку "exectest".
//   2. Вызывает execve("/forktest.elf") — этот ELF уже есть в initrd.
//   3. Если execve сработал, управление НЕ вернётся сюда: начнёт работу
//      forktest, который при старте сам выведет свой [FORK] Parent ...
//      message, с тем же PID (это — ключевое доказательство execve).
//   4. Если execve вернулся — это ошибка, печатаем диагностику.
// ============================================================================

#include "../libmyos/myos.h"

int main(void) {
    myos_print("[EXEC] exectest.elf starting, pid=");
    myos_print_int((int)myos_getpid());
    myos_print("\n[EXEC] calling execve(/forktest.elf)...\n");

    // argv/envp пока не нужны.
    uint32_t rc = myos_execve("/forktest.elf", 0, 0);

    // Сюда попадаем ТОЛЬКО при ошибке.
    myos_print("[EXEC] execve FAILED, rc=");
    myos_print_int((int)rc);
    myos_print("\n");
    return 1;
}

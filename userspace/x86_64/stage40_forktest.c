// ============================================================================
// forktest.c — тест POSIX fork() (этап 40 группа F).
// ----------------------------------------------------------------------------
// На этапе 40 fork() в libc возвращает -ENOSYS (kernel-side fork отсутствует
// на 40-м; переносится на stage 45). Программа всё равно собирается через
// sysroot и при запуске сообщает об ошибке корректно.
// Маркер: "STAGE40_FORKTEST_OK_DEFERRED" (fork ожидаемо не работает).
// ============================================================================
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    printf("[forktest] pid=%d\n", getpid());
    int var = 42;
    printf("[forktest] before fork: var=%d\n", var);
    pid_t child = fork();
    if (child < 0 && errno == ENOSYS) {
        printf("[forktest] fork -> ENOSYS (kernel fork deferred to stage 45)\n");
        printf("STAGE40_FORKTEST_OK_DEFERRED\n");
        return 0;
    }
    if (child == 0) {
        printf("[forktest] child var=%d\n", var);
        _exit(0);
    }
    printf("[forktest] parent sees child=%d\n", (int)child);
    return 0;
}

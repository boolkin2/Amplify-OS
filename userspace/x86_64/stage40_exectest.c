// ============================================================================
// exectest.c — тест POSIX execve() (этап 40 группа F).
// ----------------------------------------------------------------------------
// execve на 40-м — stub -ENOSYS (kernel поддержка откладывается на stage 45).
// Маркер: "STAGE40_EXECTEST_OK_DEFERRED".
// ============================================================================
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    printf("[exectest] pid=%d\n", getpid());
    char* argv[] = { "hello.elf", 0 };
    int r = execv("/hello.elf", argv);
    if (r < 0 && errno == ENOSYS) {
        printf("[exectest] execv -> ENOSYS (deferred to stage 45)\n");
        printf("STAGE40_EXECTEST_OK_DEFERRED\n");
        return 0;
    }
    printf("[exectest] execv returned %d\n", r);
    return 1;
}

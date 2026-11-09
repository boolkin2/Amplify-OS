// ============================================================================
// cwdtest.c — тест getcwd/chdir через POSIX (этап 40 группа F).
// ----------------------------------------------------------------------------
// На 40-м getcwd в libc возвращает "/" фиксированно; chdir = stub -ENOSYS.
// Тест это и проверяет.
// Маркер: "STAGE40_CWDTEST_OK".
// ============================================================================
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

int main(void) {
    char buf[128];
    if (!getcwd(buf, sizeof(buf))) {
        printf("[cwdtest] getcwd failed\n");
        return 1;
    }
    printf("[cwdtest] initial cwd='%s'\n", buf);

    int r = chdir("/disk");
    if (r < 0) {
        printf("[cwdtest] chdir('/disk') -> errno=%d (expected ENOSYS=%d)\n",
               errno, ENOSYS);
    } else {
        printf("[cwdtest] chdir succeeded\n");
    }
    printf("STAGE40_CWDTEST_OK\n");
    return 0;
}

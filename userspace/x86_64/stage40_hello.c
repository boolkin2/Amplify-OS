// ============================================================================
// hello.c — простая POSIX-программа AMPLIFY-OS (этап 40 группа F).
// ----------------------------------------------------------------------------
// Заменяет устаревший userspace/hello/main.c (который использовал
// myos_* API + SYS_SWAP_TEST). Мигрирована на наш sysroot.
// Маркер для автотеста: "STAGE40_HELLO_OK".
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("[hello.c] AMPLIFY-OS hello, pid=%d\n", getpid());
    char* buf = (char*)malloc(128);
    if (!buf) { printf("[hello.c] malloc failed\n"); return 1; }
    const char* msg = "Привет, AMPLIFY-OS";
    int i = 0;
    while (msg[i]) { buf[i] = msg[i]; i++; }
    buf[i] = 0;
    printf("[hello.c] %s (len=%d)\n", buf, i);
    free(buf);
    printf("STAGE40_HELLO_OK\n");
    return 0;
}

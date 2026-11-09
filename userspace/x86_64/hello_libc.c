// ============================================================================
// hello_libc.c — первая C-программа AMPLIFY-OS, собранная через musl-aos libc
// и host-GCC c флагом --sysroot=build/sysroot (этап 39).
// ----------------------------------------------------------------------------
// Демонстрирует, что наш sysroot реально работает: printf, malloc, free,
// strlen, sprintf, time — всё через настоящие POSIX API, без AOS-специфики.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;

    // 1) Простейший вывод через printf — проверяем форматный движок.
    printf("AMPLIFY-OS libc test  (stage 39, musl-aos)\n");
    printf("  printf(%%d)  = %d\n", 42);
    printf("  printf(%%x)  = 0x%X\n", 0xCAFEBABE);
    printf("  printf(%%lld)= %lld\n", (long long)0x123456789ABCDEFLL);
    printf("  printf(%%s)  = '%s'\n", "AMPLIFY-OS");
    printf("  printf(%%5d) = [%5d]\n", 7);
    printf("  printf(%%-5d)= [%-5d]\n", 7);
    printf("  printf(%%05d)= [%05d]\n", 7);
    printf("  printf(%%p)  = %p\n", main);

    // 2) Тестируем malloc/free — несколько аллокаций разных размеров.
    void* a = malloc(16);
    void* b = malloc(4096);
    void* c = malloc(123);
    printf("\n  malloc test: 16=%p  4096=%p  123=%p\n", a, b, c);
    free(b);
    void* d = malloc(2048);
    printf("  after free(4096) + malloc(2048) → %p (best-fit reuse expected)\n", d);
    free(a); free(c); free(d);

    // 3) String API.
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "pid=%d uid=%d", getpid(), getuid());
    printf("\n  snprintf → '%s'  (length=%d)\n", buf, n);
    printf("  strlen('%s') = %zu\n", buf, strlen(buf));

    // 4) Tied — pthread mutex (на 39-м stub, но должен работать без ошибки).
    char msg[32];
    strcpy(msg, "Hello, world!");
    strcat(msg, "  [ok]");
    printf("\n  strcpy/strcat → '%s'\n", msg);

    // 5) time + monotonic.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    printf("\n  CLOCK_MONOTONIC: %lds %ldns since boot\n",
           (long)ts.tv_sec, (long)ts.tv_nsec);

    // 6) Возврат — exit code пойдёт через __libc_start_main → _exit.
    printf("\n  exit(0) ...\n");
    return 0;
}

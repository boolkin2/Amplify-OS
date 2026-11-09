// ============================================================================
// shmtest.c — тест POSIX shm_open/mmap (этап 40 группа F).
// ----------------------------------------------------------------------------
// Наш libc не имеет shm_open — это POSIX API, ядро имеет только SysV shm
// (shmget/shmat из kernel/arch/x86_64/shm.c). На 40-м миграция отложена;
// вместо неё — проверяем, что mmap(MAP_ANON) работает как эрзац.
// Маркер: "STAGE40_SHMTEST_OK_DEFERRED".
// ============================================================================
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>

int main(void) {
    printf("[shmtest] mmap(ANON) test\n");
    void* p = mmap(0, 4096, PROT_READ | PROT_WRITE,
                   MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p == MAP_FAILED) {
        printf("[shmtest] mmap failed\n");
        return 1;
    }
    const char* s = "Hello from anon-mmap";
    int i = 0;
    while (s[i]) { ((char*)p)[i] = s[i]; i++; }
    ((char*)p)[i] = 0;
    printf("[shmtest] wrote+read: '%s'\n", (char*)p);
    printf("[shmtest] (POSIX shm_open deferred to stage 45)\n");
    printf("STAGE40_SHMTEST_OK_DEFERRED\n");
    return 0;
}

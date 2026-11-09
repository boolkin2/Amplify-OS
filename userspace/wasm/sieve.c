// ============================================================================
// sieve.c — «Решето»: простые числа до N (подэтап 51.4, программа для людей).
// ----------------------------------------------------------------------------
// Считает долго и требует памяти — на ней видно, что программа работает на
// своём ядре (окно при этом живое), как идёт время счёта и как срабатывает
// предел памяти: решету нужен байт на число, `sieve 30000000` при пределе 8 МБ
// получит отказ и скажет об этом сама.
//   sieve [N]     N по умолчанию 2 000 000, не больше 60 000 000.
// Строка «время: …» — единственная, что зависит от машины; сверка с gcc её
// пропускает.
// ============================================================================
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __wasm__
// Строка для библиотеки окна Rune — пользовательская секция модуля «rune.about».
// Атрибут section у clang для этого не годится (данные уходят в обычный
// сегмент), секцию создаёт только ассемблерная вставка.
__asm__(".section .custom_section.rune.about,\"\",@\n"
        ".ascii \"Решето: простые числа до N, с замером времени\"\n"
        ".text\n");
#endif

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

int main(int argc, char** argv) {
    uint32_t n = 2000000;
    if (argc > 1) n = (uint32_t)strtoul(argv[1], 0, 10);
    if (n < 10 || n > 60000000u) {
        puts("usage: sieve [N]   10 <= N <= 60000000");
        return 2;
    }
    printf("Решето Эратосфена до %u\n", (unsigned)n);
    fflush(stdout);
    uint64_t t0 = now_ms();
    uint8_t* comp = malloc((size_t)n + 1);
    if (!comp) {
        printf("Не хватило памяти: нужно %u КБ.\n", (unsigned)(n / 1024 + 1));
        return 1;
    }
    memset(comp, 0, (size_t)n + 1);
    for (uint32_t i = 2; (uint64_t)i * i <= n; i++)
        if (!comp[i])
            for (uint32_t j = i * i; j <= n; j += i) comp[j] = 1;

    uint32_t count = 0, last[5] = { 0, 0, 0, 0, 0 };
    uint64_t sum = 0;
    for (uint32_t i = 2; i <= n; i++) {
        if (comp[i]) continue;
        count++;
        sum += i;
        last[0] = last[1]; last[1] = last[2]; last[2] = last[3]; last[3] = last[4]; last[4] = i;
    }
    uint64_t ms = now_ms() - t0;
    free(comp);

    printf("простых чисел: %u\n", (unsigned)count);
    printf("их сумма:      %llu\n", (unsigned long long)sum);
    printf("последние:     %u %u %u %u %u\n", (unsigned)last[0], (unsigned)last[1], (unsigned)last[2],
           (unsigned)last[3], (unsigned)last[4]);
    printf("время: %llu мс\n", (unsigned long long)ms);
    return 0;
}

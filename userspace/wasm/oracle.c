// ============================================================================
// oracle.c — «Оракул»: игра «угадай число» (подэтап 51.4, программа для людей).
// ----------------------------------------------------------------------------
// Показывает диалог с программой в окне Rune: приглашение без перевода строки,
// строка с клавиатуры, ответ. Число от 1 до 100 выбирается по часам; с
// аргументом (`oracle 42`) — по нему: так игра повторяема, и её вывод можно
// сверить с тем же исходником под gcc.
//   Конец ввода (Ctrl+D, кнопка «Конец ввода») — оракул называет число сам.
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
        ".ascii \"Оракул: угадайте число от 1 до 100\"\n"
        ".text\n");
#endif

int main(int argc, char** argv) {
    uint32_t seed;
    if (argc > 1) seed = (uint32_t)strtoul(argv[1], 0, 10);
    else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        seed = (uint32_t)ts.tv_sec * 1000003u + (uint32_t)ts.tv_nsec;
    }
    // Один шаг линейного генератора и старшие биты: младшие у него слабые.
    seed = seed * 1664525u + 1013904223u;
    int secret = (int)((seed >> 8) % 100u) + 1;

    puts("Оракул загадал число от 1 до 100.");
    puts("Назовите число - он ответит, больше оно или меньше.");
    char line[64];
    int tries = 0;
    for (;;) {
        fputs("> ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\nОракул устал ждать. Это было число %d.\n", secret);
            return 1;
        }
        char* end;
        long v = strtol(line, &end, 10);
        if (end == line || v < 1 || v > 100) {
            puts("Нужно число от 1 до 100.");
            continue;
        }
        tries++;
        if (v < secret) puts("Больше.");
        else if (v > secret) puts("Меньше.");
        else {
            printf("Верно! Это %d. Попыток: %d.\n", secret, tries);
            return 0;
        }
    }
}

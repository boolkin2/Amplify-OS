// ============================================================================
// env.c — проверочная программа: то, что WASI даёт помимо файлов (этап 51.2).
// ----------------------------------------------------------------------------
// Первый аргумент — что проверить:
//   args ...      напечатать свои аргументы (эталон — сборка gcc с теми же)
//   getenv ИМЯ    значение переменной окружения
//   clock         монотонные часы идут вперёд, настенные — после 2020 года,
//                 сон на 60 мс длится не меньше 50 мс
//   random        два запроса случайных байт дают разное и не нули
//   exit N        завершиться с кодом N (без вывода после)
//   stdin         сколько байт пришло на ввод до его конца
//   abort         аварийное завершение (для движка — ловушка unreachable)
//   spin          вечный цикл (останавливать должен движок)
//   oom           просить память, пока дают; программа обязана получить отказ,
//                 а не уронить систему
//   sleep N       спать N миллисекунд (51.3: спящая программа не занимает ядро)
//   echo          диалог: возвращает каждую введённую строку (51.3: ввод с клавиатуры)
//   utf8          русский текст; вторая строка выводится двумя кусками, и
//                 разрез приходится на середину буквы (2 байта UTF-8)
// ============================================================================
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile unsigned g_spin;

int main(int argc, char** argv) {
    const char* what = argc > 1 ? argv[1] : "";
    if (!strcmp(what, "args")) {
        printf("argc=%d\n", argc - 1);
        for (int i = 1; i < argc; i++) printf("arg[%d]=[%s]\n", i, argv[i]);
        return 0;
    }
    if (!strcmp(what, "getenv") && argc > 2) {
        const char* v = getenv(argv[2]);
        printf("%s=%s\n", argv[2], v ? v : "(unset)");
        return 0;
    }
    if (!strcmp(what, "clock")) {
        struct timespec a, b, real;
        clock_gettime(CLOCK_MONOTONIC, &a);
        struct timespec nap = { 0, 60 * 1000000L };
        nanosleep(&nap, 0);
        clock_gettime(CLOCK_MONOTONIC, &b);
        clock_gettime(CLOCK_REALTIME, &real);
        long long ms = (long long)(b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000;
        printf("monotonic: %s\n", ms >= 0 ? "forward" : "BACKWARD");
        printf("sleep 60 ms: %s\n", (ms >= 50 && ms < 2000) ? "ok" : "WRONG");
        // 1 января 2020 = 1577836800; time() обязан сходиться с часами.
        printf("realtime: %s\n", (real.tv_sec > 1577836800 && time(0) >= real.tv_sec && time(0) - real.tv_sec < 5) ? "after 2020" : "WRONG");
        if (!(ms >= 50 && ms < 2000)) printf("  measured %lld ms\n", ms);
        return 0;
    }
    if (!strcmp(what, "random")) {
        uint8_t a[16], b[16];
        int zero_a = 1;
        if (getentropy(a, sizeof(a)) != 0 || getentropy(b, sizeof(b)) != 0) { puts("random: getentropy failed"); return 1; }
        for (size_t i = 0; i < sizeof(a); i++) zero_a &= a[i] == 0;
        printf("random: %s\n", (!zero_a && memcmp(a, b, sizeof(a)) != 0) ? "ok" : "WRONG");
        return 0;
    }
    if (!strcmp(what, "exit")) {
        puts("exiting");
        exit(argc > 2 ? atoi(argv[2]) : 0);
    }
    if (!strcmp(what, "stdin")) {
        long n = 0;
        while (getchar() != EOF) n++;
        printf("stdin: %ld bytes\n", n);
        return 0;
    }
    if (!strcmp(what, "abort")) {
        puts("aborting");
        fflush(stdout);
        abort();
    }
    if (!strcmp(what, "spin")) {
        puts("spinning");
        fflush(stdout);
        for (;;) g_spin++;
    }
    if (!strcmp(what, "oom")) {
        // По мегабайту; блоки связаны в список и потом читаются — иначе
        // компилятор вправе выбросить и malloc, и запись в «никому не нужную»
        // память, и цикл досчитает до конца, ничего не попросив.
        long mb = 0;
        char* volatile head = 0;
        for (;;) {
            char* p = malloc(1 << 20);
            if (!p) break;
            memset(p, 0x5A, 1 << 20);
            *(char**)p = head;
            head = p;
            mb++;
            if (mb > 4096) { puts("oom: NO LIMIT"); return 1; }
        }
        long seen = 0, good = 0;
        for (char* p = head; p; p = *(char**)p) { seen++; good += p[(1 << 20) - 1] == 0x5A; }
        printf("oom: malloc refused after %ld megabytes, all %s\n", mb,
               (seen == mb && good == mb) ? "intact" : "DAMAGED");
        return 0;
    }
    if (!strcmp(what, "sleep") && argc > 2) {
        // Спать заданное число миллисекунд: ядро в это время должно быть свободно.
        long ms = atol(argv[2]);
        struct timespec nap = { ms / 1000, (ms % 1000) * 1000000L };
        puts("sleeping");
        fflush(stdout);
        nanosleep(&nap, 0);
        puts("slept");
        return 0;
    }
    if (!strcmp(what, "echo")) {
        // Диалог: каждую введённую строку возвращает в ответ (до конца ввода).
        char line[128];
        int n = 0;
        puts("echo: ready");
        fflush(stdout);
        while (fgets(line, sizeof(line), stdin)) {
            line[strcspn(line, "\n")] = 0;
            printf("echo %d: [%s]\n", ++n, line);
            fflush(stdout);
        }
        printf("echo: end of input after %d lines\n", n);
        return 0;
    }
    if (!strcmp(what, "utf8")) {
        static const char one[] = "Привет из WebAssembly\n";
        static const char two[] = "Руна: строка в два приёма\n";
        write(1, one, sizeof(one) - 1);
        write(1, two, 3);                       // «Р» и первый байт «у»
        write(1, two + 3, sizeof(two) - 1 - 3);
        return 0;
    }
    puts("usage: env args|getenv NAME|clock|random|exit N|stdin|abort|spin|oom|utf8");
    return 2;
}

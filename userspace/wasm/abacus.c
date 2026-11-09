// ============================================================================
// abacus.c — «Абак»: калькулятор-диалог (подэтап 51.4, программа для людей).
// ----------------------------------------------------------------------------
// Целые выражения: + - * / % и скобки, унарный минус. Каждая строка ввода —
// выражение, ответ — на следующей строке. С аргументами (`abacus 2+3*4`)
// считает их и выходит. Пустая строка пропускается, «q» или конец ввода —
// выход.
// Арифметика — в uint64_t с переносом (у знакового переполнение в C не
// определено, а вывод обязан совпадать со сборкой под gcc); деление на ноль и
// INT64_MIN / -1 — сообщение, а не ловушка.
// ============================================================================
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __wasm__
// Строка для библиотеки окна Rune — пользовательская секция модуля «rune.about».
// Атрибут section у clang для этого не годится (данные уходят в обычный
// сегмент), секцию создаёт только ассемблерная вставка.
__asm__(".section .custom_section.rune.about,\"\",@\n"
        ".ascii \"Абак: калькулятор целых чисел, + - * / % и скобки\"\n"
        ".text\n");
#endif

static const char* p;                   // разбираемая строка
static const char* err;                 // первая ошибка (0 — нет)

static void skip(void) { while (*p == ' ' || *p == '\t') p++; }
static int64_t expr(void);

static int64_t atom(void) {
    skip();
    if (*p == '(') {
        p++;
        int64_t v = expr();
        skip();
        if (*p == ')') p++; else if (!err) err = "не хватает закрывающей скобки";
        return v;
    }
    if (*p == '-') { p++; return (int64_t)(0u - (uint64_t)atom()); }
    if (*p == '+') { p++; return atom(); }
    if (*p < '0' || *p > '9') { if (!err) err = *p ? "здесь ожидалось число" : "выражение оборвано"; return 0; }
    uint64_t v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10u + (uint64_t)(*p++ - '0');
    return (int64_t)v;
}

static int64_t term(void) {
    int64_t a = atom();
    for (;;) {
        skip();
        char op = *p;
        if (op != '*' && op != '/' && op != '%') return a;
        p++;
        int64_t b = atom();
        if (op == '*') a = (int64_t)((uint64_t)a * (uint64_t)b);
        else if (b == 0) { if (!err) err = "деление на ноль"; a = 0; }
        else if (a == INT64_MIN && b == -1) { if (!err) err = "результат не помещается в 64 бита"; a = 0; }
        else a = op == '/' ? a / b : a % b;
    }
}

static int64_t expr(void) {
    int64_t a = term();
    for (;;) {
        skip();
        char op = *p;
        if (op != '+' && op != '-') return a;
        p++;
        int64_t b = term();
        a = op == '+' ? (int64_t)((uint64_t)a + (uint64_t)b) : (int64_t)((uint64_t)a - (uint64_t)b);
    }
}

static void eval(const char* s) {
    p = s;
    err = 0;
    int64_t v = expr();
    skip();
    if (!err && *p && *p != '\n' && *p != '\r') err = "лишнее в конце выражения";
    if (err) printf("  ошибка: %s\n", err);
    else printf("  = %" PRId64 "\n", v);
}

int main(int argc, char** argv) {
    static char line[512];
    if (argc > 1) {
        // Аргументы склеиваются через пробел: `abacus 2 + 3` и `abacus 2+3` — одно и то же.
        size_t n = 0;
        for (int i = 1; i < argc; i++)
            for (const char* c = argv[i]; n < sizeof(line) - 2; c++) {
                if (!*c) { line[n++] = ' '; break; }
                line[n++] = *c;
            }
        line[n] = 0;
        printf("%s\n", line);
        eval(line);
        return 0;
    }
    puts("Абак. Введите выражение, например (2 + 3) * 4. Выход - q.");
    for (;;) {
        fputs(": ", stdout);
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) { puts(""); break; }
        const char* s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\n' || *s == '\r' || !*s) continue;
        if ((*s == 'q' || *s == 'Q') && (s[1] == '\n' || s[1] == '\r' || !s[1])) break;
        eval(s);
    }
    puts("Абак убран.");
    return 0;
}

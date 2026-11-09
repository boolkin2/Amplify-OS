// ============================================================================
// rune.h — WebAssembly в AMPLIFY-OS: связка движка (wasm.h) с ядром (этап 51).
// ----------------------------------------------------------------------------
// «Руна» — так в ОС зовётся модуль WebAssembly: маленькая переносимая
// программа, которую система исполняет сама, не доверяя ей ни памяти, ни
// файлов. Движок (wasm_*.c) о ядре не знает; здесь — всё, что знает: чтение
// модуля с диска, самопроверка при загрузке, команды терминала.
// Подэтап 51.0: модуль разбирается и проверяется. Подэтап 51.1: исполняется
// интерпретатором — пока прямо на BSP, по команде `rune call`, и без
// импортов. Подэтап 51.2: `rune run` — программа с окружением WASI (вывод,
// файлы в одном каталоге, аргументы, часы); своё ядро и пределы — 51.3.
// Все функции — для BSP.
// ============================================================================
#ifndef AOS_X86_64_RUNE_H
#define AOS_X86_64_RUNE_H

#include <stdint.h>
#include <stdbool.h>

typedef void (*rune_sink_t)(const char* s);

// smoke 51: встроенные модули разбираются и проверяются, испорченные —
// отклоняются с верной причиной. Строки `[smoke 51] …` в serial.
bool rune_boot_selftest(void);

// `rune selftest` — то же из терминала. Возвращает число провалов.
int  rune_selftest(rune_sink_t out);

// `rune info <файл>` — что в модуле: счётчики, память, импорты и экспорты.
// Строки `[rune] …` идут в serial и в out. false — файла нет или модуль отклонён.
bool rune_info(rune_sink_t out, const char* path);

// `rune spec <пакет>` — официальный набор тестов (build/wasm/spec.pack,
// scripts/wasm_spec_pack.sh). Итог — строка `[wasm-spec] total: …`.
bool rune_spec(rune_sink_t out, const char* path, bool verbose);

// [51.1] `rune call <файл> <функция> [аргументы]` — загрузить модуль, запустить
// и вызвать экспортированную функцию. Целые аргументы — десятичные или 0x…,
// дробные — вида 1.5. Итог — строка `[rune] call f(…) = значение | N ms, fuel K`
// (f32/f64 печатаются битами) либо `… trapped: причина`. Исполнение идёт на
// BSP: интерфейс на это время стоит, поэтому есть предел времени limit_ms
// (0 — по умолчанию), после которого программа останавливается.
#define RUNE_DEFAULT_LIMIT_MS 10000u
bool rune_call(rune_sink_t out, const char* path, const char* func, int nargs, char** args, uint32_t limit_ms);

// [51.2, 51.3] Запуск программы WASI (wasm32-wasi: обычный C с printf и fopen).
// С 51.3 программа исполняется на ОТДЕЛЬНОМ ЯДРЕ (если оно есть): rune_start
// возвращается сразу, а всё, что вычислительному ядру нельзя (диск, окно,
// выделение памяти), по её просьбе делает главное ядро в rune_tick() — её
// зовёт цикл WM. Одновременно — одна программа.
//   dir         каталог, который программа видит как «/» (0 — RUNE_DEFAULT_DIR);
//               только на томе FAT; корень тома и /disk/Sanctum не даются;
//   read_only   менять в каталоге нельзя;  no_files — каталога нет вовсе;
//   interactive ввод остаётся открытым (rune_input), иначе — сразу «конец ввода»;
//   cpu_ms      предел времени счёта, мс (0 — без предела; на машине без
//               второго ядра программа идёт на главном, и предел обязателен:
//               0 = RUNE_DEFAULT_LIMIT_MS);
//   memory_mb   предел памяти программы (0 — 64 МБ).
// Вывод программы и строки `[rune] run …`, `[rune] exit N | …`,
// `[rune] stopped: причина | …` идут в out (0 — только в журнал serial).
#define RUNE_DEFAULT_DIR "/disk/nexus"
typedef struct {
    const char* path;
    int         nargs;
    char**      args;
    const char* dir;
    bool        read_only, no_files, interactive;
    uint32_t    cpu_ms, memory_mb;
    void*       owner;                  // чья это программа (окно терминала); 0 — ничья (фон)
} rune_opts_t;
bool rune_start(const rune_opts_t* o, rune_sink_t out);
void rune_tick(void);                   // цикл WM: услуги работающей программе
bool rune_active(void);                 // программа запущена и ещё не кончилась
bool rune_wants_service(void);          // у главного ядра есть для неё работа прямо сейчас
void rune_stop(void);                   // остановить (итог придёт строкой `[rune] stopped: by user`)
void rune_input(const char* data, int n);   // ввод программе (клавиатура терминала, `rune type`)
void rune_input_eof(void);
void rune_status(rune_sink_t out);      // `rune status`
// Владелец работающей программы (rune_opts_t.owner) или 0. Ему идёт её вывод,
// от него — её ввод. rune_disown — владелец закрылся, программа остаётся.
void* rune_owner(void);
void  rune_disown(void);

// [51.4] То же, что печатает `rune status`, но числами — для окна Rune.
// Вторая половина — итог ПОСЛЕДНЕЙ закончившейся программы: он переживает и
// её саму, и запуск следующей (до её собственного конца); ends — счётчик
// закончившихся программ, по нему окно узнаёт о конце, даже если пропустило
// сам момент.
enum { RUNE_ST_IDLE = 0, RUNE_ST_COMPUTING, RUNE_ST_SYSCALL, RUNE_ST_INPUT, RUNE_ST_SLEEPING };
enum { RUNE_END_NONE = 0, RUNE_END_EXIT, RUNE_END_STOPPED };
typedef struct {
    bool     active;
    int      state;                     // RUNE_ST_*
    int      core;                      // 0 — главное ядро (второго нет)
    void*    owner;
    char     path[160];
    uint64_t cpu_ms, wall_ms, services;
    uint32_t memory_pages;              // сейчас, страниц по 64 КБ
    uint32_t memory_limit_mb;           // 0 — предел по умолчанию (64 МБ)
    bool     input_open;                // программе ещё можно что-то ввести
    bool     jit;                       // [51.5] исполняется машинным кодом (иначе — интерпретатором)
    // ---- последняя закончившаяся ----
    uint64_t ends;
    int      end_kind;                  // RUNE_END_*
    int      exit_code;
    char     end_text[64];              // причина остановки (как в строке `[rune] stopped: …`)
    uint64_t end_cpu_ms, end_wall_ms;
    uint32_t end_pages;
} rune_state_t;
void rune_query(rune_state_t* out);

// [51.4] Почему последний rune_start ответил false (та же причина, что ушла в
// out и в журнал). Пустая строка — отказа не было.
const char* rune_last_error(void);

// [51.4] Библиотека программ для окна Rune (и `rune list`): встроенные — файлы
// *.wasm из initrd, свои — из каталога RUNE_PROG_DIR тома FAT. path годится для
// rune_start. about — строка из пользовательской секции «rune.about» модуля
// (программа описывает себя сама; секции нет — строка пустая), уже в CP1251.
#define RUNE_PROG_DIR "/disk/vault/runes"
typedef struct {
    char     name[64];
    char     path[160];
    char     about[128];
    uint32_t bytes;
    bool     builtin;
} rune_prog_t;
int rune_programs(rune_prog_t* out, int max);

// [51.5] JIT: программы, запущенные ПОСЛЕ вызова, исполняются машинным кодом
// (по умолчанию) или интерпретатором. `rune jit [on|off]`.
void rune_jit_set(bool on);
bool rune_jit_on(void);

// Зовётся из таймера ядра раз в миллисекунду: ставит флаг остановки, когда
// у исполняемой программы вышло время.
void rune_tick_1ms(void);

#endif

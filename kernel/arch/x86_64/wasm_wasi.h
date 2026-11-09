// ============================================================================
// wasm_wasi.h — окружение WASI (preview1) для программ WebAssembly (этап 51.2).
// ----------------------------------------------------------------------------
// Модуль WebAssembly сам не умеет ничего, кроме счёта: вывод, файлы, часы,
// аргументы он просит у окружения через импорты. WASI preview1 — принятый
// набор таких импортов (модуль `wasi_snapshot_preview1`, 45 функций); под него
// собирает clang с wasi-libc, то есть обычная программа на C с printf и fopen.
//
// Этот слой — общий для ядра и для сборки на хосте, как и весь движок: всё,
// что зависит от того, ГДЕ мы исполняемся, вынесено в wasi_host_t (вывод,
// чтение и запись файла целиком, список каталога, часы, случайные байты).
// А вся ПОЛИТИКА живёт здесь и проверяется одинаково там и там:
//   * таблица описателей программы (0, 1, 2 — стандартные потоки, с 3 —
//     каталоги, которые ей открыты заранее);
//   * ГРАНИЦА: программа видит только заранее открытые каталоги. Любой путь
//     разбирается по компонентам здесь, «..» выше корня каталога — отказ,
//     абсолютный путь — отказ. До хозяина доходит уже готовый путь внутри
//     разрешённого каталога;
//   * каждый указатель и каждая длина, пришедшие от программы, сверяются с
//     размером её памяти (EFAULT), прежде чем по ним что-то читать или писать.
//
// Файлы. Хозяин умеет только «прочитать файл целиком» и «записать файл
// целиком» (так устроен драйвер FAT ядра). Поэтому открытый файл — это его
// копия в памяти: чтение, запись и перемещение идут по копии, на диск она
// уходит при закрытии, по fsync и при завершении программы.
// ============================================================================
#ifndef AOS_X86_64_WASM_WASI_H
#define AOS_X86_64_WASM_WASI_H

#include "wasm.h"

// Коды ошибок WASI — те, что мы возвращаем (значения из спецификации).
enum {
    WASI_OK = 0, WASI_E2BIG = 1, WASI_EACCES = 2, WASI_EBADF = 8, WASI_EEXIST = 20, WASI_EFAULT = 21,
    WASI_EFBIG = 22, WASI_EILSEQ = 25, WASI_EINVAL = 28, WASI_EIO = 29, WASI_EISDIR = 31, WASI_EMFILE = 33,
    WASI_ENAMETOOLONG = 37, WASI_ENOENT = 44, WASI_ENOMEM = 48, WASI_ENOSPC = 51, WASI_ENOSYS = 52,
    WASI_ENOTDIR = 54, WASI_ENOTEMPTY = 55, WASI_ENOTSUP = 58, WASI_EPERM = 63, WASI_EROFS = 69,
    WASI_ESPIPE = 70, WASI_ENOTCAPABLE = 76,
};

#define WASI_PATH_MAX     256           // путь хозяина вместе с каталогом-корнем
#define WASI_MAX_FDS      48
#define WASI_MAX_PREOPENS 4

typedef struct {
    bool     is_dir;
    uint64_t size;
    uint64_t mtime_ns;                  // 0 — хозяин времени не знает
} wasi_stat_t;

// Очередная запись каталога; false — дальше не нужно.
typedef bool (*wasi_list_fn)(void* user, const char* name, bool is_dir);

// Что даёт хозяин. Пути — абсолютные, уже проверенные; коды возврата — WASI_*.
typedef struct {
    void*    ctx;
    void     (*out)(void* ctx, int fd, const uint8_t* data, uint32_t n);    // fd: 1 — вывод, 2 — ошибки
    uint32_t (*in)(void* ctx, uint8_t* buf, uint32_t n);                    // ввод; 0 — конец. Может быть 0.
                                                                            // WASI_IN_WAIT — пока пусто (см. nonblocking)
    int      (*stat)(void* ctx, const char* path, wasi_stat_t* st);
    int      (*load)(void* ctx, const char* path, uint8_t* buf, uint64_t size);         // ровно size байт
    int      (*save)(void* ctx, const char* path, const uint8_t* data, uint64_t size);  // создать или заменить
    int      (*mkdir)(void* ctx, const char* path);
    int      (*remove)(void* ctx, const char* path, bool dir);
    int      (*rename)(void* ctx, const char* from, const char* to);        // может быть 0: тогда файл копируется
    int      (*list)(void* ctx, const char* path, wasi_list_fn cb, void* user);
    uint64_t (*clock_ns)(void* ctx, bool realtime);                         // настенное время (от 1970) или «от запуска»
    void     (*random)(void* ctx, uint8_t* buf, uint32_t n);
    bool     (*sleep_ns)(void* ctx, uint64_t ns);                           // false — ожидание прервано снаружи
} wasi_host_t;

// Ответ in(): ввода пока нет, но он ещё может прийти. Только при nonblocking.
#define WASI_IN_WAIT 0xFFFFFFFFu

// Каталог, открытый программе заранее: под именем guest она видит каталог
// хозяина host (и всё, что в нём), и ничего кроме.
typedef struct {
    const char* guest;                  // обычно "/"
    const char* host;                   // без завершающего '/'
    bool        read_only;
} wasi_preopen_t;

// Строки и массивы конфигурации обязаны жить дольше окружения (не копируются).
typedef struct {
    int                   argc;
    const char* const*    argv;
    int                   envc;
    const char* const*    envp;         // "ИМЯ=значение"
    int                   n_preopens;
    const wasi_preopen_t* preopens;
    uint64_t              max_file_bytes;   // потолок одного открытого файла (0 — 16 МБ)
    uint64_t              max_total_bytes;  // … и всех открытых файлов вместе (0 — 64 МБ)
    // [51.3] Хозяину нельзя ждать (он — главное ядро системы, на нём окна):
    // чтение пустого ввода и сон не блокируют, а отвечают WASM_HOST_RETRY —
    // «вызови ещё раз». Повторяет тот, кто исполняет программу отрезками
    // (wasm_service); когда повторить сон — wasi_wake_ns. sleep_ns не зовётся.
    bool                  nonblocking;
    wasi_host_t           host;
} wasi_config_t;

typedef struct wasi wasi_t;

// Создать окружение в хранилище s (функции WASI становятся его функциями
// хоста). 0 — нет памяти или неверная конфигурация. Освобождать ДО хранилища.
wasi_t* wasi_new(wasm_store_t* s, const wasi_config_t* cfg);
// Закрыть всё, что программа оставила открытым (несохранённые файлы пишутся),
// и освободить. Возвращает число файлов, которые не удалось сохранить.
int     wasi_free(wasi_t* w);
// Для wasm_instantiate: отдаёт импорты модуля wasi_snapshot_preview1. ctx — wasi_t*.
bool    wasi_resolve(void* ctx, const wasm_module_t* m, const wasm_import_t* imp, wasm_extern_t* out);

enum {
    WASI_RUN_EXIT = 0,                  // программа завершилась сама: exit_code
    WASI_RUN_TRAP,                      // остановлена: trap (WASM_TRAP_*)
    WASI_RUN_NOSTART,                   // не запустилась: text
};
typedef struct {
    int      status;                    // WASI_RUN_*
    int      exit_code;
    int      trap;
    int      unsaved;                   // файлов, которые не удалось сохранить при завершении
    uint64_t calls;                     // сколько раз программа обратилась к окружению
    uint32_t memory_pages;              // память программы к концу работы, страниц по 64 КБ
    char     text[128];
} wasi_result_t;

// Запустить программу-«команду»: создать экземпляр (импорты — из w), вызвать
// _start, закрыть её файлы. Пределы времени задаются хранилищу заранее
// (wasm_store_limit).
void wasi_run(wasm_store_t* s, const wasm_module_t* m, wasi_t* w, wasi_result_t* r);

// [51.3] То же в два приёма — для исполнения отрезками на другом ядре.
// wasi_prepare создаёт экземпляр и находит _start (её вызывают сами:
// wasm_call / wasm_resume / wasm_service). false — вызывать нечего: программа
// не запустилась или уже завершилась, r заполнен, файлы закрыты.
// wasi_finish подводит итог по результату вызова _start (0 или ловушка) и
// закрывает файлы программы. Обе — только там, где можно писать на диск.
bool wasi_prepare(wasm_store_t* s, const wasm_module_t* m, wasi_t* w, wasi_result_t* r, wasm_funcinst_t** start);
void wasi_finish(wasm_store_t* s, wasi_t* w, int trap, wasi_result_t* r);
// Программа спит (poll_oneoff) — до какого момента по монотонным часам хозяина.
// 0 — не спит.
uint64_t wasi_wake_ns(const wasi_t* w);
uint32_t wasi_memory_pages(const wasi_t* w);     // [51.4] память программы сейчас, страниц
// Сколько байт программа вывела и сколько раз обратилась к окружению.
uint64_t wasi_calls(const wasi_t* w);

#endif

// ============================================================================
// wasm.h — движок WebAssembly AMPLIFY-OS «rune» (этап 51).
// ----------------------------------------------------------------------------
// Свой движок, не перенос чужого (решение пользователя, 07.10.2026). Объём
// языка — WebAssembly 2.0 без SIMD: базовый набор 1.0 плюс то, что выдаёт
// clang 19 для wasm32-wasi (несколько результатов у блоков и функций,
// расширение знака, преобразования с насыщением, групповые операции с памятью
// и таблицами, ссылочные типы). Чего нет и о чём модуль получит честный
// отказ при проверке: SIMD (v128), потоки и атомарные операции, исключения,
// хвостовые вызовы, сборка мусора, 64-битная память, несколько памятей.
//
// Слои (каждый — свой файл, общий для ядра и для сборки на хосте):
//   wasm_bin.c   — разбор двоичного модуля в wasm_module_t. Строки и тела
//                  функций НЕ копируются: структуры ссылаются на исходные
//                  байты, поэтому буфер с модулем обязан жить дольше модуля.
//   wasm_valid.c — проверка тел функций по алгоритму из приложения к
//                  спецификации (стек типов + стек управляющих блоков).
//                  После неё исполнителю не нужны ни проверки типов, ни
//                  проверки глубины стека: всё доказано заранее.
//                  [51.1] Она же переводит тело функции во внутренний код
//                  (wasm_int.h): переходы уже разрешены в адреса, все числа
//                  раскодированы.
//   wasm_rt.c    — [51.1] хранилище и экземпляры: память, таблицы, глобальные,
//                  связывание импортов, запуск модуля.
//   wasm_interp.c— [51.1] интерпретатор внутреннего кода.
//   wasm_spec.c  — прогон официального набора тестов (упакованного
//                  tools/wasm_pack_spec.py) — один и тот же код в ядре
//                  (`rune spec`) и на хосте (tools/wasm_host.c).
// Движок не знает, где он исполняется: память берёт через wasm_port_alloc /
// wasm_port_free (в ядре — куча, на хосте — malloc) и ничего не печатает.
// ============================================================================
#ifndef AOS_X86_64_WASM_H
#define AOS_X86_64_WASM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define WASM_PAGE 65536u

// Типы значений — кодами двоичного формата.
enum {
    WT_I32 = 0x7F, WT_I64 = 0x7E, WT_F32 = 0x7D, WT_F64 = 0x7C,
    WT_V128 = 0x7B,                     // SIMD: распознаём, чтобы отказать внятно
    WT_FUNCREF = 0x70, WT_EXTERNREF = 0x6F,
};

// Виды импорта и экспорта.
enum { WK_FUNC = 0, WK_TABLE = 1, WK_MEM = 2, WK_GLOBAL = 3 };

// Пределы реализации. Формат допускает до 2^32 всего подряд; модуль, который
// не помещается в эти числа, отклоняется как «слишком большой для этой
// машины» (WASM_E_LIMIT), а не как неверный. Числа — того же порядка, что у
// движков браузеров.
#define WASM_MAX_TYPES       100000u
#define WASM_MAX_FUNCS       200000u
#define WASM_MAX_IMPORTS     100000u
#define WASM_MAX_EXPORTS     100000u
#define WASM_MAX_GLOBALS     100000u
#define WASM_MAX_TABLES      1000u
#define WASM_MAX_SEGMENTS    100000u
#define WASM_MAX_PARAMS      1000u
#define WASM_MAX_RESULTS     1000u
#define WASM_MAX_LOCALS      50000u     // вместе с параметрами
#define WASM_MAX_BODY        (16u << 20)
#define WASM_MAX_NESTING     20000u     // вложенность блоков в одной функции
#define WASM_MAX_OPSTACK     (1u << 20) // глубина стека операндов при проверке

// Чем кончилась загрузка.
enum {
    WASM_OK = 0,
    WASM_E_MALFORMED,                   // это не двоичный модуль (ошибка кодирования)
    WASM_E_INVALID,                     // закодирован верно, но нарушает правила типов
    WASM_E_LIMIT,                       // верный, но больше наших пределов
    WASM_E_NOMEM,
    WASM_E_UNSUPPORTED,                 // возможность, которой у нас нет (SIMD и т.п.)
};

typedef struct {
    int      kind;                      // WASM_E_*
    uint32_t offset;                    // смещение в файле, где замечена ошибка
    char     text[96];
} wasm_err_t;

// Строка UTF-8 внутри двоичного модуля (без завершающего нуля).
typedef struct { const uint8_t* p; uint32_t n; } wasm_name_t;

typedef struct {
    uint8_t* types;                     // сначала параметры, затем результаты
    uint32_t n_params, n_results;
} wasm_functype_t;

typedef struct { uint32_t min, max; bool has_max; } wasm_limits_t;
typedef struct { uint8_t elem; wasm_limits_t lim; bool imported; } wasm_tabletype_t;
typedef struct { wasm_limits_t lim; bool imported; } wasm_memtype_t;

// Константное выражение (начальное значение глобальной, смещение сегмента).
// В 2.0 это ровно одна команда, поэтому хранится не код, а его смысл.
enum { WCX_CONST = 0, WCX_GLOBAL, WCX_REF_NULL, WCX_REF_FUNC };
typedef struct {
    uint8_t  kind;                      // WCX_*
    uint8_t  type;                      // тип результата
    uint64_t bits;                      // значение (биты) либо индекс глобальной / функции
} wasm_cexpr_t;

typedef struct {
    wasm_name_t module, field;
    uint8_t     kind;                   // WK_*
    uint32_t    index;                  // номер в своём пространстве индексов
} wasm_import_t;

typedef struct {
    uint32_t       type;                // индекс типа
    bool           imported;
    const uint8_t* locals;              // группы «сколько, какого типа» как в файле
    uint32_t       n_local_groups;
    uint32_t       n_locals;            // сколько локальных без параметров
    const uint8_t* code;                // команды до конца тела (последняя — end)
    uint32_t       code_len;
    uint32_t       max_stack;           // наибольшая глубина стека операндов (после проверки)
    uint64_t*      ir;                  // [51.1] внутренний код (wasm_int.h); 0 у импортированной
    uint32_t       ir_len;
} wasm_func_t;

typedef struct {
    uint8_t      type;
    bool         mut, imported;
    wasm_cexpr_t init;                  // у импортированной не используется
} wasm_global_t;

typedef struct { wasm_name_t name; uint8_t kind; uint32_t index; } wasm_export_t;

enum { WSEG_ACTIVE = 0, WSEG_PASSIVE, WSEG_DECLARATIVE };
#define WASM_ELEM_NULL    0xFFFFFFFFu   // элемент сегмента — пустая ссылка
#define WASM_ELEM_GLOBAL  0x80000000u   // … или значение глобальной (младшие биты — её номер)
typedef struct {
    uint8_t      mode;                  // WSEG_*
    uint8_t      type;                  // funcref / externref
    uint32_t     table;
    wasm_cexpr_t offset;
    uint32_t     n;
    uint32_t*    items;                 // номера функций либо WASM_ELEM_*
} wasm_elem_t;

typedef struct {
    uint8_t        mode;                // WSEG_ACTIVE / WSEG_PASSIVE
    wasm_cexpr_t   offset;
    const uint8_t* bytes;
    uint32_t       n;
} wasm_data_t;

typedef struct wasm_module {
    const uint8_t* bin;                 // исходные байты (владеет вызывающий)
    uint32_t       bin_len;

    wasm_functype_t*  types;    uint32_t n_types;
    wasm_import_t*    imports;  uint32_t n_imports;
    wasm_func_t*      funcs;    uint32_t n_funcs,   n_imp_funcs;    // импортированные — первыми
    wasm_tabletype_t* tables;   uint32_t n_tables,  n_imp_tables;
    wasm_memtype_t*   mems;     uint32_t n_mems,    n_imp_mems;
    wasm_global_t*    globals;  uint32_t n_globals, n_imp_globals;
    wasm_export_t*    exports;  uint32_t n_exports;
    wasm_elem_t*      elems;    uint32_t n_elems;
    wasm_data_t*      datas;    uint32_t n_datas;
    bool     has_start;         uint32_t start;
    bool     has_data_count;    uint32_t data_count;

    // Функции, на которые модуль вправе брать ссылку командой ref.func: те,
    // что названы вне тел функций (экспорт, сегмент элементов, глобальная).
    uint8_t* declared;                  // битовая карта по n_funcs

    uint32_t n_custom;                  // сколько пользовательских секций (имена, сведения о сборке)
    uint64_t code_bytes;                // суммарный размер тел функций
    uint64_t ir_words;                  // [51.1] … и их внутреннего кода, в словах по 8 байт

    wasm_err_t err;
    void*      arena;                   // блоки памяти модуля (wasm_int.h)
} wasm_module_t;

// Разобрать и проверить модуль. bin должен жить, пока жив модуль. 0 — отказ,
// причина в *err (если err не 0).
wasm_module_t* wasm_module_load(const uint8_t* bin, uint32_t len, wasm_err_t* err);
void           wasm_module_free(wasm_module_t* m);

const char* wasm_type_name(uint8_t t);          // "i32", "funcref", …
const char* wasm_kind_name(uint8_t k);          // "func", "table", "memory", "global"
const char* wasm_err_name(int kind);            // "malformed", "invalid", …
// Найти экспорт по имени и виду. -1 — нет такого.
int64_t     wasm_find_export(const wasm_module_t* m, const char* name, uint8_t kind);

// ---- порт (в ядре — rune.c, на хосте — tools/wasm_host.c) ---------------------
void* wasm_port_alloc(size_t n);                // блок не обнулён; 0 — памяти нет
void  wasm_port_free(void* p);
// [51.1] Линейная память экземпляра: большой непрерывный кусок, ОБНУЛЁННЫЙ.
// Отдельно от wasm_port_alloc: это десятки мегабайт, в ядре им не место в куче.
void* wasm_port_mem_alloc(uint64_t bytes);
void  wasm_port_mem_free(void* p, uint64_t bytes);
// [51.5] Память под машинный код JIT: в неё пишут, потом из неё исполняют.
void* wasm_port_exec_alloc(uint64_t bytes);
void  wasm_port_exec_free(void* p, uint64_t bytes);

// ============================================================================
// [51.1] Исполнение.
// ----------------------------------------------------------------------------
// Хранилище (store) владеет всем, что живёт во время работы: экземплярами
// модулей, их памятью, таблицами, глобальными, а также стеком интерпретатора.
// Экземпляры одного хранилища могут импортировать друг у друга (общая память,
// общая таблица). Освобождается хранилище целиком; модули (wasm_module_t)
// обязаны жить дольше хранилища — экземпляры ссылаются на их код.
// Одно хранилище — один поток исполнения.
// ============================================================================
typedef struct wasm_store      wasm_store_t;
typedef struct wasm_instance   wasm_instance_t;
typedef struct wasm_funcinst   wasm_funcinst_t;
typedef struct wasm_memory     wasm_memory_t;
typedef struct wasm_table      wasm_table_t;
typedef struct wasm_globalinst wasm_globalinst_t;

// Значение на стеке и в ячейках — 64 бита без типа (типы доказаны проверкой):
// i32 и f32 — в младших 32 битах (старшие нули), i64 и f64 — целиком, ссылка —
// 0 для пустой, иначе указатель на функцию (funcref) или число хоста (externref).
typedef uint64_t wasm_val_t;

// Причины остановки исполнения («ловушки»). Первые десять — из спецификации,
// их названия сверяются с текстом официальных тестов.
enum {
    WASM_TRAP_NONE = 0,
    WASM_TRAP_UNREACHABLE,              // команда unreachable
    WASM_TRAP_DIV_ZERO,                 // целое деление на ноль
    WASM_TRAP_INT_OVERFLOW,             // INT_MIN / -1; число не влезает при преобразовании
    WASM_TRAP_BAD_CONVERSION,           // NaN -> целое
    WASM_TRAP_MEM_OOB,                  // обращение за границу памяти
    WASM_TRAP_TABLE_OOB,                // … таблицы
    WASM_TRAP_UNDEF_ELEM,               // call_indirect: индекс за концом таблицы
    WASM_TRAP_NULL_ELEM,                // call_indirect: в ячейке пусто
    WASM_TRAP_SIG_MISMATCH,             // call_indirect: у функции другой тип
    WASM_TRAP_STACK,                    // стек вызовов исчерпан
    WASM_TRAP_FUEL,                     // кончилось отпущенное «топливо» (наш предел времени)
    WASM_TRAP_STOPPED,                  // остановлен снаружи
    WASM_TRAP_HOST,                     // функция хоста сообщила об ошибке
    WASM_TRAP_EXIT,                     // программа завершилась сама (proc_exit, 51.2)
    WASM_TRAP_NOMEM,
};
const char* wasm_trap_name(int trap);

// Пределы хранилища; 0 в поле — значение по умолчанию.
typedef struct {
    uint32_t max_memory_pages;          // потолок памяти одного экземпляра, страниц по 64 КБ (1024 = 64 МБ)
    uint32_t max_table_elems;           // … и таблицы (1 048 576)
    uint32_t stack_slots;               // стек значений интерпретатора (262 144 = 2 МБ)
    uint32_t max_frames;                // глубина вызовов (16 384)
} wasm_config_t;

wasm_store_t* wasm_store_new(const wasm_config_t* cfg);
void          wasm_store_free(wasm_store_t* s);
// «Топливо»: сколько ещё отпущено исполнению (единица — 256 витков циклов и
// вызовов). Отрицательное — без предела. stop — флаг остановки снаружи
// (читается раз в 256 витков), 0 — нет.
void          wasm_store_limit(wasm_store_t* s, int64_t fuel, volatile int* stop);
uint64_t      wasm_store_ticks(const wasm_store_t* s);     // сколько единиц «топлива» сожжено всего

// Всё, что можно импортировать и экспортировать.
typedef struct { uint8_t kind; void* ptr; } wasm_extern_t;  // WK_*; funcinst / table / memory / globalinst

// Функция хоста. vals — её аргументы; туда же она кладёт результаты.
// Возвращает WASM_TRAP_NONE или причину остановки.
typedef int (*wasm_host_fn)(wasm_instance_t* caller, wasm_val_t* vals, void* user);
wasm_funcinst_t*   wasm_host_func(wasm_store_t* s, const uint8_t* params, uint32_t n_params,
                                  const uint8_t* results, uint32_t n_results, wasm_host_fn fn, void* user);
wasm_memory_t*     wasm_memory_new(wasm_store_t* s, uint32_t min_pages, uint32_t max_pages, bool has_max);
wasm_table_t*      wasm_table_new(wasm_store_t* s, uint8_t elem, uint32_t min, uint32_t max, bool has_max);
wasm_globalinst_t* wasm_global_new(wasm_store_t* s, uint8_t type, bool mut, wasm_val_t value);

// Кто даёт модулю его импорты. true — нашёл и положил в *out.
typedef bool (*wasm_resolve_fn)(void* ctx, const wasm_module_t* m, const wasm_import_t* imp, wasm_extern_t* out);

enum { WASM_INST_OK = 0, WASM_INST_LINK, WASM_INST_TRAP, WASM_INST_NOMEM };
typedef struct {
    int  kind;                          // WASM_INST_*
    int  trap;                          // при WASM_INST_TRAP
    char text[96];
} wasm_inst_err_t;

// Создать экземпляр: связать импорты, выделить память и таблицы, разложить
// сегменты, выполнить функцию start. 0 — отказ, причина в *err.
wasm_instance_t* wasm_instantiate(wasm_store_t* s, const wasm_module_t* m, wasm_resolve_fn resolve,
                                  void* ctx, wasm_inst_err_t* err);
bool wasm_instance_export(const wasm_instance_t* inst, const uint8_t* name, uint32_t n, wasm_extern_t* out);

// Вызвать функцию: args — по числу параметров, results — по числу результатов.
int  wasm_call(wasm_store_t* s, wasm_funcinst_t* f, const wasm_val_t* args, wasm_val_t* results);

// ---- [51.3] исполнение отрезками и на другом ядре -------------------------------
// wasm_call может вернуть не итог, а ПАУЗУ (отрицательное число): вызов не
// закончен, его положение сохранено в хранилище.
//   WASM_PAUSED_YIELD   — отрезок выработан; продолжить — wasm_resume;
//   WASM_PAUSED_SERVICE — следующей команде нужно то, чего на этом ядре нельзя
//                         (функция хоста, выделение памяти): её исполняет
//                         wasm_service там, где можно, затем — wasm_resume.
// Функция хоста вправе ответить WASM_HOST_RETRY: «пока нечем ответить, вызови
// ещё раз» (ввода нет, срок сна не вышел). Тогда wasm_service снова вернёт
// WASM_PAUSED_SERVICE, и вызов останется несделанным. Такая функция обязана
// ничего не менять до того, как решит ответить.
#define WASM_PAUSED_YIELD    (-1)
#define WASM_PAUSED_SERVICE  (-2)
#define WASM_HOST_RETRY      (-3)
// remote: привилегированные команды — только через wasm_service.
void wasm_store_remote(wasm_store_t* s, bool remote);
// Длина отрезка в единицах «топлива» (0 — без отрезков) и сама единица в
// витках (по умолчанию 256; единица 1 нужна тестам — пауза на каждом витке).
void wasm_store_slice(wasm_store_t* s, uint32_t units, uint32_t tick_unit);
bool wasm_store_paused(const wasm_store_t* s);
int  wasm_resume(wasm_store_t* s, wasm_val_t* results);
int  wasm_service(wasm_store_t* s, wasm_val_t* results);
// Бросить прерванный вызов (программу остановили, пока она ждала).
void wasm_abort(wasm_store_t* s);

// ---- [51.5] JIT ---------------------------------------------------------------
// Функции модуля при создании экземпляра переводятся в машинный код x86-64
// (wasm_jit.c). Выключатель — на всё, что будет создано ПОСЛЕ вызова (новые
// хранилища); по умолчанию включён. Поведение программы от него не зависит —
// только скорость.
void wasm_jit_default(bool on);
bool wasm_jit_enabled(void);
// Что вышло у хранилища: сколько функций переведено и сколько байт машинного
// кода (0 и 0 — исполняет интерпретатор).
void wasm_store_jit_info(const wasm_store_t* s, uint32_t* funcs, uint64_t* bytes);

const wasm_functype_t* wasm_func_type(const wasm_funcinst_t* f);
uint8_t*   wasm_memory_data(wasm_memory_t* mem, uint64_t* bytes);
uint32_t   wasm_memory_pages(const wasm_memory_t* mem);
wasm_memory_t* wasm_instance_memory(const wasm_instance_t* inst);      // 0 — памяти нет
wasm_val_t wasm_global_get(const wasm_globalinst_t* g, uint8_t* type);
// Ссылка на число хоста и обратно: 0 занят под «пусто», поэтому сдвиг на 1.
static inline wasm_val_t wasm_externref(uint64_t v) { return v + 1; }

// ---- отчёты (wasm_spec.c) ---------------------------------------------------------
typedef void (*wasm_sink_t)(const char* s);
// Описание модуля строками: счётчики, память, первые max_items импортов и
// экспортов с типами. Каждая строка начинается с prefix.
void wasm_describe(const wasm_module_t* m, wasm_sink_t out, const char* prefix, uint32_t max_items);

// ---- официальный набор тестов (wasm_spec.c) ------------------------------------
typedef struct {
    uint32_t files;
    uint32_t modules, modules_ok;               // верные модули: сколько и сколько принято
    uint32_t invalid, invalid_ok;               // assert_invalid: сколько и сколько отклонено
    uint32_t malformed, malformed_ok;           // assert_malformed (двоичные)
    uint32_t skipped_text;                      // модули в текстовом виде — не наш слой
    uint32_t skipped_exec;                      // проверки, которые не выполнялись (нет экземпляра и т.п.)
    // [51.1] исполнение: всего / сошлось
    uint32_t returns, returns_ok;               // assert_return: вызов дал ожидаемые значения
    uint32_t traps, traps_ok;                   // assert_trap: вызов прерван нужной причиной
    uint32_t exhaust, exhaust_ok;               // assert_exhaustion: стек вызовов исчерпан
    uint32_t unlinkable, unlinkable_ok;         // модуль не связывается (нет импорта или не тот тип)
    uint32_t uninst, uninst_ok;                 // модуль прерывается при запуске
    uint32_t actions, actions_ok;               // вызовы без проверки результата
    uint32_t failures;
} wasm_spec_stats_t;
// Прогнать упакованный набор. Строки отчёта уходят в out. false — пакет повреждён.
bool wasm_spec_run(const uint8_t* pack, uint64_t len, wasm_sink_t out, bool verbose,
                   wasm_spec_stats_t* st);
// [51.3] Пошаговый прогон: каждый вызов из набора исполняется с паузой на
// КАЖДОМ витке цикла и перед каждой привилегированной командой (их выполняет
// wasm_service) — так официальные тесты проверяют и сохранение положения.
void wasm_spec_set_stepped(bool on);

#endif

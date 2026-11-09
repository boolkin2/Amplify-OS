// ============================================================================
// wasm_int.h — внутренности движка WebAssembly, общие для его файлов.
// Снаружи движка не подключать: внешний интерфейс — wasm.h.
// ============================================================================
#ifndef AOS_X86_64_WASM_INT_H
#define AOS_X86_64_WASM_INT_H

#include "wasm.h"

// ---- память ------------------------------------------------------------------------
// Всё, что принадлежит модулю (и хранилищу), берётся из «арены»: блоки связаны
// в список и освобождаются разом. Отдельных free нет — при отказе посреди
// разбора не нужно помнить, что уже выделено. Память обнулена.
void* wasm_blk_alloc(void** head, uint64_t n, bool* too_big);
void  wasm_blk_free_all(void** head);
// То же для модуля: при нехватке памяти записывает ошибку в модуль.
void* wasm_arena(wasm_module_t* m, uint64_t n);

// Записать причину отказа (только первую — она ближе всего к настоящей) и
// вернуть false: `return wasm_fail(...)` в функциях с результатом bool.
bool wasm_fail(wasm_module_t* m, int kind, uint32_t off, const char* text);

// ---- чтение двоичного формата ------------------------------------------------------
typedef struct {
    const uint8_t* p;
    const uint8_t* end;
    wasm_module_t* m;
} wasm_rd_t;

static inline uint32_t wasm_rd_off(const wasm_rd_t* r) { return (uint32_t)(r->p - r->m->bin); }
static inline uint32_t wasm_rd_left(const wasm_rd_t* r) { return (uint32_t)(r->end - r->p); }

bool wasm_rd_u8 (wasm_rd_t* r, uint8_t* out);
bool wasm_rd_u32(wasm_rd_t* r, uint32_t* out);      // LEB128 без знака, не длиннее 5 байт
bool wasm_rd_s32(wasm_rd_t* r, int32_t* out);
bool wasm_rd_s64(wasm_rd_t* r, int64_t* out);
bool wasm_rd_s33(wasm_rd_t* r, int64_t* out);       // тип блока: 33-битное со знаком
bool wasm_rd_f32(wasm_rd_t* r, uint32_t* bits);     // 4 байта как есть
bool wasm_rd_f64(wasm_rd_t* r, uint64_t* bits);

static inline bool wasm_is_num(uint8_t t) { return t == WT_I32 || t == WT_I64 || t == WT_F32 || t == WT_F64; }
static inline bool wasm_is_ref(uint8_t t) { return t == WT_FUNCREF || t == WT_EXTERNREF; }
static inline bool wasm_is_val(uint8_t t) { return wasm_is_num(t) || wasm_is_ref(t); }

static inline bool wasm_declared(const wasm_module_t* m, uint32_t f) {
    return m->declared && (m->declared[f >> 3] >> (f & 7)) & 1;
}

// Этапы загрузки (wasm_module_load зовёт их по очереди).
bool wasm_decode(wasm_module_t* m);                 // wasm_bin.c
bool wasm_validate(wasm_module_t* m);               // wasm_valid.c

// ============================================================================
// [51.1] Внутренний код функции.
// ----------------------------------------------------------------------------
// Исполнять двоичный код WebAssembly как есть невыгодно: числа в нём записаны
// переменной длиной (LEB128), а переходы заданы не адресом, а «выйти из
// стольких-то вложенных блоков» — на каждом br пришлось бы искать парный end.
// Поэтому проверка (wasm_valid.c), которая и так проходит по каждой команде,
// попутно пишет внутренний код: слова по 64 бита, числа раскодированы,
// переходы разрешены в номер слова внутри функции, а при переходе заранее
// известно, сколько значений перенести и сколько сбросить со стека.
// Блоки (block/loop/end) в нём исчезают: остаются только переходы.
// Стековая машина остаётся стековой — это тот же код, только готовый к
// исполнению. Из него же будет строить машинный код JIT (51.5).
//
// Слово: биты 0..15 — команда, 16..31 — малый операнд A, 32..63 — операнд B.
// Некоторым командам нужно второе слово (см. ниже).
// ============================================================================
#define IR_OP(w)   ((uint16_t)(w))
#define IR_A(w)    ((uint32_t)((w) >> 16) & 0xFFFFu)
#define IR_B(w)    ((uint32_t)((w) >> 32))
#define IR_MAKE(op, a, b)  ((uint64_t)(uint16_t)(op) | ((uint64_t)(uint16_t)(a) << 16) | ((uint64_t)(uint32_t)(b) << 32))

// Номера команд. Где это возможно — те же, что в двоичном формате (числовые
// 0x45..0xC4, чтение и запись памяти 0x28..0x3E и т.д.): меньше таблиц
// соответствия и легче читать дамп. Свои — на месте команд, которые во
// внутреннем коде не нужны (block, loop, if, else, end).
enum {
    IR_UNREACHABLE = 0x00,
    IR_LOOP        = 0x03,      // начало цикла: здесь списывается «топливо» и проверяется остановка
    IR_BR_IFZ      = 0x04,      // снять условие; если 0 — перейти на B (так исполняется if)
    IR_BR          = 0x0C,      // перейти на B; второе слово: сколько значений перенести | сколько сбросить << 32
    IR_BR_IF       = 0x0D,      // снять условие; если не 0 — как IR_BR; второе слово такое же
    IR_BR_TABLE    = 0x0E,      // B — число меток n; дальше (n + 1) пар слов:
                                //   [перенести | адрес << 32], [сбросить]
    IR_RETURN      = 0x0F,
    IR_CALL        = 0x10,      // B — номер функции
    IR_CALL_INDIRECT = 0x11,    // B — номер типа, A — номер таблицы
    IR_DROP        = 0x1A,
    IR_SELECT      = 0x1B,
    IR_LOCAL_GET   = 0x20,      // B — номер локальной (параметры — первые)
    IR_LOCAL_SET   = 0x21,
    IR_LOCAL_TEE   = 0x22,
    IR_GLOBAL_GET  = 0x23,
    IR_GLOBAL_SET  = 0x24,
    IR_TABLE_GET   = 0x25,      // B — номер таблицы
    IR_TABLE_SET   = 0x26,
    // 0x28..0x3E — чтение и запись памяти, B — смещение
    IR_MEMORY_SIZE = 0x3F,
    IR_MEMORY_GROW = 0x40,
    IR_CONST32     = 0x41,      // B — значение (i32, f32, пустая ссылка)
    IR_CONST64     = 0x42,      // значение — во втором слове (i64, f64)
    // 0x45..0xC4 — числовые команды как в двоичном формате
    IR_REF_FUNC    = 0xD2,      // B — номер функции
    // Команды с приставкой 0xFC: 0x100 + номер после приставки.
    IR_FC          = 0x100,     // 0x100..0x107 — преобразования с насыщением
    IR_MEMORY_INIT = 0x108,     // B — сегмент данных
    IR_DATA_DROP   = 0x109,
    IR_MEMORY_COPY = 0x10A,
    IR_MEMORY_FILL = 0x10B,
    IR_TABLE_INIT  = 0x10C,     // B — сегмент элементов, A — таблица
    IR_ELEM_DROP   = 0x10D,
    IR_TABLE_COPY  = 0x10E,     // B — куда, A — откуда
    IR_TABLE_GROW  = 0x10F,     // B — таблица
    IR_TABLE_SIZE  = 0x110,
    IR_TABLE_FILL  = 0x111,
    IR__COUNT
};

// ============================================================================
// [51.1] То, что живёт во время исполнения.
// ============================================================================
// [51.5] Точка входа в машинный код функции: место во внутреннем коде -> адрес.
typedef struct { const uint64_t* ip; void* native; } wasm_jit_pt_t;

struct wasm_funcinst {
    const wasm_functype_t* type;
    uint32_t canon;                     // номер типа в хранилище: равные типы — равные номера
    uint32_t n_params, n_results;
    uint32_t n_locals;                  // без параметров
    uint32_t max_stack;
    const uint64_t* code;               // внутренний код; 0 у функции хоста
    struct wasm_instance* inst;         // чей это код (0 у функции хоста)
    wasm_host_fn host;
    void*        user;
    // [51.5] Машинный код (wasm_jit.c); 0 — его нет (функция хоста, JIT выключен).
    void*                jit;           // вход: с прологом
    const wasm_jit_pt_t* jit_map;       // остальные точки входа, по возрастанию ip
    uint32_t             jit_n;
};

struct wasm_memory {
    uint8_t* data;
    uint64_t bytes;                     // сейчас доступно (pages * 64 КБ)
    uint64_t cap;                       // выделено
    uint32_t pages, max_pages;          // max_pages — уже с учётом предела хранилища
    uint32_t decl_max;                  // максимум, заявленный при создании (для сверки импорта)
    bool     has_max;
    struct wasm_memory* next;           // список хранилища — для освобождения
};

struct wasm_table {
    wasm_val_t* elems;
    uint32_t size, max, cap;            // max — уже с учётом предела хранилища
    uint32_t decl_max;                  // максимум, заявленный при создании
    bool     has_max;
    uint8_t  type;
    struct wasm_table* next;
};

struct wasm_globalinst {
    wasm_val_t value;
    uint8_t    type;
    bool       mut;
};

struct wasm_instance {
    wasm_store_t*        store;
    const wasm_module_t* module;
    wasm_funcinst_t**    funcs;         // всё пространство индексов: импорты + свои
    wasm_table_t**       tables;
    wasm_memory_t*       mem;           // единственная память или 0
    wasm_globalinst_t**  globals;
    uint32_t*            canon;         // тип модуля -> номер в хранилище
    uint8_t*             elem_dropped;  // сегмент элементов уже сброшен (elem.drop)
    uint8_t*             data_dropped;
};

typedef struct {
    const uint64_t*  ip;                // куда вернуться
    wasm_val_t*      fp;                // локальные вызвавшей функции
    wasm_funcinst_t* fn;
    void*            native;            // [51.5] то же «куда вернуться» в машинном коде; 0 — кадр положил интерпретатор
} wasm_frame_t;

// [51.5] Контекст машинного кода: то, что он держит в регистрах, пока исполняется,
// и то, о чём договаривается с водителем (wasm_jit.c). Смещения полей вшиты в
// машинный код через offsetof.
typedef struct {
    wasm_val_t*   fp;
    wasm_val_t*   sp;
    uint64_t      ticks;
    uint8_t*      mem;
    uint64_t      mem_bytes;
    wasm_frame_t* ftop;                 // первый свободный кадр
    wasm_frame_t* fbase;
    wasm_frame_t* fend;
    wasm_val_t*   stack_end;
    uint64_t      aux;                  // вызываемая функция — для выхода «вызов»
} wasm_jit_ctx_t;

typedef struct wasm_jit_block { void* code; uint64_t bytes; struct wasm_jit_block* next; } wasm_jit_block_t;

typedef struct { const uint8_t* types; uint32_t n_params, n_results, hash; } wasm_canon_t;

struct wasm_store {
    wasm_config_t   cfg;
    void*           arena;
    wasm_canon_t*   canon;              // таблица различных типов функций
    uint32_t        n_canon, cap_canon;
    wasm_memory_t*  mems;               // всё выделенное — для wasm_store_free
    wasm_table_t*   tables;
    wasm_val_t*     stack;              // стек значений интерпретатора
    wasm_frame_t*   frames;
    int64_t         fuel;               // < 0 — без предела
    volatile int*   stop;
    uint64_t        ticks;
    bool            running;            // интерпретатор уже работает: вложенный вызов запрещён
    // [51.3] Пауза и исполнение отрезками (wasm_interp.c).
    bool             paused;            // вызов прерван и ждёт wasm_resume / wasm_service
    bool             remote;            // привилегированные команды здесь исполнять нельзя
    bool             svc_once;          // исполнить одну привилегированную команду и встать
    uint32_t         tick_unit;         // раз в столько витков — часы, флаги, счёт отрезка (256)
    int64_t          slice_len;         // длина отрезка в таких единицах; 0 — без отрезков
    int64_t          slice_left;
    wasm_funcinst_t* r_top;             // функция, с которой начат вызов (для результатов)
    wasm_funcinst_t* r_fn;              // сохранённое положение: функция, кадр, стек, код
    wasm_val_t*      r_fp;
    wasm_val_t*      r_sp;
    const uint64_t*  r_ip;
    uint32_t         r_depth;
    int              r_ticks;
    wasm_funcinst_t* r_pend;            // вызов, на пороге которого встали (0 — не на вызове)
    // [51.5] JIT.
    bool             jit;               // функции этого хранилища переводятся в машинный код
    bool             jit_off;           // перевод не удался — всё хранилище на интерпретаторе
    bool             jit_run;           // текущий вызов идёт в машинном коде
    bool             step_once;         // интерпретатору: исполнить одну команду и вернуть WASM_STEPPED
    void*            jit_enter;         // вход в машинный код из C
    wasm_jit_block_t* jit_blocks;
    uint64_t         jit_bytes;
    uint32_t         jit_funcs;
    wasm_jit_ctx_t   jc;
};

// [51.5] Внутренний итог интерпретатора: «одна команда исполнена, положение
// сохранено» (режим step_once). Наружу из движка не выходит.
#define WASM_STEPPED  (-4)

// wasm_jit.c. Перевести функции экземпляра в машинный код (false — не вышло,
// хранилище остаётся на интерпретаторе); исполнять — тот же договор, что у
// wasm_interp; освободить код.
bool wasm_jit_compile(wasm_store_t* s, wasm_instance_t* inst);
int  wasm_jit_run(wasm_store_t* s, wasm_funcinst_t* f);
void wasm_jit_free(wasm_store_t* s);

// wasm_rt.c — для интерпретатора.
// Нарастить память на delta страниц. Прежний размер в страницах или -1.
int64_t  wasm_memory_grow(wasm_store_t* s, wasm_memory_t* mem, uint32_t delta);
// Нарастить таблицу, заполнив новые ячейки значением. Прежний размер или -1.
int64_t  wasm_table_grow(wasm_store_t* s, wasm_table_t* t, uint32_t delta, wasm_val_t fill);
// Номер типа в хранилище (заводит новый, если такого ещё не было). UINT32_MAX — нет памяти.
uint32_t wasm_canon_type(wasm_store_t* s, const wasm_functype_t* ft);

// wasm_interp.c. Аргументы уже лежат в s->stack[0..]; результаты — там же.
// [51.3] f == 0 — продолжить прерванный вызов с сохранённого положения.
// Результат < 0 — пауза (WASM_PAUSED_*), положение сохранено в хранилище.
int wasm_interp(wasm_store_t* s, wasm_funcinst_t* f);

#endif

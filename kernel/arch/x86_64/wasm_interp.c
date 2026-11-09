// ============================================================================
// wasm_interp.c — интерпретатор внутреннего кода WebAssembly (этап 51.1).
// ----------------------------------------------------------------------------
// Одна функция-цикл: взять слово, выполнить, взять следующее. Вызовы функций
// модуля друг другом НЕ превращаются во вложенные вызовы этого C-кода: кадр
// вызова кладётся в свой массив (s->frames), цикл продолжается с кода
// вызванной функции. Поэтому глубокая рекурсия в модуле не тратит стек ядра
// (он всего 32 КБ), а «стек исчерпан» — это наша проверка предела, а не
// падение.
//
// Стек значений — массив 64-битных ячеек без типов. Кадр функции на нём:
//     fp -> [ параметры | локальные | операнды ... ] <- sp
// Аргументы вызова уже лежат на вершине стека вызывающего, поэтому вызов их не
// копирует: они становятся первыми локальными вызванной. Результаты при
// возврате сдвигаются на место аргументов.
//
// Чего здесь НЕТ, потому что это доказала проверка (wasm_valid.c): проверок
// типов, проверок «хватит ли значений на стеке», проверок номеров локальных,
// функций, таблиц. Что ЕСТЬ на каждом шаге, потому что зависит от данных:
// границы памяти и таблиц, деление на ноль, переполнение при преобразовании,
// глубина вызовов и стека.
//
// Арифметика с плавающей точкой — обычные операции C: ядро собрано с SSE2,
// это ровно IEEE 754 одинарной и двойной точности, как требует спецификация.
// Отдельно написано только то, где у процессора другая семантика, чем у
// WebAssembly: min/max (знак нуля и NaN), округления (нет в SSE2) и
// преобразования в целое (обязаны останавливать, а не давать «мусор»).
//
// [51.3] Пауза и продолжение. Программа исполняется на вычислительном ядре
// (AP), а ему нельзя ни диска, ни экрана, ни выделения памяти. Поэтому цикл
// умеет ОСТАНОВИТЬСЯ, сохранив своё положение в хранилище, и позже продолжить
// с того же места — возможно, на другом ядре:
//   * кончился отпущенный отрезок (счётчик витков) — пауза «уступаю ядро»;
//   * следующая команда «привилегированная» (вызов функции хоста, рост памяти
//     или таблицы), а хранилище помечено remote — пауза «нужна услуга».
//     Услугу оказывает wasm_service на главном ядре: тот же цикл исполняет
//     РОВНО ЭТУ команду и снова встаёт на паузу.
// Положение — пять значений (функция, кадр, вершина стека, место в коде,
// глубина); сами стеки и так лежат в хранилище. Проверки паузы стоят только
// там, где уже был счётчик витков, и в трёх привилегированных командах — на
// скорость обычных команд они не влияют.
// ============================================================================
#include "wasm_int.h"

// Чтение и запись памяти модуля по невыровненным адресам.
typedef uint16_t __attribute__((may_alias, aligned(1))) u16u;
typedef uint32_t __attribute__((may_alias, aligned(1))) u32u;
typedef uint64_t __attribute__((may_alias, aligned(1))) u64u;

static inline float    u2f(uint32_t u) { union { uint32_t u; float f; } x; x.u = u; return x.f; }
static inline uint32_t f2u(float f)    { union { uint32_t u; float f; } x; x.f = f; return x.u; }
static inline double   u2d(uint64_t u) { union { uint64_t u; double d; } x; x.u = u; return x.d; }
static inline uint64_t d2u(double d)   { union { uint64_t u; double d; } x; x.d = d; return x.u; }

// ---- целые -------------------------------------------------------------------------
// У процессора «число ведущих нулей» от нуля не определено, у WebAssembly — 32.
static inline uint32_t clz32(uint32_t v) { return v ? (uint32_t)__builtin_clz(v) : 32; }
static inline uint32_t ctz32(uint32_t v) { return v ? (uint32_t)__builtin_ctz(v) : 32; }
static inline uint64_t clz64(uint64_t v) { return v ? (uint64_t)__builtin_clzll(v) : 64; }
static inline uint64_t ctz64(uint64_t v) { return v ? (uint64_t)__builtin_ctzll(v) : 64; }
// Подсчёт единиц — своей функцией: встроенная без -mpopcnt зовёт libgcc (§5.10).
static inline uint64_t popcnt64(uint64_t v) {
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0F0F0F0F0F0F0F0Full;
    return (v * 0x0101010101010101ull) >> 56;
}
static inline uint32_t rotl32(uint32_t v, uint32_t n) { n &= 31; return (v << n) | (v >> ((32 - n) & 31)); }
static inline uint32_t rotr32(uint32_t v, uint32_t n) { n &= 31; return (v >> n) | (v << ((32 - n) & 31)); }
static inline uint64_t rotl64(uint64_t v, uint64_t n) { n &= 63; return (v << n) | (v >> ((64 - n) & 63)); }
static inline uint64_t rotr64(uint64_t v, uint64_t n) { n &= 63; return (v >> n) | (v << ((64 - n) & 63)); }

// ---- плавающая точка ------------------------------------------------------------------
// min/max: если хоть один операнд NaN — результат NaN (сложение даёт «тихий»);
// нули разного знака считаются равными, но min обязан вернуть -0, а max — +0.
static inline float f32_min(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2f(f2u(a) | f2u(b));
    return a < b ? a : b;
}
static inline float f32_max(float a, float b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2f(f2u(a) & f2u(b));
    return a > b ? a : b;
}
static inline double f64_min(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2d(d2u(a) | d2u(b));
    return a < b ? a : b;
}
static inline double f64_max(double a, double b) {
    if (a != a || b != b) return a + b;
    if (a == b) return u2d(d2u(a) & d2u(b));
    return a > b ? a : b;
}

static inline double f64_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "xm"(x)); return r; }
static inline float  f32_sqrt(float x)  { float r;  __asm__("sqrtss %1, %0" : "=x"(r) : "xm"(x)); return r; }

// Отбросить дробную часть — обнулением младших битов мантиссы: точно, без
// обращения к режиму округления процессора и без переполнения при переходе
// через целое. Числа от 2^52 дробной части не имеют; NaN «утихомиривается».
static double f64_trunc(double x) {
    uint64_t b = d2u(x);
    int e = (int)((b >> 52) & 0x7FF) - 1023;
    if (e >= 52) return e == 1024 ? x + x : x;
    if (e < 0) return u2d(b & 0x8000000000000000ull);           // |x| < 1: ноль со знаком x
    return u2d(b & ~((1ull << (52 - e)) - 1));
}
static double f64_floor(double x) { double t = f64_trunc(x); return t > x ? t - 1.0 : t; }
static double f64_ceil(double x) {
    double t = f64_trunc(x);
    if (t < x) return t + 1.0;
    return t;
}
// К ближайшему целому, половина — к чётному.
static double f64_nearest(double x) {
    uint64_t b = d2u(x);
    int e = (int)((b >> 52) & 0x7FF) - 1023;
    if (e >= 52) return e == 1024 ? x + x : x;
    double t = f64_trunc(x), diff = x - t;
    double ad = diff < 0 ? -diff : diff;
    if (ad > 0.5 || (ad == 0.5 && ((int64_t)t & 1))) t += x < 0 ? -1.0 : 1.0;
    return t == 0 ? u2d(b & 0x8000000000000000ull) : t;         // ноль сохраняет знак исходного числа
}
// Для f32 — через f64: одинарное число в двойном представимо точно, а целый
// результат меньше 2^24 по модулю возвращается обратно без потерь.
static inline float f32_trunc(float x)   { return (float)f64_trunc((double)x); }
static inline float f32_floor(float x)   { return (float)f64_floor((double)x); }
static inline float f32_ceil(float x)    { return (float)f64_ceil((double)x); }
static inline float f32_nearest(float x) { return (float)f64_nearest((double)x); }

static void mem_move(uint8_t* d, const uint8_t* s, uint64_t n) {
    if (d == s || !n) return;
    if (d < s || d >= s + n) {
        __asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    } else {
        for (uint64_t i = n; i > 0; i--) d[i - 1] = s[i - 1];   // области внахлёст, приёмник выше
    }
}
static void mem_fill(uint8_t* d, uint8_t v, uint64_t n) {
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(n) : "a"(v) : "memory");
}

static wasm_val_t elem_value(const wasm_instance_t* inst, uint32_t item) {
    if (item == WASM_ELEM_NULL) return 0;
    if (item & WASM_ELEM_GLOBAL) return inst->globals[item & ~WASM_ELEM_GLOBAL]->value;
    return (wasm_val_t)(uintptr_t)inst->funcs[item];
}

// Рост памяти, которому не нужно новое выделение: ноль страниц, отказ по
// пределу или место в уже выделенном куске. Такой рост можно исполнить на
// любом ядре; остальное — только там, где разрешено выделять память.
static inline bool grow_is_local(const wasm_memory_t* m, uint32_t delta) {
    uint64_t pages = (uint64_t)m->pages + delta;
    return delta == 0 || pages > m->max_pages || pages * WASM_PAGE <= m->cap;
}
static inline bool tgrow_is_local(const wasm_table_t* t, uint32_t delta) {
    uint64_t size = (uint64_t)t->size + delta;
    return delta == 0 || size > t->max || size <= t->cap;
}

int wasm_interp(wasm_store_t* s, wasm_funcinst_t* fn) {
    wasm_val_t* const stack_end = s->stack + s->cfg.stack_slots;
    wasm_frame_t* const frames = s->frames;
    const uint32_t max_frames = s->cfg.max_frames;
    const int unit = (int)s->tick_unit;     // раз в столько витков смотрим на часы и флаги
    uint32_t depth = 0;
    int trap = WASM_TRAP_NONE;
    int ticks = unit;
    wasm_funcinst_t* pend = 0;              // вызов, на пороге которого встали на паузу
    // [51.5] Одна команда для JIT: то, что машинный код не умеет сам, он отдаёт
    // сюда поштучно. Положение у нас общее, поэтому хватает «исполни и встань».
    const bool step = s->step_once;

    wasm_val_t* fp = s->stack;
    wasm_val_t* sp;
    const uint64_t* code;
    const uint64_t* ip;
    wasm_instance_t* inst;
    uint8_t* mem = 0;
    uint64_t mem_bytes = 0;
    wasm_funcinst_t* callee;

#define TRAP(code_)   do { trap = (code_); goto out; } while (0)
// Память экземпляра могла вырасти и переехать — после вызова, возврата и
// memory.grow указатель берём заново.
#define RELOAD_MEM()  do { if (inst->mem) { mem = inst->mem->data; mem_bytes = inst->mem->bytes; } else { mem = 0; mem_bytes = 0; } } while (0)
// Вход в функцию fn с кадром fp: хватит ли стека на локальные и операнды,
// обнулить локальные, встать на первое слово кода.
#define ENTER() do { \
        if ((uint64_t)(stack_end - fp) < (uint64_t)fn->n_params + fn->n_locals + fn->max_stack) TRAP(WASM_TRAP_STACK); \
        sp = fp + fn->n_params; \
        for (uint32_t i_ = 0; i_ < fn->n_locals; i_++) *sp++ = 0; \
        code = fn->code; ip = code; inst = fn->inst; RELOAD_MEM(); \
    } while (0)
#define TICK() do { \
        if (--ticks <= 0) { \
            ticks = unit; s->ticks++; \
            if (s->stop && *s->stop) TRAP(WASM_TRAP_STOPPED); \
            if (s->fuel >= 0 && --s->fuel < 0) { s->fuel = 0; TRAP(WASM_TRAP_FUEL); } \
            if (s->slice_left > 0 && --s->slice_left == 0) goto pause_yield; \
        } \
    } while (0)
// Переход с переносом keep верхних значений вниз на drop ячеек.
#define DO_BR(target_, keep_, drop_) do { \
        uint32_t k_ = (keep_), d_ = (drop_); \
        if (d_) { wasm_val_t* to_ = sp - k_ - d_; for (uint32_t i_ = 0; i_ < k_; i_++) to_[i_] = sp[(int64_t)i_ - k_]; sp -= d_; } \
        ip = code + (target_); \
    } while (0)

#define A32   ((uint32_t)sp[-2])
#define B32   ((uint32_t)sp[-1])
#define A64   (sp[-2])
#define B64   (sp[-1])
#define AF32  u2f((uint32_t)sp[-2])
#define BF32  u2f((uint32_t)sp[-1])
#define AF64  u2d(sp[-2])
#define BF64  u2d(sp[-1])
#define BIN(expr)      do { wasm_val_t r_ = (wasm_val_t)(expr); sp--; sp[-1] = r_; } while (0)
#define UN32  ((uint32_t)sp[-1])
#define UN64  (sp[-1])
#define UNF32 u2f((uint32_t)sp[-1])
#define UNF64 u2d(sp[-1])
#define EA(n) uint64_t ea = (uint64_t)(uint32_t)sp[-1] + IR_B(w); if (ea + (n) > mem_bytes) TRAP(WASM_TRAP_MEM_OOB)
#define EA_ST(n) uint64_t ea = (uint64_t)(uint32_t)sp[-2] + IR_B(w); if (ea + (n) > mem_bytes) TRAP(WASM_TRAP_MEM_OOB)

    if (fn) {
        ENTER();
    } else {
        // Продолжение после паузы: положение — из хранилища.
        fn = s->r_fn; fp = s->r_fp; sp = s->r_sp; ip = s->r_ip;
        depth = s->r_depth; ticks = s->r_ticks;
        code = fn->code; inst = fn->inst;
        RELOAD_MEM();
        callee = s->r_pend;
        s->r_pend = 0;
        if (callee) goto call_go;           // встали на пороге вызова — с него и начинаем
    }
    for (;;) {
        uint64_t w = *ip++;
        switch (IR_OP(w)) {
        // ---- управление ----
        case IR_UNREACHABLE: TRAP(WASM_TRAP_UNREACHABLE);
        case IR_LOOP: TICK(); break;
        case IR_BR_IFZ: if (!(uint32_t)*--sp) ip = code + IR_B(w); break;
        case IR_BR: { uint64_t kd = *ip; DO_BR(IR_B(w), (uint32_t)kd, (uint32_t)(kd >> 32)); break; }
        case IR_BR_IF: {
            uint64_t kd = *ip++;
            if ((uint32_t)*--sp) DO_BR(IR_B(w), (uint32_t)kd, (uint32_t)(kd >> 32));
            break;
        }
        case IR_BR_TABLE: {
            uint32_t n = IR_B(w), i = (uint32_t)*--sp;
            const uint64_t* e = ip + (uint64_t)(i < n ? i : n) * 2;     // за пределами — метка по умолчанию
            DO_BR(IR_B(e[0]), (uint32_t)e[0], (uint32_t)e[1]);
            break;
        }
        case IR_RETURN: {
            uint32_t n = fn->n_results;
            wasm_val_t* from = sp - n;
            for (uint32_t i = 0; i < n; i++) fp[i] = from[i];
            sp = fp + n;
            if (!depth) goto out;                       // вернулась функция, с которой начинали
            wasm_frame_t* fr = &frames[--depth];
            ip = fr->ip; fp = fr->fp; fn = fr->fn;
            code = fn->code; inst = fn->inst;
            RELOAD_MEM();
            break;
        }
        case IR_CALL:
            callee = inst->funcs[IR_B(w)];
        do_call:
            pend = callee;                  // если отрезок кончится здесь — продолжим с этого вызова
            TICK();
            pend = 0;
        call_go:
            if (!callee->code) {
                // Функция хоста: аргументы — на вершине стека, результаты она
                // кладёт туда же.
                wasm_val_t* a = sp - callee->n_params;
                if ((uint64_t)(stack_end - a) < callee->n_results) TRAP(WASM_TRAP_STACK);
                // [51.3] На этом ядре функции хоста исполнять нельзя — просим услугу.
                if (s->remote && !s->svc_once) { pend = callee; goto pause_service; }
                int t = callee->host(inst, a, callee->user);
                if (t == WASM_HOST_RETRY) {
                    // «Пока нечем ответить» (ввода нет, срок сна не вышел): вызов
                    // остаётся несделанным, его повторят позже. Без того, кто
                    // повторит, это ошибка окружения.
                    if (!s->svc_once) TRAP(WASM_TRAP_HOST);
                    pend = callee;
                    goto pause_service;
                }
                if (t) TRAP(t);
                sp = a + callee->n_results;
                RELOAD_MEM();
                if (s->svc_once) goto pause_yield;      // услуга оказана — возвращаем ядро
                break;
            }
            if (depth >= max_frames) TRAP(WASM_TRAP_STACK);
            frames[depth].ip = ip; frames[depth].fp = fp; frames[depth].fn = fn;
            frames[depth].native = 0;       // [51.5] адреса в машинном коде у этого кадра нет
            depth++;
            fp = sp - callee->n_params;
            fn = callee;
            ENTER();
            break;
        case IR_CALL_INDIRECT: {
            const wasm_table_t* t = inst->tables[IR_A(w)];
            uint32_t i = (uint32_t)*--sp;
            if (i >= t->size) TRAP(WASM_TRAP_UNDEF_ELEM);
            callee = (wasm_funcinst_t*)(uintptr_t)t->elems[i];
            if (!callee) TRAP(WASM_TRAP_NULL_ELEM);
            // Тип сверяется на ходу: что лежит в ячейке таблицы, заранее неизвестно.
            if (callee->canon != inst->canon[IR_B(w)]) TRAP(WASM_TRAP_SIG_MISMATCH);
            goto do_call;
        }

        // ---- стек, локальные, глобальные ----
        case IR_DROP: sp--; break;
        case IR_SELECT: { uint32_t c = (uint32_t)sp[-1]; sp -= 2; if (!c) sp[-1] = sp[0]; break; }
        case IR_LOCAL_GET: *sp++ = fp[IR_B(w)]; break;
        case IR_LOCAL_SET: fp[IR_B(w)] = *--sp; break;
        case IR_LOCAL_TEE: fp[IR_B(w)] = sp[-1]; break;
        case IR_GLOBAL_GET: *sp++ = inst->globals[IR_B(w)]->value; break;
        case IR_GLOBAL_SET: inst->globals[IR_B(w)]->value = *--sp; break;
        case IR_CONST32: *sp++ = IR_B(w); break;
        case IR_CONST64: *sp++ = *ip++; break;
        case IR_REF_FUNC: *sp++ = (wasm_val_t)(uintptr_t)inst->funcs[IR_B(w)]; break;

        // ---- таблицы ----
        case IR_TABLE_GET: {
            const wasm_table_t* t = inst->tables[IR_B(w)];
            uint32_t i = (uint32_t)sp[-1];
            if (i >= t->size) TRAP(WASM_TRAP_TABLE_OOB);
            sp[-1] = t->elems[i];
            break;
        }
        case IR_TABLE_SET: {
            wasm_table_t* t = inst->tables[IR_B(w)];
            uint32_t i = (uint32_t)sp[-2];
            if (i >= t->size) TRAP(WASM_TRAP_TABLE_OOB);
            t->elems[i] = sp[-1];
            sp -= 2;
            break;
        }
        case IR_TABLE_SIZE: *sp++ = inst->tables[IR_B(w)]->size; break;
        case IR_TABLE_GROW: {
            // [51.3] Рост с выделением памяти — только там, где это разрешено.
            bool far = s->remote && !tgrow_is_local(inst->tables[IR_B(w)], (uint32_t)sp[-1]);
            if (far && !s->svc_once) { ip--; goto pause_service; }
            int64_t r = wasm_table_grow(s, inst->tables[IR_B(w)], (uint32_t)sp[-1], sp[-2]);
            sp--;
            sp[-1] = (uint32_t)r;
            if (s->svc_once) goto pause_yield;
            break;
        }
        case IR_TABLE_FILL: {
            wasm_table_t* t = inst->tables[IR_B(w)];
            uint64_t i = (uint32_t)sp[-3], n = (uint32_t)sp[-1];
            if (i + n > t->size) TRAP(WASM_TRAP_TABLE_OOB);
            for (uint64_t k = 0; k < n; k++) t->elems[i + k] = sp[-2];
            sp -= 3;
            break;
        }
        case IR_TABLE_COPY: {
            wasm_table_t* dt = inst->tables[IR_B(w)];
            const wasm_table_t* st = inst->tables[IR_A(w)];
            uint64_t d = (uint32_t)sp[-3], sr = (uint32_t)sp[-2], n = (uint32_t)sp[-1];
            if (sr + n > st->size || d + n > dt->size) TRAP(WASM_TRAP_TABLE_OOB);
            if (dt == st && d > sr) { for (uint64_t k = n; k > 0; k--) dt->elems[d + k - 1] = st->elems[sr + k - 1]; }
            else for (uint64_t k = 0; k < n; k++) dt->elems[d + k] = st->elems[sr + k];
            sp -= 3;
            break;
        }
        case IR_TABLE_INIT: {
            wasm_table_t* t = inst->tables[IR_A(w)];
            const wasm_elem_t* e = &inst->module->elems[IR_B(w)];
            uint64_t d = (uint32_t)sp[-3], sr = (uint32_t)sp[-2], n = (uint32_t)sp[-1];
            uint64_t have = inst->elem_dropped[IR_B(w)] ? 0 : e->n;     // сброшенный сегмент пуст
            if (sr + n > have || d + n > t->size) TRAP(WASM_TRAP_TABLE_OOB);
            for (uint64_t k = 0; k < n; k++) t->elems[d + k] = elem_value(inst, e->items[sr + k]);
            sp -= 3;
            break;
        }
        case IR_ELEM_DROP: inst->elem_dropped[IR_B(w)] = 1; break;

        // ---- память ----
        case 0x28: case 0x2A: { EA(4); sp[-1] = *(const u32u*)(mem + ea); break; }              // i32.load, f32.load
        case 0x29: case 0x2B: { EA(8); sp[-1] = *(const u64u*)(mem + ea); break; }              // i64.load, f64.load
        case 0x2C: { EA(1); sp[-1] = (uint32_t)(int32_t)(int8_t)mem[ea]; break; }
        case 0x2D: { EA(1); sp[-1] = mem[ea]; break; }
        case 0x2E: { EA(2); sp[-1] = (uint32_t)(int32_t)(int16_t)*(const u16u*)(mem + ea); break; }
        case 0x2F: { EA(2); sp[-1] = *(const u16u*)(mem + ea); break; }
        case 0x30: { EA(1); sp[-1] = (uint64_t)(int64_t)(int8_t)mem[ea]; break; }
        case 0x31: { EA(1); sp[-1] = mem[ea]; break; }
        case 0x32: { EA(2); sp[-1] = (uint64_t)(int64_t)(int16_t)*(const u16u*)(mem + ea); break; }
        case 0x33: { EA(2); sp[-1] = *(const u16u*)(mem + ea); break; }
        case 0x34: { EA(4); sp[-1] = (uint64_t)(int64_t)(int32_t)*(const u32u*)(mem + ea); break; }
        case 0x35: { EA(4); sp[-1] = *(const u32u*)(mem + ea); break; }
        case 0x36: case 0x38: case 0x3E: { EA_ST(4); *(u32u*)(mem + ea) = (uint32_t)sp[-1]; sp -= 2; break; }  // i32/f32.store, i64.store32
        case 0x37: case 0x39: { EA_ST(8); *(u64u*)(mem + ea) = sp[-1]; sp -= 2; break; }                      // i64/f64.store
        case 0x3A: case 0x3C: { EA_ST(1); mem[ea] = (uint8_t)sp[-1]; sp -= 2; break; }                        // store8
        case 0x3B: case 0x3D: { EA_ST(2); *(u16u*)(mem + ea) = (uint16_t)sp[-1]; sp -= 2; break; }            // store16
        case IR_MEMORY_SIZE: *sp++ = inst->mem->pages; break;
        case IR_MEMORY_GROW: {
            bool far = s->remote && !grow_is_local(inst->mem, (uint32_t)sp[-1]);
            if (far && !s->svc_once) { ip--; goto pause_service; }
            sp[-1] = (uint32_t)wasm_memory_grow(s, inst->mem, (uint32_t)sp[-1]);
            RELOAD_MEM();
            if (s->svc_once) goto pause_yield;
            break;
        }
        case IR_MEMORY_COPY: {
            uint64_t d = (uint32_t)sp[-3], sr = (uint32_t)sp[-2], n = (uint32_t)sp[-1];
            if (sr + n > mem_bytes || d + n > mem_bytes) TRAP(WASM_TRAP_MEM_OOB);
            mem_move(mem + d, mem + sr, n);
            sp -= 3;
            break;
        }
        case IR_MEMORY_FILL: {
            uint64_t d = (uint32_t)sp[-3], n = (uint32_t)sp[-1];
            if (d + n > mem_bytes) TRAP(WASM_TRAP_MEM_OOB);
            if (n) mem_fill(mem + d, (uint8_t)sp[-2], n);
            sp -= 3;
            break;
        }
        case IR_MEMORY_INIT: {
            const wasm_data_t* dseg = &inst->module->datas[IR_B(w)];
            uint64_t d = (uint32_t)sp[-3], sr = (uint32_t)sp[-2], n = (uint32_t)sp[-1];
            uint64_t have = inst->data_dropped[IR_B(w)] ? 0 : dseg->n;
            if (sr + n > have || d + n > mem_bytes) TRAP(WASM_TRAP_MEM_OOB);
            mem_move(mem + d, dseg->bytes + sr, n);
            sp -= 3;
            break;
        }
        case IR_DATA_DROP: inst->data_dropped[IR_B(w)] = 1; break;

        // ---- сравнения ----
        case 0x45: sp[-1] = UN32 == 0; break;
        case 0x46: BIN(A32 == B32); break;
        case 0x47: BIN(A32 != B32); break;
        case 0x48: BIN((int32_t)A32 <  (int32_t)B32); break;
        case 0x49: BIN(A32 <  B32); break;
        case 0x4A: BIN((int32_t)A32 >  (int32_t)B32); break;
        case 0x4B: BIN(A32 >  B32); break;
        case 0x4C: BIN((int32_t)A32 <= (int32_t)B32); break;
        case 0x4D: BIN(A32 <= B32); break;
        case 0x4E: BIN((int32_t)A32 >= (int32_t)B32); break;
        case 0x4F: BIN(A32 >= B32); break;
        case 0x50: sp[-1] = UN64 == 0; break;
        case 0x51: BIN(A64 == B64); break;
        case 0x52: BIN(A64 != B64); break;
        case 0x53: BIN((int64_t)A64 <  (int64_t)B64); break;
        case 0x54: BIN(A64 <  B64); break;
        case 0x55: BIN((int64_t)A64 >  (int64_t)B64); break;
        case 0x56: BIN(A64 >  B64); break;
        case 0x57: BIN((int64_t)A64 <= (int64_t)B64); break;
        case 0x58: BIN(A64 <= B64); break;
        case 0x59: BIN((int64_t)A64 >= (int64_t)B64); break;
        case 0x5A: BIN(A64 >= B64); break;
        case 0x5B: BIN(AF32 == BF32); break;
        case 0x5C: BIN(AF32 != BF32); break;
        case 0x5D: BIN(AF32 <  BF32); break;
        case 0x5E: BIN(AF32 >  BF32); break;
        case 0x5F: BIN(AF32 <= BF32); break;
        case 0x60: BIN(AF32 >= BF32); break;
        case 0x61: BIN(AF64 == BF64); break;
        case 0x62: BIN(AF64 != BF64); break;
        case 0x63: BIN(AF64 <  BF64); break;
        case 0x64: BIN(AF64 >  BF64); break;
        case 0x65: BIN(AF64 <= BF64); break;
        case 0x66: BIN(AF64 >= BF64); break;

        // ---- i32 ----
        case 0x67: sp[-1] = clz32(UN32); break;
        case 0x68: sp[-1] = ctz32(UN32); break;
        case 0x69: sp[-1] = popcnt64(UN32); break;
        case 0x6A: BIN((uint32_t)(A32 + B32)); break;
        case 0x6B: BIN((uint32_t)(A32 - B32)); break;
        case 0x6C: BIN((uint32_t)(A32 * B32)); break;
        case 0x6D: {                                    // div_s: два случая остановки
            int32_t a = (int32_t)A32, b = (int32_t)B32;
            if (b == 0) TRAP(WASM_TRAP_DIV_ZERO);
            if (a == INT32_MIN && b == -1) TRAP(WASM_TRAP_INT_OVERFLOW);
            BIN((uint32_t)(a / b));
            break;
        }
        case 0x6E: if (B32 == 0) TRAP(WASM_TRAP_DIV_ZERO); BIN(A32 / B32); break;
        case 0x6F: {                                    // rem_s: INT_MIN % -1 = 0 (а у процессора — исключение)
            int32_t a = (int32_t)A32, b = (int32_t)B32;
            if (b == 0) TRAP(WASM_TRAP_DIV_ZERO);
            BIN(b == -1 ? 0u : (uint32_t)(a % b));
            break;
        }
        case 0x70: if (B32 == 0) TRAP(WASM_TRAP_DIV_ZERO); BIN(A32 % B32); break;
        case 0x71: BIN(A32 & B32); break;
        case 0x72: BIN(A32 | B32); break;
        case 0x73: BIN(A32 ^ B32); break;
        case 0x74: BIN((uint32_t)(A32 << (B32 & 31))); break;
        case 0x75: BIN((uint32_t)((int32_t)A32 >> (B32 & 31))); break;
        case 0x76: BIN(A32 >> (B32 & 31)); break;
        case 0x77: BIN(rotl32(A32, B32)); break;
        case 0x78: BIN(rotr32(A32, B32)); break;

        // ---- i64 ----
        case 0x79: sp[-1] = clz64(UN64); break;
        case 0x7A: sp[-1] = ctz64(UN64); break;
        case 0x7B: sp[-1] = popcnt64(UN64); break;
        case 0x7C: BIN(A64 + B64); break;
        case 0x7D: BIN(A64 - B64); break;
        case 0x7E: BIN(A64 * B64); break;
        case 0x7F: {
            int64_t a = (int64_t)A64, b = (int64_t)B64;
            if (b == 0) TRAP(WASM_TRAP_DIV_ZERO);
            if (a == INT64_MIN && b == -1) TRAP(WASM_TRAP_INT_OVERFLOW);
            BIN((uint64_t)(a / b));
            break;
        }
        case 0x80: if (B64 == 0) TRAP(WASM_TRAP_DIV_ZERO); BIN(A64 / B64); break;
        case 0x81: {
            int64_t a = (int64_t)A64, b = (int64_t)B64;
            if (b == 0) TRAP(WASM_TRAP_DIV_ZERO);
            BIN(b == -1 ? 0ull : (uint64_t)(a % b));
            break;
        }
        case 0x82: if (B64 == 0) TRAP(WASM_TRAP_DIV_ZERO); BIN(A64 % B64); break;
        case 0x83: BIN(A64 & B64); break;
        case 0x84: BIN(A64 | B64); break;
        case 0x85: BIN(A64 ^ B64); break;
        case 0x86: BIN(A64 << (B64 & 63)); break;
        case 0x87: BIN((uint64_t)((int64_t)A64 >> (B64 & 63))); break;
        case 0x88: BIN(A64 >> (B64 & 63)); break;
        case 0x89: BIN(rotl64(A64, B64)); break;
        case 0x8A: BIN(rotr64(A64, B64)); break;

        // ---- f32 ---- (abs, neg, copysign — над битами: они не должны трогать NaN)
        case 0x8B: sp[-1] = UN32 & 0x7FFFFFFFu; break;
        case 0x8C: sp[-1] = UN32 ^ 0x80000000u; break;
        case 0x8D: sp[-1] = f2u(f32_ceil(UNF32)); break;
        case 0x8E: sp[-1] = f2u(f32_floor(UNF32)); break;
        case 0x8F: sp[-1] = f2u(f32_trunc(UNF32)); break;
        case 0x90: sp[-1] = f2u(f32_nearest(UNF32)); break;
        case 0x91: sp[-1] = f2u(f32_sqrt(UNF32)); break;
        case 0x92: BIN(f2u(AF32 + BF32)); break;
        case 0x93: BIN(f2u(AF32 - BF32)); break;
        case 0x94: BIN(f2u(AF32 * BF32)); break;
        case 0x95: BIN(f2u(AF32 / BF32)); break;
        case 0x96: BIN(f2u(f32_min(AF32, BF32))); break;
        case 0x97: BIN(f2u(f32_max(AF32, BF32))); break;
        case 0x98: BIN((A32 & 0x7FFFFFFFu) | (B32 & 0x80000000u)); break;

        // ---- f64 ----
        case 0x99: sp[-1] = UN64 & 0x7FFFFFFFFFFFFFFFull; break;
        case 0x9A: sp[-1] = UN64 ^ 0x8000000000000000ull; break;
        case 0x9B: sp[-1] = d2u(f64_ceil(UNF64)); break;
        case 0x9C: sp[-1] = d2u(f64_floor(UNF64)); break;
        case 0x9D: sp[-1] = d2u(f64_trunc(UNF64)); break;
        case 0x9E: sp[-1] = d2u(f64_nearest(UNF64)); break;
        case 0x9F: sp[-1] = d2u(f64_sqrt(UNF64)); break;
        case 0xA0: BIN(d2u(AF64 + BF64)); break;
        case 0xA1: BIN(d2u(AF64 - BF64)); break;
        case 0xA2: BIN(d2u(AF64 * BF64)); break;
        case 0xA3: BIN(d2u(AF64 / BF64)); break;
        case 0xA4: BIN(d2u(f64_min(AF64, BF64))); break;
        case 0xA5: BIN(d2u(f64_max(AF64, BF64))); break;
        case 0xA6: BIN((A64 & 0x7FFFFFFFFFFFFFFFull) | (B64 & 0x8000000000000000ull)); break;

        // ---- преобразования ----
        // В целое: NaN и число вне диапазона останавливают программу. Границы
        // записаны так, чтобы само сравнение было точным в данном формате.
        case 0xA7: sp[-1] = (uint32_t)UN64; break;                                              // i32.wrap_i64
        case 0xA8: { float x = UNF32; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x >= -2147483648.0f && x < 2147483648.0f)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint32_t)(int32_t)x; break; }
        case 0xA9: { float x = UNF32; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x > -1.0f && x < 4294967296.0f)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint32_t)x; break; }
        case 0xAA: { double x = UNF64; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x > -2147483649.0 && x < 2147483648.0)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint32_t)(int32_t)x; break; }
        case 0xAB: { double x = UNF64; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x > -1.0 && x < 4294967296.0)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint32_t)x; break; }
        case 0xAC: sp[-1] = (uint64_t)(int64_t)(int32_t)UN32; break;                            // i64.extend_i32_s
        case 0xAD: sp[-1] = UN32; break;
        case 0xAE: { float x = UNF32; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x >= -9223372036854775808.0f && x < 9223372036854775808.0f)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint64_t)(int64_t)x; break; }
        case 0xAF: { float x = UNF32; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x > -1.0f && x < 18446744073709551616.0f)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint64_t)x; break; }
        case 0xB0: { double x = UNF64; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint64_t)(int64_t)x; break; }
        case 0xB1: { double x = UNF64; if (x != x) TRAP(WASM_TRAP_BAD_CONVERSION);
                     if (!(x > -1.0 && x < 18446744073709551616.0)) TRAP(WASM_TRAP_INT_OVERFLOW);
                     sp[-1] = (uint64_t)x; break; }
        case 0xB2: sp[-1] = f2u((float)(int32_t)UN32); break;
        case 0xB3: sp[-1] = f2u((float)UN32); break;
        case 0xB4: sp[-1] = f2u((float)(int64_t)UN64); break;
        case 0xB5: sp[-1] = f2u((float)UN64); break;
        case 0xB6: sp[-1] = f2u((float)UNF64); break;                                           // f32.demote_f64
        case 0xB7: sp[-1] = d2u((double)(int32_t)UN32); break;
        case 0xB8: sp[-1] = d2u((double)UN32); break;
        case 0xB9: sp[-1] = d2u((double)(int64_t)UN64); break;
        case 0xBA: sp[-1] = d2u((double)UN64); break;
        case 0xBB: sp[-1] = d2u((double)UNF32); break;                                          // f64.promote_f32
        case 0xBC: sp[-1] = UN32; break;                                                        // reinterpret: те же биты
        case 0xBD: case 0xBF: break;
        case 0xBE: sp[-1] = UN32; break;
        case 0xC0: sp[-1] = (uint32_t)(int32_t)(int8_t)UN32; break;                             // расширение знака
        case 0xC1: sp[-1] = (uint32_t)(int32_t)(int16_t)UN32; break;
        case 0xC2: sp[-1] = (uint64_t)(int64_t)(int8_t)UN64; break;
        case 0xC3: sp[-1] = (uint64_t)(int64_t)(int16_t)UN64; break;
        case 0xC4: sp[-1] = (uint64_t)(int64_t)(int32_t)UN64; break;

        // В целое с насыщением: NaN -> 0, за границей -> граница.
        case IR_FC + 0: { float x = UNF32;  sp[-1] = (uint32_t)(x != x ? 0 : x <= -2147483648.0f ? INT32_MIN : x >= 2147483648.0f ? INT32_MAX : (int32_t)x); break; }
        case IR_FC + 1: { float x = UNF32;  sp[-1] = x != x ? 0u : x <= 0.0f ? 0u : x >= 4294967296.0f ? UINT32_MAX : (uint32_t)x; break; }
        case IR_FC + 2: { double x = UNF64; sp[-1] = (uint32_t)(x != x ? 0 : x <= -2147483648.0 ? INT32_MIN : x >= 2147483647.0 ? INT32_MAX : (int32_t)x); break; }
        case IR_FC + 3: { double x = UNF64; sp[-1] = x != x ? 0u : x <= 0.0 ? 0u : x >= 4294967295.0 ? UINT32_MAX : (uint32_t)x; break; }
        case IR_FC + 4: { float x = UNF32;  sp[-1] = (uint64_t)(x != x ? 0 : x <= -9223372036854775808.0f ? INT64_MIN : x >= 9223372036854775808.0f ? INT64_MAX : (int64_t)x); break; }
        case IR_FC + 5: { float x = UNF32;  sp[-1] = x != x ? 0ull : x <= 0.0f ? 0ull : x >= 18446744073709551616.0f ? UINT64_MAX : (uint64_t)x; break; }
        case IR_FC + 6: { double x = UNF64; sp[-1] = (uint64_t)(x != x ? 0 : x <= -9223372036854775808.0 ? INT64_MIN : x >= 9223372036854775808.0 ? INT64_MAX : (int64_t)x); break; }
        case IR_FC + 7: { double x = UNF64; sp[-1] = x != x ? 0ull : x <= 0.0 ? 0ull : x >= 18446744073709551616.0 ? UINT64_MAX : (uint64_t)x; break; }

        default:
            // Сюда попасть нельзя: код пишет наша же проверка. Если попали —
            // это ошибка движка, и молча идти дальше нельзя.
            TRAP(WASM_TRAP_HOST);
        }
        if (step) { trap = WASM_STEPPED; goto save; }
    }
pause_service:
    trap = WASM_PAUSED_SERVICE;
    goto save;
pause_yield:
    trap = WASM_PAUSED_YIELD;
save:
    s->r_fn = fn; s->r_fp = fp; s->r_sp = sp; s->r_ip = ip;
    s->r_depth = depth; s->r_ticks = ticks; s->r_pend = pend;
    return trap;
out:
    return trap;
}

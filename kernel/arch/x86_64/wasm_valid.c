// ============================================================================
// wasm_valid.c — проверка тел функций WebAssembly (этап 51.0). См. wasm.h.
// ----------------------------------------------------------------------------
// Зачем проверка, если команды всё равно исполняет наш же интерпретатор?
// Затем, что после неё исполнителю НЕЧЕГО проверять на ходу: каждой команде
// доказано, что на стеке лежат значения нужных типов, что переход ведёт на
// существующую метку, что индексы локальных, глобальных, функций и таблиц
// существуют. Модуль, не прошедший проверку, не исполняется вовсе — это и
// есть первая стена песочницы (вторая — границы памяти на каждом обращении).
//
// Алгоритм — из приложения к спецификации («Validation Algorithm»): один
// проход по командам с двумя стеками.
//   Стек типов    — что сейчас лежит на стеке операндов.
//   Стек блоков   — открытые block/loop/if и сама функция: типы на входе и
//                   выходе, глубина стека типов при входе и признак
//                   «недостижимо».
// Тонкость — недостижимый код. После безусловного перехода (br, return,
// unreachable) стек «полиморфен»: с него можно снять значение любого типа,
// сколько угодно раз (тип WT_BOT). Так спецификация разрешает мёртвый код,
// который компиляторы оставляют после перехода, не разрешая при этом
// бессмыслицу в живом.
//
// [51.1] Тем же проходом пишется внутренний код функции (wasm_int.h): проверка
// и так знает о каждой команде всё, что нужно исполнителю, — глубину стека в
// каждой точке и то, куда ведёт каждый переход. Переход вперёд (на конец
// block/if) в момент записи ещё не знает адреса: такие слова связываются в
// цепочку через своё же поле адреса и получают его, когда блок закрывается.
// ============================================================================
#include "wasm_int.h"

#define WT_BOT 0                        // «любой тип» на полиморфном стеке

typedef struct {
    uint8_t        op;                  // 0x02 block, 0x03 loop, 0x04 if, 0x05 ветка else, 0xFF — тело функции
    bool           unreachable;
    uint32_t       height;              // глубина стека типов под блоком
    const uint8_t* in;                  // типы параметров блока
    const uint8_t* out;                 // типы результатов
    uint32_t       n_in, n_out;
    uint32_t       target;              // цикл: номер слова его начала (туда ведут переходы)
    uint32_t       patch;               // прочие блоки: цепочка слов, ждущих адрес конца (номер + 1; 0 — пусто)
    uint32_t       else_patch;          // if: слово «перейти, если 0», ждущее адрес else/конца (номер + 1)
} ctrl_t;

typedef struct {
    wasm_module_t* m;
    wasm_rd_t      r;
    uint8_t*       locals;              // типы параметров и локальных текущей функции
    uint32_t       n_locals, cap_locals;
    uint8_t*       vals;
    uint32_t       n_vals, cap_vals, max_vals;
    ctrl_t*        ctrls;
    uint32_t       n_ctrls, cap_ctrls;
    uint64_t*      code;                // внутренний код текущей функции
    uint32_t       n_code, cap_code;
} vctx_t;

#define V_BAD(v, text)      wasm_fail((v)->m, WASM_E_MALFORMED, wasm_rd_off(&(v)->r), text)
#define V_INVALID(v, text)  wasm_fail((v)->m, WASM_E_INVALID, wasm_rd_off(&(v)->r), text)
#define V_LIMIT(v, text)    wasm_fail((v)->m, WASM_E_LIMIT, wasm_rd_off(&(v)->r), text)

// Рабочие массивы проверки растут удвоением и живут до конца проверки модуля
// (не в арене модуля: после проверки они не нужны).
static bool ensure(vctx_t* v, void** buf, uint32_t* cap, uint32_t need, size_t item) {
    if (need <= *cap) return true;
    uint32_t ncap = *cap ? *cap : 256;
    while (ncap < need) ncap *= 2;
    uint8_t* nb = (uint8_t*)wasm_port_alloc((size_t)ncap * item);
    if (!nb) return wasm_fail(v->m, WASM_E_NOMEM, wasm_rd_off(&v->r), "out of memory while validating");
    const uint8_t* ob = (const uint8_t*)*buf;
    for (size_t i = 0; i < (size_t)*cap * item; i++) nb[i] = ob[i];
    if (*buf) wasm_port_free(*buf);
    *buf = nb;
    *cap = ncap;
    return true;
}

static bool push(vctx_t* v, uint8_t t) {
    if (v->n_vals >= WASM_MAX_OPSTACK) return V_LIMIT(v, "operand stack is too deep for this engine");
    if (!ensure(v, (void**)&v->vals, &v->cap_vals, v->n_vals + 1, 1)) return false;
    v->vals[v->n_vals++] = t;
    if (v->n_vals > v->max_vals) v->max_vals = v->n_vals;
    return true;
}

// ---- запись внутреннего кода -----------------------------------------------------
static bool emit(vctx_t* v, uint64_t w) {
    if (v->n_code >= 0x7FFFFFF0u) return V_LIMIT(v, "function is too large for this engine");
    if (!ensure(v, (void**)&v->code, &v->cap_code, v->n_code + 1, sizeof(uint64_t))) return false;
    v->code[v->n_code++] = w;
    return true;
}
#define EMIT(op, a, b)  emit(v, IR_MAKE(op, a, b))

static void set_target(vctx_t* v, uint32_t word, uint32_t target) {
    v->code[word] = (v->code[word] & 0xFFFFFFFFull) | ((uint64_t)target << 32);
}

// Слово word ведёт на метку глубины depth: у цикла адрес известен сразу, у
// остальных блоков слово встаёт в цепочку ждущих конца блока.
static void link_label(vctx_t* v, uint32_t depth, uint32_t word) {
    ctrl_t* c = &v->ctrls[v->n_ctrls - 1 - depth];
    if (c->op == 0x03) set_target(v, word, c->target);
    else { set_target(v, word, c->patch); c->patch = word + 1; }
}

// Все слова цепочки получают адрес target.
static void resolve(vctx_t* v, uint32_t chain, uint32_t target) {
    while (chain) {
        uint32_t word = chain - 1;
        chain = IR_B(v->code[word]);
        set_target(v, word, target);
    }
}

// Что сделать со стеком при переходе на метку: сколько верхних значений
// перенести (их ждёт метка) и сколько под ними сбросить. Считать ДО снятия
// значений метки со стека типов. В недостижимом коде глубина условна — такой
// переход никогда не исполнится, лишь бы числа не ушли в минус.
static void keep_drop(vctx_t* v, uint32_t depth, uint32_t* keep, uint32_t* drop) {
    const ctrl_t* c = &v->ctrls[v->n_ctrls - 1 - depth];
    uint32_t arity = c->op == 0x03 ? c->n_in : c->n_out;
    uint32_t want = c->height + arity;
    *keep = arity;
    *drop = v->n_vals >= want ? v->n_vals - want : 0;
}

static bool emit_br(vctx_t* v, uint16_t op, uint32_t depth) {
    uint32_t keep, drop, word = v->n_code;
    keep_drop(v, depth, &keep, &drop);
    if (!EMIT(op, 0, 0) || !emit(v, (uint64_t)keep | ((uint64_t)drop << 32))) return false;
    link_label(v, depth, word);
    return true;
}

static bool pop_any(vctx_t* v, uint8_t* t) {
    ctrl_t* c = &v->ctrls[v->n_ctrls - 1];
    if (v->n_vals == c->height) {
        // Дно блока. В недостижимом коде стек «бездонный», в живом — ошибка.
        if (!c->unreachable) return V_INVALID(v, "type mismatch (operand stack is empty)");
        *t = WT_BOT;
        return true;
    }
    *t = v->vals[--v->n_vals];
    return true;
}

static bool pop(vctx_t* v, uint8_t want) {
    uint8_t t = WT_BOT;
    if (!pop_any(v, &t)) return false;
    if (t != want && t != WT_BOT) return V_INVALID(v, "type mismatch");
    return true;
}

static bool pop_types(vctx_t* v, const uint8_t* types, uint32_t n) {
    for (uint32_t i = n; i > 0; i--) if (!pop(v, types[i - 1])) return false;
    return true;
}

static bool push_types(vctx_t* v, const uint8_t* types, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) if (!push(v, types[i])) return false;
    return true;
}

// Проверить, что на вершине лежат значения данных типов, НЕ снимая их.
// Это «снять и вернуть ТО ЖЕ САМОЕ» из алгоритма спецификации для br_table:
// возвращать надо именно снятое (в недостижимом коде — «любой тип»), а не типы
// метки. Иначе вторая метка с другим типом результата дала бы ложный отказ в
// мёртвом коде (официальный тест unreached-valid «meet-bottom»).
static bool check_top(vctx_t* v, const uint8_t* types, uint32_t n) {
    const ctrl_t* c = &v->ctrls[v->n_ctrls - 1];
    uint32_t have = v->n_vals - c->height;
    for (uint32_t i = 0; i < n; i++) {
        if (i >= have) {
            // Дальше — дно блока: в недостижимом коде там «что угодно».
            return c->unreachable ? true : V_INVALID(v, "type mismatch (operand stack is empty)");
        }
        uint8_t t = v->vals[v->n_vals - 1 - i];
        if (t != types[n - 1 - i] && t != WT_BOT) return V_INVALID(v, "type mismatch");
    }
    return true;
}

static bool push_ctrl(vctx_t* v, uint8_t op, const uint8_t* in, uint32_t n_in, const uint8_t* out, uint32_t n_out) {
    if (v->n_ctrls >= WASM_MAX_NESTING) return V_LIMIT(v, "blocks are nested too deeply for this engine");
    if (!ensure(v, (void**)&v->ctrls, &v->cap_ctrls, v->n_ctrls + 1, sizeof(ctrl_t))) return false;
    ctrl_t* c = &v->ctrls[v->n_ctrls++];
    c->op = op;
    c->unreachable = false;
    c->height = v->n_vals;
    c->in = in; c->n_in = n_in;
    c->out = out; c->n_out = n_out;
    c->target = 0; c->patch = 0; c->else_patch = 0;
    return push_types(v, in, n_in);
}

static bool pop_ctrl(vctx_t* v, ctrl_t* out) {
    ctrl_t* c = &v->ctrls[v->n_ctrls - 1];
    if (!pop_types(v, c->out, c->n_out)) return false;
    if (v->n_vals != c->height) return V_INVALID(v, "type mismatch (values left on the stack at the end of a block)");
    *out = *c;
    v->n_ctrls--;
    return true;
}

static void set_unreachable(vctx_t* v) {
    ctrl_t* c = &v->ctrls[v->n_ctrls - 1];
    v->n_vals = c->height;
    c->unreachable = true;
}

// Типы, которых ждёт переход на метку: у цикла переход ведёт В НАЧАЛО (нужны
// параметры), у остальных блоков — В КОНЕЦ (нужны результаты).
static bool label(vctx_t* v, uint32_t depth, const uint8_t** types, uint32_t* n) {
    if (depth >= v->n_ctrls) return V_INVALID(v, "unknown label");
    const ctrl_t* c = &v->ctrls[v->n_ctrls - 1 - depth];
    if (c->op == 0x03) { *types = c->in; *n = c->n_in; }
    else               { *types = c->out; *n = c->n_out; }
    return true;
}

// Тип блока: пусто (0x40), один результат (байт типа) либо индекс типа
// функции — тогда у блока есть и параметры, и несколько результатов.
static const uint8_t k_single[6] = { WT_I32, WT_I64, WT_F32, WT_F64, WT_FUNCREF, WT_EXTERNREF };

static bool rd_blocktype(vctx_t* v, const uint8_t** in, uint32_t* n_in, const uint8_t** out, uint32_t* n_out) {
    *in = 0; *n_in = 0; *out = 0; *n_out = 0;
    if (v->r.p >= v->r.end) return wasm_fail(v->m, WASM_E_MALFORMED, wasm_rd_off(&v->r), "unexpected end");
    uint8_t b = *v->r.p;
    if (b == 0x40) { v->r.p++; return true; }
    for (int i = 0; i < 6; i++)
        if (b == k_single[i]) { v->r.p++; *out = &k_single[i]; *n_out = 1; return true; }
    if (b == WT_V128) return wasm_fail(v->m, WASM_E_UNSUPPORTED, wasm_rd_off(&v->r), "SIMD (v128) is not supported");
    int64_t idx;
    if (!wasm_rd_s33(&v->r, &idx)) return false;
    if (idx < 0) return V_BAD(v, "malformed block type");
    if ((uint64_t)idx >= v->m->n_types) return V_INVALID(v, "unknown type");
    const wasm_functype_t* ft = &v->m->types[idx];
    *in = ft->types; *n_in = ft->n_params;
    *out = ft->types + ft->n_params; *n_out = ft->n_results;
    return true;
}

// Команды чтения и записи памяти 0x28..0x3E: тип значения и наибольшее
// допустимое выравнивание (двоичный логарифм размера обращения).
static const uint8_t k_mem_type[23] = {
    WT_I32, WT_I64, WT_F32, WT_F64, WT_I32, WT_I32, WT_I32, WT_I32,
    WT_I64, WT_I64, WT_I64, WT_I64, WT_I64, WT_I64,
    WT_I32, WT_I64, WT_F32, WT_F64, WT_I32, WT_I32, WT_I64, WT_I64, WT_I64,
};
static const uint8_t k_mem_align[23] = {
    2, 3, 2, 3, 0, 0, 1, 1,
    0, 0, 1, 1, 2, 2,
    2, 3, 2, 3, 0, 1, 0, 1, 2,
};

static bool need_memory(vctx_t* v) {
    return v->m->n_mems ? true : V_INVALID(v, "unknown memory");
}

static bool rd_memarg(vctx_t* v, uint8_t natural, uint32_t* offset_out) {
    uint32_t align, offset;
    if (!wasm_rd_u32(&v->r, &align) || !wasm_rd_u32(&v->r, &offset)) return false;
    *offset_out = offset;
    if (align > natural) return V_INVALID(v, "alignment must not be larger than natural");
    return need_memory(v);
}

// «Запасные» байты после memory.size / memory.grow / memory.copy и т.п. — место
// под номер памяти из будущих версий формата. Сейчас это обязан быть ровно
// один нулевой байт (не LEB-запись нуля в два байта).
static bool rd_zero(vctx_t* v) {
    uint8_t b;
    if (!wasm_rd_u8(&v->r, &b)) return false;
    if (b != 0) { v->r.p--; return V_BAD(v, "zero byte expected"); }
    return true;
}

// Числовые команды 0x45..0xC4: какие типы снимают и какой кладут.
// b == 0 — команда с одним операндом.
static void num_sig(uint8_t op, uint8_t* a, uint8_t* b, uint8_t* res) {
    *b = 0;
    if (op == 0x45)                 { *a = WT_I32; *res = WT_I32; }                      // i32.eqz
    else if (op <= 0x4F)            { *a = *b = WT_I32; *res = WT_I32; }                 // сравнения i32
    else if (op == 0x50)            { *a = WT_I64; *res = WT_I32; }                      // i64.eqz
    else if (op <= 0x5A)            { *a = *b = WT_I64; *res = WT_I32; }                 // сравнения i64
    else if (op <= 0x60)            { *a = *b = WT_F32; *res = WT_I32; }                 // сравнения f32
    else if (op <= 0x66)            { *a = *b = WT_F64; *res = WT_I32; }                 // сравнения f64
    else if (op <= 0x69)            { *a = WT_I32; *res = WT_I32; }                      // clz ctz popcnt
    else if (op <= 0x78)            { *a = *b = WT_I32; *res = WT_I32; }                 // арифметика i32
    else if (op <= 0x7B)            { *a = WT_I64; *res = WT_I64; }
    else if (op <= 0x8A)            { *a = *b = WT_I64; *res = WT_I64; }                 // арифметика i64
    else if (op <= 0x91)            { *a = WT_F32; *res = WT_F32; }                      // abs neg ceil floor trunc nearest sqrt
    else if (op <= 0x98)            { *a = *b = WT_F32; *res = WT_F32; }                 // add sub mul div min max copysign
    else if (op <= 0x9F)            { *a = WT_F64; *res = WT_F64; }
    else if (op <= 0xA6)            { *a = *b = WT_F64; *res = WT_F64; }
    else switch (op) {                                                                  // преобразования
        case 0xA7: *a = WT_I64; *res = WT_I32; break;                                   // wrap
        case 0xA8: case 0xA9: *a = WT_F32; *res = WT_I32; break;                        // trunc f32 -> i32
        case 0xAA: case 0xAB: *a = WT_F64; *res = WT_I32; break;
        case 0xAC: case 0xAD: *a = WT_I32; *res = WT_I64; break;                        // extend
        case 0xAE: case 0xAF: *a = WT_F32; *res = WT_I64; break;
        case 0xB0: case 0xB1: *a = WT_F64; *res = WT_I64; break;
        case 0xB2: case 0xB3: *a = WT_I32; *res = WT_F32; break;                        // convert
        case 0xB4: case 0xB5: *a = WT_I64; *res = WT_F32; break;
        case 0xB6: *a = WT_F64; *res = WT_F32; break;                                   // demote
        case 0xB7: case 0xB8: *a = WT_I32; *res = WT_F64; break;
        case 0xB9: case 0xBA: *a = WT_I64; *res = WT_F64; break;
        case 0xBB: *a = WT_F32; *res = WT_F64; break;                                   // promote
        case 0xBC: *a = WT_F32; *res = WT_I32; break;                                   // reinterpret
        case 0xBD: *a = WT_F64; *res = WT_I64; break;
        case 0xBE: *a = WT_I32; *res = WT_F32; break;
        case 0xBF: *a = WT_I64; *res = WT_F64; break;
        case 0xC0: case 0xC1: *a = WT_I32; *res = WT_I32; break;                        // расширение знака
        default:   *a = WT_I64; *res = WT_I64; break;                                   // 0xC2..0xC4
    }
}

static bool rd_table(vctx_t* v, uint32_t* idx) {
    if (!wasm_rd_u32(&v->r, idx)) return false;
    return *idx < v->m->n_tables ? true : V_INVALID(v, "unknown table");
}

// Команды с приставкой 0xFC: преобразования с насыщением и групповые операции.
static bool op_fc(vctx_t* v) {
    wasm_module_t* m = v->m;
    uint32_t sub, x, y;
    if (!wasm_rd_u32(&v->r, &sub)) return false;
    if (sub <= 7) {
        return pop(v, (sub & 2) ? WT_F64 : WT_F32) && push(v, (sub & 4) ? WT_I64 : WT_I32) && EMIT(IR_FC + sub, 0, 0);
    }
    switch (sub) {
    case 8:                                             // memory.init сегмент
        if (!wasm_rd_u32(&v->r, &x) || !rd_zero(v)) return false;
        // Номер сегмента данных проверяется по секции «число сегментов»: сама
        // секция данных в файле идёт ПОСЛЕ кода, и однопроходный движок её
        // ещё не видел. Поэтому без этой секции команда недопустима.
        if (!m->has_data_count) return V_BAD(v, "data count section required");
        if (x >= m->data_count) return V_INVALID(v, "unknown data segment");
        return need_memory(v) && pop(v, WT_I32) && pop(v, WT_I32) && pop(v, WT_I32) && EMIT(IR_MEMORY_INIT, 0, x);
    case 9:                                             // data.drop
        if (!wasm_rd_u32(&v->r, &x)) return false;
        if (!m->has_data_count) return V_BAD(v, "data count section required");
        return x < m->data_count ? EMIT(IR_DATA_DROP, 0, x) : V_INVALID(v, "unknown data segment");
    case 10:                                            // memory.copy
        if (!rd_zero(v) || !rd_zero(v)) return false;
        return need_memory(v) && pop(v, WT_I32) && pop(v, WT_I32) && pop(v, WT_I32) && EMIT(IR_MEMORY_COPY, 0, 0);
    case 11:                                            // memory.fill
        if (!rd_zero(v)) return false;
        return need_memory(v) && pop(v, WT_I32) && pop(v, WT_I32) && pop(v, WT_I32) && EMIT(IR_MEMORY_FILL, 0, 0);
    case 12:                                            // table.init сегмент, таблица
        if (!wasm_rd_u32(&v->r, &x) || !rd_table(v, &y)) return false;
        if (x >= m->n_elems) return V_INVALID(v, "unknown elem segment");
        if (m->elems[x].type != m->tables[y].elem) return V_INVALID(v, "type mismatch (element segment and table)");
        return pop(v, WT_I32) && pop(v, WT_I32) && pop(v, WT_I32) && EMIT(IR_TABLE_INIT, y, x);
    case 13:                                            // elem.drop
        if (!wasm_rd_u32(&v->r, &x)) return false;
        return x < m->n_elems ? EMIT(IR_ELEM_DROP, 0, x) : V_INVALID(v, "unknown elem segment");
    case 14:                                            // table.copy куда, откуда
        if (!rd_table(v, &x) || !rd_table(v, &y)) return false;
        if (m->tables[x].elem != m->tables[y].elem) return V_INVALID(v, "type mismatch (tables of different element types)");
        return pop(v, WT_I32) && pop(v, WT_I32) && pop(v, WT_I32) && EMIT(IR_TABLE_COPY, y, x);
    case 15:                                            // table.grow
        if (!rd_table(v, &x)) return false;
        return pop(v, WT_I32) && pop(v, m->tables[x].elem) && push(v, WT_I32) && EMIT(IR_TABLE_GROW, 0, x);
    case 16:                                            // table.size
        if (!rd_table(v, &x)) return false;
        return push(v, WT_I32) && EMIT(IR_TABLE_SIZE, 0, x);
    case 17:                                            // table.fill
        if (!rd_table(v, &x)) return false;
        return pop(v, WT_I32) && pop(v, m->tables[x].elem) && pop(v, WT_I32) && EMIT(IR_TABLE_FILL, 0, x);
    default:
        return V_BAD(v, "illegal opcode (unknown 0xFC instruction)");
    }
}

static bool same_types(const uint8_t* a, uint32_t na, const uint8_t* b, uint32_t nb) {
    if (na != nb) return false;
    for (uint32_t i = 0; i < na; i++) if (a[i] != b[i]) return false;
    return true;
}

// Одна функция: от первой команды до end, закрывающего её тело.
static bool validate_func(vctx_t* v, wasm_func_t* f) {
    wasm_module_t* m = v->m;
    const wasm_functype_t* ft = &m->types[f->type];

    // Типы параметров и локальных одним массивом — по нему ходят local.get/set.
    uint32_t total = ft->n_params + f->n_locals;
    if (!ensure(v, (void**)&v->locals, &v->cap_locals, total + 1, 1)) return false;
    for (uint32_t i = 0; i < ft->n_params; i++) v->locals[i] = ft->types[i];
    {
        wasm_rd_t lr = { f->locals, f->code, m };
        uint32_t at = ft->n_params;
        for (uint32_t g = 0; g < f->n_local_groups; g++) {
            uint32_t cnt;
            uint8_t t;
            if (!wasm_rd_u32(&lr, &cnt) || !wasm_rd_u8(&lr, &t)) return false;
            for (uint32_t k = 0; k < cnt; k++) v->locals[at++] = t;
        }
    }
    v->n_locals = total;
    v->r.p = f->code;
    v->r.end = f->code + f->code_len;
    v->n_vals = 0;
    v->max_vals = 0;
    v->n_ctrls = 0;
    v->n_code = 0;
    if (!push_ctrl(v, 0xFF, 0, 0, ft->types + ft->n_params, ft->n_results)) return false;

    while (v->n_ctrls) {
        uint8_t op;
        if (!wasm_rd_u8(&v->r, &op)) return false;
        const uint8_t *in, *out;
        uint32_t n_in, n_out, x, y;
        uint8_t t, t2;
        switch (op) {
        case 0x00:                                      // unreachable
            if (!EMIT(IR_UNREACHABLE, 0, 0)) return false;
            set_unreachable(v);
            break;
        case 0x01: break;                               // nop
        case 0x02: case 0x03:                           // block, loop
            if (!rd_blocktype(v, &in, &n_in, &out, &n_out)) return false;
            if (!pop_types(v, in, n_in) || !push_ctrl(v, op, in, n_in, out, n_out)) return false;
            if (op == 0x03) {
                // Переходы на метку цикла ведут сюда; здесь же интерпретатор
                // списывает «топливо» — раз за виток.
                v->ctrls[v->n_ctrls - 1].target = v->n_code;
                if (!EMIT(IR_LOOP, 0, 0)) return false;
            }
            break;
        case 0x04: {                                    // if
            if (!rd_blocktype(v, &in, &n_in, &out, &n_out)) return false;
            if (!pop(v, WT_I32) || !pop_types(v, in, n_in)) return false;
            uint32_t word = v->n_code;
            if (!EMIT(IR_BR_IFZ, 0, 0) || !push_ctrl(v, 0x04, in, n_in, out, n_out)) return false;
            v->ctrls[v->n_ctrls - 1].else_patch = word + 1;
            break;
        }
        case 0x05: {                                    // else
            ctrl_t c;
            if (v->ctrls[v->n_ctrls - 1].op != 0x04) return V_INVALID(v, "else without a matching if");
            if (!pop_ctrl(v, &c)) return false;
            // Конец ветки «да»: перепрыгнуть ветку «нет». Стек уже такой, каким
            // его ждёт конец блока, — переносить и сбрасывать нечего.
            uint32_t word = v->n_code;
            if (!EMIT(IR_BR, 0, c.patch) || !emit(v, 0)) return false;
            set_target(v, c.else_patch - 1, v->n_code);     // «если 0» ведёт сюда — в начало ветки «нет»
            if (!push_ctrl(v, 0x05, c.in, c.n_in, c.out, c.n_out)) return false;
            v->ctrls[v->n_ctrls - 1].patch = word + 1;
            break;
        }
        case 0x0B: {                                    // end
            ctrl_t c;
            if (!pop_ctrl(v, &c)) return false;
            // if без else: пропущенная ветка «ничего не делает», значит блок
            // обязан отдавать ровно то, что принял.
            if (c.op == 0x04 && !same_types(c.in, c.n_in, c.out, c.n_out))
                return V_INVALID(v, "type mismatch (if without else must not change the stack)");
            if (c.op == 0x04) set_target(v, c.else_patch - 1, v->n_code);  // if без else: «если 0» — сразу в конец
            resolve(v, c.patch, v->n_code);
            if (v->n_ctrls) { if (!push_types(v, c.out, c.n_out)) return false; }
            else if (!EMIT(IR_RETURN, 0, 0)) return false;  // конец тела: сюда же ведут переходы «из функции»
            break;
        }
        case 0x0C:                                      // br
            if (!wasm_rd_u32(&v->r, &x) || !label(v, x, &in, &n_in)) return false;
            if (!emit_br(v, IR_BR, x) || !pop_types(v, in, n_in)) return false;
            set_unreachable(v);
            break;
        case 0x0D:                                      // br_if
            if (!wasm_rd_u32(&v->r, &x) || !label(v, x, &in, &n_in)) return false;
            if (!pop(v, WT_I32) || !emit_br(v, IR_BR_IF, x)) return false;
            if (!pop_types(v, in, n_in) || !push_types(v, in, n_in)) return false;
            break;
        case 0x0E: {                                    // br_table
            uint32_t n;
            if (!wasm_rd_u32(&v->r, &n)) return false;
            if (n > wasm_rd_left(&v->r)) return V_BAD(v, "unexpected end (br_table)");
            const uint8_t* list = v->r.p;
            for (uint32_t i = 0; i < n; i++) if (!wasm_rd_u32(&v->r, &x)) return false;
            if (!wasm_rd_u32(&v->r, &y) || !label(v, y, &out, &n_out)) return false;   // метка по умолчанию
            if (!pop(v, WT_I32)) return false;
            if (!EMIT(IR_BR_TABLE, 0, n)) return false;
            wasm_rd_t lr = { list, v->r.end, m };
            for (uint32_t i = 0; i <= n; i++) {
                // Метки по порядку, последней — метка по умолчанию (y).
                if (i < n) {
                    if (!wasm_rd_u32(&lr, &x) || !label(v, x, &in, &n_in)) return false;
                    if (n_in != n_out) return V_INVALID(v, "type mismatch (br_table targets take different numbers of values)");
                    // Типы сверяются с каждой меткой, стек остаётся прежним для
                    // следующей (см. check_top).
                    if (!check_top(v, in, n_in)) return false;
                } else x = y;
                uint32_t keep, drop, word = v->n_code;
                keep_drop(v, x, &keep, &drop);
                if (!emit(v, keep) || !emit(v, drop)) return false;
                link_label(v, x, word);
            }
            if (!pop_types(v, out, n_out)) return false;
            set_unreachable(v);
            break;
        }
        case 0x0F:                                      // return
            if (!pop_types(v, ft->types + ft->n_params, ft->n_results) || !EMIT(IR_RETURN, 0, 0)) return false;
            set_unreachable(v);
            break;
        case 0x10: {                                    // call
            if (!wasm_rd_u32(&v->r, &x)) return false;
            if (x >= m->n_funcs) return V_INVALID(v, "unknown function");
            const wasm_functype_t* ct = &m->types[m->funcs[x].type];
            if (!pop_types(v, ct->types, ct->n_params) || !push_types(v, ct->types + ct->n_params, ct->n_results)) return false;
            if (!EMIT(IR_CALL, 0, x)) return false;
            break;
        }
        case 0x11: {                                    // call_indirect тип, таблица
            if (!wasm_rd_u32(&v->r, &x) || !wasm_rd_u32(&v->r, &y)) return false;
            if (y >= m->n_tables) return V_INVALID(v, "unknown table");
            if (x >= m->n_types) return V_INVALID(v, "unknown type");
            if (m->tables[y].elem != WT_FUNCREF) return V_INVALID(v, "type mismatch (call_indirect needs a table of functions)");
            const wasm_functype_t* ct = &m->types[x];
            if (!pop(v, WT_I32) || !pop_types(v, ct->types, ct->n_params) ||
                !push_types(v, ct->types + ct->n_params, ct->n_results)) return false;
            if (!EMIT(IR_CALL_INDIRECT, y, x)) return false;
            break;
        }
        case 0x1A: if (!pop_any(v, &t) || !EMIT(IR_DROP, 0, 0)) return false; break;   // drop
        case 0x1B:                                      // select без типа: только числа
            if (!pop(v, WT_I32) || !pop_any(v, &t) || !pop_any(v, &t2)) return false;
            if ((t != WT_BOT && !wasm_is_num(t)) || (t2 != WT_BOT && !wasm_is_num(t2)))
                return V_INVALID(v, "type mismatch (select without a type takes numbers only)");
            if (t != t2 && t != WT_BOT && t2 != WT_BOT) return V_INVALID(v, "type mismatch (select operands differ)");
            if (!push(v, t == WT_BOT ? t2 : t) || !EMIT(IR_SELECT, 0, 0)) return false;
            break;
        case 0x1C:                                      // select с типом
            if (!wasm_rd_u32(&v->r, &x)) return false;
            if (x != 1) return V_INVALID(v, "invalid result arity (select takes exactly one type)");
            if (!wasm_rd_u8(&v->r, &t)) return false;
            if (!wasm_is_val(t)) { v->r.p--; return V_BAD(v, "malformed value type"); }
            if (!pop(v, WT_I32) || !pop(v, t) || !pop(v, t) || !push(v, t) || !EMIT(IR_SELECT, 0, 0)) return false;
            break;
        case 0x20: case 0x21: case 0x22:                // local.get / set / tee
            if (!wasm_rd_u32(&v->r, &x)) return false;
            if (x >= v->n_locals) return V_INVALID(v, "unknown local");
            t = v->locals[x];
            if (op == 0x20) { if (!push(v, t)) return false; }
            else if (op == 0x21) { if (!pop(v, t)) return false; }
            else if (!pop(v, t) || !push(v, t)) return false;
            if (!EMIT(op, 0, x)) return false;
            break;
        case 0x23: case 0x24:                           // global.get / set
            if (!wasm_rd_u32(&v->r, &x)) return false;
            if (x >= m->n_globals) return V_INVALID(v, "unknown global");
            if (op == 0x23) { if (!push(v, m->globals[x].type)) return false; }
            else {
                if (!m->globals[x].mut) return V_INVALID(v, "global is immutable");
                if (!pop(v, m->globals[x].type)) return false;
            }
            if (!EMIT(op, 0, x)) return false;
            break;
        case 0x25:                                      // table.get
            if (!rd_table(v, &x) || !pop(v, WT_I32) || !push(v, m->tables[x].elem) || !EMIT(IR_TABLE_GET, 0, x)) return false;
            break;
        case 0x26:                                      // table.set
            if (!rd_table(v, &x) || !pop(v, m->tables[x].elem) || !pop(v, WT_I32) || !EMIT(IR_TABLE_SET, 0, x)) return false;
            break;
        case 0x3F:                                      // memory.size
            if (!rd_zero(v) || !need_memory(v) || !push(v, WT_I32) || !EMIT(IR_MEMORY_SIZE, 0, 0)) return false;
            break;
        case 0x40:                                      // memory.grow
            if (!rd_zero(v) || !need_memory(v) || !pop(v, WT_I32) || !push(v, WT_I32) || !EMIT(IR_MEMORY_GROW, 0, 0)) return false;
            break;
        case 0x41: { int32_t c; if (!wasm_rd_s32(&v->r, &c) || !push(v, WT_I32) || !EMIT(IR_CONST32, 0, (uint32_t)c)) return false; break; }
        case 0x42: { int64_t c; if (!wasm_rd_s64(&v->r, &c) || !push(v, WT_I64) || !EMIT(IR_CONST64, 0, 0) || !emit(v, (uint64_t)c)) return false; break; }
        case 0x43: { uint32_t c; if (!wasm_rd_f32(&v->r, &c) || !push(v, WT_F32) || !EMIT(IR_CONST32, 0, c)) return false; break; }
        case 0x44: { uint64_t c; if (!wasm_rd_f64(&v->r, &c) || !push(v, WT_F64) || !EMIT(IR_CONST64, 0, 0) || !emit(v, c)) return false; break; }
        case 0xD0:                                      // ref.null тип
            if (!wasm_rd_u8(&v->r, &t)) return false;
            if (!wasm_is_ref(t)) { v->r.p--; return V_BAD(v, "malformed reference type"); }
            if (!push(v, t) || !EMIT(IR_CONST32, 0, 0)) return false;       // пустая ссылка — ноль
            break;
        case 0xD1:                                      // ref.is_null
            if (!pop_any(v, &t)) return false;
            if (t != WT_BOT && !wasm_is_ref(t)) return V_INVALID(v, "type mismatch (ref.is_null takes a reference)");
            if (!push(v, WT_I32) || !EMIT(0x50, 0, 0)) return false;        // «равно ли нулю» над 64 битами = i64.eqz
            break;
        case 0xD2:                                      // ref.func
            if (!wasm_rd_u32(&v->r, &x)) return false;
            if (x >= m->n_funcs) return V_INVALID(v, "unknown function");
            // Ссылку можно взять только на функцию, «объявленную» вне тел
            // функций: так движок заранее знает, какие функции могут уйти из
            // модуля ссылкой.
            if (!wasm_declared(m, x)) return V_INVALID(v, "undeclared function reference");
            if (!push(v, WT_FUNCREF) || !EMIT(IR_REF_FUNC, 0, x)) return false;
            break;
        case 0xFC: if (!op_fc(v)) return false; break;
        case 0xFD:
            v->r.p--;
            return wasm_fail(m, WASM_E_UNSUPPORTED, wasm_rd_off(&v->r), "SIMD instructions are not supported");
        case 0xFE:
            v->r.p--;
            return wasm_fail(m, WASM_E_UNSUPPORTED, wasm_rd_off(&v->r), "threads and atomics are not supported");
        default:
            if (op >= 0x28 && op <= 0x35) {             // чтение памяти
                if (!rd_memarg(v, k_mem_align[op - 0x28], &x) || !pop(v, WT_I32) || !push(v, k_mem_type[op - 0x28])) return false;
                if (!EMIT(op, 0, x)) return false;
            } else if (op >= 0x36 && op <= 0x3E) {      // запись в память
                if (!rd_memarg(v, k_mem_align[op - 0x28], &x) || !pop(v, k_mem_type[op - 0x28]) || !pop(v, WT_I32)) return false;
                if (!EMIT(op, 0, x)) return false;
            } else if (op >= 0x45 && op <= 0xC4) {      // числа
                uint8_t a, b, res;
                num_sig(op, &a, &b, &res);
                if (b && !pop(v, b)) return false;
                if (!pop(v, a) || !push(v, res) || !EMIT(op, 0, 0)) return false;
            } else {
                v->r.p--;
                return V_BAD(v, "illegal opcode");
            }
            break;
        }
    }
    // Тело закрыто последним end — за ним в теле не должно быть ничего.
    if (v->r.p != v->r.end) return V_BAD(v, "END opcode expected (bytes after the end of a function)");
    f->max_stack = v->max_vals;
    // Готовый код — в память модуля, ровно по размеру.
    f->ir = (uint64_t*)wasm_arena(m, (uint64_t)v->n_code * sizeof(uint64_t));
    if (!f->ir) return false;
    for (uint32_t i = 0; i < v->n_code; i++) f->ir[i] = v->code[i];
    f->ir_len = v->n_code;
    m->ir_words += v->n_code;
    return true;
}

bool wasm_validate(wasm_module_t* m) {
    vctx_t v;
    for (size_t i = 0; i < sizeof(v); i++) ((uint8_t*)&v)[i] = 0;
    v.m = m;
    v.r.m = m;
    v.r.p = v.r.end = m->bin;
    bool ok = true;
    for (uint32_t i = m->n_imp_funcs; i < m->n_funcs && ok; i++) ok = validate_func(&v, &m->funcs[i]);
    if (v.locals) wasm_port_free(v.locals);
    if (v.vals) wasm_port_free(v.vals);
    if (v.ctrls) wasm_port_free(v.ctrls);
    if (v.code) wasm_port_free(v.code);
    return ok;
}

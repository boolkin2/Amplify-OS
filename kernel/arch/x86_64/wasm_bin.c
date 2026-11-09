// ============================================================================
// wasm_bin.c — разбор двоичного модуля WebAssembly (этап 51.0). См. wasm.h.
// ----------------------------------------------------------------------------
// Формат: заголовок «\0asm» + версия 1, дальше секции «номер, длина, тело».
// Секции со своими номерами идут в строго заданном порядке и не повторяются;
// секции с номером 0 («пользовательские»: имена, сведения о сборке) могут
// стоять где угодно и нами только пропускаются.
//
// Правило файла: НИ ОДНОМУ ЧИСЛУ ИЗ ФАЙЛА НЕ ВЕРИМ. Счётчик элементов
// сверяется с числом оставшихся байт ДО выделения памяти (элемент занимает
// хотя бы байт) — иначе модуль из двадцати байт заказал бы четыре гигабайта.
// Каждая секция обязана занять ровно столько, сколько заявила.
//
// Тела функций здесь только находятся (границы, локальные переменные);
// команды в них разбирает и проверяет wasm_valid.c.
// ============================================================================
#include "wasm_int.h"

// ----------------------------------------------------------------------------
// Память модуля и запись ошибки.
// ----------------------------------------------------------------------------
typedef struct wasm_blk {
    struct wasm_blk* next;
    uint64_t used, cap;                 // байт занято / всего за заголовком
} wasm_blk_t;

#define ARENA_CHUNK (64u << 10)

bool wasm_fail(wasm_module_t* m, int kind, uint32_t off, const char* text) {
    if (m->err.kind) return false;      // первая причина — настоящая
    m->err.kind = kind;
    m->err.offset = off;
    size_t i = 0;
    for (; text[i] && i < sizeof(m->err.text) - 1; i++) m->err.text[i] = text[i];
    m->err.text[i] = 0;
    return false;
}

// Блок из списка *head: память обнулена, выровнена на 8. 0 — памяти нет или
// запрос больше 2 ГБ (тогда *too_big = true, если он задан).
void* wasm_blk_alloc(void** head, uint64_t n, bool* too_big) {
    n = (n + 7) & ~7ull;
    wasm_blk_t* b = (wasm_blk_t*)*head;
    if (!b || b->cap - b->used < n) {
        uint64_t cap = n > ARENA_CHUNK ? n : ARENA_CHUNK;
        if (cap > (1ull << 31)) { if (too_big) *too_big = true; return 0; }
        wasm_blk_t* nb = (wasm_blk_t*)wasm_port_alloc(sizeof(wasm_blk_t) + cap);
        if (!nb) return 0;
        nb->used = 0;
        nb->cap = cap;
        // Большой блок под один массив ставим ВТОРЫМ: в первом ещё есть место
        // для мелочи, незачем его бросать.
        if (b && n > ARENA_CHUNK) { nb->next = b->next; b->next = nb; }
        else { nb->next = b; *head = nb; }
        b = nb;
    }
    uint8_t* p = (uint8_t*)(b + 1) + b->used;
    b->used += n;
    for (uint64_t i = 0; i < n; i++) p[i] = 0;
    return p;
}

void wasm_blk_free_all(void** head) {
    wasm_blk_t* b = (wasm_blk_t*)*head;
    while (b) { wasm_blk_t* next = b->next; wasm_port_free(b); b = next; }
    *head = 0;
}

void* wasm_arena(wasm_module_t* m, uint64_t n) {
    bool too_big = false;
    void* p = wasm_blk_alloc(&m->arena, n, &too_big);
    if (!p) {
        if (too_big) wasm_fail(m, WASM_E_LIMIT, 0, "module needs too much memory");
        else wasm_fail(m, WASM_E_NOMEM, 0, "out of memory");
    }
    return p;
}

void wasm_module_free(wasm_module_t* m) {
    if (!m) return;
    wasm_blk_free_all(&m->arena);
    wasm_port_free(m);
}

wasm_module_t* wasm_module_load(const uint8_t* bin, uint32_t len, wasm_err_t* err) {
    wasm_module_t* m = (wasm_module_t*)wasm_port_alloc(sizeof(*m));
    if (!m) {
        if (err) { err->kind = WASM_E_NOMEM; err->offset = 0; err->text[0] = 0; }
        return 0;
    }
    for (size_t i = 0; i < sizeof(*m); i++) ((uint8_t*)m)[i] = 0;
    m->bin = bin;
    m->bin_len = len;
    if (wasm_decode(m) && wasm_validate(m)) return m;
    if (err) *err = m->err;
    wasm_module_free(m);
    return 0;
}

const char* wasm_type_name(uint8_t t) {
    switch (t) {
    case WT_I32: return "i32";
    case WT_I64: return "i64";
    case WT_F32: return "f32";
    case WT_F64: return "f64";
    case WT_V128: return "v128";
    case WT_FUNCREF: return "funcref";
    case WT_EXTERNREF: return "externref";
    default: return "?";
    }
}

const char* wasm_kind_name(uint8_t k) {
    static const char* const n[4] = { "func", "table", "memory", "global" };
    return k < 4 ? n[k] : "?";
}

const char* wasm_err_name(int kind) {
    static const char* const n[] = { "ok", "malformed", "invalid", "too big for this engine", "out of memory", "unsupported" };
    return (kind >= 0 && kind <= WASM_E_UNSUPPORTED) ? n[kind] : "?";
}

int64_t wasm_find_export(const wasm_module_t* m, const char* name, uint8_t kind) {
    uint32_t len = 0;
    while (name[len]) len++;
    for (uint32_t i = 0; i < m->n_exports; i++) {
        const wasm_export_t* e = &m->exports[i];
        if (e->kind != kind || e->name.n != len) continue;
        uint32_t k = 0;
        while (k < len && e->name.p[k] == (uint8_t)name[k]) k++;
        if (k == len) return e->index;
    }
    return -1;
}

// ----------------------------------------------------------------------------
// Чтение чисел. LEB128 — по семь бит на байт, старший бит «есть продолжение».
// Спецификация требует КРАТЧАЙШЕЙ ДОПУСТИМОЙ по ширине записи: 32-битное
// число — не длиннее пяти байт, а в пятом байте «лишние» биты обязаны быть
// нулями (у знакового — копиями знака). Иначе одно и то же число имело бы
// записи, которые разные движки поняли бы по-разному.
// ----------------------------------------------------------------------------
#define RD_END(r)  wasm_fail((r)->m, WASM_E_MALFORMED, wasm_rd_off(r), "unexpected end")

bool wasm_rd_u8(wasm_rd_t* r, uint8_t* out) {
    if (r->p >= r->end) return RD_END(r);
    *out = *r->p++;
    return true;
}

bool wasm_rd_u32(wasm_rd_t* r, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 5; i++) {
        if (r->p >= r->end) return RD_END(r);
        uint8_t b = *r->p++;
        if (i == 4 && (b & 0xF0)) {
            return wasm_fail(r->m, WASM_E_MALFORMED, wasm_rd_off(r) - 1,
                             (b & 0x80) ? "integer representation too long" : "integer too large");
        }
        v |= (uint32_t)(b & 0x7F) << (7 * i);
        if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;                       // недостижимо: пятый байт разобран выше
}

// Знаковое шириной bits (32, 33 или 64) в int64.
static bool rd_sleb(wasm_rd_t* r, int bits, int64_t* out) {
    int max_bytes = (bits + 6) / 7;
    uint64_t v = 0;
    int shift = 0;
    for (int i = 0; i < max_bytes; i++) {
        if (r->p >= r->end) return RD_END(r);
        uint8_t b = *r->p++;
        if (i == max_bytes - 1) {
            if (b & 0x80)
                return wasm_fail(r->m, WASM_E_MALFORMED, wasm_rd_off(r) - 1, "integer representation too long");
            // В последнем байте значащих бит осталось (bits - shift); всё, что
            // выше, должно повторять знак.
            int used = bits - shift;                    // 1..7
            uint8_t sign_and_up = (uint8_t)(b >> (used - 1)) & (uint8_t)(0x7F >> (used - 1));
            uint8_t all = (uint8_t)(0x7F >> (used - 1));
            if (sign_and_up != 0 && sign_and_up != all)
                return wasm_fail(r->m, WASM_E_MALFORMED, wasm_rd_off(r) - 1, "integer too large");
        }
        v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40)) v |= ~0ull << shift;     // расширение знака
            *out = (int64_t)v;
            return true;
        }
    }
    return false;
}

bool wasm_rd_s32(wasm_rd_t* r, int32_t* out) {
    int64_t v;
    if (!rd_sleb(r, 32, &v)) return false;
    *out = (int32_t)v;
    return true;
}
bool wasm_rd_s64(wasm_rd_t* r, int64_t* out) { return rd_sleb(r, 64, out); }
bool wasm_rd_s33(wasm_rd_t* r, int64_t* out) { return rd_sleb(r, 33, out); }

bool wasm_rd_f32(wasm_rd_t* r, uint32_t* bits) {
    if (wasm_rd_left(r) < 4) return RD_END(r);
    *bits = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return true;
}

bool wasm_rd_f64(wasm_rd_t* r, uint64_t* bits) {
    if (wasm_rd_left(r) < 8) return RD_END(r);
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)r->p[i] << (8 * i);
    *bits = v;
    r->p += 8;
    return true;
}

// ----------------------------------------------------------------------------
// Мелкие читатели.
// ----------------------------------------------------------------------------
#define BAD(r, text)      wasm_fail((r)->m, WASM_E_MALFORMED, wasm_rd_off(r), text)
#define INVALID(r, text)  wasm_fail((r)->m, WASM_E_INVALID, wasm_rd_off(r), text)
#define LIMIT(r, text)    wasm_fail((r)->m, WASM_E_LIMIT, wasm_rd_off(r), text)

// Счётчик вектора: элементов не может быть больше, чем осталось байт.
static bool rd_count(wasm_rd_t* r, uint32_t* n) {
    if (!wasm_rd_u32(r, n)) return false;
    if (*n > wasm_rd_left(r)) return RD_END(r);
    return true;
}

// Имена обязаны быть верным UTF-8: кратчайшая запись, без суррогатов, не
// дальше U+10FFFF. Проверяем сами — другого места, где модуль с битым именем
// был бы остановлен, нет.
static bool utf8_ok(const uint8_t* p, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        if (c < 0x80) { i++; continue; }
        uint32_t need, cp, lo;
        if ((c & 0xE0) == 0xC0)      { need = 1; cp = c & 0x1F; lo = 0x80; }
        else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; lo = 0x800; }
        else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; lo = 0x10000; }
        else return false;
        if (n - i - 1 < need) return false;
        for (uint32_t k = 1; k <= need; k++) {
            if ((p[i + k] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (cp < lo || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += need + 1;
    }
    return true;
}

static bool rd_name(wasm_rd_t* r, wasm_name_t* out) {
    uint32_t n;
    if (!wasm_rd_u32(r, &n)) return false;
    if (n > wasm_rd_left(r)) return BAD(r, "unexpected end (name runs past its section)");
    if (!utf8_ok(r->p, n)) return BAD(r, "malformed UTF-8 encoding");
    out->p = r->p;
    out->n = n;
    r->p += n;
    return true;
}

static bool rd_valtype(wasm_rd_t* r, uint8_t* t) {
    if (!wasm_rd_u8(r, t)) return false;
    if (*t == WT_V128) { r->p--; return wasm_fail(r->m, WASM_E_UNSUPPORTED, wasm_rd_off(r), "SIMD (v128) is not supported"); }
    if (!wasm_is_val(*t)) { r->p--; return BAD(r, "malformed value type"); }
    return true;
}

static bool rd_reftype(wasm_rd_t* r, uint8_t* t) {
    if (!wasm_rd_u8(r, t)) return false;
    if (!wasm_is_ref(*t)) { r->p--; return BAD(r, "malformed reference type"); }
    return true;
}

// Пределы: признак (0 — только минимум, 1 — минимум и максимум). Другие
// признаки — общая память потоков и 64-битная память, которых у нас нет.
static bool rd_limits(wasm_rd_t* r, wasm_limits_t* lim) {
    uint8_t flag;
    if (!wasm_rd_u8(r, &flag)) return false;
    if (flag > 1) { r->p--; return BAD(r, "malformed limits flags"); }
    if (!wasm_rd_u32(r, &lim->min)) return false;
    lim->has_max = flag == 1;
    lim->max = 0;
    if (lim->has_max && !wasm_rd_u32(r, &lim->max)) return false;
    return true;
}

static bool limits_ok(wasm_rd_t* r, const wasm_limits_t* lim, uint64_t ceiling, const char* too_big) {
    if (lim->min > ceiling || (lim->has_max && lim->max > ceiling)) return INVALID(r, too_big);
    if (lim->has_max && lim->min > lim->max) return INVALID(r, "size minimum must not be greater than maximum");
    return true;
}

static void mark_declared(wasm_module_t* m, uint32_t f) {
    if (!m->declared) m->declared = (uint8_t*)wasm_arena(m, (m->n_funcs + 7) / 8 + 1);
    if (m->declared) m->declared[f >> 3] |= (uint8_t)(1u << (f & 7));
}

// Константное выражение: одна команда и end. want — какого типа ждём.
static bool rd_cexpr(wasm_rd_t* r, uint8_t want, wasm_cexpr_t* cx) {
    wasm_module_t* m = r->m;
    uint8_t op;
    if (!wasm_rd_u8(r, &op)) return false;
    cx->bits = 0;
    switch (op) {
    case 0x41: { int32_t v; if (!wasm_rd_s32(r, &v)) return false; cx->kind = WCX_CONST; cx->type = WT_I32; cx->bits = (uint32_t)v; break; }
    case 0x42: { int64_t v; if (!wasm_rd_s64(r, &v)) return false; cx->kind = WCX_CONST; cx->type = WT_I64; cx->bits = (uint64_t)v; break; }
    case 0x43: { uint32_t v; if (!wasm_rd_f32(r, &v)) return false; cx->kind = WCX_CONST; cx->type = WT_F32; cx->bits = v; break; }
    case 0x44: { uint64_t v; if (!wasm_rd_f64(r, &v)) return false; cx->kind = WCX_CONST; cx->type = WT_F64; cx->bits = v; break; }
    case 0x23: {
        uint32_t g;
        if (!wasm_rd_u32(r, &g)) return false;
        // Только импортированная и неизменяемая: её значение известно до
        // вычисления собственных глобальных модуля.
        if (g >= m->n_imp_globals) return INVALID(r, "unknown global in a constant expression");
        if (m->globals[g].mut) return INVALID(r, "constant expression required");
        cx->kind = WCX_GLOBAL; cx->type = m->globals[g].type; cx->bits = g;
        break;
    }
    case 0xD0: {
        uint8_t t;
        if (!rd_reftype(r, &t)) return false;
        cx->kind = WCX_REF_NULL; cx->type = t;
        break;
    }
    case 0xD2: {
        uint32_t f;
        if (!wasm_rd_u32(r, &f)) return false;
        if (f >= m->n_funcs) return INVALID(r, "unknown function");
        mark_declared(m, f);
        cx->kind = WCX_REF_FUNC; cx->type = WT_FUNCREF; cx->bits = f;
        break;
    }
    case 0x0B: r->p--; return INVALID(r, "type mismatch (empty constant expression)");
    default:   r->p--; return INVALID(r, "constant expression required");
    }
    uint8_t end;
    if (!wasm_rd_u8(r, &end)) return false;
    if (end != 0x0B) { r->p--; return INVALID(r, "constant expression required"); }
    if (cx->type != want) return INVALID(r, "type mismatch in a constant expression");
    return true;
}

// ----------------------------------------------------------------------------
// Секции.
// ----------------------------------------------------------------------------
static bool sec_type(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if (n > WASM_MAX_TYPES) return LIMIT(r, "too many types");
    m->types = (wasm_functype_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_functype_t));
    if (n && !m->types) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t form;
        if (!wasm_rd_u8(r, &form)) return false;
        if (form != 0x60) { r->p--; return BAD(r, "malformed type (function type expected)"); }
        uint32_t np, nr;
        if (!rd_count(r, &np)) return false;
        if (np > WASM_MAX_PARAMS) return LIMIT(r, "too many parameters");
        const uint8_t* params = r->p;
        for (uint32_t k = 0; k < np; k++) { uint8_t t; if (!rd_valtype(r, &t)) return false; }
        if (!rd_count(r, &nr)) return false;
        if (nr > WASM_MAX_RESULTS) return LIMIT(r, "too many results");
        const uint8_t* results = r->p;
        for (uint32_t k = 0; k < nr; k++) { uint8_t t; if (!rd_valtype(r, &t)) return false; }
        wasm_functype_t* ft = &m->types[i];
        ft->types = (uint8_t*)wasm_arena(m, (uint64_t)np + nr + 1);
        if (!ft->types) return false;
        for (uint32_t k = 0; k < np; k++) ft->types[k] = params[k];
        for (uint32_t k = 0; k < nr; k++) ft->types[np + k] = results[k];
        ft->n_params = np;
        ft->n_results = nr;
    }
    m->n_types = n;
    return true;
}

static bool sec_import(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if (n > WASM_MAX_IMPORTS) return LIMIT(r, "too many imports");
    // Сколько импортов какого вида, заранее неизвестно — массивы пространств
    // индексов берём с запасом на все n.
    m->imports = (wasm_import_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_import_t));
    m->funcs   = (wasm_func_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_func_t));
    m->tables  = (wasm_tabletype_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_tabletype_t));
    m->mems    = (wasm_memtype_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_memtype_t));
    m->globals = (wasm_global_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_global_t));
    if (n && (!m->imports || !m->funcs || !m->tables || !m->mems || !m->globals)) return false;
    for (uint32_t i = 0; i < n; i++) {
        wasm_import_t* im = &m->imports[i];
        if (!rd_name(r, &im->module) || !rd_name(r, &im->field) || !wasm_rd_u8(r, &im->kind)) return false;
        switch (im->kind) {
        case WK_FUNC: {
            uint32_t t;
            if (!wasm_rd_u32(r, &t)) return false;
            if (t >= m->n_types) return INVALID(r, "unknown type");
            im->index = m->n_funcs;
            m->funcs[m->n_funcs].type = t;
            m->funcs[m->n_funcs].imported = true;
            m->n_funcs++;
            break;
        }
        case WK_TABLE: {
            wasm_tabletype_t* t = &m->tables[m->n_tables];
            if (!rd_reftype(r, &t->elem) || !rd_limits(r, &t->lim)) return false;
            if (!limits_ok(r, &t->lim, 0xFFFFFFFFull, "table size")) return false;
            t->imported = true;
            im->index = m->n_tables++;
            break;
        }
        case WK_MEM: {
            wasm_memtype_t* t = &m->mems[m->n_mems];
            if (!rd_limits(r, &t->lim)) return false;
            if (!limits_ok(r, &t->lim, 65536, "memory size must be at most 65536 pages (4GiB)")) return false;
            t->imported = true;
            im->index = m->n_mems++;
            break;
        }
        case WK_GLOBAL: {
            wasm_global_t* g = &m->globals[m->n_globals];
            uint8_t mut;
            if (!rd_valtype(r, &g->type) || !wasm_rd_u8(r, &mut)) return false;
            if (mut > 1) { r->p--; return BAD(r, "malformed mutability"); }
            g->mut = mut;
            g->imported = true;
            im->index = m->n_globals++;
            break;
        }
        default:
            r->p--;
            return BAD(r, "malformed import kind");
        }
    }
    m->n_imports = n;
    m->n_imp_funcs = m->n_funcs;
    m->n_imp_tables = m->n_tables;
    m->n_imp_mems = m->n_mems;
    m->n_imp_globals = m->n_globals;
    return true;
}

// Нарастить массив пространства индексов на add элементов (импортированные
// уже лежат в начале).
static void* grow(wasm_module_t* m, void* old, uint32_t have, uint32_t add, size_t item) {
    uint8_t* nw = (uint8_t*)wasm_arena(m, ((uint64_t)have + add) * item + 8);
    if (!nw) return 0;
    const uint8_t* o = (const uint8_t*)old;
    for (uint64_t i = 0; i < (uint64_t)have * item; i++) nw[i] = o[i];
    return nw;
}

static bool sec_function(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if ((uint64_t)m->n_funcs + n > WASM_MAX_FUNCS) return LIMIT(r, "too many functions");
    m->funcs = (wasm_func_t*)grow(m, m->funcs, m->n_funcs, n, sizeof(wasm_func_t));
    if (!m->funcs) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t;
        if (!wasm_rd_u32(r, &t)) return false;
        if (t >= m->n_types) return INVALID(r, "unknown type");
        m->funcs[m->n_funcs++].type = t;
    }
    return true;
}

static bool sec_table(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if ((uint64_t)m->n_tables + n > WASM_MAX_TABLES) return LIMIT(r, "too many tables");
    m->tables = (wasm_tabletype_t*)grow(m, m->tables, m->n_tables, n, sizeof(wasm_tabletype_t));
    if (!m->tables) return false;
    for (uint32_t i = 0; i < n; i++) {
        wasm_tabletype_t* t = &m->tables[m->n_tables];
        if (!rd_reftype(r, &t->elem) || !rd_limits(r, &t->lim)) return false;
        if (!limits_ok(r, &t->lim, 0xFFFFFFFFull, "table size")) return false;
        m->n_tables++;
    }
    return true;
}

static bool sec_memory(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    m->mems = (wasm_memtype_t*)grow(m, m->mems, m->n_mems, n, sizeof(wasm_memtype_t));
    if (!m->mems) return false;
    for (uint32_t i = 0; i < n; i++) {
        wasm_memtype_t* t = &m->mems[m->n_mems];
        if (!rd_limits(r, &t->lim)) return false;
        if (!limits_ok(r, &t->lim, 65536, "memory size must be at most 65536 pages (4GiB)")) return false;
        m->n_mems++;
    }
    return true;
}

static bool sec_global(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if ((uint64_t)m->n_globals + n > WASM_MAX_GLOBALS) return LIMIT(r, "too many globals");
    m->globals = (wasm_global_t*)grow(m, m->globals, m->n_globals, n, sizeof(wasm_global_t));
    if (!m->globals) return false;
    for (uint32_t i = 0; i < n; i++) {
        wasm_global_t* g = &m->globals[m->n_globals];
        uint8_t mut;
        if (!rd_valtype(r, &g->type) || !wasm_rd_u8(r, &mut)) return false;
        if (mut > 1) { r->p--; return BAD(r, "malformed mutability"); }
        g->mut = mut;
        if (!rd_cexpr(r, g->type, &g->init)) return false;
        m->n_globals++;                 // после выражения: сослаться на себя глобальная не может
    }
    return true;
}

static bool name_eq(const wasm_name_t* a, const wasm_name_t* b) {
    if (a->n != b->n) return false;
    for (uint32_t i = 0; i < a->n; i++) if (a->p[i] != b->p[i]) return false;
    return true;
}

static uint32_t name_hash(const wasm_name_t* a) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < a->n; i++) h = (h ^ a->p[i]) * 16777619u;
    return h;
}

static bool sec_export(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if (n > WASM_MAX_EXPORTS) return LIMIT(r, "too many exports");
    m->exports = (wasm_export_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_export_t));
    // Имена экспорта обязаны различаться. Таблица с открытой адресацией: у
    // больших модулей экспортов тысячи, попарное сравнение было бы квадратом.
    uint32_t cap = 16;
    while (cap < n * 2) cap <<= 1;
    uint32_t* seen = (uint32_t*)wasm_arena(m, (uint64_t)cap * sizeof(uint32_t));
    if ((n && !m->exports) || !seen) return false;
    for (uint32_t i = 0; i < n; i++) {
        wasm_export_t* e = &m->exports[i];
        if (!rd_name(r, &e->name) || !wasm_rd_u8(r, &e->kind)) return false;
        if (e->kind > WK_GLOBAL) { r->p--; return BAD(r, "malformed export kind"); }
        if (!wasm_rd_u32(r, &e->index)) return false;
        uint32_t limit = e->kind == WK_FUNC ? m->n_funcs : e->kind == WK_TABLE ? m->n_tables
                       : e->kind == WK_MEM ? m->n_mems : m->n_globals;
        if (e->index >= limit) {
            return INVALID(r, e->kind == WK_FUNC ? "unknown function" : e->kind == WK_TABLE ? "unknown table"
                              : e->kind == WK_MEM ? "unknown memory" : "unknown global");
        }
        if (e->kind == WK_FUNC) mark_declared(m, e->index);
        uint32_t h = name_hash(&e->name) & (cap - 1);
        while (seen[h]) {
            if (name_eq(&m->exports[seen[h] - 1].name, &e->name)) return INVALID(r, "duplicate export name");
            h = (h + 1) & (cap - 1);
        }
        seen[h] = i + 1;
    }
    m->n_exports = n;
    return true;
}

static bool sec_start(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    if (!wasm_rd_u32(r, &m->start)) return false;
    if (m->start >= m->n_funcs) return INVALID(r, "unknown function");
    const wasm_functype_t* ft = &m->types[m->funcs[m->start].type];
    if (ft->n_params || ft->n_results) return INVALID(r, "start function must have no parameters and no results");
    m->has_start = true;
    return true;
}

// Сегмент элементов. Восемь вариантов записи (признак 0..7) — следы того, как
// формат дорастал от «одна таблица, только номера функций» до ссылочных
// типов: бит 0 — пассивный/описательный, бит 1 — явная таблица (или
// «описательный»), бит 2 — элементы записаны выражениями, а не номерами.
static bool sec_element(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if (n > WASM_MAX_SEGMENTS) return LIMIT(r, "too many element segments");
    m->elems = (wasm_elem_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_elem_t));
    if (n && !m->elems) return false;
    for (uint32_t i = 0; i < n; i++) {
        wasm_elem_t* e = &m->elems[i];
        uint32_t flags;
        if (!wasm_rd_u32(r, &flags)) return false;
        if (flags > 7) return BAD(r, "malformed element segment flags");
        bool not_active = flags & 1, explicit_ = flags & 2, exprs = flags & 4;
        e->mode = !not_active ? WSEG_ACTIVE : explicit_ ? WSEG_DECLARATIVE : WSEG_PASSIVE;
        e->type = WT_FUNCREF;
        e->table = 0;
        if (e->mode == WSEG_ACTIVE) {
            if (explicit_ && !wasm_rd_u32(r, &e->table)) return false;
            if (e->table >= m->n_tables) return INVALID(r, "unknown table");
            if (!rd_cexpr(r, WT_I32, &e->offset)) return false;
        }
        if (not_active || explicit_) {
            // Вид элементов: байт 0 («функции») либо, у выражений, ссылочный тип.
            if (exprs) { if (!rd_reftype(r, &e->type)) return false; }
            else {
                uint8_t kind;
                if (!wasm_rd_u8(r, &kind)) return false;
                if (kind != 0) { r->p--; return BAD(r, "malformed element kind"); }
            }
        }
        if (e->mode == WSEG_ACTIVE && m->tables[e->table].elem != e->type) return INVALID(r, "type mismatch (element segment and table)");
        if (!rd_count(r, &e->n)) return false;
        e->items = (uint32_t*)wasm_arena(m, (uint64_t)e->n * sizeof(uint32_t) + 8);
        if (!e->items) return false;
        for (uint32_t k = 0; k < e->n; k++) {
            if (!exprs) {
                uint32_t f;
                if (!wasm_rd_u32(r, &f)) return false;
                if (f >= m->n_funcs) return INVALID(r, "unknown function");
                mark_declared(m, f);
                e->items[k] = f;
            } else {
                wasm_cexpr_t cx;
                if (!rd_cexpr(r, e->type, &cx)) return false;
                e->items[k] = cx.kind == WCX_REF_NULL ? WASM_ELEM_NULL
                            : cx.kind == WCX_GLOBAL   ? (WASM_ELEM_GLOBAL | (uint32_t)cx.bits)
                            :                           (uint32_t)cx.bits;
            }
        }
    }
    m->n_elems = n;
    return true;
}

static bool sec_data_count(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    if (!wasm_rd_u32(r, &m->data_count)) return false;
    m->has_data_count = true;
    return true;
}

static bool sec_code(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if (n != m->n_funcs - m->n_imp_funcs) return BAD(r, "function and code section have inconsistent lengths");
    for (uint32_t i = 0; i < n; i++) {
        wasm_func_t* f = &m->funcs[m->n_imp_funcs + i];
        uint32_t size;
        if (!wasm_rd_u32(r, &size)) return false;
        if (size > wasm_rd_left(r)) return BAD(r, "unexpected end (function body runs past the code section)");
        if (size > WASM_MAX_BODY) return LIMIT(r, "function body is too large");
        wasm_rd_t b = { r->p, r->p + size, m };
        r->p += size;

        uint32_t groups;
        if (!rd_count(&b, &groups)) return false;
        f->locals = b.p;
        f->n_local_groups = groups;
        uint64_t total = 0;
        for (uint32_t g = 0; g < groups; g++) {
            uint32_t cnt;
            uint8_t t;
            if (!wasm_rd_u32(&b, &cnt)) return false;
            total += cnt;
            // Сумма сверяется на каждом шаге: счётчики по 2^32-1 переполнили бы
            // и 64 бита, если бы групп было много.
            if (total > 0xFFFFFFFFull) return BAD(&b, "too many locals");
            if (!rd_valtype(&b, &t)) return false;
        }
        if (total + m->types[f->type].n_params > WASM_MAX_LOCALS) return LIMIT(&b, "too many locals for this engine");
        f->n_locals = (uint32_t)total;
        f->code = b.p;
        f->code_len = (uint32_t)(b.end - b.p);
        if (!f->code_len) return BAD(&b, "unexpected end (function body has no end)");
        m->code_bytes += f->code_len;
    }
    return true;
}

static bool sec_data(wasm_rd_t* r) {
    wasm_module_t* m = r->m;
    uint32_t n;
    if (!rd_count(r, &n)) return false;
    if (n > WASM_MAX_SEGMENTS) return LIMIT(r, "too many data segments");
    if (m->has_data_count && n != m->data_count) return BAD(r, "data count and data section have inconsistent lengths");
    m->datas = (wasm_data_t*)wasm_arena(m, (uint64_t)n * sizeof(wasm_data_t));
    if (n && !m->datas) return false;
    for (uint32_t i = 0; i < n; i++) {
        wasm_data_t* d = &m->datas[i];
        uint32_t flags;
        if (!wasm_rd_u32(r, &flags)) return false;
        if (flags > 2) return BAD(r, "malformed data segment flags");
        d->mode = flags == 1 ? WSEG_PASSIVE : WSEG_ACTIVE;
        if (d->mode == WSEG_ACTIVE) {
            uint32_t mem = 0;
            if (flags == 2 && !wasm_rd_u32(r, &mem)) return false;
            if (mem >= m->n_mems) return INVALID(r, "unknown memory");
            if (!rd_cexpr(r, WT_I32, &d->offset)) return false;
        }
        if (!wasm_rd_u32(r, &d->n)) return false;
        if (d->n > wasm_rd_left(r)) return BAD(r, "unexpected end (data segment runs past its section)");
        d->bytes = r->p;
        r->p += d->n;
    }
    m->n_datas = n;
    return true;
}

// ----------------------------------------------------------------------------
// Модуль целиком.
// ----------------------------------------------------------------------------
bool wasm_decode(wasm_module_t* m) {
    wasm_rd_t r = { m->bin, m->bin + m->bin_len, m };
    if (m->bin_len < 4 || m->bin[0] != 0 || m->bin[1] != 'a' || m->bin[2] != 's' || m->bin[3] != 'm')
        return wasm_fail(m, WASM_E_MALFORMED, 0, m->bin_len < 4 ? "unexpected end (no magic header)" : "magic header not detected");
    if (m->bin_len < 8) return wasm_fail(m, WASM_E_MALFORMED, 4, "unexpected end (no version)");
    if (m->bin[4] != 1 || m->bin[5] != 0 || m->bin[6] != 0 || m->bin[7] != 0)
        return wasm_fail(m, WASM_E_MALFORMED, 4, "unknown binary version");
    r.p += 8;

    // Место секции в обязательном порядке: номера идут 1..11, но «число
    // сегментов данных» (12) стоит между элементами (9) и кодом (10).
    static const uint8_t order[13] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 10 };
    int last = 0;
    bool have_code = false;
    while (r.p < r.end) {
        uint8_t id;
        uint32_t size;
        if (!wasm_rd_u8(&r, &id)) return false;
        if (id > 12) { r.p--; return BAD(&r, "malformed section id"); }
        if (!wasm_rd_u32(&r, &size)) return false;
        if (size > wasm_rd_left(&r)) return BAD(&r, "unexpected end (section runs past the file)");
        wasm_rd_t s = { r.p, r.p + size, m };
        r.p += size;
        if (id == 0) {
            wasm_name_t nm;
            if (!rd_name(&s, &nm)) return false;
            m->n_custom++;
            continue;
        }
        if (order[id] <= last) return BAD(&s, "unexpected content after last section (section out of order or repeated)");
        last = order[id];
        bool ok;
        switch (id) {
        case 1:  ok = sec_type(&s); break;
        case 2:  ok = sec_import(&s); break;
        case 3:  ok = sec_function(&s); break;
        case 4:  ok = sec_table(&s); break;
        case 5:  ok = sec_memory(&s); break;
        case 6:  ok = sec_global(&s); break;
        case 7:  ok = sec_export(&s); break;
        case 8:  ok = sec_start(&s); break;
        case 9:  ok = sec_element(&s); break;
        case 10: ok = sec_code(&s); have_code = true; break;
        case 11: ok = sec_data(&s); break;
        default: ok = sec_data_count(&s); break;
        }
        if (!ok) return false;
        if (s.p != s.end) return BAD(&s, "section size mismatch");
    }
    if (m->n_mems > 1) return wasm_fail(m, WASM_E_INVALID, 0, "multiple memories");
    if (!have_code && m->n_funcs != m->n_imp_funcs)
        return wasm_fail(m, WASM_E_MALFORMED, m->bin_len, "function and code section have inconsistent lengths");
    if (m->has_data_count && m->data_count != m->n_datas)
        return wasm_fail(m, WASM_E_MALFORMED, m->bin_len, "data count and data section have inconsistent lengths");
    return true;
}

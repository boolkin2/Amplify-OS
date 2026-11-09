// ============================================================================
// wasm_rt.c — хранилище и экземпляры модулей WebAssembly (этап 51.1). См. wasm.h.
// ----------------------------------------------------------------------------
// Модуль (wasm_module_t) — это текст программы: типы, код, описания. Чтобы он
// заработал, из него делают ЭКЗЕМПЛЯР: выделяют память и таблицы, вычисляют
// глобальные, связывают импорты с тем, что даёт окружение, раскладывают
// сегменты данных и элементов, вызывают функцию start. Этим занят этот файл.
//
// Импортировать можно четыре вида вещей (функцию, таблицу, память, глобальную),
// и импортированная вещь — та же самая, а не копия: два экземпляра с общей
// памятью видят записи друг друга. Поэтому экземпляр хранит не сами объекты,
// а указатели на них, а владеет объектами хранилище.
//
// Правило границ: всё, что приходит из модуля во время запуска (смещения
// сегментов, размеры), сверяется с фактическими размерами в 64-битной
// арифметике — сумма двух 32-битных чисел не должна «обернуться» и пройти
// проверку.
// ============================================================================
#include "wasm_int.h"

#define DEF_MEMORY_PAGES  1024u         // 64 МБ
#define DEF_TABLE_ELEMS   (1u << 20)
#define DEF_STACK_SLOTS   (1u << 18)    // 2 МБ
#define DEF_FRAMES        (1u << 14)

static void* s_alloc(wasm_store_t* s, uint64_t n) { return wasm_blk_alloc(&s->arena, n, 0); }

const char* wasm_trap_name(int trap) {
    static const char* const n[] = {
        "none", "unreachable", "integer divide by zero", "integer overflow", "invalid conversion to integer",
        "out of bounds memory access", "out of bounds table access", "undefined element", "uninitialized element",
        "indirect call type mismatch", "call stack exhausted", "out of fuel", "stopped", "host function failed",
        "exit", "out of memory",
    };
    return (trap >= 0 && trap <= WASM_TRAP_NOMEM) ? n[trap] : "?";
}

// ----------------------------------------------------------------------------
// Хранилище.
// ----------------------------------------------------------------------------
wasm_store_t* wasm_store_new(const wasm_config_t* cfg) {
    wasm_store_t* s = (wasm_store_t*)wasm_port_alloc(sizeof(*s));
    if (!s) return 0;
    for (size_t i = 0; i < sizeof(*s); i++) ((uint8_t*)s)[i] = 0;
    if (cfg) s->cfg = *cfg;
    if (!s->cfg.max_memory_pages) s->cfg.max_memory_pages = DEF_MEMORY_PAGES;
    if (s->cfg.max_memory_pages > 65536) s->cfg.max_memory_pages = 65536;
    if (!s->cfg.max_table_elems) s->cfg.max_table_elems = DEF_TABLE_ELEMS;
    if (!s->cfg.stack_slots) s->cfg.stack_slots = DEF_STACK_SLOTS;
    if (!s->cfg.max_frames) s->cfg.max_frames = DEF_FRAMES;
    s->fuel = -1;
    s->tick_unit = 256;
    s->jit = wasm_jit_enabled();
    s->stack = (wasm_val_t*)wasm_port_alloc((size_t)s->cfg.stack_slots * sizeof(wasm_val_t));
    s->frames = (wasm_frame_t*)wasm_port_alloc((size_t)s->cfg.max_frames * sizeof(wasm_frame_t));
    if (!s->stack || !s->frames) { wasm_store_free(s); return 0; }
    return s;
}

void wasm_store_free(wasm_store_t* s) {
    if (!s) return;
    wasm_jit_free(s);
    for (wasm_memory_t* m = s->mems; m; m = m->next) if (m->data) wasm_port_mem_free(m->data, m->cap);
    for (wasm_table_t* t = s->tables; t; t = t->next) if (t->elems) wasm_port_free(t->elems);
    if (s->canon) wasm_port_free(s->canon);
    if (s->stack) wasm_port_free(s->stack);
    if (s->frames) wasm_port_free(s->frames);
    wasm_blk_free_all(&s->arena);
    wasm_port_free(s);
}

void wasm_store_limit(wasm_store_t* s, int64_t fuel, volatile int* stop) {
    s->fuel = fuel;
    s->stop = stop;
}

uint64_t wasm_store_ticks(const wasm_store_t* s) { return s->ticks; }
void wasm_store_jit_info(const wasm_store_t* s, uint32_t* funcs, uint64_t* bytes) {
    bool on = s->jit && !s->jit_off;
    if (funcs) *funcs = on ? s->jit_funcs : 0;
    if (bytes) *bytes = on ? s->jit_bytes : 0;
}

// Типы функций сравниваются по устройству, а не по номеру в модуле: функция,
// положенная в общую таблицу одним модулем, вызывается другим, у которого тот
// же тип стоит под другим номером. Чтобы call_indirect сверял типы одним
// сравнением чисел, каждый различный тип получает номер в хранилище.
uint32_t wasm_canon_type(wasm_store_t* s, const wasm_functype_t* ft) {
    uint32_t h = 2166136261u ^ ft->n_params;
    for (uint32_t i = 0; i < ft->n_params + ft->n_results; i++) h = (h ^ ft->types[i]) * 16777619u;
    for (uint32_t i = 0; i < s->n_canon; i++) {
        const wasm_canon_t* c = &s->canon[i];
        if (c->hash != h || c->n_params != ft->n_params || c->n_results != ft->n_results) continue;
        uint32_t k = 0, n = ft->n_params + ft->n_results;
        while (k < n && c->types[k] == ft->types[k]) k++;
        if (k == n) return i;
    }
    if (s->n_canon == s->cap_canon) {
        uint32_t ncap = s->cap_canon ? s->cap_canon * 2 : 64;
        wasm_canon_t* nc = (wasm_canon_t*)wasm_port_alloc((size_t)ncap * sizeof(wasm_canon_t));
        if (!nc) return 0xFFFFFFFFu;
        for (uint32_t i = 0; i < s->n_canon; i++) nc[i] = s->canon[i];
        if (s->canon) wasm_port_free(s->canon);
        s->canon = nc;
        s->cap_canon = ncap;
    }
    // Копия типов — в память хранилища: модуль, принёсший тип первым, может
    // оказаться не тем, кто им пользуется.
    uint32_t n = ft->n_params + ft->n_results;
    uint8_t* types = (uint8_t*)s_alloc(s, (uint64_t)n + 1);
    if (!types) return 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) types[i] = ft->types[i];
    wasm_canon_t* c = &s->canon[s->n_canon];
    c->types = types; c->n_params = ft->n_params; c->n_results = ft->n_results; c->hash = h;
    return s->n_canon++;
}

// ----------------------------------------------------------------------------
// Объекты.
// ----------------------------------------------------------------------------
wasm_funcinst_t* wasm_host_func(wasm_store_t* s, const uint8_t* params, uint32_t n_params,
                                const uint8_t* results, uint32_t n_results, wasm_host_fn fn, void* user) {
    wasm_funcinst_t* f = (wasm_funcinst_t*)s_alloc(s, sizeof(*f));
    wasm_functype_t* ft = (wasm_functype_t*)s_alloc(s, sizeof(*ft));
    uint8_t* types = (uint8_t*)s_alloc(s, (uint64_t)n_params + n_results + 1);
    if (!f || !ft || !types) return 0;
    for (uint32_t i = 0; i < n_params; i++) types[i] = params[i];
    for (uint32_t i = 0; i < n_results; i++) types[n_params + i] = results[i];
    ft->types = types; ft->n_params = n_params; ft->n_results = n_results;
    f->type = ft;
    f->canon = wasm_canon_type(s, ft);
    if (f->canon == 0xFFFFFFFFu) return 0;
    f->n_params = n_params; f->n_results = n_results;
    f->host = fn; f->user = user;
    return f;
}

wasm_memory_t* wasm_memory_new(wasm_store_t* s, uint32_t min_pages, uint32_t max_pages, bool has_max) {
    if (min_pages > s->cfg.max_memory_pages) return 0;
    wasm_memory_t* m = (wasm_memory_t*)s_alloc(s, sizeof(*m));
    if (!m) return 0;
    m->max_pages = (has_max && max_pages < s->cfg.max_memory_pages) ? max_pages : s->cfg.max_memory_pages;
    m->decl_max = max_pages;
    m->has_max = has_max;
    m->pages = min_pages;
    m->bytes = (uint64_t)min_pages * WASM_PAGE;
    m->cap = m->bytes;
    if (m->cap) {
        m->data = (uint8_t*)wasm_port_mem_alloc(m->cap);
        if (!m->data) return 0;
    }
    m->next = s->mems;
    s->mems = m;
    return m;
}

int64_t wasm_memory_grow(wasm_store_t* s, wasm_memory_t* mem, uint32_t delta) {
    (void)s;
    uint32_t old = mem->pages;
    if (!delta) return old;
    if ((uint64_t)old + delta > mem->max_pages) return -1;
    uint64_t need = ((uint64_t)old + delta) * WASM_PAGE;
    if (need > mem->cap) {
        // Растём с запасом (вдвое), чтобы программа, прибавляющая по странице,
        // не копировала всю память на каждом шаге; но не дальше своего потолка.
        uint64_t cap = mem->cap ? mem->cap * 2 : 16 * (uint64_t)WASM_PAGE;
        if (cap < need) cap = need;
        uint64_t ceiling = (uint64_t)mem->max_pages * WASM_PAGE;
        if (cap > ceiling) cap = ceiling;
        uint8_t* nd = (uint8_t*)wasm_port_mem_alloc(cap);
        if (!nd && cap > need) { cap = need; nd = (uint8_t*)wasm_port_mem_alloc(cap); }
        if (!nd) return -1;             // по спецификации нехватка памяти — это «-1», а не остановка
        for (uint64_t i = 0; i < mem->bytes; i++) nd[i] = mem->data[i];
        if (mem->data) wasm_port_mem_free(mem->data, mem->cap);
        mem->data = nd;
        mem->cap = cap;
    }
    mem->pages = old + delta;
    mem->bytes = need;
    return old;
}

wasm_table_t* wasm_table_new(wasm_store_t* s, uint8_t elem, uint32_t min, uint32_t max, bool has_max) {
    if (min > s->cfg.max_table_elems) return 0;
    wasm_table_t* t = (wasm_table_t*)s_alloc(s, sizeof(*t));
    if (!t) return 0;
    t->type = elem;
    t->max = (has_max && max < s->cfg.max_table_elems) ? max : s->cfg.max_table_elems;
    t->decl_max = max;
    t->has_max = has_max;
    t->size = min;
    t->cap = min;
    if (min) {
        t->elems = (wasm_val_t*)wasm_port_alloc((size_t)min * sizeof(wasm_val_t));
        if (!t->elems) return 0;
        for (uint32_t i = 0; i < min; i++) t->elems[i] = 0;
    }
    t->next = s->tables;
    s->tables = t;
    return t;
}

int64_t wasm_table_grow(wasm_store_t* s, wasm_table_t* t, uint32_t delta, wasm_val_t fill) {
    (void)s;
    uint32_t old = t->size;
    if (!delta) return old;
    if ((uint64_t)old + delta > t->max) return -1;
    uint32_t need = old + delta;
    if (need > t->cap) {
        uint64_t cap = t->cap ? (uint64_t)t->cap * 2 : 16;
        if (cap < need) cap = need;
        if (cap > t->max) cap = t->max;
        wasm_val_t* ne = (wasm_val_t*)wasm_port_alloc((size_t)cap * sizeof(wasm_val_t));
        if (!ne) return -1;
        for (uint32_t i = 0; i < old; i++) ne[i] = t->elems[i];
        if (t->elems) wasm_port_free(t->elems);
        t->elems = ne;
        t->cap = (uint32_t)cap;
    }
    for (uint32_t i = old; i < need; i++) t->elems[i] = fill;
    t->size = need;
    return old;
}

wasm_globalinst_t* wasm_global_new(wasm_store_t* s, uint8_t type, bool mut, wasm_val_t value) {
    wasm_globalinst_t* g = (wasm_globalinst_t*)s_alloc(s, sizeof(*g));
    if (!g) return 0;
    g->type = type; g->mut = mut; g->value = value;
    return g;
}

const wasm_functype_t* wasm_func_type(const wasm_funcinst_t* f) { return f->type; }
uint8_t* wasm_memory_data(wasm_memory_t* mem, uint64_t* bytes) { if (bytes) *bytes = mem->bytes; return mem->data; }
uint32_t wasm_memory_pages(const wasm_memory_t* mem) { return mem->pages; }
wasm_memory_t* wasm_instance_memory(const wasm_instance_t* inst) { return inst->mem; }
wasm_val_t wasm_global_get(const wasm_globalinst_t* g, uint8_t* type) { if (type) *type = g->type; return g->value; }

// ----------------------------------------------------------------------------
// Создание экземпляра.
// ----------------------------------------------------------------------------
static void err_set(wasm_inst_err_t* err, int kind, int trap, const char* a, const wasm_import_t* imp) {
    if (!err) return;
    err->kind = kind;
    err->trap = trap;
    size_t n = 0;
    for (; a[n] && n < sizeof(err->text) - 1; n++) err->text[n] = a[n];
    if (imp) {
        // «…: модуль.поле» — чтобы было видно, какого импорта не хватило.
        const wasm_name_t* part[2] = { &imp->module, &imp->field };
        if (n < sizeof(err->text) - 3) { err->text[n++] = ':'; err->text[n++] = ' '; }
        for (int k = 0; k < 2; k++) {
            if (k && n < sizeof(err->text) - 1) err->text[n++] = '.';
            for (uint32_t i = 0; i < part[k]->n && n < sizeof(err->text) - 1; i++) {
                uint8_t c = part[k]->p[i];
                err->text[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
            }
        }
    }
    err->text[n] = 0;
}

static bool limits_fit(uint32_t have_min, uint32_t have_max, bool have_has_max, const wasm_limits_t* want) {
    // Импортёр требует «не меньше min и, если он назвал max, не больше max».
    if (have_min < want->min) return false;
    if (want->has_max && (!have_has_max || have_max > want->max)) return false;
    return true;
}

static wasm_val_t eval_cexpr(const wasm_instance_t* inst, const wasm_cexpr_t* cx) {
    switch (cx->kind) {
    case WCX_GLOBAL:   return inst->globals[cx->bits]->value;
    case WCX_REF_FUNC: return (wasm_val_t)(uintptr_t)inst->funcs[cx->bits];
    case WCX_REF_NULL: return 0;
    default:           return cx->bits;
    }
}

wasm_instance_t* wasm_instantiate(wasm_store_t* s, const wasm_module_t* m, wasm_resolve_fn resolve,
                                  void* ctx, wasm_inst_err_t* err) {
    if (err) { err->kind = WASM_INST_OK; err->trap = 0; err->text[0] = 0; }
    wasm_instance_t* inst = (wasm_instance_t*)s_alloc(s, sizeof(*inst));
    if (!inst) { err_set(err, WASM_INST_NOMEM, 0, "out of memory", 0); return 0; }
    inst->store = s;
    inst->module = m;
    inst->funcs   = (wasm_funcinst_t**)s_alloc(s, ((uint64_t)m->n_funcs + 1) * sizeof(void*));
    inst->tables  = (wasm_table_t**)s_alloc(s, ((uint64_t)m->n_tables + 1) * sizeof(void*));
    inst->globals = (wasm_globalinst_t**)s_alloc(s, ((uint64_t)m->n_globals + 1) * sizeof(void*));
    inst->canon   = (uint32_t*)s_alloc(s, ((uint64_t)m->n_types + 1) * sizeof(uint32_t));
    inst->elem_dropped = (uint8_t*)s_alloc(s, (uint64_t)m->n_elems + 1);
    inst->data_dropped = (uint8_t*)s_alloc(s, (uint64_t)m->n_datas + 1);
    wasm_funcinst_t* own = (wasm_funcinst_t*)s_alloc(s, ((uint64_t)(m->n_funcs - m->n_imp_funcs) + 1) * sizeof(wasm_funcinst_t));
    if (!inst->funcs || !inst->tables || !inst->globals || !inst->canon || !inst->elem_dropped || !inst->data_dropped || !own) {
        err_set(err, WASM_INST_NOMEM, 0, "out of memory", 0);
        return 0;
    }
    for (uint32_t i = 0; i < m->n_types; i++) {
        inst->canon[i] = wasm_canon_type(s, &m->types[i]);
        if (inst->canon[i] == 0xFFFFFFFFu) { err_set(err, WASM_INST_NOMEM, 0, "out of memory", 0); return 0; }
    }

    // 1. Импорты: окружение даёт объект, мы сверяем его с тем, что модуль заявил.
    for (uint32_t i = 0; i < m->n_imports; i++) {
        const wasm_import_t* im = &m->imports[i];
        wasm_extern_t ex = { 0, 0 };
        if (!resolve || !resolve(ctx, m, im, &ex) || !ex.ptr) { err_set(err, WASM_INST_LINK, 0, "unknown import", im); return 0; }
        if (ex.kind != im->kind) { err_set(err, WASM_INST_LINK, 0, "incompatible import type", im); return 0; }
        switch (im->kind) {
        case WK_FUNC: {
            wasm_funcinst_t* f = (wasm_funcinst_t*)ex.ptr;
            if (f->canon != inst->canon[m->funcs[im->index].type]) { err_set(err, WASM_INST_LINK, 0, "incompatible import type", im); return 0; }
            inst->funcs[im->index] = f;
            break;
        }
        case WK_TABLE: {
            wasm_table_t* t = (wasm_table_t*)ex.ptr;
            const wasm_tabletype_t* tt = &m->tables[im->index];
            // Сверяется максимум, ЗАЯВЛЕННЫЙ таблицей, а не наш внутренний предел.
            if (t->type != tt->elem || !limits_fit(t->size, t->decl_max, t->has_max, &tt->lim)) { err_set(err, WASM_INST_LINK, 0, "incompatible import type", im); return 0; }
            inst->tables[im->index] = t;
            break;
        }
        case WK_MEM: {
            wasm_memory_t* mem = (wasm_memory_t*)ex.ptr;
            if (!limits_fit(mem->pages, mem->decl_max, mem->has_max, &m->mems[0].lim)) { err_set(err, WASM_INST_LINK, 0, "incompatible import type", im); return 0; }
            inst->mem = mem;
            break;
        }
        default: {
            wasm_globalinst_t* g = (wasm_globalinst_t*)ex.ptr;
            const wasm_global_t* gt = &m->globals[im->index];
            if (g->type != gt->type || g->mut != gt->mut) { err_set(err, WASM_INST_LINK, 0, "incompatible import type", im); return 0; }
            inst->globals[im->index] = g;
            break;
        }
        }
    }

    // 2. Свои функции, таблицы, память, глобальные.
    for (uint32_t i = m->n_imp_funcs; i < m->n_funcs; i++) {
        const wasm_func_t* d = &m->funcs[i];
        wasm_funcinst_t* f = &own[i - m->n_imp_funcs];
        f->type = &m->types[d->type];
        f->canon = inst->canon[d->type];
        f->n_params = f->type->n_params;
        f->n_results = f->type->n_results;
        f->n_locals = d->n_locals;
        f->max_stack = d->max_stack;
        f->code = d->ir;
        f->inst = inst;
        inst->funcs[i] = f;
    }
    for (uint32_t i = m->n_imp_tables; i < m->n_tables; i++) {
        const wasm_tabletype_t* tt = &m->tables[i];
        inst->tables[i] = wasm_table_new(s, tt->elem, tt->lim.min, tt->lim.max, tt->lim.has_max);
        if (!inst->tables[i]) { err_set(err, WASM_INST_NOMEM, 0, "table is larger than this engine allows", 0); return 0; }
    }
    if (m->n_mems && !m->mems[0].imported) {
        const wasm_limits_t* l = &m->mems[0].lim;
        inst->mem = wasm_memory_new(s, l->min, l->max, l->has_max);
        if (!inst->mem) { err_set(err, WASM_INST_NOMEM, 0, "memory is larger than this engine allows", 0); return 0; }
    }
    for (uint32_t i = m->n_imp_globals; i < m->n_globals; i++) {
        const wasm_global_t* g = &m->globals[i];
        inst->globals[i] = wasm_global_new(s, g->type, g->mut, eval_cexpr(inst, &g->init));
        if (!inst->globals[i]) { err_set(err, WASM_INST_NOMEM, 0, "out of memory", 0); return 0; }
    }

    // [51.5] Машинный код — ДО сегментов: функция модуля, чей запуск потом
    // прервётся, могла уже попасть в общую таблицу, и вызвать её должно быть
    // можно. Не вышло (нет памяти) — хранилище остаётся на интерпретаторе.
    (void)wasm_jit_compile(s, inst);

    // 3. Сегменты элементов и данных — по порядку. Сегмент, не поместившийся
    //    в таблицу или память, прерывает запуск, но записанное до него
    //    остаётся (в общей таблице его увидят другие экземпляры).
    for (uint32_t i = 0; i < m->n_elems; i++) {
        const wasm_elem_t* e = &m->elems[i];
        if (e->mode == WSEG_ACTIVE) {
            wasm_table_t* t = inst->tables[e->table];
            uint64_t off = (uint32_t)eval_cexpr(inst, &e->offset);
            if (off + e->n > t->size) { err_set(err, WASM_INST_TRAP, WASM_TRAP_TABLE_OOB, "out of bounds table access", 0); return 0; }
            for (uint32_t k = 0; k < e->n; k++) {
                uint32_t it = e->items[k];
                t->elems[off + k] = it == WASM_ELEM_NULL ? 0
                                  : (it & WASM_ELEM_GLOBAL) ? inst->globals[it & ~WASM_ELEM_GLOBAL]->value
                                  : (wasm_val_t)(uintptr_t)inst->funcs[it];
            }
        }
        // Активный и описательный сегменты после запуска недоступны.
        if (e->mode != WSEG_PASSIVE) inst->elem_dropped[i] = 1;
    }
    for (uint32_t i = 0; i < m->n_datas; i++) {
        const wasm_data_t* d = &m->datas[i];
        if (d->mode != WSEG_ACTIVE) continue;
        uint64_t off = (uint32_t)eval_cexpr(inst, &d->offset);
        if (off + d->n > inst->mem->bytes) { err_set(err, WASM_INST_TRAP, WASM_TRAP_MEM_OOB, "out of bounds memory access", 0); return 0; }
        for (uint32_t k = 0; k < d->n; k++) inst->mem->data[off + k] = d->bytes[k];
        inst->data_dropped[i] = 1;
    }

    // 4. Функция start.
    if (m->has_start) {
        int trap = wasm_call(s, inst->funcs[m->start], 0, 0);
        if (trap) { err_set(err, WASM_INST_TRAP, trap, wasm_trap_name(trap), 0); return 0; }
    }
    return inst;
}

bool wasm_instance_export(const wasm_instance_t* inst, const uint8_t* name, uint32_t n, wasm_extern_t* out) {
    const wasm_module_t* m = inst->module;
    for (uint32_t i = 0; i < m->n_exports; i++) {
        const wasm_export_t* e = &m->exports[i];
        if (e->name.n != n) continue;
        uint32_t k = 0;
        while (k < n && e->name.p[k] == name[k]) k++;
        if (k != n) continue;
        out->kind = e->kind;
        out->ptr = e->kind == WK_FUNC  ? (void*)inst->funcs[e->index]
                 : e->kind == WK_TABLE ? (void*)inst->tables[e->index]
                 : e->kind == WK_MEM   ? (void*)inst->mem
                 :                       (void*)inst->globals[e->index];
        return true;
    }
    return false;
}

// Итог отрезка: пауза — вызов остаётся открытым; иначе он закрыт, и при
// успехе результаты переносятся вызывающему.
static int call_done(wasm_store_t* s, int trap, wasm_val_t* results) {
    if (trap < 0) { s->paused = true; return trap; }
    s->paused = false;
    s->running = false;
    if (!trap && results) for (uint32_t i = 0; i < s->r_top->n_results; i++) results[i] = s->stack[i];
    return trap;
}

int wasm_call(wasm_store_t* s, wasm_funcinst_t* f, const wasm_val_t* args, wasm_val_t* results) {
    // Один стек на хранилище: вызов из функции хоста обратно в модуль затёр бы
    // его. Пока такой путь никому не нужен, он запрещён явно.
    if (s->running) return WASM_TRAP_HOST;
    if (f->n_params > s->cfg.stack_slots || f->n_results > s->cfg.stack_slots) return WASM_TRAP_STACK;
    for (uint32_t i = 0; i < f->n_params; i++) s->stack[i] = args[i];
    if (!f->code) {
        int trap = f->host(0, s->stack, f->user);
        if (trap < 0) trap = WASM_TRAP_HOST;        // «повтори позже» некому повторять
        if (!trap && results) for (uint32_t i = 0; i < f->n_results; i++) results[i] = s->stack[i];
        return trap;
    }
    s->running = true;
    s->r_top = f;
    s->slice_left = s->slice_len;
    // [51.5] Машинный код — если он есть у всего хранилища; иначе интерпретатор.
    s->jit_run = s->jit && !s->jit_off && f->jit;
    return call_done(s, s->jit_run ? wasm_jit_run(s, f) : wasm_interp(s, f), results);
}

int wasm_resume(wasm_store_t* s, wasm_val_t* results) {
    if (!s->paused) return WASM_TRAP_HOST;
    s->slice_left = s->slice_len;
    return call_done(s, s->jit_run ? wasm_jit_run(s, 0) : wasm_interp(s, 0), results);
}

// Одна привилегированная команда прерванного вызова — на том ядре, где ей
// можно исполняться. Отрезки на это время не считаются.
int wasm_service(wasm_store_t* s, wasm_val_t* results) {
    if (!s->paused) return WASM_TRAP_HOST;
    s->svc_once = true;
    s->slice_left = 0;
    int trap = wasm_interp(s, 0);
    s->svc_once = false;
    return call_done(s, trap, results);
}

void wasm_abort(wasm_store_t* s) {
    s->paused = false;
    s->running = false;
}

void wasm_store_remote(wasm_store_t* s, bool remote) { s->remote = remote; }
void wasm_store_slice(wasm_store_t* s, uint32_t units, uint32_t tick_unit) {
    s->slice_len = units;
    s->tick_unit = tick_unit ? tick_unit : 256;
}
bool wasm_store_paused(const wasm_store_t* s) { return s->paused; }

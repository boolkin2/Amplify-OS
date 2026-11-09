// ============================================================================
// wasm_spec.c — прогон официального набора тестов WebAssembly и описание
// модуля словами (этапы 51.0, 51.1). См. wasm.h.
// ----------------------------------------------------------------------------
// Набор тестов (WebAssembly/spec, test/core, тег wg-2.0) написан в текстовом
// формате .wast: модули вперемежку с утверждениями «этот модуль неверен»,
// «вызов f(1) возвращает 2», «вызов g() прерывается». Разбирать текстовый
// формат в ядре незачем: на узле `wast2json` (wabt) превращает каждый файл в
// двоичные модули и список команд, а tools/wasm_pack_spec.py склеивает всё в
// один файл-пакет. Его и читает этот код — один и тот же в ядре (`rune spec`)
// и на хосте (tools/wasm_host.c): так цифры «прошло / не прошло» относятся к
// тому самому движку, который работает в ОС, а не к его родственнику.
//
// Формат пакета (числа — младшим байтом вперёд):
//   "AWSP", u32 версия = 1, u32 число записей;
//   запись: u8 вид, u32 строка исходного .wast, u32 длина, данные.
// Запись можно пропустить по длине, не понимая её.
// Содержимое записей о вызовах:
//   вызов: u8 вид (0 — вызвать, 1 — прочитать глобальную), имя модуля,
//          имя экспорта, u16 число аргументов, аргументы;
//   значение: u8 тип, u8 способ сравнения (0 — биты, 1 — канонический NaN,
//          2 — арифметический NaN, 3 — пустая ссылка), u64 биты;
//   имя и текст: u16 длина + байты.
// ============================================================================
#include "wasm_int.h"

enum {
    REC_FILE = 1,               // имя файла .wast — дальше записи из него
    REC_MODULE,                 // верный модуль: должен загрузиться
    REC_ASSERT_MALFORMED,       // должен быть отклонён как нераспознаваемый
    REC_ASSERT_INVALID,         // должен быть отклонён проверкой типов
    REC_ASSERT_UNLINKABLE,      // импорт не сходится (51.1)
    REC_ASSERT_UNINSTANTIABLE,  // прерывается при запуске (51.1)
    REC_REGISTER,               // дать модулю имя для импорта другими (51.1)
    REC_ACTION,                 // вызвать функцию (51.1)
    REC_ASSERT_RETURN,
    REC_ASSERT_TRAP,
    REC_ASSERT_EXHAUSTION,
};

typedef struct { const uint8_t* p; const uint8_t* end; bool bad; } prd_t;

static uint32_t p_u8(prd_t* r)  { if (r->end - r->p < 1) { r->bad = true; return 0; } return *r->p++; }
static uint32_t p_u16(prd_t* r) { if (r->end - r->p < 2) { r->bad = true; return 0; } uint32_t v = r->p[0] | (r->p[1] << 8); r->p += 2; return v; }
static uint32_t p_u32(prd_t* r) {
    if (r->end - r->p < 4) { r->bad = true; return 0; }
    uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) | ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return v;
}
static const uint8_t* p_bytes(prd_t* r, uint32_t n) {
    if ((uint64_t)(r->end - r->p) < n) { r->bad = true; return r->p; }
    const uint8_t* p = r->p;
    r->p += n;
    return p;
}

// ---- сборка строк отчёта (без libc) ------------------------------------------------
typedef struct { char s[320]; int n; } line_t;

static void l_s(line_t* l, const char* s) {
    while (*s && l->n < (int)sizeof(l->s) - 1) l->s[l->n++] = *s++;
    l->s[l->n] = 0;
}
static void l_sn(line_t* l, const uint8_t* s, uint32_t n) {
    for (uint32_t i = 0; i < n && l->n < (int)sizeof(l->s) - 1; i++) {
        char c = (char)s[i];
        l->s[l->n++] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    l->s[l->n] = 0;
}
static void l_u(line_t* l, uint64_t v) {
    char b[24];
    int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && l->n < (int)sizeof(l->s) - 1) l->s[l->n++] = b[--n];
    l->s[l->n] = 0;
}
static void l_frac(line_t* l, uint32_t ok, uint32_t all) { l_u(l, ok); l_s(l, "/"); l_u(l, all); }

static void l_hex(line_t* l, uint64_t v) {
    static const char d[] = "0123456789abcdef";
    l_s(l, "0x");
    bool on = false;
    for (int sh = 60; sh >= 0; sh -= 4) {
        uint32_t x = (uint32_t)(v >> sh) & 15;
        if (x || on || sh == 0) { char c[2] = { d[x], 0 }; l_s(l, c); on = true; }
    }
}

// ----------------------------------------------------------------------------
// [51.1] Окружение одного файла .wast.
// ----------------------------------------------------------------------------
// Файл — самостоятельный сценарий: модули в нём импортируют друг у друга, но
// не у соседних файлов. Поэтому на файл заводится своё хранилище, а в конце
// файла оно освобождается целиком вместе с модулями.
//
// Модули файла освобождаются только В КОНЦЕ файла, даже те, чей запуск
// прервался: сегмент элементов мог успеть положить в чужую (импортированную)
// таблицу ссылку на функцию такого модуля, и тесты эту функцию потом вызывают.
//
// Имён у экземпляра может быть два, и это разные пространства: имя в сценарии
// («$M», им пользуются вызовы) и имя для импорта (команда register).
// ----------------------------------------------------------------------------
#define SPEC_MAX_NAMES 128
#define SPEC_MAX_VALS  64                       // аргументов или результатов в одном вызове
// На один вызов — около миллиона витков циклов и вызовов. Самому набору
// хватает шестнадцатой части (больше всего тратят тесты бесконечной рекурсии:
// 16 384 кадра до отказа). Запас нельзя делать огромным: если движок сломан и
// цикл теста не кончается, каждый такой вызов сгорает до предела — а тест
// обязан провалиться быстро, а не висеть минутами.
#define SPEC_FUEL      (1 << 12)

enum { VF_EXACT = 0, VF_NAN_CANON, VF_NAN_ARITH, VF_REF_NULL };

typedef struct { const uint8_t* p; uint32_t n; wasm_instance_t* inst; bool reg; } named_t;

typedef struct {
    wasm_spec_stats_t s;                // счёт по текущему файлу
    const uint8_t*    name;
    uint32_t          name_n;
    bool              open;

    wasm_store_t*     store;
    bool              store_failed;
    wasm_instance_t*  cur;              // последний запущенный модуль — к нему идут вызовы без имени
    wasm_module_t**   mods;
    uint32_t          n_mods, cap_mods;
    named_t           names[SPEC_MAX_NAMES];
    uint32_t          n_names;
    // Модуль «spectest», который обязано дать окружение тестов.
    wasm_funcinst_t*   st_funcs[7];
    wasm_globalinst_t* st_globals[4];
    wasm_table_t*      st_table;
    wasm_memory_t*     st_mem;
} file_t;

static const struct { const char* name; uint8_t n; uint8_t p[2]; } k_st_funcs[7] = {
    { "print",         0, { 0, 0 } },
    { "print_i32",     1, { WT_I32, 0 } },
    { "print_i64",     1, { WT_I64, 0 } },
    { "print_f32",     1, { WT_F32, 0 } },
    { "print_f64",     1, { WT_F64, 0 } },
    { "print_i32_f32", 2, { WT_I32, WT_F32 } },
    { "print_f64_f64", 2, { WT_F64, WT_F64 } },
};
static const char* const k_st_globals[4] = { "global_i32", "global_i64", "global_f32", "global_f64" };

static int st_print(wasm_instance_t* caller, wasm_val_t* vals, void* user) {
    (void)caller; (void)vals; (void)user;       // печатать нечего и некуда: важно лишь, что вызов состоялся
    return WASM_TRAP_NONE;
}

static bool name_is(const uint8_t* p, uint32_t n, const char* s) {
    uint32_t i = 0;
    for (; i < n && s[i]; i++) if (p[i] != (uint8_t)s[i]) return false;
    return i == n && !s[i];
}

static bool env_open(file_t* f) {
    if (f->store) return true;
    if (f->store_failed) return false;
    f->store = wasm_store_new(0);
    if (!f->store) { f->store_failed = true; return false; }
    bool ok = true;
    for (int i = 0; i < 7; i++) {
        f->st_funcs[i] = wasm_host_func(f->store, k_st_funcs[i].p, k_st_funcs[i].n, 0, 0, st_print, 0);
        ok = ok && f->st_funcs[i];
    }
    union { float f; uint32_t u; } f32 = { 666.6f };
    union { double d; uint64_t u; } f64 = { 666.6 };
    f->st_globals[0] = wasm_global_new(f->store, WT_I32, false, 666);
    f->st_globals[1] = wasm_global_new(f->store, WT_I64, false, 666);
    f->st_globals[2] = wasm_global_new(f->store, WT_F32, false, f32.u);
    f->st_globals[3] = wasm_global_new(f->store, WT_F64, false, f64.u);
    f->st_table = wasm_table_new(f->store, WT_FUNCREF, 10, 20, true);
    f->st_mem = wasm_memory_new(f->store, 1, 2, true);
    ok = ok && f->st_globals[0] && f->st_globals[1] && f->st_globals[2] && f->st_globals[3] && f->st_table && f->st_mem;
    if (!ok) { wasm_store_free(f->store); f->store = 0; f->store_failed = true; }
    return ok;
}

static void env_close(file_t* f) {
    if (f->store) wasm_store_free(f->store);
    for (uint32_t i = 0; i < f->n_mods; i++) wasm_module_free(f->mods[i]);
    if (f->mods) wasm_port_free(f->mods);
    f->store = 0; f->store_failed = false; f->cur = 0;
    f->mods = 0; f->n_mods = 0; f->cap_mods = 0; f->n_names = 0;
}

static bool keep_module(file_t* f, wasm_module_t* m) {
    if (f->n_mods == f->cap_mods) {
        uint32_t ncap = f->cap_mods ? f->cap_mods * 2 : 64;
        wasm_module_t** nm = (wasm_module_t**)wasm_port_alloc((size_t)ncap * sizeof(*nm));
        if (!nm) return false;
        for (uint32_t i = 0; i < f->n_mods; i++) nm[i] = f->mods[i];
        if (f->mods) wasm_port_free(f->mods);
        f->mods = nm; f->cap_mods = ncap;
    }
    f->mods[f->n_mods++] = m;
    return true;
}

// Последнее имя побеждает: сценарии переопределяют и «$M», и имена для импорта.
static wasm_instance_t* find_name(const file_t* f, const uint8_t* p, uint32_t n, bool reg) {
    for (uint32_t i = f->n_names; i > 0; i--) {
        const named_t* e = &f->names[i - 1];
        if (e->reg != reg || e->n != n) continue;
        uint32_t k = 0;
        while (k < n && e->p[k] == p[k]) k++;
        if (k == n) return e->inst;
    }
    return 0;
}

static bool add_name(file_t* f, const uint8_t* p, uint32_t n, wasm_instance_t* inst, bool reg) {
    if (f->n_names >= SPEC_MAX_NAMES) return false;
    named_t* e = &f->names[f->n_names++];
    e->p = p; e->n = n; e->inst = inst; e->reg = reg;
    return true;
}

static bool spec_resolve(void* ctx, const wasm_module_t* m, const wasm_import_t* imp, wasm_extern_t* out) {
    (void)m;
    file_t* f = (file_t*)ctx;
    // Имя «spectest» сценарий вправе занять своим модулем — тогда берём его.
    wasm_instance_t* inst = find_name(f, imp->module.p, imp->module.n, true);
    if (inst) return wasm_instance_export(inst, imp->field.p, imp->field.n, out);
    if (!name_is(imp->module.p, imp->module.n, "spectest")) return false;
    for (int i = 0; i < 7; i++)
        if (name_is(imp->field.p, imp->field.n, k_st_funcs[i].name)) { out->kind = WK_FUNC; out->ptr = f->st_funcs[i]; return true; }
    for (int i = 0; i < 4; i++)
        if (name_is(imp->field.p, imp->field.n, k_st_globals[i])) { out->kind = WK_GLOBAL; out->ptr = f->st_globals[i]; return true; }
    if (name_is(imp->field.p, imp->field.n, "table"))  { out->kind = WK_TABLE; out->ptr = f->st_table; return true; }
    if (name_is(imp->field.p, imp->field.n, "memory")) { out->kind = WK_MEM;   out->ptr = f->st_mem;   return true; }
    return false;
}

static void exec_line(line_t* l, const file_t* f) {
    l_s(l, ", returns "); l_frac(l, f->s.returns_ok, f->s.returns);
    l_s(l, ", traps "); l_frac(l, f->s.traps_ok, f->s.traps);
    if (f->s.exhaust)    { l_s(l, ", exhaustion "); l_frac(l, f->s.exhaust_ok, f->s.exhaust); }
    if (f->s.unlinkable) { l_s(l, ", unlinkable "); l_frac(l, f->s.unlinkable_ok, f->s.unlinkable); }
    if (f->s.uninst)     { l_s(l, ", uninstantiable "); l_frac(l, f->s.uninst_ok, f->s.uninst); }
    if (f->s.actions)    { l_s(l, ", actions "); l_frac(l, f->s.actions_ok, f->s.actions); }
    if (f->s.skipped_exec) { l_s(l, ", not run "); l_u(l, f->s.skipped_exec); }
}

static void file_close(file_t* f, wasm_sink_t out, bool verbose, wasm_spec_stats_t* total) {
    if (!f->open) return;
    f->open = false;
    env_close(f);
    total->files++;
    total->modules += f->s.modules;           total->modules_ok += f->s.modules_ok;
    total->invalid += f->s.invalid;           total->invalid_ok += f->s.invalid_ok;
    total->malformed += f->s.malformed;       total->malformed_ok += f->s.malformed_ok;
    total->skipped_text += f->s.skipped_text; total->skipped_exec += f->s.skipped_exec;
    total->returns += f->s.returns;           total->returns_ok += f->s.returns_ok;
    total->traps += f->s.traps;               total->traps_ok += f->s.traps_ok;
    total->exhaust += f->s.exhaust;           total->exhaust_ok += f->s.exhaust_ok;
    total->unlinkable += f->s.unlinkable;     total->unlinkable_ok += f->s.unlinkable_ok;
    total->uninst += f->s.uninst;             total->uninst_ok += f->s.uninst_ok;
    total->actions += f->s.actions;           total->actions_ok += f->s.actions_ok;
    total->failures += f->s.failures;
    if (!verbose && !f->s.failures) return;
    line_t l; l.n = 0;
    l_s(&l, "[wasm-spec] "); l_sn(&l, f->name, f->name_n);
    l_s(&l, ": modules "); l_frac(&l, f->s.modules_ok, f->s.modules);
    l_s(&l, ", invalid rejected "); l_frac(&l, f->s.invalid_ok, f->s.invalid);
    l_s(&l, ", malformed rejected "); l_frac(&l, f->s.malformed_ok, f->s.malformed);
    exec_line(&l, f);
    if (f->s.skipped_text) { l_s(&l, ", text skipped "); l_u(&l, f->s.skipped_text); }
    if (f->s.failures) { l_s(&l, ", FAILURES "); l_u(&l, f->s.failures); }
    l_s(&l, "\n");
    out(l.s);
}

static void report_fail(file_t* f, wasm_sink_t out, uint32_t line, const char* what,
                        const uint8_t* text, uint32_t text_n, const wasm_err_t* err) {
    f->s.failures++;
    if (f->s.failures > 8) return;      // по файлу — первые восемь, остальное в счётчике
    line_t l; l.n = 0;
    l_s(&l, "[wasm-spec] FAIL "); l_sn(&l, f->name, f->name_n); l_s(&l, ":"); l_u(&l, line);
    l_s(&l, ": "); l_s(&l, what);
    if (text_n) { l_s(&l, " (test expects: "); l_sn(&l, text, text_n); l_s(&l, ")"); }
    if (err && err->kind) {
        l_s(&l, " - our verdict: "); l_s(&l, wasm_err_name(err->kind)); l_s(&l, ", ");
        l_s(&l, err->text); l_s(&l, " @"); l_u(&l, err->offset);
    }
    l_s(&l, "\n");
    out(l.s);
}

// Провал проверки исполнения: что ждали и что получили — уже собрано в detail.
static void report_exec(file_t* f, wasm_sink_t out, uint32_t line, const char* what, const line_t* detail) {
    f->s.failures++;
    if (f->s.failures > 8) return;
    line_t l; l.n = 0;
    l_s(&l, "[wasm-spec] FAIL "); l_sn(&l, f->name, f->name_n); l_s(&l, ":"); l_u(&l, line);
    l_s(&l, ": "); l_s(&l, what);
    if (detail && detail->n) { l_s(&l, " - "); l_s(&l, detail->s); }
    l_s(&l, "\n");
    out(l.s);
}

// ---- вызов из сценария -------------------------------------------------------------
typedef struct {
    const char* why;                    // не 0 — вызов не состоялся (нет экземпляра, нет экспорта…)
    bool        quiet;                  // … и причина уже названа раньше (модуль не запустился)
    int         trap;
    uint32_t    n;                      // сколько значений получено
    const uint8_t* field; uint32_t field_n;
} act_t;

static bool g_stepped;
void wasm_spec_set_stepped(bool on) { g_stepped = on; }

// Буферы вызова — статические: функция не вложенная, а на стеке ядра им тесно.
static wasm_val_t g_args[SPEC_MAX_VALS];
static wasm_val_t g_res[SPEC_MAX_VALS];
static uint8_t    g_res_type[SPEC_MAX_VALS];

// Значение из пакета -> ячейка движка.
static wasm_val_t val_in(uint8_t type, uint8_t flag, uint64_t bits) {
    if (type == WT_I32 || type == WT_F32) return (uint32_t)bits;
    if (type == WT_EXTERNREF) return flag == VF_REF_NULL ? 0 : wasm_externref(bits);
    if (type == WT_FUNCREF) return 0;                   // сценарий умеет передать только пустую ссылку
    return bits;
}

static uint64_t p_u64(prd_t* r) { uint64_t lo = p_u32(r); uint64_t hi = p_u32(r); return lo | (hi << 32); }

static void run_action(file_t* f, prd_t* b, act_t* a) {
    a->why = 0; a->quiet = false; a->trap = WASM_TRAP_NONE; a->n = 0;
    uint32_t kind = p_u8(b);
    uint32_t mn = p_u16(b);
    const uint8_t* mname = p_bytes(b, mn);
    uint32_t fn = p_u16(b);
    const uint8_t* fname = p_bytes(b, fn);
    uint32_t nargs = p_u16(b);
    a->field = fname; a->field_n = fn;
    if (nargs > SPEC_MAX_VALS) { for (uint32_t i = 0; i < nargs; i++) (void)p_bytes(b, 10); a->why = "too many arguments for the test runner"; return; }
    for (uint32_t i = 0; i < nargs; i++) {
        uint8_t type = (uint8_t)p_u8(b), flag = (uint8_t)p_u8(b);
        g_args[i] = val_in(type, flag, p_u64(b));
    }
    if (b->bad) { a->why = "damaged record"; return; }

    wasm_instance_t* inst = mn ? find_name(f, mname, mn, false) : f->cur;
    if (!inst) { a->why = "no module instance to act on"; a->quiet = true; return; }
    wasm_extern_t ex;
    if (!wasm_instance_export(inst, fname, fn, &ex)) { a->why = "no such export"; return; }
    if (kind == 1) {                                    // прочитать глобальную
        if (ex.kind != WK_GLOBAL) { a->why = "export is not a global"; return; }
        g_res[0] = wasm_global_get((wasm_globalinst_t*)ex.ptr, &g_res_type[0]);
        a->n = 1;
        return;
    }
    if (ex.kind != WK_FUNC) { a->why = "export is not a function"; return; }
    wasm_funcinst_t* fi = (wasm_funcinst_t*)ex.ptr;
    const wasm_functype_t* ft = wasm_func_type(fi);
    if (ft->n_params != nargs) { a->why = "wrong number of arguments"; return; }
    if (ft->n_results > SPEC_MAX_VALS) { a->why = "too many results for the test runner"; return; }
    if (!g_stepped) {
        wasm_store_limit(f->store, SPEC_FUEL, 0);
        a->trap = wasm_call(f->store, fi, g_args, g_res);
    } else {
        // Единица счёта здесь — один виток, а не 256: «топлива» даём во столько же раз больше.
        wasm_store_limit(f->store, (int64_t)SPEC_FUEL * 256, 0);
        wasm_store_remote(f->store, true);
        wasm_store_slice(f->store, 1, 1);
        a->trap = wasm_call(f->store, fi, g_args, g_res);
        while (a->trap < 0)
            a->trap = a->trap == WASM_PAUSED_SERVICE ? wasm_service(f->store, g_res) : wasm_resume(f->store, g_res);
        wasm_store_remote(f->store, false);
        wasm_store_slice(f->store, 0, 0);
    }
    if (a->trap) return;
    a->n = ft->n_results;
    for (uint32_t i = 0; i < a->n; i++) g_res_type[i] = ft->types[ft->n_params + i];
}

// Сошлось ли полученное значение с ожидаемым. У NaN тесты задают не биты, а
// класс: «канонический» (только старший бит мантиссы) или «арифметический»
// (старший бит мантиссы есть, остальные — любые).
static bool val_matches(uint8_t type, uint8_t flag, uint64_t want, wasm_val_t got) {
    switch (type) {
    case WT_I32: return (uint32_t)got == (uint32_t)want;
    case WT_I64: return got == want;
    case WT_F32: {
        uint32_t g = (uint32_t)got;
        if (flag == VF_NAN_CANON) return (g & 0x7FFFFFFFu) == 0x7FC00000u;
        if (flag == VF_NAN_ARITH) return (g & 0x7FC00000u) == 0x7FC00000u;
        return g == (uint32_t)want;
    }
    case WT_F64:
        if (flag == VF_NAN_CANON) return (got & 0x7FFFFFFFFFFFFFFFull) == 0x7FF8000000000000ull;
        if (flag == VF_NAN_ARITH) return (got & 0x7FF8000000000000ull) == 0x7FF8000000000000ull;
        return got == want;
    case WT_EXTERNREF: return flag == VF_REF_NULL ? got == 0 : got == wasm_externref(want);
    case WT_FUNCREF:   return flag == VF_REF_NULL ? got == 0 : got != 0;    // «какая-то функция»
    default: return false;
    }
}

static bool starts_with(const uint8_t* text, uint32_t n, const char* prefix) {
    uint32_t i = 0;
    for (; prefix[i]; i++) if (i >= n || text[i] != (uint8_t)prefix[i]) return false;
    return true;
}

static void l_field(line_t* l, const act_t* a) { l_s(l, "\""); l_sn(l, a->field, a->field_n); l_s(l, "\""); }

// Запись с модулем: [u16 имя][u32 длина][wasm] либо [u8 текст?][u16 пояснение][u32 длина][wasm].
// Загрузить и запустить. Возвращает экземпляр; модуль остаётся в списке файла.
static wasm_instance_t* load_and_start(file_t* f, const uint8_t* w, uint32_t wl, wasm_err_t* err,
                                       wasm_inst_err_t* ierr, bool* loaded) {
    *loaded = false;
    ierr->kind = WASM_INST_OK; ierr->trap = 0; ierr->text[0] = 0;
    wasm_module_t* m = wasm_module_load(w, wl, err);
    if (!m) return 0;
    *loaded = true;
    if (!keep_module(f, m)) { wasm_module_free(m); ierr->kind = WASM_INST_NOMEM; return 0; }
    if (!env_open(f)) { ierr->kind = WASM_INST_NOMEM; return 0; }
    wasm_store_limit(f->store, SPEC_FUEL, 0);           // функция start тоже под пределом
    return wasm_instantiate(f->store, m, spec_resolve, f, ierr);
}

bool wasm_spec_run(const uint8_t* pack, uint64_t len, wasm_sink_t out, bool verbose, wasm_spec_stats_t* st) {
    for (size_t i = 0; i < sizeof(*st); i++) ((uint8_t*)st)[i] = 0;
    prd_t r = { pack, pack + len, false };
    const uint8_t* magic = p_bytes(&r, 4);
    uint32_t version = p_u32(&r), n_rec = p_u32(&r);
    if (r.bad || magic[0] != 'A' || magic[1] != 'W' || magic[2] != 'S' || magic[3] != 'P' || version != 1) {
        out("[wasm-spec] not a test pack (bad header)\n");
        return false;
    }
    static file_t f;                    // с таблицей имён — несколько килобайт: не на стек ядра
    for (size_t i = 0; i < sizeof(f); i++) ((uint8_t*)&f)[i] = 0;
    bool damaged = false;

    for (uint32_t i = 0; i < n_rec && !damaged; i++) {
        uint32_t type = p_u8(&r), line = p_u32(&r), plen = p_u32(&r);
        const uint8_t* body = p_bytes(&r, plen);
        if (r.bad) { out("[wasm-spec] pack is truncated\n"); damaged = true; break; }
        prd_t b = { body, body + plen, false };
        line_t d; d.n = 0; d.s[0] = 0;

        if (type == REC_FILE) {
            file_close(&f, out, verbose, st);
            for (size_t k = 0; k < sizeof(f.s); k++) ((uint8_t*)&f.s)[k] = 0;
            f.name = body; f.name_n = plen; f.open = true;
            continue;
        }
        if (!f.open) continue;

        if (type == REC_MODULE) {
            uint32_t nn = p_u16(&b);
            const uint8_t* name = p_bytes(&b, nn);
            uint32_t wl = p_u32(&b);
            const uint8_t* w = p_bytes(&b, wl);
            if (b.bad) { damaged = true; break; }
            f.s.modules++;
            wasm_err_t err;
            wasm_inst_err_t ierr;
            bool loaded;
            f.cur = load_and_start(&f, w, wl, &err, &ierr, &loaded);
            if (!loaded) report_fail(&f, out, line, "valid module rejected", 0, 0, &err);
            else if (!f.cur) {
                l_s(&d, ierr.text[0] ? ierr.text : "out of memory");
                report_exec(&f, out, line, "valid module did not start", &d);
            } else {
                f.s.modules_ok++;
                if (nn && !add_name(&f, name, nn, f.cur, false)) report_exec(&f, out, line, "too many named modules for the test runner", 0);
            }
        } else if (type == REC_ASSERT_MALFORMED || type == REC_ASSERT_INVALID) {
            uint32_t is_text = p_u8(&b);
            uint32_t tn = p_u16(&b);
            const uint8_t* text = p_bytes(&b, tn);
            uint32_t wl = p_u32(&b);
            const uint8_t* w = p_bytes(&b, wl);
            if (b.bad) { damaged = true; break; }
            // Модуль в текстовом виде проверяет разборщик текста — у нас его нет.
            if (is_text) { f.s.skipped_text++; continue; }
            bool mal = type == REC_ASSERT_MALFORMED;
            if (mal) f.s.malformed++; else f.s.invalid++;
            wasm_err_t err;
            wasm_module_t* m = wasm_module_load(w, wl, &err);
            if (!m) { if (mal) f.s.malformed_ok++; else f.s.invalid_ok++; }
            else {
                wasm_module_free(m);
                report_fail(&f, out, line, mal ? "malformed module accepted" : "invalid module accepted", text, tn, 0);
            }
        } else if (type == REC_ASSERT_UNLINKABLE || type == REC_ASSERT_UNINSTANTIABLE) {
            uint32_t is_text = p_u8(&b);
            uint32_t tn = p_u16(&b);
            const uint8_t* text = p_bytes(&b, tn);
            uint32_t wl = p_u32(&b);
            const uint8_t* w = p_bytes(&b, wl);
            if (b.bad) { damaged = true; break; }
            if (is_text) { f.s.skipped_text++; continue; }
            bool link = type == REC_ASSERT_UNLINKABLE;
            if (link) f.s.unlinkable++; else f.s.uninst++;
            wasm_err_t err;
            wasm_inst_err_t ierr;
            bool loaded;
            wasm_instance_t* inst = load_and_start(&f, w, wl, &err, &ierr, &loaded);
            if (!loaded) { report_fail(&f, out, line, "module of a link/start check was rejected by the validator", text, tn, &err); continue; }
            // Причина должна быть та, которую ждёт тест: «не связался» и
            // «прервался при запуске» — разные исходы.
            int want = link ? WASM_INST_LINK : WASM_INST_TRAP;
            if (!inst && ierr.kind == want && (link || starts_with(text, tn, wasm_trap_name(ierr.trap)))) {
                if (link) f.s.unlinkable_ok++; else f.s.uninst_ok++;
            } else {
                l_s(&d, "expected \""); l_sn(&d, text, tn); l_s(&d, "\", got: ");
                l_s(&d, inst ? "module started" : ierr.text[0] ? ierr.text : "out of memory");
                report_exec(&f, out, line, link ? "module should not link" : "module should trap while starting", &d);
            }
        } else if (type == REC_REGISTER) {
            uint32_t an = p_u16(&b);
            const uint8_t* as = p_bytes(&b, an);
            uint32_t nn = p_u16(&b);
            const uint8_t* name = p_bytes(&b, nn);
            if (b.bad) { damaged = true; break; }
            wasm_instance_t* inst = nn ? find_name(&f, name, nn, false) : f.cur;
            if (!inst) { f.s.skipped_exec++; continue; }
            if (!add_name(&f, as, an, inst, true)) report_exec(&f, out, line, "too many registered modules for the test runner", 0);
        } else if (type == REC_ACTION || type == REC_ASSERT_RETURN || type == REC_ASSERT_TRAP || type == REC_ASSERT_EXHAUSTION) {
            act_t a;
            run_action(&f, &b, &a);
            if (b.bad) { damaged = true; break; }
            if (a.why) {
                // Вызывать некого: модуль выше не запустился, и об этом уже сказано.
                f.s.skipped_exec++;
                if (!a.quiet) {
                    l_field(&d, &a); l_s(&d, ": "); l_s(&d, a.why);
                    report_exec(&f, out, line, "action could not run", &d);
                }
                continue;
            }
            if (type == REC_ACTION) {
                f.s.actions++;
                if (!a.trap) f.s.actions_ok++;
                else { l_field(&d, &a); l_s(&d, " trapped: "); l_s(&d, wasm_trap_name(a.trap)); report_exec(&f, out, line, "action", &d); }
            } else if (type == REC_ASSERT_RETURN) {
                f.s.returns++;
                uint32_t n = p_u16(&b);
                bool ok = !a.trap && n == a.n;
                uint32_t bad_at = 0;
                uint64_t want = 0;
                for (uint32_t k = 0; k < n; k++) {
                    uint8_t vt = (uint8_t)p_u8(&b), vf = (uint8_t)p_u8(&b);
                    uint64_t bits = p_u64(&b);
                    if (ok && !val_matches(vt, vf, bits, g_res[k])) { ok = false; bad_at = k; want = bits; }
                }
                if (b.bad) { damaged = true; break; }
                if (ok) f.s.returns_ok++;
                else {
                    l_field(&d, &a);
                    if (a.trap) { l_s(&d, " trapped: "); l_s(&d, wasm_trap_name(a.trap)); }
                    else if (n != a.n) { l_s(&d, " returned "); l_u(&d, a.n); l_s(&d, " values, expected "); l_u(&d, n); }
                    else { l_s(&d, " result "); l_u(&d, bad_at); l_s(&d, ": got "); l_hex(&d, g_res[bad_at]); l_s(&d, ", expected "); l_hex(&d, want); }
                    report_exec(&f, out, line, "wrong result", &d);
                }
            } else {
                bool exh = type == REC_ASSERT_EXHAUSTION;
                if (exh) f.s.exhaust++; else f.s.traps++;
                uint32_t tn = p_u16(&b);
                const uint8_t* text = p_bytes(&b, tn);
                if (b.bad) { damaged = true; break; }
                bool ok = exh ? a.trap == WASM_TRAP_STACK
                              : a.trap > WASM_TRAP_NONE && a.trap <= WASM_TRAP_SIG_MISMATCH && starts_with(text, tn, wasm_trap_name(a.trap));
                if (ok) { if (exh) f.s.exhaust_ok++; else f.s.traps_ok++; }
                else {
                    l_field(&d, &a); l_s(&d, ": expected \""); l_sn(&d, text, tn); l_s(&d, "\", got ");
                    l_s(&d, a.trap ? wasm_trap_name(a.trap) : "a normal return");
                    report_exec(&f, out, line, exh ? "call should exhaust the stack" : "call should trap", &d);
                }
            }
        } else {
            f.s.skipped_exec++;         // вид записи, которого этот движок не знает
        }
    }
    if (damaged) {
        if (!r.bad) out("[wasm-spec] pack record is damaged\n");
        f.open = false;
        env_close(&f);
        return false;
    }
    file_close(&f, out, verbose, st);

    line_t l; l.n = 0;
    l_s(&l, "[wasm-spec] total: files="); l_u(&l, st->files);
    l_s(&l, " modules="); l_frac(&l, st->modules_ok, st->modules);
    l_s(&l, " invalid="); l_frac(&l, st->invalid_ok, st->invalid);
    l_s(&l, " malformed="); l_frac(&l, st->malformed_ok, st->malformed);
    l_s(&l, " text-skipped="); l_u(&l, st->skipped_text);
    l_s(&l, " exec-skipped="); l_u(&l, st->skipped_exec);
    l_s(&l, " failures="); l_u(&l, st->failures);
    l_s(&l, "\n");
    out(l.s);
    l.n = 0;
    l_s(&l, "[wasm-spec] exec: returns="); l_frac(&l, st->returns_ok, st->returns);
    l_s(&l, " traps="); l_frac(&l, st->traps_ok, st->traps);
    l_s(&l, " exhaustion="); l_frac(&l, st->exhaust_ok, st->exhaust);
    l_s(&l, " unlinkable="); l_frac(&l, st->unlinkable_ok, st->unlinkable);
    l_s(&l, " uninstantiable="); l_frac(&l, st->uninst_ok, st->uninst);
    l_s(&l, " actions="); l_frac(&l, st->actions_ok, st->actions);
    l_s(&l, "\n");
    out(l.s);
    return true;
}

// ----------------------------------------------------------------------------
// Описание модуля словами — для `rune info` и tools/wasm_host.
// ----------------------------------------------------------------------------
static void l_sig(line_t* l, const wasm_functype_t* ft) {
    l_s(l, "(");
    for (uint32_t i = 0; i < ft->n_params; i++) { if (i) l_s(l, ", "); l_s(l, wasm_type_name(ft->types[i])); }
    l_s(l, ") -> (");
    for (uint32_t i = 0; i < ft->n_results; i++) { if (i) l_s(l, ", "); l_s(l, wasm_type_name(ft->types[ft->n_params + i])); }
    l_s(l, ")");
}

static void l_limits(line_t* l, const wasm_limits_t* lim) {
    l_u(l, lim->min);
    if (lim->has_max) { l_s(l, ".."); l_u(l, lim->max); } else l_s(l, "..unlimited");
}

void wasm_describe(const wasm_module_t* m, wasm_sink_t out, const char* prefix, uint32_t max_items) {
    line_t l;
    l.n = 0;
    l_s(&l, prefix); l_s(&l, "size="); l_u(&l, m->bin_len);
    l_s(&l, " types="); l_u(&l, m->n_types);
    l_s(&l, " funcs="); l_u(&l, m->n_funcs - m->n_imp_funcs);
    l_s(&l, " imports="); l_u(&l, m->n_imports);
    l_s(&l, " exports="); l_u(&l, m->n_exports);
    l_s(&l, " globals="); l_u(&l, m->n_globals);
    l_s(&l, " tables="); l_u(&l, m->n_tables);
    l_s(&l, " elems="); l_u(&l, m->n_elems);
    l_s(&l, " datas="); l_u(&l, m->n_datas);
    l_s(&l, " code="); l_u(&l, m->code_bytes);
    l_s(&l, " custom="); l_u(&l, m->n_custom);
    l_s(&l, "\n");
    out(l.s);

    l.n = 0;
    l_s(&l, prefix);
    if (m->n_mems) { l_s(&l, "memory pages="); l_limits(&l, &m->mems[0].lim); l_s(&l, m->mems[0].imported ? " (imported)" : ""); }
    else l_s(&l, "memory none");
    uint64_t data = 0;
    for (uint32_t i = 0; i < m->n_datas; i++) data += m->datas[i].n;
    l_s(&l, " data_bytes="); l_u(&l, data);
    uint32_t deepest = 0;
    for (uint32_t i = m->n_imp_funcs; i < m->n_funcs; i++) if (m->funcs[i].max_stack > deepest) deepest = m->funcs[i].max_stack;
    l_s(&l, " max_operand_stack="); l_u(&l, deepest);
    if (m->has_start) { l_s(&l, " start=func"); l_u(&l, m->start); }
    l_s(&l, "\n");
    out(l.s);

    for (uint32_t i = 0; i < m->n_imports && i < max_items; i++) {
        const wasm_import_t* im = &m->imports[i];
        l.n = 0;
        l_s(&l, prefix); l_s(&l, "import "); l_s(&l, wasm_kind_name(im->kind)); l_s(&l, " ");
        l_sn(&l, im->module.p, im->module.n); l_s(&l, "."); l_sn(&l, im->field.p, im->field.n);
        if (im->kind == WK_FUNC) { l_s(&l, " "); l_sig(&l, &m->types[m->funcs[im->index].type]); }
        l_s(&l, "\n");
        out(l.s);
    }
    if (m->n_imports > max_items) {
        l.n = 0; l_s(&l, prefix); l_s(&l, "... and "); l_u(&l, m->n_imports - max_items); l_s(&l, " more imports\n"); out(l.s);
    }
    for (uint32_t i = 0; i < m->n_exports && i < max_items; i++) {
        const wasm_export_t* e = &m->exports[i];
        l.n = 0;
        l_s(&l, prefix); l_s(&l, "export "); l_s(&l, wasm_kind_name(e->kind)); l_s(&l, " ");
        l_sn(&l, e->name.p, e->name.n);
        if (e->kind == WK_FUNC) { l_s(&l, " "); l_sig(&l, &m->types[m->funcs[e->index].type]); }
        l_s(&l, "\n");
        out(l.s);
    }
    if (m->n_exports > max_items) {
        l.n = 0; l_s(&l, prefix); l_s(&l, "... and "); l_u(&l, m->n_exports - max_items); l_s(&l, " more exports\n"); out(l.s);
    }
}

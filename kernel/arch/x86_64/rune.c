// ============================================================================
// rune.c — WebAssembly в AMPLIFY-OS: связка движка с ядром (этап 51). См. rune.h.
// ============================================================================
#include "rune.h"
#include "wasm.h"
#include "wasm_wasi.h"
#include "rtc.h"
#include "crypto.h"
#include "mind.h"
#include "fat32.h"
#include "smp_pool.h"
#include "cpu_stats.h"
#include "hv.h"
#include "vfs.h"
#include "initrd.h"
#include "heap.h"
#include "pmm.h"
#include "timer.h"
#include "serial.h"
#include <stddef.h>

// ---- порт движка: память ----------------------------------------------------------
// Структуры модуля — из кучи ядра: это тысячи мелких блоков, живущих вместе.
void* wasm_port_alloc(size_t n) { return kmalloc(n ? n : 1); }
void  wasm_port_free(void* p)   { if (p) kfree(p); }

// Линейная память программы — десятки мегабайт одним куском: страницы у
// pmm_alloc_big (на машинах с RAM выше 4 ГБ — оттуда), не у кучи ядра
// (она всего 128 МБ и общая для всех). Страницы приходят с прежним
// содержимым, а программа вправе рассчитывать на нули — и не должна увидеть
// чужие данные.
void* wasm_port_mem_alloc(uint64_t bytes) {
    uint64_t pages = (bytes + 4095) / 4096;
    if (!pages) pages = 1;
    uint64_t hpa = pmm_alloc_big(pages);
    if (hpa == PMM_INVALID || !hpa) return 0;
    void* p = (void*)(uintptr_t)hpa;
    void* d = p;
    uint64_t n = pages * 4096;
    __asm__ __volatile__("rep stosb" : "+D"(d), "+c"(n) : "a"(0) : "memory");
    return p;
}
void wasm_port_mem_free(void* p, uint64_t bytes) {
    uint64_t pages = (bytes + 4095) / 4096;
    if (p) pmm_free_big((uint64_t)(uintptr_t)p, pages ? pages : 1);
}

// [51.5] Машинный код JIT. Куча ядра отображена без запрета исполнения, а код
// программы — сотни килобайт: отдельного распределителя ему не нужно.
void* wasm_port_exec_alloc(uint64_t bytes) { return kmalloc(bytes ? bytes : 1); }
void  wasm_port_exec_free(void* p, uint64_t bytes) { (void)bytes; if (p) kfree(p); }

// ---- вывод: то, что говорит rune, должно оказаться и в serial (по нему судят
// тесты), и в окне терминала. Терминал сам дублирует свой вывод в serial,
// поэтому при заданном приёмнике пишем только в него — иначе каждая строка
// легла бы в журнал дважды. Без приёмника (самопроверка при загрузке) — прямо
// в serial.
static rune_sink_t g_out;
static void emit(const char* s) {
    if (g_out) g_out(s);
    else serial_puts(s);
}

static char g_ln[320];
static int  g_ln_n;
static void ln_reset(void) { g_ln_n = 0; g_ln[0] = 0; }
static void ln_s(const char* s) {
    while (*s && g_ln_n < (int)sizeof(g_ln) - 1) g_ln[g_ln_n++] = *s++;
    g_ln[g_ln_n] = 0;
}
static void ln_u(uint64_t v) {
    char b[24];
    int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && g_ln_n < (int)sizeof(g_ln) - 1) g_ln[g_ln_n++] = b[--n];
    g_ln[g_ln_n] = 0;
}

// ----------------------------------------------------------------------------
// Файл целиком в память. Модуль обязан лежать в памяти одним куском (структуры
// движка ссылаются на его байты), а размером он бывает в мегабайты — берём
// страницы у pmm_alloc_big, не у кучи. Сначала том FAT (большие файлы из
// подкаталогов в реестре VFS не числятся, CLAUDE.md §5.36), потом VFS (initrd).
// ----------------------------------------------------------------------------
typedef struct { uint8_t* data; uint64_t len; uint64_t hpa, pages; } blob_t;

static void blob_free(blob_t* b) {
    if (b->hpa) pmm_free_big(b->hpa, b->pages);
    b->hpa = 0;
    b->data = 0;
}

static const char* blob_load(const char* path, blob_t* b) {
    b->data = 0; b->len = 0; b->hpa = 0; b->pages = 0;
    uint64_t size = 0;
    uint8_t probe;
    int fd = -1;
    bool on_fat = fat32_is_mounted() && fat32_read_file_at(path, 0, &probe, 0, &size) >= 0;
    if (!on_fat) {
        fd = vfs_open(path);
        if (fd < 0) return "file not found";
        int64_t s = vfs_size_fd(fd);
        if (s < 0) { vfs_close(fd); return "file not found"; }
        size = (uint64_t)s;
    }
    const char* why = 0;
    if (size == 0) why = "file is empty";
    else if (size > 0xFFFFFFFFull) why = "file is too large";
    if (!why) {
        b->pages = (size + 4095) / 4096;
        b->hpa = pmm_alloc_big(b->pages);
        if (b->hpa == PMM_INVALID) { b->hpa = 0; why = "not enough contiguous memory for the file"; }
    }
    if (!why) {
        b->data = (uint8_t*)(uintptr_t)b->hpa;
        for (uint64_t got = 0; got < size && !why;) {
            uint64_t part = size - got > (4ull << 20) ? (4ull << 20) : size - got;
            int64_t r = on_fat ? fat32_read_file_at(path, got, b->data + got, part, 0)
                               : vfs_read(fd, b->data + got, part);
            if (r <= 0) why = "reading the file failed";
            else got += (uint64_t)r;
        }
        b->len = size;
    }
    if (fd >= 0) vfs_close(fd);
    if (why) blob_free(b);
    return why;
}

// ----------------------------------------------------------------------------
// Встроенные модули самопроверки — байты записаны вручную (на машине сборки
// может не быть ни одного инструмента WebAssembly).
// ----------------------------------------------------------------------------
// (module (memory (export "mem") 1)
//   (func (export "add") (param i32 i32) (result i32) local.get 0 local.get 1 i32.add))
static const uint8_t k_mod_add[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,                 // «\0asm», версия 1
    0x01, 0x07, 0x01, 0x60, 0x02, 0x7F, 0x7F, 0x01, 0x7F,           // типы: (i32, i32) -> i32
    0x03, 0x02, 0x01, 0x00,                                         // функции: одна, типа 0
    0x05, 0x03, 0x01, 0x00, 0x01,                                   // память: от 1 страницы
    0x07, 0x0D, 0x02, 0x03, 'a', 'd', 'd', 0x00, 0x00,              // экспорт: add = функция 0,
                      0x03, 'm', 'e', 'm', 0x02, 0x00,              //          mem = память 0
    0x0A, 0x09, 0x01, 0x07, 0x00,                                   // код: одно тело, локальных нет
    0x20, 0x00, 0x20, 0x01, 0x6A, 0x0B,                             // local.get 0, local.get 1, i32.add, end
};
#define ADD_OFF_LOCAL1 49               // операнд второго local.get
#define ADD_OFF_OP     50               // i32.add

// (func (export "sum") (param i32) (result i32) (local i32) — сумма 1..n циклом
//   block  loop  local.get 0  i32.eqz  br_if 1
//                local.get 1  local.get 0  i32.add  local.set 1
//                local.get 0  i32.const 1  i32.sub  local.set 0  br 0  end  end
//   local.get 1)
static const uint8_t k_mod_sum[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,                 // типы: (i32) -> i32
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x07, 0x01, 0x03, 's', 'u', 'm', 0x00, 0x00,
    0x0A, 0x23, 0x01, 0x21, 0x01, 0x01, 0x7F,                       // код: тело 33 байта, одна локальная i32
    0x02, 0x40, 0x03, 0x40,                                         // block, loop
    0x20, 0x00, 0x45, 0x0D, 0x01,                                   // local.get 0, i32.eqz, br_if 1
    0x20, 0x01, 0x20, 0x00, 0x6A, 0x21, 0x01,
    0x20, 0x00, 0x41, 0x01, 0x6B, 0x21, 0x00,
    0x0C, 0x00, 0x0B, 0x0B,                                         // br 0, end, end
    0x20, 0x01, 0x0B,
};

// Секция памяти раньше секции типов — порядок нарушен.
static const uint8_t k_mod_order[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
};

static bool check(const char* prefix, const char* name, bool ok, const char* detail) {
    ln_reset();
    ln_s(prefix); ln_s(name); ln_s(ok ? ": PASS" : ": FAIL");
    if (detail && detail[0]) { ln_s(" ("); ln_s(detail); ln_s(")"); }
    ln_s("\n");
    emit(g_ln);
    return ok;
}

// Модуль должен быть отклонён именно с такой причиной (kind).
static bool rejected_as(const uint8_t* bin, uint32_t len, int kind) {
    wasm_err_t err;
    wasm_module_t* m = wasm_module_load(bin, len, &err);
    if (m) { wasm_module_free(m); return false; }
    return err.kind == kind;
}

static int run_selftest(const char* prefix) {
    int fails = 0;
    static char d[160];
    wasm_err_t err;
    uint64_t t0 = timer_get_uptime_ms();

    // 1. Маленький модуль: счётчики и глубина стека — те, что должны быть.
    wasm_module_t* m = wasm_module_load(k_mod_add, sizeof(k_mod_add), &err);
    bool ok = m && m->n_types == 1 && m->n_funcs == 1 && m->n_exports == 2 && m->n_mems == 1 &&
              m->mems[0].lim.min == 1 && m->funcs[0].max_stack == 2 &&
              wasm_find_export(m, "add", WK_FUNC) == 0 && wasm_find_export(m, "mem", WK_MEM) == 0 &&
              wasm_find_export(m, "nope", WK_FUNC) < 0;
    if (m) wasm_module_free(m);
    if (!check(prefix, "module add (52 bytes: types, memory, exports, code)", ok, m ? "" : err.text)) fails++;

    // 2. Блоки, цикл, условный переход, локальные.
    m = wasm_module_load(k_mod_sum, sizeof(k_mod_sum), &err);
    ok = m && m->funcs[0].n_locals == 1 && m->funcs[0].max_stack == 2 && m->code_bytes == 30;
    if (m) wasm_module_free(m);
    if (!check(prefix, "module sum (block, loop, br_if, locals)", ok, m ? "" : err.text)) fails++;

    // 3. Испорченные модули отклоняются, и причина названа верно: «это не
    //    модуль» (malformed) или «нарушены типы» (invalid).
    static uint8_t bad[sizeof(k_mod_add)];
    int good = 0;
    for (size_t i = 0; i < sizeof(bad); i++) bad[i] = k_mod_add[i];
    bad[1] = 'b';                                                   // не «\0asm»
    good += rejected_as(bad, sizeof(bad), WASM_E_MALFORMED);
    good += rejected_as(k_mod_add, sizeof(k_mod_add) - 3, WASM_E_MALFORMED);        // обрезан
    good += rejected_as(k_mod_order, sizeof(k_mod_order), WASM_E_MALFORMED);        // порядок секций
    for (size_t i = 0; i < sizeof(bad); i++) bad[i] = k_mod_add[i];
    if (bad[ADD_OFF_OP] == 0x6A) {
        bad[ADD_OFF_OP] = 0x7C;                                     // i64.add над двумя i32
        good += rejected_as(bad, sizeof(bad), WASM_E_INVALID);
        bad[ADD_OFF_OP] = 0x6A;
        bad[ADD_OFF_LOCAL1] = 0x05;                                 // локальной 5 нет
        good += rejected_as(bad, sizeof(bad), WASM_E_INVALID);
    }
    d[0] = 0;
    ln_reset(); ln_u((uint64_t)good); ln_s(" of 5: bad magic, truncated, section order, type mismatch, unknown local");
    for (int i = 0; i <= g_ln_n && i < (int)sizeof(d); i++) d[i] = g_ln[i];
    if (!check(prefix, "broken modules are rejected with the right reason", good == 5, d)) fails++;

    // 4. Память модулей возвращается куче целиком.
    heap_stats_t h0 = heap_get_stats();
    for (int i = 0; i < 200; i++) {
        m = wasm_module_load(k_mod_sum, sizeof(k_mod_sum), 0);
        if (m) wasm_module_free(m);
        (void)rejected_as(k_mod_order, sizeof(k_mod_order), WASM_E_MALFORMED);
    }
    heap_stats_t h1 = heap_get_stats();
    if (!check(prefix, "200 load/free cycles leave the kernel heap unchanged", h0.used_size == h1.used_size, "")) fails++;

    ln_reset();
    ln_s(prefix); ln_s("done in "); ln_u(timer_get_uptime_ms() - t0); ln_s(" ms: ");
    ln_s(fails ? "FAIL\n" : "PASS\n");
    emit(g_ln);
    return fails;
}

// ============================================================================
// [51.1] Исполнение.
// ============================================================================
// Предел времени. Интерпретатор сам на часы не смотрит (он не знает, где
// исполняется): раз в 256 витков он читает флаг остановки, а ставит флаг
// таймер ядра, когда выходит отпущенное время. Пока программа исполняется
// прямо на BSP (подэтап 51.1), это единственное, что не даёт зациклившейся
// программе остановить всю систему.
static volatile int      g_stop;
static volatile uint64_t g_deadline_ms;         // 0 — никто не исполняется

static void job_tick_1ms(void);         // [51.3] предел времени счёта программы на другом ядре

void rune_tick_1ms(void) {
    uint64_t d = g_deadline_ms;
    if (d && timer_get_uptime_ms() >= d) g_stop = 1;
    job_tick_1ms();
}

static void ln_i(int64_t v) {
    if (v < 0) { ln_s("-"); ln_u(0 - (uint64_t)v); }
    else ln_u((uint64_t)v);
}
static void ln_hex(uint64_t v, int digits) {
    static const char d[] = "0123456789abcdef";
    char c[2] = { 0, 0 };
    for (int i = digits - 1; i >= 0; i--) { c[0] = d[(v >> (i * 4)) & 15]; ln_s(c); }
}

// (func (export "spin") (loop (br 0))) — вечный цикл.
static const uint8_t k_mod_spin[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,                             // типы: () -> ()
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 's', 'p', 'i', 'n', 0x00, 0x00,
    0x0A, 0x09, 0x01, 0x07, 0x00, 0x03, 0x40, 0x0C, 0x00, 0x0B, 0x0B,   // loop, br 0, end, end
};
// (func (export "deep") (call 0)) — бесконечная рекурсия.
static const uint8_t k_mod_deep[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x08, 0x01, 0x04, 'd', 'e', 'e', 'p', 0x00, 0x00,
    0x0A, 0x06, 0x01, 0x04, 0x00, 0x10, 0x00, 0x0B,                 // call 0, end
};
// (import "env" "twice" (func (param i32) (result i32)))
// (func (export "run") (param i32) (result i32) local.get 0  call 0  i32.const 1  i32.add)
static const uint8_t k_mod_imp[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x06, 0x01, 0x60, 0x01, 0x7F, 0x01, 0x7F,                 // типы: (i32) -> i32
    0x02, 0x0D, 0x01, 0x03, 'e', 'n', 'v', 0x05, 't', 'w', 'i', 'c', 'e', 0x00, 0x00,
    0x03, 0x02, 0x01, 0x00,
    0x07, 0x07, 0x01, 0x03, 'r', 'u', 'n', 0x00, 0x01,              // экспорт: run = функция 1 (0 — импорт)
    0x0A, 0x0B, 0x01, 0x09, 0x00, 0x20, 0x00, 0x10, 0x00, 0x41, 0x01, 0x6A, 0x0B,
};

static int host_twice(wasm_instance_t* caller, wasm_val_t* vals, void* user) {
    (void)caller; (void)user;
    vals[0] = (uint32_t)(vals[0] * 2);
    return WASM_TRAP_NONE;
}
static bool resolve_env(void* ctx, const wasm_module_t* m, const wasm_import_t* imp, wasm_extern_t* out) {
    (void)m;
    if (imp->kind != WK_FUNC || imp->field.n != 5 || imp->field.p[0] != 't') return false;
    out->kind = WK_FUNC;
    out->ptr = ctx;
    return true;
}

static wasm_funcinst_t* get_func(const wasm_instance_t* inst, const char* name) {
    wasm_extern_t ex;
    uint32_t n = 0;
    while (name[n]) n++;
    if (!inst || !wasm_instance_export(inst, (const uint8_t*)name, n, &ex) || ex.kind != WK_FUNC) return 0;
    return (wasm_funcinst_t*)ex.ptr;
}

// Вызов функции вида (i32, i32) -> i32 или уже. Причина остановки либо 0.
static int call2(wasm_store_t* s, wasm_funcinst_t* f, uint32_t a, uint32_t b, uint32_t* out) {
    wasm_val_t args[2] = { a, b }, res[2] = { 0, 0 };
    if (!f) return WASM_TRAP_HOST;
    int trap = wasm_call(s, f, args, res);
    if (out) *out = (uint32_t)res[0];
    return trap;
}

static int run_exec_selftest(const char* prefix) {
    int fails = 0;
    uint64_t t0 = timer_get_uptime_ms();
    heap_stats_t h0 = heap_get_stats();
    uint64_t ram0 = pmm_free_ram();

    // Тот же модуль add, но вместо сложения — деление со знаком.
    static uint8_t div_bin[sizeof(k_mod_add)];
    for (size_t i = 0; i < sizeof(div_bin); i++) div_bin[i] = k_mod_add[i];
    div_bin[ADD_OFF_OP] = 0x6D;

    wasm_module_t* m_add  = wasm_module_load(k_mod_add, sizeof(k_mod_add), 0);
    wasm_module_t* m_sum  = wasm_module_load(k_mod_sum, sizeof(k_mod_sum), 0);
    wasm_module_t* m_div  = wasm_module_load(div_bin, sizeof(div_bin), 0);
    wasm_module_t* m_spin = wasm_module_load(k_mod_spin, sizeof(k_mod_spin), 0);
    wasm_module_t* m_deep = wasm_module_load(k_mod_deep, sizeof(k_mod_deep), 0);
    wasm_module_t* m_imp  = wasm_module_load(k_mod_imp, sizeof(k_mod_imp), 0);
    wasm_store_t* s = wasm_store_new(0);
    bool ready = m_add && m_sum && m_div && m_spin && m_deep && m_imp && s;
    wasm_instance_t* i_add = ready ? wasm_instantiate(s, m_add, 0, 0, 0) : 0;
    wasm_funcinst_t* f_add  = get_func(i_add, "add");
    wasm_funcinst_t* f_sum  = ready ? get_func(wasm_instantiate(s, m_sum, 0, 0, 0), "sum") : 0;
    wasm_funcinst_t* f_div  = ready ? get_func(wasm_instantiate(s, m_div, 0, 0, 0), "add") : 0;
    wasm_funcinst_t* f_spin = ready ? get_func(wasm_instantiate(s, m_spin, 0, 0, 0), "spin") : 0;
    wasm_funcinst_t* f_deep = ready ? get_func(wasm_instantiate(s, m_deep, 0, 0, 0), "deep") : 0;
    uint32_t r = 0, r2 = 0, r3 = 0;
    bool ok;

    // 1. Обычный счёт: сложение (с переносом за 32 бита) и цикл.
    ok = call2(s, f_add, 2, 3, &r) == 0 && r == 5 &&
         call2(s, f_add, 0xFFFFFFFFu, 1, &r2) == 0 && r2 == 0 &&
         call2(s, f_sum, 1000, 0, &r3) == 0 && r3 == 500500;
    if (!check(prefix, "add(2, 3) = 5, sum(1000) = 500500", ok, ready ? "" : "modules or store did not load")) fails++;

    // 2. Ошибки счёта останавливают программу, а не ядро; после остановки
    //    хранилище годно для следующего вызова.
    ok = call2(s, f_div, 7, 2, &r) == 0 && r == 3 &&
         call2(s, f_div, 7, 0, 0) == WASM_TRAP_DIV_ZERO &&
         call2(s, f_div, 0x80000000u, 0xFFFFFFFFu, 0) == WASM_TRAP_INT_OVERFLOW &&
         call2(s, f_div, (uint32_t)-7, 2, &r2) == 0 && r2 == (uint32_t)-3;
    if (!check(prefix, "traps: divide by zero, INT_MIN / -1; the next call still works", ok, "")) fails++;

    // 3. Вечный цикл: кончается «топливо»; ставится флаг остановки; выходит
    //    время (флаг ставит таймер ядра). В третьем случае «топливо» — страховка:
    //    если таймер не сработает, проверка провалится, а не повиснет.
    int t_fuel = WASM_TRAP_HOST, t_flag = WASM_TRAP_HOST, t_time = WASM_TRAP_HOST;
    uint64_t waited = 0;
    if (s && f_spin) {
        wasm_store_limit(s, 50, 0);
        t_fuel = call2(s, f_spin, 0, 0, 0);
        g_stop = 1;
        wasm_store_limit(s, -1, &g_stop);
        t_flag = call2(s, f_spin, 0, 0, 0);
        g_stop = 0;
        wasm_store_limit(s, 2000000, &g_stop);
        uint64_t w0 = timer_get_uptime_ms();
        g_deadline_ms = w0 + 5;
        t_time = call2(s, f_spin, 0, 0, 0);
        g_deadline_ms = 0;
        g_stop = 0;
        waited = timer_get_uptime_ms() - w0;
        wasm_store_limit(s, -1, 0);
    }
    ln_reset(); ln_s("time limit 5 ms hit after "); ln_u(waited); ln_s(" ms");
    static char d[96];
    for (int i = 0; i <= g_ln_n && i < (int)sizeof(d); i++) d[i] = g_ln[i];
    ok = t_fuel == WASM_TRAP_FUEL && t_flag == WASM_TRAP_STOPPED && t_time == WASM_TRAP_STOPPED && waited < 100;
    if (!check(prefix, "endless loop is stopped: out of fuel, stop flag, time limit", ok, d)) fails++;

    // 4. Бесконечная рекурсия упирается в НАШ предел глубины: кадры вызовов
    //    лежат в памяти хранилища, стек ядра (32 КБ) не растёт.
    ok = call2(s, f_deep, 0, 0, 0) == WASM_TRAP_STACK && call2(s, f_add, 1, 1, &r) == 0 && r == 2;
    if (!check(prefix, "endless recursion ends with 'call stack exhausted'", ok, "")) fails++;

    // 5. Импорт: без того, кто его даст, модуль не запускается; с функцией
    //    ядра — вызывает её.
    wasm_inst_err_t ierr;
    ierr.kind = 0;
    bool no_link = ready && !wasm_instantiate(s, m_imp, 0, 0, &ierr) && ierr.kind == WASM_INST_LINK;
    static const uint8_t t_i32[1] = { WT_I32 };
    wasm_funcinst_t* twice = s ? wasm_host_func(s, t_i32, 1, t_i32, 1, host_twice, 0) : 0;
    wasm_funcinst_t* f_run = (ready && twice) ? get_func(wasm_instantiate(s, m_imp, resolve_env, twice, 0), "run") : 0;
    ok = no_link && call2(s, f_run, 20, 0, &r) == 0 && r == 41;
    if (!check(prefix, "import: unresolved = link error; a kernel function is called (run(20) = 41)", ok, no_link ? "" : ierr.text)) fails++;

    // 6. Память программы: страница 64 КБ, обнулена; после освобождения
    //    хранилища ядру возвращается всё — и куча, и страницы.
    wasm_memory_t* mem = i_add ? wasm_instance_memory(i_add) : 0;
    uint64_t bytes = 0;
    uint8_t* data = mem ? wasm_memory_data(mem, &bytes) : 0;
    ok = data && bytes == WASM_PAGE && wasm_memory_pages(mem) == 1 && data[0] == 0 && data[WASM_PAGE - 1] == 0;
    if (s) wasm_store_free(s);
    if (m_add) wasm_module_free(m_add);
    if (m_sum) wasm_module_free(m_sum);
    if (m_div) wasm_module_free(m_div);
    if (m_spin) wasm_module_free(m_spin);
    if (m_deep) wasm_module_free(m_deep);
    if (m_imp) wasm_module_free(m_imp);
    heap_stats_t h1 = heap_get_stats();
    ok = ok && h0.used_size == h1.used_size && pmm_free_ram() == ram0;
    if (!check(prefix, "program memory: one zeroed page; everything returns to the kernel", ok, "")) fails++;

    ln_reset();
    ln_s(prefix); ln_s("done in "); ln_u(timer_get_uptime_ms() - t0); ln_s(" ms: ");
    ln_s(fails ? "FAIL\n" : "PASS\n");
    emit(g_ln);
    return fails;
}

// ---- [51.5] самопроверка JIT ---------------------------------------------------------
// Машинный код обязан (а) вообще исполняться в ядре — память под него должна
// быть исполняемой, (б) давать то же, что интерпретатор, (в) останавливаться
// теми же пределами. Сверяем на встроенных модулях: один и тот же счёт в двух
// хранилищах — с JIT и без. Время печатается для сведения, порога на него нет.
static int run_jit_selftest(const char* prefix) {
    int fails = 0;
    uint64_t t0 = timer_get_uptime_ms();
    bool was = wasm_jit_enabled();
    const uint32_t n = 1000000;
    uint32_t r[2] = { 0, 0 }, funcs[2] = { 0, 0 };
    uint64_t ms[2] = { 0, 0 }, bytes[2] = { 0, 0 };
    int t_fuel = -100, t_deep = -100, t_div = -100;
    bool ok = true;
    for (int mode = 0; mode < 2; mode++) {              // 0 — интерпретатор, 1 — машинный код
        wasm_jit_default(mode == 1);
        wasm_module_t* m = wasm_module_load(k_mod_sum, sizeof(k_mod_sum), 0);
        wasm_store_t* s = wasm_store_new(0);
        wasm_funcinst_t* f = (m && s) ? get_func(wasm_instantiate(s, m, 0, 0, 0), "sum") : 0;
        uint64_t a = timer_get_uptime_ms();
        ok = ok && f && call2(s, f, n, 0, &r[mode]) == 0;
        ms[mode] = timer_get_uptime_ms() - a;
        if (s) wasm_store_jit_info(s, &funcs[mode], &bytes[mode]);
        if (mode == 1 && s) {
            // Пределы и ловушки — те же, что у интерпретатора.
            static uint8_t div_bin[sizeof(k_mod_add)];
            for (size_t i = 0; i < sizeof(div_bin); i++) div_bin[i] = k_mod_add[i];
            div_bin[ADD_OFF_OP] = 0x6D;
            wasm_module_t* m_spin = wasm_module_load(k_mod_spin, sizeof(k_mod_spin), 0);
            wasm_module_t* m_deep = wasm_module_load(k_mod_deep, sizeof(k_mod_deep), 0);
            wasm_module_t* m_div  = wasm_module_load(div_bin, sizeof(div_bin), 0);
            wasm_funcinst_t* f_spin = m_spin ? get_func(wasm_instantiate(s, m_spin, 0, 0, 0), "spin") : 0;
            wasm_funcinst_t* f_deep = m_deep ? get_func(wasm_instantiate(s, m_deep, 0, 0, 0), "deep") : 0;
            wasm_funcinst_t* f_div  = m_div ? get_func(wasm_instantiate(s, m_div, 0, 0, 0), "add") : 0;
            if (f_spin) { wasm_store_limit(s, 50, 0); t_fuel = call2(s, f_spin, 0, 0, 0); wasm_store_limit(s, -1, 0); }
            if (f_deep) t_deep = call2(s, f_deep, 0, 0, 0);
            if (f_div) t_div = call2(s, f_div, 7, 0, 0);
            wasm_store_free(s); s = 0;
            if (m_spin) wasm_module_free(m_spin);
            if (m_deep) wasm_module_free(m_deep);
            if (m_div) wasm_module_free(m_div);
        }
        if (s) wasm_store_free(s);
        if (m) wasm_module_free(m);
    }
    wasm_jit_default(was);
    static char d[120];
    ln_reset();
    ln_u(bytes[1]); ln_s(" bytes of code; sum("); ln_u(n); ln_s("): "); ln_u(ms[1]); ln_s(" ms, interpreter "); ln_u(ms[0]); ln_s(" ms");
    for (int i = 0; i <= g_ln_n && i < (int)sizeof(d); i++) d[i] = g_ln[i];
    ok = ok && r[0] == r[1] && r[1] == (uint32_t)((uint64_t)n * (n + 1) / 2) && funcs[1] == 1 && bytes[1] > 0 && funcs[0] == 0;
    if (!check(prefix, "machine code runs in the kernel and agrees with the interpreter", ok, d)) fails++;
    ok = t_fuel == WASM_TRAP_FUEL && t_deep == WASM_TRAP_STACK && t_div == WASM_TRAP_DIV_ZERO;
    if (!check(prefix, "machine code is stopped the same way: out of fuel, call stack exhausted, divide by zero", ok, "")) fails++;
    ln_reset();
    ln_s(prefix); ln_s("done in "); ln_u(timer_get_uptime_ms() - t0); ln_s(" ms: ");
    ln_s(fails ? "FAIL\n" : "PASS\n");
    emit(g_ln);
    return fails;
}

void rune_jit_set(bool on) { wasm_jit_default(on); }
bool rune_jit_on(void) { return wasm_jit_enabled(); }

// ---- `rune call` -------------------------------------------------------------------
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Целое: [-]десятичное или [-]0xшестнадцатеричное.
static bool parse_int(const char* s, uint64_t* out) {
    bool neg = false;
    uint64_t v = 0;
    if (*s == '-') { neg = true; s++; }
    if (!*s) return false;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return false;
        for (; *s; s++) { int dgt = hex_digit(*s); if (dgt < 0) return false; v = (v << 4) | (uint64_t)dgt; }
    } else {
        for (; *s; s++) { if (*s < '0' || *s > '9') return false; v = v * 10 + (uint64_t)(*s - '0'); }
    }
    *out = neg ? 0 - v : v;
    return true;
}
// Дробное: [-]цифры[.цифры]. Все цифры собираются в целое, затем одно деление
// на степень десяти: оба числа представимы точно, значит результат — ближайшее
// к записанному число (как у strtod), пока цифр не больше пятнадцати.
static bool parse_float(const char* s, double* out) {
    bool neg = false, dot = false, any = false;
    uint64_t mant = 0;
    int frac = 0;
    if (*s == '-') { neg = true; s++; }
    for (; *s; s++) {
        if (*s == '.' && !dot) { dot = true; continue; }
        if (*s < '0' || *s > '9') return false;
        if (mant >= (1ull << 53) / 10) return false;
        mant = mant * 10 + (uint64_t)(*s - '0');
        any = true;
        if (dot) frac++;
    }
    if (!any) return false;
    double v = (double)mant, p = 1.0;
    while (frac-- > 0) p *= 10.0;
    v /= p;
    *out = neg ? -v : v;
    return true;
}

static void ln_value(uint8_t type, wasm_val_t v) {
    switch (type) {
    case WT_I32: ln_i((int32_t)(uint32_t)v); break;
    case WT_I64: ln_i((int64_t)v); break;
    case WT_F32: ln_s("f32:0x"); ln_hex((uint32_t)v, 8); break;     // биты: печатать дробные ядро не умеет
    case WT_F64: ln_s("f64:0x"); ln_hex(v, 16); break;
    default:     ln_s(v ? "ref" : "null"); break;
    }
}

#define RUNE_MAX_ARGS 16

bool rune_call(rune_sink_t out, const char* path, const char* func, int nargs, char** args, uint32_t limit_ms) {
    g_out = out;
    blob_t b;
    static char why_buf[200];
    static wasm_val_t vals[RUNE_MAX_ARGS], res[RUNE_MAX_ARGS];
    const char* why = blob_load(path, &b);
    wasm_module_t* m = 0;
    wasm_store_t* s = 0;
    wasm_funcinst_t* f = 0;
    const wasm_functype_t* ft = 0;
    bool ok = false;
    if (!limit_ms) limit_ms = RUNE_DEFAULT_LIMIT_MS;

    if (!why) {
        wasm_err_t err;
        m = wasm_module_load(b.data, (uint32_t)b.len, &err);
        if (!m) {
            ln_reset(); ln_s("rejected - "); ln_s(wasm_err_name(err.kind)); ln_s(": "); ln_s(err.text);
            for (int i = 0; i <= g_ln_n && i < (int)sizeof(why_buf); i++) why_buf[i] = g_ln[i];
            why = why_buf;
        }
    }
    if (!why && !(s = wasm_store_new(0))) why = "not enough memory for the engine";
    if (!why) {
        // Функция start исполняется уже при запуске модуля — предел времени
        // должен действовать и на неё.
        g_stop = 0;
        wasm_store_limit(s, -1, &g_stop);
        g_deadline_ms = timer_get_uptime_ms() + limit_ms;
        wasm_inst_err_t ierr;
        wasm_instance_t* inst = wasm_instantiate(s, m, 0, 0, &ierr);
        if (!inst) {
            ln_reset(); ln_s(ierr.text[0] ? ierr.text : "module did not start");
            // Импорты (файлы, вывод, часы) программе даёт WASI — подэтап 51.2.
            if (ierr.kind == WASM_INST_LINK) ln_s(" (nothing provides imports yet)");
            for (int i = 0; i <= g_ln_n && i < (int)sizeof(why_buf); i++) why_buf[i] = g_ln[i];
            why = why_buf;
        } else if (!(f = get_func(inst, func))) {
            why = "no exported function with this name";
        }
    }
    if (!why) {
        ft = wasm_func_type(f);
        if (ft->n_params != (uint32_t)nargs) why = "wrong number of arguments";
        else if (ft->n_params > RUNE_MAX_ARGS || ft->n_results > RUNE_MAX_ARGS) why = "too many arguments or results for this command";
    }
    for (int i = 0; !why && i < nargs; i++) {
        uint8_t t = ft->types[i];
        uint64_t v = 0;
        double x = 0;
        if (t == WT_I32 || t == WT_I64) {
            if (!parse_int(args[i], &v)) why = "argument is not an integer";
            vals[i] = t == WT_I32 ? (uint32_t)v : v;
        } else if (t == WT_F32 || t == WT_F64) {
            if (!parse_float(args[i], &x)) why = "argument is not a number";
            if (t == WT_F32) { union { float f; uint32_t u; } c; c.f = (float)x; vals[i] = c.u; }
            else { union { double d; uint64_t u; } c; c.d = x; vals[i] = c.u; }
        } else why = "reference arguments cannot be given from the terminal";
    }

    if (!why) {
        uint64_t t0 = timer_get_uptime_ms();
        uint64_t k0 = wasm_store_ticks(s);
        int trap = wasm_call(s, f, vals, res);
        uint64_t ms = timer_get_uptime_ms() - t0;
        ln_reset();
        ln_s("[rune] call "); ln_s(func); ln_s("(");
        for (int i = 0; i < nargs; i++) { if (i) ln_s(", "); ln_s(args[i]); }
        ln_s(")");
        if (trap) {
            ln_s(" trapped: "); ln_s(wasm_trap_name(trap));
            if (trap == WASM_TRAP_STOPPED) { ln_s(" (time limit "); ln_u(limit_ms); ln_s(" ms)"); }
        } else {
            ln_s(" = ");
            if (!ft->n_results) ln_s("(nothing)");
            for (uint32_t i = 0; i < ft->n_results; i++) { if (i) ln_s(", "); ln_value(ft->types[ft->n_params + i], res[i]); }
            ok = true;
        }
        ln_s(" | "); ln_u(ms); ln_s(" ms, fuel "); ln_u(wasm_store_ticks(s) - k0); ln_s("\n");
        emit(g_ln);
    } else {
        ln_reset();
        ln_s("[rune] call "); ln_s(path); ln_s(": "); ln_s(why); ln_s("\n");
        emit(g_ln);
    }
    g_deadline_ms = 0;
    g_stop = 0;
    if (s) wasm_store_free(s);
    if (m) wasm_module_free(m);
    blob_free(&b);
    g_out = 0;
    return ok;
}

// ============================================================================
// [51.2] WASI: что ядро даёт программе.
// ============================================================================
// Слой wasm_wasi.c сам ведёт описатели и границу каталога; сюда приходят уже
// готовые пути внутри разрешённого каталога. Здесь — перевод его просьб на
// язык ядра: том FAT, окно терминала, таймер, RTC.
//
// Кодировки. Программа говорит на UTF-8, ядро и драйвер FAT — на CP1251.
// Поэтому вывод программы и имена файлов переводятся в CP1251, имена из
// каталога — обратно в UTF-8 (mind_to_cp1251 / mind_to_utf8).
typedef struct {
    int      col;                       // столбец — для табуляции
    bool     line_start;                // последним выведен перевод строки (или ничего)
    uint8_t  carry[4];                  // начало символа UTF-8, разрезанного между двумя кусками вывода
    int      carry_n;
    char*    capture;                   // самопроверка: вывод не на экран, а в буфер
    int      cap_n, cap_max;
    uint64_t t0_ms, unix0_s;            // настенное время в момент запуска
    rune_sink_t sink;                   // [51.3] куда идёт вывод программы (0 — только в журнал)
} krun_t;
static krun_t g_run;

// Уже CP1251. Управляющие символы в окно терминала не пускаем: программа не
// должна уметь двигать курсор и стирать чужой текст.
static void run_emit(krun_t* k, const char* s) {
    if (k->sink) k->sink(s); else serial_puts(s);
}
static void run_put(krun_t* k, const char* s, int n) {
    char line[160];
    int ln = 0;
    for (int i = 0; i < n; i++) {
        char ch = s[i];
        int reps = 1;
        if (ch == '\r') continue;
        if (ch == '\t') { ch = ' '; reps = 8 - k->col % 8; }
        else if (((uint8_t)ch < 0x20 && ch != '\n') || ch == 0x7F) ch = '?';
        for (; reps > 0; reps--) {
            if (k->capture) { if (k->cap_n < k->cap_max - 1) { k->capture[k->cap_n++] = ch; k->capture[k->cap_n] = 0; } }
            else {
                line[ln++] = ch;
                if (ln == (int)sizeof(line) - 1) { line[ln] = 0; run_emit(k, line); ln = 0; }
            }
            k->col = ch == '\n' ? 0 : k->col + 1;
        }
        k->line_start = ch == '\n';
    }
    if (ln) { line[ln] = 0; run_emit(k, line); }
}

// Сколько последних байт куска — начало символа UTF-8, конец которого ещё не пришёл.
static int utf8_tail(const char* s, int n) {
    for (int back = 1; back <= 3 && back <= n; back++) {
        uint8_t b = (uint8_t)s[n - back];
        if ((b & 0xC0) == 0x80) continue;           // байт продолжения — ищем начало левее
        int need = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1;
        return need > back ? back : 0;
    }
    return 0;
}

static void kp_out(void* ctx, int fd, const uint8_t* data, uint32_t n) {
    krun_t* k = (krun_t*)ctx;
    (void)fd;                                       // вывод и ошибки — в одно окно
    static char in[520], cv[528];
    for (uint32_t i = 0; i < n;) {
        int m = 0;
        for (int c = 0; c < k->carry_n; c++) in[m++] = (char)k->carry[c];
        k->carry_n = 0;
        while (i < n && m < 512) in[m++] = (char)data[i++];
        int tail = utf8_tail(in, m);
        m -= tail;
        for (int t = 0; t < tail; t++) k->carry[t] = (uint8_t)in[m + t];
        k->carry_n = tail;
        run_put(k, cv, mind_to_cp1251(in, m, cv, (int)sizeof(cv)));
    }
}

// Путь слоя (UTF-8) -> путь для драйвера FAT (CP1251). false — в имени есть
// то, чего том хранить не может: символ вне CP1251, знак, запрещённый в
// именах FAT, или слишком длинное имя (драйвер молча обрезал бы его).
static bool fat_path(const char* path, char* out) {
    int n = 0;
    while (path[n]) n++;
    int o = mind_to_cp1251(path, n, out, WASI_PATH_MAX);
    int comp = 0;
    for (int i = 0; i < o; i++) {
        char c = out[i];
        if (c == '/') { comp = 0; continue; }
        if (c == '?' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|' || c == ':') return false;
        if (++comp >= FAT32_LONG_NAME_MAX - 2) return false;
    }
    return true;
}

static bool fat_parent_is_dir(const char* fat) {
    static char parent[WASI_PATH_MAX];
    int n = 0, cut = -1;
    for (; fat[n]; n++) { parent[n] = fat[n]; if (fat[n] == '/') cut = n; }
    if (cut <= 0) return false;
    parent[cut] = 0;
    fat32_dir_entry_t e;
    return fat32_stat(parent, &e) == 0 && e.is_dir;
}

static int kp_stat(void* ctx, const char* path, wasi_stat_t* st) {
    (void)ctx;
    static char fat[WASI_PATH_MAX];
    fat32_dir_entry_t e;
    if (!fat_path(path, fat) || fat32_stat(fat, &e) != 0) return WASI_ENOENT;
    st->is_dir = e.is_dir;
    st->size = e.size;
    st->mtime_ns = 0;                               // времени файлов драйвер FAT не ведёт
    return WASI_OK;
}
static int kp_load(void* ctx, const char* path, uint8_t* buf, uint64_t size) {
    (void)ctx;
    static char fat[WASI_PATH_MAX];
    if (!fat_path(path, fat)) return WASI_ENOENT;
    return fat32_read_file_at(fat, 0, buf, size, 0) == (int64_t)size ? WASI_OK : WASI_EIO;
}
static int kp_save(void* ctx, const char* path, const uint8_t* data, uint64_t size) {
    (void)ctx;
    static char fat[WASI_PATH_MAX];
    if (!fat_path(path, fat)) return WASI_EILSEQ;
    if (size > 0xFFFFFFFFull) return WASI_EFBIG;
    // Файлы программы в реестр VFS не идут: он плоский, на 256 записей, и
    // записи из него не удаляются — одна программа заполнила бы его целиком.
    bool was = fat32_vfs_autoregister(false);
    int64_t r = fat32_save_file(fat, data, size);
    fat32_vfs_autoregister(was);
    if (r == (int64_t)size) return WASI_OK;
    return fat_parent_is_dir(fat) ? WASI_ENOSPC : WASI_ENOENT;
}
static int kp_mkdir(void* ctx, const char* path) {
    (void)ctx;
    static char fat[WASI_PATH_MAX];
    if (!fat_path(path, fat)) return WASI_EILSEQ;
    int r = fat32_mkdir(fat);
    if (r == 0) return WASI_OK;
    if (r == 1) return WASI_EEXIST;
    return fat_parent_is_dir(fat) ? WASI_ENOSPC : WASI_ENOENT;
}
static int kp_first(const fat32_dir_entry_t* e, void* user) { (void)e; *(bool*)user = true; return 1; }
static int kp_remove(void* ctx, const char* path, bool dir) {
    (void)ctx;
    static char fat[WASI_PATH_MAX];
    if (!fat_path(path, fat)) return WASI_ENOENT;
    if (dir) {
        // Драйвер сам проверяет пустоту только по первому кластеру каталога.
        bool any = false;
        (void)fat32_iter_dir(fat, kp_first, &any);
        if (any) return WASI_ENOTEMPTY;
    }
    return fat32_delete_file(fat) == 0 ? WASI_OK : WASI_EIO;
}
typedef struct { wasi_list_fn cb; void* user; } klist_t;
static int kp_list_cb(const fat32_dir_entry_t* e, void* user) {
    klist_t* k = (klist_t*)user;
    static char name[FAT32_LONG_NAME_MAX * 2 + 4];
    int n = 0;
    while (e->name[n]) n++;
    mind_to_utf8(e->name, n, name, (int)sizeof(name));
    return k->cb(k->user, name, e->is_dir) ? 0 : 1;
}
static int kp_list(void* ctx, const char* path, wasi_list_fn cb, void* user) {
    (void)ctx;
    static char fat[WASI_PATH_MAX];
    klist_t k = { cb, user };
    if (!fat_path(path, fat)) return WASI_ENOENT;
    return fat32_iter_dir(fat, kp_list_cb, &k) < 0 ? WASI_EIO : WASI_OK;
}

// Дни от 1 января 1970 (алгоритм Хиннанта для григорианского календаря).
static uint64_t days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153u * (unsigned)(m + (m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)era * 146097 + doe - 719468;
}
// Настенное время: RTC читается один раз при запуске (его чтение ждёт конца
// обновления микросхемы — программе, которая спрашивает время в цикле, это
// было бы дорого), дальше — по таймеру ядра. RTC считаем идущим по UTC.
static void run_clock_start(krun_t* k) {
    rtc_time_t t;
    rtc_read(&t);
    k->t0_ms = timer_get_uptime_ms();
    k->unix0_s = days_from_civil(t.year, t.month, t.day) * 86400ull + t.hour * 3600ull + t.minute * 60ull + t.second;
}
static uint64_t kp_clock(void* ctx, bool realtime) {
    krun_t* k = (krun_t*)ctx;
    uint64_t ms = timer_get_uptime_ms();
    if (!realtime) return ms * 1000000ull;
    return k->unix0_s * 1000000000ull + (ms - k->t0_ms) * 1000000ull;
}
static void kp_random(void* ctx, uint8_t* buf, uint32_t n) { (void)ctx; crypto_rand_bytes(buf, n); }
// Сон программы: ядро спит до следующего прерывания; выход времени (флаг
// остановки) будит раньше срока.
static bool kp_sleep(void* ctx, uint64_t ns) {
    (void)ctx;
    uint64_t until = timer_get_uptime_ms() + (ns + 999999) / 1000000;
    while (timer_get_uptime_ms() < until) {
        if (g_stop) return false;
        __asm__ __volatile__("hlt");
    }
    return true;
}

static void kp_fill(wasi_host_t* h, krun_t* k) {
    for (size_t i = 0; i < sizeof(*h); i++) ((uint8_t*)h)[i] = 0;
    h->ctx = k;
    h->out = kp_out;
    h->in = 0;                          // ввода с клавиатуры у программы пока нет: сразу «конец ввода»
    h->stat = kp_stat; h->load = kp_load; h->save = kp_save; h->mkdir = kp_mkdir; h->remove = kp_remove;
    h->rename = 0;                      // драйвер FAT переименовывать не умеет — слой скопирует файл
    h->list = kp_list; h->clock_ns = kp_clock; h->random = kp_random; h->sleep_ns = kp_sleep;
}

// (import "wasi_snapshot_preview1" "fd_write" …) (import … "proc_exit" …)
// (memory (export "memory") 1)  (data (i32.const 8) <описатель куска: адрес 24, длина 5> … "rune\n")
// (func (export "_start")  fd_write(1, 8, 1, 20)  drop  proc_exit(7))
static const uint8_t k_mod_wasi[] = {
    0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x10, 0x03, 0x60, 0x04, 0x7F, 0x7F, 0x7F, 0x7F, 0x01, 0x7F,   // (i32 x4) -> i32
                      0x60, 0x01, 0x7F, 0x00,                           // (i32) -> ()
                      0x60, 0x00, 0x00,                                 // () -> ()
    0x02, 0x46, 0x02,
        0x16, 'w','a','s','i','_','s','n','a','p','s','h','o','t','_','p','r','e','v','i','e','w','1',
        0x08, 'f','d','_','w','r','i','t','e', 0x00, 0x00,
        0x16, 'w','a','s','i','_','s','n','a','p','s','h','o','t','_','p','r','e','v','i','e','w','1',
        0x09, 'p','r','o','c','_','e','x','i','t', 0x00, 0x01,
    0x03, 0x02, 0x01, 0x02,
    0x05, 0x03, 0x01, 0x00, 0x01,
    0x07, 0x13, 0x02, 0x06, 'm','e','m','o','r','y', 0x02, 0x00,
                      0x06, '_','s','t','a','r','t', 0x00, 0x02,
    0x0A, 0x13, 0x01, 0x11, 0x00,
        0x41, 0x01, 0x41, 0x08, 0x41, 0x01, 0x41, 0x14, 0x10, 0x00, 0x1A,   // fd_write(1, 8, 1, 20), drop
        0x41, 0x07, 0x10, 0x01, 0x0B,                                       // proc_exit(7)
    0x0B, 0x1B, 0x01, 0x00, 0x41, 0x08, 0x0B, 0x15,
        0x18, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,                     // кусок вывода: адрес 24, длина 5
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                     // сюда придёт «сколько записано»
        'r', 'u', 'n', 'e', 0x0A,
};

static int run_wasi_selftest(const char* prefix) {
    int fails = 0;
    uint64_t t0 = timer_get_uptime_ms();
    heap_stats_t h0 = heap_get_stats();
    uint64_t ram0 = pmm_free_ram();
    static char got[32];
    krun_t* k = &g_run;
    for (size_t i = 0; i < sizeof(*k); i++) ((uint8_t*)k)[i] = 0;
    k->capture = got; k->cap_max = (int)sizeof(got); got[0] = 0;
    k->line_start = true;

    wasm_module_t* m = wasm_module_load(k_mod_wasi, sizeof(k_mod_wasi), 0);
    wasm_store_t* s = wasm_store_new(0);
    static const char* const av[1] = { "selftest" };
    wasi_preopen_t pre = { "/", "/disk/nexus", true };
    wasi_config_t cfg;
    for (size_t i = 0; i < sizeof(cfg); i++) ((uint8_t*)&cfg)[i] = 0;
    cfg.argc = 1; cfg.argv = av;
    cfg.n_preopens = 1; cfg.preopens = &pre;
    kp_fill(&cfg.host, k);
    wasi_t* w = (m && s) ? wasi_new(s, &cfg) : 0;
    wasi_result_t r;
    r.status = -1; r.exit_code = -1; r.calls = 0;
    if (w) wasi_run(s, m, w, &r);
    bool ok = w && r.status == WASI_RUN_EXIT && r.exit_code == 7 && r.calls == 2 &&
              got[0] == 'r' && got[1] == 'u' && got[2] == 'n' && got[3] == 'e' && got[4] == '\n' && got[5] == 0;
    if (!check(prefix, "a WASI program prints through fd_write and ends with proc_exit(7)", ok, w ? "" : "module or environment did not load")) fails++;

    if (w) wasi_free(w);
    if (s) wasm_store_free(s);
    if (m) wasm_module_free(m);
    k->capture = 0;
    heap_stats_t h1 = heap_get_stats();
    ok = h0.used_size == h1.used_size && pmm_free_ram() == ram0;
    if (!check(prefix, "the environment returns all memory to the kernel", ok, "")) fails++;

    ln_reset();
    ln_s(prefix); ln_s("done in "); ln_u(timer_get_uptime_ms() - t0); ln_s(" ms: ");
    ln_s(fails ? "FAIL\n" : "PASS\n");
    emit(g_ln);
    return fails;
}

#define RUNE_MAX_ARGV 30

// ============================================================================
// [51.3] Программа на своём ядре.
// ============================================================================
// До 51.3 программа исполнялась прямо на главном ядре (BSP): пока она считала,
// не рисовались окна и не читалась клавиатура. Теперь её исполняет
// вычислительное ядро (AP) — как «жилец» пула (smp_pool.h), тем же способом,
// каким живёт vCPU гостя phantom:
//
//   AP:  rune_resident() — отрезок счёта (~2 мс) в интерпретаторе; на время
//        отрезка ядро помечено «занято жильцом», и пул MIND его не ждёт.
//   BSP: rune_tick() из цикла WM — всё, чего AP нельзя (§5.11): функции
//        окружения (диск, окно терминала), рост памяти программы. Интерпретатор
//        встаёт на паузу ПЕРЕД такой командой (wasm.h: WASM_PAUSED_SERVICE),
//        AP будит BSP прерыванием и засыпает; BSP исполняет одну эту команду
//        (wasm_service) и будит AP.
//
// Хранилище программы в каждый момент трогает ровно одно ядро: состояние JOB_RUN
// — оно у AP, JOB_WAIT — у BSP. Передача — записью состояния, без блокировок.
//
// Ожидание не занимает ядро. Программа ждёт ввода или спит — её вызов остаётся
// «несделанным» (WASM_HOST_RETRY), AP свободен для пула, а BSP повторяет вызов,
// когда появился ввод или вышел срок.
// ----------------------------------------------------------------------------
#define RUNE_SLICE_UNITS 400            // отрезок счёта: ~100 тысяч витков, около 2 мс
#define RUNE_START_MS    2000           // предел на запуск модуля (он идёт на BSP)

enum { JOB_NONE = 0, JOB_RUN, JOB_WAIT, JOB_DONE };

typedef struct {
    volatile int      state;            // JOB_*
    volatile uint64_t gen;              // номер запуска: по нему «жилец» прошлой программы узнаёт, что он лишний
    int               slot;             // ядро-исполнитель
    volatile int      result;           // чем кончился счёт на AP
    bool              first, quiet, sync;
    blob_t            blob;
    bool              blob_owned;
    wasm_module_t*    m;
    wasm_store_t*     s;
    wasi_t*           w;
    wasm_funcinst_t*  start;
    rune_sink_t       sink;             // куда идёт вывод (0 — только в журнал)
    void*             owner;            // окно-владелец (терминал) или 0
    char              path[160];
    char              dir[WASI_PATH_MAX];
    bool              read_only, no_files;
    char              argbuf[640];
    const char*       av[RUNE_MAX_ARGV + 2];
    wasi_preopen_t    pre;
    wasi_config_t     cfg;
    uint64_t          t0_ms;
    volatile uint64_t cpu_tsc;          // сколько программа считала (такты процессора)
    uint64_t          slices, services, retries;
    uint32_t          cpu_limit_ms, memory_mb;
    const char*       stop_why;         // почему остановили: предел времени, пользователь
    uint8_t           in[1024];         // ввод программы: пишет BSP (клавиатура), читает BSP (услуга)
    uint32_t          in_r, in_w;
    bool              in_eof, interactive;
    volatile bool     waiting_input;
    uint64_t          retry_ms;         // когда повторить несделанный вызов (сон программы)
    void              (*on_done)(void); // позвать, когда программа кончилась (BSP)
    char              why_limit[48];    // «time limit N ms» — текст причины остановки
    wasi_result_t     last;             // итог последней программы
    int               last_slot;
    uint64_t          last_cpu_ms, last_services;
} job_t;
static job_t    g_job;
static uint64_t g_tsc_ms;               // тактов процессора в миллисекунде

// [51.4] Итог последней закончившейся программы и причина последнего отказа в
// запуске — для окна Rune (rune_query, rune_last_error). Лежат отдельно от
// job_t: тот обнуляется при каждом запуске, а окно читает итог и после него.
static struct {
    uint64_t ends;
    int      kind, exit_code;
    char     text[64];
    uint64_t cpu_ms, wall_ms;
    uint32_t pages;
} g_end;
static char g_refuse[200];
static void refuse(const char* why) {
    int i = 0;
    for (; why && why[i] && why[i] != '\n' && i < (int)sizeof(g_refuse) - 1; i++) g_refuse[i] = why[i];
    g_refuse[i] = 0;
}

static void job_say(const job_t* j, const char* s) {
    if (j->quiet) return;
    if (j->sink) j->sink(s); else serial_puts(s);
}

static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
// Частота счётчика тактов: её уже мерил гипервизор (по каналу 8254); без него —
// по таймеру ядра за пять миллисекунд. Только с BSP и с разрешёнными прерываниями.
static void tsc_calibrate(void) {
    if (g_tsc_ms) return;
    g_tsc_ms = hv_tsc_per_ms();
    if (g_tsc_ms) return;
    uint64_t m = timer_get_uptime_ms();
    while (timer_get_uptime_ms() == m) { }
    uint64_t a = tsc_now();
    m = timer_get_uptime_ms();
    while (timer_get_uptime_ms() < m + 5) { }
    g_tsc_ms = (tsc_now() - a) / 5;
    if (!g_tsc_ms) g_tsc_ms = 1;
}
static uint64_t job_cpu_ms(const job_t* j) { return g_tsc_ms ? j->cpu_tsc / g_tsc_ms : 0; }

// ---- что программе можно видеть -----------------------------------------------------
// Каталог программы: только на томе FAT, существующий, и не из тех, где лежит
// системное. Корень тома — это users.txt (свёртки паролей), Sanctum —
// настройки системы: программе их не дают ни читать, ни тем более менять.
// Сравнение без учёта регистра — FAT его не различает.
static bool ieq_n(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) return false;
    }
    return true;
}
static const char* dir_check(const char* dir, char* out) {
    int n = 0;
    if (!dir || !dir[0]) dir = RUNE_DEFAULT_DIR;
    for (; dir[n] && n < WASI_PATH_MAX - 64; n++) out[n] = dir[n];
    while (n > 1 && out[n - 1] == '/') n--;
    out[n] = 0;
    if (!(n >= 5 && ieq_n(out, "/disk", 5) && (out[5] == '/' || out[5] == 0)))
        return "the program directory must be on the disk (/disk/...)";
    for (int i = 0; i + 1 < n; i++)
        if (out[i] == '.' && out[i + 1] == '.') return "the program directory must be given without '..'";
    if (n <= 6) return "a program cannot be given the whole disk (it holds system files)";
    if (n >= 13 && ieq_n(out + 5, "/sanctum", 8) && (out[13] == '/' || out[13] == 0))
        return "a program cannot be given the system settings directory";
    fat32_dir_entry_t de;
    if (fat32_stat(out, &de) != 0 || !de.is_dir) return "the program directory does not exist";
    return 0;
}

// Ввод программы (услуга на BSP). «Пока пусто» — только когда кто-то может
// ещё что-то ввести; иначе — конец ввода.
static uint32_t kp_in(void* ctx, uint8_t* buf, uint32_t n) {
    (void)ctx;
    job_t* j = &g_job;
    uint32_t got = 0;
    while (got < n && j->in_r != j->in_w) buf[got++] = j->in[j->in_r++ % sizeof(j->in)];
    if (got) return got;
    if (j->in_eof || !j->interactive || j->sync) return 0;
    j->waiting_input = true;
    return WASI_IN_WAIT;
}

static void job_release(job_t* j) {
    if (j->w) (void)wasi_free(j->w);
    if (j->s) wasm_store_free(j->s);
    if (j->m) wasm_module_free(j->m);
    if (j->blob_owned) blob_free(&j->blob);
    j->w = 0; j->s = 0; j->m = 0; j->blob_owned = false;
}

// Общая подготовка: каталог, файл, модуль, хранилище с пределами, окружение.
// 0 — готово; иначе причина отказа (всё взятое уже возвращено).
static const char* job_setup(job_t* j, const rune_opts_t* o, const uint8_t* bin, uint32_t bin_len,
                             rune_sink_t out, bool sync) {
    static char why_buf[200];
    uint64_t gen = j->gen;
    wasi_result_t last = j->last;
    int last_slot = j->last_slot;
    uint64_t last_cpu = j->last_cpu_ms, last_svc = j->last_services;
    for (size_t i = 0; i < sizeof(*j); i++) ((uint8_t*)j)[i] = 0;
    j->gen = gen; j->last = last; j->last_slot = last_slot; j->last_cpu_ms = last_cpu; j->last_services = last_svc;
    j->sink = out;
    j->owner = o->owner;
    j->sync = sync;
    j->read_only = o->read_only;
    j->no_files = o->no_files;
    j->interactive = o->interactive;
    j->cpu_limit_ms = o->cpu_ms;
    j->memory_mb = o->memory_mb;

    int pn = 0;
    for (; o->path[pn] && pn < (int)sizeof(j->path) - 1; pn++) j->path[pn] = o->path[pn];
    j->path[pn] = 0;
    if (!o->no_files) {
        const char* why = dir_check(o->dir, j->dir);
        if (why) return why;
    }
    if (o->nargs > RUNE_MAX_ARGV) return "too many arguments";
    // argv[0] — имя файла без каталогов, как привыкли программы. Аргументы
    // копируются: строки командной строки не доживут до конца программы.
    const char* base = j->path;
    for (const char* c = j->path; *c; c++) if (*c == '/') base = c + 1;
    j->av[0] = base;
    int an = 0;
    for (int i = 0; i < o->nargs; i++) {
        int l = 0;
        while (o->args[i][l]) l++;
        if (an + l + 1 > (int)sizeof(j->argbuf)) return "arguments are too long";
        j->av[1 + i] = &j->argbuf[an];
        for (int k = 0; k <= l; k++) j->argbuf[an++] = o->args[i][k];
    }

    if (bin) { j->blob.data = (uint8_t*)(uintptr_t)bin; j->blob.len = bin_len; j->blob.hpa = 0; }
    else {
        const char* why = blob_load(o->path, &j->blob);
        if (why) return why;
        j->blob_owned = true;
    }
    wasm_err_t err;
    j->m = wasm_module_load(j->blob.data, (uint32_t)j->blob.len, &err);
    if (!j->m) {
        ln_reset(); ln_s("rejected - "); ln_s(wasm_err_name(err.kind)); ln_s(": "); ln_s(err.text);
        for (int i = 0; i <= g_ln_n && i < (int)sizeof(why_buf); i++) why_buf[i] = g_ln[i];
        job_release(j);
        return why_buf;
    }
    // Предел памяти программы: страниц по 64 КБ. Больше программа не получит —
    // memory.grow ответит ей отказом, и это её дело, что с ним делать.
    wasm_config_t sc;
    for (size_t i = 0; i < sizeof(sc); i++) ((uint8_t*)&sc)[i] = 0;
    sc.max_memory_pages = o->memory_mb ? o->memory_mb * 16 : 0;
    j->s = wasm_store_new(&sc);
    if (!j->s) { job_release(j); return "not enough memory for the engine"; }

    krun_t* k = &g_run;
    for (size_t i = 0; i < sizeof(*k); i++) ((uint8_t*)k)[i] = 0;
    k->line_start = true;
    k->sink = out;
    run_clock_start(k);
    static const char* const env[3] = { "HOME=/", "PWD=/", "OS=AMPLIFY-OS" };
    j->pre.guest = "/"; j->pre.host = j->dir; j->pre.read_only = o->read_only;
    j->cfg.argc = o->nargs + 1; j->cfg.argv = j->av;
    j->cfg.envc = 3; j->cfg.envp = env;
    j->cfg.n_preopens = o->no_files ? 0 : 1; j->cfg.preopens = &j->pre;
    kp_fill(&j->cfg.host, k);
    j->cfg.host.in = kp_in;
    j->cfg.nonblocking = !sync;         // BSP ждать нельзя: сон и пустой ввод — «повтори позже»
    j->w = wasi_new(j->s, &j->cfg);
    if (!j->w) { job_release(j); return "not enough memory for the environment"; }
    j->t0_ms = timer_get_uptime_ms();
    return 0;
}

static void job_header(job_t* j) {
    ln_reset();
    ln_s("[rune] run "); ln_s(j->path);
    if (j->no_files) ln_s(" | no files");
    else { ln_s(" | dir "); ln_s(j->dir); ln_s(j->read_only ? " (read-only)" : ""); }
    if (j->sync) { ln_s(", main core, limit "); ln_u(j->cpu_limit_ms); ln_s(" ms\n"); }
    else {
        ln_s(", core "); ln_u((uint64_t)j->slot);
        if (j->cpu_limit_ms) { ln_s(", limit "); ln_u(j->cpu_limit_ms); ln_s(" ms"); }
        if (j->memory_mb) { ln_s(", memory "); ln_u(j->memory_mb); ln_s(" MB"); }
        ln_s("\n");
    }
    // [51.5] Чем исполняется — в самом конце строки: её начало разбирают тесты.
    {
        uint32_t jf = 0;
        if (j->s) wasm_store_jit_info(j->s, &jf, 0);
        g_ln_n--; g_ln[g_ln_n] = 0;
        ln_s(jf ? ", jit\n" : ", interpreter\n");
    }
    job_say(j, g_ln);
}

// Итог программы — в окно и в журнал; всё взятое — обратно ядру.
static void job_report(job_t* j, const wasi_result_t* r) {
    uint64_t ms = timer_get_uptime_ms() - j->t0_ms;
    if (!g_run.line_start && !g_run.capture) job_say(j, "\n");     // итог — всегда с новой строки
    ln_reset();
    if (r->status == WASI_RUN_NOSTART) {
        ln_s("[rune] run "); ln_s(j->path); ln_s(": "); ln_s(r->text); ln_s("\n");
    } else {
        if (r->status == WASI_RUN_EXIT) { ln_s("[rune] exit "); ln_u((uint64_t)(uint32_t)r->exit_code); }
        else {
            ln_s("[rune] stopped: ");
            if (r->trap == WASM_TRAP_STOPPED && j->stop_why) ln_s(j->stop_why);
            else ln_s(wasm_trap_name(r->trap));
        }
        ln_s(" | "); ln_u(ms); ln_s(" ms");
        if (!j->sync) { ln_s(", cpu "); ln_u(job_cpu_ms(j)); ln_s(" ms"); }
        ln_s(", fuel "); ln_u(j->s ? wasm_store_ticks(j->s) : 0);
        ln_s(", "); ln_u(r->calls); ln_s(" calls, memory "); ln_u(r->memory_pages); ln_s(" pages\n");
    }
    job_say(j, g_ln);
    if (r->unsaved) {
        ln_reset(); ln_s("[rune] warning: "); ln_u((uint64_t)r->unsaved); ln_s(" file(s) could not be saved\n");
        job_say(j, g_ln);
    }
    // [51.4] Итог — ещё и числами, для окна. Модуль, который не запустился
    // вовсе, программой не считается: это отказ (rune_last_error).
    if (r->status == WASI_RUN_NOSTART) refuse(r->text);
    else {
        g_end.kind = r->status == WASI_RUN_EXIT ? RUNE_END_EXIT : RUNE_END_STOPPED;
        g_end.exit_code = r->exit_code;
        const char* t = r->status == WASI_RUN_EXIT ? ""
                      : (r->trap == WASM_TRAP_STOPPED && j->stop_why) ? j->stop_why : wasm_trap_name(r->trap);
        int n = 0;
        for (; t[n] && n < (int)sizeof(g_end.text) - 1; n++) g_end.text[n] = t[n];
        g_end.text[n] = 0;
        g_end.cpu_ms = j->sync ? ms : job_cpu_ms(j);
        g_end.wall_ms = ms;
        g_end.pages = r->memory_pages;
        g_end.ends++;
    }
    j->last = *r;
    j->last_slot = j->slot;
    j->last_cpu_ms = job_cpu_ms(j);
    j->last_services = j->services;
}

// Конец программы (BSP). К этому моменту AP хранилище уже не трогает: либо он
// сам сообщил итог (JOB_DONE), либо спит и ждёт услугу (JOB_WAIT).
static void job_finish(job_t* j, int trap) {
    __atomic_store_n(&j->state, JOB_DONE, __ATOMIC_RELEASE);
    smp_pool_kick(j->slot);             // «жилец» увидит, что программа кончилась, и съедет
    if (wasm_store_paused(j->s)) wasm_abort(j->s);
    wasi_result_t r;
    for (size_t i = 0; i < sizeof(r); i++) ((uint8_t*)&r)[i] = 0;
    wasi_finish(j->s, j->w, trap, &r);
    job_report(j, &r);
    void (*done)(void) = j->on_done;
    job_release(j);
    g_stop = 0;
    j->gen++;                           // прежний «жилец» больше не наш
    __atomic_store_n(&j->state, JOB_NONE, __ATOMIC_RELEASE);
    if (done) done();
}

// ---- AP: отрезок счёта ----------------------------------------------------------------
static int rune_resident(void* arg) {
    job_t* j = &g_job;
    if ((uint64_t)(uintptr_t)arg != __atomic_load_n(&j->gen, __ATOMIC_ACQUIRE)) return -1;
    int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
    if (st == JOB_WAIT) return 0;                   // ждём BSP: ядро свободно
    if (st != JOB_RUN) return -1;
    if (!smp_pool_guest_enter(j->slot)) return 2;   // ядро нужно пулу прямо сейчас
    uint64_t c0 = tsc_now();
    int rc;
    if (j->first) { j->first = false; rc = wasm_call(j->s, j->start, 0, 0); }
    else rc = wasm_resume(j->s, 0);
    j->cpu_tsc += tsc_now() - c0;
    j->slices++;
    smp_pool_guest_leave(j->slot);
    if (rc == WASM_PAUSED_YIELD) return 1;
    if (rc == WASM_PAUSED_SERVICE) {
        __atomic_store_n(&j->state, JOB_WAIT, __ATOMIC_RELEASE);
        smp_pool_wake_bsp();
        return 0;
    }
    j->result = rc;
    __atomic_store_n(&j->state, JOB_DONE, __ATOMIC_RELEASE);
    smp_pool_wake_bsp();
    return -1;
}

// Какому ядру отдать программу. Старшее оставляем гостю phantom, первое —
// дирижёру MIND, пока есть выбор; занятое другим жильцом не годится вовсе.
static int job_pick_slot(job_t* j) {
    int senior = smp_pool_pick_resident();
    void* tag = (void*)(uintptr_t)j->gen;
    for (int s = 2; s < MAX_CPUS; s++)
        if (s != senior && smp_pool_set_resident(s, rune_resident, tag)) return s;
    if (senior > 1 && smp_pool_set_resident(senior, rune_resident, tag)) return senior;
    if (smp_pool_set_resident(1, rune_resident, tag)) return 1;
    return 0;
}

// ---- BSP: услуги программе -------------------------------------------------------------
bool rune_active(void) { return __atomic_load_n(&g_job.state, __ATOMIC_ACQUIRE) != JOB_NONE; }
void* rune_owner(void) { return rune_active() ? g_job.owner : 0; }
void  rune_disown(void) { g_job.owner = 0; }

// Есть ли у BSP работа для программы прямо сейчас (цикл WM не должен засыпать).
bool rune_wants_service(void) {
    job_t* j = &g_job;
    int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
    if (st == JOB_DONE) return true;
    if (st != JOB_WAIT) return false;
    if (g_stop) return true;
    if (j->waiting_input) return false;
    return !j->retry_ms || timer_get_uptime_ms() >= j->retry_ms;
}

// Одна услуга. true — оказана, программа пошла считать дальше (возможно, тут
// же попросит следующую); false — делать нечего, программа ждёт или кончилась.
static bool job_serve_one(job_t* j) {
    int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
    if (st == JOB_NONE) return false;
    if (st == JOB_DONE) { job_finish(j, j->result); return false; }
    if (st != JOB_WAIT) return false;
    if (g_stop) { job_finish(j, WASM_TRAP_STOPPED); return false; }
    if (j->waiting_input) return false;             // разбудит rune_input
    if (j->retry_ms && timer_get_uptime_ms() < j->retry_ms) return false;
    j->services++;
    int rc = wasm_service(j->s, 0);
    if (rc == WASM_PAUSED_YIELD) {
        j->retry_ms = 0;
        __atomic_store_n(&j->state, JOB_RUN, __ATOMIC_RELEASE);
        smp_pool_kick(j->slot);
        return true;
    }
    if (rc == WASM_PAUSED_SERVICE) {
        // Вызов не сделан: программа спит до срока либо ждёт ввода.
        j->retries++;
        uint64_t wake = wasi_wake_ns(j->w);
        j->retry_ms = wake ? (wake + 999999) / 1000000 : (j->waiting_input ? 0 : timer_get_uptime_ms() + 1);
        return false;
    }
    job_finish(j, rc);
    return false;
}

// Программа просит услуги пачками: чтение файла кусками, вывод строк, работа
// с каталогом. Если на каждую ждать следующего прохода цикла WM (а в нём —
// опрос портов и, бывает, кадр), выходит около двух миллисекунд на вызов.
// Поэтому оказываем услуги подряд, пока программа их просит, — но не дольше
// двух миллисекунд за проход: остальное время прохода принадлежит окнам. После
// каждой услуги коротко (0.1 мс) ждём следующую просьбу — программе нужно
// время дойти до неё.
void rune_tick(void) {
    job_t* j = &g_job;
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == JOB_NONE || j->sync) return;
    uint64_t t_end = tsc_now() + 2 * g_tsc_ms;
    while (job_serve_one(j)) {
        if (tsc_now() >= t_end) break;
        uint64_t w_end = tsc_now() + g_tsc_ms / 10;
        while (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == JOB_RUN && tsc_now() < w_end)
            __asm__ __volatile__("pause");
    }
}

void rune_stop(void) {
    job_t* j = &g_job;
    if (!rune_active()) return;
    if (!j->stop_why) j->stop_why = "by user";
    g_stop = 1;
    j->waiting_input = false;
}

void rune_input(const char* data, int n) {
    job_t* j = &g_job;
    if (!rune_active()) return;
    for (int i = 0; i < n; i++) {
        if (j->in_w - j->in_r >= sizeof(j->in)) break;      // переполнение: лишнее отбрасывается
        j->in[j->in_w++ % sizeof(j->in)] = (uint8_t)data[i];
    }
    j->waiting_input = false;
}
void rune_input_eof(void) {
    g_job.in_eof = true;
    g_job.waiting_input = false;
}

static bool job_start(const rune_opts_t* o, const uint8_t* bin, uint32_t bin_len, rune_sink_t out, void (*on_done)(void)) {
    job_t* j = &g_job;
    g_refuse[0] = 0;
    if (rune_active()) {
        refuse("another program is still running");
        ln_reset(); ln_s("[rune] run "); ln_s(o->path); ln_s(": another program is still running (rune stop)\n");
        if (out) out(g_ln); else serial_puts(g_ln);
        return false;
    }
    bool sync = smp_pool_workers() <= 0;
    const char* why = job_setup(j, o, bin, bin_len, out, sync);
    j->on_done = on_done;
    wasi_result_t r;
    if (!why) {
        if (sync && !j->cpu_limit_ms) j->cpu_limit_ms = RUNE_DEFAULT_LIMIT_MS;
        ln_reset(); ln_s("time limit "); ln_u(j->cpu_limit_ms); ln_s(" ms");
        for (int i = 0; i <= g_ln_n && i < (int)sizeof(j->why_limit); i++) j->why_limit[i] = g_ln[i];
        tsc_calibrate();
        // Запуск модуля (память, сегменты данных, функция start) идёт здесь, на
        // BSP: он выделяет память. Срок ему — короткий.
        g_stop = 0;
        wasm_store_limit(j->s, -1, &g_stop);
        g_deadline_ms = timer_get_uptime_ms() + RUNE_START_MS;
        bool ready = wasi_prepare(j->s, j->m, j->w, &r, &j->start);
        g_deadline_ms = 0;
        if (!ready) {
            j->slot = 0;
            j->quiet = (bin != 0);
            if (r.status != WASI_RUN_NOSTART) job_header(j);
            if (r.status == WASI_RUN_TRAP && r.trap == WASM_TRAP_STOPPED) j->stop_why = "the module took too long to start";
            job_report(j, &r);
            // Ловушка в самом запуске модуля: для окна это тоже отказ, а не
            // «программа отработала» — до своего кода она не дошла.
            if (r.status != WASI_RUN_NOSTART) { g_end.ends--; refuse(g_end.text[0] ? g_end.text : "the module failed to start"); }
            job_release(j);
            g_stop = 0;
            return false;
        }
    }
    if (why) {
        refuse(why);
        ln_reset();
        ln_s("[rune] run "); ln_s(o->path); ln_s(": "); ln_s(why); ln_s("\n");
        if (out) out(g_ln); else serial_puts(g_ln);
        return false;
    }
    j->quiet = (bin != 0);

    if (sync) {
        // Второго ядра нет: программа исполняется здесь, как в 51.2 — интерфейс
        // стоит, поэтому предел времени обязателен.
        job_header(j);
        j->stop_why = j->why_limit;
        g_deadline_ms = timer_get_uptime_ms() + j->cpu_limit_ms;
        int trap = wasm_call(j->s, j->start, 0, 0);
        g_deadline_ms = 0;
        wasi_finish(j->s, j->w, trap, &r);
        job_report(j, &r);
        job_release(j);
        g_stop = 0;
        return true;
    }

    // Счёт — на AP отрезками; привилегированные команды — через rune_tick.
    wasm_store_remote(j->s, true);
    wasm_store_slice(j->s, RUNE_SLICE_UNITS, 0);
    j->first = true;
    j->gen++;
    __atomic_store_n(&j->state, JOB_RUN, __ATOMIC_RELEASE);
    j->slot = job_pick_slot(j);
    // Ядро прошлой программы могло ещё не заметить, что она кончилась: ждём недолго.
    for (uint64_t until = timer_get_uptime_ms() + 30; !j->slot && timer_get_uptime_ms() < until;) j->slot = job_pick_slot(j);
    if (!j->slot) {
        __atomic_store_n(&j->state, JOB_NONE, __ATOMIC_RELEASE);
        job_release(j);
        refuse("no free core");
        ln_reset(); ln_s("[rune] run "); ln_s(o->path); ln_s(": no free core (every core already hosts a guest)\n");
        if (out) out(g_ln); else serial_puts(g_ln);
        return false;
    }
    job_header(j);
    return true;
}

// Запуск. true — программа пошла (или, на машине без второго ядра, уже отработала).
bool rune_start(const rune_opts_t* o, rune_sink_t out) {
    return job_start(o, 0, 0, out, 0);
}

void rune_status(rune_sink_t out) {
    g_out = out;
    job_t* j = &g_job;
    ln_reset();
    if (!rune_active()) {
        ln_s("[rune] status: idle");
        if (j->last_slot || j->last.calls) {
            ln_s(" | last program: core "); ln_u((uint64_t)j->last_slot); ln_s(", cpu "); ln_u(j->last_cpu_ms);
            ln_s(" ms, "); ln_u(j->last_services); ln_s(" services");
        }
        ln_s("\n");
    } else {
        int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
        ln_s("[rune] status: running "); ln_s(j->path); ln_s(" on core "); ln_u((uint64_t)j->slot);
        ln_s(" | "); ln_s(st == JOB_RUN ? "computing" : j->waiting_input ? "waiting for input" : j->retry_ms ? "sleeping" : "in a system call");
        ln_s(", cpu "); ln_u(job_cpu_ms(j)); ln_s(" ms of "); ln_u(timer_get_uptime_ms() - j->t0_ms);
        ln_s(" ms, "); ln_u(j->slices); ln_s(" slices, "); ln_u(j->services); ln_s(" services\n");
    }
    emit(g_ln);
    g_out = 0;
}

// ---- [51.4] для окна Rune ------------------------------------------------------------
void rune_query(rune_state_t* q) {
    job_t* j = &g_job;
    for (size_t i = 0; i < sizeof(*q); i++) ((uint8_t*)q)[i] = 0;
    int st = __atomic_load_n(&j->state, __ATOMIC_ACQUIRE);
    if (st != JOB_NONE) {
        q->active = true;
        q->state = st == JOB_RUN ? RUNE_ST_COMPUTING : j->waiting_input ? RUNE_ST_INPUT
                 : j->retry_ms ? RUNE_ST_SLEEPING : RUNE_ST_SYSCALL;
        q->core = j->sync ? 0 : j->slot;
        q->owner = j->owner;
        for (int i = 0; i < (int)sizeof(q->path) - 1 && j->path[i]; i++) q->path[i] = j->path[i];
        q->cpu_ms = job_cpu_ms(j);
        q->wall_ms = timer_get_uptime_ms() - j->t0_ms;
        q->services = j->services;
        q->memory_pages = wasi_memory_pages(j->w);
        q->memory_limit_mb = j->memory_mb;
        q->input_open = j->interactive && !j->in_eof;
        uint32_t jf = 0;
        if (j->s) wasm_store_jit_info(j->s, &jf, 0);
        q->jit = jf != 0;
    }
    q->ends = g_end.ends;
    q->end_kind = g_end.kind;
    q->exit_code = g_end.exit_code;
    for (int i = 0; i < (int)sizeof(q->end_text); i++) q->end_text[i] = g_end.text[i];
    q->end_cpu_ms = g_end.cpu_ms;
    q->end_wall_ms = g_end.wall_ms;
    q->end_pages = g_end.pages;
}

const char* rune_last_error(void) { return g_refuse; }

// ---- библиотека программ ----
static bool ends_wasm(const char* name) {
    int n = 0;
    while (name[n]) n++;
    return n > 5 && ieq_n(name + n - 5, ".wasm", 5);
}

// Пользовательская секция «rune.about»: программа описывает себя одной строкой
// (в исходнике — массив в секции `.custom_section.rune.about`). Модуль здесь НЕ
// проверяется и не загружается — идём только по заголовкам секций, и ни одному
// числу из файла не верим: каждое сверяется с остатком байт.
static bool leb_u32(const uint8_t* d, uint64_t len, uint64_t* at, uint32_t* out) {
    uint32_t v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
        if (*at >= len) return false;
        uint8_t b = d[(*at)++];
        v |= (uint32_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;
}
static void module_about(const uint8_t* d, uint64_t len, char* out, int cap) {
    static const char want[] = "rune.about";
    out[0] = 0;
    if (len < 8 || d[0] != 0 || d[1] != 'a' || d[2] != 's' || d[3] != 'm') return;
    for (uint64_t at = 8; at < len;) {
        uint8_t id = d[at++];
        uint32_t size, nlen;
        if (!leb_u32(d, len, &at, &size) || size > len - at) return;
        uint64_t end = at + size, p = at;
        at = end;
        if (id != 0) continue;
        if (!leb_u32(d, end, &p, &nlen) || nlen > end - p) return;
        if (nlen != sizeof(want) - 1 || !ieq_n((const char*)d + p, want, (int)nlen)) continue;
        p += nlen;
        static char u8[200];
        int n = 0;
        for (; p < end && n < (int)sizeof(u8) - 1 && d[p] >= 0x20; p++) u8[n++] = (char)d[p];
        n -= utf8_tail(u8, n);                      // строку могло обрезать посреди символа
        int m = mind_to_cp1251(u8, n, out, cap - 1);
        out[m < 0 ? 0 : m] = 0;
        return;
    }
}

typedef struct { rune_prog_t* out; int max, n; } prog_list_t;
static void prog_add(prog_list_t* L, const char* dir, const char* name, uint32_t bytes, bool builtin) {
    if (L->n >= L->max || !ends_wasm(name)) return;
    rune_prog_t* p = &L->out[L->n];
    for (size_t i = 0; i < sizeof(*p); i++) ((uint8_t*)p)[i] = 0;
    int k = 0, n = 0;
    for (; dir[k] && k < (int)sizeof(p->path) - 1; k++) p->path[k] = dir[k];
    for (; name[n] && k < (int)sizeof(p->path) - 1; n++, k++) p->path[k] = name[n];
    if (name[n]) return;                            // путь не поместился — такую не запустить
    for (n = 0; name[n] && n < (int)sizeof(p->name) - 1; n++) p->name[n] = name[n];
    p->bytes = bytes;
    p->builtin = builtin;
    L->n++;
}
static int prog_fat_cb(const fat32_dir_entry_t* e, void* user) {
    if (!e->is_dir) prog_add((prog_list_t*)user, RUNE_PROG_DIR "/", e->name, e->size, false);
    return 0;
}

int rune_programs(rune_prog_t* out, int max) {
    prog_list_t L = { out, max, 0 };
    // Встроенные: initrd плоский, имя файла в нём — оно же путь для VFS.
    uint64_t isz = 0;
    const uint8_t* img = initrd_image(&isz);
    if (img && isz >= sizeof(initrd_header_t)) {
        const initrd_header_t* h = (const initrd_header_t*)img;
        const initrd_file_header_t* fh = (const initrd_file_header_t*)(img + sizeof(*h));
        for (uint32_t i = 0; i < h->num_files && i < INITRD_MAX_FILES; i++) {
            char nm[INITRD_MAX_FILENAME + 1];
            int k = 0;
            for (; k < INITRD_MAX_FILENAME && fh[i].name[k]; k++) nm[k] = fh[i].name[k];
            nm[k] = 0;
            int before = L.n;
            prog_add(&L, "", nm, fh[i].size, true);
            if (L.n > before && (uint64_t)fh[i].offset + fh[i].size <= isz)
                module_about(img + fh[i].offset, fh[i].size, out[before].about, (int)sizeof(out[before].about));
        }
    }
    // Свои: каталог на диске. Его заводим сами — чтобы было куда положить.
    int first_disk = L.n;
    if (fat32_is_mounted()) {
        fat32_dir_entry_t de;
        if (fat32_stat(RUNE_PROG_DIR, &de) != 0 && fat32_stat("/disk/vault", &de) == 0 && de.is_dir)
            (void)fat32_mkdir(RUNE_PROG_DIR);
        (void)fat32_iter_dir(RUNE_PROG_DIR, prog_fat_cb, &L);
    }
    // Описания читаем ПОСЛЕ обхода каталога: чтение файла внутри обхода
    // испортило бы драйверу FAT его же буфер каталога. Большой модуль ради
    // одной строки целиком в память не тянем.
    for (int i = first_disk; i < L.n; i++) {
        if (out[i].bytes > (2u << 20)) continue;
        blob_t b;
        if (blob_load(out[i].path, &b) != 0) continue;
        module_about(b.data, b.len, out[i].about, (int)sizeof(out[i].about));
        blob_free(&b);
    }
    return L.n;
}

// Самопроверка: программа WASI на другом ядре, вывод и итог приходят на BSP.
static int run_core_selftest(const char* prefix) {
    int fails = 0;
    uint64_t t0 = timer_get_uptime_ms();
    if (smp_pool_workers() <= 0) {
        if (!check(prefix, "no second core: programs run on the main core", true, "nothing to test")) fails++;
    } else {
        heap_stats_t h0 = heap_get_stats();
        uint64_t ram0 = pmm_free_ram();
        static char got[32];
        rune_opts_t o;
        for (size_t i = 0; i < sizeof(o); i++) ((uint8_t*)&o)[i] = 0;
        o.path = "selftest"; o.no_files = true;
        got[0] = 0;
        bool started = job_start(&o, k_mod_wasi, sizeof(k_mod_wasi), 0, 0);
        g_run.capture = got; g_run.cap_max = (int)sizeof(got); g_run.cap_n = 0;
        // Цикла WM ещё может не быть (загрузка): услуги оказываем сами.
        uint64_t until = timer_get_uptime_ms() + 500;
        while (started && rune_active() && timer_get_uptime_ms() < until) {
            rune_tick();
            if (!rune_wants_service()) __asm__ __volatile__("pause");
        }
        bool ended = !rune_active();
        if (!ended) { rune_stop(); for (int i = 0; i < 200000 && rune_active(); i++) rune_tick(); }
        g_run.capture = 0;
        job_t* j = &g_job;
        ln_reset(); ln_s("core "); ln_u((uint64_t)j->last_slot); ln_s(", "); ln_u(j->last_services); ln_s(" services through the main core");
        static char d[96];
        for (int i = 0; i <= g_ln_n && i < (int)sizeof(d); i++) d[i] = g_ln[i];
        bool ok = started && ended && j->last.status == WASI_RUN_EXIT && j->last.exit_code == 7 && j->last_slot >= 1 &&
                  j->last_services >= 2 && got[0] == 'r' && got[3] == 'e' && got[4] == '\n';
        if (!check(prefix, "a WASI program runs on another core; output and exit code arrive", ok, d)) fails++;
        heap_stats_t h1 = heap_get_stats();
        ok = h0.used_size == h1.used_size && pmm_free_ram() == ram0;
        if (!check(prefix, "everything is returned to the kernel afterwards", ok, "")) fails++;
    }
    ln_reset();
    ln_s(prefix); ln_s("done in "); ln_u(timer_get_uptime_ms() - t0); ln_s(" ms: ");
    ln_s(fails ? "FAIL\n" : "PASS\n");
    emit(g_ln);
    return fails;
}

// Раз в миллисекунду (обработчик таймера): предел времени счёта программы на AP.
static void job_tick_1ms(void) {
    job_t* j = &g_job;
    if (!j->cpu_limit_ms || j->sync || g_stop || !g_tsc_ms) return;
    if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) != JOB_RUN) return;
    if (j->cpu_tsc / g_tsc_ms >= j->cpu_limit_ms) { j->stop_why = j->why_limit; g_stop = 1; }
}

bool rune_boot_selftest(void) {
    g_out = 0;
    serial_puts("[smoke 51] WebAssembly engine \"rune\": binary format + validation (WebAssembly 2.0 without SIMD)\n");
    bool ok = run_selftest("[smoke 51] ") == 0;
    serial_puts("[smoke 51.1] interpreter: calls, traps, limits, imports, program memory\n");
    ok = (run_exec_selftest("[smoke 51.1] ") == 0) && ok;
    serial_puts("[smoke 51.2] WASI preview1: program output, exit code\n");
    ok = (run_wasi_selftest("[smoke 51.2] ") == 0) && ok;
    serial_puts("[smoke 51.3] a program on its own core, served by the main core\n");
    ok = (run_core_selftest("[smoke 51.3] ") == 0) && ok;
    serial_puts("[smoke 51.5] JIT: machine code x86-64 from the internal code\n");
    return (run_jit_selftest("[smoke 51.5] ") == 0) && ok;
}

// Сколько проверок в `rune selftest`: 4 (формат) + 6 (счёт) + 2 (WASI) + ядро
// (2 проверки; на машине без второго ядра — одна строка-пометка) + 2 (JIT).
#define RUNE_SELFTESTS (smp_pool_workers() > 0 ? 16 : 15)
int rune_selftest(rune_sink_t out) {
    g_out = out;
    int fails = run_selftest("[rune] test ") + run_exec_selftest("[rune] test ") + run_wasi_selftest("[rune] test ") +
                run_core_selftest("[rune] test ") + run_jit_selftest("[rune] test ");
    ln_reset();
    ln_s("[rune] selftest: "); ln_u((uint64_t)(RUNE_SELFTESTS - (fails > RUNE_SELFTESTS ? RUNE_SELFTESTS : fails))); ln_s(" passed, "); ln_u((uint64_t)fails); ln_s(" failed\n");
    emit(g_ln);
    g_out = 0;
    return fails;
}

bool rune_info(rune_sink_t out, const char* path) {
    g_out = out;
    blob_t b;
    const char* why = blob_load(path, &b);
    wasm_module_t* m = 0;
    wasm_err_t err;
    if (!why) {
        m = wasm_module_load(b.data, (uint32_t)b.len, &err);
        if (!m) {
            ln_reset();
            ln_s("[rune] info "); ln_s(path); ln_s(": rejected - "); ln_s(wasm_err_name(err.kind));
            ln_s(": "); ln_s(err.text); ln_s(" (offset "); ln_u(err.offset); ln_s(")\n");
            emit(g_ln);
        }
    } else {
        ln_reset();
        ln_s("[rune] info "); ln_s(path); ln_s(": "); ln_s(why); ln_s("\n");
        emit(g_ln);
    }
    if (m) {
        ln_reset();
        ln_s("[rune] info "); ln_s(path); ln_s(": valid module\n");
        emit(g_ln);
        wasm_describe(m, emit, "[rune]   ", 24);
        wasm_module_free(m);
    }
    blob_free(&b);
    g_out = 0;
    return m != 0;
}

bool rune_spec(rune_sink_t out, const char* path, bool verbose) {
    g_out = out;
    blob_t b;
    const char* why = blob_load(path, &b);
    bool ok = false;
    if (why) {
        ln_reset();
        ln_s("[rune] spec "); ln_s(path); ln_s(": "); ln_s(why); ln_s("\n");
        emit(g_ln);
    } else {
        heap_stats_t h0 = heap_get_stats();
        uint64_t t0 = timer_get_uptime_ms();
        wasm_spec_stats_t st;
        ok = wasm_spec_run(b.data, b.len, emit, verbose, &st) && st.failures == 0;
        heap_stats_t h1 = heap_get_stats();
        ln_reset();
        ln_s("[rune] spec: "); ln_s(ok ? "PASS" : "FAIL"); ln_s(" in "); ln_u(timer_get_uptime_ms() - t0);
        ln_s(" ms, pack "); ln_u(b.len); ln_s(" bytes, kernel heap ");
        ln_s(h0.used_size == h1.used_size ? "unchanged" : "CHANGED"); ln_s("\n");
        emit(g_ln);
    }
    blob_free(&b);
    g_out = 0;
    return ok;
}

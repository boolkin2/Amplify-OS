// ============================================================================
// wasm_host.c — движок WebAssembly AMPLIFY-OS, собранный на хосте (этап 51).
// ----------------------------------------------------------------------------
// Те же файлы, что в ядре (wasm_bin.c, wasm_valid.c, wasm_rt.c, wasm_interp.c,
// wasm_spec.c), с обычной
// libc вокруг: память — malloc, вывод — stdout. Нужен, чтобы гонять
// официальный набор тестов за секунды и смотреть на модули, не загружая ОС.
// Тот же пакет потом проходит ядро (`rune spec`) — цифры должны совпасть.
//
//   build/wasm_host spec <пакет> [-v]   — прогон набора тестов (код 0 — без провалов)
//   build/wasm_host info <файл.wasm>    — что в модуле; код 1 — модуль отклонён
//   build/wasm_host run [-d каталог] [-r] [-e ИМЯ=значение] <файл.wasm> [аргументы]
//                                       — запустить программу WASI (51.2): каталог
//                                         она видит как "/", вывод идёт в stdout
//   build/wasm_host call <файл.wasm> <функция> [аргументы]
//                                       — вызвать функцию; строка итога — того же
//                                         вида, что у `rune call` в ядре
//   build/wasm_host fuzz <файл> <сколько> [зерно]       — портить модуль и загружать
//   build/wasm_host fuzz-exec <файл> <сколько> [зерно]  — … и исполнять уцелевшие
// Сборка: make -f Makefile.x86_64 wasm-host
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "../kernel/arch/x86_64/wasm.h"
#include "../kernel/arch/x86_64/wasm_wasi.h"

void* wasm_port_alloc(size_t n) { return malloc(n ? n : 1); }
void  wasm_port_free(void* p)   { free(p); }
void* wasm_port_mem_alloc(uint64_t bytes) { return calloc(bytes ? bytes : 1, 1); }
void  wasm_port_mem_free(void* p, uint64_t bytes) { (void)bytes; free(p); }
// [51.5] Машинный код JIT: страницы, из которых можно исполнять.
void* wasm_port_exec_alloc(uint64_t bytes) {
    void* p = mmap(0, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
void  wasm_port_exec_free(void* p, uint64_t bytes) { if (p) munmap(p, bytes); }

static void sink(const char* s) { fputs(s, stdout); }

static uint8_t* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "cannot read %s\n", path); fclose(f); free(buf); return 0; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

// ---- [51.2] окружение WASI на хосте --------------------------------------------------
// Тот же слой wasm_wasi.c, что в ядре; здесь — только то, что он просит у
// хозяина: обычные файлы POSIX вместо FAT, stdout вместо окна терминала.
static int hp_errno(void) {
    switch (errno) {
    case ENOENT:       return WASI_ENOENT;
    case EEXIST:       return WASI_EEXIST;
    case ENOTDIR:      return WASI_ENOTDIR;
    case EISDIR:       return WASI_EISDIR;
    case ENOTEMPTY:    return WASI_ENOTEMPTY;
    case EACCES:       return WASI_EACCES;
    case ENOSPC:       return WASI_ENOSPC;
    case ENAMETOOLONG: return WASI_ENAMETOOLONG;
    default:           return WASI_EIO;
    }
}
static const char* hp_path(const char* p) { return p[0] ? p : "/"; }

static void hp_out(void* ctx, int fd, const uint8_t* data, uint32_t n) { (void)ctx; fwrite(data, 1, n, fd == 2 ? stderr : stdout); }
static void hp_out_null(void* ctx, int fd, const uint8_t* data, uint32_t n) { (void)ctx; (void)fd; (void)data; (void)n; }
static uint32_t hp_in(void* ctx, uint8_t* buf, uint32_t n) { (void)ctx; return (uint32_t)fread(buf, 1, n, stdin); }

static int hp_stat(void* ctx, const char* path, wasi_stat_t* st) {
    (void)ctx;
    struct stat s;
    if (stat(hp_path(path), &s) != 0) return hp_errno();
    st->is_dir = S_ISDIR(s.st_mode);
    st->size = (uint64_t)s.st_size;
    st->mtime_ns = (uint64_t)s.st_mtime * 1000000000ull;
    return WASI_OK;
}
static int hp_load(void* ctx, const char* path, uint8_t* buf, uint64_t size) {
    (void)ctx;
    FILE* f = fopen(hp_path(path), "rb");
    if (!f) return hp_errno();
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    return got == size ? WASI_OK : WASI_EIO;
}
static int hp_save(void* ctx, const char* path, const uint8_t* data, uint64_t size) {
    (void)ctx;
    FILE* f = fopen(hp_path(path), "wb");
    if (!f) return hp_errno();
    size_t put = fwrite(data, 1, (size_t)size, f);
    return (fclose(f) == 0 && put == size) ? WASI_OK : WASI_EIO;
}
static int hp_mkdir(void* ctx, const char* path) { (void)ctx; return mkdir(hp_path(path), 0777) == 0 ? WASI_OK : hp_errno(); }
static int hp_remove(void* ctx, const char* path, bool dir) {
    (void)ctx;
    return (dir ? rmdir(hp_path(path)) : unlink(hp_path(path))) == 0 ? WASI_OK : hp_errno();
}
static int hp_rename(void* ctx, const char* from, const char* to) { (void)ctx; return rename(hp_path(from), hp_path(to)) == 0 ? WASI_OK : hp_errno(); }
static int hp_list(void* ctx, const char* path, wasi_list_fn cb, void* user) {
    (void)ctx;
    DIR* d = opendir(hp_path(path));
    if (!d) return hp_errno();
    struct dirent* e;
    while ((e = readdir(d)) != 0) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char full[4096];
        struct stat s;
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        bool is_dir = stat(full, &s) == 0 && S_ISDIR(s.st_mode);
        if (!cb(user, e->d_name, is_dir)) break;
    }
    closedir(d);
    return WASI_OK;
}
static uint64_t hp_clock(void* ctx, bool realtime) {
    (void)ctx;
    struct timespec ts;
    clock_gettime(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static void hp_random(void* ctx, uint8_t* buf, uint32_t n) {
    (void)ctx;
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, n, f) != n) for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)rand();
    if (f) fclose(f);
}
static bool hp_sleep(void* ctx, uint64_t ns) {
    (void)ctx;
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, 0);
    return true;
}
static bool hp_sleep_none(void* ctx, uint64_t ns) { (void)ctx; (void)ns; return true; }

static void hp_fill(wasi_host_t* h, bool quiet) {
    memset(h, 0, sizeof(*h));
    h->out = quiet ? hp_out_null : hp_out;
    h->in = quiet ? 0 : hp_in;
    h->stat = hp_stat; h->load = hp_load; h->save = hp_save; h->mkdir = hp_mkdir; h->remove = hp_remove;
    // WASI_NO_RENAME=1 — как в ядре, где драйвер FAT переименовывать не умеет:
    // так проверяется запасной путь слоя (копия + удаление).
    h->rename = getenv("WASI_NO_RENAME") ? 0 : hp_rename;
    h->list = hp_list; h->clock_ns = hp_clock; h->random = hp_random;
    h->sleep_ns = quiet ? hp_sleep_none : hp_sleep;
}

// build/wasm_host run [-d каталог] [-r] [-e ИМЯ=значение]... <файл.wasm> [аргументы]
// Каталог (по умолчанию текущий) программа видит как "/"; -r — только чтение.
// Код возврата — код программы; 100 + причина, если её остановил движок.
static int cmd_run(int argc, char** argv) {
    static char dir[1024] = ".";
    const char* env[32];
    int n_env = 0, at = 2;
    bool ro = false;
    for (; at < argc && argv[at][0] == '-'; at++) {
        if (!strcmp(argv[at], "-d") && at + 1 < argc) snprintf(dir, sizeof(dir), "%s", argv[++at]);
        else if (!strcmp(argv[at], "-r")) ro = true;
        else if (!strcmp(argv[at], "-e") && at + 1 < argc && n_env < 32) env[n_env++] = argv[++at];
        else break;
    }
    if (at >= argc) { fprintf(stderr, "usage: wasm_host run [-d dir] [-r] [-e NAME=value] <file.wasm> [args]\n"); return 2; }
    size_t dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dir[--dl] = 0;
    if (!strcmp(dir, "/")) dir[0] = 0;              // корень хозяина: пути и так начнутся с '/'

    size_t len;
    uint8_t* bin = read_file(argv[at], &len);
    if (!bin) return 2;
    wasm_err_t err;
    wasm_module_t* m = wasm_module_load(bin, (uint32_t)len, &err);
    if (!m) { fprintf(stderr, "wasm_host: rejected - %s: %s\n", wasm_err_name(err.kind), err.text); free(bin); return 2; }
    wasm_store_t* s = wasm_store_new(0);
    wasi_preopen_t pre = { "/", dir, ro };
    wasi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.argc = argc - at;
    cfg.argv = (const char* const*)&argv[at];
    cfg.envc = n_env;
    cfg.envp = env;
    cfg.n_preopens = 1;
    cfg.preopens = &pre;
    hp_fill(&cfg.host, false);
    wasi_t* w = s ? wasi_new(s, &cfg) : 0;
    int rc = 2;
    if (!w) fprintf(stderr, "wasm_host: out of memory\n");
    else {
        const char* fe = getenv("WASM_FUEL");
        wasm_store_limit(s, fe ? atoll(fe) : -1, 0);
        wasi_result_t r;
        if (!getenv("WASM_STEPPED")) wasi_run(s, m, w, &r);
        else {
            // Как в ядре (51.3), только в одном потоке: программа идёт отрезками
            // в один виток, а функции окружения и рост памяти исполняет
            // wasm_service — «другое ядро».
            wasm_funcinst_t* start;
            if (wasi_prepare(s, m, w, &r, &start)) {
                wasm_store_remote(s, true);
                wasm_store_slice(s, 1, 1);
                long pauses = 0, services = 0;
                int t = wasm_call(s, start, 0, 0);
                while (t < 0) {
                    if (t == WASM_PAUSED_SERVICE) { services++; t = wasm_service(s, 0); }
                    else { pauses++; t = wasm_resume(s, 0); }
                }
                wasi_finish(s, w, t, &r);
                if (getenv("WASM_STATS")) fprintf(stderr, "wasm_host: stepped: %ld pauses, %ld services\n", pauses, services);
            }
        }
        fflush(stdout);
        if (r.status == WASI_RUN_EXIT) rc = r.exit_code & 0xFF;
        else if (r.status == WASI_RUN_TRAP) { fprintf(stderr, "wasm_host: stopped: %s\n", wasm_trap_name(r.trap)); rc = 100 + r.trap; }
        else fprintf(stderr, "wasm_host: %s\n", r.text);
        if (r.unsaved) fprintf(stderr, "wasm_host: %d file(s) could not be saved\n", r.unsaved);
        if (getenv("WASM_STATS")) fprintf(stderr, "wasm_host: %llu calls to the environment, fuel %llu, memory %u pages\n",
                                          (unsigned long long)r.calls, (unsigned long long)wasm_store_ticks(s), r.memory_pages);
        if (getenv("WASM_STATS")) {
            // [51.5] Сколько функций переведено в машинный код и сколько его вышло.
            uint32_t jf = 0; uint64_t jb = 0;
            wasm_store_jit_info(s, &jf, &jb);
            fprintf(stderr, "wasm_host: jit: %u functions, %llu bytes of machine code\n", jf, (unsigned long long)jb);
        }
        wasi_free(w);
    }
    if (s) wasm_store_free(s);
    wasm_module_free(m);
    free(bin);
    return rc;
}

// ---- [51.1] исполнение при фаззинге ------------------------------------------------
// Интерпретатор доверяет проверке: не сверяет типы и глубину стека. Значит,
// любая её ошибка стала бы чтением или записью мимо. Поэтому испорченные
// модули, которые проверка ПРИНЯЛА, запускаются: импорты подставляются
// заглушками, каждая экспортированная функция вызывается с небольшим запасом
// «топлива». Под санитайзером адресов любой выход за буфер остановит утилиту.
static int host_zero(wasm_instance_t* caller, wasm_val_t* vals, void* user) {
    (void)caller;
    for (uintptr_t i = 0; i < (uintptr_t)user; i++) vals[i] = 0;
    return WASM_TRAP_NONE;
}

// Дать модулю всё, что он просит: функцию нужного типа, таблицу, память,
// глобальную — ровно такие, какие заявлены в импорте.
static bool stub_resolve(void* ctx, const wasm_module_t* m, const wasm_import_t* imp, wasm_extern_t* out) {
    wasm_store_t* s = (wasm_store_t*)ctx;
    out->kind = imp->kind;
    switch (imp->kind) {
    case WK_FUNC: {
        const wasm_functype_t* ft = &m->types[m->funcs[imp->index].type];
        out->ptr = wasm_host_func(s, ft->types, ft->n_params, ft->types + ft->n_params, ft->n_results,
                                  host_zero, (void*)(uintptr_t)ft->n_results);
        break;
    }
    case WK_TABLE: {
        const wasm_tabletype_t* t = &m->tables[imp->index];
        out->ptr = wasm_table_new(s, t->elem, t->lim.min, t->lim.max, t->lim.has_max);
        break;
    }
    case WK_MEM:
        out->ptr = wasm_memory_new(s, m->mems[0].lim.min, m->mems[0].lim.max, m->mems[0].lim.has_max);
        break;
    default:
        out->ptr = wasm_global_new(s, m->globals[imp->index].type, m->globals[imp->index].mut, 0);
        break;
    }
    return out->ptr != 0;
}

enum { FX_STARTED, FX_NOT_STARTED, FX_CALLS, FX_TRAPS, FX_N };

// [51.2] С WASM_FUZZ_DIR=<каталог> испорченные программы получают настоящее
// окружение WASI (каталог — их "/"), а не заглушки: так под санитайзером
// оказывается и разбор всего, что программа передаёт окружению (указатели,
// длины, пути). Остальные импорты по-прежнему закрываются заглушками.
typedef struct { wasm_store_t* s; wasi_t* w; } fuzz_ctx_t;
static bool fuzz_resolve(void* ctx, const wasm_module_t* m, const wasm_import_t* imp, wasm_extern_t* out) {
    fuzz_ctx_t* f = (fuzz_ctx_t*)ctx;
    if (f->w && wasi_resolve(f->w, m, imp, out)) return true;
    return stub_resolve(f->s, m, imp, out);
}

static void fuzz_exec(const wasm_module_t* m, long* st, long* by_trap) {
    // Хранилище маленькое: его создают десятки тысяч раз.
    wasm_config_t cfg = { 64, 4096, 16384, 256 };
    wasm_store_t* s = wasm_store_new(&cfg);
    if (!s) return;
    // Запас «топлива» на вызов. С окружением WASI его стоит давать больше
    // (WASM_FUZZ_FUEL): программе нужно пройти запуск libc, чтобы добраться до
    // файлов.
    const char* ff = getenv("WASM_FUZZ_FUEL");
    long fuzz_fuel = ff ? atol(ff) : 64;
    wasm_store_limit(s, fuzz_fuel, 0);
    const char* jail = getenv("WASM_FUZZ_DIR");
    static const char* const fz_argv[] = { "fuzz", "one", "two" };
    wasi_preopen_t pre = { "/", jail ? jail : "", false };
    wasi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    wc.argc = 3; wc.argv = fz_argv;
    wc.n_preopens = 1; wc.preopens = &pre;
    wc.max_file_bytes = 1 << 20; wc.max_total_bytes = 4 << 20;
    hp_fill(&wc.host, true);
    fuzz_ctx_t fc = { s, jail ? wasi_new(s, &wc) : 0 };
    wasm_inst_err_t ierr;
    wasm_instance_t* inst = wasm_instantiate(s, m, fuzz_resolve, &fc, &ierr);
    if (!inst) st[FX_NOT_STARTED]++;
    else {
        st[FX_STARTED]++;
        static wasm_val_t args[WASM_MAX_PARAMS], res[WASM_MAX_RESULTS];
        int done = 0;
        for (uint32_t i = 0; i < m->n_exports && done < 8; i++) {
            wasm_extern_t ex;
            if (m->exports[i].kind != WK_FUNC) continue;
            if (!wasm_instance_export(inst, m->exports[i].name.p, m->exports[i].name.n, &ex)) continue;
            const wasm_functype_t* ft = wasm_func_type((wasm_funcinst_t*)ex.ptr);
            // Числа — произвольные; ссылки — только пустые (настоящую ссылку
            // программе даёт движок, выдумать её снаружи нельзя).
            for (uint32_t k = 0; k < ft->n_params; k++) {
                uint8_t t = ft->types[k];
                args[k] = (t == WT_FUNCREF || t == WT_EXTERNREF) ? 0 : (uint32_t)((i + 1) * 2654435761u + k * 40503u);
            }
            wasm_store_limit(s, fuzz_fuel, 0);
            int trap;
            if (!getenv("WASM_STEPPED")) trap = wasm_call(s, (wasm_funcinst_t*)ex.ptr, args, res);
            else {
                // Отрезки по три единицы «топлива» и услуги — как на другом ядре.
                wasm_store_remote(s, true);
                wasm_store_slice(s, 3, 0);
                trap = wasm_call(s, (wasm_funcinst_t*)ex.ptr, args, res);
                while (trap < 0) trap = trap == WASM_PAUSED_SERVICE ? wasm_service(s, res) : wasm_resume(s, res);
                wasm_store_remote(s, false);
                wasm_store_slice(s, 0, 0);
            }
            st[FX_CALLS]++;
            if (trap) { st[FX_TRAPS]++; by_trap[trap & 15]++; }
            done++;
        }
    }
    if (fc.w) wasi_free(fc.w);
    wasm_store_free(s);
}

// Целое как в `rune call`: [-]десятичное или [-]0x…
static uint64_t arg_int(const char* a) {
    bool neg = a[0] == '-';
    if (neg) a++;
    uint64_t v = (a[0] == '0' && (a[1] == 'x' || a[1] == 'X')) ? strtoull(a + 2, 0, 16) : strtoull(a, 0, 10);
    return neg ? 0 - v : v;
}

static int cmd_call(int argc, char** argv) {
    size_t len;
    uint8_t* bin = read_file(argv[2], &len);
    if (!bin) return 2;
    wasm_err_t err;
    wasm_module_t* m = wasm_module_load(bin, (uint32_t)len, &err);
    if (!m) { printf("call %s: rejected - %s: %s\n", argv[2], wasm_err_name(err.kind), err.text); free(bin); return 1; }
    wasm_store_t* s = wasm_store_new(0);
    wasm_inst_err_t ierr;
    wasm_instance_t* inst = s ? wasm_instantiate(s, m, 0, 0, &ierr) : 0;
    wasm_extern_t ex;
    int rc = 1;
    int nargs = argc - 4;
    if (!inst) printf("call %s: %s\n", argv[2], s ? ierr.text : "out of memory");
    else if (!wasm_instance_export(inst, (const uint8_t*)argv[3], (uint32_t)strlen(argv[3]), &ex) || ex.kind != WK_FUNC)
        printf("call %s: no exported function with this name\n", argv[2]);
    else {
        const wasm_functype_t* ft = wasm_func_type((wasm_funcinst_t*)ex.ptr);
        static wasm_val_t vals[WASM_MAX_PARAMS], res[WASM_MAX_RESULTS];
        if (ft->n_params != (uint32_t)nargs) printf("call %s: wrong number of arguments\n", argv[2]);
        else {
            for (int i = 0; i < nargs; i++) {
                uint8_t t = ft->types[i];
                if (t == WT_I32) vals[i] = (uint32_t)arg_int(argv[4 + i]);
                else if (t == WT_I64) vals[i] = arg_int(argv[4 + i]);
                else if (t == WT_F32) { union { float f; uint32_t u; } c; c.f = (float)strtod(argv[4 + i], 0); vals[i] = c.u; }
                else if (t == WT_F64) { union { double d; uint64_t u; } c; c.d = strtod(argv[4 + i], 0); vals[i] = c.u; }
                else vals[i] = 0;
            }
            // Часов у движка нет — на хосте вечный цикл останавливает «топливо».
            const char* fe = getenv("WASM_FUEL");
            wasm_store_limit(s, fe ? atoll(fe) : (1 << 18), 0);
            int trap = wasm_call(s, (wasm_funcinst_t*)ex.ptr, vals, res);
            printf("call %s(", argv[3]);
            for (int i = 0; i < nargs; i++) printf("%s%s", i ? ", " : "", argv[4 + i]);
            if (trap) printf(") trapped: %s\n", wasm_trap_name(trap));
            else {
                printf(") = %s", ft->n_results ? "" : "(nothing)");
                for (uint32_t i = 0; i < ft->n_results; i++) {
                    uint8_t t = ft->types[ft->n_params + i];
                    if (i) printf(", ");
                    if (t == WT_I32) printf("%d", (int32_t)(uint32_t)res[i]);
                    else if (t == WT_I64) printf("%lld", (long long)res[i]);
                    else if (t == WT_F32) printf("f32:0x%08x", (uint32_t)res[i]);
                    else if (t == WT_F64) printf("f64:0x%016llx", (unsigned long long)res[i]);
                    else printf("%s", res[i] ? "ref" : "null");
                }
                printf(" | fuel %llu\n", (unsigned long long)wasm_store_ticks(s));
                rc = 0;
            }
            if (trap) rc = 3;
        }
    }
    if (s) wasm_store_free(s);
    wasm_module_free(m);
    free(bin);
    return rc;
}

int main(int argc, char** argv) {
    // [51.5] WASM_JIT=0 — исполнять интерпретатором (по умолчанию — машинный код).
    const char* jit = getenv("WASM_JIT");
    if (jit && jit[0] == '0') wasm_jit_default(false);
    if (argc >= 3 && !strcmp(argv[1], "spec")) {
        size_t len;
        uint8_t* pack = read_file(argv[2], &len);
        if (!pack) return 2;
        wasm_spec_stats_t st;
        bool verbose = false;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "-v")) verbose = true;
            if (!strcmp(argv[i], "--stepped")) wasm_spec_set_stepped(true);
        }
        bool ok = wasm_spec_run(pack, len, sink, verbose, &st);
        free(pack);
        return (ok && !st.failures) ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "info")) {
        size_t len;
        uint8_t* bin = read_file(argv[2], &len);
        if (!bin) return 2;
        wasm_err_t err;
        wasm_module_t* m = wasm_module_load(bin, (uint32_t)len, &err);
        if (!m) {
            printf("rejected: %s - %s (offset %u)\n", wasm_err_name(err.kind), err.text, err.offset);
            free(bin);
            return 1;
        }
        wasm_describe(m, sink, "", argc >= 4 ? (uint32_t)atoi(argv[3]) : 64);
        wasm_module_free(m);
        free(bin);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "run")) return cmd_run(argc, argv);
    if (argc >= 4 && !strcmp(argv[1], "call")) return cmd_call(argc, argv);
    if (argc >= 4 && (!strcmp(argv[1], "fuzz") || !strcmp(argv[1], "fuzz-exec"))) {
        bool exec = !strcmp(argv[1], "fuzz-exec");
        long fx[FX_N] = { 0 }, by_trap[16] = { 0 };
        // Модуль приходит из недоверенного источника, а разбирает его ядро:
        // ни один испорченный файл не должен увести чтение за пределы буфера.
        // Портим верный модуль случайным образом и загружаем — тысячи раз.
        // Смысл проверки — в сборке с -fsanitize=address (цель wasm-host-asan):
        // там выход за границу буфера останавливает программу с отчётом.
        // Образцов может быть много: fuzz[-exec] <файл> <сколько на файл> <зерно> [ещё файлы…]
        long iters = atol(argv[3]);
        uint64_t x = argc >= 5 ? strtoull(argv[4], 0, 0) : 0x9E3779B97F4A7C15ull;
        long by_kind[8] = { 0 };
        long total = 0;
        int n_files = 0;
        size_t len = 0;
        for (int fi = 2; fi < argc; fi = fi == 2 ? 5 : fi + 1) {
            uint8_t* orig = read_file(argv[fi], &len);
            if (!orig) return 2;
            n_files++;
            for (long it = 0; it < iters; it++) {
#define RND() (x ^= x << 13, x ^= x >> 7, x ^= x << 17, x)
                size_t n = len;
                // Буфер точно по размеру (без запаса): чтение за концом — это
                // чтение чужой памяти, и ASAN его увидит.
                if (RND() % 4 == 0) n = (size_t)(RND() % (len + 1));          // обрезать хвост
                uint8_t* buf = malloc(n ? n : 1);
                memcpy(buf, orig, n);
                int muts = (int)(RND() % 4);
                for (int k = 0; k < muts && n; k++) {
                    size_t at = (size_t)(RND() % n);
                    switch (RND() % 4) {
                    case 0: buf[at] = (uint8_t)RND(); break;                  // случайный байт
                    case 1: buf[at] ^= (uint8_t)(1u << (RND() % 8)); break;   // один бит
                    case 2: buf[at] = 0xFF; break;                            // «длинное число»
                    default: buf[at] = (uint8_t)(buf[at] + 1); break;         // соседнее значение
                    }
                }
                wasm_err_t err = { 0, 0, { 0 } };
                wasm_module_t* m = wasm_module_load(buf, (uint32_t)n, &err);
                if (m && exec) fuzz_exec(m, fx, by_trap);
                if (m) wasm_module_free(m);
                by_kind[err.kind & 7]++;
                free(buf);
                total++;
            }
            free(orig);
        }
        if (n_files == 1) printf("fuzz: %ld mutants of %s (%zu bytes): ", total, argv[2], len);
        else printf("fuzz: %ld mutants of %d files: ", total, n_files);
        printf("accepted=%ld malformed=%ld invalid=%ld limit=%ld nomem=%ld unsupported=%ld\n",
               by_kind[WASM_OK], by_kind[WASM_E_MALFORMED], by_kind[WASM_E_INVALID],
               by_kind[WASM_E_LIMIT], by_kind[WASM_E_NOMEM], by_kind[WASM_E_UNSUPPORTED]);
        if (exec) {
            printf("fuzz-exec: started=%ld not-started=%ld calls=%ld traps=%ld (", fx[FX_STARTED], fx[FX_NOT_STARTED], fx[FX_CALLS], fx[FX_TRAPS]);
            for (int t = 1, first = 1; t <= WASM_TRAP_NOMEM; t++)
                if (by_trap[t]) { printf("%s%s=%ld", first ? "" : ", ", wasm_trap_name(t), by_trap[t]); first = 0; }
            printf(")\n");
        }
        return 0;
    }
    fprintf(stderr, "usage: wasm_host spec <pack> [-v] | info <file.wasm> [max items] | run [-d dir] [-r] <file.wasm> [args] | call <file.wasm> <func> [args] | fuzz[-exec] <file.wasm> <count> [seed]\n");
    return 2;
}

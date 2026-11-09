// ============================================================================
// mind.c — AMPLIFY MIND: модель, диалог, генерация, инструменты (этапы 49, 49.1).
// ----------------------------------------------------------------------------
// Схема одного вопроса:
//   BSP  mind_submit(text)
//          ├─ /команда -> инструмент -> реплика TOOL (точные данные ОС)
//          ├─ роутер: вопрос про процессы/файлы/журнал -> тот же инструмент,
//          │  а его вывод подкладывается модели как контекст
//          └─ промпт по шаблону модели -> токены -> g_job.input -> smp_pool_spawn
//   AP1  job_worker: prefill входных токенов (пакетами по MIND_BATCH), затем
//        сэмплинг; каждый токен — в SPSC-кольцо g_ring. Остальные AP считают
//        строки матриц.
//   BSP  mind_poll() каждый кадр: кольцо -> текст реплики MIND (потоково);
//        по окончании — разбор причины остановки (конец реплики, лимит,
//        вызов инструмента от модели, отмена).
//
// Два стиля диалога (mind_chat.h):
//   * CHATML (SmolLM2-135M, этап 49). Модель инструкции вызова инструментов
//     не выполняет — роутер детерминированно достаёт данные и кладёт их в
//     СИСТЕМНЫЙ промпт (так SmolLM2 отвечает по ним, а не вырождается), и
//     каждый вопрос по данным начинается с чистого контекста.
//   * QWEN3 (этап 49.1, Qwen3-1.7B). Родной формат модели: описания
//     инструментов в system (<tools>…</tools>), модель сама отвечает
//     <tool_call>{"name": …, "arguments": {…}}</tool_call>, результат
//     возвращается реплией user <tool_response>…</tool_response>. Роутер
//     остаётся запасным путём: для вопросов про процессы/файлы/журнал он сам
//     «делает вызов» от имени ассистента в том же формате, а для
//     процессов/памяти/ядер/окон ещё и собирает точный короткий ответ из
//     данных (mind_facts) — он становится началом ответа ассистента, и
//     модель лишь дополняет его. «Размышление» Qwen3 выключено пустым
//     блоком <think></think> (mind.conf think=1 — включить).
//     Системный промпт неизменен, поэтому его KV-кэш считается один раз
//     сразу после загрузки (g_sys_n): /new и переполнение контекста
//     откатываются к нему, а не к нулю.
//
// Состояние диалога: g_hist[0..g_npast) — токены, уже лежащие в KV-кэше.
// g_pending — последний сэмплированный токен, который в кэш ещё не прогнан
// (обычно <|im_end|> или </tool_call>): он идёт первым в следующем ходе.
// ============================================================================
#include "mind.h"
#include "mind_gguf.h"
#include "mind_model.h"
#include "mind_tok.h"
#include "mind_chat.h"
#include "mind_math.h"
#include "mind_util.h"
#include "smp_pool.h"
#include "timer.h"
#include "serial.h"
#include "heap.h"
#include "pmm.h"
#include "memory.h"
#include "fat32.h"
#include "initrd.h"
#include "iso9660.h"

// ----------------------------------------------------------------------------
// Конфигурация (/disk/Sanctum/mind.conf, строки key=value). Нули/минусы —
// «по модели»: у SmolLM2 и Qwen3 разные рекомендованные настройки.
// ----------------------------------------------------------------------------
typedef struct {
    char     model[160];
    int      ctx;            // 0 — Qwen3: 2048, прочие: 1024
    int      max_tokens;     // 0 — Qwen3: 300, прочие: 200
    int      top_k;          // 0 — по модели
    float    temp;           // < 0 — по модели
    float    top_p;
    float    repeat;
    bool     greedy;
    int      agent;          // -1 — по модели (Qwen3: инструменты в system)
    bool     think;          // Qwen3: режим размышления
    unsigned tools;          // какие инструменты описывать модели
    int      simd;           // MIND_SIMD_AUTO / SSE2 / SSSE3 (ключ simd=)
    int      bench_on_boot;  // >0 — при загрузке ОС замерить скорость (headless-тесты)
} mind_conf_t;

static mind_conf_t g_conf;

// Действующие параметры генерации (conf + умолчания стиля модели).
typedef struct {
    int      max_tokens, top_k;
    float    temp, top_p, repeat;
    bool     agent, think;
    unsigned tools;
} gen_params_t;

static gen_params_t g_gp;

static float parse_float(const char* s) {
    float v = 0.0f, scale = 1.0f;
    bool neg = false, frac = false;
    if (*s == '-') { neg = true; s++; }
    for (; *s; s++) {
        if (*s == '.') { frac = true; continue; }
        if (*s < '0' || *s > '9') break;
        if (frac) { scale *= 0.1f; v += (float)(*s - '0') * scale; }
        else v = v * 10.0f + (float)(*s - '0');
    }
    return neg ? -v : v;
}

static void conf_defaults(void) {
    m_memset(&g_conf, 0, sizeof(g_conf));
    g_conf.temp = -1.0f;
    g_conf.top_p = -1.0f;
    g_conf.repeat = -1.0f;
    g_conf.agent = -1;
    g_conf.tools = MIND_TOOLS_ALL;
}

static void conf_load(void) {
    static char buf[2048];
    conf_defaults();
    if (!fat32_is_mounted()) return;
    int64_t n = fat32_read_file(MIND_CONF_PATH, buf, sizeof(buf) - 1);
    if (n <= 0) return;
    buf[n] = 0;
    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '#' || !*line) continue;
        char* eq = line;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        char* ke = eq;
        while (ke > line && (ke[-1] == ' ' || ke[-1] == '\t')) ke--;
        *ke = 0;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        char* ve = v + m_strlen(v);
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\r' || ve[-1] == '\t')) *--ve = 0;
        if (m_streq(line, "model")) { g_conf.model[0] = 0; m_append(g_conf.model, sizeof(g_conf.model), v); }
        else if (m_streq(line, "ctx")) g_conf.ctx = (int)parse_float(v);
        else if (m_streq(line, "max_tokens")) g_conf.max_tokens = (int)parse_float(v);
        else if (m_streq(line, "top_k")) g_conf.top_k = (int)parse_float(v);
        else if (m_streq(line, "temp")) g_conf.temp = parse_float(v);
        else if (m_streq(line, "top_p")) g_conf.top_p = parse_float(v);
        else if (m_streq(line, "repeat")) g_conf.repeat = parse_float(v);
        else if (m_streq(line, "greedy")) g_conf.greedy = parse_float(v) != 0.0f;
        else if (m_streq(line, "agent")) g_conf.agent = parse_float(v) != 0.0f ? 1 : 0;
        else if (m_streq(line, "think")) g_conf.think = parse_float(v) != 0.0f;
        else if (m_streq(line, "tools")) g_conf.tools = mind_tool_mask_parse(v);
        else if (m_streq(line, "simd")) g_conf.simd = m_streq(v, "sse2") ? MIND_SIMD_SSE2 :
                                                      m_streq(v, "ssse3") ? MIND_SIMD_SSSE3 :
                                                      m_streq(v, "scalar") ? MIND_SIMD_SCALAR : MIND_SIMD_AUTO;
        else if (m_streq(line, "bench_on_boot")) g_conf.bench_on_boot = (int)parse_float(v);
    }
    if (g_conf.ctx && g_conf.ctx < 64) g_conf.ctx = 64;
    if (g_conf.ctx > 8192) g_conf.ctx = 8192;
    if (g_conf.max_tokens < 0) g_conf.max_tokens = 0;
    if (g_conf.max_tokens > 800) g_conf.max_tokens = 800;
    if (g_conf.top_k < 0) g_conf.top_k = 0;
    if (g_conf.top_k > 64) g_conf.top_k = 64;
    mind_math_set_simd(g_conf.simd);
}

// Умолчания по стилю модели. SmolLM2: авторы советуют temp 0.2..0.3, а
// маленькая модель легко зацикливается (repeat 1.15). Qwen3 в режиме без
// размышлений: temp 0.7, top_p 0.8, top_k 20 (рекомендация Qwen); жёсткий
// штраф за повторы портил бы пути и числа в ответах — 1.05.
static void apply_params(int style) {
    bool q = style == MIND_CHAT_QWEN3;
    g_gp.max_tokens = g_conf.max_tokens ? g_conf.max_tokens : (q ? 300 : 200);
    g_gp.top_k = g_conf.top_k ? g_conf.top_k : (q ? 20 : 40);
    g_gp.temp = g_conf.temp >= 0.0f ? g_conf.temp : (q ? 0.7f : 0.3f);
    g_gp.top_p = g_conf.top_p >= 0.0f ? g_conf.top_p : (q ? 0.8f : 0.9f);
    g_gp.repeat = g_conf.repeat >= 0.0f ? g_conf.repeat : (q ? 1.05f : 1.15f);
    g_gp.agent = g_conf.agent >= 0 ? g_conf.agent != 0 : q;
    g_gp.think = q && g_conf.think;
    g_gp.tools = g_conf.tools;
}

// ----------------------------------------------------------------------------
// Модель.
// ----------------------------------------------------------------------------
#define LOAD_CHUNK   (1ull * 1024 * 1024)
#define LOAD_SLICE_MS 40                     // сколько читать за кадр UI

static struct {
    mind_state_t st;
    char         err[128];
    char         path[160];
    char         label[72];
    uint8_t*     file;
    uint64_t     file_size;
    uint64_t     file_pages;       // 0 — файл лежит в initrd (не освобождаем)
    uint64_t     loaded;
    uint64_t     t_load0;
    bool         from_cd;          // веса читаются с установочного носителя
    iso_entry_t  cd_file;
    gguf_file_t  g;
    mind_model_t m;
    mind_tok_t   t;
    bool         have;             // g/m/t инициализированы
    bool         chat;
    int          style;            // MIND_CHAT_*
    bool         warming;          // считается KV-кэш системного промпта
} M;

static bool g_inited;

void mind_init(void) {
    if (g_inited) return;
    g_inited = true;
    mind_math_init();
    conf_defaults();
    M.st = MIND_ST_EMPTY;
    serial_puts("[mind] init: kernels=");
    serial_puts(mind_math_kernels());
    serial_puts(mind_math_tcg() ? " (qemu tcg)" : "");
    serial_puts(" pool=");
    serial_print_dec((uint64_t)smp_pool_workers());
    serial_puts("\n");
}

mind_state_t mind_state(void) { return M.st; }
const char* mind_error(void) { return M.err; }
bool mind_is_chat(void) { return M.have && M.chat; }
bool mind_warming(void) { return M.warming; }
bool mind_think_mode(void) { return M.have && M.style == MIND_CHAT_QWEN3 && g_gp.think; }
const char* mind_model_label(void) { return M.have ? M.label : (M.st == MIND_ST_LOADING ? "loading" : "not loaded"); }

int mind_load_progress(void) {
    if (M.st != MIND_ST_LOADING || !M.file_size) return M.have ? 100 : 0;
    return (int)(M.loaded * 100 / M.file_size);
}

static void set_label(const char* path) {
    const char* b = path;
    for (const char* s = path; *s; s++) if (*s == '/') b = s + 1;
    M.label[0] = 0;
    m_append(M.label, sizeof(M.label), b);
    uint64_t n = m_strlen(M.label);
    if (n > 5 && (M.label[n - 5] == '.') && ((M.label[n - 4] | 0x20) == 'g')) M.label[n - 5] = 0;
}

static void fail(const char* a, const char* b) {
    M.err[0] = 0;
    m_append(M.err, sizeof(M.err), a);
    if (b) m_append(M.err, sizeof(M.err), b);
    M.st = MIND_ST_ERROR;
    serial_puts("[mind] error: ");
    serial_puts(M.err);
    serial_puts("\n");
}

static void free_file(void) {
    if (M.file && M.file_pages) pmm_free_big((uint64_t)(uintptr_t)M.file, M.file_pages);
    M.file = 0;
    M.file_pages = 0;
    M.file_size = 0;
}

// ---- выбор модели ---------------------------------------------------------------
// Кандидаты: *.gguf в /disk/vault/mind и /mind на установочном носителе.
// Берём САМУЮ БОЛЬШУЮ модель, которая помещается в память: веса — одним
// непрерывным блоком (pmm_big_largest_free), плюс оценка KV-кэша и буферов
// (mind_model_estimate_bytes) — в свободную RAM. На машине с 1–2 ГБ это
// будет SmolLM2-135M, с 8 ГБ — Qwen3-1.7B. mind.conf model= — явный выбор.
#define MIND_MAX_CAND 8
typedef struct { char path[160]; uint64_t size; } cand_t;
static cand_t g_cand[MIND_MAX_CAND];
static int    g_ncand;

static bool is_gguf_name(const char* name) {
    uint64_t n = m_strlen(name);
    return n > 5 && name[n - 5] == '.' && (name[n - 4] | 0x20) == 'g' && (name[n - 3] | 0x20) == 'g' &&
           (name[n - 2] | 0x20) == 'u' && (name[n - 1] | 0x20) == 'f';
}

static void cand_add(const char* prefix, const char* name, uint64_t size) {
    if (g_ncand >= MIND_MAX_CAND || !is_gguf_name(name)) return;
    cand_t* c = &g_cand[g_ncand++];
    c->path[0] = 0;
    m_append(c->path, sizeof(c->path), prefix);
    m_append(c->path, sizeof(c->path), name);
    c->size = size;
}

static int find_cb(const fat32_dir_entry_t* e, void* u) {
    (void)u;
    if (!e->is_dir) cand_add(MIND_MODEL_DIR "/", e->name, e->size);
    return 0;
}

static int cd_find_cb(const char* name, const iso_entry_t* e, void* u) {
    (void)u;
    if (!e->is_dir) cand_add(MIND_CD_PREFIX "mind/", name, e->size);
    return 0;
}

// Путь лучшей модели в out (или пусто).
static void pick_model(char* out, int cap) {
    out[0] = 0;
    uint64_t block = pmm_big_largest_free(), freeb = pmm_free_ram();
    int best = -1;
    for (int i = 0; i < g_ncand; i++) {
        bool fits = g_cand[i].size + 16ull * 1024 * 1024 <= block &&
                    mind_model_estimate_bytes(g_cand[i].size) <= freeb;
        serial_puts("[mind] candidate ");
        serial_puts(g_cand[i].path);
        serial_puts(" ");
        serial_print_dec(g_cand[i].size / (1024 * 1024));
        serial_puts(" MB: ");
        serial_puts(fits ? "fits\n" : "does not fit in RAM\n");
        if (fits && (best < 0 || g_cand[i].size > g_cand[best].size)) best = i;
    }
    if (best >= 0) m_append(out, (uint64_t)cap, g_cand[best].path);
    else if (g_ncand) {
        serial_puts("[mind] no model fits: free RAM ");
        serial_print_dec(freeb / (1024 * 1024));
        serial_puts(" MB, largest block ");
        serial_print_dec(block / (1024 * 1024));
        serial_puts(" MB\n");
    }
}

static int32_t* g_hist;
static int      g_hist_cap;
static int      g_npast;
static int      g_pending = -1;
static int      g_sys_n;           // Qwen3: длина системного промпта в KV-кэше

static void start_warmup(void);

static void finalize_load(void) {
    char err[128];
    if (gguf_parse(&M.g, M.file, M.file_size, err, sizeof(err)) != 0) { free_file(); fail("GGUF: ", err); return; }
    if (mind_tok_init(&M.t, &M.g, err, sizeof(err)) != 0) { gguf_free(&M.g); free_file(); fail("tokenizer: ", err); return; }
    mind_str_t arch;
    bool qwen = gguf_get_str(&M.g, "general.architecture", &arch) && m_lstreq(arch.p, arch.len, "qwen3");
    M.chat = M.t.im_start >= 0 && M.t.im_end >= 0;
    M.style = !M.chat ? MIND_CHAT_NONE : (qwen ? MIND_CHAT_QWEN3 : MIND_CHAT_CHATML);
    apply_params(M.style);
    int ctx = g_conf.ctx ? g_conf.ctx : (M.style == MIND_CHAT_QWEN3 ? 2048 : 1024);
    if (mind_model_init(&M.m, &M.g, ctx, err, sizeof(err)) != 0) {
        mind_tok_free(&M.t); gguf_free(&M.g); free_file(); fail("model: ", err); return;
    }
    g_hist_cap = M.m.n_ctx;
    g_hist = (int32_t*)kmalloc((uint64_t)g_hist_cap * 4);
    if (!g_hist) {
        mind_model_free(&M.m); mind_tok_free(&M.t); gguf_free(&M.g); free_file();
        fail("out of memory (history)", 0);
        return;
    }
    g_npast = 0;
    g_pending = -1;
    g_sys_n = 0;
    M.have = true;
    M.st = MIND_ST_READY;
    uint64_t ms = timer_get_uptime_ms() - M.t_load0;
    serial_puts("[mind] loaded ");
    serial_puts(M.label);
    serial_puts(": layers=");
    serial_print_dec((uint64_t)M.m.n_layer);
    serial_puts(" embd=");
    serial_print_dec((uint64_t)M.m.n_embd);
    serial_puts(" vocab=");
    serial_print_dec((uint64_t)M.m.n_vocab);
    serial_puts(" ctx=");
    serial_print_dec((uint64_t)M.m.n_ctx);
    serial_puts(" type=");
    serial_puts(gguf_type_name(M.m.main_type));
    serial_puts(" chat=");
    serial_puts(M.chat ? "yes" : "no");
    serial_puts(" size=");
    serial_print_dec(M.file_size / 1024);
    serial_puts("K kv=");
    serial_print_dec(mind_model_arena_bytes(&M.m) / 1024);
    serial_puts("K in ");
    serial_print_dec(ms);
    serial_puts(" ms arch=");
    serial_puts(M.m.arch == MIND_ARCH_QWEN3 ? "qwen3" : "llama");
    serial_puts(" weights@0x");
    serial_print_hex((uint64_t)(uintptr_t)M.file, 16);
    serial_puts("\n");
}

static void msg_info(const char* s);

int mind_load_begin(const char* path) {
    mind_init();
    if (M.st == MIND_ST_BUSY || M.st == MIND_ST_LOADING || M.st == MIND_ST_CONFIRM) return -1;
    mind_unload();
    conf_load();
    M.err[0] = 0;
    M.t_load0 = timer_get_uptime_ms();

    char p[160];
    p[0] = 0;
    if (path && *path) m_append(p, sizeof(p), path);
    else if (g_conf.model[0]) m_append(p, sizeof(p), g_conf.model);
    else {
        g_ncand = 0;
        if (fat32_is_mounted()) {
            // Папка для весов — чтобы было видно, куда класть свои .gguf.
            (void)fat32_mkdir_p(MIND_MODEL_DIR);
            (void)fat32_iter_dir(MIND_MODEL_DIR, find_cb, 0);
        }
        // Live-сессия с установочного носителя: модели лежат на нём в /mind.
        if (!g_ncand && iso_mount_boot()) (void)iso_list("/mind", cd_find_cb, 0);
        pick_model(p, sizeof(p));
    }
    if (!p[0]) m_append(p, sizeof(p), MIND_BUILTIN_MODEL);
    M.path[0] = 0;
    m_append(M.path, sizeof(M.path), p);
    set_label(p);
    serial_puts("[mind] load ");
    serial_puts(p);
    serial_puts("\n");

    if (p[0] != '/' || m_starts(p, "/initrd/")) {
        // Модель из initrd: уже в памяти, копировать не нужно.
        const char* nm = p[0] == '/' ? p + 8 : p;
        uint64_t sz = 0;
        const uint8_t* d = initrd_get_file(nm, &sz);
        if (!d) { fail("model not found: ", p); return -1; }
        M.file = (uint8_t*)d;
        M.file_size = sz;
        M.file_pages = 0;
        finalize_load();
        return M.st == MIND_ST_READY ? 0 : -1;
    }
    M.from_cd = false;
    uint64_t sz = 0;
    if (m_starts(p, MIND_CD_PREFIX)) {
        if (!iso_mount_boot() || !iso_lookup(p + sizeof(MIND_CD_PREFIX) - 2, &M.cd_file) || M.cd_file.is_dir) {
            fail("model not found on the installation medium: ", p);
            return -1;
        }
        sz = M.cd_file.size;
        M.from_cd = true;
    } else {
        if (!fat32_is_mounted()) { fail("disk is not mounted", 0); return -1; }
        uint8_t probe;
        if (fat32_read_file_at(p, 0, &probe, 0, &sz) < 0 || sz < 64) { fail("model not found: ", p); return -1; }
    }
    // Веса — одним блоком (с 49.1 — из памяти выше 4 ГБ, если она есть).
    uint64_t pages = (sz + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_big(pages);
    if (!phys) { fail("not enough RAM for model weights", 0); return -1; }
    M.file = (uint8_t*)(uintptr_t)phys;
    M.file_pages = pages;
    M.file_size = sz;
    M.loaded = 0;
    M.st = MIND_ST_LOADING;
    return 0;
}

// Кусок загрузки (читаем, пока не истечёт LOAD_SLICE_MS); true — ещё не всё.
static bool load_step(void) {
    if (M.st != MIND_ST_LOADING) return false;
    uint64_t t0 = timer_get_uptime_ms();
    do {
        uint64_t n = M.file_size - M.loaded;
        if (n > LOAD_CHUNK) n = LOAD_CHUNK;
        int64_t got = M.from_cd ? iso_read(&M.cd_file, M.loaded, M.file + M.loaded, n)
                                : fat32_read_file_at(M.path, M.loaded, M.file + M.loaded, n, 0);
        if (got != (int64_t)n) { free_file(); fail("disk read error: ", M.path); return false; }
        M.loaded += n;
    } while (M.loaded < M.file_size && timer_get_uptime_ms() - t0 < LOAD_SLICE_MS);
    if (M.loaded < M.file_size) return true;
    finalize_load();
    return false;
}

int mind_load_sync(const char* path) {
    if (mind_load_begin(path) != 0) return -1;
    while (load_step()) { }
    return M.st == MIND_ST_READY ? 0 : -1;
}

void mind_unload(void) {
    if (M.st == MIND_ST_BUSY || M.st == MIND_ST_CONFIRM) return;
    if (M.have) {
        mind_model_free(&M.m);
        mind_tok_free(&M.t);
        gguf_free(&M.g);
        M.have = false;
    }
    if (g_hist) { kfree(g_hist); g_hist = 0; }
    free_file();
    g_npast = 0;
    g_pending = -1;
    g_sys_n = 0;
    M.warming = false;
    M.st = MIND_ST_EMPTY;
}

// ----------------------------------------------------------------------------
// Журнал реплик (хранится в CP1251 — кодировке экрана и shell).
// ----------------------------------------------------------------------------
#define MIND_MAX_MSGS 40

typedef struct {
    mind_role_t role;
    char*       text;
    int         len;
    int         cap;
} msg_t;

static msg_t    g_msgs[MIND_MAX_MSGS];
static int      g_nmsg;
static uint32_t g_ver;
static uint32_t g_msg_total;       // сколько реплик создано за всё время (монотонно)

static msg_t* msg_new(mind_role_t role) {
    if (g_nmsg == MIND_MAX_MSGS) {
        if (g_msgs[0].text) kfree(g_msgs[0].text);
        for (int i = 1; i < MIND_MAX_MSGS; i++) g_msgs[i - 1] = g_msgs[i];
        g_nmsg--;
    }
    msg_t* m = &g_msgs[g_nmsg++];
    g_msg_total++;
    m->role = role;
    m->cap = 256;
    m->len = 0;
    m->text = (char*)kmalloc((uint64_t)m->cap);
    if (m->text) m->text[0] = 0;
    else m->cap = 0;
    g_ver++;
    return m;
}

static void msg_add(msg_t* m, const char* s, int n) {
    if (!m || n <= 0) return;
    if (m->len + n + 1 > m->cap) {
        int nc = m->cap ? m->cap : 256;
        while (m->len + n + 1 > nc) nc *= 2;
        if (nc > 65536) { n = 65535 - m->len; if (n <= 0) return; nc = 65536; }
        char* t = (char*)kmalloc((uint64_t)nc);
        if (!t) return;
        if (m->text) { m_memcpy(t, m->text, (uint64_t)m->len); kfree(m->text); }
        m->text = t;
        m->cap = nc;
    }
    m_memcpy(m->text + m->len, s, (uint64_t)n);
    m->len += n;
    m->text[m->len] = 0;
    g_ver++;
}

static void msg_add_s(msg_t* m, const char* s) { msg_add(m, s, (int)m_strlen(s)); }

static void msg_info(const char* s) { msg_add_s(msg_new(MIND_ROLE_INFO), s); }

// Убрать последнюю реплику (пустую реплику MIND после вызова инструмента).
static void msg_pop(msg_t* m) {
    if (g_nmsg == 0 || m != &g_msgs[g_nmsg - 1]) return;
    if (m->text) kfree(m->text);
    m->text = 0;
    g_nmsg--;
    g_msg_total--;
    g_ver++;
}

// UTF-8-текст -> реплика (перекодировка в CP1251).
static void msg_add_utf8(msg_t* m, const char* s, int n) {
    static char tmp[8192];
    while (n > 0) {
        int chunk = n > 2048 ? 2048 : n;
        while (chunk < n && chunk > 1 && (((uint8_t)s[chunk]) & 0xC0) == 0x80) chunk--;
        int k = mind_to_cp1251(s, chunk, tmp, sizeof(tmp));
        msg_add(m, tmp, k);
        s += chunk;
        n -= chunk;
    }
}

int mind_msg_count(void) { return g_nmsg; }
mind_role_t mind_msg_role(int i) { return (i >= 0 && i < g_nmsg) ? g_msgs[i].role : MIND_ROLE_INFO; }
const char* mind_msg_text(int i) { return (i >= 0 && i < g_nmsg && g_msgs[i].text) ? g_msgs[i].text : ""; }
uint32_t mind_version(void) { return g_ver; }
uint32_t mind_msg_total(void) { return g_msg_total; }

// ----------------------------------------------------------------------------
// Задание генерации (исполняется на AP-дирижёре или шагами на BSP).
// ----------------------------------------------------------------------------
#define MIND_MAX_INPUT 4096
#define RING_SIZE      1024

enum { STOP_NONE = 0, STOP_EOG, STOP_MAX, STOP_CTX, STOP_TOOL, STOP_CANCEL };
enum { PH_PREFILL = 0, PH_GEN };

typedef struct {
    int32_t      input[MIND_MAX_INPUT];
    int          n_input;
    int          max_new;
    int          batch;            // токенов промпта за один проход матриц
    bool         greedy;
    bool         stop_tool_id;     // остановка на </tool_call> (Qwen3 с инструментами)
    float        temp, top_p, repeat;
    int          top_k;
    volatile int cancel;
    volatile int running;
    int          stop;
    int          last;
    bool         last_pending;     // last сэмплирован, но в KV-кэш не прогнан
    int          n_gen;
    uint64_t     t0, t1, t2;
    int          phase, i;
    float*       logits;
    char         tail[48];
    int          tail_n;
} job_t;

static job_t             g_job;
static volatile int32_t  g_ring[RING_SIZE];
static volatile uint32_t g_ring_w, g_ring_r;
static bool              g_job_active;     // BSP: задание запущено и ещё не разобрано
static bool              g_job_on_ap;
static uint64_t          g_rng = 0x9E3779B97F4A7C15ull;
static int               g_last_gen, g_last_tps10, g_last_pps10;
static bool              g_force_greedy;

static uint32_t rng_u32(void) {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return (uint32_t)((g_rng * 0x2545F4914F6CDD1Dull) >> 32);
}

// Сэмплер: штраф за повторы (как repeat_penalty в llama.cpp) -> top-k ->
// температура -> top-p -> случайный выбор. greedy — просто argmax.
static int sample(job_t* j, float* lg, int n) {
    if (j->greedy) {
        int b = 0;
        for (int i = 1; i < n; i++) if (lg[i] > lg[b]) b = i;
        return b;
    }
    if (j->repeat > 1.0f) {
        int from = g_npast > 64 ? g_npast - 64 : 0;
        for (int k = from; k < g_npast; k++) {
            int t = g_hist[k];
            bool dup = false;
            for (int q = from; q < k; q++) if (g_hist[q] == t) { dup = true; break; }
            if (dup || t < 0 || t >= n) continue;
            lg[t] = lg[t] > 0.0f ? lg[t] / j->repeat : lg[t] * j->repeat;
        }
    }
    int K = j->top_k;
    int idx[64];
    float val[64];
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        float v = lg[i];
        if (cnt == K && v <= val[K - 1]) continue;
        int p = cnt < K ? cnt++ : K - 1;
        while (p > 0 && val[p - 1] < v) { val[p] = val[p - 1]; idx[p] = idx[p - 1]; p--; }
        val[p] = v;
        idx[p] = i;
    }
    float t = j->temp > 0.01f ? j->temp : 0.01f;
    double sum = 0.0;
    float pr[64];
    for (int i = 0; i < cnt; i++) { pr[i] = m_expf((val[i] - val[0]) / t); sum += pr[i]; }
    // top-p: оставляем минимальный префикс с массой >= top_p.
    double acc = 0.0;
    int keep = cnt;
    for (int i = 0; i < cnt; i++) {
        acc += pr[i] / sum;
        if (acc >= (double)j->top_p) { keep = i + 1; break; }
    }
    double mass = 0.0;
    for (int i = 0; i < keep; i++) mass += pr[i];
    double r = (double)(rng_u32() >> 8) / 16777216.0 * mass;
    for (int i = 0; i < keep; i++) {
        r -= pr[i];
        if (r <= 0.0) return idx[i];
    }
    return idx[keep - 1];
}

static bool tail_has_tool_close(job_t* j) {
    static const char tag[] = "</tool>";
    const int tl = 7;
    if (j->tail_n < tl) return false;
    for (int k = 0; k < tl; k++) if (j->tail[j->tail_n - tl + k] != tag[k]) return false;
    return true;
}

static void tail_push(job_t* j, const char* s, int n) {
    for (int k = 0; k < n; k++) {
        if (j->tail_n == (int)sizeof(j->tail)) {
            for (int q = 1; q < j->tail_n; q++) j->tail[q - 1] = j->tail[q];
            j->tail_n--;
        }
        j->tail[j->tail_n++] = s[k];
    }
}

// Один шаг (пакет prefill или один сгенерированный токен). false — задание
// закончено (причина в j->stop).
static bool job_step(job_t* j) {
    if (j->cancel) { j->stop = STOP_CANCEL; return false; }
    if (j->phase == PH_PREFILL) {
        if (j->i >= j->n_input) { j->phase = PH_GEN; j->t1 = timer_get_uptime_ms(); return true; }
        int k = j->n_input - j->i;
        if (k > j->batch) k = j->batch;
        if (g_npast + k > M.m.n_ctx) { j->stop = STOP_CTX; return false; }
        bool last = j->i + k >= j->n_input;
        j->logits = mind_model_forward_batch(&M.m, j->input + j->i, k, g_npast, last);
        for (int q = 0; q < k; q++) g_hist[g_npast + q] = j->input[j->i + q];
        g_npast += k;
        j->i += k;
        if (last) { j->phase = PH_GEN; j->t1 = timer_get_uptime_ms(); }
        return true;
    }
    if (!j->logits) { j->stop = STOP_EOG; return false; }
    if (j->n_gen >= j->max_new) { j->stop = STOP_MAX; return false; }
    int tok = sample(j, j->logits, M.m.n_vocab);
    while (g_ring_w - __atomic_load_n(&g_ring_r, __ATOMIC_ACQUIRE) >= RING_SIZE) {
        if (j->cancel) { j->stop = STOP_CANCEL; return false; }
        __asm__ __volatile__("pause");
    }
    g_ring[g_ring_w % RING_SIZE] = tok;
    __atomic_store_n(&g_ring_w, g_ring_w + 1, __ATOMIC_RELEASE);
    j->last = tok;
    j->last_pending = true;
    j->n_gen++;
    if (mind_tok_is_eog(&M.t, tok)) { j->stop = STOP_EOG; return false; }
    if (j->stop_tool_id && tok == M.t.tool_call_close) { j->stop = STOP_TOOL; return false; }
    char pc[64];
    int pn = mind_tok_piece(&M.t, tok, pc, sizeof(pc));
    tail_push(j, pc, pn);
    if (tail_has_tool_close(j)) { j->stop = STOP_TOOL; return false; }
    if (g_npast >= M.m.n_ctx) { j->stop = STOP_CTX; return false; }
    j->logits = mind_model_forward(&M.m, tok, g_npast);
    g_hist[g_npast++] = tok;
    j->last_pending = false;
    return true;
}

static void job_worker(void* arg) {
    job_t* j = (job_t*)arg;
    smp_pool_hot_begin();
    while (job_step(j)) { }
    smp_pool_hot_end();
    j->t2 = timer_get_uptime_ms();
    __atomic_store_n(&j->running, 0, __ATOMIC_RELEASE);
}

static void job_launch(int max_new, bool allow_ap) {
    job_t* j = &g_job;
    j->max_new = max_new;
    if (j->max_new > RING_SIZE - 16) j->max_new = RING_SIZE - 16;
    j->greedy = g_force_greedy || g_conf.greedy || g_gp.temp <= 0.0f;
    j->temp = g_gp.temp;
    j->top_p = g_gp.top_p;
    j->top_k = g_gp.top_k;
    j->repeat = g_gp.repeat;
    j->stop_tool_id = M.style == MIND_CHAT_QWEN3 && g_gp.agent && M.t.tool_call_close >= 0;
    j->cancel = 0;
    j->stop = STOP_NONE;
    j->last = -1;
    j->last_pending = false;
    j->n_gen = 0;
    j->phase = PH_PREFILL;
    j->i = 0;
    j->logits = 0;
    j->tail_n = 0;
    j->t0 = timer_get_uptime_ms();
    j->t1 = j->t2 = j->t0;
    g_ring_w = g_ring_r = 0;
    g_rng ^= timer_get_uptime_ms() * 0x9E3779B97F4A7C15ull + 1;
    j->running = 1;
    g_job_active = true;
    g_job_on_ap = allow_ap && smp_pool_workers() > 0 && smp_pool_spawn(job_worker, j);
    // Пакетный prefill — на дирижёре: на однопроцессорной машине задание
    // идёт шагами в главном цикле UI, и пакет из 32 токенов 1.7B-модели
    // заморозил бы экран на секунды — там по одному токену.
    j->batch = g_job_on_ap ? MIND_BATCH : 1;
    M.st = MIND_ST_BUSY;
}

// ----------------------------------------------------------------------------
// Построение промпта.
// ----------------------------------------------------------------------------
static const char k_system[] =
    "You are MIND, the assistant built into AMPLIFY-OS, a small operating system "
    "written from scratch. Answer briefly and clearly. When tool output is given, "
    "answer using only that output.";
static const char k_system_agent[] =
    " To inspect the system you may reply with one tool call: <tool>ps</tool>, "
    "<tool>ls PATH</tool>, <tool>read PATH</tool>, <tool>logs</tool> or "
    "<tool>exec COMMAND</tool>. The result comes back in the next message.";
// Qwen3: системный промпт один на всю сессию (его KV-кэш считается один раз).
static const char k_system_qwen[] =
    "You are MIND, the assistant built into AMPLIFY-OS, an operating system written from "
    "scratch. You run locally on this computer, nothing leaves it. Answer briefly and "
    "clearly, in the language of the question. Facts about this computer (processes, "
    "memory, files, logs) come only from tool results - never invent them.";

static char g_ptext[16384];        // накопитель текста сборщика промпта

// Данные инструмента для следующего хода (UTF-8): кладутся в СИСТЕМНЫЙ промпт
// (стиль CHATML). Подобрано замером на хосте (scripts/probe_stage49_prompts.sh):
// SmolLM2-135M, получив «Tool output ... Question: ...» в реплике
// пользователя, вырождается в повторы, а с данными в system-части отвечает по ним.
static char g_sys_data[2048];

static bool build_chatml(const char* user_utf8, bool fresh) {
    static char sys[4096];
    mind_prompt_t p;
    mind_prompt_begin(&p, &M.t, g_job.input, MIND_MAX_INPUT, g_ptext, sizeof(g_ptext));
    if (fresh) {
        sys[0] = 0;
        m_append(sys, sizeof(sys), k_system);
        if (g_gp.agent) m_append(sys, sizeof(sys), k_system_agent);
        if (g_sys_data[0]) { m_append(sys, sizeof(sys), "\n\n"); m_append(sys, sizeof(sys), g_sys_data); }
        mind_chat_system(&p, MIND_CHAT_CHATML, sys, 0);
    } else {
        mind_chat_close(&p, g_pending);
    }
    mind_chat_user(&p, user_utf8);
    mind_chat_assistant(&p, MIND_CHAT_CHATML, false, 0);
    int n = mind_prompt_end(&p);
    g_job.n_input = n > 0 ? n : 0;
    return n > 0;
}

// ---- Qwen3 ----------------------------------------------------------------------
enum { QT_USER = 0, QT_ROUTED, QT_RESULT };

typedef struct {
    int         kind;
    const char* text;        // вопрос пользователя (UTF-8)
    int         tool;        // QT_ROUTED: какой инструмент «вызвал» роутер
    const char* arg;
    const char* data;        // вывод инструмента (UTF-8)
    int         data_n;
    const char* prefix;      // начало ответа ассистента (точные факты) или 0
} qturn_t;

static void qwen_body(mind_prompt_t* p, const qturn_t* q) {
    if (q->kind == QT_USER || q->kind == QT_ROUTED) mind_chat_user(p, q->text);
    if (q->kind == QT_ROUTED) {
        // Вызов «от имени ассистента» — как шаблон Qwen3 рендерит прошлую
        // реплику с tool_calls: без блока <think> и без текста перед вызовом.
        mind_prompt_tok(p, M.t.im_start);
        mind_prompt_text(p, "assistant\n");
        mind_chat_tool_call_text(p, q->tool, q->arg);
        mind_prompt_tok(p, M.t.im_end);
        mind_prompt_text(p, "\n");
    }
    if (q->kind == QT_ROUTED || q->kind == QT_RESULT) mind_chat_tool_response(p, q->data, q->data_n);
    mind_chat_assistant(p, MIND_CHAT_QWEN3, g_gp.think, q->prefix);
}

// Промпт хода Qwen3 в g_job.input. *max_new может уменьшиться под остаток
// контекста. Не влезает — откат к системному промпту (кроме продолжения
// после вызова инструмента: его нельзя оторвать от вызова). -1 — никак.
static int qwen_build(const qturn_t* q, int* max_new) {
    for (int attempt = 0; attempt < 2; attempt++) {
        mind_prompt_t p;
        mind_prompt_begin(&p, &M.t, g_job.input, MIND_MAX_INPUT, g_ptext, sizeof(g_ptext));
        if (g_npast == 0) mind_chat_system(&p, MIND_CHAT_QWEN3, k_system_qwen, g_gp.agent ? g_gp.tools : 0);
        else if (g_npast > g_sys_n || g_pending >= 0) mind_chat_close(&p, g_pending);
        qwen_body(&p, q);
        int n = mind_prompt_end(&p);
        if (n > 0 && g_npast + n + 16 <= M.m.n_ctx) {
            if (g_npast + n + *max_new > M.m.n_ctx) *max_new = M.m.n_ctx - g_npast - n;
            g_job.n_input = n;
            return n;
        }
        if (attempt == 0 && q->kind != QT_RESULT && g_npast > g_sys_n) {
            msg_info("context is full - started a new conversation");
            g_npast = g_sys_n;
            g_pending = -1;
            continue;
        }
        break;
    }
    g_job.n_input = 0;
    return -1;
}

static msg_t* g_cur;               // реплика MIND, в которую льётся вывод
static char   g_u8[8];             // незавершённая UTF-8-последовательность
static int    g_u8n;
static int    g_tool_rounds;

// Системный промпт Qwen3 — один раз после загрузки, в фоне на дирижёре.
static void start_warmup(void) {
    if (M.style != MIND_CHAT_QWEN3 || g_npast != 0) return;
    mind_prompt_t p;
    mind_prompt_begin(&p, &M.t, g_job.input, MIND_MAX_INPUT, g_ptext, sizeof(g_ptext));
    mind_chat_system(&p, MIND_CHAT_QWEN3, k_system_qwen, g_gp.agent ? g_gp.tools : 0);
    int n = mind_prompt_end(&p);
    if (n <= 0 || n + 64 > M.m.n_ctx) return;
    g_job.n_input = n;
    g_cur = 0;
    M.warming = true;
    job_launch(0, true);
    serial_puts("[mind] caching system prompt: ");
    serial_print_dec((uint64_t)n);
    serial_puts(" tokens\n");
}

static int start_turn(const char* user_utf8) {
    if (!M.have) return -1;
    int max_new = g_gp.max_tokens;
    if (M.style == MIND_CHAT_NONE) {
        // Модель без чата (stories): каждый запрос — продолжение текста с нуля.
        g_npast = 0;
        g_pending = -1;
        g_job.n_input = 0;
        int r = mind_tok_encode(&M.t, user_utf8, (int)m_strlen(user_utf8), M.t.add_bos,
                                g_job.input, MIND_MAX_INPUT);
        if (r <= 0) { msg_info("prompt is too long"); return -1; }
        g_job.n_input = r;
    } else if (M.style == MIND_CHAT_QWEN3) {
        qturn_t q = { QT_USER, user_utf8, 0, 0, 0, 0, 0 };
        if (qwen_build(&q, &max_new) < 0) { msg_info("prompt is too long for the context"); return -1; }
    } else {
        bool fresh = g_npast == 0;
        bool ok = build_chatml(user_utf8, fresh);
        if (!ok || g_npast + g_job.n_input + max_new > M.m.n_ctx) {
            // Контекст переполнен — начинаем диалог заново (с системным промптом).
            if (!fresh) msg_info("context is full - started a new conversation");
            g_npast = 0;
            g_pending = -1;
            ok = build_chatml(user_utf8, true);
            if (!ok || g_job.n_input + 8 > M.m.n_ctx) { msg_info("prompt is too long for the context"); return -1; }
            if (g_job.n_input + max_new > M.m.n_ctx) max_new = M.m.n_ctx - g_job.n_input;
        }
    }
    g_cur = msg_new(MIND_ROLE_MIND);
    g_u8n = 0;
    job_launch(max_new, true);
    return 0;
}

// ----------------------------------------------------------------------------
// Инструменты и роутер.
// ----------------------------------------------------------------------------
#define TOOL_OUT  4096
#define DATA_LIM  1500              // сколько вывода инструмента отдаём модели: prefill не бесплатен
static char g_tool_out[TOOL_OUT];
static char g_prompt[6144];
static char g_exec_pending[256];
static char g_deferred[512];       // вопрос, заданный до окончания загрузки
static char g_facts[1024];
static char g_last_q[1024];        // последний вопрос (UTF-8): повтор, если результат не влез

static int data_limit(int n) {
    int lim = n > DATA_LIM ? DATA_LIM : (n > 0 ? n : 0);
    while (lim < n && lim > 0 && (((uint8_t)g_tool_out[lim]) & 0xC0) == 0x80) lim--;
    return lim;
}

static void tool_card(const char* name, const char* args, int n) {
    msg_t* tm = msg_new(MIND_ROLE_TOOL);
    msg_add_s(tm, name);
    if (args && *args) { msg_add_s(tm, " "); msg_add_s(tm, args); }
    msg_add_s(tm, "\n");
    if (n > 0) msg_add_utf8(tm, g_tool_out, n);
}

// Qwen3: вопрос + «вызов» инструмента роутером + его результат (+ точные
// факты как начало ответа) -> модель.
static void ask_qwen_with_data(int tool, const char* args, const char* question, int n) {
    int fn = 0;
    if (tool == MIND_TOOL_PS && !g_gp.think) fn = mind_facts(question, g_facts, sizeof(g_facts));
    int max_new = g_gp.max_tokens;
    int lim = data_limit(n);
    static char q8[1024];
    q8[0] = 0;
    m_append(q8, sizeof(q8), question);
    qturn_t q = { QT_ROUTED, q8, tool, args, g_tool_out, lim, fn > 0 ? g_facts : 0 };
    if (qwen_build(&q, &max_new) < 0) {
        // Не влезло даже с чистого диалога — меньше данных.
        q.data_n = data_limit(lim / 2);
        if (qwen_build(&q, &max_new) < 0) { msg_info("prompt is too long for the context"); return; }
    }
    g_cur = msg_new(MIND_ROLE_MIND);
    g_u8n = 0;
    if (fn > 0) {
        msg_add_utf8(g_cur, g_facts, fn);
        serial_puts("[mind] facts: ");
        for (int i = 0; i < fn && i < 300; i++) serial_putc(g_facts[i] == '\n' ? '|' : g_facts[i]);
        serial_puts("\n");
    }
    job_launch(max_new, true);
}

// Выполнить инструмент, показать результат, и (если есть вопрос и модель)
// спросить модель с этим контекстом.
static void run_tool_and_ask(const char* name, const char* args, const char* question) {
    int n = mind_tool_run(name, args, g_tool_out, TOOL_OUT);
    if (n < 0) { msg_info("unknown tool"); return; }
    tool_card(name, args, n);
    // Модель без чата (сказочная stories) по данным отвечать не умеет —
    // хватит самих данных.
    if (!question || !*question || !M.have || !M.chat) return;
    if (M.style == MIND_CHAT_QWEN3) {
        ask_qwen_with_data(mind_tool_index(name), args ? args : "", question, n);
        return;
    }
    int lim = data_limit(n);
    g_sys_data[0] = 0;
    if (m_streq(name, "ps")) m_append(g_sys_data, sizeof(g_sys_data), "Current state of this computer:\n");
    else if (m_streq(name, "logs")) m_append(g_sys_data, sizeof(g_sys_data), "Recent kernel log of this computer:\n");
    else if (m_streq(name, "ls")) {
        m_append(g_sys_data, sizeof(g_sys_data), "Files in ");
        m_append(g_sys_data, sizeof(g_sys_data), args && *args ? args : "/disk");
        m_append(g_sys_data, sizeof(g_sys_data), " on this computer:\n");
    } else {
        m_append(g_sys_data, sizeof(g_sys_data), "Contents of the file ");
        m_append(g_sys_data, sizeof(g_sys_data), args ? args : "");
        m_append(g_sys_data, sizeof(g_sys_data), ":\n");
    }
    uint64_t base = m_strlen(g_sys_data);
    if (base + (uint64_t)lim + 1 < sizeof(g_sys_data)) {
        m_memcpy(g_sys_data + base, g_tool_out, (uint64_t)lim);
        g_sys_data[base + (uint64_t)lim] = 0;
    }
    // Вопрос по данным — всегда с чистого контекста: системный промпт
    // существует только в начале диалога, а маленькая модель с длинной
    // историей продолжает прошлую тему вместо ответа по данным (тест 49).
    g_npast = 0;
    g_pending = -1;
    g_prompt[0] = 0;
    m_append(g_prompt, sizeof(g_prompt), question);
    (void)start_turn(g_prompt);
    g_sys_data[0] = 0;          // одноразово: следующий вопрос — без данных
}

// Слово w (без учёта регистра) с начала слова: "cores" найдёт "core",
// а "score" — нет.
static bool has_word(const char* s, const char* w) {
    uint64_t wl = m_strlen(w);
    const char* start = s;
    for (; *s; s++) {
        if (s > start && (((s[-1] | 0x20) >= 'a' && (s[-1] | 0x20) <= 'z') || (s[-1] >= '0' && s[-1] <= '9')))
            continue;
        uint64_t k = 0;
        while (k < wl && s[k] && ((s[k] >= 'A' && s[k] <= 'Z') ? s[k] + 32 : s[k]) ==
                                 ((w[k] >= 'A' && w[k] <= 'Z') ? w[k] + 32 : w[k])) k++;
        if (k == wl) return true;
    }
    return false;
}

// Первый токен вида /xxx в тексте (путь).
static void find_path(const char* s, char* out, int cap) {
    out[0] = 0;
    for (const char* p = s; *p; p++) {
        if (*p == '/' && (p == s || p[-1] == ' ')) {
            int n = 0;
            while (p[n] && p[n] != ' ' && p[n] != '?' && p[n] != ',' && n + 1 < cap) { out[n] = p[n]; n++; }
            while (n > 1 && (out[n - 1] == '.' || out[n - 1] == '!')) n--;
            out[n] = 0;
            return;
        }
    }
}

// Роутер намерений: вопрос на естественном языке -> инструмент. Русские
// основы слов сравниваются в CP1251 (так их хранит и строковый литерал
// ядра), вопрос приводится к нижнему регистру (mind_lower_cp1251).
static bool route(const char* q) {
    static char low[1024];
    int ln = mind_to_cp1251(q, (int)m_strlen(q), low, sizeof(low));
    mind_lower_cp1251(low, ln);
    char path[128];
    find_path(q, path, sizeof(path));
    if (has_word(q, "process") || has_word(q, "task") || has_word(q, "memory") || has_word(q, "ram") ||
        has_word(q, "uptime") || has_word(q, "cpu") || has_word(q, "core") || has_word(q, "window") ||
        mind_has_stem(low, "процесс") || mind_has_stem(low, "памят") || mind_has_stem(low, "ядер") ||
        mind_has_stem(low, "ядра") || mind_has_stem(low, "процессор") || mind_has_stem(low, "окн") ||
        mind_has_stem(low, "задач") || mind_has_stem(low, "запущен") || mind_has_stem(low, "оператив")) {
        run_tool_and_ask("ps", "", q);
        return true;
    }
    // Qwen3 с инструментами: файлы и журнал модель запрашивает сама
    // (<tool_call>), роутер — запасной путь для моделей без этого умения.
    if (M.style == MIND_CHAT_QWEN3 && g_gp.agent) return false;
    if (has_word(q, "log") || has_word(q, "journal") || has_word(q, "dmesg") || has_word(q, "boot messages") ||
        mind_has_stem(low, "журнал") || mind_has_stem(low, "лог")) {
        run_tool_and_ask("logs", "", q);
        return true;
    }
    bool file_words = has_word(q, "file") || has_word(q, "folder") || has_word(q, "director") || has_word(q, "list") ||
                      mind_has_stem(low, "файл") || mind_has_stem(low, "папк") || mind_has_stem(low, "каталог");
    bool read_words = has_word(q, "read") || has_word(q, "content") || has_word(q, "inside") || has_word(q, "show") ||
                      mind_has_stem(low, "прочит") || mind_has_stem(low, "содерж") || mind_has_stem(low, "покажи");
    if (path[0] && read_words) {
        uint64_t n = m_strlen(path);
        bool looks_file = false;
        for (uint64_t i = n; i > 0 && path[i - 1] != '/'; i--) if (path[i - 1] == '.') looks_file = true;
        if (looks_file) { run_tool_and_ask("read", path, q); return true; }
    }
    if (path[0] || file_words) {
        run_tool_and_ask("ls", path[0] ? path : "/disk", q);
        return true;
    }
    return false;
}

static const char k_help[] =
    "Commands:\n"
    "  /ps [question]          processes, windows, memory\n"
    "  /ls [path] [question]   list a directory (default /disk)\n"
    "  /read <path> [question] show a file\n"
    "  /logs [question]        kernel log\n"
    "  /exec <command>         run a shell command\n"
    "  /new                    new conversation\n"
    "  /think                  Qwen3: reasoning mode on/off (slower, shown dimmed)\n"
    "  /model [path]           load another .gguf model\n"
    "Plain questions about processes, files or logs fetch the data automatically.\n"
    "Ctrl+L switches the keyboard layout of this panel (EN / RU).";

// Разделить "cmd rest" -> args (первое слово) и question (остальное).
static const char* split_word(const char* s, char* word, int cap) {
    while (*s == ' ') s++;
    int n = 0;
    while (*s && *s != ' ' && n + 1 < cap) word[n++] = *s++;
    word[n] = 0;
    while (*s == ' ') s++;
    return s;
}

static void handle_command(const char* text) {
    char cmd[16], arg[160];
    const char* rest = split_word(text + 1, cmd, sizeof(cmd));
    if (m_streq(cmd, "help") || m_streq(cmd, "?")) { msg_info(k_help); return; }
    if (m_streq(cmd, "new")) { mind_new_chat(); msg_info("new conversation"); return; }
    if (m_streq(cmd, "think")) {
        // Режим размышления Qwen3 меняется на ходу: системный промпт от него
        // не зависит (пустой <think></think> ставится в начало ОТВЕТА), так
        // что кэш префикса остаётся верным.
        if (M.style != MIND_CHAT_QWEN3) { msg_info("think mode needs a Qwen3 model"); return; }
        g_gp.think = !g_gp.think;
        msg_info(g_gp.think ? "think mode on - the model reasons first (slower, shown dimmed)" : "think mode off");
        serial_puts(g_gp.think ? "[mind] think on\n" : "[mind] think off\n");
        return;
    }
    if (m_streq(cmd, "model")) {
        if (mind_load_begin(*rest ? rest : 0) != 0) msg_info(M.err);
        else msg_info(M.st == MIND_ST_LOADING ? "loading model..." : "model loaded");
        return;
    }
    static char q8[1024];
    mind_to_utf8(rest, (int)m_strlen(rest), q8, sizeof(q8));
    if (m_streq(cmd, "ps") || m_streq(cmd, "logs")) { run_tool_and_ask(cmd, "", q8); return; }
    if (m_streq(cmd, "ls") || m_streq(cmd, "read")) {
        const char* q = q8;
        arg[0] = 0;
        if (*q8 == '/') q = split_word(q8, arg, sizeof(arg));
        if (m_streq(cmd, "read") && !arg[0]) { msg_info("usage: /read <path> [question]"); return; }
        run_tool_and_ask(cmd, arg, q);
        return;
    }
    if (m_streq(cmd, "exec")) { run_tool_and_ask("exec", rest, 0); return; }
    msg_info("unknown command - try /help");
}

static void submit_text(const char* text) {
    static char u8[1024];
    mind_to_utf8(text, (int)m_strlen(text), u8, sizeof(u8));
    g_last_q[0] = 0;
    m_append(g_last_q, sizeof(g_last_q), u8);
    if (M.chat && route(u8)) return;
    (void)start_turn(u8);
}

int mind_submit(const char* text) {
    mind_init();
    if (M.st == MIND_ST_BUSY && M.warming) {
        // Модель ещё считает системный промпт — вопрос подождёт.
        while (*text == ' ') text++;
        if (!*text) return 0;
        msg_add_s(msg_new(MIND_ROLE_USER), text);
        if (text[0] == '/') { handle_command(text); return 0; }
        g_deferred[0] = 0;
        m_append(g_deferred, sizeof(g_deferred), text);
        return 0;
    }
    if (M.st == MIND_ST_BUSY || M.st == MIND_ST_CONFIRM) return -1;
    while (*text == ' ') text++;
    if (!*text) return 0;
    msg_add_s(msg_new(MIND_ROLE_USER), text);
    serial_puts("[mind] user: ");
    serial_puts(text);
    serial_puts("\n");
    g_tool_rounds = 0;
    if (text[0] == '/') { handle_command(text); return 0; }
    if (M.st == MIND_ST_LOADING) {
        g_deferred[0] = 0;
        m_append(g_deferred, sizeof(g_deferred), text);
        return 0;
    }
    if (!M.have) {
        if (mind_load_begin(0) != 0) { msg_info(M.err); return -1; }
        if (M.st == MIND_ST_LOADING) {
            msg_info("loading model...");
            g_deferred[0] = 0;
            m_append(g_deferred, sizeof(g_deferred), text);
            return 0;
        }
    }
    submit_text(text);
    return 0;
}

void mind_cancel(void) {
    if (g_job_active) g_job.cancel = 1;
    if (M.st == MIND_ST_CONFIRM) mind_confirm(false);
}

void mind_new_chat(void) {
    if (M.st == MIND_ST_BUSY || M.st == MIND_ST_CONFIRM) return;
    // Qwen3: системный промпт уже в KV-кэше — начинаем сразу после него.
    g_npast = M.style == MIND_CHAT_QWEN3 ? g_sys_n : 0;
    g_pending = -1;
    g_ver++;
}

const char* mind_pending_exec(void) { return M.st == MIND_ST_CONFIRM ? g_exec_pending : 0; }

// Продолжить диалог с результатом инструмента, который вызвала модель.
static void continue_with_tool(const char* name, const char* args) {
    int n = mind_tool_run(name, args, g_tool_out, TOOL_OUT);
    tool_card(name, args, n);
    if (M.style == MIND_CHAT_QWEN3) {
        int max_new = g_gp.max_tokens;
        qturn_t q = { QT_RESULT, 0, 0, 0, g_tool_out, data_limit(n), 0 };
        if (qwen_build(&q, &max_new) < 0) {
            // Результат не влез в остаток контекста (проверено на Proxmox:
            // вывод logs после нескольких вопросов). Продолжение нельзя
            // оторвать от вызова — начинаем диалог заново после системного
            // промпта: тот же вопрос, тот же вызов «от ассистента» и его
            // результат (формат QT_ROUTED), при нужде — с урезанными данными.
            msg_info("context is full - started a new conversation");
            g_npast = g_sys_n;
            g_pending = -1;
            qturn_t r = { QT_ROUTED, g_last_q, mind_tool_index(name), args, g_tool_out, data_limit(n), 0 };
            if (qwen_build(&r, &max_new) < 0) {
                r.data_n = data_limit(r.data_n / 2);
                if (qwen_build(&r, &max_new) < 0) { msg_info("the tool output does not fit the context"); return; }
            }
        }
        g_cur = msg_new(MIND_ROLE_MIND);
        g_u8n = 0;
        job_launch(max_new, true);
        return;
    }
    g_prompt[0] = 0;
    m_append(g_prompt, sizeof(g_prompt), "Tool output:\n");
    uint64_t base = m_strlen(g_prompt);
    int lim = data_limit(n);
    m_memcpy(g_prompt + base, g_tool_out, (uint64_t)lim);
    g_prompt[base + (uint64_t)lim] = 0;
    (void)start_turn(g_prompt);
}

void mind_confirm(bool allow) {
    if (M.st != MIND_ST_CONFIRM) return;
    M.st = MIND_ST_READY;
    if (allow) {
        continue_with_tool("exec", g_exec_pending);
        return;
    }
    msg_info("exec denied");
    static const char denied[] = "the user denied running this command";
    if (M.style == MIND_CHAT_QWEN3) {
        m_memcpy(g_tool_out, denied, sizeof(denied));
        int max_new = g_gp.max_tokens;
        qturn_t q = { QT_RESULT, 0, 0, 0, g_tool_out, (int)sizeof(denied) - 1, 0 };
        if (qwen_build(&q, &max_new) < 0) return;
        g_cur = msg_new(MIND_ROLE_MIND);
        g_u8n = 0;
        job_launch(max_new, true);
        return;
    }
    g_prompt[0] = 0;
    m_append(g_prompt, sizeof(g_prompt), "Tool output:\n");
    m_append(g_prompt, sizeof(g_prompt), denied);
    (void)start_turn(g_prompt);
}

// Реплика MIND после вызова инструмента: сам вызов (<tool_call>… или
// <tool>…) пользователю не показываем — вместо него будет карточка данных.
static void cut_tool_call(msg_t* m) {
    if (!m || !m->text) return;
    char* cut = 0;
    for (char* p = m->text; *p; p++) if (m_starts(p, "<tool_call>") || m_starts(p, "<tool>")) { cut = p; break; }
    if (!cut) return;
    while (cut > m->text && (cut[-1] == ' ' || cut[-1] == '\n' || cut[-1] == '\r')) cut--;
    *cut = 0;
    m->len = (int)(cut - m->text);
    g_ver++;
    if (m->len == 0) msg_pop(m);
}

static void print_reply_ids(job_t* j) {
    // Детерминированный режим: ids ответа — тест сверяет их с эталоном
    // tools/mind_ref.py. Сгенерированные токены лежат в хвосте g_hist
    // (последний — в j->last, если не прогнан).
    serial_puts("[mind] ids: ");
    int in_kv = j->n_gen - (j->last_pending ? 1 : 0);
    for (int k = 0; k < in_kv; k++) {
        if (k) serial_puts(",");
        serial_print_dec((uint64_t)g_hist[g_npast - in_kv + k]);
    }
    if (j->last_pending) {
        if (in_kv) serial_puts(",");
        serial_print_dec((uint64_t)j->last);
    }
    serial_puts("\n");
}

// Разбор завершённого задания (BSP).
static void job_finish(void) {
    job_t* j = &g_job;
    g_job_active = false;
    M.st = MIND_ST_READY;
    // Лимит токенов: последний токен уже прогнан через модель (он в кэше).
    // Конец реплики / вызов инструмента: последний токен сэмплирован, но не
    // прогнан — он станет первым входом следующего хода.
    g_pending = j->last_pending ? j->last : -1;
    uint64_t pf = j->t1 - j->t0, gn = j->t2 - j->t1;
    if (M.warming) {
        M.warming = false;
        if (j->stop == STOP_CANCEL || j->stop == STOP_CTX) {
            g_npast = 0;
            g_sys_n = 0;
        } else {
            g_sys_n = g_npast;
            g_pending = -1;
        }
        g_last_pps10 = pf ? (int)((uint64_t)j->n_input * 10000 / pf) : 0;
        serial_puts("[mind] system prompt cached: ");
        serial_print_dec((uint64_t)g_sys_n);
        serial_puts(" tokens in ");
        serial_print_dec(pf);
        serial_puts(" ms (prefill tok/s x10=");
        serial_print_dec((uint64_t)g_last_pps10);
        serial_puts(")\n");
        g_ver++;
        if (g_deferred[0]) {
            static char q[512];
            q[0] = 0;
            m_append(q, sizeof(q), g_deferred);
            g_deferred[0] = 0;
            serial_puts("[mind] user: ");
            serial_puts(q);
            serial_puts("\n");
            g_tool_rounds = 0;
            submit_text(q);
        }
        return;
    }
    g_last_gen = j->n_gen;
    g_last_pps10 = pf ? (int)((uint64_t)j->n_input * 10000 / pf) : 0;
    g_last_tps10 = gn ? (int)((uint64_t)j->n_gen * 10000 / gn) : 0;
    serial_puts("[mind] reply done: stop=");
    serial_print_dec((uint64_t)j->stop);
    serial_puts(" prefill=");
    serial_print_dec((uint64_t)j->n_input);
    serial_puts(" gen=");
    serial_print_dec((uint64_t)j->n_gen);
    serial_puts(" tok/s=");
    serial_print_dec((uint64_t)(g_last_tps10 / 10));
    serial_puts(" ap=");
    serial_puts(g_job_on_ap ? "yes" : "no");
    serial_puts(" prefill_ms=");
    serial_print_dec(pf);
    serial_puts(" gen_ms=");
    serial_print_dec(gn);
    serial_puts("\n");
    if (g_cur && g_cur->text) {
        serial_puts("[mind] reply: ");
        serial_puts(g_cur->text);
        serial_puts("\n");
    }
    if (j->greedy && j->n_gen > 0) print_reply_ids(j);
    if (j->stop == STOP_CANCEL) {
        // Кэш мог остаться с половиной промпта — надёжнее начать заново
        // (у Qwen3 системный промпт в кэше не тронут — с него).
        g_npast = M.style == MIND_CHAT_QWEN3 ? g_sys_n : 0;
        g_pending = -1;
        msg_info("stopped");
        return;
    }
    if (j->stop == STOP_CTX) { msg_info("context is full - type /new"); return; }
    if (j->stop == STOP_TOOL && g_cur && M.chat) {
        char name[16], args[200];
        bool parsed = g_cur->text && (M.style == MIND_CHAT_QWEN3
                          ? mind_parse_tool_call_json(g_cur->text, name, sizeof(name), args, sizeof(args))
                          : mind_parse_tool_call_tag(g_cur->text, name, sizeof(name), args, sizeof(args)));
        cut_tool_call(g_cur);
        g_cur = 0;
        if (parsed) {
            serial_puts("[mind] model calls tool: ");
            serial_puts(name);
            serial_puts(" ");
            serial_puts(args);
            serial_puts("\n");
        }
        if (g_tool_rounds < 3 && parsed && mind_tool_known(name)) {
            g_tool_rounds++;
            if (mind_tool_needs_confirm(name)) {
                g_exec_pending[0] = 0;
                m_append(g_exec_pending, sizeof(g_exec_pending), args);
                M.st = MIND_ST_CONFIRM;
                serial_puts("[mind] model asks to exec: ");
                serial_puts(args);
                serial_puts("\n");
                return;
            }
            continue_with_tool(name, args);
        } else if (M.style == MIND_CHAT_QWEN3) {
            msg_info(parsed ? "the model asked for an unknown tool" : "the model produced a broken tool call");
        }
    }
}

// Байты токена -> реплика (с переносом незаконченной UTF-8-последовательности).
static void emit_piece(const char* pc, int n) {
    char buf[80];
    int bn = 0;
    for (int k = 0; k < g_u8n; k++) buf[bn++] = g_u8[k];
    for (int k = 0; k < n && bn < (int)sizeof(buf); k++) buf[bn++] = pc[k];
    // Отрезаем хвост, если он — начало многобайтового символа.
    int cut = bn;
    for (int back = 1; back <= 3 && back <= bn; back++) {
        uint8_t c = (uint8_t)buf[bn - back];
        if ((c & 0xC0) == 0x80) continue;
        int need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (need > back) cut = bn - back;
        break;
    }
    g_u8n = bn - cut;
    for (int k = 0; k < g_u8n; k++) g_u8[k] = buf[cut + k];
    if (cut > 0) msg_add_utf8(g_cur, buf, cut);
}

static void drain_ring(void) {
    uint32_t w = __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE);
    while (g_ring_r != w) {
        int tok = g_ring[g_ring_r % RING_SIZE];
        __atomic_store_n(&g_ring_r, g_ring_r + 1, __ATOMIC_RELEASE);
        if (!g_cur) continue;
        char pc[64];
        int n = mind_tok_piece(&M.t, tok, pc, sizeof(pc));
        // Первый пробел реплики SentencePiece — артефакт «▁» в начале слова.
        if (g_cur->len == 0 && g_u8n == 0 && n > 0 && pc[0] == ' ') { if (n > 1) emit_piece(pc + 1, n - 1); }
        else if (n > 0) emit_piece(pc, n);
    }
}

// [48.2] Интерфейс занят анимацией: порция чтения модели (LOAD_SLICE_MS = 40 мс
// за проход цикла WM) в эти 0.2–0.3 с пропускается. Иначе панель, закрытая во
// время загрузки модели, уезжала четырьмя кадрами вместо четырнадцати.
static bool g_ui_busy = false;
void mind_set_ui_busy(bool busy) { g_ui_busy = busy; }

void mind_poll(void) {
    if (M.st == MIND_ST_LOADING && g_ui_busy) return;
    if (M.st == MIND_ST_LOADING) {
        (void)load_step();
        g_ver++;
        if (M.st == MIND_ST_READY) {
            msg_info("model loaded");
            if (M.style == MIND_CHAT_QWEN3) {
                start_warmup();
                if (M.warming) return;          // вопрос подождёт конца prefill системы
            }
            if (g_deferred[0]) {
                static char q[512];
                q[0] = 0;
                m_append(q, sizeof(q), g_deferred);
                g_deferred[0] = 0;
                // Повтор без повторной записи вопроса в журнал.
                submit_text(q);
            }
        } else if (M.st == MIND_ST_ERROR) {
            msg_info(M.err);
        }
        return;
    }
    if (!g_job_active) return;
    if (!g_job_on_ap) {
        // Однопроцессорная машина: генерируем шагами прямо в главном цикле,
        // не дольше ~30 мс за кадр, чтобы UI оставался живым.
        uint64_t t0 = timer_get_uptime_ms();
        bool more = true;
        while (more && timer_get_uptime_ms() - t0 < 30) more = job_step(&g_job);
        if (!more) { g_job.t2 = timer_get_uptime_ms(); g_job.running = 0; }
    }
    drain_ring();
    if (!__atomic_load_n(&g_job.running, __ATOMIC_ACQUIRE)) {
        drain_ring();
        job_finish();
    }
}

// ----------------------------------------------------------------------------
// Статус.
// ----------------------------------------------------------------------------
void mind_status(char* out, int cap) {
    out[0] = 0;
    if (M.st == MIND_ST_LOADING) {
        m_append(out, (uint64_t)cap, "loading ");
        m_append(out, (uint64_t)cap, M.label);
        m_append(out, (uint64_t)cap, " ");
        m_append_dec(out, (uint64_t)cap, mind_load_progress());
        m_append(out, (uint64_t)cap, "%");
        return;
    }
    if (!M.have) {
        m_append(out, (uint64_t)cap, M.st == MIND_ST_ERROR ? M.err : "no model loaded");
        return;
    }
    m_append(out, (uint64_t)cap, M.label);
    m_append(out, (uint64_t)cap, " \xB7 ");
    uint64_t p = M.m.n_params;
    if (p >= 1000000) { m_append_dec(out, (uint64_t)cap, (int64_t)(p / 1000000)); m_append(out, (uint64_t)cap, "M"); }
    else { m_append_dec(out, (uint64_t)cap, (int64_t)(p / 1000)); m_append(out, (uint64_t)cap, "K"); }
    m_append(out, (uint64_t)cap, " params \xB7 ");
    m_append(out, (uint64_t)cap, gguf_type_name(M.m.main_type));
    m_append(out, (uint64_t)cap, " \xB7 ctx ");
    m_append_dec(out, (uint64_t)cap, g_npast);
    m_append(out, (uint64_t)cap, "/");
    m_append_dec(out, (uint64_t)cap, M.m.n_ctx);
    if (M.warming) m_append(out, (uint64_t)cap, " \xB7 preparing");
    if (g_last_tps10) {
        m_append(out, (uint64_t)cap, " \xB7 ");
        m_append_dec1(out, (uint64_t)cap, g_last_tps10);
        m_append(out, (uint64_t)cap, " tok/s");
    }
    if (g_gp.think) m_append(out, (uint64_t)cap, " \xB7 think");
    m_append(out, (uint64_t)cap, " \xB7 ");
    m_append_dec(out, (uint64_t)cap, smp_pool_workers() + 1);
    m_append(out, (uint64_t)cap, " cores");
}

void mind_set_greedy(bool greedy) { g_force_greedy = greedy; }
int mind_last_gen_tokens(void) { return g_last_gen; }
int mind_last_tok_per_s10(void) { return g_last_tps10; }
int mind_last_prefill_per_s10(void) { return g_last_pps10; }

// ----------------------------------------------------------------------------
// Самопроверка (smoke 49 / 49.1) и замер.
// ----------------------------------------------------------------------------
// Эталон: tools/mind_ref.py gen stories260K.gguf "Once upon a time" 32
// (независимая numpy-реализация, см. шапку того файла).
static const int32_t k_golden_prompt[5] = { 1, 403, 407, 261, 378 };
static const int32_t k_golden_ids[32] = {
    432, 383, 286, 261, 376, 298, 315, 421, 395, 317, 426, 338, 401, 396, 267, 337,
    410, 408, 419, 292, 411, 322, 265, 282, 295, 433, 426, 385, 328, 432, 358, 394,
};

// Синхронная greedy-генерация через обычный механизм заданий.
static int gen_ids_sync(const int32_t* prompt, int n_prompt, int max_new, bool on_ap, int32_t* out, int cap) {
    g_npast = 0;
    g_pending = -1;
    if (n_prompt <= 0 || n_prompt > MIND_MAX_INPUT) return -1;
    m_memcpy(g_job.input, prompt, (uint64_t)n_prompt * 4);
    g_job.n_input = n_prompt;
    g_cur = 0;
    bool fg = g_force_greedy;
    g_force_greedy = true;
    job_launch(max_new, on_ap);
    g_force_greedy = fg;
    int n = 0;
    uint64_t t0 = timer_get_uptime_ms();
    for (;;) {
        if (!g_job_on_ap) {
            if (!job_step(&g_job)) { g_job.t2 = timer_get_uptime_ms(); g_job.running = 0; }
        } else {
            __asm__ __volatile__("sti; hlt");
        }
        uint32_t w = __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE);
        while (g_ring_r != w) {
            int tok = g_ring[g_ring_r % RING_SIZE];
            __atomic_store_n(&g_ring_r, g_ring_r + 1, __ATOMIC_RELEASE);
            if (n < cap) out[n++] = tok;
        }
        if (!__atomic_load_n(&g_job.running, __ATOMIC_ACQUIRE)) break;
        if (timer_get_uptime_ms() - t0 > 1800000) { g_job.cancel = 1; }   // страховка (TCG медленный)
    }
    uint32_t w = __atomic_load_n(&g_ring_w, __ATOMIC_ACQUIRE);
    while (g_ring_r != w) {
        int tok = g_ring[g_ring_r % RING_SIZE];
        g_ring_r++;
        if (n < cap) out[n++] = tok;
    }
    g_job_active = false;
    M.st = MIND_ST_READY;
    uint64_t gn = g_job.t2 - g_job.t1;
    g_last_tps10 = gn ? (int)((uint64_t)g_job.n_gen * 10000 / gn) : 0;
    g_npast = 0;
    g_sys_n = 0;
    return n;
}

void mind_boot_bench(void) {
    mind_init();
    conf_load();
    int n = g_conf.bench_on_boot;
    if (n <= 0) return;
    uint64_t t0 = timer_get_uptime_ms();
    if (mind_load_sync(0) != 0) {
        serial_puts("[mind] boot bench: load failed: ");
        serial_puts(M.err);
        serial_puts("\n");
        return;
    }
    uint64_t tl = timer_get_uptime_ms() - t0;
    int g = 0, p = 0;
    int got = mind_bench(n, &g, &p);
    serial_puts("[mind] boot bench: model=");
    serial_puts(M.label);
    serial_puts(" load_ms=");
    serial_print_dec(tl);
    serial_puts(" load_MBps=");
    serial_print_dec(tl ? M.file_size / 1024 * 1000 / tl / 1024 : 0);
    serial_puts(" tokens=");
    serial_print_dec((uint64_t)(got > 0 ? got : 0));
    serial_puts(" gen_tps10=");
    serial_print_dec((uint64_t)g);
    serial_puts(" prefill_tps10=");
    serial_print_dec((uint64_t)p);
    serial_puts(" simd=");
    serial_puts(mind_math_kernels());
    serial_puts(" cores=");
    serial_print_dec((uint64_t)(smp_pool_workers() + 1));
    serial_puts("\n");
    mind_unload();
}

// Замер: prefill — первые 64 токена системного промпта (для чат-модели; это
// ровно та работа, что ждёт пользователя после /new), затем n токенов greedy.
int mind_bench(int n, int* gen_tps10, int* prefill_tps10) {
    if (!M.have || M.st != MIND_ST_READY) return -1;
    static int32_t out[512];
    static int32_t in[MIND_MAX_INPUT];
    if (n < 1) n = 1;
    if (n > 500) n = 500;
    int k;
    if (M.chat) {
        mind_prompt_t p;
        mind_prompt_begin(&p, &M.t, in, MIND_MAX_INPUT, g_ptext, sizeof(g_ptext));
        mind_chat_system(&p, M.style, M.style == MIND_CHAT_QWEN3 ? k_system_qwen : k_system,
                         M.style == MIND_CHAT_QWEN3 ? MIND_TOOLS_ALL : 0);
        k = mind_prompt_end(&p);
        if (k > 64) k = 64;
    } else {
        k = mind_tok_encode(&M.t, "Once upon a time", 16, M.t.add_bos, in, MIND_MAX_INPUT);
    }
    int got = gen_ids_sync(in, k, n, true, out, 512);
    uint64_t pf = g_job.t1 - g_job.t0;
    if (gen_tps10) *gen_tps10 = g_last_tps10;
    if (prefill_tps10) *prefill_tps10 = pf ? (int)((uint64_t)g_job.n_input * 10000 / pf) : 0;
    return got;
}

static bool ids_equal(const int32_t* a, const int32_t* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

static void print_ids(const int32_t* a, int n) {
    for (int i = 0; i < n; i++) {
        if (i) serial_puts(",");
        serial_print_dec((uint64_t)a[i]);
    }
}

// [49.1] Память выше 4 ГБ: 64 МБ блоком, узор в начале/середине/конце.
static bool highmem_test(void) {
    const uint64_t pages = 16384;
    uint64_t phys = pmm_alloc_big(pages);
    if (!phys) { serial_puts("[smoke 49.1] highmem: alloc FAIL\n"); return false; }
    volatile uint64_t* p = (volatile uint64_t*)(uintptr_t)phys;
    const uint64_t words = pages * PAGE_SIZE / 8;
    const uint64_t at[3] = { 0, words / 2, words - 1 };
    for (int i = 0; i < 3; i++) p[at[i]] = 0xA5A5000000000000ull ^ (at[i] * 0x9E3779B97F4A7C15ull);
    bool ok = true;
    for (int i = 0; i < 3; i++) if (p[at[i]] != (0xA5A5000000000000ull ^ (at[i] * 0x9E3779B97F4A7C15ull))) ok = false;
    pmm_free_big(phys, pages);
    pmm_stats_t st = pmm_get_stats();
    serial_puts("[smoke 49.1] highmem ");
    serial_puts(ok ? "OK" : "FAIL");
    serial_puts(" block@0x");
    serial_print_hex(phys, 16);
    serial_puts(phys >= PMM_MAX_PHYS ? " (above 4 GB)" : " (below 4 GB)");
    serial_puts(" high_total_mb=");
    serial_print_dec(st.high_total_pages * PAGE_SIZE / (1024 * 1024));
    serial_puts(" ram_mb=");
    serial_print_dec(pmm_total_ram() / (1024 * 1024));
    serial_puts("\n");
    return ok;
}

bool mind_self_test(void) {
    mind_init();
    bool ok = smp_pool_self_test();

    // klog: журнал ядра должен содержать только что напечатанное.
    static char tail[512];
    klog_read_tail(tail, sizeof(tail));
    bool klog_ok = false;
    for (char* p = tail; *p; p++) if (m_starts(p, "[smoke 49] pool")) { klog_ok = true; break; }
    serial_puts("[smoke 49] klog ");
    serial_puts(klog_ok ? "OK" : "FAIL");
    serial_puts(" total=");
    serial_print_dec(klog_total());
    serial_puts("\n");
    ok = ok && klog_ok;
    ok = highmem_test() && ok;

    // Модель из initrd.
    if (mind_load_sync(MIND_BUILTIN_MODEL) != 0) {
        serial_puts("[smoke 49] load FAIL\n");
        return false;
    }
    serial_puts("[smoke 49] model ");
    serial_puts(M.label);
    serial_puts(" v");
    serial_print_dec(M.g.version);
    serial_puts(" tensors=");
    serial_print_dec(M.g.n_tensors);
    serial_puts(" vocab=");
    serial_print_dec((uint64_t)M.t.n_vocab);
    serial_puts("\n");

    int32_t ids[40];
    int n = mind_tok_encode(&M.t, "Once upon a time", 16, M.t.add_bos, ids, 40);
    bool tok_ok = n == 5 && ids_equal(ids, k_golden_prompt, 5);
    serial_puts("[smoke 49] tokenize ids=");
    if (n > 0) print_ids(ids, n);
    serial_puts(tok_ok ? " OK\n" : " MISMATCH\n");
    ok = ok && tok_ok;

    static int32_t out[40];
    int g1 = gen_ids_sync(k_golden_prompt, 5, 32, true, out, 40);
    bool ap_ok = g1 == 32 && ids_equal(out, k_golden_ids, 32);
    serial_puts("[smoke 49] greedy on pool (ap=");
    serial_puts(g_job_on_ap ? "yes" : "no");
    serial_puts(") ");
    serial_puts(ap_ok ? "OK" : "MISMATCH");
    serial_puts(" ids=");
    if (g1 > 0) print_ids(out, g1 < 8 ? g1 : 8);
    serial_puts("...\n");

    static int32_t out2[40];
    int g2 = gen_ids_sync(k_golden_prompt, 5, 32, false, out2, 40);
    bool bsp_ok = g2 == 32 && ids_equal(out2, k_golden_ids, 32);
    serial_puts("[smoke 49] greedy on BSP ");
    serial_puts(bsp_ok ? "OK" : "MISMATCH");
    serial_puts("\n");
    ok = ok && ap_ok && bsp_ok;

    // [49.1] Пакетный prefill побитово равен поштучному (логиты последнего
    // токена промпта из 5 токенов + ещё 3 сгенерированных).
    {
        static float ref[1024];
        static int32_t seq[8];
        for (int i = 0; i < 5; i++) seq[i] = k_golden_prompt[i];
        for (int i = 0; i < 3; i++) seq[5 + i] = k_golden_ids[i];
        bool bok = M.m.n_vocab <= 1024;
        float* lg = 0;
        for (int i = 0; i < 8 && bok; i++) lg = mind_model_forward(&M.m, seq[i], i);
        if (bok && lg) m_memcpy(ref, lg, (uint64_t)M.m.n_vocab * 4);
        lg = bok ? mind_model_forward_batch(&M.m, seq, 5, 0, false) : 0;
        lg = bok ? mind_model_forward_batch(&M.m, seq + 5, 3, 5, true) : 0;
        bool same = bok && lg;
        for (int i = 0; same && i < M.m.n_vocab; i++) {
            union { float f; uint32_t u; } a = { ref[i] }, b = { lg[i] };
            if (a.u != b.u) same = false;
        }
        serial_puts("[smoke 49.1] batch prefill = sequential: ");
        serial_puts(same ? "OK (bit-exact)\n" : "MISMATCH\n");
        ok = ok && same;
    }

    // Текст — глазами (тест проверяет его подстрокой).
    static char txt[512];
    txt[0] = 0;
    for (int i = 0; i < g1 && i < 32; i++) {
        char pc[64];
        int k = mind_tok_piece(&M.t, out[i], pc, sizeof(pc) - 1);
        pc[k] = 0;
        m_append(txt, sizeof(txt), pc);
    }
    for (char* p = txt; *p; p++) if (*p == '\n') *p = ' ';
    serial_puts("[smoke 49] text: Once upon a time");
    serial_puts(txt);
    serial_puts("\n");

    // Инструменты и разбор вызова.
    int r1 = mind_tool_run("ps", "", g_tool_out, TOOL_OUT);
    bool ps_ok = r1 > 0 && has_word(g_tool_out, "uptime:") && has_word(g_tool_out, "memory:");
    int r2 = mind_tool_run("ls", "/", g_tool_out, TOOL_OUT);
    bool ls_ok = r2 > 0 && has_word(g_tool_out, MIND_BUILTIN_MODEL);
    int r3 = mind_tool_run("logs", "", g_tool_out, TOOL_OUT);
    bool lg_ok = r3 > 0 && has_word(g_tool_out, "[smoke 49]");
    int r4 = mind_tool_run("read", "/disk/users.txt", g_tool_out, TOOL_OUT);
    bool deny_ok = r4 > 0 && has_word(g_tool_out, "access denied");
    char nm[16], ar[64];
    bool parse_ok = mind_parse_tool_call_tag("Let me check. <tool>ls /disk/vault</tool>", nm, sizeof(nm), ar, sizeof(ar)) &&
                    m_streq(nm, "ls") && m_streq(ar, "/disk/vault");
    serial_puts("[smoke 49] tools ps=");
    serial_puts(ps_ok ? "OK" : "FAIL");
    serial_puts(" ls=");
    serial_puts(ls_ok ? "OK" : "FAIL");
    serial_puts(" logs=");
    serial_puts(lg_ok ? "OK" : "FAIL");
    serial_puts(" deny=");
    serial_puts(deny_ok ? "OK" : "FAIL");
    serial_puts(" parse=");
    serial_puts(parse_ok ? "OK" : "FAIL");
    serial_puts("\n");
    ok = ok && ps_ok && ls_ok && lg_ok && deny_ok && parse_ok;

    // [49.1] Родной формат Qwen3: JSON внутри <tool_call>, с экранированием.
    bool j1 = mind_parse_tool_call_json("<tool_call>\n{\"name\": \"ls\", \"arguments\": {\"path\": \"/disk/vault\"}}\n</tool_call>",
                                        nm, sizeof(nm), ar, sizeof(ar)) && m_streq(nm, "ls") && m_streq(ar, "/disk/vault");
    bool j2 = mind_parse_tool_call_json("ok <tool_call>{\"arguments\": {\"command\": \"echo \\\"hi\\\"\"}, \"name\": \"exec\"}</tool_call>",
                                        nm, sizeof(nm), ar, sizeof(ar)) && m_streq(nm, "exec") && m_streq(ar, "echo \"hi\"");
    bool j3 = mind_parse_tool_call_json("<tool_call>\n{\"name\": \"ps\", \"arguments\": {}}\n</tool_call>",
                                        nm, sizeof(nm), ar, sizeof(ar)) && m_streq(nm, "ps") && ar[0] == 0;
    bool j4 = !mind_parse_tool_call_json("<tool_call>{\"name\": \"ls\"", nm, sizeof(nm), ar, sizeof(ar));
    serial_puts("[smoke 49.1] tool_call json parse ");
    serial_puts(j1 && j2 && j3 && j4 ? "OK\n" : "FAIL\n");
    ok = ok && j1 && j2 && j3 && j4;

    mind_unload();
    serial_puts(ok ? "[smoke 49] AMPLIFY MIND self-test PASS\n" : "[smoke 49] AMPLIFY MIND self-test FAIL\n");
    return ok;
}

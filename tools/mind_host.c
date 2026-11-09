// ============================================================================
// tools/mind_host.c — хостовая сборка движка AMPLIFY MIND (этапы 49, 49.1).
// ----------------------------------------------------------------------------
// Те же исходники ядра (mind_gguf/math/model/tok/chat.c) компилируются обычным
// gcc под Linux (WSL) с заглушками kmalloc/pmm/smp_pool. Нужно для быстрой
// проверки корректности без загрузки QEMU: вывод в том же формате, что у
// tools/mind_ref.py ("prompt: ...", "ids: ...", "text: ...").
// parallel_for — на pthreads (MIND_THREADS, по умолчанию все ядра хоста):
// порядок операций внутри строки/головы от числа потоков не зависит, так что
// ids те же, что в ядре.
//
//   gcc -O2 -pthread -Ikernel -o build/mind_host tools/mind_host.c \
//       kernel/arch/x86_64/mind_{gguf,math,model,tok,chat}.c -lm
//   build/mind_host gen  <model.gguf> "<prompt>" <n> [--chat] [--system=...]
//        [--tools=all|ps,ls,...] [--think] [--prefix=...] [--tool-response=<файл>]
//        [--nobatch]
//   build/mind_host prompt <model.gguf> "<user>" [--system=...] [--tools=...] [--think]
//   build/mind_host tok  <model.gguf> "<text>"
//   build/mind_host bench <model.gguf> <n>
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include "arch/x86_64/mind_gguf.h"
#include "arch/x86_64/mind_model.h"
#include "arch/x86_64/mind_tok.h"
#include "arch/x86_64/mind_chat.h"
#include "arch/x86_64/smp_pool.h"

// ---- заглушки ядра -----------------------------------------------------------
void* kmalloc(size_t n) { return malloc(n ? n : 1); }
void  kfree(void* p) { free(p); }
uint64_t pmm_alloc_pages(uint64_t count) {
    void* p = aligned_alloc(4096, count * 4096);
    return (uint64_t)(uintptr_t)p;
}
void pmm_free_pages(uint64_t phys, uint64_t count) { (void)count; free((void*)(uintptr_t)phys); }
uint64_t pmm_alloc_big(uint64_t count) { return pmm_alloc_pages(count); }
void pmm_free_big(uint64_t phys, uint64_t count) { pmm_free_pages(phys, count); }

// ---- parallel_for на pthreads ---------------------------------------------------
static int g_threads = 1;
typedef struct { smp_pool_range_fn fn; void* ctx; int b, e; } pf_arg_t;
static void* pf_thread(void* a) {
    pf_arg_t* p = (pf_arg_t*)a;
    p->fn(p->ctx, p->b, p->e);
    return 0;
}
int  smp_pool_workers(void) { return g_threads - 1; }
int  smp_pool_current_slot(void) { return 0; }
void smp_pool_parallel_for(smp_pool_range_fn fn, void* ctx, int n, int grain) {
    (void)grain;
    int T = g_threads;
    if (T <= 1 || n < 2 * T) { fn(ctx, 0, n); return; }
    pthread_t th[64];
    pf_arg_t ar[64];
    if (T > 64) T = 64;
    for (int i = 0; i < T; i++) {
        ar[i].fn = fn; ar[i].ctx = ctx;
        ar[i].b = (int)((long)n * i / T);
        ar[i].e = (int)((long)n * (i + 1) / T);
    }
    for (int i = 1; i < T; i++) pthread_create(&th[i], 0, pf_thread, &ar[i]);
    pf_thread(&ar[0]);
    for (int i = 1; i < T; i++) pthread_join(th[i], 0);
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int argmax(const float* x, int n) {
    int b = 0;
    for (int i = 1; i < n; i++) if (x[i] > x[b]) b = i;
    return b;
}

static const char* opt(int argc, char** argv, const char* name) {
    size_t l = strlen(name);
    for (int i = 3; i < argc; i++)
        if (!strncmp(argv[i], "--", 2) && !strncmp(argv[i] + 2, name, l) && argv[i][2 + l] == '=')
            return argv[i] + 3 + l;
    return 0;
}
static int flag(int argc, char** argv, const char* name) {
    for (int i = 3; i < argc; i++) if (!strncmp(argv[i], "--", 2) && !strcmp(argv[i] + 2, name)) return 1;
    return 0;
}

static void print_ids(const char* label, const int32_t* ids, int n) {
    printf("%s:", label);
    for (int i = 0; i < n; i++) printf("%s%d", i ? "," : " ", ids[i]);
    printf("\n");
}

static void print_text(const char* label, const mind_tok_t* tok, const int32_t* ids, int n) {
    printf("%s: ", label);
    for (int i = 0; i < n; i++) {
        char pc[64];
        int k = mind_tok_piece(tok, ids[i], pc, sizeof pc);
        for (int j = 0; j < k; j++) {
            unsigned char c = (unsigned char)pc[j];
            if (c == '\n') printf("\\n");
            else if (c >= 32 && c < 127) putchar(c);
            else printf("\\x%02x", c);
        }
    }
    printf("\n");
}

static mind_model_t g_m;
static int g_nobatch;

// Прогнать ids через модель начиная с позиции *pos; вернуть логиты последнего.
static float* prefill(const int32_t* ids, int n, int* pos) {
    float* lg = 0;
    for (int i = 0; i < n;) {
        int k = g_nobatch ? 1 : n - i;
        if (k > MIND_BATCH) k = MIND_BATCH;
        lg = mind_model_forward_batch(&g_m, ids + i, k, *pos, i + k == n);
        if (!lg) { fprintf(stderr, "context overflow\n"); exit(1); }
        *pos += k;
        i += k;
    }
    return lg;
}

// Greedy до конца реплики / </tool_call>. Возврат — число токенов в out.
static int generate(const mind_tok_t* tok, float* lg, int* pos, int n_new, int32_t* out) {
    int no = 0;
    for (int i = 0; i < n_new; i++) {
        int nx = argmax(lg, g_m.n_vocab);
        out[no++] = nx;
        if (mind_tok_is_eog(tok, nx) || (tok->tool_call_close >= 0 && nx == tok->tool_call_close)) break;
        lg = mind_model_forward(&g_m, nx, (*pos)++);
    }
    return no;
}

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: mind_host gen|prompt|tok|bench model.gguf ...\n"); return 2; }
    const char* cmd = argv[1];
    const char* th = getenv("MIND_THREADS");
    g_threads = th ? atoi(th) : (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (g_threads < 1) g_threads = 1;
    FILE* f = fopen(argv[2], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = aligned_alloc(4096, ((size_t)sz + 4095) / 4096 * 4096);
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { perror("read"); return 1; }
    fclose(f);

    char err[128];
    gguf_file_t g;
    if (gguf_parse(&g, buf, (uint64_t)sz, err, sizeof(err)) != 0) { printf("gguf: %s\n", err); return 1; }
    mind_math_init();
    mind_tok_t tok;
    if (mind_tok_init(&tok, &g, err, sizeof(err)) != 0) { printf("tok: %s\n", err); return 1; }

    static int32_t ids[16384];
    static char tbuf[65536];
    if (!strcmp(cmd, "tok")) {
        int n = mind_tok_encode(&tok, argv[3], (int)strlen(argv[3]), tok.add_bos, ids, 16384);
        print_ids("ids", ids, n);
        return 0;
    }

    if (!strcmp(cmd, "tokfile")) {
        // Строки файла — тексты в hex (UTF-8); вывод — ids по строке.
        FILE* tf = fopen(argv[3], "r");
        if (!tf) { perror("tokfile"); return 1; }
        static char line[65536], txt[32768];
        while (fgets(line, sizeof line, tf)) {
            int n = 0;
            for (int i = 0; line[i] && line[i + 1] && line[i] != '\n'; i += 2) {
                unsigned v;
                sscanf(line + i, "%2x", &v);
                txt[n++] = (char)v;
            }
            int k = mind_tok_encode(&tok, txt, n, false, ids, 16384);
            for (int i = 0; i < k; i++) printf("%s%d", i ? "," : "", ids[i]);
            printf("\n");
        }
        fclose(tf);
        return 0;
    }

    mind_str_t arch;
    int style = MIND_CHAT_CHATML;
    if (gguf_get_str(&g, "general.architecture", &arch) && arch.len == 5 && !memcmp(arch.p, "qwen3", 5))
        style = MIND_CHAT_QWEN3;
    const char* sys = opt(argc, argv, "system");
    const char* tools = opt(argc, argv, "tools");
    // --tools=tools/mind_tools.json (как у mind_ref.py) — все инструменты,
    // --tools=ps,ls — список имён.
    unsigned tmask = !tools ? 0 : strstr(tools, ".json") ? MIND_TOOLS_ALL : mind_tool_mask_parse(tools);
    int think = flag(argc, argv, "think");
    g_nobatch = flag(argc, argv, "nobatch");

    int n_new = argc > 4 ? atoi(argv[4]) : 32;
    int n = 0;
    int chat = flag(argc, argv, "chat") || !strcmp(cmd, "prompt");
    if (!strcmp(cmd, "gen") || !strcmp(cmd, "prompt")) {
        if (chat) {
            mind_prompt_t p;
            mind_prompt_begin(&p, &tok, ids, 16384, tbuf, sizeof tbuf);
            if ((sys && *sys) || tmask) mind_chat_system(&p, style, sys, tmask);
            mind_chat_user(&p, argv[3]);
            mind_chat_assistant(&p, style, think, opt(argc, argv, "prefix"));
            n = mind_prompt_end(&p);
            if (n < 0) { printf("prompt too long\n"); return 1; }
        } else {
            n = mind_tok_encode(&tok, argv[3], (int)strlen(argv[3]), tok.add_bos, ids, 16384);
        }
        if (!strcmp(cmd, "prompt")) { print_ids("prompt", ids, n); return 0; }
    } else if (!strcmp(cmd, "bench")) {
        n_new = atoi(argv[3]);
        n = mind_tok_encode(&tok, "Once upon a time", 16, tok.add_bos, ids, 16384);
    } else {
        fprintf(stderr, "unknown command\n");
        return 2;
    }

    const char* resp = opt(argc, argv, "tool-response");
    if (mind_model_init(&g_m, &g, n + n_new + 8 + (resp ? 1200 : 0), err, sizeof(err)) != 0) {
        printf("model: %s\n", err);
        return 1;
    }
    print_ids("prompt", ids, n);

    double t0 = now_s();
    int pos = 0;
    float* lg = prefill(ids, n, &pos);
    double t1 = now_s();
    if (getenv("MIND_TOP")) {            // отладка: топ-5 логитов после prefill
        int top[5] = { -1, -1, -1, -1, -1 };
        for (int k = 0; k < 5; k++)
            for (int i = 0; i < g_m.n_vocab; i++) {
                int used = 0;
                for (int q = 0; q < k; q++) if (top[q] == i) used = 1;
                if (!used && (top[k] < 0 || lg[i] > lg[top[k]])) top[k] = i;
            }
        printf("top:");
        for (int k = 0; k < 5; k++) printf(" %d=%.5f", top[k], lg[top[k]]);
        printf("\n");
    }
    static int32_t out[4096];
    int no = generate(&tok, lg, &pos, n_new, out);
    double t2 = now_s();
    print_ids("ids", out, no);
    print_text("text", &tok, out, no);
    printf("stats: prefill %d tok %.3fs (%.1f tok/s), gen %d tok %.3fs (%.1f tok/s), ssse3=%d threads=%d\n",
           n, t1 - t0, n / (t1 - t0 + 1e-9), no, t2 - t1, no / (t2 - t1 + 1e-9), mind_math_ssse3(), g_threads);

    // Ответ инструмента и продолжение (как агентный цикл в ядре).
    if (resp && no > 0 && out[no - 1] == tok.tool_call_close) {
        static char rbuf[8192];
        FILE* rf = fopen(resp, "rb");
        if (!rf) { perror("tool-response"); return 1; }
        int rn = (int)fread(rbuf, 1, sizeof rbuf - 1, rf);
        fclose(rf);
        while (rn > 0 && rbuf[rn - 1] == '\n') rn--;
        rbuf[rn] = 0;
        mind_prompt_t p;
        static int32_t ids2[8192];
        mind_prompt_begin(&p, &tok, ids2, 8192, tbuf, sizeof tbuf);
        mind_chat_close(&p, out[no - 1]);
        mind_chat_tool_response(&p, rbuf, rn);
        mind_chat_assistant(&p, style, think, 0);
        int n2 = mind_prompt_end(&p);
        if (n2 < 0) { printf("prompt2 too long\n"); return 1; }
        // Последний токен (</tool_call>) не прогнан через модель — он первый в ids2.
        print_ids("prompt2", ids2, n2);
        float* lg2 = prefill(ids2, n2, &pos);
        static int32_t out2[4096];
        int no2 = generate(&tok, lg2, &pos, n_new, out2);
        print_ids("ids2", out2, no2);
        print_text("text2", &tok, out2, no2);
    }
    return 0;
}

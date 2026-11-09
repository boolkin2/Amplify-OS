// ============================================================================
// phantom.c — стенд гипервизора phantom (подэтап 50.1). См. phantom.h.
// ----------------------------------------------------------------------------
// Модель устройств здесь умещается в пять портов отладки; гость один —
// phantom_guest.asm, его поведение задаётся номером режима в блоке параметров.
// Сценарии сверяют то, что доложил гость, с тем, что знает хост.
//
// Правило слоя: обработчик выходов pt_exit исполняется НА AP гостя. Он только
// пишет в заранее выделенную структуру; всё остальное (создание ВМ, ожидание,
// печать, сверка) делает BSP.
// ============================================================================
#include "phantom.h"
#include "hv.h"
#include "phantom_pc.h"
#include "phantom_linux.h"
#include "phantom_bios.h"
#include "fat32.h"
#include "iso9660.h"
#include "rtc.h"
#include "vmx.h"
#include "smp_pool.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "serial.h"
#include <stddef.h>

extern const uint8_t phantom_guest_start[];
extern const uint8_t phantom_guest_end[];

// Числа ниже — зеркало phantom_guest.asm.
#define GUEST_RAM    (4ull << 20)
#define GLOAD        0x1000
#define PORT_NULL    0xE7
#define PORT_RES     0xE8
#define PORT_CHAR    0xE9
#define PORT_XMM     0xEA
#define PORT_IN      0xEB
#define IN_MAGIC     0xC0FFEE42u
#define PARAM        0x500
#define PARG         0x508
#define COUNTER      0x600
#define SUM_BASE     0x20000
#define SUM_BYTES    0x10000
enum { MODE_HALT = 0, MODE_EPT, MODE_SPIN, MODE_IO, MODE_TRIPLE, MODE_REAL };

typedef struct {
    char              out[64];
    volatile int      out_n;
    uint32_t          res[4];
    volatile int      res_n;
    volatile int      vmcalls;
    volatile int      end_reason;       // последний выход, дошедший до модели
    uint64_t          ept_gpa, ept_qual;
    volatile uint64_t null_io;          // обращений к PORT_NULL
    uint64_t          io_t0, io_t1;     // TSC первого и последнего из них
} pt_ctx_t;

typedef struct {
    hv_vm_t* vm;
    pt_ctx_t ctx;
    uint64_t expect_sum;
} pt_run_t;

static pt_run_t g_run;                  // сценарии, которых BSP ждёт до конца
static pt_run_t g_spin;                 // фоновый гость (`phantom spin`)

// Сеанс настоящего гостя (`phantom linux`). Один, как и ядро под гостя.
// [50.5] У сеанса три состояния: образ читается в память (LX_LOADING, ВМ ещё
// нет), гость работает (vm != 0) и гость кончил сам (vm != 0, reported) —
// в последнем его экран и счётчики ещё доступны, пока не скажут «стоп».
enum { LX_NONE = 0, LX_LOADING, LX_GUEST };

static struct {
    int      phase;                     // LX_*
    hv_vm_t* vm;
    pc_t*    pc;
    bios_t*  bios;                      // 0 — стенд без BIOS (`phantom linux`)
    uint64_t iso_hpa, iso_pages;        // образ ISO в памяти хоста
    bool     reported;                  // о конце гостя уже сказано
    volatile bool keys_on;              // клавиши окна идут гостю (читает обработчик прерывания)
    char     line[240];                 // недописанная строка консоли гостя
    int      n;
    int      esc;                       // разбор управляющей последовательности терминала
    uint64_t last_ms;                   // когда в неё пришёл последний символ
    uint64_t mark_tsc, mark_guest;      // для доли времени ядра между двумя status

    // [50.5] Чтение образа по частям и сведения для окна.
    char     path[160];                 // откуда образ
    char     name[64];                  // имя файла
    char     pretype[200];              // строка для загрузчика
    bool     from_cd;                   // образ — файл установочного носителя
    iso_entry_t cd;
    uint64_t iso_bytes, loaded;
    uint64_t ram;                       // памяти гостю
    uint32_t chunk;                     // размер очередной порции чтения
    uint64_t load_ms0;                  // когда начали читать
    uint64_t start_ms;                  // когда гость стартовал
    uint64_t end_ms;                    // … и когда кончил
    uint64_t cpu_ms, cpu_tsc, cpu_guest;    // замер доли ядра раз в секунду
    uint32_t cpu_pct;
    char     end_text[120];             // чем гость кончил
} g_lx;

static char g_lx_error[120];            // почему не состоялся последний запуск (переживает сеанс)

// ----------------------------------------------------------------------------
// Мелочи.
// ----------------------------------------------------------------------------
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static bool str_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int popcount32(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

// Занятое ожидание на BSP: гость и пул при этом работают на других ядрах.
static void wait_ms(uint32_t ms) {
    uint64_t end = timer_get_uptime_ms() + ms;
    while (timer_get_uptime_ms() < end) __asm__ __volatile__("pause" ::: "memory");
}

// Строка отчёта собирается в буфер и уходит целиком: в serial и (короче) в окно.
static char g_ln[320];
static int  g_ln_n;
static void ln_reset(void) { g_ln_n = 0; g_ln[0] = 0; }
static void ln_s(const char* s) {
    while (*s && g_ln_n < (int)sizeof(g_ln) - 1) g_ln[g_ln_n++] = *s++;
    g_ln[g_ln_n] = 0;
}
static void ln_u(uint64_t v) {
    char b[24]; int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n && g_ln_n < (int)sizeof(g_ln) - 1) g_ln[g_ln_n++] = b[--n];
    g_ln[g_ln_n] = 0;
}
static void ln_x(uint64_t v) {
    ln_s("0x");
    bool lead = true;
    for (int i = 15; i >= 0; i--) {
        int d = (int)((v >> (i * 4)) & 15);
        if (lead && !d && i) continue;
        lead = false;
        char c[2] = { "0123456789abcdef"[d], 0 };
        ln_s(c);
    }
}

static bool report(phantom_sink_t out, const char* prefix, const char* name, bool ok) {
    serial_puts(prefix);
    serial_puts(name);
    serial_puts(ok ? ": PASS (" : ": FAIL (");
    serial_puts(g_ln);
    serial_puts(")\n");
    if (out) {
        out("  "); out(name); out(ok ? ": PASS\n" : ": FAIL\n");
    }
    return ok;
}

// Что известно о сбое: чем кончил гость и где он был.
static void ln_diag(hv_vm_t* vm) {
    const hv_exit_t* e = hv_last_exit(vm);
    const hv_diag_t* d = hv_diag(vm);
    ln_s(" | end="); ln_u((uint64_t)e->reason);
    ln_s(" hw="); ln_x(e->hw_reason);
    ln_s(" insn_err="); ln_u(e->insn_error);
    ln_s(" qual="); ln_x(e->qual);
    ln_s(" rip="); ln_x(d->rip);
    ln_s(" cs="); ln_x(d->cs_sel);
    ln_s(" cr0="); ln_x(d->cr0);
    ln_s(" cr4="); ln_x(d->cr4);
    ln_s(" efer="); ln_x(d->efer);
    if (hv_vcpu_error(vm)[0]) { ln_s(" err: "); ln_s(hv_vcpu_error(vm)); }
}

// ----------------------------------------------------------------------------
// Модель устройств стенда (AP гостя).
// ----------------------------------------------------------------------------
static void clobber_xmm(void) {
    __asm__ __volatile__(
        "pcmpeqd %%xmm0, %%xmm0\n pcmpeqd %%xmm1, %%xmm1\n pcmpeqd %%xmm2, %%xmm2\n pcmpeqd %%xmm3, %%xmm3\n"
        "pcmpeqd %%xmm4, %%xmm4\n pcmpeqd %%xmm5, %%xmm5\n pcmpeqd %%xmm6, %%xmm6\n pcmpeqd %%xmm7, %%xmm7\n"
        "pcmpeqd %%xmm8, %%xmm8\n pcmpeqd %%xmm9, %%xmm9\n pcmpeqd %%xmm10, %%xmm10\n pcmpeqd %%xmm11, %%xmm11\n"
        "pcmpeqd %%xmm12, %%xmm12\n pcmpeqd %%xmm13, %%xmm13\n pcmpeqd %%xmm14, %%xmm14\n pcmpeqd %%xmm15, %%xmm15\n"
        : : : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
              "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15");
}

static int pt_exit(hv_vm_t* vm, hv_exit_t* ex, void* arg) {
    (void)vm;
    pt_ctx_t* t = (pt_ctx_t*)arg;
    t->end_reason = ex->reason;
    switch (ex->reason) {
    case HV_EXIT_IO:
        switch (ex->port) {
        case PORT_CHAR:
            if (!ex->in && t->out_n < (int)sizeof(t->out) - 1) t->out[t->out_n++] = (char)ex->value;
            break;
        case PORT_RES:
            if (!ex->in && t->res_n < 4) t->res[t->res_n++] = ex->value;
            break;
        case PORT_XMM:
            clobber_xmm();
            break;
        case PORT_IN:
            if (ex->in) ex->value = IN_MAGIC;
            break;
        case PORT_NULL: {
            uint64_t now = rdtsc();
            if (!t->null_io) t->io_t0 = now;
            t->io_t1 = now;
            t->null_io++;
            break;
        }
        default: break;
        }
        return HV_RESUME;
    case HV_EXIT_VMCALL:
        t->vmcalls++;
        return HV_RESUME;
    case HV_EXIT_EPT:
        t->ept_gpa = ex->gpa;
        t->ept_qual = ex->qual;
        return HV_STOP;
    default:                            // HLT, тройной сбой, отказ — гость закончил
        return HV_STOP;
    }
}

// ----------------------------------------------------------------------------
// Подготовка и запуск гостя (BSP).
// ----------------------------------------------------------------------------
static bool pt_prepare(pt_run_t* r, uint32_t mode, uint64_t arg, uint32_t flags) {
    uint8_t* z = (uint8_t*)r;
    for (size_t i = 0; i < sizeof(*r); i++) z[i] = 0;
    r->vm = hv_vm_create(GUEST_RAM, flags);
    if (!r->vm) return false;

    uint64_t len = (uint64_t)(phantom_guest_end - phantom_guest_start);
    uint8_t* dst = (uint8_t*)hv_gpa(r->vm, GLOAD, len);
    for (uint64_t i = 0; i < len; i++) dst[i] = phantom_guest_start[i];
    *(uint32_t*)hv_gpa(r->vm, PARAM, 4) = mode;
    *(uint64_t*)hv_gpa(r->vm, PARG, 8) = arg;

    // Данные для суммы: псевдослучайные, свои для каждого режима. Гость
    // считает ту же сумму той же формулой (сложение и циклический сдвиг).
    uint64_t* d = (uint64_t*)hv_gpa(r->vm, SUM_BASE, SUM_BYTES);
    uint64_t x = 0x9E3779B97F4A7C15ull ^ (uint64_t)(mode + 1), acc = 0;
    for (int i = 0; i < SUM_BYTES / 8; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        d[i] = x;
        acc += x;
        acc = (acc << 1) | (acc >> 63);
    }
    r->expect_sum = acc;
    return true;
}

static bool pt_start(pt_run_t* r) {
    return hv_vcpu_start_real(r->vm, 0, GLOAD, pt_exit, &r->ctx);
}

static void pt_done(pt_run_t* r) {
    if (r->vm) hv_vm_destroy(r->vm);
    r->vm = 0;
}

static uint64_t tsc_to_us(uint64_t tsc) {
    uint64_t per_ms = hv_tsc_per_ms();
    return per_ms ? tsc * 1000 / per_ms : 0;
}

// ----------------------------------------------------------------------------
// Сценарии.
// ----------------------------------------------------------------------------
// VMX включается на всех ядрах с первой ВМ и выключается с последней.
static bool t_cores(phantom_sink_t out, const char* prefix) {
    ln_reset();
    hv_vm_t* vm = hv_vm_create(GUEST_RAM, 0);
    uint32_t on = vmx_on_mask();
    if (vm) hv_vm_destroy(vm);
    uint32_t after = vmx_on_mask();
    int want = vmx_caps()->cpus_seen;
    bool ok = vm && popcount32(on) == want && after == 0;
    ln_s("VMX on "); ln_u((uint64_t)popcount32(on)); ln_s("/"); ln_u((uint64_t)want);
    ln_s(" cores, mask after the last VM="); ln_x(after);
    if (!vm) { ln_s(", "); ln_s(hv_last_error()); }
    return report(out, prefix, "cores", ok);
}

// Реальный -> защищённый -> длинный режим и все проверки гостя.
static bool t_modes(phantom_sink_t out, const char* prefix, const char* name, uint32_t flags) {
    ln_reset();
    if (!pt_prepare(&g_run, MODE_HALT, 0, flags)) { ln_s(hv_last_error()); return report(out, prefix, name, false); }
    uint64_t t0 = rdtsc();
    bool fin = pt_start(&g_run) && hv_vcpu_wait(g_run.vm, 2000);
    uint64_t us = tsc_to_us(rdtsc() - t0);
    pt_ctx_t* c = &g_run.ctx;
    c->out[c->out_n] = 0;
    uint64_t sum = ((uint64_t)c->res[1] << 32) | c->res[0];
    bool ok = fin && str_eq(c->out, "RPLIFV") && c->res_n == 2 && sum == g_run.expect_sum &&
              c->vmcalls == 1 && c->end_reason == HV_EXIT_HLT && hv_guard_intact(g_run.vm);
    const hv_stats_t* st = hv_stats(g_run.vm);
    ln_s("guest says \""); ln_s(c->out); ln_s("\", memory sum ");
    ln_s(c->res_n == 2 && sum == g_run.expect_sum ? "matches" : "DIFFERS");
    ln_s(", exits="); ln_u(st->exits);
    ln_s(" (cr="); ln_u(st->cr); ln_s(" msr="); ln_u(st->msr); ln_s(" cpuid="); ln_u(st->cpuid);
    ln_s(" io="); ln_u(st->io); ln_s("), "); ln_u(us); ln_s(" us");
    if (!ok) ln_diag(g_run.vm);
    pt_done(&g_run);
    return report(out, prefix, name, ok);
}

// Гость пишет по адресу сразу за своей памятью: выход EPT, память хоста цела.
static bool t_ept(phantom_sink_t out) {
    ln_reset();
    if (!pt_prepare(&g_run, MODE_EPT, GUEST_RAM, 0)) { ln_s(hv_last_error()); return report(out, "[phantom] test ", "isolation", false); }
    bool fin = pt_start(&g_run) && hv_vcpu_wait(g_run.vm, 2000);
    pt_ctx_t* c = &g_run.ctx;
    c->out[c->out_n] = 0;
    bool ok = fin && c->end_reason == HV_EXIT_EPT && c->ept_gpa == GUEST_RAM &&
              (c->ept_qual & 2) && str_eq(c->out, "RPLIFV") && hv_guard_intact(g_run.vm);
    ln_s("write past guest RAM -> EPT violation at "); ln_x(c->ept_gpa);
    ln_s(" qual="); ln_x(c->ept_qual);
    ln_s(", host page behind guest RAM "); ln_s(hv_guard_intact(g_run.vm) ? "intact" : "DAMAGED");
    if (!ok) ln_diag(g_run.vm);
    pt_done(&g_run);
    return report(out, "[phantom] test ", "isolation", ok);
}

// Тройной сбой гостя — это остановка гостя, а не хоста.
static bool t_triple(phantom_sink_t out) {
    ln_reset();
    if (!pt_prepare(&g_run, MODE_TRIPLE, 0, 0)) { ln_s(hv_last_error()); return report(out, "[phantom] test ", "triple-fault", false); }
    bool fin = pt_start(&g_run) && hv_vcpu_wait(g_run.vm, 2000);
    bool ok = fin && g_run.ctx.end_reason == HV_EXIT_SHUTDOWN;
    ln_s("guest triple fault reported as shutdown");
    if (!ok) ln_diag(g_run.vm);
    pt_done(&g_run);
    return report(out, "[phantom] test ", "triple-fault", ok);
}

// Зациклившийся гость: его режет таймер вытеснения, а BSP может его снять.
static bool t_spin(phantom_sink_t out) {
    ln_reset();
    if (!pt_prepare(&g_run, MODE_SPIN, 0, 0)) { ln_s(hv_last_error()); return report(out, "[phantom] test ", "preempt", false); }
    volatile uint64_t* cnt = (volatile uint64_t*)hv_gpa(g_run.vm, COUNTER, 8);
    bool started = pt_start(&g_run);
    uint64_t end = timer_get_uptime_ms() + 1000;
    while (started && !*cnt && timer_get_uptime_ms() < end) __asm__ __volatile__("pause");
    uint64_t c1 = *cnt;
    uint64_t s1 = hv_stats(g_run.vm)->slices;
    wait_ms(50);
    uint64_t c2 = *cnt;
    uint64_t slices = hv_stats(g_run.vm)->slices - s1, pt = hv_stats(g_run.vm)->ptimer;
    uint64_t t0 = rdtsc();
    bool stopped = hv_vcpu_stop(g_run.vm);
    uint64_t stop_us = tsc_to_us(rdtsc() - t0);
    uint64_t c3 = *cnt;
    wait_ms(5);
    uint64_t c4 = *cnt;
    // 50 мс при кванте 2 мс — около 25 квантов; порог с запасом на чужую нагрузку узла.
    bool ok = started && c1 > 0 && c2 > c1 && slices >= 8 && pt >= 8 && stopped &&
              stop_us < 50000 && c4 == c3 && hv_vcpu_state(g_run.vm) == HV_VCPU_STOPPED;
    ln_s("endless guest loop: +"); ln_u(c2 - c1); ln_s(" iterations in 50 ms, ");
    ln_u(slices); ln_s(" time slices, "); ln_u(pt); ln_s(" preemption-timer exits, stopped in ");
    ln_u(stop_us); ln_s(" us, counter frozen after stop: "); ln_s(c4 == c3 ? "yes" : "NO");
    if (!ok) ln_diag(g_run.vm);
    pt_done(&g_run);
    return report(out, "[phantom] test ", "preempt", ok);
}

// Цена выхода: N команд OUT подряд, время между первой и последней.
static bool t_io(phantom_sink_t out, const char* name) {
    const uint64_t N = 20000;
    ln_reset();
    if (!pt_prepare(&g_run, MODE_IO, N, 0)) { ln_s(hv_last_error()); return report(out, "[phantom] test ", name, false); }
    bool fin = pt_start(&g_run) && hv_vcpu_wait(g_run.vm, 20000);
    pt_ctx_t* c = &g_run.ctx;
    c->out[c->out_n] = 0;
    uint64_t per_ms = hv_tsc_per_ms();
    uint64_t ns = (c->null_io > 1 && per_ms) ? (c->io_t1 - c->io_t0) * 1000000ull / per_ms / (c->null_io - 1) : 0;
    bool ok = fin && c->null_io == N && str_eq(c->out, "RPLIFVB") && ns > 0;
    ln_u(c->null_io); ln_s(" OUT exits, round trip "); ln_u(ns); ln_s(" ns each (");
    ln_u(ns ? 1000000000ull / ns : 0); ln_s(" exits/s), TSC "); ln_u(per_ms / 1000); ln_s(" MHz");
    if (!ok) ln_diag(g_run.vm);
    pt_done(&g_run);
    return report(out, "[phantom] test ", name, ok);
}

// Сто раз создать ВМ, запустить гостя, снять: память возвращается вся.
static bool t_cycles(phantom_sink_t out) {
    const int N = 100;
    ln_reset();
    uint64_t ram0 = pmm_free_ram();
    uint64_t heap0 = heap_get_stats().used_size;
    int good = 0;
    uint64_t t0 = rdtsc();
    for (int i = 0; i < N; i++) {
        if (!pt_prepare(&g_run, MODE_REAL, 0, 0)) break;
        bool fin = pt_start(&g_run) && hv_vcpu_wait(g_run.vm, 2000);
        g_run.ctx.out[g_run.ctx.out_n] = 0;
        if (fin && str_eq(g_run.ctx.out, "R") && g_run.ctx.end_reason == HV_EXIT_HLT) good++;
        pt_done(&g_run);
    }
    uint64_t us = tsc_to_us(rdtsc() - t0);
    uint64_t ram1 = pmm_free_ram();
    uint64_t heap1 = heap_get_stats().used_size;
    bool ok = good == N && ram1 == ram0 && heap1 == heap0 && vmx_on_mask() == 0;
    ln_u((uint64_t)good); ln_s("/"); ln_u((uint64_t)N); ln_s(" create-run-destroy cycles, ");
    ln_u(us / (uint64_t)N); ln_s(" us each, free RAM ");
    ln_s(ram1 == ram0 ? "unchanged" : "CHANGED"); ln_s(", kernel heap ");
    ln_s(heap1 == heap0 ? "unchanged" : "CHANGED");
    if (ram1 != ram0) { ln_s(" (delta "); ln_u(ram0 > ram1 ? ram0 - ram1 : ram1 - ram0); ln_s(" bytes)"); }
    if (good != N) { ln_s(", "); ln_s(hv_last_error()); }
    return report(out, "[phantom] test ", "cycles", ok);
}

// ---- гость и пул на одном ядре -------------------------------------------------
#define PJ_N 1024
static uint32_t g_pj_out[PJ_N];
static volatile uint32_t g_pj_slots;

static uint32_t pj_value(int i, uint32_t salt) {
    // Около микросекунды работы на элемент: иначе вызывающее ядро успевает
    // посчитать всё само раньше, чем остальные заметят задачу.
    uint32_t x = (uint32_t)i * 2654435761u + salt;
    for (int k = 0; k < 300; k++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; }
    return x;
}

static void pj_range(void* ctx, int b, int e) {
    uint32_t salt = *(uint32_t*)ctx;
    for (int i = b; i < e; i++) g_pj_out[i] = pj_value(i, salt);
    __atomic_fetch_or(&g_pj_slots, 1u << smp_pool_current_slot(), __ATOMIC_RELAXED);
}

static bool pj_once(uint32_t salt) {
    smp_pool_parallel_for(pj_range, &salt, PJ_N, 16);
    // Сверяем выборочно: полная сверка стоила бы столько же, сколько задача.
    for (int i = 0; i < PJ_N; i += 37) if (g_pj_out[i] != pj_value(i, salt)) return false;
    return g_pj_out[PJ_N - 1] == pj_value(PJ_N - 1, salt);
}

static bool t_pool(phantom_sink_t out) {
    ln_reset();
    if (!pt_prepare(&g_run, MODE_SPIN, 0, 0)) { ln_s(hv_last_error()); return report(out, "[phantom] test ", "pool-share", false); }
    volatile uint64_t* cnt = (volatile uint64_t*)hv_gpa(g_run.vm, COUNTER, 8);
    int slot = hv_vcpu_slot(g_run.vm);
    bool started = pt_start(&g_run);
    uint64_t end = timer_get_uptime_ms() + 1000;
    while (started && !*cnt && timer_get_uptime_ms() < end) __asm__ __volatile__("pause");

    // 1) Обычный режим: гость считает, пул получает редкие задачи. Ядро гостя
    //    в них не участвует, а задачи не ждут его и остаются верными.
    bool good = started;
    uint64_t c0 = *cnt;
    int cold_jobs = 0;
    uint64_t until = timer_get_uptime_ms() + 40;
    while (timer_get_uptime_ms() < until) { good = pj_once((uint32_t)cold_jobs + 1) && good; cold_jobs++; }
    uint64_t cold_gain = *cnt - c0;

    // 2) Горячий режим (так пул работает, пока MIND генерирует): ядро гостя
    //    обязано появляться среди исполнителей, а гость — не останавливаться.
    uint64_t s0 = hv_stats(g_run.vm)->slices;
    uint64_t c1 = *cnt;
    int hot_jobs = 0;
    g_pj_slots = 0;
    smp_pool_hot_begin();
    until = timer_get_uptime_ms() + 80;
    while (timer_get_uptime_ms() < until) { good = pj_once(0x1000u + (uint32_t)hot_jobs) && good; hot_jobs++; }
    smp_pool_hot_end();
    uint32_t hot_slots = g_pj_slots;
    uint64_t hot_gain = *cnt - c1;
    uint64_t hot_slices = hv_stats(g_run.vm)->slices - s0;

    bool stopped = hv_vcpu_stop(g_run.vm);
    // 80 мс: без дележа гость взял бы ~40 квантов по 2 мс, при 2 из каждых 10 мс — ~8.
    bool ok = good && stopped && cold_gain > 0 && hot_gain > 0 &&
              (hot_slots & (1u << slot)) && hot_slices >= 2 && hot_slices <= 24;
    ln_s("guest on core "); ln_u((uint64_t)slot); ln_s("; pool results ");
    ln_s(good ? "correct" : "WRONG"); ln_s(" ("); ln_u((uint64_t)cold_jobs); ln_s(" jobs idle, ");
    ln_u((uint64_t)hot_jobs); ln_s(" jobs hot); hot mode: pool ran on cores mask ");
    ln_x(hot_slots); ln_s(", guest got "); ln_u(hot_slices); ln_s(" slices in 80 ms and advanced by ");
    ln_u(hot_gain); ln_s("; idle mode: guest advanced by "); ln_u(cold_gain);
    if (!ok) ln_diag(g_run.vm);
    pt_done(&g_run);
    return report(out, "[phantom] test ", "pool-share", ok);
}

// ----------------------------------------------------------------------------
// Точки входа.
// ----------------------------------------------------------------------------
static const char* unavailable(void) {
    if (!vmx_ready()) return "no usable VT-x on this machine";
    if (!smp_pool_pick_resident()) return "phantom needs a second CPU core for the guest";
    return 0;
}

bool phantom_boot_selftest(void) {
    const char* why = unavailable();
    if (why) {
        serial_puts("[smoke 50.1] skipped: ");
        serial_puts(why);
        serial_puts("\n");
        return true;
    }
    bool ok = t_cores(0, "[smoke 50.1] ");
    ok = t_modes(0, "[smoke 50.1] ", "guest real->protected->long", 0) && ok;
    return ok;
}

int phantom_selftest(phantom_sink_t out) {
    const char* why = unavailable();
    if (!why && (g_spin.vm || g_lx.phase)) why = "stop the background guest first (phantom stop)";
    if (why) {
        serial_puts("[phantom] selftest: skipped ("); serial_puts(why); serial_puts(")\n");
        if (out) { out("  skipped: "); out(why); out("\n"); }
        return 0;
    }
    int pass = 0, fail = 0;
#define RUN(x) do { if (x) pass++; else fail++; } while (0)
    RUN(t_cores(out, "[phantom] test "));
    RUN(t_modes(out, "[phantom] test ", "modes", 0));
    RUN(t_modes(out, "[phantom] test ", "modes-4k-ept", HV_VM_SMALL_PAGES));
    RUN(t_ept(out));
    RUN(t_triple(out));
    RUN(t_spin(out));
    RUN(t_io(out, "exit-cost"));
    RUN(t_cycles(out));
    RUN(t_pool(out));
#undef RUN
    serial_puts("[phantom] selftest: ");
    serial_print_dec((uint64_t)pass);
    serial_puts(" passed, ");
    serial_print_dec((uint64_t)fail);
    serial_puts(" failed\n");
    return fail;
}

void phantom_bench(phantom_sink_t out) {
    const char* why = unavailable();
    if (!why && (g_spin.vm || g_lx.phase)) why = "stop the background guest first (phantom stop)";
    if (why) { if (out) { out("  "); out(why); out("\n"); } return; }
    t_io(out, "exit-cost");
    if (out) { out("  "); out(g_ln); out("\n"); }
}

bool phantom_spin_active(void) { return g_spin.vm != 0; }

bool phantom_spin_start(phantom_sink_t out) {
    const char* why = unavailable();
    if (!why && (g_spin.vm || g_lx.phase)) why = "a background guest is already running";
    if (!why && !pt_prepare(&g_spin, MODE_SPIN, 0, 0)) why = hv_last_error();
    if (!why && !pt_start(&g_spin)) { why = hv_last_error(); pt_done(&g_spin); }
    if (why) {
        serial_puts("[phantom] spin: not started ("); serial_puts(why); serial_puts(")\n");
        if (out) { out("  not started: "); out(why); out("\n"); }
        return false;
    }
    serial_puts("[phantom] spin started on core ");
    serial_print_dec((uint64_t)hv_vcpu_slot(g_spin.vm));
    serial_puts("\n");
    if (out) out("  guest started: it counts in an endless loop (phantom status, phantom stop)\n");
    return true;
}

static void lx_status(phantom_sink_t out);
static void lx_stop(phantom_sink_t out);

void phantom_spin_status(phantom_sink_t out) {
    if (g_lx.phase) { lx_status(out); return; }
    if (!g_spin.vm) {
        serial_puts("[phantom] spin: none\n");
        if (out) out("  no background guest\n");
        return;
    }
    const hv_stats_t* st = hv_stats(g_spin.vm);
    ln_reset();
    ln_s("counter="); ln_u(*(volatile uint64_t*)hv_gpa(g_spin.vm, COUNTER, 8));
    ln_s(" slices="); ln_u(st->slices);
    ln_s(" exits="); ln_u(st->exits);
    ln_s(" state="); ln_s(hv_vcpu_state(g_spin.vm) == HV_VCPU_STOPPED ? "stopped" : "running");
    serial_puts("[phantom] spin "); serial_puts(g_ln); serial_puts("\n");
    if (out) { out("  "); out(g_ln); out("\n"); }
}

void phantom_spin_stop(phantom_sink_t out) {
    if (g_lx.phase) { lx_stop(out); return; }
    if (!g_spin.vm) {
        serial_puts("[phantom] spin: none\n");
        if (out) out("  no background guest\n");
        return;
    }
    uint64_t t0 = rdtsc();
    bool stopped = hv_vcpu_stop(g_spin.vm);
    uint64_t us = tsc_to_us(rdtsc() - t0);
    ln_reset();
    ln_s(stopped ? "stopped in " : "DID NOT STOP in "); ln_u(us); ln_s(" us, counter=");
    ln_u(*(volatile uint64_t*)hv_gpa(g_spin.vm, COUNTER, 8));
    ln_s(" slices="); ln_u(hv_stats(g_spin.vm)->slices);
    serial_puts("[phantom] spin "); serial_puts(g_ln); serial_puts("\n");
    if (out) { out("  "); out(g_ln); out("\n"); }
    pt_done(&g_spin);
}

// ----------------------------------------------------------------------------
// [50.2] Настоящий гость: ядро Linux на устройствах phantom_pc.
// ----------------------------------------------------------------------------
#define LX_RAM      (256ull << 20)
#define LX_KERNEL   "/disk/vault/phantom/vmlinuz64"
#define LX_INITRD   "/disk/vault/phantom/corepure64.gz"
// earlyprintk — чтобы сообщения шли в порт раньше, чем ядро заведёт консоль:
// если гость встанет в самом начале, будет видно где.
#define LX_CMDLINE  "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 loglevel=7"

static void lx_copy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static void lx_set_name(const char* s) { lx_copy(g_lx.name, s, (int)sizeof(g_lx.name)); }

static void lx_flush(void) {
    if (!g_lx.n) return;
    g_lx.line[g_lx.n] = 0;
    serial_puts("[guest] ");
    serial_puts(g_lx.line);
    serial_puts("\n");
    g_lx.n = 0;
}

static const char* lx_state_name(void) {
    int s = hv_vcpu_state(g_lx.vm);
    return s == HV_VCPU_STOPPED ? "stopped" : s == HV_VCPU_HALTED ? "sleeping" : "running";
}

// Счётчики одной строкой — и в serial (для теста), и в окно.
static void lx_counters(void) {
    const hv_stats_t* st = hv_stats(g_lx.vm);
    const pc_stats_t* ps = pc_stats(g_lx.pc);
    ln_s(" exits="); ln_u(st->exits);
    ln_s(" io="); ln_u(st->io);
    ln_s(" msr="); ln_u(st->msr);
    ln_s(" cpuid="); ln_u(st->cpuid);
    ln_s(" irqs="); ln_u(st->injected);
    ln_s(" windows="); ln_u(st->windows);
    ln_s(" sleeps="); ln_u(st->sleeps);
    ln_s(" maxsleep_ms="); ln_u(hv_tsc_per_ms() ? st->sleep_max_tsc / hv_tsc_per_ms() : 0);
    if (st->sleeps_open) { ln_s(" open_sleeps="); ln_u(st->sleeps_open); }
    ln_s(" timer="); ln_u(ps->irq0);
    ln_s(" tx="); ln_u(ps->tx);
    ln_s(" rx="); ln_u(ps->rx);
    if (ps->tx_lost) { ln_s(" tx_lost="); ln_u(ps->tx_lost); }
    if (st->msr_unknown) {
        ln_s(" unknown_msr="); ln_u(st->msr_unknown); ln_s(" (");
        for (int i = 0; i < st->unknown_n; i++) { if (i) ln_s(","); ln_x(st->unknown_list[i]); }
        ln_s(")");
    }
    if (ps->unknown_io) { ln_s(" empty_ports="); ln_u(ps->unknown_io); ln_s(" last="); ln_x(ps->last_unknown_port); }
    if (ps->blk_reqs) {
        ln_s(" disk_reqs="); ln_u(ps->blk_reqs);
        ln_s(" disk_mb="); ln_u(ps->blk_sectors / 2048);
        if (ps->blk_errors) { ln_s(" disk_errors="); ln_u(ps->blk_errors); }
    }
    if (g_lx.bios) {
        const bios_stats_t* bs = bios_stats(g_lx.bios);
        ln_s(" bios_calls="); ln_u(bs->calls);
        ln_s(" disk_reads="); ln_u(bs->disk_reads);
        ln_s(" sectors="); ln_u(bs->disk_sectors);
        ln_s(" keys="); ln_u(bs->keys);
        if (bs->unknown_n) {
            // Службы BIOS, которых у нас нет: «вектор:AH».
            ln_s(" bios_unknown=(");
            for (int i = 0; i < bs->unknown_n; i++) {
                if (i) ln_s(",");
                ln_x(bs->unknown[i] >> 8); ln_s(":"); ln_x(bs->unknown[i] & 0xFF);
            }
            ln_s(")");
        }
    }
}

// Чем гость кончил — одной строкой, плюс последние выходы для разбора.
static void lx_report_end(void) {
    const pc_stats_t* ps = pc_stats(g_lx.pc);
    const hv_exit_t* e = hv_last_exit(g_lx.vm);
    const bios_stats_t* bs = g_lx.bios ? bios_stats(g_lx.bios) : 0;
    ln_reset();
    if (bs && bs->end == BIOS_END_BOOT_FAIL)    ln_s("guest BIOS: nothing to boot from (INT 18h)");
    else if (bs && bs->end == BIOS_END_REBOOT)  ln_s("guest asked the BIOS to reboot (INT 19h)");
    else if (bs && bs->end == BIOS_END_EXCEPTION) {
        ln_s("guest CPU exception "); ln_x(bs->end_vec); ln_s(" in real mode at ");
        ln_x(bs->end_cs); ln_s(":"); ln_x(bs->end_ip);
    }
    else if (ps->reset)                    ln_s("guest asked for a reset");
    else if (e->reason == HV_EXIT_HLT)     ln_s("guest halted itself with interrupts off");
    else if (e->reason == HV_EXIT_SHUTDOWN) ln_s("guest triple fault");
    else if (e->reason == HV_EXIT_EPT)     { ln_s("guest touched memory outside its RAM at "); ln_x(e->gpa); }
    else if (e->reason == HV_EXIT_FAIL)    ln_s("hypervisor could not continue");
    else                                   ln_s("stopped on request");
    for (int i = 0; i < (int)sizeof(g_lx.end_text) - 1 && g_ln[i]; i++) { g_lx.end_text[i] = g_ln[i]; g_lx.end_text[i + 1] = 0; }
    lx_counters();
    if (!ps->reset && e->reason && e->reason != HV_EXIT_IO) ln_diag(g_lx.vm);
    serial_puts("[phantom] guest ended: "); serial_puts(g_ln); serial_puts("\n");

    if (e->reason == HV_EXIT_SHUTDOWN || e->reason == HV_EXIT_EPT || e->reason == HV_EXIT_FAIL) {
        int head;
        const hv_trace_t* t = hv_trace(g_lx.vm, &head);
        ln_reset();
        ln_s("last exits (reason:arg), oldest first:");
        for (int i = 0; i < HV_TRACE_N; i++) {
            const hv_trace_t* r = &t[(head + i) % HV_TRACE_N];
            if (!r->reason && !r->arg) continue;
            ln_s(" "); ln_u(r->reason & 0xFFFF); ln_s(":"); ln_x(r->arg);
        }
        serial_puts("[phantom] "); serial_puts(g_ln); serial_puts("\n");
    }
}

static void lx_load_step(void);

void phantom_tick(void) {
    if (g_lx.phase == LX_LOADING) { lx_load_step(); return; }
    if (!g_lx.vm) return;
    // Не больше 192 символов за проход: печать в serial хоста небесплатна, а
    // проход цикла WM должен оставаться коротким (бюджет кадра).
    char buf[192];
    int n = pc_console_read(g_lx.pc, buf, (int)sizeof(buf));
    uint64_t now = timer_get_uptime_ms();
    // [50.5] Доля ядра, занятая гостем, — раз в секунду, для окна.
    if (now - g_lx.cpu_ms >= 1000) {
        uint64_t t = rdtsc(), g = hv_stats(g_lx.vm)->guest_tsc;
        uint64_t dt = t - g_lx.cpu_tsc, dg = g - g_lx.cpu_guest;
        g_lx.cpu_pct = (g_lx.cpu_tsc && dt) ? (uint32_t)(dg * 100 / dt) : 0;
        if (g_lx.cpu_pct > 100) g_lx.cpu_pct = 100;
        g_lx.cpu_tsc = t; g_lx.cpu_guest = g; g_lx.cpu_ms = now;
    }
    for (int i = 0; i < n; i++) {
        char ch = buf[i];
        // Управляющие последовательности терминала (ESC [ … буква) — цвета,
        // режимы, движение курсора — в журнал не идут целиком, а не только
        // их первый байт: иначе в строках остаются огрызки вроде «[?7h».
        if (g_lx.esc == 1) { g_lx.esc = (ch == '[') ? 2 : 0; continue; }
        if (g_lx.esc == 2) { if (ch >= 0x40 && ch <= 0x7E) g_lx.esc = 0; continue; }
        if (ch == 0x1B) { g_lx.esc = 1; continue; }
        if (ch == '\n') { lx_flush(); continue; }
        if (ch == '\t') ch = ' ';
        if (ch < 0x20 || ch > 0x7E) continue;           // управляющие и не-ASCII — мимо
        g_lx.line[g_lx.n++] = ch;
        g_lx.last_ms = now;
        if (g_lx.n >= (int)sizeof(g_lx.line) - 1) lx_flush();
    }
    // Приглашение оболочки не кончается переводом строки: выводим «хвост»,
    // если гость замолчал.
    if (g_lx.n && now - g_lx.last_ms > 300) lx_flush();

    if (!g_lx.reported && hv_vcpu_state(g_lx.vm) == HV_VCPU_STOPPED && !n) {
        lx_flush();
        g_lx.reported = true;
        g_lx.keys_on = false;
        g_lx.end_ms = now;
        lx_report_end();
    }
}

bool phantom_linux_start(phantom_sink_t out, bool full_init) {
    const char* why = unavailable();
    if (!why && (g_spin.vm || g_lx.phase)) why = "a background guest is already running (phantom stop)";
    hv_vm_t* vm = 0;
    pc_t* pc = 0;
    hv_start_t st;
    plinux_info_t info;
    if (!why) {
        vm = hv_vm_create(LX_RAM, 0);
        if (!vm) why = hv_last_error();
    }
    if (!why) {
        rtc_time_t t;
        rtc_read(&t);
        uint64_t unix_now = pc_unix_time(t.year, t.month, t.day, t.hour, t.minute, t.second);
        hv_set_wallclock(vm, unix_now);
        pc = pc_create(vm, unix_now);
        if (!pc) why = "the PC platform is busy";
    }
    if (!why) {
        const char* err = "";
        const char* cmd = full_init ? LX_CMDLINE : LX_CMDLINE " rdinit=/bin/sh";
        if (!plinux_load(vm, LX_KERNEL, LX_INITRD, cmd, &st, &info, &err)) why = err;
    }
    if (!why && !hv_vcpu_start(vm, &st, pc_model(), pc)) why = hv_last_error();
    if (why) {
        if (pc) pc_destroy(pc);
        if (vm) hv_vm_destroy(vm);
        serial_puts("[phantom] linux: not started ("); serial_puts(why); serial_puts(")\n");
        if (out) { out("  not started: "); out(why); out("\n"); }
        return false;
    }
    uint8_t* z = (uint8_t*)&g_lx;
    for (size_t i = 0; i < sizeof(g_lx); i++) z[i] = 0;
    g_lx.phase = LX_GUEST;
    g_lx.vm = vm;
    g_lx.pc = pc;
    g_lx.mark_tsc = rdtsc();
    g_lx.start_ms = timer_get_uptime_ms();
    g_lx.ram = LX_RAM;
    lx_set_name("linux (test bench)");
    g_lx.keys_on = true;
    ln_reset();
    ln_s("kernel "); ln_u(info.kernel_bytes); ln_s(" bytes (boot protocol "); ln_x(info.protocol);
    ln_s("), initrd "); ln_u(info.initrd_bytes); ln_s(" bytes at "); ln_x(info.initrd_gpa);
    ln_s(", RAM "); ln_u(LX_RAM >> 20); ln_s(" MB, core "); ln_u((uint64_t)hv_vcpu_slot(vm));
    ln_s(full_init ? ", init" : ", shell");
    serial_puts("[phantom] linux started: "); serial_puts(g_ln); serial_puts("\n");
    if (out) { out("  guest started: "); out(g_ln); out("\n"); }
    return true;
}

void phantom_type(phantom_sink_t out, const char* text) {
    if (!g_lx.vm || g_lx.reported) {
        serial_puts("[phantom] type: no guest\n");
        if (out) out("  no guest to type to\n");
        return;
    }
    int n = 0;
    while (text[n]) n++;
    bool ok = (!n || pc_console_input(g_lx.pc, text, n)) && pc_console_input(g_lx.pc, "\r", 1);
    serial_puts(ok ? "[phantom] typed\n" : "[phantom] type: input buffer is full\n");
    if (out && !ok) out("  the guest is not reading its console\n");
}

static void lx_status(phantom_sink_t out) {
    if (g_lx.phase == LX_LOADING) {
        ln_reset();
        ln_s("state=loading loaded="); ln_u(g_lx.loaded); ln_s(" of "); ln_u(g_lx.iso_bytes); ln_s(" bytes");
        serial_puts("[phantom] linux "); serial_puts(g_ln); serial_puts("\n");
        if (out) { out("  "); out(g_ln); out("\n"); }
        return;
    }
    const hv_stats_t* st = hv_stats(g_lx.vm);
    uint64_t now = rdtsc();
    // Доля времени ядра, которую гость занял с прошлого вызова.
    uint64_t dt = now - g_lx.mark_tsc, dg = st->guest_tsc - g_lx.mark_guest;
    g_lx.mark_tsc = now;
    g_lx.mark_guest = st->guest_tsc;
    ln_reset();
    ln_s("state="); ln_s(lx_state_name());
    ln_s(" run="); ln_u(dt ? dg * 100 / dt : 0); ln_s("%");
    lx_counters();
    serial_puts("[phantom] linux "); serial_puts(g_ln); serial_puts("\n");
    if (out) { out("  "); out(g_ln); out("\n"); }
}

static void lx_free_image(void) {
    if (g_lx.iso_hpa) pmm_free_big(g_lx.iso_hpa, g_lx.iso_pages);
    g_lx.iso_hpa = 0;
}

static void lx_stop(phantom_sink_t out) {
    if (g_lx.phase == LX_LOADING) {
        // Гостя ещё нет — только память под образ.
        lx_free_image();
        g_lx.phase = LX_NONE;
        serial_puts("[phantom] loading cancelled\n");
        if (out) out("  loading cancelled\n");
        return;
    }
    // Обработчик прерывания клавиатуры шлёт клавиши гостю через g_lx.pc:
    // сначала закрываем ему дорогу (он на этом же ядре — после записи флага
    // уже не вклинится со старым значением), потом разбираем ВМ.
    g_lx.keys_on = false;
    uint64_t t0 = rdtsc();
    bool stopped = hv_vcpu_stop(g_lx.vm);
    uint64_t us = tsc_to_us(rdtsc() - t0);
    // Что гость успел напечатать — довыводим, пока ВМ ещё есть.
    for (int i = 0; i < 64; i++) { phantom_tick(); if (g_lx.reported) break; }
    ln_reset();
    ln_s(stopped ? "stopped in " : "DID NOT STOP in "); ln_u(us); ln_s(" us");
    serial_puts("[phantom] linux "); serial_puts(g_ln); serial_puts("\n");
    if (out) { out("  "); out(g_ln); out("\n"); }
    hv_vm_destroy(g_lx.vm);
    pc_destroy(g_lx.pc);
    if (g_lx.bios) bios_destroy(g_lx.bios);
    // Образ ISO — только после ВМ: службы BIOS читают его, пока гость жив.
    lx_free_image();
    g_lx.vm = 0;
    g_lx.pc = 0;
    g_lx.bios = 0;
    g_lx.phase = LX_NONE;
}

// ----------------------------------------------------------------------------
// [50.3] Гость с ISO через BIOS.
// ----------------------------------------------------------------------------
// [50.5] Запуск идёт в два приёма. phantom_run только проверяет, что запускать
// можно, выделяет память и возвращается; сам образ читается порциями из цикла
// WM (lx_load_step в phantom_tick), и когда он весь в памяти — lx_boot создаёт
// ВМ и запускает гостя. Раньше образ читался одним вызовом: 66 МБ с IDE-диска
// — это полминуты, в которые не рисовался ни один кадр и не работал ввод.
static void lx_refuse(phantom_sink_t out, const char* why) {
    lx_copy(g_lx_error, why, (int)sizeof(g_lx_error));
    serial_puts("[phantom] run refused ("); serial_puts(why); serial_puts(")\n");
    if (out) { out("  not started: "); out(why); out("\n"); }
}

static bool starts_with(const char* s, const char* prefix) {
    while (*prefix) if (*s++ != *prefix++) return false;
    return true;
}

static int64_t lx_read(uint64_t off, void* buf, uint64_t len) {
    return g_lx.from_cd ? iso_read(&g_lx.cd, off, buf, len)
                        : fat32_read_file_at(g_lx.path, off, buf, len, 0);
}

bool phantom_run(phantom_sink_t out, const char* iso_path, const char* pretype) {
    const char* why = unavailable();
    if (!why && (g_spin.vm || g_lx.phase)) why = "a guest is already running (phantom stop)";
    uint64_t iso_hpa = 0, iso_pages = 0, iso_bytes = 0;
    uint64_t ram = LX_RAM;
    bool from_cd = false;
    iso_entry_t cd = { 0, 0, false };
    int plen = 0;
    while (pretype && pretype[plen]) plen++;

    if (!why) {
        if (starts_with(iso_path, "/cdrom/")) {
            // Файл установочного носителя (/cdrom/guests/…): читаем прямо с CD.
            from_cd = true;
            if (!iso_mount_boot() || !iso_lookup(iso_path + 6, &cd) || cd.is_dir) why = "ISO file not found";
            else iso_bytes = cd.size;
        } else {
            uint8_t probe;
            if (fat32_read_file_at(iso_path, 0, &probe, 0, &iso_bytes) < 0) why = "ISO file not found";
        }
    }
    if (!why) {
        if (iso_bytes < (64u << 10)) why = "file is too small to be an ISO image";
        else if (plen >= (int)sizeof(g_lx.pretype)) why = "the boot line is too long";
        else if (iso_bytes + LX_RAM + (64ull << 20) > pmm_free_ram()) why = "not enough free RAM for the ISO and the guest";
        // Дистрибутив, который разворачивается в память (Alpine ставит пакеты
        // в tmpfs), в 256 МБ тесно: даём 512, если у хоста есть запас.
        else if (iso_bytes + (512ull << 20) + (256ull << 20) <= pmm_free_ram()) ram = 512ull << 20;
    }
    if (!why) {
        // Подпись ISO 9660 — сразу, одним маленьким чтением: отказ «это не
        // образ диска» человек должен увидеть в ответ на команду, а не после
        // того, как весь файл прочитан.
        static uint8_t pvd[2048];
        int64_t r = from_cd ? iso_read(&cd, 16 * 2048, pvd, sizeof(pvd))
                            : fat32_read_file_at(iso_path, 16 * 2048, pvd, sizeof(pvd), 0);
        if (r != (int64_t)sizeof(pvd)) why = "reading the ISO file failed";
        else if (pvd[1] != 'C' || pvd[2] != 'D' || pvd[3] != '0' || pvd[4] != '0' || pvd[5] != '1')
            why = "not an ISO 9660 image (no CD001 signature)";
    }
    if (!why) {
        // Образ целиком в память: службы BIOS исполняются на ядре гостя, а с
        // него читать диск нельзя (CLAUDE.md §5.11) — только готовую память.
        iso_pages = (iso_bytes + 4095) / 4096;
        iso_hpa = pmm_alloc_big(iso_pages);
        if (iso_hpa == PMM_INVALID) { iso_hpa = 0; why = "no contiguous RAM for the ISO image"; }
    }
    if (why) { lx_refuse(out, why); return false; }

    uint8_t* z = (uint8_t*)&g_lx;
    for (size_t i = 0; i < sizeof(g_lx); i++) z[i] = 0;
    g_lx_error[0] = 0;
    g_lx.phase = LX_LOADING;
    g_lx.iso_hpa = iso_hpa;
    g_lx.iso_pages = iso_pages;
    g_lx.iso_bytes = iso_bytes;
    g_lx.ram = ram;
    g_lx.from_cd = from_cd;
    g_lx.cd = cd;
    g_lx.chunk = 256u << 10;
    g_lx.load_ms0 = timer_get_uptime_ms();
    lx_copy(g_lx.path, iso_path, (int)sizeof(g_lx.path));
    lx_copy(g_lx.pretype, pretype ? pretype : "", (int)sizeof(g_lx.pretype));
    const char* base = iso_path;
    for (const char* c = iso_path; *c; c++) if (*c == '/') base = c + 1;
    lx_set_name(base);
    ln_reset();
    ln_s(iso_path); ln_s(": "); ln_u(iso_bytes); ln_s(" bytes");
    ln_s(from_cd ? " from the installation medium" : " from the disk");
    ln_s(", guest RAM "); ln_u(ram >> 20); ln_s(" MB");
    serial_puts("[phantom] loading "); serial_puts(g_ln); serial_puts("\n");
    if (out) out("  reading the image into memory - the guest starts by itself (phantom status)\n");
    return true;
}

// Образ в памяти: собрать ВМ и запустить гостя. Отказ — в serial и в
// phantom_info (окно его покажет); `out` здесь уже нет — команда давно вернулась.
static void lx_boot(void) {
    const char* why = 0;
    hv_vm_t* vm = 0;
    pc_t* pc = 0;
    bios_t* bios = 0;
    hv_start_t st;
    bios_boot_t info;
    const uint8_t* iso = (const uint8_t*)(uintptr_t)g_lx.iso_hpa;
    uint64_t load_ms = timer_get_uptime_ms() - g_lx.load_ms0;

    vm = hv_vm_create(g_lx.ram, 0);
    if (!vm) why = hv_last_error();
    if (!why) {
        rtc_time_t t;
        rtc_read(&t);
        uint64_t unix_now = pc_unix_time(t.year, t.month, t.day, t.hour, t.minute, t.second);
        hv_set_wallclock(vm, unix_now);
        pc = pc_create(vm, unix_now);
        if (!pc) why = "the PC platform is busy";
    }
    if (!why) {
        // Один и тот же образ: загрузчику — через INT 13h, ядру — как диск.
        pc_attach_disk(pc, iso, g_lx.iso_bytes);
        int n = 0;
        while (g_lx.pretype[n]) n++;
        if (n && (!pc_console_input(pc, g_lx.pretype, n) || !pc_console_input(pc, "\r", 1)))
            why = "the boot line is too long";
    }
    if (!why) {
        bios = bios_create(vm, pc, iso, g_lx.iso_bytes);
        if (!bios) why = "the guest BIOS is busy";
    }
    if (!why) {
        const char* err = "";
        if (!bios_boot_cd(bios, &st, &info, &err)) why = err;
    }
    if (!why && !hv_vcpu_start(vm, &st, pc_model(), pc)) why = hv_last_error();
    if (why) {
        if (bios) bios_destroy(bios);
        if (pc) pc_destroy(pc);
        if (vm) hv_vm_destroy(vm);
        lx_free_image();
        g_lx.phase = LX_NONE;
        lx_refuse(0, why);
        return;
    }
    g_lx.phase = LX_GUEST;
    g_lx.vm = vm;
    g_lx.pc = pc;
    g_lx.bios = bios;
    g_lx.mark_tsc = rdtsc();
    g_lx.start_ms = timer_get_uptime_ms();
    g_lx.keys_on = true;
    ln_reset();
    ln_s("ISO \""); ln_s(info.volume); ln_s("\", "); ln_u(g_lx.iso_bytes);
    ln_s(" bytes; El Torito boot image at sector "); ln_u(info.boot_lba);
    ln_s(", "); ln_u(info.boot_sectors); ln_s(" x 512 bytes loaded at ");
    ln_x(info.load_seg); ln_s(":0; RAM "); ln_u(g_lx.ram >> 20); ln_s(" MB, core ");
    ln_u((uint64_t)hv_vcpu_slot(vm));
    ln_s("; image read in "); ln_u(load_ms); ln_s(" ms");
    serial_puts("[phantom] bios boot: "); serial_puts(g_ln); serial_puts("\n");
}

// Одна порция чтения образа (BSP, проход цикла WM). Проход должен оставаться
// коротким, поэтому читаем не «сколько успеем», а до LOAD_BUDGET_MS, и размер
// порции подстраиваем под диск: virtio-blk отдаёт мегабайты за миллисекунду,
// IDE PIO под KVM — 32 КБ за девять. Пока идёт чтение, цикл WM не засыпает
// (phantom_loading) — иначе на медленном диске половина времени уходила бы в сон.
#define LOAD_BUDGET_MS  8
#define LOAD_CHUNK_MIN  (32u << 10)
#define LOAD_CHUNK_MAX  (4u << 20)

static void lx_load_step(void) {
    uint8_t* dst = (uint8_t*)(uintptr_t)g_lx.iso_hpa;
    uint64_t t0 = timer_get_uptime_ms();
    while (g_lx.loaded < g_lx.iso_bytes) {
        uint64_t part = g_lx.iso_bytes - g_lx.loaded;
        if (part > g_lx.chunk) part = g_lx.chunk;
        uint64_t c0 = rdtsc();
        int64_t r = lx_read(g_lx.loaded, dst + g_lx.loaded, part);
        if (r <= 0) {
            lx_free_image();
            g_lx.phase = LX_NONE;
            lx_refuse(0, "reading the ISO file failed");
            return;
        }
        g_lx.loaded += (uint64_t)r;
        uint64_t us = tsc_to_us(rdtsc() - c0);
        if (us < 2000 && g_lx.chunk < LOAD_CHUNK_MAX) g_lx.chunk *= 2;
        else if (us > 6000 && g_lx.chunk > LOAD_CHUNK_MIN) g_lx.chunk /= 2;
        if (timer_get_uptime_ms() - t0 >= LOAD_BUDGET_MS) break;
    }
    if (g_lx.loaded >= g_lx.iso_bytes) lx_boot();
}

bool phantom_loading(void) { return g_lx.phase == LX_LOADING; }

// ----------------------------------------------------------------------------
// [50.5] Для окна гостя (app_phantom.c).
// ----------------------------------------------------------------------------
void phantom_info(phantom_info_t* o) {
    uint8_t* z = (uint8_t*)o;
    for (size_t i = 0; i < sizeof(*o); i++) z[i] = 0;
    lx_copy(o->error, g_lx_error, (int)sizeof(o->error));
    const char* why = unavailable();
    if (why) lx_copy(o->unavailable, why, (int)sizeof(o->unavailable));
    o->test_guest = g_spin.vm != 0;
    if (!g_lx.phase) return;
    lx_copy(o->image, g_lx.name, (int)sizeof(o->image));
    o->image_bytes = g_lx.iso_bytes;
    o->ram_mb = g_lx.ram >> 20;
    if (g_lx.phase == LX_LOADING) {
        o->state = PHANTOM_LOADING;
        o->loaded_bytes = g_lx.loaded;
        return;
    }
    const hv_stats_t* st = hv_stats(g_lx.vm);
    const pc_stats_t* ps = pc_stats(g_lx.pc);
    o->state = g_lx.reported ? PHANTOM_ENDED : PHANTOM_RUNNING;
    o->loaded_bytes = g_lx.iso_bytes;
    o->core = hv_vcpu_slot(g_lx.vm);
    o->sleeping = hv_vcpu_state(g_lx.vm) == HV_VCPU_HALTED;
    o->cpu_pct = g_lx.reported ? 0 : g_lx.cpu_pct;
    o->uptime_ms = (g_lx.reported ? g_lx.end_ms : timer_get_uptime_ms()) - g_lx.start_ms;
    o->exits = st->exits;
    o->disk_mb = ps->blk_sectors / 2048;
    o->keys = ps->kbd;
    lx_copy(o->end_text, g_lx.end_text, (int)sizeof(o->end_text));
}

void phantom_clear_error(void) { g_lx_error[0] = 0; }

bool phantom_screen_cells(uint16_t* cells, int* cur_col, int* cur_row) {
    if (!g_lx.vm) return false;
    // Видеопамять текста — окно 32 КБ; начало экрана гость двигает сам.
    const uint8_t* t = (const uint8_t*)hv_gpa(g_lx.vm, 0xB8000, 0x8000);
    if (!t) return false;
    uint32_t org = pc_vga_origin(g_lx.pc);
    for (uint32_t i = 0; i < 80 * 25; i++) {
        uint32_t off = ((org + i) * 2) & 0x7FFF;
        cells[i] = (uint16_t)(t[off] | (t[off + 1] << 8));
    }
    uint32_t cur = (pc_vga_cursor(g_lx.pc) - org) & 0x3FFF;
    bool on = pc_vga_cursor_on(g_lx.pc) && cur < 80 * 25;
    if (cur_col) *cur_col = on ? (int)(cur % 80) : -1;
    if (cur_row) *cur_row = on ? (int)(cur / 80) : -1;
    return true;
}

void phantom_key(uint8_t sc, bool extended) {
    if (!g_lx.keys_on) return;
    pc_t* pc = g_lx.pc;
    if (extended) (void)pc_kbd_scancode(pc, 0xE0);
    (void)pc_kbd_scancode(pc, sc);
}

void phantom_stop_guest(void) {
    if (g_lx.phase) lx_stop(0);
}

// Образы гостей: файлы *.iso в /disk/vault/phantom и в каталоге /guests
// установочного носителя.
static bool is_iso_name(const char* n) {
    int len = 0;
    while (n[len]) len++;
    return len > 4 && n[len - 4] == '.' && (n[len - 3] | 0x20) == 'i' &&
           (n[len - 2] | 0x20) == 's' && (n[len - 1] | 0x20) == 'o';
}

typedef struct { phantom_image_t* out; int max, n; } img_list_t;

static void img_add(img_list_t* L, const char* dir, const char* name, uint64_t bytes, bool medium) {
    if (L->n >= L->max || !is_iso_name(name)) return;
    phantom_image_t* im = &L->out[L->n];
    int k = 0;
    for (; name[k] && k < (int)sizeof(im->name) - 1; k++) im->name[k] = name[k];
    if (name[k]) return;                                // имя длиннее поля — такой файл не запустить
    im->name[k] = 0;
    lx_copy(im->path, dir, (int)sizeof(im->path));
    int p = 0;
    while (im->path[p]) p++;
    lx_copy(im->path + p, name, (int)sizeof(im->path) - p);
    im->bytes = bytes;
    im->on_medium = medium;
    L->n++;
}

static int img_fat_cb(const fat32_dir_entry_t* e, void* user) {
    if (!e->is_dir) img_add((img_list_t*)user, PHANTOM_DISK_DIR "/", e->name, e->size, false);
    return 0;
}

static int img_cd_cb(const char* name, const iso_entry_t* e, void* user) {
    if (!e->is_dir) img_add((img_list_t*)user, "/cdrom" PHANTOM_MEDIUM_DIR "/", name, e->size, true);
    return 0;
}

int phantom_images(phantom_image_t* out, int max) {
    img_list_t L = { out, max, 0 };
    if (fat32_is_mounted()) (void)fat32_iter_dir(PHANTOM_DISK_DIR, img_fat_cb, &L);
    if (iso_mount_boot()) (void)iso_list(PHANTOM_MEDIUM_DIR, img_cd_cb, &L);
    return L.n;
}

void phantom_screen(phantom_sink_t out) {
    if (!g_lx.vm) {
        serial_puts("[phantom] screen: no guest\n");
        if (out) out("  no guest\n");
        return;
    }
    char row[81];
    int shown = 0;
    for (int r = 0; r < 25; r++) {
        if (!bios_screen_row(g_lx.vm, pc_vga_origin(g_lx.pc), r, row)) continue;
        serial_puts("[screen] ");
        serial_print_dec((uint64_t)(r / 10)); serial_print_dec((uint64_t)(r % 10));
        serial_puts("|"); serial_puts(row); serial_puts("\n");
        shown++;
    }
    serial_puts("[phantom] screen: "); serial_print_dec((uint64_t)shown); serial_puts(" non-empty rows\n");
    if (out) out("  guest text screen dumped to the kernel log\n");
}

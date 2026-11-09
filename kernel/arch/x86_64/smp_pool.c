// ============================================================================
// smp_pool.c — пул вычислительных ядер (этап 49). Контракт — в smp_pool.h.
// ----------------------------------------------------------------------------
// Протокол parallel-for (почему он такой):
//   Параметры задачи (fn, ctx, n, grain) лежат в общей структуре g_job.
//   Публикуется задача ОДНИМ атомарным 64-битным словом
//       word = (поколение << 16) | маска_участников
//   Воркер читает слово целиком, поэтому пара «поколение + маска» у него
//   всегда согласована. Участник обязан ответить ack; вызывающий не трогает
//   g_job, пока не соберёт ack от ВСЕХ участников. Значит, никто не может
//   прочитать fn от одной задачи и ctx от следующей. Неучастники параметры
//   не читают вовсе — только запоминают поколение.
//   Куски раздаются динамически (atomic fetch_add по счётчику next): ядра,
//   которые хост-гипервизор притормозил, просто возьмут меньше работы.
//
// Сон без потерянных пробуждений:
//   cli → перепроверить «есть ли работа» → sti; hlt. Инструкция после sti
//   исполняется до приёма прерывания, поэтому IPI, пришедший между проверкой
//   и hlt, разбудит hlt, а не потеряется.
// ============================================================================
#include "smp_pool.h"
#include "smp.h"
#include "idt.h"
#include "lapic.h"
#include "cpu_stats.h"
#include "spinlock.h"
#include "serial.h"
#include "timer.h"
#include "io.h"

// ---- parallel-for -----------------------------------------------------------
typedef struct {
    smp_pool_range_fn fn;
    void*             ctx;
    int               n;
    int               grain;
    volatile int      next;        // следующий необработанный индекс
    volatile int      ack;         // сколько участников закончили
    int               nparts;      // сколько участников ждём
    volatile uint64_t word;        // (gen << 16) | mask — точка публикации
} pool_job_t;

static pool_job_t      g_job;
static uint64_t        g_job_gen;           // пишет только владелец g_job_lock
static spinlock_t      g_job_lock;

// ---- задача дирижёра ----------------------------------------------------------
static volatile uint64_t g_task_gen;
static smp_pool_task_fn  g_task_fn;
static void*             g_task_arg;
static volatile int      g_task_running;

// ---- состояние пула ------------------------------------------------------------
static volatile uint32_t g_online_mask;     // биты слотов AP, вошедших в цикл
static volatile int      g_hot;             // >0 — воркеры крутятся без сна

// ---- [этап 50] вызов на ядре, жилец, маска «в госте» ---------------------------
typedef struct {
    volatile uint64_t gen;        // номер запроса (пишет вызывающий)
    volatile uint64_t done;       // номер выполненного (пишет ядро)
    smp_pool_task_fn  fn;
    void*             arg;
} pool_call_t;
static pool_call_t       g_call[MAX_CPUS];
static spinlock_t        g_call_lock;

typedef struct {
    smp_pool_resident_fn fn;
    void*                arg;
    volatile int         active;
    volatile int         wake;        // «загляни к жильцу»: ставит smp_pool_kick
    uint64_t             pool_until;  // до этого момента (мс) ядро служит пулу
} pool_resident_t;
static pool_resident_t   g_res[MAX_CPUS];
static volatile uint32_t g_guest_mask;      // ядра внутри кванта гостя

// Что ядро уже видело. До этапа 50 это были локальные переменные цикла AP;
// теперь их читает и жилец (smp_pool_pending), поэтому они вынесены сюда.
// Каждую ячейку пишет только её ядро.
static uint64_t g_seen_job[MAX_CPUS];
static uint64_t g_seen_task;
static uint64_t g_seen_call[MAX_CPUS];

static inline uint64_t atomic_load64(volatile uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static inline void cpu_relax(void) { __asm__ __volatile__("pause" ::: "memory"); }

// __builtin_popcount без -mpopcnt зовёт __popcountdi2 из libgcc, а libgcc
// ядро не линкует. Маски у нас 16-битные — хватает цикла.
static inline int popcount16(uint32_t m) {
    int n = 0;
    for (; m; m &= m - 1) n++;
    return n;
}

// Обработчик IPI: вся работа — в цикле AP; здесь только EOI, иначе LAPIC
// держит бит ISR и следующие IPI того же приоритета не доставит.
static void pool_wake_handler(registers64_t* r) {
    (void)r;
    lapic_eoi();
}

void smp_pool_init(void) {
    spinlock_init(&g_job_lock);
    spinlock_init(&g_call_lock);
    g_guest_mask = 0;
    g_online_mask = 0;
    g_hot = 0;
    g_task_running = 0;
    idt64_set_handler(SMP_POOL_WAKE_VECTOR, pool_wake_handler);
}

int smp_pool_workers(void) {
    return popcount16(g_online_mask);
}

int smp_pool_current_slot(void) {
    // До появления AP (или на однопроцессорной машине) мы всегда на BSP.
    if (!g_online_mask) return 0;
    uint32_t id = lapic_get_id();
    for (int s = 1; s < MAX_CPUS; s++) {
        if ((g_online_mask & (1u << s)) && cpu_stats[s].lapic_id == id) return s;
    }
    return 0;
}

static void wake_slot(int slot) {
    lapic_send_ipi(cpu_stats[slot].lapic_id, SMP_POOL_WAKE_VECTOR,
                   LAPIC_DELIV_FIXED, 1, 0);
}

// [51.3] Разбудить главное ядро: программа на AP просит услугу (диск, окно,
// память), а BSP спит в цикле WM. Тот же вектор, тот же обработчик (один EOI);
// цикл WM считает это «настоящим» прерыванием и делает проход.
void smp_pool_wake_bsp(void) {
    lapic_send_ipi(cpu_stats[0].lapic_id, SMP_POOL_WAKE_VECTOR, LAPIC_DELIV_FIXED, 1, 0);
}

static void wake_mask(uint32_t mask) {
    for (int s = 1; s < MAX_CPUS; s++) {
        if (mask & (1u << s)) wake_slot(s);
    }
}

// Общий для вызывающего и воркеров цикл «взять кусок — посчитать».
static void run_chunks(void) {
    const int n = g_job.n, grain = g_job.grain;
    for (;;) {
        int b = __atomic_fetch_add(&g_job.next, grain, __ATOMIC_ACQ_REL);
        if (b >= n) break;
        int e = b + grain;
        if (e > n) e = n;
        g_job.fn(g_job.ctx, b, e);
    }
}

void smp_pool_parallel_for(smp_pool_range_fn fn, void* ctx, int n, int grain) {
    if (n <= 0) return;
    if (grain < 1) grain = 1;

    int me = smp_pool_current_slot();
    uint32_t mask = g_online_mask & ~(1u << me);
    // Дирижёр, занятый своей задачей, не может отвечать на чужие parallel-for.
    if (g_task_running && me != SMP_POOL_CONDUCTOR) mask &= ~(1u << SMP_POOL_CONDUCTOR);
    // [этап 50] Ядро внутри кванта гостя в задаче не участвует: ждать его
    // ack пришлось бы до конца кванта (2 мс на каждый parallel-for).
    mask &= ~__atomic_load_n(&g_guest_mask, __ATOMIC_SEQ_CST);

    if (!mask || n <= grain || __sync_lock_test_and_set(&g_job_lock.lock, 1)) {
        fn(ctx, 0, n);                      // нет помощников / пул занят
        return;
    }

    g_job.fn = fn;
    g_job.ctx = ctx;
    g_job.n = n;
    g_job.grain = grain;
    g_job.next = 0;
    g_job.ack = 0;
    g_job.nparts = popcount16(mask);
    g_job_gen++;
    // Все поля выше видны воркеру раньше, чем новое слово. SEQ_CST (а не
    // RELEASE) — ради проверки ниже: запись слова обязана дойти до памяти
    // раньше, чем мы прочитаем маску гостей (на x86 это xchg, полный барьер).
    __atomic_store_n(&g_job.word, (g_job_gen << 16) | mask, __ATOMIC_SEQ_CST);
    if (!g_hot) wake_mask(mask);
    // [этап 50] Гонка с входом в гостя: ядро могло поставить свой бит уже
    // после того, как мы прочитали маску. Обе стороны действуют по схеме
    // «объяви о себе, потом посмотри на другого» (ядро: бит, затем слово; мы:
    // слово, затем бит), поэтому хотя бы одна увидит другую. Если видим мы —
    // выбиваем ядро из гостя, и оно ответит ack как обычный участник.
    uint32_t late = mask & __atomic_load_n(&g_guest_mask, __ATOMIC_SEQ_CST);
    if (late) wake_mask(late);

    run_chunks();
    while (__atomic_load_n(&g_job.ack, __ATOMIC_ACQUIRE) < g_job.nparts) cpu_relax();

    __sync_lock_release(&g_job_lock.lock);
}

void smp_pool_hot_begin(void) {
    if (__atomic_fetch_add(&g_hot, 1, __ATOMIC_ACQ_REL) == 0) {
        // Будим спящих, чтобы они перешли в режим ожидания на pause.
        uint32_t mask = g_online_mask & ~(1u << smp_pool_current_slot());
        wake_mask(mask);
    }
}

void smp_pool_hot_end(void) {
    if (__atomic_load_n(&g_hot, __ATOMIC_ACQUIRE) > 0)
        __atomic_fetch_sub(&g_hot, 1, __ATOMIC_ACQ_REL);
}

bool smp_pool_spawn(smp_pool_task_fn fn, void* arg) {
    if (!(g_online_mask & (1u << SMP_POOL_CONDUCTOR))) return false;
    if (__atomic_load_n(&g_task_running, __ATOMIC_ACQUIRE)) return false;
    g_task_fn = fn;
    g_task_arg = arg;
    __atomic_store_n(&g_task_running, 1, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_task_gen, 1, __ATOMIC_ACQ_REL);
    wake_slot(SMP_POOL_CONDUCTOR);
    return true;
}

bool smp_pool_task_running(void) {
    return __atomic_load_n(&g_task_running, __ATOMIC_ACQUIRE) != 0;
}

// ----------------------------------------------------------------------------
// [этап 50] Вызов на конкретном ядре.
// ----------------------------------------------------------------------------
bool smp_pool_call(int slot, smp_pool_task_fn fn, void* arg) {
    if (slot == smp_pool_current_slot()) { fn(arg); return true; }
    if (slot < 1 || slot >= MAX_CPUS || !(g_online_mask & (1u << slot))) return false;
    if (slot == SMP_POOL_CONDUCTOR && smp_pool_task_running()) return false;

    spinlock_acquire(&g_call_lock);
    pool_call_t* c = &g_call[slot];
    c->fn = fn;
    c->arg = arg;
    uint64_t g = __atomic_add_fetch(&c->gen, 1, __ATOMIC_SEQ_CST);
    // IPI и будит спящее ядро, и выбивает его из гостя (внешнее прерывание
    // = VM exit); жилец увидит запрос через smp_pool_pending и уступит.
    wake_slot(slot);
    while (atomic_load64(&c->done) != g) cpu_relax();
    spinlock_release(&g_call_lock);
    return true;
}

// ----------------------------------------------------------------------------
// [этап 50] Жилец ядра (vCPU гостя). Контракт — в smp_pool.h.
// ----------------------------------------------------------------------------
int smp_pool_pick_resident(void) {
    for (int s = MAX_CPUS - 1; s >= 1; s--) {
        if (g_online_mask & (1u << s)) return s;
    }
    return 0;
}

bool smp_pool_set_resident(int slot, smp_pool_resident_fn fn, void* arg) {
    if (slot < 1 || slot >= MAX_CPUS || !(g_online_mask & (1u << slot))) return false;
    pool_resident_t* r = &g_res[slot];
    if (__atomic_load_n(&r->active, __ATOMIC_ACQUIRE)) return false;
    r->fn = fn;
    r->arg = arg;
    r->pool_until = 0;
    __atomic_store_n(&r->wake, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&r->active, 1, __ATOMIC_RELEASE);
    wake_slot(slot);
    return true;
}

bool smp_pool_resident_active(int slot) {
    if (slot < 1 || slot >= MAX_CPUS) return false;
    return __atomic_load_n(&g_res[slot].active, __ATOMIC_ACQUIRE) != 0;
}

void smp_pool_kick(int slot) {
    if (slot < 1 || slot >= MAX_CPUS) return;
    // Сначала флаг, потом IPI: ядро, уже решившее уснуть, перепроверяет флаг
    // с IF=0, а IPI, пришедший после этой проверки, разбудит его из hlt.
    __atomic_store_n(&g_res[slot].wake, 1, __ATOMIC_SEQ_CST);
    wake_slot(slot);
}

bool smp_pool_pending(int slot) {
    uint64_t w = atomic_load64(&g_job.word);
    if ((w >> 16) != g_seen_job[slot] && (w & (1u << slot))) return true;
    if (atomic_load64(&g_call[slot].gen) != g_seen_call[slot]) return true;
    if (slot == SMP_POOL_CONDUCTOR && atomic_load64(&g_task_gen) != g_seen_task) return true;
    return false;
}

bool smp_pool_guest_enter(int slot) {
    // Бит — раньше проверки (см. комментарий о гонке в smp_pool_parallel_for).
    __atomic_fetch_or(&g_guest_mask, 1u << slot, __ATOMIC_SEQ_CST);
    if (smp_pool_pending(slot)) {
        __atomic_fetch_and(&g_guest_mask, ~(1u << slot), __ATOMIC_SEQ_CST);
        return false;
    }
    return true;
}

void smp_pool_guest_leave(int slot) {
    __atomic_fetch_and(&g_guest_mask, ~(1u << slot), __ATOMIC_SEQ_CST);
}

// ----------------------------------------------------------------------------
// Цикл воркера на AP.
// ----------------------------------------------------------------------------
void smp_pool_ap_entry(int slot) {
    idt64_load_on_this_cpu();

    g_seen_job[slot]  = atomic_load64(&g_job.word) >> 16;
    g_seen_call[slot] = atomic_load64(&g_call[slot].gen);
    if (slot == SMP_POOL_CONDUCTOR) g_seen_task = atomic_load64(&g_task_gen);
    __atomic_fetch_or(&g_online_mask, 1u << slot, __ATOMIC_ACQ_REL);
    pool_resident_t* rs = &g_res[slot];

    for (;;) {
        bool did = false;

        // [этап 50] Вызов «на этом ядре» — первым: его результата ждёт BSP.
        uint64_t cg = atomic_load64(&g_call[slot].gen);
        if (cg != g_seen_call[slot]) {
            g_seen_call[slot] = cg;
            g_call[slot].fn(g_call[slot].arg);
            __atomic_store_n(&g_call[slot].done, cg, __ATOMIC_RELEASE);
            did = true;
        }

        if (slot == SMP_POOL_CONDUCTOR) {
            uint64_t tg = atomic_load64(&g_task_gen);
            if (tg != g_seen_task) {
                g_seen_task = tg;
                smp_pool_task_fn fn = g_task_fn;
                void* arg = g_task_arg;
                if (fn) fn(arg);
                __atomic_store_n(&g_task_running, 0, __ATOMIC_RELEASE);
                cpu_stats[slot].busy_ticks++;
                did = true;
            }
        }

        uint64_t w = atomic_load64(&g_job.word);
        if ((w >> 16) != g_seen_job[slot]) {
            g_seen_job[slot] = w >> 16;
            if (w & (1u << slot)) {
                run_chunks();
                __atomic_fetch_add(&g_job.ack, 1, __ATOMIC_ACQ_REL);
            }
            did = true;
        }
        if (did) continue;

        // [этап 50] Жилец (vCPU гостя) получает ядро, когда пулу оно не нужно.
        // В горячем режиме после кванта гостя ядро SMP_POOL_HOLD_MS служит
        // пулу: иначе считающий гость отнял бы у MIND ядро целиком.
        if (__atomic_load_n(&rs->active, __ATOMIC_ACQUIRE)) {
            bool hot = __atomic_load_n(&g_hot, __ATOMIC_ACQUIRE) != 0;
            if (!hot || timer_get_uptime_ms() >= rs->pool_until) {
                __atomic_store_n(&rs->wake, 0, __ATOMIC_SEQ_CST);
                int r = rs->fn(rs->arg);
                if (r < 0) { __atomic_store_n(&rs->active, 0, __ATOMIC_RELEASE); continue; }
                if (r > 0) {
                    // r == 2: гость не исполнялся, уступил пулу — квант ему не засчитан.
                    if (hot && r == 1) rs->pool_until = timer_get_uptime_ms() + SMP_POOL_HOLD_MS;
                    continue;               // гость хочет ещё; пул проверим на новом обороте
                }
                // r == 0: гость спит — ядро свободно, идём к обычному сну воркера.
            }
        }

        if (__atomic_load_n(&g_hot, __ATOMIC_ACQUIRE)) { cpu_relax(); continue; }

        // Засыпаем, перепроверив условия с IF=0 (см. шапку файла).
        cli();
        bool work = ((atomic_load64(&g_job.word) >> 16) != g_seen_job[slot]) ||
                    (atomic_load64(&g_call[slot].gen) != g_seen_call[slot]) ||
                    (slot == SMP_POOL_CONDUCTOR && atomic_load64(&g_task_gen) != g_seen_task) ||
                    (__atomic_load_n(&rs->active, __ATOMIC_ACQUIRE) &&
                     __atomic_load_n(&rs->wake, __ATOMIC_SEQ_CST)) ||
                    __atomic_load_n(&g_hot, __ATOMIC_ACQUIRE);
        if (work) { sti(); continue; }
        cpu_stats[slot].idle_ticks++;
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

// ----------------------------------------------------------------------------
// Самопроверка (smoke 49).
// ----------------------------------------------------------------------------
#define ST_N 4096
static uint32_t g_st_out[ST_N];
static volatile uint32_t g_st_slots_seen;

static uint32_t st_value(int i) {
    // Немного «настоящей» работы: FNV-подобное перемешивание + float,
    // чтобы заодно проверить, что SSE на AP работает корректно.
    uint32_t h = 2166136261u ^ (uint32_t)i;
    for (int k = 0; k < 64; k++) h = (h ^ (uint32_t)k) * 16777619u;
    float f = (float)(i % 1000) * 0.5f + 0.25f;
    return h ^ (uint32_t)(f * 4.0f);
}

static void st_range(void* ctx, int b, int e) {
    (void)ctx;
    __atomic_fetch_or(&g_st_slots_seen, 1u << smp_pool_current_slot(), __ATOMIC_ACQ_REL);
    for (int i = b; i < e; i++) g_st_out[i] = st_value(i);
}

static volatile int g_st_task_ok;
static void st_task(void* arg) {
    (void)arg;
    // Дирижёр сам раздаёт parallel-for остальным AP.
    for (int i = 0; i < ST_N; i++) g_st_out[i] = 0;
    smp_pool_hot_begin();
    smp_pool_parallel_for(st_range, 0, ST_N, 64);
    smp_pool_hot_end();
    int ok = 1;
    for (int i = 0; i < ST_N; i++) if (g_st_out[i] != st_value(i)) { ok = 0; break; }
    g_st_task_ok = ok ? 1 : -1;
}

bool smp_pool_self_test(void) {
    int workers = smp_pool_workers();
    serial_puts("[smoke 49] pool workers=");
    serial_print_dec((uint64_t)workers);
    serial_puts("\n");

    // 1) parallel-for с BSP (IPI-пробуждение, без горячего режима).
    for (int i = 0; i < ST_N; i++) g_st_out[i] = 0;
    g_st_slots_seen = 0;
    smp_pool_parallel_for(st_range, 0, ST_N, 64);
    bool pf_ok = true;
    for (int i = 0; i < ST_N; i++) if (g_st_out[i] != st_value(i)) { pf_ok = false; break; }
    serial_puts("[smoke 49] pool parallel_for ");
    serial_puts(pf_ok ? "OK" : "MISMATCH");
    serial_puts(" slots_mask=0x");
    serial_print_hex(g_st_slots_seen, 4);
    serial_puts("\n");

    // 2) Задача на дирижёре, которая сама раздаёт parallel-for.
    bool task_ok = true;
    if (workers > 0) {
        g_st_task_ok = 0;
        if (!smp_pool_spawn(st_task, 0)) task_ok = false;
        uint64_t t0 = timer_get_uptime_ms();
        while (task_ok && smp_pool_task_running() &&
               timer_get_uptime_ms() - t0 < 3000) {
            __asm__ __volatile__("sti; hlt");
        }
        if (smp_pool_task_running() || g_st_task_ok != 1) task_ok = false;
        serial_puts("[smoke 49] pool conductor task ");
        serial_puts(task_ok ? "OK" : "FAIL");
        serial_puts("\n");
    }
    return pf_ok && task_ok;
}

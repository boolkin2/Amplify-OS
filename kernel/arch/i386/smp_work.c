// ============================================================================
// smp_work.c - реализация общей очереди задач для SMP work-pull.
// ----------------------------------------------------------------------------
// Кольцевой буфер фиксированного размера, защищённый spinlock'ом. Производитель
// (любой CPU) пишет в g_tail, потребитель (AP в idle/busy) забирает с g_head.
// Такая блокировка проще и предсказуемее, чем lock-free MPMC-очередь, и
// учитывая, что под нагрузкой у нас десятки задач в секунду — оверхед лока
// ничтожен.
// ============================================================================

#include "smp_work.h"
#include "spinlock.h"
#include "lapic.h"
#include "acpi.h"
#include "cpu_stats.h"

#define SMP_WORK_QUEUE_SIZE 64
#define SMP_WORK_QUEUE_MASK (SMP_WORK_QUEUE_SIZE - 1)

typedef struct {
    work_fn_t fn;
    void*     arg;
} work_item_t;

static work_item_t  g_queue[SMP_WORK_QUEUE_SIZE];
static volatile uint32_t g_head = 0;   // куда читать (потребитель)
static volatile uint32_t g_tail = 0;   // куда писать  (производитель)
static spinlock_t   g_lock = { 0 };
static volatile bool g_initialized = false;

volatile uint32_t cpu_jobs_done[MAX_CPUS] = { 0 };

void smp_work_init(void) {
    g_head = 0;
    g_tail = 0;
    for (int i = 0; i < SMP_WORK_QUEUE_SIZE; i++) {
        g_queue[i].fn  = 0;
        g_queue[i].arg = 0;
    }
    for (int i = 0; i < MAX_CPUS; i++) {
        cpu_jobs_done[i] = 0;
    }
    g_initialized = true;
}

bool smp_post_work(work_fn_t fn, void* arg) {
    if (!g_initialized || !fn) return false;

    spinlock_acquire(&g_lock);
    uint32_t next_tail = (g_tail + 1) & SMP_WORK_QUEUE_MASK;
    if (next_tail == g_head) {
        // Очередь полна — лучше отказать, чем перетереть.
        spinlock_release(&g_lock);
        return false;
    }
    g_queue[g_tail].fn  = fn;
    g_queue[g_tail].arg = arg;
    g_tail = next_tail;
    spinlock_release(&g_lock);
    return true;
}

bool smp_run_one_if_any(void) {
    if (!g_initialized) return false;

    work_fn_t fn = 0;
    void*     arg = 0;

    spinlock_acquire(&g_lock);
    if (g_head != g_tail) {
        fn  = g_queue[g_head].fn;
        arg = g_queue[g_head].arg;
        g_queue[g_head].fn  = 0;
        g_queue[g_head].arg = 0;
        g_head = (g_head + 1) & SMP_WORK_QUEUE_MASK;
    }
    spinlock_release(&g_lock);

    if (fn) {
        fn(arg);
        // Учтём, кто из CPU выполнил работу. lapic_get_id() работает на любом
        // ядре, но нам нужно соответствие slot из cpu_stats[]. Линейный поиск
        // допустим: MAX_CPUS обычно <= 32.
        uint32_t my_lapic = lapic_get_id();
        for (int i = 0; i < MAX_CPUS; i++) {
            if (cpu_stats[i].online && cpu_stats[i].lapic_id == my_lapic) {
                __sync_fetch_and_add(&cpu_jobs_done[i], 1);
                break;
            }
        }
        return true;
    }
    return false;
}

uint32_t smp_work_pending(void) {
    if (!g_initialized) return 0;
    // Без лока: оба указателя 32-битные и читаются атомарно на i386.
    // Это приблизительная оценка для UI — точное значение не критично.
    uint32_t h = g_head;
    uint32_t t = g_tail;
    return (t - h) & SMP_WORK_QUEUE_MASK;
}

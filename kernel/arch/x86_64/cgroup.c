// ============================================================================
// cgroup.c — реализация cgroups v2 для AMPLIFY-OS x86_64 (этап 46).
// ----------------------------------------------------------------------------
// Поведение:
//
//  1. Лимит установлен в БАЙТАХ/НАНОСЕКУНДАХ. 0 = «unlimited».
//
//  2. Иерархия: при charge мы поднимаемся вверх по parent-chain, проверяя
//     лимит на каждом уровне. Если хоть один родитель отказывает — операция
//     отклоняется. При успехе обновляем counters на ВСЕХ уровнях.
//
//  3. CPU-budget пересчитывается каждый период (cpu_period_ns). Сброс
//     происходит в cgroup_tick(), вызываемом из timer-IRQ. Если в течение
//     периода cpu_used_ns превышено — charge_cpu() возвращает -1, что
//     задержит запуск этой задачи (см. task_yield в task.c, проверка
//     перед switch'ем).
//
//  4. RAM ограничивается мгновенным значением. Если задача через kmalloc
//     или vm_brk_grow пытается выйти за mem_max — операция reject'ится.
//
//  5. IO ограничен суммой total. Полезно для теста песочницы; в реальной
//     системе это был бы rate-limit, но даже простой хард-кэп даёт
//     возможность защититься от runaway loop'ов.
//
//  6. PIDS — max количество задач в группе. Используется, чтобы fork-bomb
//     внутри box не положил систему.
// ============================================================================
#include "cgroup.h"
#include "heap.h"
#include "serial.h"
#include "timer.h"

// ---- Локальные хелперы ----------------------------------------------------
static void cg_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int cg_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// ---- Глобальное состояние -------------------------------------------------
static cgroup_t   g_cgroups[CGROUP_MAX];
static bool       g_cg_used[CGROUP_MAX];
static cgroup_t*  g_root        = 0;
static int        g_next_id     = 1;

// ---- Хелперы поднятия по дереву -------------------------------------------
// Применить delta_mem: вверх по parent-chain. Если хоть один отказывает —
// откатываем уже применённые. Возвращает 0/-1.
static int chain_charge(cgroup_t* leaf, cgroup_controller_t ctrl, int64_t delta) {
    if (!leaf) return 0;

    cgroup_t* path[CGROUP_MAX];
    int npath = 0;
    for (cgroup_t* c = leaf; c && npath < CGROUP_MAX; c = c->parent) path[npath++] = c;

    // Проверка-применение в одном проходе.
    for (int i = 0; i < npath; i++) {
        cgroup_t* c = path[i];
        bool ok = true;
        switch (ctrl) {
        case CG_CTRL_MEM: {
            if (delta > 0 && c->mem_max_bytes &&
                c->mem_current + (uint64_t)delta > c->mem_max_bytes) {
                c->mem_oom++;
                ok = false;
            }
            break;
        }
        case CG_CTRL_IO: {
            if (delta > 0 && c->io_max_bytes &&
                c->io_current + (uint64_t)delta > c->io_max_bytes) {
                c->io_throttles++;
                ok = false;
            }
            break;
        }
        case CG_CTRL_PIDS: {
            if (delta > 0 && c->pids_max > 0 &&
                c->pids_current + (int)delta > c->pids_max) {
                c->pids_denied++;
                ok = false;
            }
            break;
        }
        case CG_CTRL_CPU: {
            // delta трактуется как NS, всегда положительный
            if (c->cpu_max_ns &&
                c->cpu_used_ns + (uint64_t)delta > c->cpu_max_ns) {
                c->cpu_throttles++;
                ok = false;
            }
            break;
        }
        default: ok = false;
        }
        if (!ok) {
            // Откатить уже сделанные (они не были сделаны — на этом уровне отказ).
            return -1;
        }
    }

    // Все проверили — применяем.
    for (int i = 0; i < npath; i++) {
        cgroup_t* c = path[i];
        switch (ctrl) {
        case CG_CTRL_MEM: {
            int64_t v = (int64_t)c->mem_current + delta;
            if (v < 0) v = 0;
            c->mem_current = (uint64_t)v;
            if (c->mem_current > c->mem_peak) c->mem_peak = c->mem_current;
            break;
        }
        case CG_CTRL_IO:
            c->io_current += (uint64_t)delta;
            break;
        case CG_CTRL_PIDS:
            c->pids_current += (int)delta;
            if (c->pids_current < 0) c->pids_current = 0;
            break;
        case CG_CTRL_CPU:
            c->cpu_used_ns += (uint64_t)delta;
            c->cpu_total_ns += (uint64_t)delta;
            break;
        default: break;
        }
    }
    return 0;
}

// ---- Создание / уничтожение ----------------------------------------------

void cgroup_init(void) {
    if (g_root) return;
    for (int i = 0; i < CGROUP_MAX; i++) g_cg_used[i] = false;

    g_cg_used[0] = true;
    g_root = &g_cgroups[0];
    g_root->id            = g_next_id++;
    g_root->refcount      = 1;
    cg_strncpy(g_root->name, "root", CGROUP_NAME_LEN);
    g_root->parent        = 0;
    g_root->cpu_max_ns    = 0;
    g_root->cpu_period_ns = 100000000ULL;       // 100 ms по умолчанию
    g_root->mem_max_bytes = 0;
    g_root->io_max_bytes  = 0;
    g_root->pids_max      = 0;
    g_root->cpu_used_ns   = 0;
    g_root->cpu_total_ns  = 0;
    g_root->period_start_ms = 0;
    g_root->mem_current   = 0;
    g_root->mem_peak      = 0;
    g_root->io_current    = 0;
    g_root->pids_current  = 0;
    g_root->cpu_throttles = 0;
    g_root->mem_oom       = 0;
    g_root->io_throttles  = 0;
    g_root->pids_denied   = 0;

    serial_puts("[cgroup] root cgroup created\n");
}

cgroup_t* cgroup_root(void) { return g_root; }

cgroup_t* cgroup_create(cgroup_t* parent, const char* name) {
    if (!parent) parent = g_root;
    int slot = -1;
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!g_cg_used[i]) { slot = i; break; }
    }
    if (slot < 0) return 0;
    g_cg_used[slot] = true;
    cgroup_t* c = &g_cgroups[slot];
    c->id            = g_next_id++;
    c->refcount      = 1;
    cg_strncpy(c->name, name, CGROUP_NAME_LEN);
    c->parent        = parent;
    c->cpu_max_ns    = 0;
    c->cpu_period_ns = parent->cpu_period_ns;
    c->mem_max_bytes = 0;
    c->io_max_bytes  = 0;
    c->pids_max      = 0;
    c->cpu_used_ns   = 0;
    c->cpu_total_ns  = 0;
    c->period_start_ms = timer_get_uptime_ms();
    c->mem_current   = 0;
    c->mem_peak      = 0;
    c->io_current    = 0;
    c->pids_current  = 0;
    c->cpu_throttles = 0;
    c->mem_oom       = 0;
    c->io_throttles  = 0;
    c->pids_denied   = 0;
    if (parent) parent->refcount++;
    return c;
}

void cgroup_destroy(cgroup_t* cg) {
    if (!cg || cg == g_root) return;
    int slot = (int)(cg - g_cgroups);
    if (slot < 0 || slot >= CGROUP_MAX) return;
    if (cg->parent) cg->parent->refcount--;
    g_cg_used[slot] = false;
}

cgroup_t* cgroup_retain(cgroup_t* cg) {
    if (!cg) return 0;
    cg->refcount++;
    return cg;
}

void cgroup_release(cgroup_t* cg) {
    if (!cg) return;
    cg->refcount--;
    if (cg->refcount <= 0 && cg != g_root) cgroup_destroy(cg);
}

// ---- Лимиты ---------------------------------------------------------------
void cgroup_set_limit_cpu(cgroup_t* cg, uint64_t max_ns, uint64_t period_ns) {
    if (!cg) return;
    cg->cpu_max_ns    = max_ns;
    if (period_ns) cg->cpu_period_ns = period_ns;
}

void cgroup_set_limit_mem(cgroup_t* cg, uint64_t max_bytes) {
    if (!cg) return;
    cg->mem_max_bytes = max_bytes;
}

void cgroup_set_limit_io(cgroup_t* cg, uint64_t max_bytes) {
    if (!cg) return;
    cg->io_max_bytes = max_bytes;
}

void cgroup_set_limit_pids(cgroup_t* cg, int max_pids) {
    if (!cg) return;
    cg->pids_max = max_pids;
}

// ---- Учёт ----------------------------------------------------------------
int cgroup_charge_mem(cgroup_t* cg, int64_t bytes) {
    return chain_charge(cg, CG_CTRL_MEM, bytes);
}

int cgroup_charge_io(cgroup_t* cg, int64_t bytes) {
    return chain_charge(cg, CG_CTRL_IO, bytes);
}

int cgroup_charge_cpu(cgroup_t* cg, uint64_t ns) {
    return chain_charge(cg, CG_CTRL_CPU, (int64_t)ns);
}

int cgroup_charge_pids(cgroup_t* cg, int delta) {
    return chain_charge(cg, CG_CTRL_PIDS, (int64_t)delta);
}

// ---- Поиск / итерирование ------------------------------------------------
cgroup_t* cgroup_find(const char* name) {
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_cg_used[i] && cg_strcmp(g_cgroups[i].name, name) == 0) {
            return &g_cgroups[i];
        }
    }
    return 0;
}

int cgroup_count(void) {
    int n = 0;
    for (int i = 0; i < CGROUP_MAX; i++) if (g_cg_used[i]) n++;
    return n;
}

cgroup_t* cgroup_at(int idx) {
    int n = 0;
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (g_cg_used[i]) {
            if (n == idx) return &g_cgroups[i];
            n++;
        }
    }
    return 0;
}

// ---- Tick (вызывается из timer IRQ или task_yield) -----------------------
// Сбрасывает cpu_used_ns если прошёл период. Простой и дешёвый — O(N) по
// количеству активных групп, обычно 0..3.
void cgroup_tick(void) {
    uint64_t now_ms = timer_get_uptime_ms();
    for (int i = 0; i < CGROUP_MAX; i++) {
        if (!g_cg_used[i]) continue;
        cgroup_t* c = &g_cgroups[i];
        if (!c->cpu_period_ns) continue;
        uint64_t period_ms = c->cpu_period_ns / 1000000ULL;
        if (period_ms == 0) period_ms = 1;
        if (now_ms - c->period_start_ms >= period_ms) {
            c->cpu_used_ns = 0;
            c->period_start_ms = now_ms;
        }
    }
}

// ============================================================================
// cgroup.h — иерархические контрольные группы (cgroups v2) для AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Этап 46. Минимальная реализация, достаточная для box-runtime:
//
//  * каждая группа учитывает CPU-time-budget (наносекунды в эпоху),
//    RAM-usage (байты, накапливается через kmalloc-хук) и IO-bytes
//    (через хук в disk_io.c).
//  * иерархия parent/child: дочерние ограничения наследуются и не
//    могут превышать родительские (если родитель установил max, child
//    ограничен min(child.max, parent.max)).
//  * task_t.cgroup — указатель на текущую cgroup (NULL = root, без лимитов).
//
// Учёт через специальные обёртки: cgroup_charge_*, вызываются из vm_brk_grow,
// disk_io_read/write, task_yield (CPU-tick).  Если лимит превышен — функция
// возвращает -1 и вызывающий должен корректно обработать (в случае RAM —
// brk откатывается, в случае CPU — задача yield'ится принудительно).
// ============================================================================
#ifndef AOS_X86_64_CGROUP_H
#define AOS_X86_64_CGROUP_H

#include <stdint.h>
#include <stdbool.h>

#define CGROUP_NAME_LEN  32
#define CGROUP_MAX       16   /* всего одновременно живых cgroup'ов в системе */

typedef enum {
    CG_CTRL_CPU = 0,    /* лимит CPU-time в наносекундах за CG_PERIOD_NS */
    CG_CTRL_MEM = 1,    /* лимит RAM (current usage) */
    CG_CTRL_IO  = 2,    /* лимит IO bytes total */
    CG_CTRL_PIDS= 3,    /* лимит количества задач */
    CG_CTRL_MAX
} cgroup_controller_t;

typedef struct cgroup {
    int        id;
    int        refcount;
    char       name[CGROUP_NAME_LEN];
    struct cgroup* parent;

    // Лимиты (0 = unlimited).
    uint64_t   cpu_max_ns;        // время CPU за период
    uint64_t   cpu_period_ns;     // длина периода
    uint64_t   mem_max_bytes;     // RAM usage cap
    uint64_t   io_max_bytes;      // IO total cap
    int        pids_max;          // количество задач cap

    // Учёт (running totals).
    uint64_t   cpu_used_ns;       // в текущем периоде
    uint64_t   cpu_total_ns;      // accumulated
    uint64_t   period_start_ms;   // timer_get_uptime_ms на момент сброса
    uint64_t   mem_current;
    uint64_t   mem_peak;
    uint64_t   io_current;
    int        pids_current;

    // Статистика отказов.
    uint64_t   cpu_throttles;
    uint64_t   mem_oom;
    uint64_t   io_throttles;
    uint64_t   pids_denied;
} cgroup_t;

// ---- API ------------------------------------------------------------------
void       cgroup_init(void);
cgroup_t*  cgroup_root(void);

cgroup_t*  cgroup_create(cgroup_t* parent, const char* name);
void       cgroup_destroy(cgroup_t* cg);

cgroup_t*  cgroup_retain(cgroup_t* cg);
void       cgroup_release(cgroup_t* cg);

// Заполняет лимиты. unset(0) → no limit.
void       cgroup_set_limit_cpu (cgroup_t* cg, uint64_t max_ns, uint64_t period_ns);
void       cgroup_set_limit_mem (cgroup_t* cg, uint64_t max_bytes);
void       cgroup_set_limit_io  (cgroup_t* cg, uint64_t max_bytes);
void       cgroup_set_limit_pids(cgroup_t* cg, int max_pids);

// Хуки учёта. Возвращают 0 при успехе, -1 при превышении лимита.
// charge_alloc/free для RAM: задача выделила N байт.
int        cgroup_charge_mem (cgroup_t* cg, int64_t bytes);
int        cgroup_charge_io  (cgroup_t* cg, int64_t bytes);
int        cgroup_charge_cpu (cgroup_t* cg, uint64_t ns);
int        cgroup_charge_pids(cgroup_t* cg, int delta);

// Получить cgroup по имени (для shell). NULL если не найден.
cgroup_t*  cgroup_find(const char* name);

// Итератор для shell-команды cgstat.
int        cgroup_count(void);
cgroup_t*  cgroup_at(int idx);

// Сброс CPU-budget'а в начале нового периода (вызывается timer-irq).
void       cgroup_tick(void);

#endif

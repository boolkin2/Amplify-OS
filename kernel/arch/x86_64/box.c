// ============================================================================
// box.c — реализация box-runtime (этап 46).
// ----------------------------------------------------------------------------
// Алгоритм box_run:
//
//   1. Открыть ELF из VFS, прочитать его в kernel-buffer.
//   2. Создать новый vm_space и cgroup и ns_set (через ns_unshare с
//      запрошенными флагами).
//   3. Загрузить ELF (elf64_load) в новый vm.
//   4. Аллоцировать user-stack (2 страницы).
//   5. task_create_user; затем — назначить ему ns/cgroup/seccomp.
//   6. Если в spec'е есть private_root и mount-ns — добавить allow-prefix
//      в mnt_ns (например "/box/<id>/" для /tmp).
//   7. Не делаем task_yield автоматически — пусть кто-то более высокий
//      уровень дёрнет (shell делает yield, GUI — нет, задача стартует
//      когда WM-loop тикнет).
// ============================================================================
#include "box.h"
#include "ns.h"
#include "cgroup.h"
#include "seccomp.h"
#include "task.h"
#include "vfs.h"
#include "vm.h"
#include "elf.h"
#include "pmm.h"
#include "heap.h"
#include "memory.h"
#include "serial.h"

// ---- Локальные хелперы ----------------------------------------------------
static void bx_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static int bx_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }

static void log_append(char* log, int log_max, const char* s) {
    if (!log || log_max <= 0) return;
    int cur = 0;
    while (log[cur] && cur < log_max - 1) cur++;
    int i = 0;
    while (s[i] && cur < log_max - 1) { log[cur++] = s[i++]; }
    log[cur] = 0;
}

static void log_dec(char* log, int log_max, uint64_t v) {
    if (!log || log_max <= 0) return;
    int cur = 0;
    while (log[cur] && cur < log_max - 1) cur++;
    char tmp[24]; int tn = 0;
    if (v == 0) tmp[tn++] = '0';
    else while (v > 0) { tmp[tn++] = '0' + (v % 10); v /= 10; }
    while (tn > 0 && cur < log_max - 1) log[cur++] = tmp[--tn];
    log[cur] = 0;
}

void box_spec_default(box_spec_t* spec, const char* path, const char* name) {
    if (!spec) return;
    for (uint32_t i = 0; i < sizeof(*spec); i++) ((uint8_t*)spec)[i] = 0;
    bx_strncpy(spec->path, path ? path : "", BOX_PATH_LEN);
    bx_strncpy(spec->name, name ? name : "box", 32);
    bx_strncpy(spec->seccomp_profile, "default", 16);
    bx_strncpy(spec->hostname, "box", 64);

    // По умолчанию: PID + UTS + NET + MOUNT, БЕЗ user-ns (он требует особого
    // продумывания cap'ов; для песочницы достаточно остальных четырёх).
    spec->clone_flags  = AOS_CLONE_NEWPID | AOS_CLONE_NEWUTS |
                         AOS_CLONE_NEWNET | AOS_CLONE_NEWNS;
    spec->mem_max      = 4 * 1024 * 1024;       // 4 MB
    spec->cpu_max_ns   = 50000000ULL;           // 50 ms за период
    spec->cpu_period_ns= 100000000ULL;          // 100 ms период
    spec->io_max       = 1024 * 1024;           // 1 MB IO
    spec->pids_max     = 8;
}

// ---- Внутренний реестр box-задач (для box_list / GUI) --------------------
#define BOX_MAX  16
typedef struct {
    bool     used;
    int      tid;
    int      visible_pid;
    int      cgroup_id;
    char     name[32];
    char     path[64];
    cgroup_t* cg;            // для запроса live-метрик
    seccomp_filter_t* sc;
} box_record_t;
static box_record_t g_boxes[BOX_MAX];

static void box_record_add(int tid, int vpid, cgroup_t* cg, seccomp_filter_t* sc,
                           const char* name, const char* path) {
    for (int i = 0; i < BOX_MAX; i++) {
        if (!g_boxes[i].used) {
            g_boxes[i].used = true;
            g_boxes[i].tid = tid;
            g_boxes[i].visible_pid = vpid;
            g_boxes[i].cgroup_id   = cg ? cg->id : -1;
            g_boxes[i].cg          = cg;
            g_boxes[i].sc          = sc;
            bx_strncpy(g_boxes[i].name, name, 32);
            bx_strncpy(g_boxes[i].path, path, 64);
            return;
        }
    }
}

int box_list(box_listing_t* out, int max) {
    int n = 0;
    for (int i = 0; i < BOX_MAX && n < max; i++) {
        if (!g_boxes[i].used) continue;
        // Вычистим запись если задача уже завершилась (DEAD).
        task_t* t = task_find_by_tid(g_boxes[i].tid);
        if (!t || t->state == TASK_DEAD) {
            g_boxes[i].used = false;
            continue;
        }
        out[n].tid              = g_boxes[i].tid;
        out[n].visible_pid      = g_boxes[i].visible_pid;
        out[n].cgroup_id        = g_boxes[i].cgroup_id;
        bx_strncpy(out[n].name, g_boxes[i].name, 32);
        bx_strncpy(out[n].path, g_boxes[i].path, 64);
        out[n].mem_used         = g_boxes[i].cg ? g_boxes[i].cg->mem_current : 0;
        out[n].cpu_total_ns     = g_boxes[i].cg ? g_boxes[i].cg->cpu_total_ns : 0;
        out[n].seccomp_blocked  = g_boxes[i].sc ? (int)g_boxes[i].sc->total_blocked : 0;
        n++;
    }
    return n;
}

// ---- Главная функция ------------------------------------------------------
int box_run(const box_spec_t* spec, char* log, int log_max) {
    if (!spec || !spec->path[0]) {
        log_append(log, log_max, "[error] box_run: empty path\n");
        return -1;
    }
    log_append(log, log_max, "[box] open ");
    log_append(log, log_max, spec->path);
    log_append(log, log_max, "\n");

    // 1. Прочитать ELF.
    int fd = vfs_open(spec->path);
    if (fd < 0) {
        log_append(log, log_max, "[error] file not found in VFS\n");
        return -1;
    }
    int64_t sz = vfs_size_fd(fd);
    if (sz <= 0 || sz > 8 * 1024 * 1024) {
        log_append(log, log_max, "[error] bad file size\n");
        vfs_close(fd);
        return -1;
    }
    uint8_t* blob = (uint8_t*)kmalloc((uint64_t)sz);
    if (!blob) {
        log_append(log, log_max, "[error] out of memory (blob)\n");
        vfs_close(fd);
        return -1;
    }
    int64_t got = vfs_read(fd, blob, (uint64_t)sz);
    vfs_close(fd);
    if (got != sz) {
        log_append(log, log_max, "[error] short read\n");
        kfree(blob);
        return -1;
    }

    // 2. Новый vm_space.
    vm_space_t* vm = vm_create_user();
    if (!vm) {
        log_append(log, log_max, "[error] vm_create_user failed\n");
        kfree(blob);
        return -1;
    }

    uint64_t entry = elf64_load(blob, (uint64_t)sz, vm);
    if (!entry) {
        log_append(log, log_max, "[error] elf64_load failed\n");
        kfree(blob);
        return -1;
    }

    // 3. User-stack.
    uint64_t stack_va_base = USER_BASE + 0x100000;
    for (int i = 0; i < 2; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) {
            log_append(log, log_max, "[error] OOM stack\n");
            kfree(blob); return -1;
        }
        if (!vm_map_user_page(vm, stack_va_base + i * 0x1000, pa, true)) {
            log_append(log, log_max, "[error] stack map failed\n");
            kfree(blob); return -1;
        }
    }
    uint64_t user_rsp = stack_va_base + 2 * 0x1000 - 16;

    // 4. Создать задачу.
    task_t* t = task_create_user(vm, entry, user_rsp);
    kfree(blob);
    if (!t) {
        log_append(log, log_max, "[error] task_create_user failed\n");
        return -1;
    }

    // 5. Namespaces.
    ns_set_t* base = task_current() ? task_current()->ns : ns_root();
    ns_set_t* fresh = ns_unshare(base, spec->clone_flags);
    if (!fresh) {
        log_append(log, log_max, "[error] ns_unshare failed\n");
        return -1;
    }
    if (t->ns) ns_release(t->ns);
    t->ns = fresh;
    if (spec->hostname[0]) ns_sethostname(t, spec->hostname, bx_strlen(spec->hostname));
    if (spec->private_root[0]) ns_mnt_allow(t, spec->private_root);
    int vpid = ns_pid_assign(t);
    log_append(log, log_max, "[box] ns unshared, visible_pid=");
    log_dec(log, log_max, (uint64_t)vpid);
    log_append(log, log_max, "\n");

    // 6. Cgroup.
    cgroup_t* cg = cgroup_create(cgroup_root(), spec->name);
    if (cg) {
        cgroup_set_limit_cpu (cg, spec->cpu_max_ns, spec->cpu_period_ns);
        cgroup_set_limit_mem (cg, spec->mem_max);
        cgroup_set_limit_io  (cg, spec->io_max);
        cgroup_set_limit_pids(cg, spec->pids_max);
        cgroup_charge_pids(cg, +1);
        if (t->cgroup) cgroup_release(t->cgroup);
        t->cgroup = cgroup_retain(cg);
        log_append(log, log_max, "[box] cgroup '");
        log_append(log, log_max, spec->name);
        log_append(log, log_max, "' id=");
        log_dec(log, log_max, (uint64_t)cg->id);
        log_append(log, log_max, "\n");
    }

    // 7. Seccomp.
    seccomp_filter_t* sc = seccomp_profile(spec->seccomp_profile);
    if (sc) {
        if (t->seccomp) seccomp_release(t->seccomp);
        t->seccomp = sc;     // refcount уже =1 от seccomp_profile
        log_append(log, log_max, "[box] seccomp profile '");
        log_append(log, log_max, spec->seccomp_profile);
        log_append(log, log_max, "' (");
        log_dec(log, log_max, (uint64_t)sc->n_allow);
        log_append(log, log_max, " allow rules)\n");
    }

    // 8. Регистрируем box-record.
    box_record_add(t->tid, vpid, cg, sc, spec->name, spec->path);

    log_append(log, log_max, "[ok] box spawned tid=");
    log_dec(log, log_max, (uint64_t)t->tid);
    log_append(log, log_max, "\n");
    return t->tid;
}

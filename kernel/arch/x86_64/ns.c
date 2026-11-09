// ============================================================================
// ns.c — реализация namespaces для AMPLIFY-OS x86_64 (этап 46).
// ----------------------------------------------------------------------------
// Архитектурные решения:
//
//  1. Все namespace-объекты живут в kernel-heap, аллоцируются через kmalloc.
//     Refcount обнуляется только в ns_release; объект тут же kfree'ится.
//
//  2. ns_set_t — лёгкая обёртка из 5 указателей. У root всегда refcount==1
//     даже когда ни одна задача его не держит явно (kernel-side smoke).
//
//  3. unshare(flags) копирует только те namespace'ы, флаги которых
//     установлены, остальные retain'ит. Это даёт CoW-семантику Linux.
//
//  4. Видимый PID: вместо полноценного per-ns map<vid,task*> мы вычисляем
//     vid_pid лениво — при ns_pid_assign выдаём следующий идентификатор
//     pid_ns->next_pid и записываем его в task_t->ns_pid_visible. Сами
//     задачи продолжают идентифицироваться глобальным tid (для kernel
//     ядро не теряет уникальность).  Это нарушает SETT-семантику Linux
//     (где tid внутри ns тоже маленький), но достаточно для песочницы.
//
//  5. Mount-namespace: фильтр работает только на уровне vfs_lookup и
//     fat32_*. Для box-режима мы заранее регистрируем "/box/<id>" как
//     allow-префикс. Полноценный bind-mount (одна и та же inode под
//     разными путями) требует переписи VFS — отложено на этап 51.
// ============================================================================
#include "ns.h"
#include "task.h"
#include "heap.h"
#include "serial.h"

// ---- Локальные хелперы ----------------------------------------------------
static int s_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static int s_strncmp(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}
static void s_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// ---- Глобальное состояние -------------------------------------------------
static ns_set_t*  g_root          = 0;
static int        g_next_ns_id    = 1;

// Реестр всех ns-объектов (для ns_dump_all и ns_get_summaries) — обновляется
// при register/unregister. Простой массив на 64 объекта (увеличить = +kmem).
#define NS_REG_MAX  64
typedef struct {
    int   kind;        // 0..4 как в ns_summary_t
    void* obj;
} ns_registry_entry_t;
static ns_registry_entry_t g_reg[NS_REG_MAX];
static int g_reg_count = 0;

static void ns_register(int kind, void* obj) {
    if (g_reg_count >= NS_REG_MAX) return;
    g_reg[g_reg_count].kind = kind;
    g_reg[g_reg_count].obj  = obj;
    g_reg_count++;
}

static void ns_unregister(void* obj) {
    for (int i = 0; i < g_reg_count; i++) {
        if (g_reg[i].obj == obj) {
            // shift остаток
            for (int j = i; j < g_reg_count - 1; j++) g_reg[j] = g_reg[j+1];
            g_reg_count--;
            return;
        }
    }
}

// ---- Создание/уничтожение конкретных namespace'ов -------------------------

static pid_ns_t* pidns_create(pid_ns_t* parent) {
    pid_ns_t* p = (pid_ns_t*)kmalloc(sizeof(pid_ns_t));
    if (!p) return 0;
    p->ns_id     = g_next_ns_id++;
    p->refcount  = 1;
    p->next_pid  = 1;
    p->max_pids  = 64;
    p->pid_count = 0;
    p->parent    = parent;
    ns_register(0, p);
    return p;
}

static void pidns_destroy(pid_ns_t* p) {
    if (!p) return;
    ns_unregister(p);
    kfree(p);
}

static uts_ns_t* utsns_create(const char* hostname, const char* domain) {
    uts_ns_t* p = (uts_ns_t*)kmalloc(sizeof(uts_ns_t));
    if (!p) return 0;
    p->ns_id    = g_next_ns_id++;
    p->refcount = 1;
    s_strncpy(p->hostname,   hostname, 64);
    s_strncpy(p->domainname, domain,   64);
    ns_register(1, p);
    return p;
}

static void utsns_destroy(uts_ns_t* p) {
    if (!p) return;
    ns_unregister(p);
    kfree(p);
}

static mnt_ns_t* mntns_create(bool isolated) {
    mnt_ns_t* p = (mnt_ns_t*)kmalloc(sizeof(mnt_ns_t));
    if (!p) return 0;
    p->ns_id    = g_next_ns_id++;
    p->refcount = 1;
    p->n_allow  = 0;
    p->isolated = isolated;
    for (int i = 0; i < MNT_NS_MAX_RULES; i++) p->allow[i][0] = 0;
    ns_register(2, p);
    return p;
}

static void mntns_destroy(mnt_ns_t* p) {
    if (!p) return;
    ns_unregister(p);
    kfree(p);
}

static net_ns_t* netns_create(bool isolated) {
    net_ns_t* p = (net_ns_t*)kmalloc(sizeof(net_ns_t));
    if (!p) return 0;
    p->ns_id        = g_next_ns_id++;
    p->refcount     = 1;
    p->isolated     = isolated;
    p->pkts_blocked = 0;
    ns_register(3, p);
    return p;
}

static void netns_destroy(net_ns_t* p) {
    if (!p) return;
    ns_unregister(p);
    kfree(p);
}

static user_ns_t* userns_create(uint32_t uid, uint32_t gid) {
    user_ns_t* p = (user_ns_t*)kmalloc(sizeof(user_ns_t));
    if (!p) return 0;
    p->ns_id       = g_next_ns_id++;
    p->refcount    = 1;
    p->inside_uid  = uid;
    p->inside_gid  = gid;
    p->outside_uid = uid;
    p->outside_gid = gid;
    ns_register(4, p);
    return p;
}

static void userns_destroy(user_ns_t* p) {
    if (!p) return;
    ns_unregister(p);
    kfree(p);
}

// ---- ns_set_t -------------------------------------------------------------

static ns_set_t* nsset_create_empty(void) {
    ns_set_t* s = (ns_set_t*)kmalloc(sizeof(ns_set_t));
    if (!s) return 0;
    s->refcount = 1;
    s->pid  = 0;
    s->uts  = 0;
    s->mnt  = 0;
    s->net  = 0;
    s->user = 0;
    return s;
}

static void nsset_destroy(ns_set_t* s) {
    if (!s) return;
    if (s->pid)  { s->pid->refcount--;  if (s->pid->refcount  <= 0) pidns_destroy(s->pid);   }
    if (s->uts)  { s->uts->refcount--;  if (s->uts->refcount  <= 0) utsns_destroy(s->uts);   }
    if (s->mnt)  { s->mnt->refcount--;  if (s->mnt->refcount  <= 0) mntns_destroy(s->mnt);   }
    if (s->net)  { s->net->refcount--;  if (s->net->refcount  <= 0) netns_destroy(s->net);   }
    if (s->user) { s->user->refcount--; if (s->user->refcount <= 0) userns_destroy(s->user); }
    kfree(s);
}

ns_set_t* ns_retain(ns_set_t* s) {
    if (!s) return 0;
    s->refcount++;
    return s;
}

void ns_release(ns_set_t* s) {
    if (!s) return;
    s->refcount--;
    if (s->refcount <= 0) nsset_destroy(s);
}

// ---- Инициализация --------------------------------------------------------
void ns_init(void) {
    if (g_root) return;
    g_root          = nsset_create_empty();
    if (!g_root) {
        serial_puts("[ns] ns_init: out of memory!\n");
        return;
    }
    g_root->pid     = pidns_create(0);          // root pid_ns без родителя
    g_root->uts     = utsns_create("amplify-os", "local");
    g_root->mnt     = mntns_create(false);      // root mount-ns не isolated
    g_root->net     = netns_create(false);      // host net
    g_root->user    = userns_create(0, 0);      // root uid/gid

    serial_puts("[ns] root namespaces created (PID/UTS/MNT/NET/USER)\n");
}

ns_set_t* ns_root(void) { return g_root; }

// ---- unshare --------------------------------------------------------------
ns_set_t* ns_unshare(ns_set_t* base, uint64_t flags) {
    if (!base) base = g_root;
    if (!(flags & AOS_CLONE_ALL_NS)) {
        // Ничего не unshare'им — просто retain.
        return ns_retain(base);
    }

    ns_set_t* nw = nsset_create_empty();
    if (!nw) return 0;

    // PID
    if (flags & AOS_CLONE_NEWPID) {
        nw->pid = pidns_create(base->pid);
    } else {
        nw->pid = base->pid;
        if (nw->pid) nw->pid->refcount++;
    }
    // UTS
    if (flags & AOS_CLONE_NEWUTS) {
        nw->uts = utsns_create(base->uts ? base->uts->hostname   : "box",
                               base->uts ? base->uts->domainname : "local");
    } else {
        nw->uts = base->uts;
        if (nw->uts) nw->uts->refcount++;
    }
    // MOUNT
    if (flags & AOS_CLONE_NEWNS) {
        nw->mnt = mntns_create(true);   // isolated, allow-список заполняется box_run
    } else {
        nw->mnt = base->mnt;
        if (nw->mnt) nw->mnt->refcount++;
    }
    // NET
    if (flags & AOS_CLONE_NEWNET) {
        nw->net = netns_create(true);   // изолирован
    } else {
        nw->net = base->net;
        if (nw->net) nw->net->refcount++;
    }
    // USER
    if (flags & AOS_CLONE_NEWUSER) {
        // Маппинг по умолчанию: внутри = 1000:1000, снаружи копируем base.
        nw->user = userns_create(1000, 1000);
        if (nw->user && base->user) {
            nw->user->outside_uid = base->user->inside_uid;
            nw->user->outside_gid = base->user->inside_gid;
        }
    } else {
        nw->user = base->user;
        if (nw->user) nw->user->refcount++;
    }
    return nw;
}

// ---- Видимый PID ----------------------------------------------------------
// task_t хранит только глобальный tid; видимый pid внутри pid_ns живёт в
// специальном поле task_t.ns_pid_visible (см. task.h). Если задача в root-ns
// — возвращаем tid as-is.
int ns_visible_pid(struct task* t) {
    if (!t) return -1;
    if (!t->ns || t->ns == g_root) return t->tid;
    if (t->ns_pid_visible > 0) return t->ns_pid_visible;
    // Не назначен — это происходит только при первой проверке.
    return t->tid;
}

bool ns_pid_acquire(pid_ns_t* ns) {
    if (!ns) return true;
    if (ns->max_pids > 0 && ns->pid_count >= ns->max_pids) return false;
    ns->pid_count++;
    return true;
}

void ns_pid_release(pid_ns_t* ns) {
    if (!ns) return;
    if (ns->pid_count > 0) ns->pid_count--;
}

int ns_pid_assign(struct task* t) {
    if (!t || !t->ns) return -1;
    pid_ns_t* p = t->ns->pid;
    if (!p) {
        t->ns_pid_visible = t->tid;
        return t->tid;
    }
    if (!ns_pid_acquire(p)) {
        t->ns_pid_visible = -1;
        return -1;
    }
    int v = p->next_pid++;
    t->ns_pid_visible = v;
    return v;
}

bool ns_can_see_task(struct task* viewer, struct task* target) {
    if (!viewer || !target) return false;
    if (!viewer->ns || !target->ns) return true;
    if (viewer->ns == target->ns) return true;

    pid_ns_t* vp = viewer->ns->pid;
    pid_ns_t* tp = target->ns->pid;
    if (!vp || !tp) return true;
    // Если viewer находится в pid_ns, который является родителем (или родителем
    // родителя) tp — может видеть. В обратную сторону — нет.
    pid_ns_t* p = tp;
    while (p) {
        if (p == vp) return true;
        p = p->parent;
    }
    return false;
}

// ---- Net allowed ----------------------------------------------------------
bool ns_net_allowed(struct task* t) {
    if (!t || !t->ns || !t->ns->net) return true;
    if (!t->ns->net->isolated) return true;
    t->ns->net->pkts_blocked++;
    return false;
}

// ---- Mount allowed --------------------------------------------------------
bool ns_mount_path_allowed(struct task* t, const char* path) {
    if (!t || !t->ns || !t->ns->mnt) return true;
    mnt_ns_t* m = t->ns->mnt;
    if (!m->isolated) return true;
    if (!path) return false;
    for (int i = 0; i < m->n_allow; i++) {
        int len = s_strlen(m->allow[i]);
        if (len == 0) continue;
        if (s_strncmp(path, m->allow[i], len) == 0) return true;
    }
    return false;
}

// ---- Hostname / sethostname ----------------------------------------------
const char* ns_hostname(struct task* t) {
    if (!t || !t->ns || !t->ns->uts) return "amplify-os";
    return t->ns->uts->hostname;
}

int ns_sethostname(struct task* t, const char* name, int len) {
    if (!t || !t->ns || !t->ns->uts) return -1;
    // Запрещаем менять hostname в root-ns (только в собственной копии).
    if (t->ns == g_root) return -1;
    if (len <= 0 || len >= 64) return -1;
    int i = 0;
    for (; i < len && name[i]; i++) t->ns->uts->hostname[i] = name[i];
    t->ns->uts->hostname[i] = 0;
    return 0;
}

// [этап 57] Hostname машины целиком (root UTS-ns).  В отличие от
// ns_sethostname() это не syscall-путь, а загрузочная конфигурация: имя,
// заданное в установщике, читается из /disk/Sanctum/system.conf.  Боксы,
// созданные после этого, наследуют его через CoW-копию UTS-ns.
int ns_set_root_hostname(const char* name) {
    if (!g_root || !g_root->uts || !name || !name[0]) return -1;
    int i = 0;
    for (; i < 63 && name[i]; i++) g_root->uts->hostname[i] = name[i];
    g_root->uts->hostname[i] = 0;
    return 0;
}

int ns_mnt_allow(struct task* t, const char* prefix) {
    if (!t || !t->ns || !t->ns->mnt) return -1;
    if (t->ns == g_root) return -1;
    mnt_ns_t* m = t->ns->mnt;
    if (m->n_allow >= MNT_NS_MAX_RULES) return -1;
    s_strncpy(m->allow[m->n_allow], prefix, MNT_NS_PATH_LEN);
    m->n_allow++;
    m->isolated = true;
    return 0;
}

// ---- Visible UID ----------------------------------------------------------
uint32_t ns_visible_uid(struct task* t) {
    if (!t || !t->ns || !t->ns->user) return 0;
    return t->ns->user->inside_uid;
}

// ---- Дамп / summaries -----------------------------------------------------
void ns_dump_all(void) {
    serial_puts("[ns] dump (");
    serial_print_dec((uint64_t)g_reg_count);
    serial_puts(" objects):\n");
    for (int i = 0; i < g_reg_count; i++) {
        const char* kind_s = "?";
        int extra = 0;
        switch (g_reg[i].kind) {
        case 0: kind_s = "PID";  extra = ((pid_ns_t*)g_reg[i].obj)->pid_count;    break;
        case 1: kind_s = "UTS";  extra = 0;                                      break;
        case 2: kind_s = "MNT";  extra = ((mnt_ns_t*)g_reg[i].obj)->n_allow;     break;
        case 3: kind_s = "NET";  extra = (int)((net_ns_t*)g_reg[i].obj)->pkts_blocked; break;
        case 4: kind_s = "USER"; extra = (int)((user_ns_t*)g_reg[i].obj)->inside_uid;  break;
        }
        serial_puts("  ns ");
        serial_puts(kind_s);
        serial_puts(" extra=");
        serial_print_dec((uint64_t)extra);
        serial_puts("\n");
    }
}

int ns_get_summaries(ns_summary_t* out, int max) {
    int n = 0;
    for (int i = 0; i < g_reg_count && n < max; i++) {
        out[n].ns_id   = 0;
        out[n].kind    = g_reg[i].kind;
        out[n].refcount = 0;
        out[n].extra   = 0;
        switch (g_reg[i].kind) {
        case 0: {
            pid_ns_t* p = (pid_ns_t*)g_reg[i].obj;
            out[n].ns_id    = p->ns_id;
            out[n].refcount = p->refcount;
            out[n].extra    = p->pid_count;
            break;
        }
        case 1: {
            uts_ns_t* p = (uts_ns_t*)g_reg[i].obj;
            out[n].ns_id    = p->ns_id;
            out[n].refcount = p->refcount;
            break;
        }
        case 2: {
            mnt_ns_t* p = (mnt_ns_t*)g_reg[i].obj;
            out[n].ns_id    = p->ns_id;
            out[n].refcount = p->refcount;
            out[n].extra    = p->n_allow;
            break;
        }
        case 3: {
            net_ns_t* p = (net_ns_t*)g_reg[i].obj;
            out[n].ns_id    = p->ns_id;
            out[n].refcount = p->refcount;
            out[n].extra    = (int)p->pkts_blocked;
            break;
        }
        case 4: {
            user_ns_t* p = (user_ns_t*)g_reg[i].obj;
            out[n].ns_id    = p->ns_id;
            out[n].refcount = p->refcount;
            out[n].extra    = (int)p->inside_uid;
            break;
        }
        }
        n++;
    }
    return n;
}

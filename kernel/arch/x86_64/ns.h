// ============================================================================
// ns.h — namespaces для AMPLIFY-OS x86_64 (этап 46).
// ----------------------------------------------------------------------------
// Минималистичная реализация Linux-style namespaces. Каждая подсистема
// (PID, UTS, mount, net, user) изолируется отдельным объектом, а task_t
// держит «набор пяти ссылок» в виде ns_set_t. Семантика — refcount-based,
// CoW: при unshare(CLONE_NEW*) для соответствующего namespace создаётся
// новая копия, refcount предыдущей декрементируется.
//
// ВАЖНО: на данном этапе ИСПОЛЬЗУЕМ namespace'ы только для ВИДИМОСТИ
// (фильтрация в matrix/scan/netif), не для полной аппаратной изоляции
// (для этого нужны отдельные page-tables — есть, но не разнесены на ns).
// Этого достаточно, чтобы недоверенная программа в `box run` НЕ ВИДЕЛА
// чужие процессы, чужие mount'ы и не имела доступа к сети.
//
// Конструкция выбрана с прицелом на этап 50 (VT-x): в гипервизоре каждая
// гостевая ОС будет жить в собственном net-namespace, плюс получит свой
// EPT — как реальное аппаратное продолжение этой логики.
// ============================================================================
#ifndef AOS_X86_64_NS_H
#define AOS_X86_64_NS_H

#include <stdint.h>
#include <stdbool.h>

// Forward-decl: struct task — определена в task.h. Здесь нужна для ссылок
// в API (без неё C создаёт «локальный» struct tag в каждом прототипе и
// получаются конфликтующие типы при компиляции ns.c).
struct task;

// ---- Флаги CLONE/UNSHARE — совпадают с Linux kernel.h по битам ------------
// Отдельный namespace для каждой подсистемы создаётся, если установлен бит:
#define AOS_CLONE_NEWPID    0x20000000
#define AOS_CLONE_NEWNS     0x00020000   /* mount namespace */
#define AOS_CLONE_NEWUTS    0x04000000
#define AOS_CLONE_NEWNET    0x40000000
#define AOS_CLONE_NEWUSER   0x10000000
#define AOS_CLONE_ALL_NS    (AOS_CLONE_NEWPID | AOS_CLONE_NEWNS  | \
                             AOS_CLONE_NEWUTS | AOS_CLONE_NEWNET | \
                             AOS_CLONE_NEWUSER)

// Дополнительные биты для CLONE-syscall (этап 40 group D уже определил
// семантику CLONE_THREAD внутри clone_sys_create через RDX=tls). Сейчас
// просто резервируем номера, чтобы не пересекаться:
#define AOS_CLONE_THREAD     0x00010000   /* (этап 40, уже используется) */
#define AOS_CLONE_VM         0x00000100   /* (этап 40) */
#define AOS_CLONE_FS         0x00000200
#define AOS_CLONE_FILES      0x00000400

// ---- PID-namespace --------------------------------------------------------
// Каждое pid_ns_t хранит собственное отображение «реальный tid → видимый pid».
// При создании дочернего ns через unshare у него initial_pid=1 (init).
typedef struct pid_ns {
    int             ns_id;              // глобальный ID для отладки
    int             refcount;
    int             next_pid;           // следующий pid внутри этого ns (1..)
    int             max_pids;           // лимит, 0 = неограниченно (стандартно: 64)
    int             pid_count;          // сейчас живых
    struct pid_ns*  parent;             // родительский ns (для traversал)
    // Маппинг tid → vid_pid реализуется через task_t.ns_pid, см. ns.c.
} pid_ns_t;

// ---- UTS-namespace --------------------------------------------------------
typedef struct uts_ns {
    int   ns_id;
    int   refcount;
    char  hostname[64];
    char  domainname[64];
} uts_ns_t;

// ---- Mount-namespace ------------------------------------------------------
// Очень простая модель: «список разрешённых path-префиксов». Если задан хотя
// бы один префикс — VFS-ops внутри этого ns возвращают ENOENT для путей вне
// списка. Полноценный bind-mount/private /tmp реализуется как private-prefix
// "/box/<id>/" поверх FAT32.
#define MNT_NS_MAX_RULES   8
#define MNT_NS_PATH_LEN   128
typedef struct mnt_ns {
    int   ns_id;
    int   refcount;
    char  allow[MNT_NS_MAX_RULES][MNT_NS_PATH_LEN];
    int   n_allow;
    bool  isolated;          // если true — действует фильтр allow[]
} mnt_ns_t;

// ---- Net-namespace --------------------------------------------------------
// На этапе 46 модель элементарная: сетевая видимость = bool. По умолчанию
// «host-net». Если процесс в собственном net-ns (isolated=true), system-call'ы
// SOCKET/CONNECT/SEND etc. возвращают EACCES (loopback-only TODO @ этап 50).
typedef struct net_ns {
    int      ns_id;
    int      refcount;
    bool     isolated;       // если true — нет доступа к default netif
    uint32_t pkts_blocked;   // счётчик дропнутых попыток (для cgroup статистики)
} net_ns_t;

// ---- User-namespace -------------------------------------------------------
// Простой UID/GID-маппинг. Для песочницы достаточно «inside ns: uid=1000»,
// что бы внутри было видно как обычный пользователь.
typedef struct user_ns {
    int      ns_id;
    int      refcount;
    uint32_t inside_uid;     // uid внутри ns (для getuid)
    uint32_t inside_gid;
    uint32_t outside_uid;    // uid снаружи (исходный)
    uint32_t outside_gid;
} user_ns_t;

// ---- Набор пяти ссылок на namespace-объекты ------------------------------
// Прикрепляется к task_t через указатель. Один и тот же ns_set_t могут
// держать несколько задач (потоки, дети без unshare).
typedef struct ns_set {
    int        refcount;
    pid_ns_t*  pid;
    uts_ns_t*  uts;
    mnt_ns_t*  mnt;
    net_ns_t*  net;
    user_ns_t* user;
} ns_set_t;

// ---- Public API -----------------------------------------------------------
// Один раз при boot: создаёт «корневые» namespace'ы (host-namespaces).
void       ns_init(void);

// Корневой ns_set_t (всегда не-NULL после ns_init).
ns_set_t*  ns_root(void);

// Refcount: ns_retain возвращает тот же указатель (ничего не аллоцирует).
ns_set_t*  ns_retain(ns_set_t* s);
void       ns_release(ns_set_t* s);

// unshare — создать копии тех namespace'ов, для которых установлены биты.
// Возвращает НОВЫЙ ns_set_t (с refcount=1); старый release'ится.
// Если ни один CLONE_NEW* не запрошен — возвращает retain(s).
ns_set_t*  ns_unshare(ns_set_t* base, uint64_t flags);

// Получить «видимый» PID для текущей задачи. tid — реальный из task_t.
// Если у задачи нет своего pid_ns (= root) — возвращает tid as-is.
int        ns_visible_pid(struct task* t);

// Можно ли видеть таску «target» из контекста «viewer»?
// Правило: если viewer.pid_ns == target.pid_ns ИЛИ если viewer.pid_ns
// является родителем target.pid_ns — можно. Иначе нет.
bool       ns_can_see_task(struct task* viewer, struct task* target);

// Net: можно ли создавать сокеты в текущем ns? Проверяется в sock_sys_*.
bool       ns_net_allowed(struct task* t);

// Mount: разрешён ли путь? Если ns.mnt.isolated == true — путь должен
// начинаться с одного из allow[]. Всегда true в root-ns.
bool       ns_mount_path_allowed(struct task* t, const char* path);

// UTS: hostname для текущей задачи (UTF-8 строка, не-NULL).
const char* ns_hostname(struct task* t);

// User: видимые uid/gid (для AOS_SYS_UID).
uint32_t   ns_visible_uid(struct task* t);

// Inkr/decr количества процессов в pid_ns. Используется task_create_*.
// Возвращает true если успешно (не превышен max_pids).
bool       ns_pid_acquire(pid_ns_t* ns);
void       ns_pid_release(pid_ns_t* ns);

// Назначение pid_ns конкретной задаче (alloc'ит видимый pid).
// Вызывается из task_create_user после установки t->ns.
int        ns_pid_assign(struct task* t);

// Установить hostname в UTS-ns текущей задачи. Возвращает 0 при успехе.
// При isolated=root возвращает -EPERM (нельзя менять глобальный hostname).
int        ns_sethostname(struct task* t, const char* name, int len);

// [этап 57] Установить hostname машины (root UTS-ns) из загрузочной
// конфигурации. Возвращает 0 при успехе.
int        ns_set_root_hostname(const char* name);

// Добавить allow-prefix в mnt-ns текущей задачи. -EPERM в root-ns.
int        ns_mnt_allow(struct task* t, const char* prefix);

// Печать состояния всех ns-объектов (для shell-команды `ns list`).
void       ns_dump_all(void);

// ---- Структуры для отладочной/телеметрической выгрузки -------------------
typedef struct ns_summary {
    int ns_id;
    int kind;          // 0=PID 1=UTS 2=MNT 3=NET 4=USER
    int refcount;
    int extra;         // PID: pid_count, MNT: n_allow, NET: pkts_blocked
} ns_summary_t;

int        ns_get_summaries(ns_summary_t* out, int max);

#endif

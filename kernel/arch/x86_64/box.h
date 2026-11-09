// ============================================================================
// box.h — высокоуровневая обёртка для запуска программ в песочнице.
// ----------------------------------------------------------------------------
// Используется и shell-командой `box run`, и GUI Sandbox Manager (app_box.c).
// Объединяет конфигурацию ns/cgroup/seccomp в одну структуру и применяет её
// к новой задаче.
//
//  box_spec_t {
//      char path[128]           — путь к ELF (через VFS)
//      uint64_t clone_flags     — какие namespace'ы создать
//      const char* seccomp_prof — имя профиля seccomp
//      uint64_t mem_max         — RAM лимит, 0 = no limit
//      uint64_t cpu_max_ns      — CPU лимит в наносекундах за период
//      uint64_t cpu_period_ns
//      uint64_t io_max          — IO лимит
//      int      pids_max
//      char    private_root[64] — будущая корневая (mount-prefix), может быть пустым
//  }
//
// box_run(spec) → tid новой задачи (>0) или -1.  Запуск асинхронный —
// shell может сразу вернуться к prompt'у; задача начнёт выполняться при
// следующем task_yield.
// ============================================================================
#ifndef AOS_X86_64_BOX_H
#define AOS_X86_64_BOX_H

#include <stdint.h>
#include <stdbool.h>

#define BOX_PATH_LEN     128
#define BOX_LOG_LEN      4096

typedef struct box_spec {
    char     path[BOX_PATH_LEN];           // ELF в VFS
    uint64_t clone_flags;                  // CLONE_NEW* битмаска
    char     seccomp_profile[16];          // "default"/"strict"/"net"/"compute"
    uint64_t mem_max;
    uint64_t cpu_max_ns;
    uint64_t cpu_period_ns;
    uint64_t io_max;
    int      pids_max;
    char     private_root[64];             // строится автоматически если пустой
    char     hostname[64];
    char     name[32];                     // имя box'а для логов и cgroup
} box_spec_t;

// Запуск — возвращает tid новой задачи или отрицательный код ошибки.
// log буфер — для отладочного вывода, куда box_run пишет шаги: «namespace
// created», «seccomp default applied», «task tid=N spawned».
int  box_run(const box_spec_t* spec, char* log, int log_max);

// Список запущенных box'ов (для GUI и shell box list).
typedef struct box_listing {
    int      tid;
    int      visible_pid;
    int      cgroup_id;
    char     name[32];
    char     path[64];
    uint64_t mem_used;
    uint64_t cpu_total_ns;
    int      seccomp_blocked;
} box_listing_t;

int  box_list(box_listing_t* out, int max);

// Хелпер: построить spec по умолчанию.
void box_spec_default(box_spec_t* spec, const char* path, const char* name);

#endif

// ============================================================================
// security.c - Точка входа подсистемы безопасности (Этап 32)
// ============================================================================
// Реализует:
//   - security_run() — единственная функция, которую дёргает kernel.c.
//   - sec_session_*  — управление активной сессией.
//   - sec_ring3_handle_fault() — хук на CPU-исключения в Ring 3, чтобы
//     краш userspace не утаскивал ядро.
// ============================================================================

#include "security.h"
#include "../debug_serial.h"
#include "../fs/vfs.h"
#include "../task/task.h"
#include "../memory/memory.h"

// Внешние утилиты:
extern void dbg_puts(const char* s);

// ----------------------------------------------------------------------------
// Глобальное состояние сессии. Один пользователь — один сеанс работы ОС.
// ----------------------------------------------------------------------------

static sec_session_t g_session = {0};

// Кэшируем флаг доступности диска: проверяется один раз при старте.
static bool g_disk_available = false;

// ----------------------------------------------------------------------------
// Маленькие строковые помощники (свои, чтобы не тащить libc в ядро)
// ----------------------------------------------------------------------------

static int s_strlen(const char* s) {
    int n = 0;
    while (s && s[n]) n++;
    return n;
}

static void s_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (i < max - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// ----------------------------------------------------------------------------
// Проверка доступности диска для сохранения БД пользователей.
// Стратегия: пытаемся stat'нуть монтпойнт /disk; если получилось — пробуем
// создать каталог /disk/Sanctum (если ещё нет). Если что-то из этого упало —
// диск считается недоступным и БД будет жить только в RAM.
// ----------------------------------------------------------------------------

bool security_disk_available(void) {
    return g_disk_available;
}

static bool probe_disk(void) {
    vfs_stat_info_t st;
    if (vfs_stat("/disk", &st) != 0) {
        dbg_puts("[sec] /disk not mounted — DB will live in RAM only\n");
        return false;
    }
    // Пробуем создать каталог /disk/Sanctum (сюда складываем БД и
    // файлы пользователей). Если FAT32-драйвер не умеет mkdir на
    // некоторых конфигурациях — это не критично: будем писать БД
    // в корень /disk. Сама БД-логика подхватит fallback-путь
    // автоматически (см. sec_users_save / sec_users_load).
    if (vfs_stat("/disk/Sanctum", &st) != 0) {
        int rc = vfs_mkdir("/disk/Sanctum");
        if (rc == 0) {
            dbg_puts("[sec] created /disk/Sanctum\n");
        } else {
            dbg_puts("[sec] /disk/Sanctum unavailable — using /disk root for DB\n");
        }
    }
    return true;
}

// ----------------------------------------------------------------------------
// security_run — главная точка входа. См. security.h.
// ----------------------------------------------------------------------------

void security_run(void) {
    dbg_puts("[sec] security_run begin\n");

    // 1. Подготовка БД: пытаемся прочитать её с диска.
    sec_users_init();
    g_disk_available = probe_disk();

    int loaded = -1;
    if (g_disk_available) {
        loaded = sec_users_load();
    }
    (void)loaded; // не критично — если файла нет, count == 0.

    // 2. Если пользователей нет — first-boot wizard.
    if (sec_users_count() == 0) {
        dbg_puts("[sec] no users yet — running first-boot wizard\n");
        sec_firstboot_run();
        // После firstboot БД содержит как минимум одного администратора.
    }

    // 3. Login screen — крутится, пока пользователь не введёт верный пароль.
    dbg_puts("[sec] entering login screen\n");
    const sec_user_t* u = sec_login_run();
    if (!u) {
        // sec_login_run по контракту не возвращает NULL, но на всякий случай:
        dbg_puts("[sec] login_run returned NULL — halting\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    // 4. Заполняем активную сессию.
    sec_session_set(u);
    dbg_puts("[sec] session opened — entering desktop\n");
}

// ----------------------------------------------------------------------------
// Сессия
// ----------------------------------------------------------------------------

void sec_session_set(const sec_user_t* u) {
    if (!u) {
        sec_session_clear();
        return;
    }
    g_session.active   = true;
    g_session.uid      = u->uid;
    g_session.gid      = u->gid;
    g_session.is_admin = (u->uid == SEC_UID_ROOT) ||
                         ((u->flags & SEC_UFLAG_ADMIN) != 0);
    s_strncpy(g_session.name,    u->name,    SEC_USERNAME_MAX + 1);
    s_strncpy(g_session.display, u->display, SEC_DISPLAYNAME_MAX + 1);
}

void sec_session_clear(void) {
    g_session.active   = false;
    g_session.uid      = 0;
    g_session.gid      = 0;
    g_session.is_admin = false;
    g_session.name[0]    = '\0';
    g_session.display[0] = '\0';
}

const sec_session_t* sec_get_session(void) {
    return &g_session;
}

void sec_logout(void) {
    sec_session_clear();
}

// ----------------------------------------------------------------------------
// Хук Ring 3 fault: вызывается из interrupt.c, если CS.RPL == 3.
// ВАЖНО: мы не возвращаемся обратно в исходный кадр — вместо этого мы
// помечаем текущую задачу мёртвой и зацикливаемся на sti/hlt, давая
// планировщику переключить контекст на следующую задачу. Это безопасно,
// потому что ядро уже на kernel-стеке, а память упавшего приложения нам
// больше не нужна.
// ----------------------------------------------------------------------------

bool sec_ring3_handle_fault(int int_no, uint32_t err_code,
                            uint32_t eip, uint32_t cr2) {
    task_t* t = task_get_current();
    if (!t) {
        // Multitasking ещё не инициализирован — пускай штатный handler
        // решает, что делать (и halt'ит систему). Мы не лезем.
        return false;
    }

    // Лог в serial для разбора.
    dbg_puts("[sec] Ring 3 fault: ");
    char line[160];
    int  n = 0;
    static const char* names[] = {
        "DivByZero", "Debug", "NMI", "Breakpoint", "Overflow", "BoundRange",
        "InvalidOpcode", "DevNotAvail", "DoubleFault", "CoprocOverrun",
        "InvalidTSS", "SegNotPresent", "StackSegFault", "GeneralProtection",
        "PageFault", "Reserved15", "x87FPU", "AlignCheck", "MachineCheck",
        "SIMD", "Virtualization"
    };
    const char* nm = (int_no >= 0 && int_no < (int)(sizeof(names)/sizeof(names[0])))
                     ? names[int_no] : "Unknown";
    for (int i = 0; nm[i] && n < 60; i++) line[n++] = nm[i];
    line[n++] = ' ';
    // Печатаем int_no, err, eip, cr2 в HEX.
    static const char hex[] = "0123456789ABCDEF";
    line[n++] = '#';
    line[n++] = hex[(int_no >> 4) & 0xF];
    line[n++] = hex[int_no & 0xF];
    line[n++] = ' '; line[n++] = 'E'; line[n++] = 'R'; line[n++] = 'R'; line[n++] = '=';
    for (int i = 28; i >= 0; i -= 4) line[n++] = hex[(err_code >> i) & 0xF];
    line[n++] = ' '; line[n++] = 'E'; line[n++] = 'I'; line[n++] = 'P'; line[n++] = '=';
    for (int i = 28; i >= 0; i -= 4) line[n++] = hex[(eip >> i) & 0xF];
    line[n++] = ' '; line[n++] = 'C'; line[n++] = 'R'; line[n++] = '2'; line[n++] = '=';
    for (int i = 28; i >= 0; i -= 4) line[n++] = hex[(cr2 >> i) & 0xF];
    line[n++] = '\n';
    line[n] = '\0';
    dbg_puts(line);

    // Помечаем задачу мёртвой и крутимся в hlt'е до прерывания таймера —
    // там планировщик заберёт ESP следующей задачи.
    t->state = TASK_DEAD;
    while (1) {
        __asm__ volatile ("sti; hlt");
    }
    // unreachable
    return true;
}

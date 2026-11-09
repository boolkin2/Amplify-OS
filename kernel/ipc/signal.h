// ============================================================================
// signal.h — Сигналы для MyOS (Этап 29)
// ----------------------------------------------------------------------------
// Минимальная POSIX-like система сигналов, адаптированная под учебный
// kernel без preemptive multitasking. «Процесс» — это любая долгоиграющая
// задача shell'а, которой ставится пид через signal_register_task.
// Команды в циклах проверяют signal_pending(pid, SIG_INT|SIG_KILL) и
// сами останавливаются, если флаг поднят.
//
// Поддерживаемые сигналы:
//   SIG_INT  (2) — мягкая просьба прекратить работу (аналог Ctrl+C)
//   SIG_KILL (9) — безусловное завершение (нельзя проигнорировать)
//
// API:
//   signal_init()                     — инициализация таблицы задач.
//   pid = signal_register_task(name)  — регистрирует задачу, выдаёт pid.
//   signal_unregister(pid)            — убрать задачу из таблицы.
//   signal_send(pid, sig)             — поднять флаг.
//   signal_pending(pid, sig)          — проверить.
//   signal_clear(pid, sig)            — сбросить после обработки.
//   signal_list(out, max)             — получить snapshot таблицы
//                                        (для команды plumes).
//
// Команды shell'а:
//   plumes          — список задач (аналог ps).
//   hush <pid>      — SIG_INT (заменяет kill -INT).
//   smite <pid>     — SIG_KILL (заменяет kill -9).
// ============================================================================

#ifndef IPC_SIGNAL_H
#define IPC_SIGNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SIGNAL_MAX_TASKS     8
#define SIGNAL_TASK_NAME_LEN 32

#define SIG_INT  2
#define SIG_KILL 9

// Маски флагов, хранящиеся в task->signals.
#define SIG_MASK_INT   (1u << SIG_INT)
#define SIG_MASK_KILL  (1u << SIG_KILL)

typedef struct {
    bool     used;
    int      pid;
    char     name[SIGNAL_TASK_NAME_LEN];
    uint32_t signals;   // битовая маска pending сигналов
} signal_task_t;

void signal_init(void);

// Регистрация новой задачи. Возвращает pid (>=1) или -1 при переполнении.
int signal_register_task(const char* name);

// Снятие задачи с учёта. Обычно вызывается командой в конце работы.
void signal_unregister(int pid);

// Отправить сигнал конкретной задаче. Возвращает true при успехе.
bool signal_send(int pid, int sig);

// Есть ли у задачи неотработанный сигнал?
bool signal_pending(int pid, int sig);

// Сбросить сигнал после обработки.
void signal_clear(int pid, int sig);

// Snapshot таблицы. Копирует в out до max элементов (только used == true),
// возвращает реальное число скопированных.
int signal_list(signal_task_t* out, int max);

// Получить имя задачи по pid (NULL если нет такой).
const char* signal_task_name(int pid);

#endif // IPC_SIGNAL_H

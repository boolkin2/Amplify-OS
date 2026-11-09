// ============================================================================
// syscall.h - Системные вызовы (Syscalls) для MyOS (Этап 18)
// ============================================================================
// Расширенный набор системных вызовов для поддержки ELF userspace.
// Каждый syscall вызывается через int 0x80:
//   EAX = номер syscall
//   EBX = аргумент 1
//   ECX = аргумент 2
//   EDX = аргумент 3
//   ESI = аргумент 4
//   EDI = аргумент 5
//   EAX = возвращаемое значение
// ============================================================================

#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include "isr.h"

// ============================================================================
// Номера системных вызовов
// ============================================================================

// Управление процессом
#define SYS_EXIT                0   // Завершение процесса (ebx = код возврата)
#define SYS_GETPID              8   // Получение PID текущего процесса
#define SYS_SLEEP               9   // Сон (ebx = миллисекунды)

// GUI (графический интерфейс)
#define SYS_GUI_CREATE_WINDOW   1   // Создание окна
#define SYS_GUI_PUT_PIXEL       2   // Рисование пикселя
#define SYS_GUI_REDRAW          3   // Перерисовка экрана

// Файловые операции
#define SYS_WRITE               4   // Запись (ebx=fd, ecx=buf, edx=size)
#define SYS_READ                5   // Чтение (ebx=fd, ecx=buf, edx=size)
#define SYS_OPEN                6   // Открытие (ebx=path, ecx=flags)
#define SYS_CLOSE               7   // Закрытие (ebx=fd)

// Управление памятью
#define SYS_SBRK                10  // Увеличение кучи (ebx = increment)

// Информация о файлах
#define SYS_STAT                11  // Информация о файле (ebx=path, ecx=buf)

// Потоки (pthread-совместимый интерфейс)
#define SYS_CLONE               12  // Новый поток в том же vm (ebx=entry, ecx=stack, edx=arg)
#define SYS_JOIN                13  // Ожидание завершения потока (ebx=tid)

// Память (продолжение)
#define SYS_MMAP                14  // Отображение файла/анонимной памяти
#define SYS_MUNMAP              15  // Освобождение mmap-региона
#define SYS_SWAP_TEST           16  // Отладка: выгрузить страницу в swap

// Процессы
#define SYS_FORK                17  // POSIX fork(): COW-копия текущего процесса
#define SYS_EXECVE              18  // POSIX execve(path, argv, envp): замена образа

// Рабочий каталог
#define SYS_CHDIR               19  // chdir(path): сменить рабочий каталог
#define SYS_GETCWD              20  // getcwd(buf, size): скопировать cwd в буфер

// POSIX shared memory (System V API)
#define SYS_SHMGET              21  // shmget(key, size, flags): создать/найти
#define SYS_SHMAT               22  // shmat(id): прикрепить, вернуть user-VA
#define SYS_SHMDT               23  // shmdt(vaddr): отсоединить
#define SYS_SHMCTL              24  // shmctl(id, cmd): управление сегментом

// ============================================================================
// AMPLIFY-OS native syscalls (aos_*) — Этап 33 / persistence-fix
// ----------------------------------------------------------------------------
// Уникальный набор системных вызовов AMPLIFY-OS с префиксом aos_, чтобы
// отличать от POSIX-совместимого слоя выше. Номера 0x100+ не пересекаются
// с легаси-номерами 0..24.
//   eax = номер syscall, ebx/ecx/edx/esi = аргументы 1..4, eax = результат
//   Отрицательное значение (типа int) трактуется user-space'ом как -errno.
// ============================================================================
#define AOS_SYS_EXIT            0x100  // aos_exit(code)
#define AOS_SYS_WRITE           0x101  // aos_write(fd, buf, n)
#define AOS_SYS_READ            0x102  // aos_read(fd, buf, n)
#define AOS_SYS_OPEN            0x103  // aos_open(path, flags)
#define AOS_SYS_CLOSE           0x104  // aos_close(fd)
#define AOS_SYS_SEEK            0x105  // aos_seek(fd, offset, whence)
#define AOS_SYS_PID             0x106  // aos_pid() -> pid
#define AOS_SYS_UID             0x107  // aos_uid() -> uid активной сессии
#define AOS_SYS_NOW             0x108  // aos_now(buf64) -> мс с момента старта в *buf64
#define AOS_SYS_YIELD           0x109  // aos_yield() — отдать остаток кванта
#define AOS_SYS_SLEEP           0x10A  // aos_sleep(ns_lo, ns_hi) — наносекунды (округляются до мс)
#define AOS_SYS_BRK             0x10B  // aos_brk(new_break) -> 0 / -1 (нет реализации)
#define AOS_SYS_SBRK            0x10C  // aos_sbrk(delta) -> старая граница / -1

// ============================================================================
// Функции
// ============================================================================

// Инициализация диспетчера системных вызовов
void syscall_init(void);

#endif // SYSCALL_H

// ============================================================================
// aos_syscall.h — userspace-обвязка над AMPLIFY-OS syscall API
// ============================================================================
// Уникальные имена aos_* (Этап 33 / persistence-fix). Все функции — inline,
// чтобы в user-приложениях не требовалась отдельная сборка libsys.so.
//
// ABI (i386):
//   eax = номер syscall  (см. AOS_SYS_*)
//   ebx, ecx, edx, esi   = аргументы 1..4
//   eax                  = код возврата (>=0 — успех, <0 — -errno)
//
// Точка входа: int 0x80, gate с DPL=3, обрабатывается syscall_handler в ядре.
//
// ВНИМАНИЕ: значения номеров AOS_SYS_* должны 1-в-1 совпадать с ядром
// (kernel/arch/i386/syscall.h). Если меняешь — меняй в обоих местах.
// ============================================================================

#ifndef AOS_SYSCALL_H
#define AOS_SYSCALL_H

#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// Номера syscall'ов (зеркало kernel/arch/i386/syscall.h, AOS_SYS_*)
// ----------------------------------------------------------------------------
#define AOS_SYS_EXIT     0x100
#define AOS_SYS_WRITE    0x101
#define AOS_SYS_READ     0x102
#define AOS_SYS_OPEN     0x103
#define AOS_SYS_CLOSE    0x104
#define AOS_SYS_SEEK     0x105
#define AOS_SYS_PID      0x106
#define AOS_SYS_UID      0x107
#define AOS_SYS_NOW      0x108
#define AOS_SYS_YIELD    0x109
#define AOS_SYS_SLEEP    0x10A
#define AOS_SYS_BRK      0x10B
#define AOS_SYS_SBRK     0x10C

// ----------------------------------------------------------------------------
// Низкоуровневые стабы int 0x80
// ----------------------------------------------------------------------------
static inline int32_t _aos_sys0(uint32_t num) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}
static inline int32_t _aos_sys1(uint32_t num, uint32_t a1) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}
static inline int32_t _aos_sys2(uint32_t num, uint32_t a1, uint32_t a2) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2) : "memory");
    return ret;
}
static inline int32_t _aos_sys3(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}
static inline int32_t _aos_sys4(uint32_t num, uint32_t a1, uint32_t a2,
                                uint32_t a3, uint32_t a4) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret)
                      : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4) : "memory");
    return ret;
}

// ----------------------------------------------------------------------------
// whence-константы для aos_seek (совпадают с POSIX SEEK_SET/CUR/END)
// ----------------------------------------------------------------------------
#define AOS_SEEK_SET 0
#define AOS_SEEK_CUR 1
#define AOS_SEEK_END 2

// ----------------------------------------------------------------------------
// Высокоуровневые обёртки
// ----------------------------------------------------------------------------

// Завершить текущий процесс с кодом code. Не возвращается.
static inline void aos_exit(int code) {
    _aos_sys1(AOS_SYS_EXIT, (uint32_t)code);
    for (;;) { /* unreachable */ }
}

// Записать n байт из buf в дескриптор fd. fd=1 → stdout (VGA + COM1).
// Возврат: число записанных байт или <0 при ошибке.
static inline int aos_write(int fd, const void* buf, uint32_t n) {
    return (int)_aos_sys3(AOS_SYS_WRITE, (uint32_t)fd, (uint32_t)buf, n);
}

// Прочитать до n байт из дескриптора fd в buf. Возврат: число прочитанных байт.
static inline int aos_read(int fd, void* buf, uint32_t n) {
    return (int)_aos_sys3(AOS_SYS_READ, (uint32_t)fd, (uint32_t)buf, n);
}

// Открыть файл по абсолютному пути. flags — VFS_O_RDONLY/WRONLY/RDWR/CREAT/TRUNC.
// Возврат: fd (>=0) или <0 при ошибке.
static inline int aos_open(const char* path, uint32_t flags) {
    return (int)_aos_sys2(AOS_SYS_OPEN, (uint32_t)path, flags);
}

// Закрыть файловый дескриптор. Возврат: 0 / <0 при ошибке.
static inline int aos_close(int fd) {
    return (int)_aos_sys1(AOS_SYS_CLOSE, (uint32_t)fd);
}

// Сместить курсор файла. whence: AOS_SEEK_SET/CUR/END. Возврат: новая позиция.
static inline int aos_seek(int fd, int offset, int whence) {
    return (int)_aos_sys3(AOS_SYS_SEEK,
                          (uint32_t)fd, (uint32_t)offset, (uint32_t)whence);
}

// Идентификатор текущей задачи (аналог getpid).
static inline int aos_pid(void) {
    return (int)_aos_sys0(AOS_SYS_PID);
}

// Идентификатор пользователя активной сессии (аналог getuid).
static inline int aos_uid(void) {
    return (int)_aos_sys0(AOS_SYS_UID);
}

// Монотонное время в миллисекундах с момента старта системы.
// Если out != NULL — пишем 64-битное значение туда и возвращаем 0.
// Если out == NULL — возвращаем младшие 32 бита прямо в eax.
static inline int aos_now(uint64_t* out_ms) {
    return (int)_aos_sys1(AOS_SYS_NOW, (uint32_t)out_ms);
}

// Отдать остаток кванта времени планировщику.
static inline int aos_yield(void) {
    return (int)_aos_sys0(AOS_SYS_YIELD);
}

// Сон на ns наносекунд. Внутренне округляется до миллисекунд (PIT даёт мс).
static inline int aos_sleep(uint64_t ns) {
    uint32_t lo = (uint32_t)(ns & 0xFFFFFFFFULL);
    uint32_t hi = (uint32_t)(ns >> 32);
    return (int)_aos_sys2(AOS_SYS_SLEEP, lo, hi);
}

// Установить новый «верх кучи». Сейчас принимает только 0 (возвращает 0).
// Любой другой адрес — -1, т.к. per-process heap ещё не реализован.
static inline int aos_brk(void* new_break) {
    return (int)_aos_sys1(AOS_SYS_BRK, (uint32_t)new_break);
}

// Запросить delta байт из ядерной кучи. Возврат: указатель или (void*)-1.
static inline void* aos_sbrk(int delta) {
    int32_t r = _aos_sys1(AOS_SYS_SBRK, (uint32_t)delta);
    return (void*)(uintptr_t)r;
}

#endif // AOS_SYSCALL_H

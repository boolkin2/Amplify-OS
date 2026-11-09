// ============================================================================
// myos.h - Userspace SDK для MyOS (Этап 18)
// ============================================================================
// Заголовочный файл для пользовательских программ.
// Предоставляет удобные обёртки над системными вызовами (int 0x80).
//
// Пример использования:
//   #include "myos.h"
//
//   int main() {
//       myos_print("Hello, World!\n");
//       return 0;
//   }
// ============================================================================

#ifndef MYOS_H
#define MYOS_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Номера системных вызовов
// ============================================================================

#define SYS_EXIT                0   // Завершение процесса
#define SYS_GUI_CREATE_WINDOW   1   // Создание окна
#define SYS_GUI_PUT_PIXEL       2   // Рисование пикселя в окне
#define SYS_GUI_REDRAW          3   // Запрос перерисовки
#define SYS_WRITE               4   // Запись в файл/консоль
#define SYS_READ                5   // Чтение из файла
#define SYS_OPEN                6   // Открытие файла
#define SYS_CLOSE               7   // Закрытие файла
#define SYS_GETPID              8   // Получение PID
#define SYS_SLEEP               9   // Сон (в миллисекундах)
#define SYS_SBRK                10  // Увеличение кучи процесса
#define SYS_STAT                11  // Информация о файле
#define SYS_CLONE               12  // pthread-поток: entry, stack, arg
#define SYS_JOIN                13  // pthread_join: tid
#define SYS_MMAP                14  // Отображение файла/анонимной памяти
#define SYS_MUNMAP              15  // Освобождение mmap-региона
#define SYS_FORK                17  // POSIX fork(): COW-копия процесса
#define SYS_EXECVE              18  // POSIX execve(path, argv, envp)
#define SYS_CHDIR               19  // chdir(path)
#define SYS_GETCWD              20  // getcwd(buf, size)
#define SYS_SHMGET              21  // shmget(key, size, flags)
#define SYS_SHMAT               22  // shmat(id)
#define SYS_SHMDT               23  // shmdt(vaddr)
#define SYS_SHMCTL              24  // shmctl(id, cmd)

// Флаги для shmget/shmctl (должны совпадать с kernel/ipc/shm.h)
#define IPC_CREAT   0x200
#define IPC_EXCL    0x400
#define IPC_PRIVATE 0
#define IPC_RMID    0

// ============================================================================
// Инлайн-функции для системных вызовов через int 0x80
// ============================================================================

// Системный вызов без аргументов.
// Обязательно используем "memory" clobber: ядро может записать в
// любой указатель, который мы передали (например getcwd пишет в buf),
// и компилятор не должен оптимизировать чтение этих буферов.
static inline uint32_t _syscall0(uint32_t num) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num)
        : "memory"
    );
    return ret;
}

// Системный вызов с 1 аргументом
static inline uint32_t _syscall1(uint32_t num, uint32_t a1) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (a1)
        : "memory"
    );
    return ret;
}

// Системный вызов с 2 аргументами
static inline uint32_t _syscall2(uint32_t num, uint32_t a1, uint32_t a2) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (a1), "c" (a2)
        : "memory"
    );
    return ret;
}

// Системный вызов с 3 аргументами
static inline uint32_t _syscall3(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (a1), "c" (a2), "d" (a3)
        : "memory"
    );
    return ret;
}

// Системный вызов с 4 аргументами
static inline uint32_t _syscall4(uint32_t num, uint32_t a1, uint32_t a2,
                                  uint32_t a3, uint32_t a4) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (a1), "c" (a2), "d" (a3), "S" (a4)
        : "memory"
    );
    return ret;
}

// Системный вызов с 5 аргументами
static inline uint32_t _syscall5(uint32_t num, uint32_t a1, uint32_t a2,
                                  uint32_t a3, uint32_t a4, uint32_t a5) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (num), "b" (a1), "c" (a2), "d" (a3), "S" (a4), "D" (a5)
        : "memory"
    );
    return ret;
}

// ============================================================================
// API: Управление процессом
// ============================================================================

// Завершение текущего процесса
// Параметры:
//   exit_code — код завершения (0 = успех)
static inline void myos_exit(int exit_code) {
    _syscall1(SYS_EXIT, (uint32_t)exit_code);
    // На всякий случай — бесконечный цикл
    while (1) { __asm__ volatile(""); }
}

// Получение PID текущего процесса
static inline uint32_t myos_getpid(void) {
    return _syscall0(SYS_GETPID);
}

// POSIX fork(): создать дочерний процесс с COW-копией текущего vm.
// Возвращает:
//   в родителе — PID ребёнка (>0), или (uint32_t)-1 при ошибке;
//   в ребёнке  — 0.
// Оба процесса продолжают выполнение с точки, сразу после myos_fork().
static inline uint32_t myos_fork(void) {
    return _syscall0(SYS_FORK);
}

// POSIX execve(path, argv, envp): заменить образ текущего процесса на ELF
// по указанному пути в VFS. argv/envp пока не передаются (MyOS CRT0 использует
// сигнатуру main(void)) — ставьте NULL.
// Возвращает только при ОШИБКЕ: -1 (uint32_t).
// При успехе управление уходит в _start() нового ELF, возврата не будет.
static inline uint32_t myos_execve(const char* path,
                                    char* const argv[],
                                    char* const envp[]) {
    return _syscall3(SYS_EXECVE, (uint32_t)path, (uint32_t)argv, (uint32_t)envp);
}

// chdir(path): сменить рабочий каталог процесса.
// Возвращает 0 при успехе, (uint32_t)-1 при ошибке (путь не существует или
// не является каталогом).
static inline uint32_t myos_chdir(const char* path) {
    return _syscall1(SYS_CHDIR, (uint32_t)path);
}

// getcwd(buf, size): скопировать абсолютный путь cwd в buf (NUL-терминированный).
// Возвращает длину строки (без NUL) или (uint32_t)-1 если buf не хватает.
static inline uint32_t myos_getcwd(char* buf, uint32_t size) {
    return _syscall2(SYS_GETCWD, (uint32_t)buf, size);
}

// ============================================================================
// POSIX shared memory (System V API) — Стадия F
// ============================================================================

// shmget(key, size, flags): создать или получить shm-сегмент.
// Возвращает id (>0) или (uint32_t)-1 при ошибке.
static inline int myos_shmget(int key, uint32_t size, int flags) {
    return (int)_syscall3(SYS_SHMGET, (uint32_t)key, size, (uint32_t)flags);
}

// shmat(id): прикрепить сегмент, вернуть user-VA.
// При ошибке возвращает (void*)-1 — используйте явное сравнение.
static inline void* myos_shmat(int id) {
    return (void*)_syscall1(SYS_SHMAT, (uint32_t)id);
}

// shmdt(vaddr): отсоединить по адресу. Возвращает 0 или -1.
static inline int myos_shmdt(void* addr) {
    return (int)_syscall1(SYS_SHMDT, (uint32_t)addr);
}

// shmctl(id, cmd): управление. cmd == IPC_RMID — пометить на удаление.
static inline int myos_shmctl(int id, int cmd) {
    return (int)_syscall2(SYS_SHMCTL, (uint32_t)id, (uint32_t)cmd);
}

// Сон (пауза) на указанное количество миллисекунд
static inline void myos_sleep(uint32_t ms) {
    _syscall1(SYS_SLEEP, ms);
}

// ============================================================================
// API: Вывод текста
// ============================================================================

// Вычисление длины строки
static inline int myos_strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

// Вывод строки на консоль/экран
// Параметры:
//   str — строка для вывода (нуль-терминированная)
// Возвращает количество выведенных символов
static inline int myos_print(const char* str) {
    int len = myos_strlen(str);
    // fd=1 — stdout (консоль)
    return (int)_syscall3(SYS_WRITE, 1, (uint32_t)str, (uint32_t)len);
}

// Вывод одного символа
static inline void myos_putchar(char c) {
    char buf[2] = {c, '\0'};
    _syscall3(SYS_WRITE, 1, (uint32_t)buf, 1);
}

// Вывод целого числа в десятичном формате
static inline void myos_print_int(int value) {
    char buf[16];
    int pos = 0;
    int is_neg = 0;

    if (value < 0) {
        is_neg = 1;
        value = -value;
    }

    if (value == 0) {
        buf[pos++] = '0';
    } else {
        // Записываем цифры в обратном порядке
        char tmp[16];
        int tpos = 0;
        while (value > 0) {
            tmp[tpos++] = '0' + (value % 10);
            value /= 10;
        }
        if (is_neg) buf[pos++] = '-';
        // Разворачиваем
        for (int i = tpos - 1; i >= 0; i--) {
            buf[pos++] = tmp[i];
        }
    }
    buf[pos] = '\0';
    myos_print(buf);
}

// ============================================================================
// API: Работа с файлами
// ============================================================================

// Флаги открытия файла
#define MYOS_O_RDONLY   0x01
#define MYOS_O_WRONLY   0x02
#define MYOS_O_RDWR     0x03
#define MYOS_O_CREAT    0x08
#define MYOS_O_TRUNC    0x10

// Открытие файла
// Параметры:
//   path  — путь к файлу
//   flags — флаги открытия (MYOS_O_*)
// Возвращает файловый дескриптор (>= 0) или -1 при ошибке
static inline int myos_open(const char* path, int flags) {
    return (int)_syscall2(SYS_OPEN, (uint32_t)path, (uint32_t)flags);
}

// Чтение из файла
// Параметры:
//   fd     — файловый дескриптор
//   buffer — буфер для данных
//   size   — максимальный размер чтения
// Возвращает количество прочитанных байт или -1
static inline int myos_read(int fd, void* buffer, int size) {
    return (int)_syscall3(SYS_READ, (uint32_t)fd, (uint32_t)buffer, (uint32_t)size);
}

// Запись в файл
// Параметры:
//   fd     — файловый дескриптор
//   buffer — данные для записи
//   size   — размер данных
// Возвращает количество записанных байт или -1
static inline int myos_write(int fd, const void* buffer, int size) {
    return (int)_syscall3(SYS_WRITE, (uint32_t)fd, (uint32_t)buffer, (uint32_t)size);
}

// Закрытие файла
static inline int myos_close(int fd) {
    return (int)_syscall1(SYS_CLOSE, (uint32_t)fd);
}

// ============================================================================
// API: Управление памятью (простое)
// ============================================================================

// Увеличение программного break (аналог sbrk)
// Параметры:
//   increment — размер в байтах (0 = запрос текущего адреса)
// Возвращает предыдущий break или (void*)-1 при ошибке
static inline void* myos_sbrk(int increment) {
    return (void*)_syscall1(SYS_SBRK, (uint32_t)increment);
}

// ============================================================================
// API: GUI (Графический интерфейс)
// ============================================================================

// Создание окна
// Параметры:
//   title — заголовок окна
//   x, y  — координаты
//   w, h  — ширина и высота
// Возвращает дескриптор окна или 0 при ошибке
static inline uint32_t myos_create_window(const char* title, int x, int y,
                                           int w, int h) {
    return _syscall5(SYS_GUI_CREATE_WINDOW,
                     (uint32_t)title, (uint32_t)x, (uint32_t)y,
                     (uint32_t)w, (uint32_t)h);
}

// Рисование пикселя в окне
static inline void myos_put_pixel(uint32_t win, int x, int y, uint32_t color) {
    _syscall4(SYS_GUI_PUT_PIXEL, win, (uint32_t)x, (uint32_t)y, color);
}

// Запрос перерисовки экрана
static inline void myos_redraw(void) {
    _syscall0(SYS_GUI_REDRAW);
}

#endif // MYOS_H

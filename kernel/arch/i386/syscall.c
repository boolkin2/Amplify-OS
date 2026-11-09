// ============================================================================
// syscall.c - Реализация диспетчера системных вызовов (Этап 18)
// ============================================================================
// Расширенный диспетчер с поддержкой файловых операций,
// управления процессами и памятью.
// ============================================================================

#include "syscall.h"
#include "../../vga.h"
#include "../../gui/window.h"
#include "../../gui/wm.h"
#include "../../task/task.h"
#include "../../fs/vfs.h"
#include "../../drivers/timer.h"
#include "../../memory/heap.h"
#include "../../memory/paging.h"
#include "../../memory/vm.h"
#include "../../ipc/shm.h"
#include "../../debug_serial.h"
#include "../../security/security.h"

// ============================================================================
// Диспетчер системных вызовов
// ============================================================================

// Обработчик sys_write (запись в файл/консоль)
// fd=1 → stdout (вывод на экран)
static int sys_write(uint32_t fd, const char* buf, uint32_t size) {
    if (!buf || size == 0) return -1;

    if (fd == 1) {
        // stdout — выводим в VGA/GUI консоль и ПАРАЛЛЕЛЬНО в COM1,
        // чтобы автоматизированный прогон под QEMU (-serial stdio) мог
        // захватить вывод пользовательских программ для тестирования.
        for (uint32_t i = 0; i < size; i++) {
            vga_putchar(buf[i]);
            dbg_putc(buf[i]);
        }
        return (int)size;
    }

    // Обычный файловый дескриптор
    return vfs_write_file((int)fd, buf, size);
}

// Обработчик sys_read (чтение из файла)
static int sys_read(uint32_t fd, char* buf, uint32_t size) {
    if (!buf || size == 0) return -1;
    return vfs_read_file((int)fd, buf, size);
}

// Обработчик sys_open (открытие файла)
static int sys_open(const char* path, uint32_t flags) {
    if (!path) return -1;
    return vfs_open_file(path, flags);
}

// Обработчик sys_close (закрытие файла)
static int sys_close(uint32_t fd) {
    return vfs_close_file((int)fd);
}

// Диспетчер системных вызовов (вызывается из isr_handler_t)
static uint32_t syscall_handler(registers_t* regs) {
    uint32_t syscall_no = regs->eax;
    uint32_t arg1 = regs->ebx;
    uint32_t arg2 = regs->ecx;
    uint32_t arg3 = regs->edx;
    uint32_t arg4 = regs->esi;
    uint32_t arg5 = regs->edi;

    switch (syscall_no) {

        // ================================================================
        // SYS_EXIT (0) — завершение процесса
        // ================================================================
        case SYS_EXIT: {
            task_exit_current();
            break;
        }

        // ================================================================
        // SYS_GUI_CREATE_WINDOW (1) — создание окна
        // ================================================================
        case SYS_GUI_CREATE_WINDOW: {
            const char* title = (const char*)arg1;
            int32_t x = (int32_t)arg2;
            int32_t y = (int32_t)arg3;
            uint32_t w = (uint32_t)arg4;
            uint32_t h = (uint32_t)arg5;

            window_t* win = window_create(title, x, y, w, h);
            if (win) {
                wm_add_window(win);
                regs->eax = (uint32_t)win;
            } else {
                regs->eax = 0;
            }
            break;
        }

        // ================================================================
        // SYS_GUI_PUT_PIXEL (2) — рисование пикселя в окне
        // ================================================================
        case SYS_GUI_PUT_PIXEL: {
            window_t* win = (window_t*)arg1;
            int32_t x = (int32_t)arg2;
            int32_t y = (int32_t)arg3;
            uint32_t color = (uint32_t)arg4;

            if (win) {
                window_put_pixel(win, x, y, color);
            }
            break;
        }

        // ================================================================
        // SYS_GUI_REDRAW (3) — запрос перерисовки экрана
        // ================================================================
        case SYS_GUI_REDRAW: {
            wm_require_redraw();
            break;
        }

        // ================================================================
        // SYS_WRITE (4) — запись в файл/консоль
        // ================================================================
        case SYS_WRITE: {
            regs->eax = (uint32_t)sys_write(arg1, (const char*)arg2, arg3);
            break;
        }

        // ================================================================
        // SYS_READ (5) — чтение из файла
        // ================================================================
        case SYS_READ: {
            regs->eax = (uint32_t)sys_read(arg1, (char*)arg2, arg3);
            break;
        }

        // ================================================================
        // SYS_OPEN (6) — открытие файла
        // ================================================================
        case SYS_OPEN: {
            regs->eax = (uint32_t)sys_open((const char*)arg1, arg2);
            break;
        }

        // ================================================================
        // SYS_CLOSE (7) — закрытие файла
        // ================================================================
        case SYS_CLOSE: {
            regs->eax = (uint32_t)sys_close(arg1);
            break;
        }

        // ================================================================
        // SYS_GETPID (8) — получение PID текущего процесса
        // ================================================================
        case SYS_GETPID: {
            task_t* current = task_get_current();
            regs->eax = current ? current->id : 0;
            break;
        }

        // ================================================================
        // SYS_SLEEP (9) — сон на заданное количество миллисекунд
        // ================================================================
        case SYS_SLEEP: {
            if (arg1 > 0) {
                timer_sleep(arg1);
            }
            regs->eax = 0;
            break;
        }

        // ================================================================
        // SYS_SBRK (10) — увеличение кучи процесса
        // ================================================================
        case SYS_SBRK: {
            // Упрощённая реализация: выделяем из ядерной кучи
            // В будущем — отдельная куча для каждого процесса
            if (arg1 == 0) {
                regs->eax = 0; // Текущий break
            } else {
                void* ptr = kmalloc(arg1);
                regs->eax = (uint32_t)ptr;
            }
            break;
        }

        // ================================================================
        // SYS_STAT (11) — информация о файле
        // ================================================================
        case SYS_STAT: {
            const char* path = (const char*)arg1;
            vfs_stat_info_t* stat_buf = (vfs_stat_info_t*)arg2;
            if (path && stat_buf) {
                regs->eax = (uint32_t)vfs_stat(path, stat_buf);
            } else {
                regs->eax = (uint32_t)-1;
            }
            break;
        }

        // ================================================================
        // SYS_CLONE (12) — создание нового потока в Ring 3 (pthread_create)
        //   arg1 = entry_point (user)
        //   arg2 = user_stack_top
        //   arg3 = arg для start_routine
        // Возвращает TID нового потока или (uint32_t)-1 при ошибке.
        // ================================================================
        case 12: {
            task_t* t = task_spawn_user_thread(arg1, arg2, arg3);
            regs->eax = t ? t->id : (uint32_t)-1;
            break;
        }

        // ================================================================
        // SYS_JOIN (13) — дождаться завершения потока по TID (pthread_join)
        //   arg1 = tid
        // Блокирует текущую задачу, уступая CPU через HLT, пока целевой
        // поток не перейдёт в TASK_DEAD. После этого reaper в task_switch
        // уберёт его структуру.
        // ================================================================
        case 13: {
            uint32_t tid = arg1;
            for (;;) {
                task_t* t = task_find_by_id(tid);
                if (!t || t->state == TASK_DEAD) {
                    regs->eax = 0;
                    break;  // выход из for
                }
                // Уступаем CPU до следующего тика таймера.
                __asm__ volatile ("sti; hlt");
            }
            break;  // выход из switch
        }

        // ================================================================
        // SYS_MMAP (14) — отображение файла или анонимной памяти
        //   arg1 = addr (hint, игнорируется), arg2 = length, arg3 = prot,
        //   arg4 = flags, arg5 = fd (-1 для анонимной памяти)
        // Возвращает virt-адрес или (uint32_t)-1 при ошибке.
        //
        // Адреса выделяются из mmap-арены текущего процесса (vm->mmap_top),
        // что даёт изоляцию между процессами. Для файлов страницы выделяются
        // сразу (чтение содержимого). Для анонимной памяти используется
        // lazy-alloc: PTE помечаются PTE_MMAPPED, #PF handler выделит zero-page
        // только при первом обращении.
        // ================================================================
        case 14: {
            (void)arg1;        // hint не поддерживаем
            (void)arg3;        // prot — в этой версии всегда R/W
            (void)arg4;        // flags — MAP_ANON определяется по fd<0
            uint32_t length = arg2;
            int fd = (int)arg5;
            if (length == 0) { regs->eax = (uint32_t)-1; break; }

            uint32_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;

            vm_space_t* vm = vm_current();
            if (!vm) { regs->eax = (uint32_t)-1; break; }

            uint32_t vaddr = vm->mmap_top;
            vm->mmap_top += pages * PAGE_SIZE;

            if (fd >= 0) {
                // Файл: выделяем физические страницы сразу и читаем содержимое.
                paging_alloc_user_page(vaddr, pages);
                int old_pos = vfs_seek_file(fd, 0, 1);  // SEEK_CUR
                vfs_seek_file(fd, 0, 0);                // SEEK_SET
                vfs_read_file(fd, (void*)vaddr, length);
                vfs_seek_file(fd, old_pos, 0);
            } else {
                // Анонимная память: lazy-alloc через PTE_MMAPPED.
                paging_reserve_lazy_range(vaddr, pages, PTE_WRITABLE);
            }

            regs->eax = vaddr;
            break;
        }

        // ================================================================
        // SYS_MUNMAP (15) — освобождение памяти
        //   arg1 = addr, arg2 = length
        // Возвращает физические страницы в PMM, слоты в swap, обнуляет PTE.
        // ================================================================
        case 15: {
            uint32_t addr = arg1;
            uint32_t length = arg2;
            uint32_t pages = (length + PAGE_SIZE - 1) / PAGE_SIZE;

            paging_free_user_range(addr, pages);

            regs->eax = 0;
            break;
        }

        // ================================================================
        // SYS_FORK (17) — POSIX fork: создать дочерний процесс с COW-vm
        //   Родитель: eax = pid ребёнка (или (uint32_t)-1 при ошибке)
        //   Ребёнок:  eax = 0 (устанавливается в task_fork по копии регистров)
        // ================================================================
        case 17: {
            task_t* child = task_fork(regs);
            regs->eax = child ? child->id : (uint32_t)-1;
            break;
        }

        // ================================================================
        // SYS_CHDIR (19) — сменить рабочий каталог текущей задачи.
        //   arg1 = path (может быть абсолютным или относительным)
        // Проверяем, что путь существует и является каталогом.
        // Результат записывается в current_task->cwd как абсолютный
        // нормализованный путь. eax = 0 при успехе, -1 при ошибке.
        // ================================================================
        case 19: {
            const char* path = (const char*)arg1;
            task_t* t = task_get_current();
            if (!path || !t) { regs->eax = (uint32_t)-1; break; }

            char resolved[TASK_CWD_MAX];
            if (vfs_join_path(resolved, t->cwd, path) < 0) {
                regs->eax = (uint32_t)-1; break;
            }

            vfs_node_t* node = vfs_resolve_path(resolved);
            if (!node || !(node->flags & VFS_DIRECTORY)) {
                regs->eax = (uint32_t)-1; break;
            }

            // Копируем в cwd.
            int i = 0;
            while (resolved[i] && i < TASK_CWD_MAX - 1) {
                t->cwd[i] = resolved[i];
                i++;
            }
            t->cwd[i] = '\0';
            regs->eax = 0;
            break;
        }

        // ================================================================
        // SYS_GETCWD (20) — скопировать текущий рабочий каталог в буфер.
        //   arg1 = buf (char*), arg2 = size
        // eax = длина строки (без '\0') при успехе, -1 если buf не хватает.
        // ================================================================
        case 20: {
            char* buf = (char*)arg1;
            uint32_t sz = arg2;
            task_t* t = task_get_current();
            if (!buf || sz == 0 || !t) { regs->eax = (uint32_t)-1; break; }

            // Подсчитаем длину cwd.
            uint32_t len = 0;
            while (t->cwd[len] && len < TASK_CWD_MAX) len++;
            if (len + 1 > sz) { regs->eax = (uint32_t)-1; break; }

            for (uint32_t i = 0; i <= len; i++) buf[i] = t->cwd[i];
            regs->eax = len;
            break;
        }

        // ================================================================
        // SYS_SHMGET (21) — создать или найти shm-сегмент.
        //   arg1 = key, arg2 = size, arg3 = flags (SHM_IPC_CREAT, ...)
        // ================================================================
        case 21: {
            regs->eax = (uint32_t)shm_get((int)arg1, arg2, (int)arg3);
            break;
        }

        // ================================================================
        // SYS_SHMAT (22) — прикрепить сегмент к адресному пространству.
        //   arg1 = id
        // Возвращает user-VA или (uint32_t)-1 при ошибке.
        // ================================================================
        case 22: {
            regs->eax = shm_attach((int)arg1);
            break;
        }

        // ================================================================
        // SYS_SHMDT (23) — отсоединить сегмент по адресу.
        //   arg1 = vaddr
        // ================================================================
        case 23: {
            regs->eax = (uint32_t)shm_detach(arg1);
            break;
        }

        // ================================================================
        // SYS_SHMCTL (24) — управление сегментом (пока только IPC_RMID).
        //   arg1 = id, arg2 = cmd
        // ================================================================
        case 24: {
            regs->eax = (uint32_t)shm_ctl((int)arg1, (int)arg2);
            break;
        }

        // ================================================================
        // SYS_EXECVE (18) — POSIX execve: заменить образ текущего процесса
        //   arg1 = path (const char*)
        //   arg2 = argv (пока игнорируется)
        //   arg3 = envp (пока игнорируется)
        // При успехе НЕ возвращается (регистры подменены, iret уйдёт
        // в новый ELF). При ошибке — eax = -1.
        // ================================================================
        case 18: {
            (void)arg2; (void)arg3;   // argv/envp TODO
            const char* path = (const char*)arg1;
            int rc = task_execve(path, regs);
            if (rc != 0) {
                regs->eax = (uint32_t)-1;
            }
            // Если rc == 0, task_execve уже переписал regs->eax=0 и др.
            break;
        }

        // ================================================================
        // SYS_SWAP_TEST (16) — искусственный тест выгрузки страницы
        // ================================================================
        case 16: {
            uint32_t addr = arg1; // Виртуальный адрес
            extern _Bool paging_swap_page(uint32_t virt_addr);
            regs->eax = paging_swap_page(addr) ? 1 : 0;
            break;
        }

        // ================================================================
        // ====================  AMPLIFY-OS native syscalls  ===============
        // ================================================================
        // Уникальные имена aos_* (Этап 33 / persistence-fix). Реализуют
        // запрошенный пользователем стартовый набор: exit, write, read,
        // open, close, seek (lseek), pid (getpid), uid (getuid), now
        // (gettimeofday), yield (sched_yield), sleep (nanosleep),
        // brk, sbrk. Возвращают (int): >=0 успех, <0 = -errno.
        // ----------------------------------------------------------------
        case AOS_SYS_EXIT: {
            // aos_exit(code): код возврата сейчас не сохраняется,
            // задача просто помечается DEAD и планировщик переключит
            // контекст на следующую активную.
            (void)arg1;
            task_exit_current();
            // task_exit_current не возвращается; страховка на всякий случай
            regs->eax = 0;
            break;
        }
        case AOS_SYS_WRITE: {
            regs->eax = (uint32_t)sys_write(arg1, (const char*)arg2, arg3);
            break;
        }
        case AOS_SYS_READ: {
            regs->eax = (uint32_t)sys_read(arg1, (char*)arg2, arg3);
            break;
        }
        case AOS_SYS_OPEN: {
            regs->eax = (uint32_t)sys_open((const char*)arg1, arg2);
            break;
        }
        case AOS_SYS_CLOSE: {
            regs->eax = (uint32_t)sys_close(arg1);
            break;
        }
        case AOS_SYS_SEEK: {
            // ebx=fd, ecx=offset (signed), edx=whence (0=set,1=cur,2=end)
            regs->eax = (uint32_t)vfs_seek_file((int)arg1, (int)arg2, (int)arg3);
            break;
        }
        case AOS_SYS_PID: {
            task_t* t = task_get_current();
            regs->eax = t ? (uint32_t)t->id : 0;
            break;
        }
        case AOS_SYS_UID: {
            // Возвращаем uid из активной сессии security; если сессии нет —
            // ноль (как «root» в системе без логина).
            const sec_session_t* s = sec_get_session();
            regs->eax = (s && s->active) ? s->uid : 0;
            break;
        }
        case AOS_SYS_NOW: {
            // aos_now(uint64_t* out_ms): миллисекунды с момента старта
            // системы. Используем полную 64-битную точность, чтобы клиент
            // мог считать монотонное время без переполнения.
            uint64_t* out = (uint64_t*)arg1;
            uint64_t ms = timer_get_uptime_ms();
            if (out) {
                *out = ms;
                regs->eax = 0;
            } else {
                // Без буфера возвращаем младшие 32 бита прямо в eax —
                // удобно для лёгкого профилирования.
                regs->eax = (uint32_t)ms;
            }
            break;
        }
        case AOS_SYS_YIELD: {
            // Принудительно отдаём остаток кванта: разрешаем прерывания и
            // переходим в hlt. Следующий тик таймера вытащит нас из hlt
            // и task_switch() выберет другую готовую задачу.
            __asm__ volatile ("sti; hlt");
            regs->eax = 0;
            break;
        }
        case AOS_SYS_SLEEP: {
            // aos_sleep(ns_lo, ns_hi): наносекунды (64 бита через два аргумента).
            // PIT даёт миллисекунды, поэтому округляем вверх до мс.
            uint64_t ns = ((uint64_t)arg2 << 32) | (uint64_t)arg1;
            uint32_t ms = (uint32_t)((ns + 999999ULL) / 1000000ULL);
            if (ms == 0 && ns > 0) ms = 1;
            if (ms > 0) timer_sleep(ms);
            regs->eax = 0;
            break;
        }
        case AOS_SYS_BRK: {
            // Полноценного per-process break сейчас нет (общая kernel-куча).
            // По соглашению brk возвращает 0 при успехе или -1 если
            // запрошенный адрес не может быть установлен. Без vm-аллокатора
            // считаем любое изменение нереализуемым → -1, кроме
            // brk(0) — запрос текущего break, возвращаем 0.
            if (arg1 == 0) {
                regs->eax = 0;
            } else {
                regs->eax = (uint32_t)-1;
            }
            break;
        }
        case AOS_SYS_SBRK: {
            // sbrk(delta): выделяем delta байт из ядерной кучи и возвращаем
            // указатель. Это не настоящий sbrk (нет per-process heap),
            // но интерфейс совместимый: каждый вызов даёт свежий блок.
            // sbrk(0) -> 0 (текущая «граница» неизвестна → ноль).
            if (arg1 == 0) {
                regs->eax = 0;
            } else {
                void* p = kmalloc(arg1);
                regs->eax = p ? (uint32_t)p : (uint32_t)-1;
            }
            break;
        }

        // ================================================================
        // Неизвестный системный вызов
        // ================================================================
        // Раньше здесь шла печать в VGA-консоль, что портило экран рабочего
        // стола и сбивало позицию курсора. Перенесли в serial-лог: сохраняем
        // диагностику для разработчика, но не мешаем UI.
        default: {
            dbg_puts("[syscall] unknown #");
            char buf[12]; int n = 0; uint32_t v = syscall_no;
            char tmp[12]; int t = 0;
            if (v == 0) tmp[t++] = '0';
            else while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
            while (t > 0) buf[n++] = tmp[--t];
            buf[n++] = '\n'; buf[n] = 0;
            dbg_puts(buf);
            regs->eax = (uint32_t)-1;
            break;
        }

    }

    return (uint32_t)regs;
}

// ============================================================================
// Инициализация системных вызовов
// ============================================================================

void syscall_init(void) {
    // Регистрируем обработчик для прерывания 128 (int 0x80)
    isr_register_handler(128, syscall_handler);
}

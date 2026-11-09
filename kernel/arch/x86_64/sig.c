// ============================================================================
// sig.c — доставка сигналов из ядра в userspace (этап 40 группа B).
// ----------------------------------------------------------------------------
// Архитектура:
//   * Каждая task_t имеет sig_handlers[32], sig_pending, sig_mask, sig_restorer.
//   * sys_sigaction() устанавливает sig_handlers[signum] и sig_restorer.
//   * sys_kill() ставит бит в sig_pending адресата.
//   * sig_check_and_deliver() вызывается ИЗ syscall_dispatch() перед возвратом
//     в user mode — смотрит pending & ~mask, и если есть — строит signal-frame
//     в user stack, подменяет user RIP на handler.
//   * sys_sigreturn() восстанавливает сохранённые RIP/RSP/RFLAGS из signal-frame.
//   * SIGSEGV из page fault handler: interrupt.c вызывает sig_deliver_segv().
//
// Frame layout в user stack (от верха к низу, SP указывает на signum):
//
//   [SP + 24] saved user RFLAGS
//   [SP + 16] saved user RIP (returned-to)
//   [SP + 8 ] saved user RSP (returned-to)
//   [SP + 0 ] signum (в пользовательский handler попадает через RDI)
//
// Перед handler'ом мы подменяем на stack следующий layout (сверху вниз):
//
//   user RSP --->  [sigreturn_trampoline]    <- handler вернётся сюда "ret"
//                  [saved RFLAGS]
//                  [saved RIP]
//                  [saved RSP]
//                  [signum]                  <- handler: RDI = signum
//
// sigreturn читает [RSP+0]..[RSP+24] и делает syscall SIGRETURN, ядро вернёт
// управление на сохранённые RIP/RSP/RFLAGS через обычный SYSRETQ path.
// ============================================================================
#include <stdint.h>
#include "task.h"
#include "serial.h"
#include "syscall.h"

// Магическое значение, чтобы sigreturn мог валидировать frame.
#define SIG_FRAME_MAGIC 0xA05516EFCAFEBABEULL

typedef struct {
    uint64_t magic;
    uint64_t signum;
    uint64_t saved_rsp;
    uint64_t saved_rip;
    uint64_t saved_rflags;
} sig_frame_t;

// ---- Проверка, что userspace-адрес безопасен для записи ядром --------------
// Простая эвристика: USER_BASE = 0x8000000000, user-регион ~0x8000000000 + 2^32.
// Нет реальной проверки TLB — если страница не замаплена, запись приведёт к
// вложенному page fault и это diagnosed как panic (стабильнее, чем молча SIGSEGV).
static int user_addr_ok(uint64_t addr, uint64_t len) {
    if (addr < 0x8000000000ULL) return 0;
    if (addr + len < addr) return 0;     // overflow
    return 1;
}

// ---- sys_sigaction ---------------------------------------------------------
// args: a1 = signum, a2 = &act (user-ptr), a3 = &old (user-ptr или NULL)
// Наша минимальная struct sigaction:
//   offset 0:  handler (void(*)(int))
//   offset 8:  sa_mask (uint64_t)
//   offset 16: sa_flags (int32)
//   offset 20: (pad)
//   offset 24: sa_restorer (void(*)(void)) — ключевой адрес трамполина!
// Разметка совместима с libc-x86_64/include/signal.h struct sigaction.
typedef struct {
    uint64_t handler;
    uint64_t mask;
    int32_t  flags;
    int32_t  pad;
    uint64_t restorer;
} user_sigaction_t;

int64_t sig_sys_sigaction(int signum, const user_sigaction_t* act,
                          user_sigaction_t* old) {
    task_t* t = task_current();
    if (!t) return -AOS_EPERM;
    if (signum <= 0 || signum >= 32) return -AOS_EINVAL;
    if (signum == 9 /* SIGKILL */ || signum == 19 /* SIGSTOP */) return -AOS_EINVAL;

    if (old) {
        if (!user_addr_ok((uint64_t)old, sizeof(*old))) return -AOS_EFAULT;
        old->handler  = t->sig_handlers[signum];
        old->mask     = 0;
        old->flags    = 0;
        old->pad      = 0;
        old->restorer = t->sig_restorer;
    }
    if (act) {
        if (!user_addr_ok((uint64_t)act, sizeof(*act))) return -AOS_EFAULT;
        t->sig_handlers[signum] = act->handler;
        if (act->restorer) t->sig_restorer = act->restorer;
    }
    return 0;
}

// ---- sys_kill --------------------------------------------------------------
// a1 = pid (tid на 40), a2 = signum.
// Ищем task_t с таким tid, ставим бит в sig_pending.
extern task_t* task_find_by_tid(int tid);     // см. task.c — добавим

int64_t sig_sys_kill(int pid, int signum) {
    if (signum <= 0 || signum >= 32) return -AOS_EINVAL;
    task_t* target = task_find_by_tid(pid);
    if (!target) return -AOS_ENOENT;
    if (signum == 9) {                           // SIGKILL: немедленно завершаем
        // На 40-м нет корректного способа завершить стороннюю задачу
        // из другого контекста — помечаем pending, обработка на её следующем
        // SYSCALL. Для уже бегущей задачи это задержка до yield.
        target->sig_pending |= (1u << signum);
        // Установим handler в NULL для SIGKILL чтобы default action сработал.
        target->sig_handlers[9] = 0;
    } else {
        target->sig_pending |= (1u << signum);
    }
    return 0;
}

// ---- Построение signal-frame в userspace -----------------------------------
// Вызывается syscall_dispatch перед возвратом в user.
// Берём user RSP из user_rsp_save (ABI syscall_entry.asm).
extern uint64_t user_rsp_save;

// Вызывается из syscall_dispatch, перед возвратом в user-mode.
// Если есть pending и handler установлен — перезаписывает return context
// (user_rsp_save + указатель RCX-сохранённый RIP в kernel stack).
// Возвращает новое значение user RIP (0 если не нужно менять).
//
// Этап 40: здесь же обрабатываем SIGRETURN — если последний syscall был
// AOS_SYS_SIGRETURN, задача находится В обработчике сигнала и хочет
// восстановить прежний контекст из signal-frame.
extern uint64_t g_last_syscall_num;

uint64_t sig_check_and_deliver_user(uint64_t* return_rip_slot,
                                    uint64_t* return_rflags_slot) {
    task_t* t = task_current();
    if (!t) return 0;

    // 1) SIGRETURN: задача выходит из signal-handler. Это приоритет выше,
    //    чем новые pending signals (чтобы избежать циклической вложенности).
    if (g_last_syscall_num == AOS_SYS_SIGRETURN) {
        g_last_syscall_num = 0;                      // consumed
        uint64_t rsp = user_rsp_save;
        if (user_addr_ok(rsp, sizeof(sig_frame_t))) {
            sig_frame_t* frame = (sig_frame_t*)(uintptr_t)rsp;
            if (frame->magic == SIG_FRAME_MAGIC) {
                *return_rip_slot    = frame->saved_rip;
                *return_rflags_slot = frame->saved_rflags;
                user_rsp_save       = frame->saved_rsp;
                return frame->saved_rip;
            }
        }
        // invalid frame — terminate
        serial_puts("[sig] invalid sigreturn frame\n");
        extern void task_exit(int) __attribute__((noreturn));
        task_exit(139);
    }

    // 2) Обычная доставка нового сигнала.
    uint32_t ready = t->sig_pending & ~t->sig_mask;
    if (!ready) return 0;

    // Выбираем наименьший по номеру pending signal.
    int sig = 0;
    for (int i = 1; i < 32; i++) {
        if (ready & (1u << i)) { sig = i; break; }
    }
    if (!sig) return 0;

    // Очищаем pending для этого сигнала.
    t->sig_pending &= ~(1u << sig);

    uint64_t handler = t->sig_handlers[sig];
    (void)handler;    // используется только для проверки SIG_DFL/SIG_IGN ниже

    // SIG_DFL (0): default action — terminate для большинства сигналов.
    if (handler == 0) {
        if (sig == 9 || sig == 11 || sig == 6 /* ABRT */ || sig == 8 /* FPE */
            || sig == 2 /* INT */ || sig == 15 /* TERM */ || sig == 4 /* ILL */) {
            serial_puts("[sig] default terminate for signal ");
            serial_print_dec(sig);
            serial_puts(" on tid=");
            serial_print_dec(t->tid);
            serial_puts("\n");
            extern void task_exit(int) __attribute__((noreturn));
            task_exit(128 + sig);
        }
        // Прочие — ignore.
        return 0;
    }
    // SIG_IGN (1): пропускаем.
    if (handler == 1) return 0;

    // У нас должен быть sig_restorer. Если его нет — default action.
    if (!t->sig_restorer) {
        serial_puts("[sig] signal without restorer — terminate\n");
        extern void task_exit(int) __attribute__((noreturn));
        task_exit(128 + sig);
    }

    // Строим signal-frame на user stack.
    uint64_t user_rsp = user_rsp_save;
    // 16-байтное выравнивание (SysV ABI ожидает от handler'а).
    uint64_t new_rsp = (user_rsp - sizeof(sig_frame_t)) & ~0xFULL;

    if (!user_addr_ok(new_rsp, sizeof(sig_frame_t))) {
        serial_puts("[sig] bad user rsp — terminate\n");
        extern void task_exit(int) __attribute__((noreturn));
        task_exit(128 + sig);
    }

    sig_frame_t* frame = (sig_frame_t*)(uintptr_t)new_rsp;
    frame->magic        = SIG_FRAME_MAGIC;
    frame->signum       = (uint64_t)sig;
    frame->saved_rsp    = user_rsp;
    frame->saved_rip    = *return_rip_slot;
    frame->saved_rflags = *return_rflags_slot;

    // Схема доставки: мы НЕ прыгаем напрямую в handler. Вместо этого
    // прыгаем в sig_restorer (наш libc-wrapper __aos_sigreturn_tramp),
    // передавая ему два аргумента:
    //   RDI = signum
    //   RSI = adress handler'а (task->sig_handlers[sig])
    // Wrapper сам вызовет handler(signum), а затем сделает syscall SIGRETURN.
    //
    // RSP при входе в wrapper указывает на sig_frame_t (frame->magic).
    // Wrapper должен быть написан так, чтобы не портить frame (16-байтное
    // выравнивание на frame + следующие 8 байт зарезервированы для
    // возврат-адреса; наш wrapper сам учтёт это выравнивание через sub rsp,8).

    // Переопределяем return context.
    user_rsp_save        = new_rsp;
    *return_rip_slot     = t->sig_restorer;
    *return_rflags_slot  = 0x202;

    // Передаём аргументы wrapper'у. Для этого нужно менять RDI/RSI в
    // kernel push'ах.  syscall_entry.asm пушит (сверху вниз): rax, rdi, rsi,
    // rdx, r10, r8, r9, r11, rcx.  До нашего sig_check они уже POP'нуты.
    // Но мы можем обойтись без изменения регистров: wrapper читает
    // frame напрямую, signum и handler помещены туда же.
    //
    // Дополнительно пишем handler и signum в frame (frame уже содержит их).
    return t->sig_restorer;
}

// Помощник для sigreturn-wrapper'а (используется кодом libc-трамполина):
// возвращает handler по номеру сигнала для текущей задачи.
uint64_t sig_get_handler_for_frame(uint64_t signum) {
    task_t* t = task_current();
    if (!t || signum == 0 || signum >= 32) return 0;
    return t->sig_handlers[signum];
}

// ---- sys_sigreturn ---------------------------------------------------------
// Теперь обрабатывается внутри sig_check_and_deliver_user (по g_last_syscall_num),
// а не как отдельная функция. Здесь оставляем тонкую точку входа для
// диспатчера — она лишь сигнализирует о том, что syscall_num=SIGRETURN.
int64_t sig_sys_sigreturn_from_dispatch(syscall_args_t* a) {
    (void)a;
    // Не возвращаем ничего полезного — sig_check_and_deliver_user прочитает
    // g_last_syscall_num и восстановит контекст из frame.
    return 0;
}

// ---- Доставка SIGSEGV из page fault handler --------------------------------
// Вызывается interrupt.c при page fault из CPL=3.
void sig_deliver_segv(void) {
    task_t* t = task_current();
    if (!t) return;
    t->sig_pending |= (1u << 11);  // SIGSEGV
}

// ---- Доставка SIGINT (из keyboard Ctrl+C — будущий hookup) -----------------
void sig_deliver_int(int tid) {
    task_t* t = task_find_by_tid(tid);
    if (!t) return;
    t->sig_pending |= (1u << 2);   // SIGINT
}

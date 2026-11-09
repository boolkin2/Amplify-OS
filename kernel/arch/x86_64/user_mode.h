// ============================================================================
// user_mode.h — переключение Ring 0 → Ring 3 в long mode.
// ============================================================================
#ifndef AOS_X86_64_USER_MODE_H
#define AOS_X86_64_USER_MODE_H

#include <stdint.h>

// Прыгает в user mode через IRETQ: CS=user_CS, SS=user_DS, RIP=rip, RSP=rsp,
// RFLAGS = (kernel) | IF.
// НЕ ВОЗВРАЩАЕТСЯ. Возврат к kernel-управлению только через syscall (например,
// AOS_SYS_EXIT, который halt'ит CPU; полноценный планировщик будет в 37.4b).
void enter_user_mode(uint64_t rip, uint64_t rsp) __attribute__((noreturn));

#endif

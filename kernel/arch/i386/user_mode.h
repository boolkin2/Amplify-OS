// ============================================================================
// user_mode.h - Функции для переключения в User Mode (Ring 3)
// ============================================================================

#ifndef USER_MODE_H
#define USER_MODE_H

#include <stdint.h>

// Переключение в User Mode
// eip - адрес инструкции, с которой начнется выполнение
// esp - адрес стека пользователя
extern void jump_to_user_mode(uint32_t eip, uint32_t esp);

#endif // USER_MODE_H

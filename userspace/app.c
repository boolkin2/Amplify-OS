#include <stdint.h>

// Макросы для системных вызовов через int 0x80
static inline uint32_t syscall0(uint32_t sys_num) {
    uint32_t ret;
    __asm__ volatile ("int $0x80" : "=a" (ret) : "a" (sys_num));
    return ret;
}

static inline uint32_t syscall4(uint32_t sys_num, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (sys_num), "b" (arg1), "c" (arg2), "d" (arg3), "S" (arg4)
    );
    return ret;
}

static inline uint32_t syscall5(uint32_t sys_num, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4, uint32_t arg5) {
    uint32_t ret;
    __asm__ volatile (
        "int $0x80"
        : "=a" (ret)
        : "a" (sys_num), "b" (arg1), "c" (arg2), "d" (arg3), "S" (arg4), "D" (arg5)
    );
    return ret;
}

#define SYS_EXIT                 0
#define SYS_GUI_CREATE_WINDOW    1
#define SYS_GUI_PUT_PIXEL        2
#define SYS_GUI_REDRAW           3

// Точка входа в программу (собирается как flat binary)
void _start() {
    // 1. Создаём окно "User App" (Syscall 1)
    const char* title = "Ring 3 App (XOR Pattern)";
    uint32_t win = syscall5(SYS_GUI_CREATE_WINDOW, (uint32_t)title, 450, 100, 256, 256);
    
    if (win) {
        // 2. Рисуем узор (Syscall 2)
        for (int y = 0; y < 256; y++) {
            for (int x = 0; x < 256; x++) {
                uint8_t c = x ^ y; // Знаменитый XOR-фрактал
                uint32_t color = 0xFF000000 | (c << 16) | (c << 8) | 255;
                syscall4(SYS_GUI_PUT_PIXEL, win, x, y, color);
            }
        }
        
        // 3. Запрос перерисовки экрана (Syscall 3)
        syscall0(SYS_GUI_REDRAW);
    }
    
    // Вечный цикл
    // Внимание: инструкция 'hlt' запрещена в Ring 3 и вызовет General Protection Fault
    while (1) {
        // Просто крутимся (в будущем можно добавить syscall sleep/yield)
        for(volatile int i = 0; i < 100000; i++);
    }
}

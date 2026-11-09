// ============================================================================
// timer.c - Реализация драйвера системного таймера PIT для MyOS
// ============================================================================

#include "timer.h"
#include "../arch/i386/isr.h"
#include "../arch/i386/idt.h"
#include "../arch/i386/cpu_stats.h"
#include "../io.h"
#include "../vga.h"
#include "../task/task.h"
#include "../debug_serial.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Счётчик тиков таймера (увеличивается при каждом IRQ0)
static volatile uint64_t timer_ticks = 0;

// Частота таймера (Hz)
static uint32_t timer_freq = TIMER_FREQUENCY;

// ============================================================================
// Внутренние функции
// ============================================================================

// Обработчик прерывания таймера (IRQ0)
static uint32_t timer_irq_handler(registers_t* regs) {
    // Увеличиваем счётчик тиков
    timer_ticks++;

    // Этап 30: каждый тик считается «работой» BSP'а — это даёт fallback
    // активности slot[0] даже когда в main-цикле BSP замер на hlt.
    cpu_stats_mark_busy_bsp();

    // Переключаем контекст многозадачности
    return task_switch((uint32_t)regs);
}

// Настройка частоты PIT
static void timer_set_frequency(uint32_t frequency) {
    // Вычисляем делитель для заданной частоты
    // Делитель = Базовая частота / Желаемая частота
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency;
    
    // Ограничиваем делитель (16-bit значение)
    if (divisor > 0xFFFF) {
        divisor = 0xFFFF;
    }
    if (divisor < 1) {
        divisor = 1;
    }
    
    // Отправляем команду инициализации в PIT
    outb(PIT_COMMAND, PIT_CMD_INIT);
    
    // Отправляем делитель (младший байт, затем старший)
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));        // Младший байт
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF)); // Старший байт
    
    // Сохраняем фактическую частоту
    timer_freq = PIT_BASE_FREQUENCY / divisor;
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация таймера
void timer_init(void) {
    // Сбрасываем счётчик тиков
    timer_ticks = 0;
    
    // Регистрируем обработчик IRQ0 (таймер)
    isr_register_handler(IRQ0, timer_irq_handler);
    
    // Настраиваем частоту таймера
    timer_set_frequency(TIMER_FREQUENCY);
}

// Получение количества тиков
uint64_t timer_get_ticks(void) {
    return timer_ticks;
}

// Получение времени работы в миллисекундах
uint64_t timer_get_uptime_ms(void) {
    // Преобразуем тики в миллисекунды
    // ms = (ticks * 1000) / frequency
    // Используем 32-битную арифметику для избежания __udivdi3
    uint64_t ticks = timer_ticks;
    uint32_t ms_per_tick = 1000 / timer_freq;  // Для 100 Hz = 10 ms
    return ticks * ms_per_tick;
}

// Получение времени работы в секундах
uint32_t timer_get_uptime_sec(void) {
    // Преобразуем тики в секунды
    // sec = ticks / frequency
    // Используем 32-битную арифметику
    uint32_t ticks_low = (uint32_t)timer_ticks;
    return ticks_low / timer_freq;
}

// Задержка в миллисекундах.
// Используем sti+hlt: голый hlt в контексте syscall-обработчика (вход через
// interrupt-gate с IF=0) «замораживает» CPU навсегда, потому что тики таймера
// нужны для счётчика, но сами прерывания запрещены. sti разрешает IF перед hlt;
// sti+hlt атомарны (IF включается только с этой следующей инструкции), поэтому
// гонки потерять пробуждение нет.
void timer_sleep(uint32_t ms) {
    uint32_t ticks_to_wait = (ms * timer_freq) / 1000;
    uint64_t start_ticks = timer_ticks;
    while ((timer_ticks - start_ticks) < ticks_to_wait) {
        __asm__ volatile ("sti; hlt");
    }
}

// Задержка в тиках (та же семантика, см. timer_sleep).
void timer_wait(uint32_t ticks) {
    uint64_t start_ticks = timer_ticks;
    while ((timer_ticks - start_ticks) < ticks) {
        __asm__ volatile ("sti; hlt");
    }
}

// Получение частоты таймера
uint32_t timer_get_frequency(void) {
    return timer_freq;
}

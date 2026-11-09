#include "pit.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"

#define PIT_FREQ      1193182u
#define PIT_CMD       0x43
#define PIT_DATA0     0x40

static volatile uint64_t ticks;

static void pit_isr(registers64_t* r) {
    (void)r;
    ticks++;
    // Дёргаем периодические callback'и пользователей (audio, watchdog и т.д.).
    // Внутри ISR — короткие хендлеры. См. timer_register_callback.
    timer_tick();
    pic_send_eoi(0);
}

void pit_init(uint32_t hz) {
    uint32_t divisor = PIT_FREQ / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;

    // Channel 0, lobyte+hibyte, mode 3 (square wave), binary.
    outb(PIT_CMD, 0x36);
    outb(PIT_DATA0, divisor & 0xFF);
    outb(PIT_DATA0, (divisor >> 8) & 0xFF);

    idt64_set_handler(32, pit_isr);     // IRQ0 = vector 32 после ремапа
    pic_clear_mask(0);                  // включаем IRQ0
}

uint64_t pit_ticks(void) { return ticks; }

// [41.2] Таймер LAPIC берёт на себя роль источника времени: сам PIT
// замолкает (IRQ0 маскируется), а «тик 100 Гц» — счётчик pit_ticks() и
// периодические callback'и timer_tick — продолжает идти, только теперь его
// раз в 10 мс дёргает обработчик LAPIC. Весь код, считающий время в тиках
// по 10 мс, остаётся верным.
void pit_handover_to_lapic(void) { pic_set_mask(0); }
void pit_tick_from_lapic(void)   { ticks++; timer_tick(); }

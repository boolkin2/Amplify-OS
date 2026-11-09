// ============================================================================
// lapic.c - Local APIC: разовая инициализация на каждом ядре (BSP и AP).
// ============================================================================
// Минимальная конфигурация под учебную SMP-систему MyOS:
//   * Включаем LAPIC через SVR (бит 8)
//   * Маскируем неиспользуемые LVT (timer/perf/err) — IRQ-маршрутизация у нас
//     по-прежнему через 8259 PIC.
//   * На BSP оставляем LINT0 в режиме ExtINT, чтобы PIC мог пробрасывать
//     IRQ через LINT0 после включения LAPIC (иначе timer-IRQ перестанет
//     доходить и timer_sleep() повиснет на sti+hlt). На AP'ах LINT0
//     маскируем — он там не используется.
//   * Сбрасываем ESR и шлём EOI (на случай pending'а от BIOS).
// ============================================================================

#include "lapic.h"
#include "acpi.h"
#include "../../memory/paging.h"
#include <stdbool.h>

static volatile uint32_t* lapic_ptr;

void lapic_write(uint32_t reg, uint32_t value) {
    *((volatile uint32_t*)((uint32_t)lapic_ptr + reg)) = value;
}

uint32_t lapic_read(uint32_t reg) {
    return *((volatile uint32_t*)((uint32_t)lapic_ptr + reg));
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector, uint32_t delivery_mode,
                    uint32_t level, uint32_t trigger) {
    uint32_t high = lapic_id << 24;
    uint32_t low  = vector | (delivery_mode << 8) | (level << 14) | (trigger << 15);

    lapic_write(LAPIC_ICR_HIGH, high);
    lapic_write(LAPIC_ICR_LOW,  low);

    while (lapic_read(LAPIC_ICR_LOW) & (1 << 12)) {
        __asm__ volatile ("pause");
    }
}

#define LAPIC_LVT_MASK     (1u << 16)
// Delivery mode для LVT: 7 = ExtINT (наследие 8259 PIC через LINT0),
//                         4 = NMI (BIOS обычно ставит на LINT1).
#define LAPIC_LVT_EXTINT   (7u << 8)
#define LAPIC_LVT_NMI      (4u << 8)

// Флаг: уже ли BSP инициализировал LAPIC.
//   Первый вход в lapic_init() — это BSP. Его задача: оставить LINT0 в режиме
//   ExtINT (без маски), чтобы прерывания от 8259 PIC, физически разведённого
//   только на BSP, продолжали доходить до CPU после включения LAPIC через SVR.
//   Если этого не сделать, после `lapic_write(SVR, ...|0x100)` любые IRQ от
//   PIC (включая таймер IRQ0) будут игнорироваться, и `sti; hlt` повиснет
//   навсегда.
//
//   Все последующие вызовы — это AP'ы из ap_main(); им LINT0 не нужен,
//   мы его маскируем, чтобы случайный шум на пине не порождал прерываний.
static volatile int g_lapic_bsp_done = 0;

void lapic_init(void) {
    lapic_ptr = (volatile uint32_t*)local_apic_base;

    bool is_bsp = !__sync_fetch_and_or(&g_lapic_bsp_done, 1);

    // SVR: spurious vector = 0xFF, бит 8 = software-enable LAPIC.
    lapic_write(LAPIC_SVR, 0xFF | 0x100);

    // LVT setup:
    //   * Timer/Perf/Err — нам не нужны (IRQ-роутинг идёт через PIC), маскируем.
    //   * LINT0 — на BSP оставляем ExtINT, иначе PIC «оторвётся» от CPU после
    //             включения LAPIC. На AP — маска.
    //   * LINT1 — стандартный NMI-пин, на BSP оставим NMI (как BIOS),
    //             на AP — маска.
    lapic_write(LAPIC_LVT_TMR,   LAPIC_LVT_MASK);
    lapic_write(LAPIC_LVT_PERF,  LAPIC_LVT_MASK);
    lapic_write(LAPIC_LVT_ERR,   LAPIC_LVT_MASK);

    if (is_bsp) {
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_EXTINT);
        lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_NMI);
    } else {
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASK);
        lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASK);
    }

    // Двойная запись ESR — стандартная процедура очистки регистра ошибок.
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);

    lapic_eoi();
}

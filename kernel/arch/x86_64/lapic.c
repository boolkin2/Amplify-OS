// ============================================================================
// lapic.c — Local APIC init/EOI/IPI для x86_64.
// ----------------------------------------------------------------------------
// Логика 1:1 как в kernel/arch/i386/lapic.c, но base указатель — uint64_t.
// Один экземпляр кода обслуживает BSP и AP'ы (per-CPU состояние через
// атомарный флаг g_lapic_bsp_done).
// ============================================================================
#include "lapic.h"
#include "acpi.h"
#include "serial.h"
#include "idt.h"
#include "timer.h"
#include "pit.h"

#define LAPIC_LVT_MASK      (1u << 16)
#define LAPIC_LVT_EXTINT    (7u << 8)
#define LAPIC_LVT_NMI       (4u << 8)

static volatile int g_lapic_bsp_done = 0;

uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t*)(uintptr_t)(acpi_lapic_phys + reg);
}

void lapic_write(uint32_t reg, uint32_t value) {
    *(volatile uint32_t*)(uintptr_t)(acpi_lapic_phys + reg) = value;
}

void lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

uint32_t lapic_get_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

// IPI (Inter-Processor Interrupt). Используется для SMP startup (INIT/SIPI).
//   delivery_mode: 0=FIXED, 4=NMI, 5=INIT, 6=SIPI
//   level: 1=assert (для INIT/STARTUP), 0=deassert (для INIT level-trigger reset)
//   trigger: 1=level, 0=edge
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector,
                    uint8_t delivery_mode, uint8_t level, uint8_t trigger) {
    uint32_t high = lapic_id << 24;
    uint32_t low  = (uint32_t)vector
                  | ((uint32_t)delivery_mode << 8)
                  | ((uint32_t)level   << 14)
                  | ((uint32_t)trigger << 15);

    // [50.5] Отправка — это ДВЕ записи (кому, затем что), и между ними нельзя
    // пускать обработчик прерывания: IPI шлют и из прерываний (таймер будит
    // спящего гостя — hv_tick_1ms, клавиша будит его же из обработчика
    // клавиатуры). Вклинившись между записями, обработчик оставил бы в
    // «кому» своего адресата, и сигнал главного потока ушёл бы не тому ядру.
    // Флаги возвращаем как были: сюда приходят и с IF=0 (запуск AP, прерывания).
    uint64_t flags;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(flags) : : "memory");

    lapic_write(LAPIC_ICR_HIGH, high);
    lapic_write(LAPIC_ICR_LOW,  low);

    // Ждём, пока ICR-low.bit12 (Delivery Status) очистится.
    while (lapic_read(LAPIC_ICR_LOW) & (1u << 12)) {
        __asm__ __volatile__("pause");
    }
    if (flags & 0x200) __asm__ __volatile__("sti");
}

void lapic_init(void) {
    bool is_bsp = !__sync_fetch_and_or(&g_lapic_bsp_done, 1);

    // SVR: spurious vector 0xFF, software-enable LAPIC (бит 8).
    lapic_write(LAPIC_SVR, 0xFF | 0x100);

    // LVT timer/perf/err — пока не используем, маскируем.
    // (Если ERR не маскировать, может прилететь в момент LAPIC enable и
    //  затолкать spurious 0xFE; в i386 версии тоже маска.)
    lapic_write(LAPIC_LVT_TMR,   LAPIC_LVT_MASK);
    lapic_write(LAPIC_LVT_PERF,  LAPIC_LVT_MASK);
    lapic_write(LAPIC_LVT_ERR,   LAPIC_LVT_MASK);

    if (is_bsp) {
        // На BSP оставляем LINT0 в режиме ExtINT — иначе PIC потеряет CPU.
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_EXTINT);
        lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_NMI);
    } else {
        lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASK);
        lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASK);
    }

    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_eoi();

    if (is_bsp) {
        serial_puts("[lapic] BSP enabled, ID=");
        serial_print_dec(lapic_get_id());
        serial_puts(" base=0x");
        serial_print_hex(acpi_lapic_phys, 16);
        serial_puts("\n");
    }
}

// ============================================================================
// [41.2] Таймер LAPIC на 1000 Гц вместо PIT на 100 Гц.
//
// Зачем: всё время ядра шло с шагом 10 мс — timer_sleep(1) спал до 10 мс,
// анимации и замеры видели только десятки миллисекунд. Таймер LAPIC под KVM
// обслуживается в ядре хоста (PIT — эмуляция устройства), и 1000 прерываний
// в секунду стоят дёшево.
//
// Калибровка: частота шины LAPIC заранее неизвестна, поэтому считаем, сколько
// отсчётов таймер делает за 50 мс по PIT (5 тиков), и ставим периодический
// режим с таким числом отсчётов на 1 мс. PIT после этого замолкает, а его
// «тик 100 Гц» продолжает жить каждым десятым тиком LAPIC (timer_ms_tick).
// Только BSP: вычислительные ядра спят в hlt до IPI, таймер им не нужен.
// ============================================================================
#define LAPIC_TIMER_VECTOR    0xEF
#define LAPIC_TIMER_PERIODIC  (1u << 17)

static void lapic_timer_tick_isr(registers64_t* r) {
    (void)r;
    timer_ms_tick();
    lapic_eoi();
}

bool lapic_timer_start(void) {
    // Делитель 16; считаем вниз от максимума, пока PIT отсчитает 5 тиков.
    lapic_write(LAPIC_TMRDIV, 0x3);
    uint64_t t = pit_ticks();
    while (pit_ticks() == t) __asm__ __volatile__("sti; hlt");     // к началу тика
    lapic_write(LAPIC_LVT_TMR, LAPIC_LVT_MASK);
    lapic_write(LAPIC_TMRINITCNT, 0xFFFFFFFFu);
    t = pit_ticks();
    while (pit_ticks() - t < 5) __asm__ __volatile__("sti; hlt");
    uint32_t elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_TMRCURRCNT);
    lapic_write(LAPIC_TMRINITCNT, 0);                               // стоп
    uint32_t per_ms = elapsed / 50;
    if (per_ms < 100) {
        // Таймер не считает (или считает неправдоподобно медленно) — остаёмся на PIT.
        serial_puts("[lapic] timer calibration failed, staying on PIT 100 Hz\n");
        return false;
    }
    idt64_set_handler(LAPIC_TIMER_VECTOR, lapic_timer_tick_isr);
    __asm__ __volatile__("cli");
    pit_handover_to_lapic();
    timer_ms_source_start();
    lapic_write(LAPIC_LVT_TMR, LAPIC_TIMER_VECTOR | LAPIC_TIMER_PERIODIC);
    lapic_write(LAPIC_TMRINITCNT, per_ms);
    __asm__ __volatile__("sti");
    serial_puts("[lapic] timer 1000 Hz (");
    serial_print_dec(per_ms);
    serial_puts(" counts/ms, divider 16), PIT stopped\n");
    return true;
}

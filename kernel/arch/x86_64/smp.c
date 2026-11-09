// ============================================================================
// smp.c — раскрутка дополнительных ядер (Application Processors) в 64-bit.
// ----------------------------------------------------------------------------
// 1) Копируем trampoline_start..trampoline_end в физическую страницу 0x8000
// 2) Зашиваем туда: PML4 phys, указатель на массив стеков, счётчик слотов
// 3) Для каждого AP (по списку из ACPI MADT): INIT IPI → 10 ms → SIPI ×2
// 4) Каждый AP сам забирает свой слот через `lock xadd`, переключается на
//    свой стек, входит в ap_main64()
// ============================================================================
#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "cpu_stats.h"
#include "spinlock.h"
#include "paging.h"
#include "pmm.h"
#include "pit.h"
#include "serial.h"
#include "fb.h"
#include "smp_pool.h"
#include "vmx.h"
#include "gdt.h"

#define TRAMPOLINE_PHYS  0x8000
#define AP_STACK_PAGES   8                    // 32 KB на каждое ядро
#define AP_STACK_SIZE    (AP_STACK_PAGES * PAGE_SIZE)

volatile uint32_t smp_cpus_started = 1;

// Из smp_trampoline.asm.
extern uint8_t  trampoline_start[];
extern uint8_t  trampoline_end[];
extern uint64_t tramp_pml4_phys;       // эти три — переменные ВНУТРИ trampoline blob,
extern uint64_t tramp_ap_stacks_top;   // адресуемые до копирования по их kernel-адресу
extern uint64_t tramp_ap_slot;         // (только для расчёта смещения внутри блоба)

static spinlock_t g_smp_print_lock = { 0 };

// Массив указателей на TOP каждого стека AP (BSP = слот 0, стек его уже есть).
static uint64_t ap_stacks_top[MAX_CPUS];

// Простой busy-wait, ms (PIT @ 100 Hz -> 10 ms на тик).
// Используем sti+hlt чтобы гарантировать что PIT IRQ доставляется
// (без sti PIT не тикает и wait зависает навечно).
static void busy_wait_ms(uint64_t ms) {
    uint64_t start = pit_ticks();
    uint64_t need  = (ms + 9) / 10;       // округление вверх
    while (pit_ticks() - start < need) {
        __asm__ __volatile__("sti; hlt");
    }
}

// ----------------------------------------------------------------------------
// Точка входа AP (64-bit).
// ----------------------------------------------------------------------------
void ap_main64(void) {
    lapic_init();

    uint32_t my_id = lapic_get_id();
    int my_slot = (int)__sync_fetch_and_add(&smp_cpus_started, 1);

    if (my_slot >= 0 && my_slot < MAX_CPUS) {
        cpu_stats[my_slot].lapic_id   = my_id;
        cpu_stats[my_slot].busy_ticks = 0;
        cpu_stats[my_slot].idle_ticks = 0;
        cpu_stats[my_slot].online     = true;
    }

    // [этап 50] Общий GDT ядра и свой TSS — до любой работы на этом ядре:
    // без них VM entry на AP невозможен (подробно — gdt64_init_ap).
    if (my_slot >= 1 && my_slot < MAX_CPUS) gdt64_init_ap(my_slot);

    // [этап 50] IA32_FEATURE_CONTROL у каждого ядра свой, а гость phantom
    // исполняется именно на AP: запоминаем состояние VT-x этого ядра сейчас,
    // пока оно ещё не занято пулом (BSP прочитает это в vmx_probe).
    vmx_probe_this_cpu(my_slot);

    spinlock_acquire(&g_smp_print_lock);
    serial_puts("[smp] AP online: lapic_id=");
    serial_print_dec(my_id);
    serial_puts(" slot=");
    serial_print_dec(my_slot);
    serial_puts("\n");
    spinlock_release(&g_smp_print_lock);

    // [этап 49] Раньше AP навсегда засыпал здесь в hlt с IF=0. Теперь он
    // становится воркером пула вычислений (smp_pool.c): грузит общий IDT,
    // спит в sti;hlt и просыпается по IPI 0xF0 за работой.
    if (my_slot >= 1 && my_slot < MAX_CPUS) smp_pool_ap_entry(my_slot);
    for (;;) __asm__ __volatile__("cli; hlt");
}

// ----------------------------------------------------------------------------
// SMP boot.
// ----------------------------------------------------------------------------
void smp_init(void) {
    cpu_stats_init();
    // [этап 49] Обработчик wake-IPI должен стоять в handlers[] раньше, чем
    // первый AP выполнит sti: вектор без обработчика не получил бы EOI.
    smp_pool_init();

    if (acpi_num_cpus <= 1) {
        serial_puts("[smp] uniprocessor — AP startup skipped\n");
        cpu_stats[0].lapic_id = lapic_get_id();
        cpu_stats[0].online   = true;
        return;
    }

    serial_puts("[smp] starting APs, total CPUs=");
    serial_print_dec(acpi_num_cpus);
    serial_puts("\n");

    // 1) Зарезервируем страницу 0x8000 в PMM (она в первом 1 MB,
    //    уже формально reserved, но perezakreplyaem явно).
    pmm_reserve_region(TRAMPOLINE_PHYS, PAGE_SIZE);

    // 2) Скопируем trampoline blob в 0x8000.
    uint64_t blob_size = (uint64_t)(trampoline_end - trampoline_start);
    if (blob_size > PAGE_SIZE) {
        serial_puts("[smp] FATAL: trampoline > 4 KB\n");
        return;
    }
    uint8_t* dst = (uint8_t*)(uintptr_t)TRAMPOLINE_PHYS;
    for (uint64_t i = 0; i < blob_size; i++) dst[i] = trampoline_start[i];

    // 3) Заполним служебные данные внутри скопированного blob'а.
    //    Мы знаем смещения tramp_pml4_phys / tramp_ap_stacks_top / tramp_ap_slot
    //    относительно trampoline_start через арифметику.
    uint64_t off_pml4   = (uint64_t)((uint8_t*)&tramp_pml4_phys     - trampoline_start);
    uint64_t off_stacks = (uint64_t)((uint8_t*)&tramp_ap_stacks_top - trampoline_start);
    uint64_t off_slot   = (uint64_t)((uint8_t*)&tramp_ap_slot       - trampoline_start);

    *(uint64_t*)(dst + off_pml4)   = paging_kernel_pml4_phys();
    *(uint64_t*)(dst + off_stacks) = (uint64_t)(uintptr_t)ap_stacks_top;
    *(uint64_t*)(dst + off_slot)   = 1;       // слот 0 = BSP

    // 4) Аллоцируем стеки для AP. Слот 0 (BSP) — пропускаем.
    for (int s = 1; s < acpi_num_cpus && s < MAX_CPUS; s++) {
        uint64_t base = pmm_alloc_pages(AP_STACK_PAGES);
        if (!base) {
            serial_puts("[smp] FATAL: cannot alloc AP stack\n");
            return;
        }
        ap_stacks_top[s] = base + AP_STACK_SIZE;     // top растёт вниз
    }

    // 5) BSP info.
    uint32_t bsp_id = lapic_get_id();
    acpi_bsp_lapic_id = bsp_id;
    cpu_stats[0].lapic_id = bsp_id;
    cpu_stats[0].online   = true;

    // 6) INIT-SIPI-SIPI для каждого не-BSP CPU.
    for (int i = 0; i < acpi_num_cpus; i++) {
        if (acpi_lapic_ids[i] == bsp_id) continue;

        // INIT (assert + deassert).
        lapic_send_ipi(acpi_lapic_ids[i], 0, LAPIC_DELIV_INIT, 1, 1);
        busy_wait_ms(10);
        lapic_send_ipi(acpi_lapic_ids[i], 0, LAPIC_DELIV_INIT, 0, 1);
        busy_wait_ms(2);

        // SIPI ×2 (стандарт Intel: посылать дважды).
        uint8_t sipi_vector = TRAMPOLINE_PHYS >> 12;     // = 0x08
        lapic_send_ipi(acpi_lapic_ids[i], sipi_vector, LAPIC_DELIV_SIPI, 1, 0);
        busy_wait_ms(2);
        lapic_send_ipi(acpi_lapic_ids[i], sipi_vector, LAPIC_DELIV_SIPI, 1, 0);
        busy_wait_ms(2);
    }

    // 7) Ждём, пока все APы рапортуют о готовности (макс. 500 мс).
    for (int i = 0; i < 50; i++) {
        if ((int)smp_cpus_started >= acpi_num_cpus) break;
        busy_wait_ms(10);
    }

    serial_puts("[smp] cores up: ");
    serial_print_dec(smp_cpus_started);
    serial_puts("/");
    serial_print_dec(acpi_num_cpus);
    serial_puts("\n");
}

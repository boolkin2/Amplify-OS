// ============================================================================
// smp.c - раскрутка дополнительных ядер (Application Processors).
// ----------------------------------------------------------------------------
// BSP последовательно отправляет каждому AP пару INIT/SIPI; AP стартует с
// 16-битного трамплина в 0x8000, переходит в защищённый режим, грузит наш
// page directory, инкрементит счётчик smp_cpus_started и уходит в
// бесконечный idle loop с per-CPU статистикой.
// ============================================================================

#include "smp.h"
#include "acpi.h"
#include "lapic.h"
#include "ioapic.h"
#include "cpu_stats.h"
#include "spinlock.h"
#include "smp_work.h"
#include "../../vga.h"
#include "../../drivers/timer.h"
#include "../../memory/paging.h"
#include "../../memory/memory.h"

volatile uint32_t smp_cpus_started = 1;

extern uint32_t trampoline_start;
extern uint32_t trampoline_end;

#define TRAMPOLINE_ADDR 0x8000

static spinlock_t g_smp_print_lock = { 0 };

void ap_main(void) {
    lapic_init();

    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(paging_get_current_directory())
        : "memory"
    );

    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");

    uint32_t lapic_id = lapic_get_id();

    int my_slot = (int)__sync_fetch_and_add(&smp_cpus_started, 1);
    if (my_slot >= 0 && my_slot < MAX_CPUS) {
        cpu_stats[my_slot].lapic_id   = lapic_id;
        cpu_stats[my_slot].busy_ticks = 0;
        cpu_stats[my_slot].idle_ticks = 0;
        cpu_stats[my_slot].online     = true;
    }

    spinlock_acquire(&g_smp_print_lock);
    vga_set_color(VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK);
    vga_puts("[SMP] AP online: LAPIC ID=");
    vga_print_dec(lapic_id);
    vga_puts(" slot=");
    vga_print_dec(my_slot);
    vga_println("");
    spinlock_release(&g_smp_print_lock);

    // Базовая загрузка (декоративная busy/idle петля), но с регулярными
    // проверками общей work-queue — если есть реальная задача, AP подбирает её
    // прямо посреди фазы и выполняет, что и зачитывается как busy_ticks.
    uint32_t phase_busy = 8000  + (lapic_id * 1500);
    uint32_t phase_idle = 24000 + (lapic_id * 2500);
    const uint32_t POLL_EVERY = 256;  // как часто щупать очередь работ

    while (1) {
        // ----- Busy phase + work polling -----
        for (uint32_t i = 0; i < phase_busy; i++) {
            if (my_slot >= 0 && my_slot < MAX_CPUS) {
                cpu_stats[my_slot].busy_ticks++;
            }
            if ((i & (POLL_EVERY - 1)) == 0) {
                // Проверка дешёвая (один TAS на пустой очереди), 1/256 итераций.
                while (smp_run_one_if_any()) {
                    if (my_slot >= 0 && my_slot < MAX_CPUS) {
                        cpu_stats[my_slot].busy_ticks += 64; // работа = busy
                    }
                }
            }
            __asm__ volatile ("nop");
        }
        // ----- Idle phase + work polling -----
        // В idle поллим ЧАЩЕ — задача должна быстро подхватиться, как только
        // появилась.
        for (uint32_t i = 0; i < phase_idle; i++) {
            if (my_slot >= 0 && my_slot < MAX_CPUS) {
                cpu_stats[my_slot].idle_ticks++;
            }
            if ((i & 63) == 0) {
                if (smp_run_one_if_any()) {
                    if (my_slot >= 0 && my_slot < MAX_CPUS) {
                        cpu_stats[my_slot].busy_ticks += 64;
                    }
                    // Прерываем idle — может прилететь ещё работа.
                    break;
                }
            }
            __asm__ volatile ("pause");
        }
    }
}

void smp_init(void) {
    cpu_stats_init();
    smp_work_init();

    if (num_cpus <= 1) {
        vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        vga_println("[SMP] uniprocessor - AP wake-up skipped");
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("[SMP] starting APs, total CPUs: ");
    vga_print_dec(num_cpus);
    vga_println("");

    uint32_t trampoline_len = (uint32_t)&trampoline_end - (uint32_t)&trampoline_start;
    memcpy((void*)TRAMPOLINE_ADDR, (void*)&trampoline_start, trampoline_len);

    bsp_lapic_id = lapic_get_id();
    cpu_stats[0].lapic_id = bsp_lapic_id;
    cpu_stats[0].online   = true;

    for (int i = 0; i < num_cpus; i++) {
        if (cpu_lapic_ids[i] == bsp_lapic_id) continue;

        lapic_send_ipi(cpu_lapic_ids[i], 0, 5, 1, 1);
        timer_sleep(10);
        lapic_send_ipi(cpu_lapic_ids[i], 0, 5, 0, 1);
        timer_sleep(2);

        uint8_t vector = TRAMPOLINE_ADDR >> 12;
        lapic_send_ipi(cpu_lapic_ids[i], vector, 6, 1, 0);
        timer_sleep(2);
        lapic_send_ipi(cpu_lapic_ids[i], vector, 6, 1, 0);
        timer_sleep(2);
    }

    for (int spin = 0; spin < 50; spin++) {
        if ((int)smp_cpus_started >= num_cpus) break;
        timer_sleep(10);
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("[SMP] cores up: ");
    vga_print_dec(smp_cpus_started);
    vga_puts("/");
    vga_print_dec(num_cpus);
    vga_println("");
}

// ============================================================================
// kernel64_main.c — точка входа AMPLIFY-OS в 64-битном режиме.
// ----------------------------------------------------------------------------
// Этап 37.1: GDT64 + IDT64 + VESA + PIC remap + PIT @ 100 Hz.
// Этап 37.2: + PMM + heap + 4-level paging.
// Этап 37.3: + cpu_detect + ACPI + LAPIC + IOAPIC + SMP boot.
// Этап 37.5: + VFS/initrd/FAT32/ATA/AC97/RTL8139/DHCP/DNS/TCP.
// Этап 37.6a-b: keyboard, mouse, GUI demo (3 окна, drag).
// Этап 37.6c-1: timer callbacks, RTC, speaker, power, audio, http,
//               расширенный 18-командный shell, login screen.
// ============================================================================

#include <stdint.h>
#include "arch/x86_64/multiboot2.h"
#include "arch/x86_64/serial.h"
#include "arch/x86_64/fb.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/pmm.h"
#include "arch/x86_64/heap.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/cpu_stats.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/syscall.h"
#include "arch/x86_64/user_mode.h"
#include "arch/x86_64/vm.h"
#include "arch/x86_64/elf.h"
#include "arch/x86_64/task.h"
#include "arch/x86_64/vfs.h"
#include "arch/x86_64/initrd.h"
#include "arch/x86_64/ata.h"
#include "arch/x86_64/fat32.h"
#include "arch/x86_64/pci.h"
#include "arch/x86_64/netif.h"
#include "arch/x86_64/arp.h"
#include "arch/x86_64/ipv4.h"
#include "arch/x86_64/icmp.h"
#include "arch/x86_64/udp.h"
#include "arch/x86_64/tcp.h"
#include "arch/x86_64/dhcp.h"
#include "arch/x86_64/dns.h"
#include "arch/x86_64/rtl8139.h"
#include "arch/x86_64/e1000.h"
#include "arch/x86_64/ac97.h"
#include "arch/x86_64/disk_io.h"
#include "arch/x86_64/pipe.h"
#include "arch/x86_64/signal.h"
#include "arch/x86_64/unix_socket.h"
#include "arch/x86_64/shm.h"
#include "arch/x86_64/swap.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/memory.h"
#include "arch/x86_64/keyboard.h"
#include "arch/x86_64/shell.h"
#include "arch/x86_64/gui.h"
// 37.6c-1: новые подсистемы.
#include "arch/x86_64/timer.h"
#include "arch/x86_64/rtc.h"
#include "arch/x86_64/speaker.h"
#include "arch/x86_64/power.h"
#include "arch/x86_64/audio.h"
#include "arch/x86_64/http.h"
#include "arch/x86_64/login.h"
// 37.6c-2: полноценный графический стек.
#include "arch/x86_64/graphics.h"
#include "arch/x86_64/doublebuf.h"
#include "arch/x86_64/window.h"
#include "arch/x86_64/cursor.h"
#include "arch/x86_64/wm.h"
#include "arch/x86_64/desktop.h"
#include "arch/x86_64/apps.h"
// 37.6c-4: безопасность и first-boot.
#include "arch/x86_64/users.h"
#include "arch/x86_64/firstboot.h"
#include "arch/x86_64/netcfg.h"
// 37.6c-5: splash screen + единая тема.
#include "arch/x86_64/splash.h"
#include "arch/x86_64/ui_theme.h"
// Этап 41: GPU-ускорение через virtio-gpu + DRM.
#include "arch/x86_64/virtio_gpu.h"
#include "arch/x86_64/drm.h"
// Этап 45: пакетный менеджер Amply.
#include "arch/x86_64/amply.h"
// Этап 46: песочницы — namespaces, cgroups, seccomp.
#include "arch/x86_64/ns.h"
#include "arch/x86_64/cgroup.h"
#include "arch/x86_64/seccomp.h"
#include "arch/x86_64/box.h"
// Этап 47.1: криптографические примитивы (SHA, HMAC, AES, ChaCha20-Poly1305, RNG).
#include "arch/x86_64/crypto.h"
// Этап 47.3: X.509 + trust store.
#include "arch/x86_64/crypto_x509.h"
#include "arch/x86_64/crypto_ca.h"
// Этап 47.4: TLS 1.2 client.
#include "arch/x86_64/tls.h"
#include "arch/x86_64/https.h"
#include "arch/x86_64/installer.h"
#include "arch/x86_64/installer_engine.h"
// Этап 48: шифрование системного раздела (LUKS1 + dm-crypt-аналог).
#include "arch/x86_64/fde.h"
#include "arch/x86_64/mind.h"
#include "arch/x86_64/vmx.h"
#include "arch/x86_64/phantom.h"
#include "arch/x86_64/rune.h"

extern void exception_panic(registers64_t* r);
extern uint8_t stack_top[];

// Из user_test.asm (этап 37.4a) — оставлено для обратной совместимости,
// в demo не используется (теперь грузим ELF из initrd).
extern uint8_t user_test_start[];
extern uint8_t user_test_end[];

// ----------------------------------------------------------------------------
// Логотип AMPLIFY-OS (треугольник из ASCII).
// ----------------------------------------------------------------------------
static void draw_logo(int x0, int y0, uint32_t fg, uint32_t bg) {
    static const char* lines[] = {
        "        /\\        ",
        "       /  \\       ",
        "      / /\\ \\      ",
        "     / /  \\ \\     ",
        "    / /    \\ \\    ",
        "   / /  /\\  \\ \\   ",
        "  / /  /  \\  \\ \\  ",
        " / /__/____\\__\\ \\ ",
        "/__________________\\",
        0
    };
    for (int i = 0; lines[i]; i++) fb_draw_string(x0, y0 + i * 16, lines[i], fg, bg);
}

// ----------------------------------------------------------------------------
// Этап 37.5a: spawn задачи из ELF blob'а (как в 37.4c, но blob теперь
// читается из initrd через initrd_get_file, а не embedded в kernel).
// На 37.6c-1 используется только smoke_test_initrd_elf (отключён).
// ----------------------------------------------------------------------------
__attribute__((unused))
static task_t* spawn_user_task(const uint8_t* elf_blob, uint64_t blob_size) {
    vm_space_t* vm = vm_create_user();
    if (!vm) return 0;

    uint64_t entry = elf64_load(elf_blob, blob_size, vm);
    if (!entry) return 0;

    uint64_t stack_va_base = USER_BASE + 0x100000;
    for (int i = 0; i < 2; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) return 0;
        if (!vm_map_user_page(vm, stack_va_base + i * PAGE_SIZE, pa, true)) return 0;
    }
    uint64_t user_rsp = stack_va_base + 2 * PAGE_SIZE - 16;

    return task_create_user(vm, entry, user_rsp);
}

// ----------------------------------------------------------------------------
// Этап 37.5a smoke-тест: загружаем counter.elf ИЗ INITRD, spawn 2 task'а.
// На 37.6c-1 не вызывается — оставлен на случай отладки task scheduler'а.
// ----------------------------------------------------------------------------
__attribute__((unused))
static void smoke_test_initrd_elf(void) {
    fb_console_set_color(0xFFAAAAFF, 0xFF101820);
    fb_console_puts("\n[smoke] VFS + initrd + ELF from initrd:\n");

    fb_console_puts("  VFS files: ");
    fb_console_print_dec(vfs_count());
    fb_console_puts("\n");
    for (int i = 0; i < vfs_count(); i++) {
        vfs_node_t* n = vfs_at(i);
        fb_console_puts("    "); fb_console_puts(n->name);
        fb_console_puts(" — "); fb_console_print_dec(n->size); fb_console_puts(" bytes\n");
    }

    uint64_t blob_size = 0;
    const uint8_t* blob = initrd_get_file("counter.elf", &blob_size);
    if (!blob) {
        fb_console_puts("  counter.elf not found in initrd!\n");
        return;
    }
    fb_console_puts("  counter.elf located in initrd ("); fb_console_print_dec(blob_size);
    fb_console_puts(" bytes)\n");

    task_t* t1 = spawn_user_task(blob, blob_size);
    task_t* t2 = spawn_user_task(blob, blob_size);
    if (!t1 || !t2) { fb_console_puts("  spawn FAIL\n"); return; }
    fb_console_puts("  spawned tid="); fb_console_print_dec(t1->tid);
    fb_console_puts(" and tid="); fb_console_print_dec(t2->tid); fb_console_puts("\n");
    fb_console_puts("  -> task_start (round-robin cooperative):\n");
    fb_console_set_color(0xFFFFFFFF, 0xFF101820);

    task_start();   // never returns
}

// (smoke_test_elf из 37.4b удалена — её single-task сценарий покрывается
// 37.4c, где spawn_user_task делает то же самое для счётчика.)

// ----------------------------------------------------------------------------
// LAPIC обработчики (этап 37.3): timer/error/spurious — все «no-op + EOI».
// ----------------------------------------------------------------------------
static void lapic_timer_isr(registers64_t* r) { (void)r; lapic_eoi(); }
static void lapic_error_isr(registers64_t* r) {
    (void)r;
    serial_puts("[lapic] ERROR vector triggered\n");
    lapic_eoi();
}
static void lapic_spurious_isr(registers64_t* r) {
    (void)r;
    // Spurious — НЕТ EOI (по Intel SDM).
}

// ----------------------------------------------------------------------------
static uint64_t count_available_ram(const mb2_info_t* info) {
    const mb2_tag_t* t = mb2_find_tag(info, MB2_TAG_MMAP);
    if (!t) return 0;
    const mb2_tag_mmap_t* mmap = (const mb2_tag_mmap_t*)t;
    uint64_t total = 0;
    uint32_t entry_count = (mmap->size - 16) / mmap->entry_size;
    for (uint32_t i = 0; i < entry_count; i++) {
        const mb2_mmap_entry_t* e =
            (const mb2_mmap_entry_t*)((const uint8_t*)mmap->entries + i * mmap->entry_size);
        if (e->type == 1) total += e->len;
    }
    return total;
}

// ----------------------------------------------------------------------------
// 37.6c-3: shim'ы для интеграции apps.c с desktop API,
// которые ждут `void (*)(void)` callback (без аргументов и возврата).
// ----------------------------------------------------------------------------
static void launch_about  (void) { (void)app_about_open();   }
static void launch_info   (void) { (void)app_amplifier_open(); }
static void launch_clock  (void) { (void)app_clock_open();   }
static void launch_matrix (void) { (void)app_matrix_open();  }
static void launch_calc   (void) { (void)app_calc_open();    }
static void launch_notes  (void) { (void)app_thread_open();  }
static void launch_term   (void) { (void)app_term_open();    }
static void launch_files  (void) { (void)app_strata_open();  }
static void launch_paint  (void) { (void)app_paint_open();   }
static void launch_embers (void) { (void)app_embers_open();  }
static void launch_browser(void) { (void)app_browser_open(); }
static void launch_taskmgr(void) { (void)app_amplifier_open(); }
// [этап E] Новые Pearl workspaces.
static void launch_signal(void) { (void)app_signal_open(); }
static void launch_arc(void) { (void)app_arc_open(); }
static void launch_resonance(void) { (void)app_resonance_open(); }
static void launch_core(void) { (void)app_core_open(); }
static void launch_facet(void) { (void)app_facet_open(); }
static void launch_mesh(void) { (void)app_mesh_open(); }
static void launch_phantom(void) { (void)app_phantom_open(); }   // [50.5]
static void launch_rune(void) { (void)app_rune_open(); }         // [51.4]
static void launch_capacity(void) { (void)app_capacity_open(); }
// [этап 44] 3D-куб через программный GLES.
static void launch_glcube (void) { (void)app_glcube_open();  }
// [этап 45] Amply Center — пакетный менеджер.
static void launch_amply  (void) { (void)app_amply_open();   }
// [этап 46] Sandbox Manager — namespaces / cgroups / seccomp.
static void launch_box    (void) { (void)app_box_open();     }

static void menu_reboot_click  (void) { power_reboot_with_curtain();   }
static void menu_shutdown_click(void) { power_shutdown_with_curtain(); }

// ----------------------------------------------------------------------------
// [38.1.1] Проверочное раздувание образа: make CFLAGS_EXTRA=-DAOS_BLOAT_BSS_MB=8
// добавляет в .bss массив в N МБ. Так тест 38.1 убеждается, что образ, конец
// которого заведомо за прежней «стеной» 8 МБ (CLAUDE.md §5.50), грузится.
// volatile и запись в оба конца — чтобы компилятор массив не выбросил.
#ifdef AOS_BLOAT_BSS_MB
static volatile uint8_t g_bloat_bss[(uint64_t)AOS_BLOAT_BSS_MB << 20];
#endif

void kernel_main64(uint64_t magic, uint64_t info_addr) {
    serial_init();
    serial_puts("\n[k64] AMPLIFY-OS 64-bit kernel (stage 37.3)\n");

    if (magic != MB2_BOOT_MAGIC) {
        serial_puts("[k64] FATAL: invalid Multiboot2 magic\n");
        for (;;) hlt();
    }

    const mb2_info_t* info = (const mb2_info_t*)(uintptr_t)info_addr;

    // ---- Framebuffer ---------------------------------------------------------
    const mb2_tag_framebuffer_t* fb =
        (const mb2_tag_framebuffer_t*)mb2_find_tag(info, MB2_TAG_FRAMEBUFFER);
    fb_init(fb);
    fb_console_reset();
    fb_console_set_color(0xFFFFFFFF, 0xFF101820);

    // ---- Архитектурное ядро --------------------------------------------------
    gdt64_init((uint64_t)stack_top);
    idt64_init();
    for (int i = 0; i < 32; i++) {
        if (i == 2) continue;       // NMI отдельно
        idt64_set_handler(i, exception_panic);
    }
    idt64_set_handler(0xEF, lapic_timer_isr);
    idt64_set_handler(0xFE, lapic_error_isr);
    idt64_set_handler(0xFF, lapic_spurious_isr);

    // ---- PIC + PIT (legacy IRQ-роутинг) -------------------------------------
    pic_remap(0x20, 0x28);
    // 37.6c-1: timer-таб��и��а должна быть инициализирована ДО pit_init,
    // потому что pit_isr → timer_tick дёргает её на каждом тике.
    timer_init();
    pit_init(100);

    // ---- Memory (этап 37.2) -------------------------------------------------
    paging_init();
#ifdef AOS_BLOAT_BSS_MB
    {
        // Массив обязан быть нулевым (его чистит загрузчик) и доступным на
        // запись на всём протяжении — проверяем начало, конец и шаг в 1 МБ.
        uint64_t n = sizeof(g_bloat_bss), bad = 0;
        for (uint64_t i = 0; i < n; i += 1ull << 20) if (g_bloat_bss[i]) bad++;
        if (g_bloat_bss[n - 1]) bad++;
        g_bloat_bss[0] = 0xA5; g_bloat_bss[n - 1] = 0x5A;
        if (g_bloat_bss[0] != 0xA5 || g_bloat_bss[n - 1] != 0x5A) bad++;
        serial_puts("[boot] bloat: ");
        serial_print_dec(n >> 20);
        serial_puts(" MB of extra .bss, ");
        serial_puts(bad ? "FAIL\n" : "zeroed and writable: OK\n");
    }
#endif
    pmm_init(info);
    // [41.2] Загрузчик мог положить initrd/инфоблок в диапазон кучи (GRUB-EFI
    // кладёт на 0x01600000) — выносим до того, как куча начнёт там писать.
    info = pmm_relocate_boot_data(info);
    heap_init();

    // ---- vm_space (этап 37.4b) ---------------------------------------------
    vm_init();

    // ---- VFS + initrd (этап 37.5a) -----------------------------------------
    vfs_init();
    initrd_init_from_mb2(info);

    // ---- Этап 57: определяем live/installer-режим по cmdline ---------------
    installer_init_from_mb2(info);

    // ---- ATA + FAT32 (этап 37.5b) ------------------------------------------
    ata_init();
    fat32_mount();
    // Этап 48: если /disk не нашёлся, а на диске есть LUKS-раздел системы —
    // том зашифрован; смонтируем его после экрана разблокировки (ниже,
    // когда будут клавиатура и графика).
    fde_scan();

    // ---- 37.6c-4: загрузка users (firstboot wizard вызывается ниже, после
    //              keyboard_init, иначе нельзя принять ввод) -----------------
    users_init();

    // ---- Audio (этап 37.5c-3 + 37.6c-1) ------------------------------------
    // audio_init() сам вызывает ac97_init() — поэтому отдельный вызов не нужен.
    audio_init();

    // ---- swap init (этап 37.5d-3a) ----------------------------------------
    swap_init();

    // ---- CPU + ACPI + APIC (этап 37.3) --------------------------------------
    // Должно быть ДО net_init — rtl8139 использует ioapic_route_irq, которому
    // нужен инициализированный IOAPIC и acpi_bsp_lapic_id.
    cpu_detect();
    acpi_init(info);
    lapic_init();             // BSP сам себя инициализирует
    ioapic_init();

    // ---- PCI + RTL8139 + сетевой стек (этап 37.5c-1) -----------------------
    pci_scan_dump();

    // ---- Этап 41: virtio-gpu detection ------------------------------------
    // Пробуем поднять virtio-gpu сразу после PCI-скана. Если устройства нет
    // (QEMU стартовал без -device virtio-gpu-pci) — функция вернёт false,
    // далее весь стек будет работать через VESA fb (см. doublebuf_swap).
    (void)virtio_gpu_init();

    net_init();
    // [48.2] Адрес интерфейсу назначает netcfg: режим из
    // /disk/Sanctum/network.conf (auto | dhcp | static). Для DHCP ждём адрес
    // до 5 с (в auto с сохранённой статикой — 3 с, потом запасной адрес);
    // дальше DHCP-клиент повторяет запросы и продлевает аренду из цикла WM
    // (netcfg_tick). Зашифрованный /disk в этот момент ещё закрыт — тогда файл
    // читается позже, netcfg_late_load() перед wm_main_loop.
    // Фейковый адрес slirp (10.0.2.15) при молчании DHCP не подставляется: в
    // настоящей сети он не маршрутизируется.
    if (netif_is_ready()) {
        sti();
        netcfg_boot();
    }

    // Гарантируем что ��рерывания включены перед SMP boot (PIT должен тикать
    // для busy_wait_ms в smp_init). Без sti() SMP зависает навечно.
    sti();

    // ---- SMP boot ------------------------------------------------------------
    smp_init();

    // ---- SYSCALL/SYSRET (этап 37.4a) -----------------------------------------
    syscall_init((uint64_t)stack_top);

    // ---- Баннер --------------------------------------------------------------
    draw_logo(40, 24, 0xFF60D0F0, 0xFF101820);

    fb_console_set_color(0xFFFFD200, 0xFF101820);
    fb_console_puts("================================================================================\n");
    fb_console_set_color(0xFF60D0F0, 0xFF101820);
    fb_console_puts("       AMPLIFY-OS 64-bit (Long Mode + memory + APIC + SMP)                      \n");
    fb_console_set_color(0xFFFFD200, 0xFF101820);
    fb_console_puts("================================================================================\n\n");

    fb_console_set_color(0xFFFFFFFF, 0xFF101820);
    fb_console_puts("Stage 37.6c-5 — UI refresh: splash + firstboot + login + theme.\n");
    fb_console_puts("12 GUI apps  |  WM + doublebuf  |  TCP/HTTP  |  FAT32 RW  |  AC97\n\n");

    // ---- CPU info ------------------------------------------------------------
    const cpu_info_t* ci = cpu_info();
    fb_console_set_color(0xFF80FF80, 0xFF101820);
    fb_console_puts(" CPU vendor: "); fb_console_puts(ci->vendor); fb_console_puts("\n");
    if (ci->brand[0]) {
        fb_console_puts(" CPU brand:  "); fb_console_puts(ci->brand); fb_console_puts("\n");
    }
    if (ci->hypervisor && ci->hv_vendor[0]) {
        fb_console_puts(" Hypervisor: "); fb_console_puts(ci->hv_vendor); fb_console_puts("\n");
    }
    fb_console_set_color(0xFFFFFFFF, 0xFF101820);
    fb_console_puts(" features: ");
    if (ci->fpu)        fb_console_puts("FPU ");
    if (ci->pae)        fb_console_puts("PAE ");
    if (ci->apic)       fb_console_puts("APIC ");
    if (ci->msr)        fb_console_puts("MSR ");
    if (ci->mmx)        fb_console_puts("MMX ");
    if (ci->fxsr)       fb_console_puts("FXSR ");
    if (ci->sse)        fb_console_puts("SSE ");
    if (ci->sse2)       fb_console_puts("SSE2 ");
    if (ci->sse3)       fb_console_puts("SSE3 ");
    if (ci->ssse3)      fb_console_puts("SSSE3 ");
    if (ci->sse41)      fb_console_puts("SSE4.1 ");
    if (ci->sse42)      fb_console_puts("SSE4.2 ");
    if (ci->sse4a)      fb_console_puts("SSE4A ");
    if (ci->fma)        fb_console_puts("FMA ");
    if (ci->popcnt)     fb_console_puts("POPCNT ");
    if (ci->aes)        fb_console_puts("AES ");
    if (ci->pclmulqdq)  fb_console_puts("PCLMUL ");
    if (ci->sha)        fb_console_puts("SHA ");
    if (ci->avx)        fb_console_puts("AVX ");
    if (ci->avx2)       fb_console_puts("AVX2 ");
    if (ci->avx512f)    fb_console_puts("AVX512F ");
    if (ci->bmi1)       fb_console_puts("BMI1 ");
    if (ci->bmi2)       fb_console_puts("BMI2 ");
    if (ci->lzcnt)      fb_console_puts("LZCNT ");
    if (ci->rdrand)     fb_console_puts("RDRAND ");
    if (ci->rdseed)     fb_console_puts("RDSEED ");
    if (ci->xsave)      fb_console_puts("XSAVE ");
    if (ci->x2apic)     fb_console_puts("x2APIC ");
    if (ci->nx)         fb_console_puts("NX ");
    if (ci->syscall)    fb_console_puts("SYSCALL ");
    if (ci->rdtscp)     fb_console_puts("RDTSCP ");
    if (ci->long_mode)  fb_console_puts("LM ");
    if (ci->svm)        fb_console_puts("SVM ");
    if (ci->hypervisor) fb_console_puts("HV ");
    fb_console_puts("\n");

    // ---- Memory --------------------------------------------------------------
    pmm_stats_t  ps = pmm_get_stats();
    heap_stats_t hs = heap_get_stats();
    uint64_t ram = count_available_ram(info);
    fb_console_puts(" RAM: ");                  fb_console_print_dec(ram / MB);
    fb_console_puts(" MB usable. heap: ");       fb_console_print_dec(hs.total_size / MB);
    fb_console_puts(" MB. PMM: ");               fb_console_print_dec(ps.free_pages);
    fb_console_puts(" pages free.\n");

    // ---- ACPI / APIC ---------------------------------------------------------
    fb_console_set_color(0xFFFFD200, 0xFF101820);
    fb_console_puts("\n APIC:\n");
    fb_console_set_color(0xFFFFFFFF, 0xFF101820);
    fb_console_puts("   LAPIC base:    0x"); fb_console_print_hex(acpi_lapic_phys, 16);
    fb_console_puts("\n   BSP LAPIC ID: ");   fb_console_print_dec(acpi_bsp_lapic_id);
    fb_console_puts("\n   IOAPIC base:  0x"); fb_console_print_hex(acpi_ioapic_phys, 16);
    fb_console_puts(" (");
    fb_console_print_dec(ioapic_max_redir); fb_console_puts(" GSIs)\n");
    fb_console_puts("   ACPI CPU count: "); fb_console_print_dec(acpi_num_cpus);
    fb_console_puts(", LAPIC IDs: ");
    for (int i = 0; i < acpi_num_cpus; i++) {
        fb_console_print_dec(acpi_lapic_ids[i]);
        if (i + 1 < acpi_num_cpus) fb_console_puts(",");
    }
    fb_console_puts("\n");

    // ---- SMP runtime status -------------------------------------------------
    fb_console_set_color(0xFFFFD200, 0xFF101820);
    fb_console_puts("\n SMP:\n");
    fb_console_set_color(0xFFFFFFFF, 0xFF101820);
    fb_console_puts("   cores online: "); fb_console_print_dec(smp_cpus_started);
    fb_console_puts("/"); fb_console_print_dec(acpi_num_cpus); fb_console_puts("\n");
    for (int i = 0; i < (int)smp_cpus_started && i < MAX_CPUS; i++) {
        fb_console_puts("   CPU"); fb_console_print_dec(i);
        fb_console_puts(": LAPIC=");  fb_console_print_dec(cpu_stats[i].lapic_id);
        fb_console_puts(cpu_stats[i].online ? " ONLINE" : " offline");
        fb_console_puts("\n");
    }

    // ---- Этап 37.6a/b: keyboard + mouse инициализация ---------------------
    keyboard_init();
    {
        extern void mouse_init(void);
        mouse_init();
    }
    // [41.2] Время: таймер LAPIC 1000 Гц вместо PIT 100 Гц. Здесь прерывания
    // уже разрешены и PIT тикает — он нужен для калибровки.
    {
        extern bool lapic_timer_start(void);
        (void)lapic_timer_start();
    }
    // [41.2] virtio-input (клавиатура + планшет), если устройства есть: с
    // этого момента ввод идёт через очереди virtio и прерывания MSI-X, PS/2
    // остаётся запасным путём. До экрана разблокировки FDE — он тоже ждёт ввод.
    {
        extern int virtio_input_init(void);
        (void)virtio_input_init();
    }
    // [41.2] virtio-blk поднят ещё в ata_init (до LAPIC и sti) и до сих пор
    // ждал ответы опросом; теперь прерывания разрешены — проверяем MSI-X.
    {
        extern void virtio_blk_irq_selftest(void);
        virtio_blk_irq_selftest();
    }

    // ---- 37.6c-5: doublebuf + splash screen до firstboot/login ------------
    // (firstboot и login теперь графические — требуют doublebuf)
    if (!doublebuf_init()) {
        serial_puts("[k64] doublebuf_init FAILED\n");
        for(;;) hlt();
    }

    // ---- Этап 41: подключаем primary FB к virtio-gpu ----------------------
    // doublebuf уже создал RAM-буфер кадра. drm_init создаёт GPU-ресурс,
    // attach_backing к этому RAM-буферу и set_scanout. Если virtio-gpu не
    // активен — drm_init тихо вернёт false, doublebuf_swap пойдёт по VESA.
    (void)drm_init();

    // ---- Этап 42: compositor (тройная буферизация, тени, AA, анимации) ----
    // Инициализируем ПОСЛЕ drm_init и doublebuf_init. Если не удастся --
    // WM продолжит работать через старый doublebuf-путь (graceful fallback).
    {
        extern bool compositor_init(void);
        (void)compositor_init();
    }

    // ---- Этап 41 smoke-test + bench: property-checks + 16 GPU-present'ов
    //      vs 16 CPU-копий в LFB. Одна serial-простыня после drm_init, до
    //      splash; парсится test_stage41_gpu.sh (метрика 5-10× CPU + P1-P6).
    //      drm_self_test работает ВСЕГДА (и при VESA-fallback тоже —
    //      проверяет инвариант P1, P3, P4, P5). bench только при active.
    drm_self_test();
    if (drm_active()) {
        (void)drm_self_benchmark(16);
    }

    // ---- Этап 48: разблокировка зашифрованного системного тома ------------
    // Самая ранняя точка, где есть и клавиатура, и кадр, и — главное — ещё
    // никто не обращался к /disk: Amply, smoke-тесты, firstboot и login идут
    // ниже.  users_init() повторяем: при первом вызове /disk не было.
    // В live-режиме (установщик) чужой зашифрованный диск не трогаем.
    if (fde_pending() && !installer_should_run()) {
        fde_unlock_screen();
        users_init();
    }

    // ---- Этап 46: песочницы — namespaces, cgroups, seccomp ---------------
    // Инициализируем ДО amply_init и любого task_create (root-объекты
    // должны существовать к моменту первого spawn'а — они приклеиваются как
    // дефолтные ссылки в task_create_user/thread).
    ns_init();
    cgroup_init();
    seccomp_init();

    // ---- Этап 45: пакетный менеджер Amply (после SMP/disks, до splash) ----
    amply_init();

    // ---- Этап 45 smoke: install→list→verify→remove cycle над cyber-theme.
    //                     Подтягивает welcome как зависимость.  Идёт до
    //                     firstboot, чтобы headless-тест мог увидеть его.
    {
        static char log_buf[3072];
        log_buf[0] = 0;
        serial_puts("\n[smoke 45] Amply install cyber-theme (with deps)\n");
        int r = amply_install("cyber-theme", log_buf, sizeof(log_buf));
        serial_puts(log_buf);
        serial_puts("[smoke 45] install r=");
        serial_print_dec((uint64_t)(r < 0 ? -r : r));
        serial_puts(r == 0 ? " (OK)\n" : " (FAIL)\n");

        serial_puts("[smoke 45] db_count=");
        serial_print_dec((uint64_t)amply_db_count());
        serial_puts("\n");

        log_buf[0] = 0;
        amply_verify_installed("cyber-theme", log_buf, sizeof(log_buf));
        serial_puts(log_buf);

        log_buf[0] = 0;
        serial_puts("[smoke 45] Amply remove cyber-theme\n");
        amply_remove("cyber-theme", log_buf, sizeof(log_buf));
        serial_puts(log_buf);
        serial_puts("[smoke 45] db_count after remove=");
        serial_print_dec((uint64_t)amply_db_count());
        serial_puts("\n");
    }

    // ---- Этап 46 smoke: cgroup + seccomp + ns_unshare CoW + box_run ----
    // Несколько чистых property-проверок, чтобы headless-тест 46 видел
    // ожидаемые строки в serial-логе.
    {
        serial_puts("\n[smoke 46] sandbox runtime self-test\n");

        // (1) cgroup memory limit.
        {
            cgroup_t* c = cgroup_create(cgroup_root(), "smoke46");
            if (!c) { serial_puts("[smoke 46] cgroup_create FAIL\n"); }
            else {
                cgroup_set_limit_mem(c, 4096);
                int r1 = cgroup_charge_mem(c, 2048);
                int r2 = cgroup_charge_mem(c, 4000);   // должен fail (6048>4096)
                serial_puts("[smoke 46] cgroup mem r1=");
                serial_print_dec((uint64_t)(int64_t)r1);
                serial_puts(" r2=");
                serial_print_dec((uint64_t)(int64_t)r2);
                serial_puts(" oom_count=");
                serial_print_dec(c->mem_oom);
                serial_puts(r1 == 0 && r2 == -1 && c->mem_oom == 1 ? " (OK)\n" : " (FAIL)\n");
                cgroup_charge_mem(c, -2048);
                cgroup_destroy(c);
            }
        }

        // (2) seccomp strict.
        {
            seccomp_filter_t* f = seccomp_profile("strict");
            int sr1 = seccomp_check(f, 0x101);   // WRITE allowed
            int sr2 = seccomp_check(f, 0x103);   // OPEN  blocked
            serial_puts("[smoke 46] seccomp strict write=");
            serial_print_dec((uint64_t)(int64_t)sr1);
            serial_puts(" open=");
            serial_print_dec((uint64_t)(int64_t)sr2);
            serial_puts(" blocked=");
            serial_print_dec(f->total_blocked);
            serial_puts(sr1 == 0 && sr2 == -2 && f->total_blocked == 1 ? " (OK)\n" : " (FAIL)\n");
            seccomp_release(f);
        }

        // (3) ns_unshare CoW.
        {
            ns_set_t* nw = ns_unshare(ns_root(), AOS_CLONE_NEWUTS | AOS_CLONE_NEWPID);
            bool diff = (nw && nw->uts != ns_root()->uts && nw->pid != ns_root()->pid);
            bool same = (nw && nw->net == ns_root()->net);
            serial_puts("[smoke 46] ns_unshare(NEWUTS|NEWPID) diff_uts_pid=");
            serial_puts(diff ? "yes" : "no");
            serial_puts(" same_net=");
            serial_puts(same ? "yes" : "no");
            serial_puts(diff && same ? " (OK)\n" : " (FAIL)\n");
            if (nw) ns_release(nw);
        }

        // (4) ns_get_summaries должен видеть >=5 объектов (root pid/uts/mnt/
        //     net/user — даже без unshare'ов).
        {
            ns_summary_t s[16];
            int n = ns_get_summaries(s, 16);
            serial_puts("[smoke 46] ns_summaries count=");
            serial_print_dec((uint64_t)n);
            serial_puts(n >= 5 ? " (OK)\n" : " (FAIL)\n");
        }

        // (5) Mount-ns gate: создать isolated mnt-ns с allow-prefix
        //     "/disk/Sanctum/", и проверить что путь "/secret" блокируется,
        //     а "/disk/Sanctum/foo" — нет.  Делаем через прямой вызов
        //     ns_mount_path_allowed без подмены текущего ns у kernel-task
        //     (чтобы не зацепить amply/users).
        {
            ns_set_t* nw = ns_unshare(ns_root(), AOS_CLONE_NEWNS);
            // Привязываем к временной фейковой task_t — стек-копия достаточна
            // потому что ns_mount_path_allowed читает t->ns->mnt.
            task_t fake;
            for (uint32_t k = 0; k < sizeof(fake); k++) ((uint8_t*)&fake)[k] = 0;
            fake.ns = nw;
            ns_mnt_allow(&fake, "/disk/Sanctum/");
            bool a = ns_mount_path_allowed(&fake, "/disk/Sanctum/foo");
            bool b = ns_mount_path_allowed(&fake, "/secret");
            serial_puts("[smoke 46] mnt-ns allow=");
            serial_puts(a ? "yes" : "no");
            serial_puts(" deny=");
            serial_puts(!b ? "yes" : "no");
            serial_puts(a && !b ? " (OK)\n" : " (FAIL)\n");
            if (nw) ns_release(nw);
        }

        serial_puts("[smoke 46] sandbox initialized, cgroup_count=");
        serial_print_dec((uint64_t)cgroup_count());
        serial_puts("\n");
    }

    // ---- Этап 47.1 smoke: криптографические примитивы + self-test ---------
    // crypto_init() делает: CPUID-detect, init PRNG, прогон NIST/RFC vectors.
    // Если хотя бы один KAT провалится — TLS-стек в 47.4 не активируется,
    // потому что использовать ломаную крипту хуже, чем не иметь её вообще.
    {
        serial_puts("\n[smoke 47.1] crypto subsystem boot\n");
        bool ok = crypto_init();
        serial_puts("[smoke 47.1] crypto_init=");
        serial_puts(ok ? "PASS" : "FAIL");
        serial_puts("\n");

        // Минимальный round-trip AES-GCM с PRNG-сгенерированным ключом, чтобы
        // headless-тест видел: ключ ИЗ нашего RNG проходит через GCM
        // encrypt+decrypt с валидным tag'ом.  Это не security test —
        // это integration smoke.
        if (ok) {
            uint8_t key[16];
            uint8_t iv[12];
            crypto_rand_bytes(key, 16);
            crypto_rand_bytes(iv,  12);
            const char* msg = "AMPLIFY-OS crypto smoke 47.1";
            size_t mlen = 28;
            uint8_t ct[64], tag[16], pt2[64];
            crypto_gcm_ctx_t gcm;
            crypto_gcm_init(&gcm, key, 16);
            crypto_gcm_encrypt(&gcm, iv, 0, 0, (const uint8_t*)msg, ct, mlen, tag);
            bool rt = crypto_gcm_decrypt(&gcm, iv, 0, 0, ct, pt2, mlen, tag);
            int eq = 1;
            for (size_t i = 0; i < mlen; i++) if (pt2[i] != (uint8_t)msg[i]) eq = 0;
            serial_puts("[smoke 47.1] GCM round-trip with PRNG-key: ");
            serial_puts(rt && eq ? "OK" : "FAIL");
            serial_puts("\n");
        }
    }

    // ---- Этап 47.2 smoke: X25519 ECDH self-roundtrip -----------------------
    // Простой integration-тест: генерируем 2 случайных приватных ключа,
    // обмениваемся public keys (= scalar * base), сравниваем shared secret.
    // Это смоук-тест X25519 с реальным PRNG-источником.
    {
        serial_puts("[smoke 47.2] X25519 ECDH self-roundtrip\n");
        uint8_t alice_sk[32], bob_sk[32];
        uint8_t alice_pk[32], bob_pk[32];
        uint8_t ss_alice[32], ss_bob[32];
        crypto_rand_bytes(alice_sk, 32);
        crypto_rand_bytes(bob_sk,   32);
        crypto_x25519(alice_pk, alice_sk, crypto_x25519_basepoint);
        crypto_x25519(bob_pk,   bob_sk,   crypto_x25519_basepoint);
        crypto_x25519(ss_alice, alice_sk, bob_pk);
        crypto_x25519(ss_bob,   bob_sk,   alice_pk);
        int match = 1;
        for (int i = 0; i < 32; i++) if (ss_alice[i] != ss_bob[i]) match = 0;
        serial_puts("[smoke 47.2] ss_alice == ss_bob: ");
        serial_puts(match ? "OK" : "FAIL");
        serial_puts("\n");
    }

    // ---- Этап 47.3 smoke: X.509 + trust store -----------------------------
    {
        serial_puts("[smoke 47.3] X.509 trust store boot\n");
        serial_puts("[smoke 47.3] CA store size: ");
        serial_print_dec((uint64_t)g_crypto_ca_count);
        serial_puts("\n");
        // [48.2] Корней стало 150 (весь набор Mozilla). Разобрать должны все
        // (это быстро), а подпись проверяем у каждого 16-го: проверка всех
        // стоила 1.7 с при каждой загрузке.
        int parsed = 0, sampled = 0, verified = 0;
        for (size_t i = 0; i < g_crypto_ca_count; i++) {
            x509_cert_t cert;
            if (x509_parse(g_crypto_ca_store[i].der,
                           g_crypto_ca_store[i].der_len, &cert)) {
                parsed++;
                if (i % 16 == 0) {
                    sampled++;
                    if (x509_verify_signature(&cert, &cert)) verified++;
                }
            }
        }
        serial_puts("[smoke 47.3] parsed=");
        serial_print_dec((uint64_t)parsed);
        serial_puts(" self-signed-verified=");
        serial_print_dec((uint64_t)verified);
        serial_puts("/");
        serial_print_dec((uint64_t)sampled);
        serial_puts((parsed == (int)g_crypto_ca_count && sampled > 0 &&
                     verified == sampled) ? " (OK)\n" : " (FAIL)\n");
    }

    // ---- Этап 47.4 smoke: TLS init + ClientHello build (без сети) ----------
    // Проверяем, что tls_client_init не падает.  Реальный handshake к серверу —
    // в этапе 47.5 (через https shell-команду).
    {
        serial_puts("[smoke 47.4] TLS context init\n");
        // [38.1.0] Из кучи: 117 КБ в .bss ради одной проверки при загрузке —
        // лишний вес образа (CLAUDE.md §5.50).
        tls_ctx_t* tc = (tls_ctx_t*)kcalloc(1, sizeof(tls_ctx_t));
        bool ok = false;
        if (tc) {
            tls_client_init(tc, -1, "example.com");
            ok = (tc->state == TLS_STATE_INIT) &&
                 (tc->hostname[0] == 'e') &&
                 (tc->hostname[7] == '.');   // "example.com"
            kfree(tc);
        }
        serial_puts("[smoke 47.4] ctx ready: ");
        serial_puts(ok ? "OK\n" : "FAIL\n");
    }

    // ---- Этап 47.5 smoke: HTTPS + TCP listen/accept ------------------------
    // (1) tcp_listen + state check (без accept'а — нужен входящий SYN).
    {
        serial_puts("[smoke 47.5] HTTPS subsystem + TCP server-side\n");
        bool l = tcp_listen(54321);
        bool s_ok = (tcp_current_state() == TCP_STATE_LISTEN);
        serial_puts("[smoke 47.5] tcp_listen=");
        serial_puts(l ? "ok" : "fail");
        serial_puts(" state=LISTEN=");
        serial_puts(s_ok ? "yes" : "no");
        serial_puts(l && s_ok ? " (OK)\n" : " (FAIL)\n");
        tcp_close();
    }

    // ---- Этап 47.5 LIVE smoke: end-to-end TLS handshake против openssl ----
    // Включается всегда — но в headless-test'е без openssl s_server просто
    // FAIL'нет с timeout (это OK).  scripts/test_stage47_live.sh поднимает
    // server на host'е WSL до запуска QEMU.  Host = 10.0.2.2 в QEMU-NAT.
    {
        serial_puts("[smoke 47.5 live] connecting to 10.0.2.2:4443...\n");
        // 10.0.2.2 = 0x0A000202.
        bool tcp_ok = tcp_connect(0x0A000202u, 4443);
        if (!tcp_ok) {
            serial_puts("[smoke 47.5 live] TCP connect failed (no server)\n");
        } else {
            // [38.1.0] Из кучи, не static (см. smoke 47.4 выше).
            tls_ctx_t* lt = (tls_ctx_t*)kcalloc(1, sizeof(tls_ctx_t));
            bool hs = false;
            if (lt) {
                tls_client_init(lt, -1, "10.0.2.2");
                hs = tls_client_handshake(lt);
            }
            serial_puts("[smoke 47.5 live] handshake: ");
            serial_puts(hs ? "PASS" : "FAIL");
            serial_puts("\n");
            if (lt) { tls_close(lt); kfree(lt); }
            tcp_close();
        }
    }

    // ---- Этап 47.6 smoke: SHA-1 + P-256 ECDH self-roundtrip + chain-validate ----
    // Дополнительные интеграционные проверки нового криптостека:
    //   1) SHA-1 на пустом сообщении: da39a3ee5e6b4b0d3255bfef95601890afd80709
    //   2) Эфемерные P-256 keypair'ы Alice/Bob, обмен и сравнение shared.
    //   3) x509_verify_chain на одном из root'ов (chain длины 1 — root) c
    //      hostname="" и now=0 (без time-check).  Должен пройти.
    {
        // (1) SHA-1 пустой строки
        uint8_t sha1_out[20];
        crypto_sha1_buf("", 0, sha1_out);
        // Ожидание: da39a3ee5e6b4b0d3255bfef95601890afd80709
        bool sha1_ok = (sha1_out[0] == 0xda && sha1_out[1] == 0x39 &&
                        sha1_out[19] == 0x09);
        serial_puts("[smoke 47.6] SHA-1(empty): ");
        serial_puts(sha1_ok ? "OK" : "FAIL");
        serial_puts("\n");

        // (2) P-256 ECDH self-roundtrip
        uint8_t alice_sk[32], bob_sk[32];
        uint8_t alice_pk[65], bob_pk[65];
        uint8_t ss_a[32], ss_b[32];
        crypto_rand_bytes(alice_sk, 32);
        crypto_rand_bytes(bob_sk,   32);
        // Гарантируем не-ноль
        if (alice_sk[0] == 0) alice_sk[0] = 1;
        if (bob_sk[0]   == 0) bob_sk[0]   = 1;
        bool a_ok = crypto_p256_derive_pub(alice_sk, alice_pk);
        bool b_ok = crypto_p256_derive_pub(bob_sk,   bob_pk);
        bool e1   = crypto_p256_ecdh(alice_sk, bob_pk,   ss_a);
        bool e2   = crypto_p256_ecdh(bob_sk,   alice_pk, ss_b);
        int  match = 1;
        for (int i = 0; i < 32; i++) if (ss_a[i] != ss_b[i]) match = 0;
        serial_puts("[smoke 47.6] P-256 ECDH self-roundtrip: ");
        serial_puts((a_ok && b_ok && e1 && e2 && match) ? "OK" : "FAIL");
        serial_puts("\n");

        // (3) x509_verify_chain на ISRG Root X1 (chain длины 1, без time-check).
        if (g_crypto_ca_count >= 1) {
            const uint8_t* ders[1] = { g_crypto_ca_store[0].der };
            size_t lens[1]         = { g_crypto_ca_store[0].der_len };
            x509_chain_result_t r = x509_verify_chain(ders, lens, 1, 0, 0);
            serial_puts("[smoke 47.6] chain(root only) result: ");
            serial_puts(x509_chain_result_str(r));
            // Self-signed root passes if subject==issuer и подпись валидна.
            // Code должен быть OK.
            serial_puts(r == X509_CHAIN_OK ? " (OK)\n" : " (FAIL)\n");
        }
    }

#ifdef AMPLIFY_TLS_INTERNET
    // ---- Этап 47.7 smoke: HTTPS GET к реальному интернет-сайту ----
    // Включается через флаг AMPLIFY_TLS_INTERNET (см. scripts/test_stage47_internet.sh).
    // Требует QEMU с -netdev user (slirp NAT с DNS на 10.0.2.3).
    // Цель: подтвердить что DNS + TCP + TLS(ECDHE) + chain validation +
    // HTTP response parsing работают end-to-end с настоящим сайтом.
    {
#ifdef AMPLIFY_TLS_LOCALHOST_ONLY
        // Для test_stage47_send_recv.sh — только localhost.
        // Используем port 4444 (4443 уже занят smoke 47.5 live, который
        // делает handshake-close без application data, что валит s_server).
        const char* targets[] = {
            "https://10.0.2.2:4444/",
        };
#else
        const char* targets[] = {
            "https://example.com/",
            "https://www.cloudflare.com/",
        };
#endif
        static char body[4096];
        for (int t = 0; t < (int)(sizeof(targets) / sizeof(targets[0])); t++) {
            size_t blen = 0;
            int    code = 0;
            serial_puts("[smoke 47.7] GET ");
            serial_puts(targets[t]);
            serial_puts("\n");
            // require_trust=false — чтобы chain validation не валила handshake;
            // мы лог-смотрим какой именно код получили.
            bool ok = https_get(targets[t], body, sizeof(body), &blen, &code, false);
            serial_puts("[smoke 47.7] result: ");
            serial_puts(ok ? "OK" : "FAIL");
            if (ok) {
                serial_puts(" code=");
                serial_print_dec((uint64_t)code);
                serial_puts(" body_len=");
                serial_print_dec((uint64_t)blen);
            }
            serial_puts("\n");
        }
    }
#endif

    // ---- Этап 57: конфигурация установленной системы ----------------------
    // hostname и тема, выбранные в установщике.  В live-режиме не трогаем:
    // там /disk — чужой том (или его нет), а установщик рисуется в светлой теме.
    if (!installer_should_run()) install_apply_system_config();

    // ---- Этап 48 smoke: LUKS1 + blkcrypt на RAM-диске ---------------------
    // Не трогает реальные диски: format/open/wrong-pass/XTS-через-mapping/
    // слоты — всё на временном RAM-диске, который затем затирается.
    {
        serial_puts("\n[smoke 48] FDE self-test (LUKS1 aes-xts-plain64 on a RAM disk)\n");
        uint64_t t0 = timer_get_uptime_ms();
        bool ok = fde_self_test();
        serial_puts("[smoke 48] self-test ");
        serial_puts(ok ? "PASS" : "FAIL");
        serial_puts(" in ");
        serial_print_dec(timer_get_uptime_ms() - t0);
        serial_puts(" ms; system volume: ");
        serial_puts(fde_status_str());
        serial_puts("\n");
    }

    // ---- Этап 49 smoke: AMPLIFY MIND -----------------------------------------
    // Пул AP (parallel-for + задача на ядре-дирижёре), журнал ядра klog,
    // встроенная модель stories260K из initrd: токенизация и 32 greedy-токена
    // сверяются с эталоном tools/mind_ref.py (генерация на пуле и на BSP),
    // инструменты ps/ls/logs, запрет чтения users.txt, разбор <tool>.
    // Модель после проверки выгружается: MIND грузит веса лениво (Ctrl+Space).
    {
        serial_puts("\n[smoke 49] AMPLIFY MIND self-test\n");
        uint64_t t0 = timer_get_uptime_ms();
        mind_init();
        bool ok = mind_self_test();
        serial_puts("[smoke 49] done in ");
        serial_print_dec(timer_get_uptime_ms() - t0);
        serial_puts(ok ? " ms: PASS\n" : " ms: FAIL\n");
        // Замер скорости настоящей модели — только если попросили в
        // /disk/Sanctum/mind.conf (bench_on_boot=N); обычная загрузка не ждёт.
        mind_boot_bench();
    }

    // ---- Этап 50 smoke: VT-x (подэтап 50.0 — разведка) ------------------------
    // Только чтение CPUID и MSR IA32_VMX_*: виден ли VMX ядру, разрешён ли
    // прошивкой, какие управляющие биты позволены. Ничего не включается.
    // PASS значит «разведка не противоречит сама себе», а НЕ «VT-x есть»:
    // под TCG и без вложенной виртуализации VMX нет, и это штатная загрузка
    // (вердикт ABSENT). Есть ли VT-x и годится ли — говорит строка verdict.
    {
        serial_puts("\n[smoke 50] VT-x probe (phantom hypervisor)\n");
        uint64_t t0 = timer_get_uptime_ms();
        vmx_probe();
        vmx_describe(serial_puts, "[smoke 50] ", true);
        serial_puts("[smoke 50] done in ");
        serial_print_dec(timer_get_uptime_ms() - t0);
        serial_puts(vmx_caps()->sane ? " ms: PASS\n" : " ms: FAIL\n");
    }

    // ---- Этап 50.1 smoke: первый гость -----------------------------------------
    // VMXON на всех ядрах и обратно, затем гость на AP: стартует в реальном
    // режиме, сам переходит в защищённый и длинный, проверяет CPUID, порты,
    // сохранность xmm и свою память. Без VT-x блок пишет «skipped» и PASS.
    // Остальные сценарии (изоляция, вытеснение, делёж ядра с пулом, 100 циклов)
    // долгие — они в `phantom selftest`, не в загрузке (CLAUDE.md §5.33).
    {
        uint64_t t0 = timer_get_uptime_ms();
        bool ok = phantom_boot_selftest();
        serial_puts("[smoke 50.1] done in ");
        serial_print_dec(timer_get_uptime_ms() - t0);
        serial_puts(ok ? " ms: PASS\n" : " ms: FAIL\n");
    }

    // ---- Этап 51 smoke: движок WebAssembly («rune») -----------------------------
    // Встроенные модули разбираются и проверяются, испорченные — отклоняются с
    // верной причиной, память модулей возвращается куче. Официальный набор
    // тестов долгий и лежит на диске — он в `rune spec`, не в загрузке.
    (void)rune_boot_selftest();

    splash_show();

    // ---- Этап 57: GUI-установщик (live-режим) ------------------------------
    // Если система загружена с установочного носителя (cmdline "live installer"
    // от GRUB или \live.flag на ESP), открываем установщик перед firstboot и
    // login.  Выход из установщика («Live session») продолжает обычную загрузку.
    if (installer_should_run()) {
        installer_run();
    }

    // ---- 37.6c-4: firstboot wizard (графический, после splash) ------------
    firstboot_run_if_needed();

    // ---- Этап 37.6c-1: smoke-тесты новых подсистем (serial only) ----------
    // Это не критический путь: помогает увидеть в QEMU-логах что всё работает,
    // даже если пользователь не делает interactive ввод.
    {
        serial_puts("\n[smoke 37.6c-1] timer_get_uptime_ms=");
        serial_print_dec(timer_get_uptime_ms());
        serial_puts(" ms\n");

        rtc_time_t rt;
        rtc_read(&rt);
        serial_puts("[smoke 37.6c-1] RTC: ");
        serial_print_dec(rt.year); serial_putc('-');
        serial_print_dec(rt.month); serial_putc('-');
        serial_print_dec(rt.day);  serial_putc(' ');
        serial_print_dec(rt.hour); serial_putc(':');
        serial_print_dec(rt.minute); serial_putc(':');
        serial_print_dec(rt.second); serial_puts("\n");
    }

    // ---- Этап 37.6c-1: login screen (графический) ------------------------
    // (мелодия уже сыграна в splash_show)
    login_session_t session;
    {
        // Этап 48: пароль, которым только что разблокировали диск, подходит
        // к учётной записи — второй раз его не спрашиваем (как FileVault).
        static char sso[64];
        bool signed_in = fde_take_sso(sso, (int)sizeof(sso)) &&
                         login_try_passphrase(sso, &session);
        crypto_wipe(sso, sizeof(sso));
        if (!signed_in) login_run(&session);
    }

    // ---- Этап F: WM + desktop (taskbar и «Пуск» удалены) ----------------
    // doublebuf уже инициализирован ранее (перед splash).
    wm_init();
    desktop_init();
    // desktop_init() сбрасывает тему в светлую — повторно применяем выбор
    // пользователя (тёмная тема из установщика / Facet).
    install_apply_system_config();

    // Иконки рабочего стола (8 ярлыков в 2 колонки).
    desktop_add_icon("About",   'i', 0xFF38598C, launch_about);
    desktop_add_icon("Files",   'F', 0xFF4A7AB8, launch_files);
    desktop_add_icon("Notes",   'N', 0xFFB07030, launch_notes);
    desktop_add_icon("Calc",    '+', 0xFF38B8E0, launch_calc);
    desktop_add_icon("Term",    '>', 0xFF008060, launch_term);
    desktop_add_icon("Paint",   'P', 0xFFB05050, launch_paint);
    desktop_add_icon("Embers",  '*', 0xFFB04020, launch_embers);
    desktop_add_icon("Browser", 'W', 0xFF206090, launch_browser);
    // [этап 44] Иконка 3D-куба на рабочем столе.
    desktop_add_icon("3D куб",  '3', 0xFFF5A037, launch_glcube);
    // [этап 45] Иконка Amply Center.
    desktop_add_icon("Amply",   'A', 0xFFD24A30, launch_amply);
    // [этап 46] Иконка Sandbox Manager.
    desktop_add_icon("Sandbox", 'B', 0xFFC84190, launch_box);
    // Иконка "Сведения о системе" (CPU model + instructions).
    desktop_add_icon("Info",    'i', 0xFF50A050, launch_info);
    desktop_add_icon("Signal",  'S', 0xFFC84190, launch_signal);
    desktop_add_icon("Arc",     'C', 0xFFE8785D, launch_arc);
    desktop_add_icon("Resonance",'R',0xFFF0A33A, launch_resonance);
    desktop_add_icon("Core",    'O', 0xFFB54478, launch_core);
    desktop_add_icon("Facet",   'G', 0xFFE8785D, launch_facet);
    desktop_add_icon("Mesh",    'M', 0xFFB54478, launch_mesh);
    desktop_add_icon("Phantom", 'V', 0xFFB54478, launch_phantom);   // [50.5] гипервизор
    desktop_add_icon("Rune",    'R', 0xFFE8785D, launch_rune);      // [51.4] программы WebAssembly
    desktop_add_icon("Capacity",'D', 0xFFF0A33A, launch_capacity);

    // Этап F: меню «Пуск» удалено. Все его пункты живут плитками Core,
    // который читает тот же реестр desktop_add_icon — ничего не теряется.
    desktop_add_icon("Clock",    'C', 0xFFF0A33A, launch_clock);
    desktop_add_icon("Matrix",   'M', 0xFF46A171, launch_matrix);
    desktop_add_icon("Tasks",    'T', 0xFFB54478, launch_taskmgr);
    desktop_add_icon("Reboot",   'R', 0xFFE8785D, menu_reboot_click);
    desktop_add_icon("Shutdown", 'X', 0xFFB54478, menu_shutdown_click);

    // Stage A: Apex является стартовым экраном. About доступен из dashboard
    // и legacy menu, но больше не перекрывает оболочку сразу после login.

    // [48.2] /disk мог открыться только после разблокировки FDE.
    netcfg_late_load();

    // wm_main_loop не возвращается.
    wm_main_loop();

    // ---- Smoke-тест: FAT32 read (этап 37.5b) -------------------------------
    {
        fb_console_set_color(0xFFAAAAFF, 0xFF101820);
        fb_console_puts("\n[smoke] ATA + FAT32 read-only:\n");
        fb_console_set_color(0xFFFFFFFF, 0xFF101820);

        if (!ata_present()) {
            fb_console_puts("  no ATA disk attached — пропускаем тест\n");
        } else {
            const ata_info_t* di = ata_info();
            fb_console_puts("  disk: "); fb_console_puts(di->model);
            fb_console_puts(" — "); fb_console_print_dec(di->size_mb); fb_console_puts(" MB\n");

            // Читаем /disk/HELLO.TXT через прямой fat32_read_file
            // (минуя VFS — упрощает демо).
            char buf[256];
            int64_t got = fat32_read_file("/disk/HELLO.TXT", buf, sizeof(buf) - 1);
            if (got < 0) {
                fb_console_puts("  fat32_read_file FAIL\n");
            } else {
                buf[got] = 0;
                fb_console_set_color(0xFF80FF80, 0xFF101820);
                fb_console_puts("  /disk/HELLO.TXT contents (");
                fb_console_print_dec(got); fb_console_puts(" bytes):\n");
                fb_console_set_color(0xFFFFFFFF, 0xFF101820);
                // Печатаем содержимое (с отступом)
                fb_console_puts("    | ");
                for (int i = 0; i < got; i++) {
                    fb_console_putc(buf[i]);
                    if (buf[i] == '\n' && i + 1 < got) fb_console_puts("    | ");
                }
                fb_console_puts("\n");
            }
        }
    }

    // ---- Smoke-тест: DHCP + ICMP + DNS + TCP (37.5c-1+2) -------------------
    if (netif_is_ready()) {
        fb_console_set_color(0xFFAAAAFF, 0xFF101820);
        fb_console_puts("\n[smoke] DHCP + ICMP + DNS + TCP:\n");
        fb_console_set_color(0xFFFFFFFF, 0xFF101820);

        net_interface_t* nif = netif_get_default();
        fb_console_puts("  iface "); fb_console_puts(nif->name);
        fb_console_puts(" ip="); netif_dump_ip4(nif->ip4_addr);
        fb_console_puts(" gw="); netif_dump_ip4(nif->ip4_gateway);
        fb_console_puts(" dns="); netif_dump_ip4(dhcp_get_dns());
        fb_console_puts("\n");

        // ICMP echo на gateway.
        fb_console_puts("  -> icmp echo to gateway\n");
        const char ping_payload[] = "amplify-os-ping";
        icmp_send_echo(nif->ip4_gateway, 1, 1, ping_payload, sizeof(ping_payload) - 1);
        uint64_t t0 = pit_ticks();
        while (pit_ticks() - t0 < 30) __asm__ __volatile__("sti; hlt");

        // DNS resolve.
        fb_console_puts("  -> dns resolve example.com\n");
        uint32_t ip = dns_resolve("example.com");
        if (ip) {
            fb_console_set_color(0xFF80FF80, 0xFF101820);
            fb_console_puts("  example.com -> "); netif_dump_ip4(ip);
            fb_console_puts("\n");
            fb_console_set_color(0xFFFFFFFF, 0xFF101820);

            // TCP connect 80 + GET.
            fb_console_puts("  -> tcp connect "); netif_dump_ip4(ip);
            fb_console_puts(":80\n");
            if (tcp_connect(ip, 80)) {
                fb_console_set_color(0xFF80FF80, 0xFF101820);
                fb_console_puts("  TCP ESTABLISHED, sending GET\n");
                fb_console_set_color(0xFFFFFFFF, 0xFF101820);
                const char* get =
                    "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
                size_t glen = 0; while (get[glen]) glen++;
                tcp_send(get, glen);

                t0 = pit_ticks();
                while (pit_ticks() - t0 < 200) __asm__ __volatile__("sti; hlt");

                char buf[256]; size_t got = 0;
                tcp_recv(buf, sizeof(buf) - 1, &got);
                if (got > 0) {
                    buf[got] = 0;
                    fb_console_set_color(0xFFFFFF80, 0xFF101820);
                    fb_console_puts("  HTTP first ");
                    fb_console_print_dec(got); fb_console_puts(" bytes:\n");
                    fb_console_set_color(0xFFFFFFFF, 0xFF101820);
                    for (size_t i = 0; i < got && i < 80; i++) fb_console_putc(buf[i]);
                    fb_console_puts("\n  ...\n");
                } else {
                    fb_console_puts("  no TCP data received\n");
                }
                tcp_close();
            } else {
                fb_console_puts("  tcp connect failed\n");
            }
        } else {
            fb_console_puts("  dns resolve failed\n");
        }
    }

    // ---- Smoke-тест: AC97 — 440 Hz triangle wave (этап 37.5c-3) ------------
    if (ac97_is_ready()) {
        fb_console_set_color(0xFFAAAAFF, 0xFF101820);
        fb_console_puts("\n[smoke] AC97 audio (440 Hz triangle, 250 ms):\n");
        fb_console_set_color(0xFFFFFFFF, 0xFF101820);

        const uint32_t rate = 48000;
        const uint32_t freq = 440;
        const uint32_t frames = rate / 4;        // 12000 frames
        int16_t* tone = (int16_t*)kmalloc(frames * 2 * sizeof(int16_t));
        if (tone) {
            for (uint32_t i = 0; i < frames; i++) {
                uint32_t period = rate / freq;
                uint32_t phase = i % period;
                int32_t v;
                if (phase < period / 2) v = (int32_t)((phase * 16000) / (period / 2));
                else v = (int32_t)((period - phase) * 16000) / (period / 2);
                v -= 8000;
                tone[i * 2]     = (int16_t)v;
                tone[i * 2 + 1] = (int16_t)v;
            }
            ac97_set_volume(50);
            size_t played = ac97_play(tone, frames * 2, rate);
            fb_console_puts("  played "); fb_console_print_dec(played);
            fb_console_puts(" samples (440 Hz triangle, audio sink=none)\n");
            kfree(tone);
        }
    }

    // ---- Smoke-тест: FAT32 write (этап 37.5c-3) ----------------------------
    if (ata_present()) {
        fb_console_set_color(0xFFAAAAFF, 0xFF101820);
        fb_console_puts("\n[smoke] FAT32 write (rewrite /disk/HELLO.TXT):\n");
        fb_console_set_color(0xFFFFFFFF, 0xFF101820);

        const char new_text[] =
            "*** AMPLIFY-OS overwrote me from kernel via FAT32 write! ***\n"
            "Stage 37.5c-3 works.\n";
        int64_t w = fat32_write_file("/disk/HELLO.TXT", new_text, sizeof(new_text) - 1);
        fb_console_puts("  fat32_write_file -> "); fb_console_print_dec(w);
        fb_console_puts(" bytes\n");

        char back[256];
        int64_t r = fat32_read_file("/disk/HELLO.TXT", back, sizeof(back) - 1);
        if (r > 0) {
            back[r] = 0;
            fb_console_set_color(0xFF80FF80, 0xFF101820);
            fb_console_puts("  read back ("); fb_console_print_dec(r);
            fb_console_puts(" bytes):\n");
            fb_console_set_color(0xFFFFFFFF, 0xFF101820);
            fb_console_puts("    | ");
            for (int64_t i = 0; i < r; i++) {
                fb_console_putc(back[i]);
                if (back[i] == '\n' && i + 1 < r) fb_console_puts("    | ");
            }
            fb_console_puts("\n");
        }
    }

    // ---- Smoke-тест: pipe + signal (этап 37.5d-1) --------------------------
    {
        fb_console_set_color(0xFFAAAAFF, 0xFF101820);
        fb_console_puts("\n[smoke] IPC: pipe + signal:\n");
        fb_console_set_color(0xFFFFFFFF, 0xFF101820);

        // 1) pipe — write/read FIFO.
        pipe_t* p = pipe_create(0);
        if (p) {
            const char msg[] = "Hello via pipe! (kernel-to-kernel demo)";
            size_t w = pipe_write(p, msg, sizeof(msg) - 1);
            char buf[128];
            size_t r = pipe_read(p, buf, sizeof(buf) - 1);
            buf[r] = 0;
            fb_console_puts("  pipe wrote ");  fb_console_print_dec(w);
            fb_console_puts(" bytes, read "); fb_console_print_dec(r);
            fb_console_puts(": ");
            fb_console_set_color(0xFF80FF80, 0xFF101820);
            fb_console_puts(buf);
            fb_console_set_color(0xFFFFFFFF, 0xFF101820);
            fb_console_puts("\n  pipe_eof: "); pipe_close_write(p);
            fb_console_puts(pipe_eof(p) ? "yes" : "no"); fb_console_puts("\n");
            pipe_destroy(p);
        } else {
            fb_console_puts("  pipe_create FAIL\n");
        }

        // 2) signal — register tasks, send SIG_INT, check pending.
        signal_init();
        int pid_a = signal_register_task("alpha");
        int pid_b = signal_register_task("beta");
        fb_console_puts("  registered alpha=pid"); fb_console_print_dec(pid_a);
        fb_console_puts(", beta=pid");             fb_console_print_dec(pid_b);
        fb_console_puts("\n");

        signal_send(pid_a, SIG_INT);
        signal_send(pid_b, SIG_KILL);
        fb_console_puts("  alpha SIG_INT pending: ");
        fb_console_puts(signal_pending(pid_a, SIG_INT) ? "yes" : "no");
        fb_console_puts(", beta SIG_KILL pending: ");
        fb_console_puts(signal_pending(pid_b, SIG_KILL) ? "yes" : "no");
        fb_console_puts("\n");

        signal_clear(pid_a, SIG_INT);
        fb_console_puts("  after clear, alpha SIG_INT: ");
        fb_console_puts(signal_pending(pid_a, SIG_INT) ? "yes" : "no");
        fb_console_puts(" (SIG_KILL is unmaskable, beta still: ");
        fb_console_puts(signal_pending(pid_b, SIG_KILL) ? "yes" : "no");
        fb_console_puts(")\n");

        signal_unregister(pid_a);
        signal_unregister(pid_b);
    }

    // ---- Smoke-тест: unix_socket + SHM (этап 37.5d-2) ----------------------
    {
        fb_console_set_color(0xFFAAAAFF, 0xFF101820);
        fb_console_puts("\n[smoke] unix_socket + SHM:\n");
        fb_console_set_color(0xFFFFFFFF, 0xFF101820);

        // 1) unix socket: bind/connect/accept/send/recv loopback.
        usock_init();
        int srv = usock_bind("amplify-greet");
        usock_listen(srv);
        int cli = usock_connect("amplify-greet");
        int peer = usock_accept(srv);
        if (cli >= 0 && peer >= 0) {
            const char hello[] = "ping from client";
            usock_send(cli, hello, sizeof(hello) - 1);
            char buf[64]; size_t got = usock_recv(peer, buf, sizeof(buf) - 1);
            buf[got] = 0;
            fb_console_puts("  client->server: \"");
            fb_console_set_color(0xFF80FF80, 0xFF101820);
            fb_console_puts(buf);
            fb_console_set_color(0xFFFFFFFF, 0xFF101820);
            fb_console_puts("\" (");
            fb_console_print_dec(got); fb_console_puts(" bytes)\n");

            const char reply[] = "pong from server";
            usock_send(peer, reply, sizeof(reply) - 1);
            got = usock_recv(cli, buf, sizeof(buf) - 1);
            buf[got] = 0;
            fb_console_puts("  server->client: \"");
            fb_console_set_color(0xFF80FF80, 0xFF101820);
            fb_console_puts(buf);
            fb_console_set_color(0xFFFFFFFF, 0xFF101820);
            fb_console_puts("\" (");
            fb_console_print_dec(got); fb_console_puts(" bytes)\n");

            usock_close(cli);
            usock_close(peer);
            usock_close(srv);
        } else {
            fb_console_puts("  usock setup FAIL\n");
        }

        // 2) SHM: создать сегмент, прикрепить к kernel_vm, записать, прочитать.
        shm_init();
        int shmid = shm_get(SHM_IPC_PRIVATE, 4096, SHM_IPC_CREAT);
        if (shmid > 0) {
            uint64_t va = shm_attach(vm_kernel(), shmid);
            if (va != (uint64_t)-1) {
                fb_console_puts("  shm id="); fb_console_print_dec(shmid);
                fb_console_puts(" attached @ 0x"); fb_console_print_hex(va, 12);
                fb_console_puts("\n");

                // Пишем "AMPLIFY-OS shm OK" в сегмент, перечитываем.
                volatile char* p = (volatile char*)(uintptr_t)va;
                const char msg[] = "AMPLIFY-OS shm OK!";
                for (int i = 0; i < (int)sizeof(msg); i++) p[i] = msg[i];

                fb_console_puts("  shm read-back: \"");
                fb_console_set_color(0xFF80FF80, 0xFF101820);
                for (int i = 0; p[i] && i < 32; i++) fb_console_putc(p[i]);
                fb_console_set_color(0xFFFFFFFF, 0xFF101820);
                fb_console_puts("\"\n");

                shm_detach(vm_kernel(), va);
                shm_ctl(shmid, SHM_IPC_RMID);
            } else {
                fb_console_puts("  shm_attach FAIL\n");
            }
        } else {
            fb_console_puts("  shm_get FAIL\n");
        }
    }

    // ---- Smoke-тест: swap roundtrip (этап 37.5d-3a) ------------------------
    if (swap_is_enabled()) {
        fb_console_set_color(0xFFAAAAFF, 0xFF101820);
        fb_console_puts("\n[smoke] swap roundtrip:\n");
        fb_console_set_color(0xFFFFFFFF, 0xFF101820);

        uint64_t phys = pmm_alloc_page();
        if (phys) {
            uint8_t* page = (uint8_t*)(uintptr_t)phys;
            for (int i = 0; i < 4096; i++) page[i] = (uint8_t)(i ^ 0x55);

            uint32_t slot = swap_out_page(phys);
            if (slot != (uint32_t)-1) {
                fb_console_puts("  swap_out -> slot "); fb_console_print_dec(slot);
                fb_console_puts(" ("); fb_console_print_dec(swap_used_slots());
                fb_console_puts(" used)\n");

                for (int i = 0; i < 4096; i++) page[i] = 0;       // clobber

                int rc = swap_in_page(slot, phys);
                fb_console_puts("  swap_in -> rc="); fb_console_print_dec(rc);
                fb_console_puts("  ("); fb_console_print_dec(swap_used_slots());
                fb_console_puts(" used after)\n");

                int ok = 1;
                for (int i = 0; i < 4096; i++)
                    if (page[i] != (uint8_t)(i ^ 0x55)) { ok = 0; break; }
                fb_console_set_color(ok ? 0xFF80FF80 : 0xFFFF6060, 0xFF101820);
                fb_console_puts(ok ? "  pattern verified — swap works!\n"
                                   : "  pattern MISMATCH — swap broken!\n");
                fb_console_set_color(0xFFFFFFFF, 0xFF101820);
            } else {
                fb_console_puts("  swap_out FAIL\n");
            }
            pmm_free_page(phys);
        }
    }

    // unreachable — shell стартует выше, до smoke-блока
    for (;;) __asm__ __volatile__("hlt");
}

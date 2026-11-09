// ============================================================================
// kernel.c - Главный файл ядра MyOS
// ============================================================================
// Точка входа в ядро на языке C
// Вызывается из boot.asm после настройки стека
// ============================================================================

#include <stdint.h>
#include <stddef.h>
#include "vga.h"
#include "arch/i386/gdt.h"
#include "arch/i386/idt.h"
#include "arch/i386/acpi.h"
#include "arch/i386/lapic.h"
#include "arch/i386/ioapic.h"
#include "arch/i386/cpu_stats.h"
#include "arch/i386/smp.h"
#include "arch/i386/longmode.h"
#include "drivers/keyboard.h"
#include "drivers/timer.h"
#include "drivers/speaker.h"
#include "arch/i386/syscall.h"
#include "memory/pmm.h"
#include "memory/heap.h"
#include "memory/paging.h"
#include "memory/vm.h"
#include "memory/swap.h"
#include "ipc/shm.h"
#include "memory/memory.h"
#include "fs/vfs.h"
#include "fs/initrd.h"
#include "shell/shell.h"
#include "io.h"
#include "task/task.h"
#include "drivers/ata.h"
#include "drivers/usb/usb_block.h"
#include "fs/fat32.h"
#include "loader/elf.h"
#include "loader/dynlink.h"
#include "gui/framebuffer.h"
#include "gui/graphics.h"
#include "gui/font.h"
#include "gui/doublebuf.h"
#include "drivers/mouse.h"
#include "gui/cursor.h"
#include "gui/window.h"
#include "gui/wm.h"
#include "gui/desktop.h"
#include "gui/apps.h"
#include "gui/startmenu.h"
#include "gui/ttf.h"
#include "gui/image.h"
#include "sound/audio.h"
#include "net/netif.h"
#include "net/dhcp.h"
#include "net/dns.h"
#include "net/netcfg.h"
#include "security/security.h"
#include "debug_serial.h"

// ============================================================================
// Multiboot структуры (расширенные для графического режима)
// ============================================================================

typedef struct {
    uint32_t flags;           // 0
    uint32_t mem_lower;       // 4
    uint32_t mem_upper;       // 8
    uint32_t boot_device;     // 12
    uint32_t cmdline;         // 16
    uint32_t mods_count;      // 20
    uint32_t mods_addr;       // 24
    uint32_t syms[4];         // 28-40
    uint32_t mmap_length;     // 44
    uint32_t mmap_addr;       // 48
    uint32_t drives_length;   // 52
    uint32_t drives_addr;     // 56
    uint32_t config_table;    // 60
    uint32_t boot_loader_name;// 64
    uint32_t apm_table;       // 68
    // VBE info (72-84)
    uint32_t vbe_control_info; // 72
    uint32_t vbe_mode_info;    // 76
    uint16_t vbe_mode;         // 80
    uint16_t vbe_iface_seg;    // 82
    uint16_t vbe_iface_off;    // 84
    uint16_t vbe_iface_len;    // 86
    // Framebuffer info (88-108) — доступно когда flags bit 12 установлен
    uint64_t fb_addr;          // 88: Физический адрес фреймбуфера
    uint32_t fb_pitch;         // 96: Байт в строке
    uint32_t fb_width;         // 100: Ширина в пикселях
    uint32_t fb_height;        // 104: Высота в пикселях
    uint8_t  fb_bpp;           // 108: Бит на пиксель
    uint8_t  fb_type;          // 109: Тип (1 = RGB)
    uint8_t  fb_color_info[6]; // 110-115: Информация о цветовых каналах
} __attribute__((packed)) multiboot_info_t;

typedef struct {
    uint32_t mod_start;
    uint32_t mod_end;
    uint32_t cmdline;
    uint32_t reserved;
} multiboot_module_t;

// ============================================================================
// Вспомогательные функции
// ============================================================================

static void print_status_ok(const char* component) {
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("[");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("OK");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("] ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_println(component);
}

static void print_status_fail(const char* component) {
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("[");
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts("!!");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("] ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_println(component);
}

// ============================================================================
// Этап 21: демо TTF + PNG поверх рабочего стола
// ============================================================================
// Вызывается ПОСЛЕ того, как desktop успешно нарисован. Если что-то из
// TTF/PNG упадёт — пользователь всё равно увидит работающий рабочий стол.

static void stage21_demo_overlay(void) {
    int32_t w = framebuffer_get_width();

    ttf_font_t* tf = ttf_get_system_font();
    if (tf) {
        const char* line1 = "Stage 21: TrueType fonts + PNG";
        int tw = ttf_string_width(tf, line1, 22.0f);
        ttf_draw_string_top(tf, (w - tw) / 2, 8, line1, 22.0f, RGB(255, 255, 255));

        const char* line2 = "Powered by stb_truetype & stb_image";
        int tw2 = ttf_string_width(tf, line2, 16.0f);
        ttf_draw_string_top(tf, (w - tw2) / 2, 36, line2, 16.0f, RGB(180, 210, 255));
    }

    image_t* banner = image_load("/banner.png");
    if (banner) {
        int bx = (w - banner->width) / 2;
        int by = 60;
        image_draw_alpha(banner, bx, by);
        image_free(banner);
    }

    doublebuf_swap();
}

// ============================================================================
// Splash screen — Amplify-OS (TrueType + supersampled logo + анимация)
// ============================================================================

// Проверка: точка (px,py) внутри треугольника с вершинами (ax,ay),(bx,by),(cx,cy).
// Возвращает знак-based int (не bool), чтобы можно было легко
// использовать при supersampling.
static int splash_point_in_tri(int px, int py,
                               int ax, int ay, int bx, int by, int cx, int cy) {
    int s1 = (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    int s2 = (cx - bx) * (py - by) - (cy - by) * (px - bx);
    int s3 = (ax - cx) * (py - cy) - (ay - cy) * (px - cx);
    int neg = (s1 < 0) || (s2 < 0) || (s3 < 0);
    int pos = (s1 > 0) || (s2 > 0) || (s3 > 0);
    return !(neg && pos);
}

// Базовый градиент orange→magenta + бегущий блик по фазе phase.
// (x,y) — экранные координаты; (y0,y1) — вертикальный диапазон, в котором
// разворачивается градиент (например, логотип от верха до низа).
static uint32_t splash_color(int x, int y, int y0, int y1, int phase) {
    int h = y1 - y0; if (h <= 0) h = 1;
    int t = ((y - y0) * 255) / h;
    if (t < 0) t = 0; else if (t > 255) t = 255;

    uint32_t base = color_lerp(RGB(245, 160, 55), RGB(200, 65, 140), (uint8_t)t);

    // Блик: светящаяся полоса шириной 90 пикс, центрированная на phase.
    int dx = x - phase; if (dx < 0) dx = -dx;
    if (dx < 90) {
        int brighten = (90 - dx) * 60 / 90;
        base = color_lighten(base, (uint8_t)brighten);
    }
    return base;
}

// Альфа-блендит пиксель гранатным градиентом в back-буфер.
// Используется и для логотипа (после supersampling), и для текста (coverage
// приходит от TTF-растеризатора).
static void splash_blend_pixel(int x, int y, uint8_t coverage,
                               int y0, int y1, int phase) {
    if (coverage == 0) return;
    uint32_t rgb = splash_color(x, y, y0, y1, phase);
    uint32_t src = (((uint32_t)coverage) << 24) | (rgb & 0x00FFFFFF);

    framebuffer_t* fb = framebuffer_get();
    if (x < 0 || y < 0 || x >= (int)fb->width || y >= (int)fb->height) return;
    uint32_t* buf = doublebuf_get_buffer();
    uint32_t  pitch = fb->pitch / 4;
    uint32_t  off = (uint32_t)y * pitch + (uint32_t)x;
    buf[off] = color_blend_over(src, buf[off]);
}

// Supersampled рендер логотипа: для каждого screen-пикселя берём
// SSAA_N×SSAA_N субсэмплов и считаем среднее покрытие. Результат — сглаженные
// диагональные и угловые кромки без «ступенек».
//
// (cx, cy_top) — вершина треугольника; size — полуширина основания;
// stroke — толщина штриха; phase — позиция блика.
#define SSAA_N 3
static void splash_draw_logo(int cx, int cy_top, int size, int stroke, int phase) {
    // Внешний треугольник (apex вверху).
    int ax = cx,           ay = cy_top;
    int bx = cx - size,    by = cy_top + (size * 175 / 100);
    int cx_ = cx + size,   cy_ = by;

    // Внутренний — срезан на stroke от каждой стороны.
    int iax = cx,                  iay = cy_top + stroke + stroke / 2;
    int ibx = cx - size + stroke,  iby = by - stroke / 2;
    int icx = cx + size - stroke,  icy = by - stroke / 2;

    // Внутренний arrow — крошечный треугольник, создающий ʌ-метку.
    int y_top_in = cy_top + stroke * 2 + size / 5;
    int y_bot_in = by - stroke - size / 4;
    int half_in  = (size * 45) / 100;
    int inner_ax = cx,               inner_ay = y_top_in;
    int inner_bx = cx - half_in,     inner_by = y_bot_in;
    int inner_cx = cx + half_in,     inner_cy = y_bot_in;

    int inner2_stroke = stroke * 8 / 10;
    int inner2_ax = cx,                             inner2_ay = y_top_in + inner2_stroke;
    int inner2_bx = cx - half_in + inner2_stroke,   inner2_by = y_bot_in - inner2_stroke / 2;
    int inner2_cx = cx + half_in - inner2_stroke,   inner2_cy = y_bot_in - inner2_stroke / 2;

    int minx = cx - size - 4;
    int maxx = cx + size + 4;
    int miny = cy_top - 4;
    int maxy = by + 4;

    // Для supersampling домножаем координаты на SSAA_N и проверяем треугольники
    // в «подпиксельных» координатах.
    int S = SSAA_N;
    int S2 = S * S;

    // Умноженные вершины (для сравнения в суб-пиксельной сетке).
    int ax_s = ax * S,  ay_s = ay * S;
    int bx_s = bx * S,  by_s = by * S;
    int cx_s_ = cx_ * S, cy_s_ = cy_ * S;
    int iax_s = iax * S, iay_s = iay * S;
    int ibx_s = ibx * S, iby_s = iby * S;
    int icx_s = icx * S, icy_s = icy * S;
    int inA_ax = inner_ax * S, inA_ay = inner_ay * S;
    int inA_bx = inner_bx * S, inA_by = inner_by * S;
    int inA_cx = inner_cx * S, inA_cy = inner_cy * S;
    int inB_ax = inner2_ax * S, inB_ay = inner2_ay * S;
    int inB_bx = inner2_bx * S, inB_by = inner2_by * S;
    int inB_cx = inner2_cx * S, inB_cy = inner2_cy * S;

    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int hits = 0;
            for (int sy = 0; sy < S; sy++) {
                for (int sx = 0; sx < S; sx++) {
                    // Суб-пиксель в центре «ячейки» (x*S+sx+0.5)
                    // В целочисленной форме используем (x*S*2 + sx*2 + 1)/2 сразу ×2:
                    int px = x * S + sx;
                    int py = y * S + sy;

                    int in_outer  = splash_point_in_tri(px, py,
                        ax_s, ay_s, bx_s, by_s, cx_s_, cy_s_);
                    int in_inner  = splash_point_in_tri(px, py,
                        iax_s, iay_s, ibx_s, iby_s, icx_s, icy_s);
                    int in_arrow  = splash_point_in_tri(px, py,
                        inA_ax, inA_ay, inA_bx, inA_by, inA_cx, inA_cy);
                    int in_arrow2 = splash_point_in_tri(px, py,
                        inB_ax, inB_ay, inB_bx, inB_by, inB_cx, inB_cy);

                    int outer_stroke = in_outer && !in_inner;
                    int arrow_stroke = in_arrow && !in_arrow2;
                    if (outer_stroke || arrow_stroke) hits++;
                }
            }

            if (hits == 0) continue;
            // Нормированное покрытие → alpha [0..255].
            uint8_t cov = (uint8_t)((hits * 255) / S2);
            splash_blend_pixel(x, y, cov, cy_top, by, phase);
        }
    }
}

// Контекст для ttf-callback, чтобы рисовать текст градиентом+бликом.
typedef struct {
    int y0;
    int y1;
    int phase;
} splash_text_ctx_t;

static void splash_text_cb(int sx, int sy, uint8_t cov, void* user) {
    splash_text_ctx_t* c = (splash_text_ctx_t*)user;
    splash_blend_pixel(sx, sy, cov, c->y0, c->y1, c->phase);
}

static void show_splash_screen(void) {
    int32_t w = framebuffer_get_width();
    int32_t h = framebuffer_get_height();

    const uint32_t BG      = RGB(245, 235, 245);  // светлая лаванда
    const uint32_t CAPTION = RGB(180,  80, 140);  // "COMING OUT 2025"
    const uint32_t BAR_BG  = RGB(230, 215, 230);

    // Берём системный TTF-шрифт; если отсутствует — фоллбэк на bitmap.
    ttf_font_t* font = ttf_get_system_font();

    // Геометрия: логотип центрирован, под ним — большой заголовок, подзаголовок
    // и подпись. Всё масштабируется от высоты окна.
    int logo_cx      = w / 2;
    int logo_top_y   = h / 2 - 220;
    int logo_size    = 110;       // полуширина основания треугольника
    int logo_stroke  = 14;        // толщина штриха
    int logo_bot_y   = logo_top_y + (logo_size * 175 / 100);

    // Размер TTF (в пикселях высоты) — подобран под 1024×768.
    const float TITLE_PX   = 64.0f;
    const float SUB_PX     = 38.0f;
    const float CAPTION_PX = 18.0f;

    const char* title   = "AMPLIFY-OS";
    const char* sub     = "CHRT-OS";
    const char* caption = "COMING OUT 2025";

    // Позиции рассчитываются по измеренной ширине TTF-строк (или fallback).
    int title_w = font ? ttf_string_width(font, title, TITLE_PX)
                       : (int)(10 * FONT_WIDTH * 4);
    int sub_w   = font ? ttf_string_width(font, sub, SUB_PX)
                       : (int)(7 * FONT_WIDTH * 3);
    int cap_w   = font ? ttf_string_width(font, caption, CAPTION_PX)
                       : (int)(15 * FONT_WIDTH);

    int title_x = (w - title_w) / 2;
    int title_y = logo_bot_y + 36;
    int title_h = font ? ttf_line_height(font, TITLE_PX) : 64;

    int sub_x   = (w - sub_w) / 2;
    int sub_y   = title_y + title_h + 4;
    int sub_h   = font ? ttf_line_height(font, SUB_PX) : 40;

    int cap_x   = (w - cap_w) / 2;
    int cap_y   = sub_y + sub_h + 16;
    int cap_h   = font ? ttf_line_height(font, CAPTION_PX) : 16;

    int bar_w = 360;
    int bar_h = 6;
    int bar_x = (w - bar_w) / 2;
    int bar_y = cap_y + cap_h + 20;

    // Звук стартует параллельно.
    speaker_play_boot_melody();

    const int FRAMES = 24;
    for (int step = 0; step <= FRAMES; step++) {
        int phase = step * (w / FRAMES);

        // Чистим фон (триплет-буферы — не обязательно, но и не помешает).
        gfx_fill_rect(0, 0, w, h, BG);

        // Логотип (supersampled, antialiased).
        splash_draw_logo(logo_cx, logo_top_y, logo_size, logo_stroke, phase);

        // Заголовки: TTF с градиентом и бликом через callback.
        if (font) {
            splash_text_ctx_t tctx = { title_y, title_y + title_h, phase };
            ttf_draw_string_cb(font, title_x, title_y, title, TITLE_PX,
                               splash_text_cb, &tctx);

            splash_text_ctx_t sctx = { sub_y, sub_y + sub_h, phase + 80 };
            ttf_draw_string_cb(font, sub_x, sub_y, sub, SUB_PX,
                               splash_text_cb, &sctx);

            // Подпись — без блика, сплошным розовым.
            ttf_draw_string_top(font, cap_x, cap_y, caption, CAPTION_PX, CAPTION);
        } else {
            // Фоллбэк (TTF не загрузился) — bitmap font.
            font_draw_string(title_x, title_y, title, RGB(230, 90, 120), 0xFF000000);
            font_draw_string(sub_x,   sub_y,   sub,   RGB(200, 65, 140), 0xFF000000);
            font_draw_string(cap_x,   cap_y,   caption, CAPTION, 0xFF000000);
        }

        // Прогресс-бар с тем же градиентом.
        gfx_fill_rect(bar_x, bar_y, bar_w, bar_h, BAR_BG);
        int fill_w = (bar_w * step) / FRAMES;
        for (int py = 0; py < bar_h; py++) {
            for (int px = 0; px < fill_w; px++) {
                uint8_t t = (uint8_t)((px * 255) / (bar_w > 0 ? bar_w : 1));
                uint32_t c = color_lerp(RGB(245, 160, 55), RGB(200, 65, 140), t);
                gfx_put_pixel(bar_x + px, bar_y + py, c);
            }
        }

        doublebuf_swap();
        timer_sleep(55);
    }

    // Финальная пауза на полном прогресс-баре.
    timer_sleep(250);
}

// ============================================================================
// Главная функция ядра
// ============================================================================

void kernel_main(uint32_t magic, uint32_t addr) {
    // Раннее включение COM1 для отладочных сообщений (виден через -serial stdio)
    dbg_init();
    dbg_puts("[BOOT] AMPLIFY-OS kernel starting...\n");

    // Инициализация VGA (текстовый режим — используется до перехода в графику)
    vga_init();

    // Проверка Multiboot
    if (magic != 0x2BADB002) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("ERROR: Invalid Multiboot magic!");
        while (1) __asm__ volatile("hlt");
    }

    // ========================================================================
    // Инициализация базовых подсистем
    // ========================================================================

    gdt_init();
    idt_init();
    syscall_init();
    timer_init();
    keyboard_init();

    // Память
    multiboot_info_t* mbi = (multiboot_info_t*)addr;
    uint32_t mem_size = 128 * MB;
    if (mbi->flags & 0x1) {
        mem_size = (mbi->mem_upper + 1024) * 1024;
    }

    extern uint32_t _kernel_end;
    uint32_t kernel_end_addr = (uint32_t)&_kernel_end;

    // InitRD модуль
    uint32_t initrd_start = 0;
    uint32_t initrd_end = 0;
    if (mbi->flags & (1 << 3) && mbi->mods_count > 0) {
        multiboot_module_t* mods = (multiboot_module_t*)mbi->mods_addr;
        initrd_start = mods[0].mod_start;
        initrd_end = mods[0].mod_end;
        if (initrd_end > kernel_end_addr) {
            kernel_end_addr = initrd_end;
        }
    }

    pmm_init(mem_size, kernel_end_addr);
    if (initrd_start != 0 && initrd_end > initrd_start) {
        // Резервируем регион InitRD, если он лежит отдельно от ядра
        pmm_reserve_region(initrd_start, initrd_end - initrd_start);
    }
    heap_init();
    paging_init();
    // vm_init оборачивает уже активный kernel_page_directory в общее
    // ядерное vm_space. Должен вызываться после paging_init().
    vm_init();

    // Этап 21 + Этап 30: ACPI, LAPIC, IOAPIC и SMP.
    //   1. acpi_init        — RSDP→RSDT→MADT, заполняет cpu_lapic_ids[],
    //                          local_apic_base, ioapic_base.
    //   2. lapic_init        — включаем LAPIC BSP'а, маскируем LVT'ы.
    //   3. ioapic_init       — discovery + маскируем все redirection-entries
    //                          (IRQ-роутинг у нас остался на PIC).
    //   4. cpu_stats_init    — инициализируем per-CPU счётчики, регистрируем
    //                          BSP в slot[0]. Должно быть до smp_init.
    //   5. smp_init          — рассылаем INIT-SIPI-SIPI, AP'ы стартуют
    //                          с трамплина 0x8000 и заходят в ap_main(),
    //                          увеличивая smp_cpus_started.
    acpi_init();
    lapic_init();
    ioapic_init();
    cpu_stats_init();
    smp_init();

    // Этап 33A: детекция возможностей CPU (Long Mode, PAE, NX) +
    // построение PML4 + однократная демонстрация перехода 32->64->32.
    // Безопасно вызывать здесь: paging уже настроен, прерывания включаются
    // позже через task_init. Если CPU не поддерживает LM — просто заполняет
    // cpu_info_t для UI и помечает demo как unsupported.
    longmode_init();

    // Файловая система
    vfs_init();
    vfs_node_t* initrd_root_node = NULL;
    if (initrd_start != 0) {
        initrd_root_node = initrd_init(initrd_start);
        if (initrd_root_node) {
            vfs_set_root(initrd_root_node);
        }
    }

    // Инициализация ATA и FAT32
    ata_init();
    fat32_init();
    
    // Монтирование FAT32 (ATA disk)
    vfs_node_t* fat32_root = fat32_mount(0, 0); // Primary Master, LBA 0
    if (fat32_root) {
        vfs_mount("/disk", fat32_root, "fat32");
    }

    // ========================================================================
    // Этап 28: USB Mass Storage
    // ------------------------------------------------------------------------
    // 1. usb_block_init() поднимает UHCI, делает enumeration единственного
    //    подключённого устройства, шлёт INQUIRY+TUR+READ_CAPACITY.
    // 2. Если флешка определилась — пробуем замонтировать её первый раздел
    //    как FAT32 в /usb. На QEMU usb-storage по умолчанию super-floppy
    //    (BPB лежит в LBA 0).
    // 3. После этого File Explorer автоматически покажет /usb с её
    //    содержимым.
    // ========================================================================
    dbg_puts("[kernel] usb_block_init begin\n");
    if (usb_block_init()) {
        dbg_puts("[kernel] usb_block_init OK, mounting FAT32 on /usb\n");
        vfs_node_t* usb_root = fat32_mount(USB_BLOCK_DRIVE, 0);
        if (usb_root) {
            vfs_mount("/usb", usb_root, "fat32");
            vga_puts("[USB] auto-mounted at /usb\n");
        } else {
            vga_puts("[USB] no FAT32 found on USB\n");
        }
    } else {
        dbg_puts("[kernel] no USB Mass Storage detected (skip /usb)\n");
    }

    // Инициализация подсистемы Swap (Этап 20)
    swap_init();

    // Инициализация POSIX shared memory (Стадия F).
    shm_init();

    // Инициализация ELF загрузчика и динамического линкера (Этап 18)
    dynlink_init();

    // Инициализация подсистемы TrueType-шрифтов (Этап 21)
    // Пытается загрузить /default.ttf из InitRD
    ttf_system_init();

    // ========================================================================
    // Этап 24: инициализация звука (AC'97)
    // ------------------------------------------------------------------------
    // audio_init() выполняет pci_find_device() по известным VENDOR/DEVICE,
    // поднимает codec (cold reset), настраивает BDL и регистрирует IRQ.
    // Если AC'97 не найдено (например, на реальном железе без этой карты) —
    // функция возвращает false, а все последующие audio_play_*() становятся
    // no-op. Таким образом звук — опциональная подсистема, он не блокирует
    // загрузку даже при отсутствии устройства.
    // ========================================================================
    dbg_puts("[kernel] audio_init begin\n");
    if (audio_init()) {
        dbg_puts("[kernel] audio_init OK, setting volume\n");
        audio_set_volume(80);  // 80% громкости по умолчанию
    } else {
        dbg_puts("[kernel] audio_init returned false (no hw or skip)\n");
    }
    dbg_puts("[kernel] audio_init done\n");

    // ========================================================================
    // Этап 25: сетевая подсистема.
    // ------------------------------------------------------------------------
    // net_init() сканирует PCI и поднимает драйвер RTL8139 (если карта есть),
    // регистрирует интерфейс "eth0" в абстракции netif. Ethernet-слой уже
    // готов принимать фреймы через IRQ и логировать их в serial.
    // При отсутствии карты функция тихо возвращает false — загрузка
    // продолжается штатно.
    // ========================================================================
    dbg_puts("[kernel] net_init begin\n");
    if (net_init()) {
        // Этап 27: берём IP через DHCP.
        // QEMU-slirp сам отвечает на DHCP и выдаёт 10.0.2.15/24, gw=10.0.2.2,
        // dns=10.0.2.3. Процесс асинхронный: dhcp_start() только шлёт DISCOVER,
        // а netif_set_ip4 вызовется внутри dhcp_on_udp при получении ACK.
        //
        // Fallback: если за ~500k спинов статус не BOUND, прописываем статику
        // (такое бывает, если QEMU запущен без slirp или с restrict=on).
        dhcp_start();
        for (int spin = 0; spin < 500000; spin++) {
            if (dhcp_is_bound()) break;
            __asm__ volatile ("pause");
        }
        if (!dhcp_is_bound()) {
            // НЕ выставляем фейковый QEMU SLIRP-адрес 10.0.2.15 / gw 10.0.2.2:
            // на реальных сетях (VMware NAT/Bridged) он не маршрутизируется и
            // приводит к молчаливому отказу сети. Оставляем интерфейс без IP —
            // DHCP может довязаться позже (или пользователь задаст статику).
            dbg_puts("[kernel] DHCP not bound at boot — interface idle (no QEMU fallback)\n");
        }
        // Этап 34: подсистемы прикладного уровня.
        dns_init();
    }
    dbg_puts("[kernel] net_init done\n");

    // ========================================================================
    // Инициализация графического режима (VESA Framebuffer)
    // ========================================================================

    int graphics_ok = 0;

    // Проверяем, предоставил ли GRUB информацию о фреймбуфере
    // Бит 12 в flags означает: framebuffer info доступна.
    //
    // ВНИМАНИЕ: при загрузке через QEMU `-kernel` встроенный multiboot-loader
    // QEMU НЕ выставляет этот бит и не настраивает VBE. Поэтому graphics_ok
    // останется 0 и ядро уйдёт в текстовый shell-fallback. Чтобы получить
    // рабочий стол, нужно грузиться через ISO с GRUB (`make run` теперь это
    // делает по умолчанию).
    if (mbi->flags & (1 << 12)) {
        uint32_t fb_phys = (uint32_t)(mbi->fb_addr & 0xFFFFFFFF);
        uint32_t fb_w = mbi->fb_width;
        uint32_t fb_h = mbi->fb_height;
        uint32_t fb_p = mbi->fb_pitch;
        uint8_t  fb_bpp = mbi->fb_bpp;

        // Инициализация фреймбуфера (маппинг в виртуальную память)
        if (framebuffer_init(fb_phys, fb_w, fb_h, fb_p, fb_bpp)) {
            if (doublebuf_init()) {
                graphics_ok = 1;
                wm_init();
                cursor_init();
                mouse_init();
                startmenu_init();
            } else {
                dbg_puts("[kernel] gfx: doublebuf_init FAILED — fallback to shell\n");
            }
        } else {
            dbg_puts("[kernel] gfx: framebuffer_init FAILED — fallback to shell\n");
        }
    } else {
        dbg_puts("[kernel] gfx: no multiboot framebuffer info — boot via ISO/GRUB to get desktop\n");
    }

    // ========================================================================
    // Графический или текстовый режим
    // ========================================================================

    if (graphics_ok) {
        // === ГРАФИЧЕСКИЙ РЕЖИМ ===

        // Splash screen (анимация логотипа + мелодия) рисуем ДО task_init,
        // чтобы планировщик не вмешивался в чёткий 24-кадровый цикл.
        show_splash_screen();
        task_init();

        // ====================================================================
        // Этап 32: безопасность и многопользовательский режим.
        // ------------------------------------------------------------------
        // Перед инициализацией рабочего стола показываем мастер первого
        // запуска (если БД пользователей пуста) или экран входа (если уже
        // есть хотя бы один пользователь). Эта функция не вернёт управление,
        // пока не будет открыта валидная сессия. После — sec_get_session()
        // возвращает текущего пользователя, и VFS использует его для
        // проверок RWX-прав.
        // ====================================================================
        dbg_puts("[kernel] security_run begin\n");
        security_run();
        dbg_puts("[kernel] security_run done — entering desktop\n");

        // Этап 34: после монтирования диска и логина — загрузить
        // пользовательскую сетевую конфигурацию (если она была сохранена).
        netcfg_init();

        dbg_puts("[kernel] desktop_init begin\n");
        desktop_init();
        dbg_puts("[kernel] desktop_init done, apps_register_all begin\n");
        apps_register_all();
        dbg_puts("[kernel] apps_register_all done\n");

        // Этап 24: приветственный звук при старте GUI.
        // Файл /startup.wav генерируется host-утилитой tools/makestartup
        // при сборке InitRD.
        dbg_puts("[kernel] audio_play_wav begin\n");
        audio_play_wav("/startup.wav");
        dbg_puts("[kernel] audio_play_wav done\n");

        // Этап 33: вместо Ring 3 «XOR Pattern»-демо автоматически открываем
        // полноценный «Диспетчер задач» — kernel-side GUI приложение
        // с прямым доступом к task_t (PID, имя, состояние, CPU%).
        // task_exec_app("app.bin");
        apps_open_taskmgr();

        // Автопроверка Стадий C, D, E, F:
        //   exectest → execve(/forktest.elf) → forktest → fork() → child.
        //   cwdtest — chdir/getcwd и нормализация "..".
        //   shmtest — POSIX shared memory + fork-обмен через shm.
        // Все задачи печатают результаты через sys_write, зеркалимый в COM1.
        task_exec_elf("/exectest.elf");
        task_exec_elf("/cwdtest.elf");
        task_exec_elf("/shmtest.elf");

        // Загрузка ELF-программ с диска (Этап 18)
        // Если на диске есть hello.elf — загрузим его
        // task_exec_elf("/disk/nexus/bin/hello.elf");

        // Первая отрисовка рабочего стола
        wm_require_redraw();
        wm_update();  // Прогнать один кадр, чтобы desktop попал в back-буфер

        // Этап 21: демо TrueType + PNG поверх рабочего стола
        stage21_demo_overlay();

        // ====================================================================
        // Этап 30: основной цикл BSP с per-CPU статистикой.
        // wm_update() — реальная работа (отрисовка/события), считаем как busy.
        // pause-cikl - ozhidanie sleduyushchego IRQ, schitaem kak idle.
        // Embers smotryat (busy_ticks vs idle_ticks) yadra slot=0.
        // ====================================================================
        while (1) {
            wm_update();
            cpu_stats[0].busy_ticks++;
            for (int i = 0; i < 32000; i++) {
                cpu_stats[0].idle_ticks++;
                __asm__ volatile ("pause");
            }
            __asm__ volatile("hlt");
        }
    } else {
        // === TEKSTOVYJ REZHIM (fallback) ===

        speaker_play_boot_melody();

        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        vga_puts("                                                                                ");
        vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLUE);
        vga_puts("                  AMPLIFY-OS - v2.0 (Shell Edition)                              ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE);
        vga_puts("                                                                                ");
        vga_println("");

        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_println("Zagruzka sistemy...");
        vga_println("");

        print_status_ok("Bootloader (GRUB Multiboot)");
        print_status_ok("GDT (Global Descriptor Table)");
        print_status_ok("IDT (Interrupt Descriptor Table)");
        print_status_ok("PIT Timer (100 Hz)");
        print_status_ok("PS/2 Keyboard");
        print_status_ok("PMM (Physical Memory Manager)");
        print_status_ok("Heap (kmalloc/kfree)");
        print_status_ok("Paging (Virtual Memory)");
        print_status_ok("VFS (Virtual File System)");

        if (initrd_root_node) {
            print_status_ok("InitRD (RAM Disk)");
        } else {
            print_status_fail("InitRD (modul' ne najden)");
        }

        if (ata_is_present(0)) {
            print_status_ok("ATA Drive 0 (Primary Master)");
        } else {
            print_status_fail("ATA Drive 0 (ne najden)");
        }

        if (fat32_root) {
            print_status_ok("FAT32 (smontirovana v /disk)");
        } else {
            print_status_fail("FAT32 (oshibka montirovaniya)");
        }

        print_status_fail("Framebuffer (VESA ne dostupna)");
        print_status_ok("Shell (Komandnaya obolochka)");

        vga_println("");

        pmm_info_t pmm = pmm_get_info();
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("Pamyat': ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_print_dec(pmm.total_memory / MB);
        vga_puts(" MB (svobodno: ");
        vga_print_dec(pmm.free_memory / MB);
        vga_println(" MB)");
        vga_println("");

        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_println("Vse komponenty zagruzheny uspeshno!");
        vga_println("");

        task_init();
        shell_run();
    }

    while (1) {
        __asm__ volatile("hlt");
    }
}

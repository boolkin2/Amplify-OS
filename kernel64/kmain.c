// ============================================================================
// kernel64/kmain.c — Этап 33C: 64-битная точка входа AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Этот файл компилируется с -m64 (x86_64 SysV ABI) и линкуется в отдельный
// бинарь `kernel64.bin`. Это ОТДЕЛЬНОЕ ядро, не использующее код 32-битного
// kernel/. Цель этапа 33C — доказать, что AMPLIFY-OS реально работает
// в 64-битном режиме: грузится через GRUB, входит в Long Mode, исполняет
// 64-битный код, рисует сообщение на экране, читает CPUID.
// ============================================================================

#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// VGA text-mode 80×25 — простой и надёжный способ показать "живые" данные
// без необходимости настраивать VBE framebuffer.
// ----------------------------------------------------------------------------
#define VGA_BUFFER  ((volatile uint16_t*)0xB8000)
#define VGA_COLS    80
#define VGA_ROWS    25

// VGA attribute byte: high 4 bits — фон, low 4 — цвет текста.
#define ATTR(fg, bg)  (((bg) << 4) | (fg))

enum {
    BLACK = 0, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LGREY,
    DGREY, LBLUE, LGREEN, LCYAN, LRED, LMAGENTA, YELLOW, WHITE
};

// Курсор VGA (logical).
static int vga_row = 0;
static int vga_col = 0;
static uint8_t vga_attr = ATTR(WHITE, BLACK);

static void vga_clear(uint8_t bg) {
    uint16_t cell = ((uint16_t)ATTR(WHITE, bg) << 8) | ' ';
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        VGA_BUFFER[i] = cell;
    }
    vga_row = 0;
    vga_col = 0;
    vga_attr = ATTR(WHITE, bg);
}

static void vga_putc(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else {
        if (vga_col >= VGA_COLS) { vga_col = 0; vga_row++; }
        if (vga_row >= VGA_ROWS) {
            // Простая прокрутка (сдвигаем все строки на одну вверх).
            for (int i = 0; i < (VGA_ROWS - 1) * VGA_COLS; i++) {
                VGA_BUFFER[i] = VGA_BUFFER[i + VGA_COLS];
            }
            uint16_t cell = ((uint16_t)vga_attr << 8) | ' ';
            for (int i = (VGA_ROWS - 1) * VGA_COLS;
                 i < VGA_ROWS * VGA_COLS; i++) {
                VGA_BUFFER[i] = cell;
            }
            vga_row = VGA_ROWS - 1;
        }
        VGA_BUFFER[vga_row * VGA_COLS + vga_col] =
            ((uint16_t)vga_attr << 8) | (uint8_t)c;
        vga_col++;
    }
}

static void vga_puts(const char* s) {
    while (*s) vga_putc(*s++);
}

static void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_attr = ATTR(fg, bg);
}

static void vga_print_hex(uint64_t v, int digits) {
    static const char hex[] = "0123456789ABCDEF";
    vga_putc('0'); vga_putc('x');
    for (int i = digits - 1; i >= 0; i--) {
        vga_putc(hex[(v >> (i * 4)) & 0xF]);
    }
}

static void vga_print_dec(uint64_t v) {
    char buf[24];
    int  i = 0;
    if (v == 0) { vga_putc('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) vga_putc(buf[--i]);
}

// ----------------------------------------------------------------------------
// Serial-debug на COM1 (для логов в `make run-64`).
// ----------------------------------------------------------------------------
static inline void outb(uint16_t port, uint8_t v) {
    __asm__ __volatile__("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x03);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void serial_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) { /* spin */ }
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char* s) {
    while (*s) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s++);
    }
}

// ----------------------------------------------------------------------------
// CPUID
// ----------------------------------------------------------------------------
static inline void cpuid(uint32_t leaf, uint32_t subleaf,
                         uint32_t* a, uint32_t* b,
                         uint32_t* c, uint32_t* d) {
    __asm__ __volatile__("cpuid"
        : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
        : "a"(leaf), "c"(subleaf));
}

// ----------------------------------------------------------------------------
// Главная точка входа
// Вызывается из boot.asm с RDI=multiboot magic, RSI=multiboot info.
// ----------------------------------------------------------------------------
void kmain64(uint64_t mb_magic, uint64_t mb_info) {
    serial_init();
    vga_clear(BLUE);

    // Banner.
    vga_set_color(YELLOW, BLUE);
    vga_puts("================================================================================");
    vga_set_color(WHITE, BLUE);
    vga_puts(" ");
    vga_set_color(LCYAN, BLUE);
    vga_puts("        AMPLIFY-OS  64-bit Kernel  (Stage 33C)                                 ");
    vga_set_color(YELLOW, BLUE);
    vga_puts("================================================================================");
    vga_set_color(WHITE, BLUE);

    serial_puts("\n[k64] AMPLIFY-OS 64-bit kernel booted!\n");

    vga_puts("\n");
    vga_puts(" Long Mode active. ");
    vga_set_color(LGREEN, BLUE);
    vga_puts("kmain64() running in 64-bit mode.\n");
    vga_set_color(WHITE, BLUE);

    vga_puts("\n");
    vga_puts(" sizeof(void*) = ");
    vga_set_color(LCYAN, BLUE);
    vga_print_dec(sizeof(void*));
    vga_set_color(WHITE, BLUE);
    vga_puts(" bytes        ");

    vga_puts("sizeof(long) = ");
    vga_set_color(LCYAN, BLUE);
    vga_print_dec(sizeof(long));
    vga_set_color(WHITE, BLUE);
    vga_puts(" bytes\n");

    // Multiboot info.
    vga_puts("\n Multiboot magic: ");
    vga_set_color(LCYAN, BLUE);
    vga_print_hex(mb_magic, 8);
    vga_set_color(WHITE, BLUE);
    vga_puts("   info ptr: ");
    vga_set_color(LCYAN, BLUE);
    vga_print_hex(mb_info, 16);
    vga_set_color(WHITE, BLUE);
    vga_puts("\n");

    // CPUID vendor.
    uint32_t a, b, c, d;
    cpuid(0, 0, &a, &b, &c, &d);
    char vendor[13];
    *(uint32_t*)&vendor[0] = b;
    *(uint32_t*)&vendor[4] = d;
    *(uint32_t*)&vendor[8] = c;
    vendor[12] = 0;
    vga_puts("\n CPU vendor: ");
    vga_set_color(LGREEN, BLUE);
    vga_puts(vendor);
    vga_set_color(WHITE, BLUE);
    vga_puts("\n");

    // CPUID brand.
    uint32_t max_ext;
    cpuid(0x80000000, 0, &max_ext, &b, &c, &d);
    if (max_ext >= 0x80000004) {
        char brand[49];
        for (int i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, &a, &b, &c, &d);
            uint32_t* p = (uint32_t*)&brand[i * 16];
            p[0] = a; p[1] = b; p[2] = c; p[3] = d;
        }
        brand[48] = 0;
        vga_puts(" CPU brand:  ");
        vga_set_color(LGREEN, BLUE);
        vga_puts(brand);
        vga_set_color(WHITE, BLUE);
        vga_puts("\n");
    }

    // Подтверждение возможностей.
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    int lm = (d >> 29) & 1;
    int nx = (d >> 20) & 1;
    vga_puts("\n CPUID 0x80000001 EDX bits: ");
    vga_set_color(LCYAN, BLUE);
    vga_print_hex(d, 8);
    vga_set_color(WHITE, BLUE);
    vga_puts("\n   Long Mode: ");
    vga_set_color(lm ? LGREEN : LRED, BLUE);
    vga_puts(lm ? "yes" : "no");
    vga_set_color(WHITE, BLUE);
    vga_puts("    NX bit: ");
    vga_set_color(nx ? LGREEN : LRED, BLUE);
    vga_puts(nx ? "yes" : "no");
    vga_set_color(WHITE, BLUE);
    vga_puts("\n");

    // RIP (через метку).
    uint64_t rip;
    __asm__ __volatile__(
        "lea (%%rip), %0\n"
        : "=r"(rip));
    vga_puts("\n Current RIP: ");
    vga_set_color(LCYAN, BLUE);
    vga_print_hex(rip, 16);
    vga_set_color(WHITE, BLUE);
    vga_puts("\n");

    // RSP.
    uint64_t rsp;
    __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
    vga_puts(" Current RSP: ");
    vga_set_color(LCYAN, BLUE);
    vga_print_hex(rsp, 16);
    vga_set_color(WHITE, BLUE);
    vga_puts("\n");

    // CR3 (PML4 root).
    uint64_t cr3;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(cr3));
    vga_puts(" CR3 (PML4):  ");
    vga_set_color(LCYAN, BLUE);
    vga_print_hex(cr3, 16);
    vga_set_color(WHITE, BLUE);
    vga_puts("\n");

    // Финал.
    vga_puts("\n");
    vga_set_color(BLACK, YELLOW);
    vga_puts(" 64-bit kernel works. ");
    vga_set_color(WHITE, BLUE);
    vga_puts(" Hint: stage 33C is a tech preview.\n");
    vga_puts(" Press CTRL+ALT+G to release mouse, CTRL+C in console to quit QEMU.\n");

    serial_puts("[k64] vendor: ");   serial_puts(vendor);   serial_puts("\n");
    serial_puts("[k64] long mode: "); serial_puts(lm ? "yes" : "no"); serial_puts("\n");
    serial_puts("[k64] sizeof(void*) = 8 -> 64-bit confirmed\n");
    serial_puts("[k64] kmain64 idle loop\n");

    // Idle loop.
    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

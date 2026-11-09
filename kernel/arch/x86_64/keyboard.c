// ============================================================================
// keyboard.c — PS/2 keyboard driver, scancode set 1.
// ============================================================================
#include "keyboard.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "ioapic.h"
#include "acpi.h"
#include "lapic.h"
#include "serial.h"

// Scancode set 1 → ASCII карта (lower 128 байт).  Только США-раскладка.
static const char scan_to_ascii[128] = {
    0,    27,  '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']', '\n', 0, /*Ctrl*/
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, /*LShift*/ '\\','z','x','c','v','b','n','m',',','.','/',
    0, /*RShift*/ '*', 0, /*Alt*/ ' ',
    0, /*Caps*/
    // F1..F10 + others — пропускаем
};

static const char scan_to_ascii_shift[128] = {
    0,    27,  '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}', '\n', 0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,    '|','Z','X','C','V','B','N','M','<','>','?',
    0,    '*', 0,   ' ',
    0,
};

#define KBD_BUF_SIZE 128
static volatile char    kbd_buf[KBD_BUF_SIZE];
static volatile int     kbd_head = 0;
static volatile int     kbd_tail = 0;
static volatile bool    shift_down  = false;
static volatile bool    ctrl_down   = false;
static volatile bool    super_down  = false;
static volatile bool    extended    = false;

static void kbd_push(char c) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) return;     // overflow drop
    kbd_buf[kbd_head] = c;
    kbd_head = next;
}

static void kbd_handle_scancode(uint8_t sc);

// [50.5] Перехватчик «сырых» скан-кодов (см. keyboard.h). Один указатель:
// читается в обработчике прерывания, пишется главным потоком целиком.
static volatile keyboard_raw_fn g_raw_sink;
void keyboard_set_raw_sink(keyboard_raw_fn fn) { g_raw_sink = fn; }

static void keyboard_irq(registers64_t* r) {
    (void)r;

    // ВАЖНО: читаем 0x60 ТОЛЬКО если в буфере действительно есть данные
    // ОТ КЛАВИАТУРЫ (status bit 0 = OBF, bit 5 = 0). Иначе мы:
    //   а) можем вытянуть первый байт mouse-пакета и сломать синхронизацию;
    //   б) при паразитном IRQ (level-triggered echo / cross-route) push'нем
    //      случайный 0x00 / scancode из старого слота → визуальный дубликат.
    uint8_t status = inb(0x64);
    if (!(status & 0x01) || (status & 0x20)) {
        // OBF=0 (нет данных) ИЛИ AUX=1 (от мыши) — спокойно EOI и выход.
        goto out;
    }

    kbd_handle_scancode(inb(KBD_DATA_PORT));

out:
    lapic_eoi();
    // 37.6c-1 fix: при IOAPIC PIC IRQ1 замаскирован (см. keyboard_init),
    // pic_send_eoi не нужен и был источником дубль-доставки.
    // Для legacy hardware без IOAPIC оставляем fallback.
    if (!ioapic_present()) pic_send_eoi(1);
}

// ----------------------------------------------------------------------------
// [41.2] Разбор одного скан-кода набора 1 (то, что отдаёт 8042). Вынесен из
// обработчика IRQ1, чтобы тем же путём шли клавиши virtio-клавиатуры
// (keyboard_inject_scancode): модификаторы, Super-комбинации, Ctrl+буква —
// одна и та же логика для обоих источников. Вызывается только из обработчиков
// прерываний (IF=0), поэтому общего состояния не гоняет.
// ----------------------------------------------------------------------------
static void kbd_handle_scancode(uint8_t sc) {
    // Extended prefix.
    if (sc == 0xE0) {
        extended = true;
        return;
    }

    // [50.5] Окно гостя phantom забирает клавиши целиком — с отпусканиями и
    // модификаторами, которых приложения ОС не видят (им достаются готовые
    // символы). Префикс E0 уже разобран выше и передан флагом; если
    // перехватчик код не взял, он идёт обычным путём с тем же флагом.
    keyboard_raw_fn raw = g_raw_sink;
    if (raw && raw(sc, extended)) { extended = false; return; }

    bool released = (sc & 0x80) != 0;
    uint8_t code  = sc & 0x7F;

    // Super is a Pearl shell modifier, not a printable key.
    if (extended && (code == 0x5B || code == 0x5C)) {
        super_down = !released;
        extended = false;
        return;
    }

    // Shift handling.
    if (code == 0x2A || code == 0x36) {
        shift_down = !released;
        extended = false;
        return;
    }

    // Ctrl handling (LCtrl = 0x1D; RCtrl = 0xE0 0x1D — оба отдают code=0x1D).
    if (code == 0x1D) {
        ctrl_down = !released;
        extended = false;
        return;
    }

    if (released) {
        extended = false;
        return;
    }

    if (extended) {
        char special = 0;
        if (code == 0x4B) special = KBD_KEY_LEFT;
        else if (code == 0x4D) special = KBD_KEY_RIGHT;
        else if (code == 0x48) special = KBD_KEY_UP;
        else if (code == 0x50) special = KBD_KEY_DOWN;
        else if (code == 0x49) special = KBD_KEY_PGUP;
        else if (code == 0x51) special = KBD_KEY_PGDN;
        else if (code == 0x47) special = KBD_KEY_HOME;
        else if (code == 0x4F) special = KBD_KEY_END;
        else if (code == 0x53) special = KBD_KEY_DELETE;
        if (special) kbd_push(special);
        extended = false;
        return;
    }

    char c = shift_down ? scan_to_ascii_shift[code] : scan_to_ascii[code];
    // Ctrl+letter → управляющие коды 1..26 (Ctrl+A=1, Ctrl+S=19, и т.д.),
    // что соответствует стандартной семантике ASCII и позволяет приложениям
    // ловить hotkeys без отдельного API.
    if (super_down && c == '\t') c = KBD_KEY_SUPER_TAB;
    else if (super_down && c == ' ') c = KBD_KEY_SUPER_PULSE;
    else if (super_down && (c == 'n' || c == 'N')) c = KBD_KEY_SUPER_BEACON;
    else if (super_down) c = KBD_KEY_SUPER_HOME;
    else if (c == '\t' && shift_down) c = KBD_KEY_BACKTAB;
    else if (ctrl_down && c == ' ') c = KBD_KEY_MIND;       // [49] Ctrl+Space
    else if (ctrl_down && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
    else if (ctrl_down && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
    if (c) kbd_push(c);
    extended = false;
}

void keyboard_inject_scancode(uint8_t sc) { kbd_handle_scancode(sc); }

void keyboard_init(void) {
    kbd_head = kbd_tail = 0;
    shift_down = false;
    ctrl_down = false;
    super_down = false;
    extended = false;

    idt64_set_handler(33, keyboard_irq);     // vector 32 + IRQ 1

    if (ioapic_present()) {
        uint8_t gsi = acpi_irq_to_gsi[1];
        ioapic_route_irq(gsi, 33, (uint8_t)acpi_bsp_lapic_id, false);
        // КРИТИЧНО: маскируем IRQ1 в legacy PIC, иначе клавиатура шлёт
        // прерывание ПО ОБОИМ маршрутам и keyboard_irq дёргается дважды
        // (источник «дубликатов клавиш» в shell/login).
        pic_set_mask(1);
    } else {
        // Legacy fallback: только PIC.
        pic_clear_mask(1);
    }

    // Дренаж входного буфера контроллера 8042: в QEMU после reset там
    // часто остаётся «мусорный» scancode (0xAA self-test pass и т.п.).
    // Убираем, чтобы он не превратился в первый «фантомный» символ shell.
    for (int i = 0; i < 16; i++) {
        if (!(inb(0x64) & 0x01)) break;
        (void)inb(0x60);
    }

    serial_puts("[kbd] initialized (IRQ 1, IOAPIC GSI 1 -> vector 33, legacy PIC masked)\n");
}

bool keyboard_has_char(void) { return kbd_head != kbd_tail; }

char keyboard_get_char(void) {
    if (kbd_head == kbd_tail) return 0;
    char c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return c;
}

char keyboard_wait_char(void) {
    while (!keyboard_has_char()) __asm__ __volatile__("sti; hlt");
    return keyboard_get_char();
}

bool keyboard_shift_pressed(void) { return shift_down; }
bool keyboard_ctrl_pressed(void) { return ctrl_down; }
bool keyboard_super_pressed(void) { return super_down; }

// ============================================================================
// mouse.c — PS/2 mouse driver, BAR controller @ 0x60/0x64.
// ============================================================================
#include "mouse.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "ioapic.h"
#include "acpi.h"
#include "lapic.h"
#include "fb.h"
#include "serial.h"
#include "virtio_input.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define KBD_CMD    0x64

// Команды PS/2 controller для mouse:
#define KBD_CMD_WRITE_AUX   0xD4    // следующий байт — для AUX (mouse)
#define KBD_CMD_ENABLE_AUX  0xA8

static volatile mouse_state_t g_state = {0, 0, 0};
static volatile bool          g_event_pending = false;

// Парсер 3-байтного пакета: накапливаем bytes по байту на IRQ.
static volatile uint8_t  pkt[3];
static volatile uint8_t  pkt_idx = 0;

// Диагностические счётчики (используются mouse_debug_dump). Не дорого
// держать всегда — это волатильный uint32_t, инкрементируется в IRQ и
// polling-пути. Дамп вызывается только когда хотят.
static volatile uint32_t dbg_irq_calls = 0;
static volatile uint32_t dbg_poll_calls = 0;
static volatile uint32_t dbg_bytes_read = 0;
static volatile uint32_t dbg_packets_done = 0;
static volatile uint32_t dbg_sync_drops = 0;
static volatile uint32_t dbg_overflow_drops = 0;

// Ждём готовности input/output buffer.
static void kbd_wait_input(void) {
    int t = 100000;
    while ((inb(KBD_STATUS) & 0x02) && t--) ;
}
static void kbd_wait_output(void) {
    int t = 100000;
    while (!(inb(KBD_STATUS) & 0x01) && t--) ;
}

static void mouse_send(uint8_t cmd) {
    kbd_wait_input(); outb(KBD_CMD, KBD_CMD_WRITE_AUX);
    kbd_wait_input(); outb(KBD_DATA, cmd);
    kbd_wait_output(); inb(KBD_DATA);            // ACK (0xFA)
}

// Обработка одного байта пакета. Возвращает true если пакет завершён.
static bool process_byte(uint8_t b) {
    pkt[pkt_idx++] = b;
    dbg_bytes_read++;

    // На первом байте проверяем bit 3 = 1 (always-set синхро-маркер).
    if (pkt_idx == 1 && !(b & 0x08)) {
        pkt_idx = 0;       // потеряли синхронизацию -- сброс
        dbg_sync_drops++;
        return false;
    }

    if (pkt_idx == 3) {
        uint8_t flags = pkt[0];
        int8_t  dx = (int8_t)pkt[1];
        int8_t  dy = (int8_t)pkt[2];

        // bit 6,7 = X,Y overflow -- дропаем оба пакета.
        if (flags & 0xC0) {
            pkt_idx = 0;
            dbg_overflow_drops++;
            return false;
        }

        const aos64_fb_t* st = fb_state();
        int max_x = st->ok ? (int)st->width  - 1 : 1023;
        int max_y = st->ok ? (int)st->height - 1 : 767;

        int new_x = g_state.x + (int)dx;
        int new_y = g_state.y - (int)dy;       // PS/2 dy: + вверх
        if (new_x < 0) new_x = 0;
        if (new_y < 0) new_y = 0;
        if (new_x > max_x) new_x = max_x;
        if (new_y > max_y) new_y = max_y;

        g_state.x = new_x;
        g_state.y = new_y;
        g_state.buttons = flags & 0x07;
        g_event_pending = true;

        pkt_idx = 0;
        dbg_packets_done++;
        return true;
    }
    return false;
}

// Дрейнит ВСЕ доступные байты из 8042 буфера. Используется и в IRQ,
// и в polling-fallback из main loop.
static void drain_8042(void) {
    for (int safety = 0; safety < 64; safety++) {
        uint8_t status = inb(KBD_STATUS);
        // OBF (bit 0) = 0 -> данных больше нет.
        if (!(status & 0x01)) break;
        // bit 5 = 0 -> данные от keyboard, не от мыши.
        // ВАЖНО: не читаем их здесь, иначе клавиатура потеряет байт.
        // Просто выходим -- keyboard IRQ их сам прочитает.
        if (!(status & 0x20)) break;

        uint8_t b = inb(KBD_DATA);
        process_byte(b);
    }
}

static void mouse_irq(registers64_t* r) {
    (void)r;
    dbg_irq_calls++;
    drain_8042();
    lapic_eoi();
    if (!ioapic_present()) pic_send_eoi(12);
}

// Печатает в serial счётчики mouse: IRQ/poll calls, bytes read, packets,
// sync drops, current x/y. Вызывать периодически из main loop / WM.
void mouse_debug_dump(void) {
    serial_puts("[mouse-dbg] irq="); serial_print_dec(dbg_irq_calls);
    serial_puts(" poll="); serial_print_dec(dbg_poll_calls);
    serial_puts(" bytes="); serial_print_dec(dbg_bytes_read);
    serial_puts(" pkts="); serial_print_dec(dbg_packets_done);
    serial_puts(" sync_drop="); serial_print_dec(dbg_sync_drops);
    serial_puts(" ovf="); serial_print_dec(dbg_overflow_drops);
    serial_puts(" pos=("); serial_print_dec(g_state.x);
    serial_puts(","); serial_print_dec(g_state.y);
    serial_puts(") pkt_idx="); serial_print_dec(pkt_idx);
    serial_puts(" event_pending="); serial_print_dec(g_event_pending ? 1 : 0);
    serial_puts("\n");
}

// Polling-fallback (вызывается из WM main loop). Дрейнит буфер, если в нём
// что-то накопилось без IRQ. Безопасно вызывать на каждом кадре.
//
// HOTFIX (15.05.2026, регрессия после этапа 42):
// Заворачиваем drain_8042() в CLI/STI. Иначе IRQ12, прилетевший между
// inb(KBD_STATUS) и inb(KBD_DATA) внутри drain_8042 в polling-пути,
// запускал mouse_irq → второй drain_8042, который тоже читал DATA. После
// возврата основной drain_8042 читал уже пустой регистр (или мусор),
// process_byte получал не тот байт пакета и pkt_idx уходил из синхрона.
// Результат — курсор замирал после первого движения. Critical section
// длится ~5 µs (несколько inb), на доставку прерываний не влияет.
void mouse_poll(void) {
    dbg_poll_calls++;
    __asm__ __volatile__("cli");
    drain_8042();
    // [41.2] Устройства virtio-input, которым не досталось прерывания MSI-X,
    // опрашиваются здесь же: mouse_poll зовут все циклы UI (вход, установщик,
    // FDE, WM). С прерыванием функция ничего не делает.
    virtio_input_poll();
    __asm__ __volatile__("sti");
}

void mouse_init(void) {
    // Полный сброс 8042 PS/2 контроллера (UEFI/OVMF может оставить его
    // в неопределённом состоянии после ExitBootServices).
    // Flush: читаем все pending данные из буфера.
    for (int i = 0; i < 32; i++) {
        if (inb(KBD_STATUS) & 0x01) inb(KBD_DATA);
        else break;
    }

    // Disable обоих портов на время настройки.
    kbd_wait_input(); outb(KBD_CMD, 0xAD);   // disable keyboard port
    kbd_wait_input(); outb(KBD_CMD, 0xA7);   // disable mouse port

    // Flush снова.
    for (int i = 0; i < 32; i++) {
        if (inb(KBD_STATUS) & 0x01) inb(KBD_DATA);
        else break;
    }

    // Enable AUX device.
    kbd_wait_input(); outb(KBD_CMD, KBD_CMD_ENABLE_AUX);
    // Enable keyboard port тоже (иначе клавиатура умрёт).
    kbd_wait_input(); outb(KBD_CMD, 0xAE);

    // Включаем IRQ12 в Compaq Status Byte.
    kbd_wait_input(); outb(KBD_CMD, 0x20);                   // read status
    kbd_wait_output();
    uint8_t status = inb(KBD_DATA);
    status |= 0x02;            // bit 1 = enable mouse IRQ
    status |= 0x01;            // bit 0 = enable keyboard IRQ
    status &= ~0x20;           // bit 5 = disable mouse clock (значит включить)
    kbd_wait_input(); outb(KBD_CMD, 0x60);                   // write status
    kbd_wait_input(); outb(KBD_DATA, status);

    // Reset mouse (self-test).
    mouse_send(0xFF);          // reset
    // После reset мышь шлёт 0xAA (self-test pass) + 0x00 (device ID).
    // Ждём и читаем эти байты.
    for (int i = 0; i < 2; i++) {
        kbd_wait_output();
        inb(KBD_DATA);
    }

    // Set defaults + enable streaming.
    mouse_send(0xF6);          // set defaults
    mouse_send(0xF4);          // enable data reporting

    // Установим начальную позицию по центру экрана.
    const aos64_fb_t* st = fb_state();
    if (st->ok) {
        g_state.x = (int)st->width  / 2;
        g_state.y = (int)st->height / 2;
    }

    // IRQ.
    idt64_set_handler(44, mouse_irq);    // vector 32+12
    if (ioapic_present()) {
        uint8_t gsi = acpi_irq_to_gsi[12];
        ioapic_route_irq(gsi, 44, (uint8_t)acpi_bsp_lapic_id, false);
        // 37.6c-1 fix: маскируем PIC IRQ12 — иначе мышь шлёт два прерывания.
        pic_set_mask(12);
    } else {
        pic_clear_mask(12);
        pic_clear_mask(2);               // cascade на slave
    }

    serial_puts("[mouse] initialized (IRQ 12, IOAPIC GSI 12 -> vector 44, legacy PIC masked)\n");
}

// ----------------------------------------------------------------------------
// [41.2] Указатель от virtio-input. Планшет даёт АБСОЛЮТНЫЕ координаты: курсор
// гостя стоит ровно там, где указатель хоста, — рассинхрона, как у PS/2-мыши
// в консоли Proxmox (она шлёт только смещения), быть не может.
// Звать из обработчика прерывания или при запрещённых прерываниях.
// ----------------------------------------------------------------------------
static void clamp_to_screen(int* x, int* y) {
    const aos64_fb_t* st = fb_state();
    int max_x = st->ok ? (int)st->width  - 1 : 1023;
    int max_y = st->ok ? (int)st->height - 1 : 767;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x > max_x) *x = max_x;
    if (*y > max_y) *y = max_y;
}

void mouse_inject_abs(int x, int y, uint8_t buttons) {
    clamp_to_screen(&x, &y);
    g_state.x = x;
    g_state.y = y;
    g_state.buttons = buttons & 0x07;
    g_event_pending = true;
}

void mouse_inject_rel(int dx, int dy, uint8_t buttons) {
    int x = g_state.x + dx, y = g_state.y + dy;
    clamp_to_screen(&x, &y);
    g_state.x = x;
    g_state.y = y;
    g_state.buttons = buttons & 0x07;
    g_event_pending = true;
}

// [48.2] Колесо: щелчки копятся в обработчике, забирает их цикл WM.
static volatile int g_wheel = 0;
void mouse_inject_wheel(int notches) { g_wheel += notches; g_event_pending = true; }
bool mouse_has_wheel(void)           { return g_wheel != 0; }
int  mouse_take_wheel(void) {
    uint64_t fl;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(fl));
    int v = g_wheel;
    g_wheel = 0;
    if (fl & 0x200) __asm__ __volatile__("sti");
    return v;
}

const mouse_state_t* mouse_get_state(void) { return (const mouse_state_t*)&g_state; }
bool mouse_has_event(void)                 { return g_event_pending; }
void mouse_clear_event(void)               { g_event_pending = false; }

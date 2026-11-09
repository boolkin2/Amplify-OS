// ============================================================================
// virtio_input.c — драйвер virtio-input (см. virtio_input.h).
// ----------------------------------------------------------------------------
// Устройство (virtio 1.x §5.8): очередь 0 — eventq (устройство -> драйвер),
// очередь 1 — statusq (светодиоды; не используем). Драйвер заранее кладёт в
// eventq буферы по 8 байт, устройство возвращает их заполненными:
//     struct { le16 type; le16 code; le32 value; }      — как evdev в Linux.
// Отчёт заканчивается событием EV_SYN: до него координаты и кнопки копятся,
// по нему — применяются разом (иначе курсор шёл бы «лесенкой» X, потом Y).
//
// Клавиши приходят кодами evdev. Для основной клавиатуры они совпадают со
// скан-кодами набора 1 (KEY_ESC=1 … KEY_F12=88), расширенные (стрелки, правые
// Ctrl/Alt, Super) переводим в пару E0+код — и отдаём в тот же разбор, что и
// PS/2 (keyboard_inject_scancode): раскладка и комбинации остаются общими.
//
// Буферы событий и дескрипторы закреплены друг за другом навсегда: дескриптор
// i всегда указывает на g_dev[k].ev[i]. Общий список свободных дескрипторов
// virtq_t здесь не нужен.
// ============================================================================
#include "virtio_input.h"
#include "memory.h"
#include "virtio.h"
#include "keyboard.h"
#include "mouse.h"
#include "lapic.h"
#include "fb.h"
#include "serial.h"

#define VINPUT_MAX_DEV      4
#define VINPUT_EVENTS       64

// Выбор поля конфигурации (select) и типы событий.
#define VINPUT_CFG_ID_NAME  0x01
#define VINPUT_CFG_EV_BITS  0x11
#define VINPUT_CFG_ABS_INFO 0x12
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define REL_X  0
#define REL_Y  1
#define REL_WHEEL 8
#define ABS_X  0
#define ABS_Y  1
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} vinput_event_t;

typedef struct {
    bool           ready;
    bool           is_abs;          // планшет: EV_ABS
    bool           is_rel;          // мышь: EV_REL
    bool           has_irq;
    virtio_dev_t   dev;
    virtq_t        eventq;
    uint32_t       abs_max_x, abs_max_y;
    // Накопленное до EV_SYN состояние указателя.
    uint32_t       ax, ay;
    int            dx, dy;
    uint8_t        buttons;
    bool           moved;
    vinput_event_t ev[VINPUT_EVENTS] __attribute__((aligned(8)));
} vinput_dev_t;

static vinput_dev_t g_dev[VINPUT_MAX_DEV];
static int          g_ndev = 0;
static bool         g_need_poll = false;
static uint64_t     g_events = 0;
// Первое событие клавиатуры / указателя: печатаем по одной строке в serial —
// по ним тесты видят, что ввод действительно пошёл через virtio. Печать — не
// из обработчика прерывания (serial и klog не реентерабельны), а из
// virtio_input_poll, который зовётся из главного потока.
static volatile uint8_t g_first_key = 0, g_first_ptr = 0;   // 1 — было, 2 — напечатано
static volatile int     g_first_x = 0, g_first_y = 0;

bool     virtio_input_has_tablet(void) {
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].ready && g_dev[i].is_abs) return true;
    return false;
}
uint64_t virtio_input_event_count(void) { return g_events; }

// evdev-код клавиши -> скан-код набора 1. Возвращает код, *ext = нужен префикс E0.
static uint8_t evdev_to_set1(uint16_t code, bool* ext) {
    *ext = false;
    if (code >= 1 && code <= 88) return (uint8_t)code;      // основная клавиатура
    *ext = true;
    switch (code) {
        case 96:  return 0x1C;      // KPENTER
        case 97:  return 0x1D;      // RIGHTCTRL
        case 98:  return 0x35;      // KPSLASH
        case 100: return 0x38;      // RIGHTALT
        case 102: return 0x47;      // HOME
        case 103: return 0x48;      // UP
        case 104: return 0x49;      // PAGEUP
        case 105: return 0x4B;      // LEFT
        case 106: return 0x4D;      // RIGHT
        case 107: return 0x4F;      // END
        case 108: return 0x50;      // DOWN
        case 109: return 0x51;      // PAGEDOWN
        case 110: return 0x52;      // INSERT
        case 111: return 0x53;      // DELETE
        case 125: return 0x5B;      // LEFTMETA  (Super)
        case 126: return 0x5C;      // RIGHTMETA
        default:  *ext = false; return 0;
    }
}

static void handle_event(vinput_dev_t* d, const vinput_event_t* e) {
    g_events++;
    switch (e->type) {
    case EV_KEY:
        if (e->code >= BTN_LEFT && e->code <= BTN_MIDDLE) {
            uint8_t bit = (e->code == BTN_LEFT)  ? MOUSE_BTN_LEFT
                        : (e->code == BTN_RIGHT) ? MOUSE_BTN_RIGHT : MOUSE_BTN_MIDDLE;
            if (e->value) d->buttons |= bit; else d->buttons &= (uint8_t)~bit;
            d->moved = true;
        } else {
            bool ext;
            uint8_t sc = evdev_to_set1(e->code, &ext);
            if (!sc) break;
            if (!g_first_key) g_first_key = 1;
            if (ext) keyboard_inject_scancode(0xE0);
            // value: 1 — нажатие, 2 — автоповтор хоста (как нажатие), 0 — отпускание.
            keyboard_inject_scancode(e->value ? sc : (uint8_t)(sc | 0x80));
        }
        break;
    case EV_ABS:
        if (e->code == ABS_X) { d->ax = e->value; d->moved = true; }
        else if (e->code == ABS_Y) { d->ay = e->value; d->moved = true; }
        break;
    case EV_REL:
        if (e->code == REL_X) { d->dx += (int)e->value; d->moved = true; }
        else if (e->code == REL_Y) { d->dy += (int)e->value; d->moved = true; }
        // [48.2] Колесо есть и у мыши, и у планшета QEMU. Положение указателя
        // оно не меняет, поэтому идёт мимо накопления до EV_SYN.
        else if (e->code == REL_WHEEL) mouse_inject_wheel((int)e->value);
        break;
    case EV_SYN:
        if (!d->moved) break;
        if (d->is_abs) {
            const aos64_fb_t* st = fb_state();
            uint32_t w = st->ok ? st->width : 1024, h = st->ok ? st->height : 768;
            uint32_t mx = d->abs_max_x ? d->abs_max_x : 0x7FFF;
            uint32_t my = d->abs_max_y ? d->abs_max_y : 0x7FFF;
            int px = (int)((uint64_t)d->ax * (w - 1) / mx);
            int py = (int)((uint64_t)d->ay * (h - 1) / my);
            mouse_inject_abs(px, py, d->buttons);
            if (!g_first_ptr) { g_first_x = px; g_first_y = py; g_first_ptr = 1; }
        } else {
            mouse_inject_rel(d->dx, d->dy, d->buttons);
            d->dx = d->dy = 0;
        }
        d->moved = false;
        break;
    default:
        break;
    }
}

// Забрать использованные буферы, разобрать события и сразу вернуть буферы
// устройству. Прерывания должны быть запрещены (обработчик или mouse_poll).
static void drain(vinput_dev_t* d) {
    virtq_t* q = &d->eventq;
    bool any = false;
    while (q->last_used_idx != q->used->idx) {
        virtq_used_elem_t el = q->used->ring[q->last_used_idx % q->num];
        q->last_used_idx = (uint16_t)(q->last_used_idx + 1);
        uint32_t id = el.id;
        if (id >= VINPUT_EVENTS) continue;          // не наш дескриптор — игнор
        handle_event(d, &d->ev[id]);
        q->avail->ring[q->avail->idx % q->num] = (uint16_t)id;
        __asm__ __volatile__("sfence" ::: "memory");
        q->avail->idx = (uint16_t)(q->avail->idx + 1);
        any = true;
    }
    if (any) {
        __asm__ __volatile__("mfence" ::: "memory");
        virtq_notify(&d->dev, q);
    }
}

// Один обработчик на все устройства: вектор у каждого свой, но какое именно
// выстрелило — проще не различать, а обойти все (их не больше четырёх).
static void vinput_irq(registers64_t* r) {
    (void)r;
    for (int i = 0; i < g_ndev; i++) if (g_dev[i].ready) drain(&g_dev[i]);
    lapic_eoi();
}

void virtio_input_poll(void) {
    if (g_first_key == 1) { g_first_key = 2; serial_puts("[vinput] first key event\n"); }
    if (g_first_ptr == 1) {
        g_first_ptr = 2;
        serial_puts("[vinput] first absolute pointer event at ");
        serial_print_dec((uint64_t)g_first_x); serial_puts(",");
        serial_print_dec((uint64_t)g_first_y); serial_puts("\n");
    }
    if (!g_need_poll) return;
    for (int i = 0; i < g_ndev; i++)
        if (g_dev[i].ready && !g_dev[i].has_irq) drain(&g_dev[i]);
}

// Чтение поля конфигурации: записать select/subsel, прочитать size.
static uint8_t cfg_select(vinput_dev_t* d, uint8_t sel, uint8_t subsel) {
    virtio_dev_cfg_write8(&d->dev, 0, sel);
    virtio_dev_cfg_write8(&d->dev, 1, subsel);
    return virtio_dev_cfg_read8(&d->dev, 2);
}

static bool init_one(vinput_dev_t* d, int nth) {
    virtio_pci_probe_nth(&d->dev, VIRTIO_DEV_INPUT, nth);
    if (!d->dev.present) return false;
    if (!d->dev.is_modern) {
        serial_puts("[vinput] legacy transport is not supported for virtio-input\n");
        return false;
    }
    if (!virtio_pci_init_begin(&d->dev)) return false;
    virtio_pci_negotiate(&d->dev, 0);

    // Запись 0 таблицы MSI-X — очередь событий. Нет MSI-X или кончились
    // векторы — устройство останется на опросе из mouse_poll.
    uint16_t entry = VIRTIO_MSI_NO_VECTOR;
    if (virtio_pci_msix_setup(&d->dev) && virtio_pci_msix_route(&d->dev, 0, vinput_irq))
        entry = 0;

    // Размер очереди берём у устройства (0): virtq_init не сообщает устройству
    // уменьшенный размер, и индексы колец разошлись бы. Буферов кладём не
    // больше VINPUT_EVENTS.
    if (!virtq_init_irq(&d->dev, &d->eventq, 0, 0, entry)) {
        serial_puts("[vinput] eventq init failed\n");
        return false;
    }
    d->has_irq = d->eventq.irq;
    if (!d->has_irq) g_need_poll = true;

    // Все буферы — устройству на запись.
    virtq_t* q = &d->eventq;
    uint16_t n = q->num < VINPUT_EVENTS ? q->num : VINPUT_EVENTS;
    for (uint16_t i = 0; i < n; i++) {
        q->desc[i].addr  = virt_to_phys(&d->ev[i]);
        q->desc[i].len   = sizeof(vinput_event_t);
        q->desc[i].flags = VIRTQ_DESC_F_WRITE;
        q->desc[i].next  = 0;
        q->avail->ring[i] = i;
    }
    q->free_count = 0;
    __asm__ __volatile__("sfence" ::: "memory");
    q->avail->idx = n;

    // Что это за устройство: есть ли абсолютные оси / относительные.
    d->is_abs = cfg_select(d, VINPUT_CFG_EV_BITS, EV_ABS) != 0;
    d->is_rel = cfg_select(d, VINPUT_CFG_EV_BITS, EV_REL) != 0;
    if (d->is_abs) {
        // absinfo: min(+8) max(+12) fuzz flat res — по 4 байта.
        if (cfg_select(d, VINPUT_CFG_ABS_INFO, ABS_X) >= 8) d->abs_max_x = virtio_dev_cfg_read32(&d->dev, 12);
        if (cfg_select(d, VINPUT_CFG_ABS_INFO, ABS_Y) >= 8) d->abs_max_y = virtio_dev_cfg_read32(&d->dev, 12);
    }
    char name[40];
    uint8_t nl = cfg_select(d, VINPUT_CFG_ID_NAME, 0);
    if (nl > sizeof(name) - 1) nl = sizeof(name) - 1;
    for (uint8_t i = 0; i < nl; i++) {
        char c = (char)virtio_dev_cfg_read8(&d->dev, (uint16_t)(8 + i));
        if (!c) { nl = i; break; }          // size у QEMU включает завершающий NUL
        name[i] = (c >= 32 && c < 127) ? c : '?';
    }
    name[nl] = 0;

    d->ready = true;
    virtio_pci_init_end(&d->dev);       // DRIVER_OK — только теперь можно уведомлять
    virtq_notify(&d->dev, q);

    serial_puts("[vinput] ");
    serial_puts(name);
    serial_puts(d->is_abs ? " (tablet, abs max=" : d->is_rel ? " (mouse, relative" : " (keyboard");
    if (d->is_abs) { serial_print_dec(d->abs_max_x); serial_puts("x"); serial_print_dec(d->abs_max_y); }
    serial_puts(d->has_irq ? ") irq=MSI-X\n" : ") irq=none, polled\n");
    return true;
}

int virtio_input_init(void) {
    g_ndev = 0;
    for (int nth = 0; nth < VINPUT_MAX_DEV; nth++) {
        vinput_dev_t* d = &g_dev[g_ndev];
        d->ready = false; d->is_abs = d->is_rel = d->has_irq = false;
        d->abs_max_x = d->abs_max_y = 0;
        d->ax = d->ay = 0; d->dx = d->dy = 0; d->buttons = 0; d->moved = false;
        // g_ndev растёт ДО init_one: обработчик прерывания обходит g_dev[0..g_ndev)
        // и смотрит на ready, а первое событие может прийти сразу после DRIVER_OK.
        g_ndev++;
        if (!init_one(d, nth)) {
            g_ndev--;
            if (!d->dev.pci.found) break;       // устройств больше нет
        }
    }
    if (g_ndev == 0) serial_puts("[vinput] no virtio-input devices, PS/2 stays in charge\n");
    else {
        serial_puts("[vinput] devices: "); serial_print_dec((uint64_t)g_ndev);
        serial_puts(virtio_input_has_tablet() ? " (absolute pointer available)\n" : "\n");
    }
    return g_ndev;
}

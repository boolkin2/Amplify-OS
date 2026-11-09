// ============================================================================
// phantom_pc.c — устройства PC для гостя phantom (этап 50.2). См. phantom_pc.h.
// ----------------------------------------------------------------------------
// Точность эмуляции — «сколько нужно ядру Linux», не больше. Где это решение
// сужает поведение настоящей микросхемы, рядом написано, чем именно.
// ============================================================================
#include "phantom_pc.h"
#include <stddef.h>

#define PIT_HZ        1193182ull
#define OUT_RING      65536u            // вывод консоли гостя (степень двойки)
#define IN_RING       256u
#define KB_RING       256u              // скан-коды клавиатуры (степень двойки)
#define KB_RESP       8u                // ответов клавиатуры в очереди

typedef struct {
    uint8_t irr, isr, imr;
    uint8_t base;                       // вектор для IRQ0 этого контроллера (ICW2)
    uint8_t step;                       // какой байт инициализации ждём (0 — не идёт)
    uint8_t init4;                      // ICW1: будет ли ICW4
    uint8_t read_isr;                   // OCW3: что отдавать по чтению командного порта
} pic_t;

typedef struct {
    uint8_t  mode, rw;
    uint8_t  wstate, rstate;            // для двухбайтного доступа: 0 — ждём младший
    uint8_t  lsb;
    bool     armed;                     // счётчик загружен и считает
    bool     latched;
    uint16_t latch;
    uint32_t reload;                    // 1..65536
    uint64_t start;                     // TSC момента загрузки счётчика
} pit_ch_t;

struct pc {
    hv_vm_t* vm;
    uint64_t tsc_hz;
    uint64_t t0;                        // TSC создания: от него идут часы CMOS
    uint64_t unix0;

    pic_t    pic[2];
    uint16_t line;                      // уровни линий IRQ: прерывание даёт только фронт

    pit_ch_t ch[3];
    uint64_t irq0_next;                 // TSC следующего IRQ0 (0 — не ожидается)
    uint64_t irq0_period;               // 0 — одиночный
    uint8_t  port61, refresh;

    uint8_t  ier, lcr, mcr, scr, dll, dlm, fcr;
    bool     thre;                      // ждёт прерывание «передатчик пуст»
    bool     loop_has;                  // режим петли: байт вернулся в приёмник
    uint8_t  loop_ch;

    uint8_t  cmos_idx;
    uint8_t  port92;
    uint8_t  crtc_idx;                  // видеоконтроллер: выбранный регистр
    uint8_t  crtc[32];                  // его регистры (нужны 0Ch..0Fh: начало экрана и курсор)

    char     out[OUT_RING];
    volatile uint32_t out_w, out_r;     // пишет AP гостя, читает BSP
    char     in[IN_RING];
    volatile uint32_t in_w, in_r;       // пишет BSP, читает AP гостя

    pc_vmcall_fn vmcall;                // службы BIOS (phantom_bios.c)
    void*    vmcall_ctx;
    uint8_t  con_src;                   // кто писал в консоль последним: 1 — BIOS, 2 — порт
    bool     uart_tx_seen;              // гость уже писал в порт (см. pc_con_putc)

    // [50.4] Шина PCI и диск virtio-blk (устройство 00:01.0).
    const uint8_t* disk;                // 0 — диска и шины нет
    uint64_t disk_bytes;
    uint32_t cf8;                       // регистр адреса конфигурации PCI
    uint8_t  elcr[2];                   // «фронт/уровень» линий 8259 (только хранится)
    uint16_t vb_cmd;                    // регистр команд PCI
    uint32_t vb_bar;                    // BAR0: где гость видит регистры диска
    uint8_t  vb_irq;                    // на какую линию 8259 выведено прерывание
    uint32_t vb_feat;                   // что драйвер гостя согласился использовать
    uint32_t vb_pfn;                    // адрес очереди в памяти гостя, в страницах
    uint16_t vb_sel;
    uint8_t  vb_status, vb_isr;
    uint16_t vb_last_avail, vb_used_idx;

    // [50.5] Контроллер клавиатуры 8042 и клавиатура за ним.
    uint8_t  kb_ctr;                    // командный байт контроллера
    uint8_t  kb_out;                    // байт в выходном буфере (порт 0x60)
    bool     kb_obf;                    // выходной буфер полон
    bool     kb_out_kbd;                // … и байт пришёл от клавиатуры (ему положен IRQ1)
    bool     kb_last_cmd;               // последняя запись гостя была в порт команд
    bool     kb_scan;                   // клавиатура шлёт скан-коды (команды F4/F5)
    uint8_t  kb_ctl_cmd;                // команда контроллера, ждущая байт данных (0 — нет)
    uint8_t  kb_dev_cmd;                // команда клавиатуры, ждущая параметр (0 — нет)
    uint8_t  kb_rq[KB_RESP];            // ответы клавиатуры на команды (ACK, идентификатор…)
    uint8_t  kb_rq_r, kb_rq_n;
    uint8_t  kq[KB_RING];               // скан-коды набора 1 от окна хоста
    volatile uint32_t kq_w, kq_r;       // пишет BSP, читает AP гостя

    pc_stats_t st;
};

static struct pc g_pc;
static bool g_pc_used;

static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ----------------------------------------------------------------------------
// Контроллеры прерываний 8259 (ведущий — IRQ0..7, ведомый — IRQ8..15 через IRQ2).
// Режим — тот единственный, в котором их программирует Linux: фронт, полная
// вложенность, без автоматического EOI и без ротации приоритетов.
// ----------------------------------------------------------------------------
static void pic_pulse(struct pc* p, int irq) {
    if (irq < 8) p->pic[0].irr |= (uint8_t)(1u << irq);
    else         p->pic[1].irr |= (uint8_t)(1u << (irq - 8));
}

// Линия с уровнем (UART держит её, пока есть причина): запрос — по фронту.
static void pic_line(struct pc* p, int irq, bool level) {
    bool old = (p->line >> irq) & 1;
    if (level && !old) pic_pulse(p, irq);
    if (level) p->line |= (uint16_t)(1u << irq);
    else       p->line &= (uint16_t)~(1u << irq);
}

// Старший запрос, который контроллер сейчас выдаст, или -1. Меньший номер —
// выше приоритет; пока обслуживается (ISR) равный или старший, новый ждёт.
static int pic_highest(uint8_t req, uint8_t isr) {
    for (int i = 0; i < 8; i++) {
        if (isr & (1u << i)) return -1;
        if (req & (1u << i)) return i;
    }
    return -1;
}

static int pic_slave_out(struct pc* p) {
    return pic_highest(p->pic[1].irr & (uint8_t)~p->pic[1].imr, p->pic[1].isr);
}

static int pic_master_out(struct pc* p) {
    uint8_t req = p->pic[0].irr;
    if (pic_slave_out(p) >= 0) req |= 4;                // каскад на IRQ2
    return pic_highest(req & (uint8_t)~p->pic[0].imr, p->pic[0].isr);
}

// Цикл подтверждения (INTA): запрос переходит в «обслуживается», наружу — вектор.
static int pic_ack(struct pc* p) {
    int m = pic_master_out(p);
    if (m < 0) return -1;
    if (m == 2) {
        int s = pic_slave_out(p);
        if (s >= 0) {
            p->pic[1].irr &= (uint8_t)~(1u << s);
            p->pic[1].isr |= (uint8_t)(1u << s);
            p->pic[0].isr |= 4;
            return p->pic[1].base + s;
        }
    }
    p->pic[0].irr &= (uint8_t)~(1u << m);
    p->pic[0].isr |= (uint8_t)(1u << m);
    return p->pic[0].base + m;
}

static void pic_write(struct pc* p, int n, int a0, uint8_t v) {
    pic_t* c = &p->pic[n];
    if (!a0) {
        if (v & 0x10) {                                 // ICW1: начало инициализации
            c->step = 1;
            c->init4 = v & 1;
            c->imr = 0; c->isr = 0; c->irr = 0; c->read_isr = 0;
            // Линии считаем опущенными: устройство, которое держит уровень,
            // даст новый фронт при следующем обновлении.
            if (n == 0) p->line &= 0xFF00; else p->line &= 0x00FF;
        } else if (v & 0x08) {                          // OCW3
            if (v & 2) c->read_isr = v & 1;
        } else {                                        // OCW2
            int cmd = v >> 5;
            if (cmd == 1) {                             // обычный EOI: старший из обслуживаемых
                for (int i = 0; i < 8; i++) if (c->isr & (1u << i)) { c->isr &= (uint8_t)~(1u << i); break; }
            } else if (cmd == 3) {                      // EOI конкретного уровня
                c->isr &= (uint8_t)~(1u << (v & 7));
            }
        }
        return;
    }
    switch (c->step) {
        case 1: c->base = v & 0xF8; c->step = 2; break;             // ICW2: база векторов
        case 2: c->step = c->init4 ? 3 : 0; break;                  // ICW3: схема каскада (у PC одна)
        case 3: c->step = 0; break;                                 // ICW4
        default: c->imr = v; break;                                 // OCW1: маска
    }
}

static uint8_t pic_read(struct pc* p, int n, int a0) {
    pic_t* c = &p->pic[n];
    if (a0) return c->imr;
    return c->read_isr ? c->isr : c->irr;
}

// ----------------------------------------------------------------------------
// Таймер 8254. Счёт не ведётся — он вычисляется из TSC в момент обращения.
// ----------------------------------------------------------------------------
// Такты TSC -> такты таймера и обратно, без 128-битной арифметики (libgcc нет).
static uint64_t tsc_to_pit(struct pc* p, uint64_t dt) {
    return (dt / p->tsc_hz) * PIT_HZ + ((dt % p->tsc_hz) * PIT_HZ) / p->tsc_hz;
}
static uint64_t pit_to_tsc(struct pc* p, uint64_t ticks) {
    return ticks * p->tsc_hz / PIT_HZ;                  // ticks <= 65536: переполнения нет
}

static uint16_t pit_count(struct pc* p, int n, uint64_t now) {
    pit_ch_t* c = &p->ch[n];
    if (!c->armed) return 0;
    uint64_t el = tsc_to_pit(p, now - c->start);
    if (c->mode == 2 || c->mode == 3) return (uint16_t)(c->reload - (el % c->reload));
    // Одиночные режимы: после нуля счётчик продолжает убывать по кругу.
    return (uint16_t)(c->reload - el);
}

// Выход канала 2 (бит 5 порта 0x61): в режиме 0 поднимается, когда счёт дошёл
// до нуля; в режиме 3 — меандр. По нему Linux меряет интервалы.
static bool pit_out2(struct pc* p, uint64_t now) {
    pit_ch_t* c = &p->ch[2];
    if (!c->armed) return false;
    uint64_t el = tsc_to_pit(p, now - c->start);
    if (c->mode == 3) return (el % c->reload) < c->reload / 2;
    return el >= c->reload;
}

static void pit_write(struct pc* p, int port, uint8_t v, uint64_t now) {
    if (port == 3) {                                    // управляющее слово
        int sc = v >> 6;
        if (sc == 3) return;                            // «чтение состояния»: не эмулируется
        pit_ch_t* c = &p->ch[sc];
        int rw = (v >> 4) & 3;
        if (rw == 0) {                                  // защёлкнуть текущий счёт
            c->latch = pit_count(p, sc, now);
            c->latched = true;
            c->rstate = 0;
            return;
        }
        c->rw = (uint8_t)rw;
        c->mode = (v >> 1) & 7;
        if (c->mode > 5) c->mode &= 3;                  // 6 и 7 — те же 2 и 3
        c->wstate = 0; c->rstate = 0;
        c->armed = false; c->latched = false;
        if (sc == 0) { p->irq0_next = 0; p->irq0_period = 0; }
        return;
    }
    pit_ch_t* c = &p->ch[port];
    uint32_t value;
    if (c->rw == 3) {
        if (!c->wstate) { c->lsb = v; c->wstate = 1; return; }
        value = c->lsb | ((uint32_t)v << 8);
        c->wstate = 0;
    } else if (c->rw == 1) {
        value = v;
    } else {
        value = (uint32_t)v << 8;
    }
    c->reload = value ? value : 0x10000;
    c->start = now;
    c->armed = true;
    if (port == 0) {
        // Режимы 2 и 3 — периодические (тик ядра), остальные — одиночные
        // (так Linux заказывает «следующее событие» без периодического тика).
        uint64_t per = pit_to_tsc(p, c->reload);
        if (!per) per = 1;
        p->irq0_period = (c->mode == 2 || c->mode == 3) ? per : 0;
        p->irq0_next = now + per;
    }
}

static uint8_t pit_read(struct pc* p, int port, uint64_t now) {
    if (port == 3) return 0xFF;
    pit_ch_t* c = &p->ch[port];
    uint16_t v = c->latched ? c->latch : pit_count(p, port, now);
    if (c->rw == 3) {
        if (!c->rstate) { c->rstate = 1; return (uint8_t)v; }
        c->rstate = 0;
        c->latched = false;
        return (uint8_t)(v >> 8);
    }
    c->latched = false;
    return c->rw == 1 ? (uint8_t)v : (uint8_t)(v >> 8);
}

// ----------------------------------------------------------------------------
// Последовательный порт 16550A. Передача мгновенная: байт сразу уходит в
// кольцо вывода, передатчик всегда свободен.
// ----------------------------------------------------------------------------
// Байт в кольцо вывода консоли (AP гостя).
static void con_put(struct pc* p, char ch) {
    uint32_t w = p->out_w;
    if (w - __atomic_load_n(&p->out_r, __ATOMIC_ACQUIRE) < OUT_RING) {
        p->out[w & (OUT_RING - 1)] = ch;
        __atomic_store_n(&p->out_w, w + 1, __ATOMIC_RELEASE);
        p->st.tx++;
    } else {
        p->st.tx_lost++;
    }
}

static bool uart_rx_avail(struct pc* p) {
    return p->loop_has || p->in_r != __atomic_load_n(&p->in_w, __ATOMIC_ACQUIRE);
}

static void uart_irq(struct pc* p) {
    // Линию к контроллеру прерываний порт выдаёт только при OUT2 (MCR бит 3).
    bool lvl = (p->mcr & 8) && (((p->ier & 1) && uart_rx_avail(p)) || ((p->ier & 2) && p->thre));
    if (lvl && !((p->line >> 4) & 1)) p->st.irq4++;
    pic_line(p, 4, lvl);
}

static void uart_write(struct pc* p, int reg, uint8_t v) {
    bool dlab = p->lcr & 0x80;
    switch (reg) {
    case 0:
        if (dlab) { p->dll = v; break; }
        if (p->mcr & 0x10) {                            // петля: байт возвращается в приёмник
            p->loop_ch = v; p->loop_has = true;
        } else {
            // Экран BIOS и порт пишут в одну консоль. Загрузчик кончает строку
            // словом «ready.» без перевода, а ядро начинает со своей — без
            // этого они склеивались в одну.
            if (p->con_src == 1) con_put(p, '\n');
            p->con_src = 2;
            p->uart_tx_seen = true;
            con_put(p, (char)v);
        }
        // Настоящий порт на время передачи снимает «передатчик пуст» и
        // поднимает снова: контроллер прерываний видит новый фронт. Без
        // этого первый же байт остался бы последним — фронта больше не будет.
        p->thre = false; uart_irq(p);
        p->thre = true;  uart_irq(p);
        return;
    case 1:
        if (dlab) { p->dlm = v; break; }
        p->ier = v & 0x0F;
        // Разрешение прерывания передатчика при пустом передатчике сразу его
        // вызывает — на это рассчитывает драйвер, начиная передачу.
        if (p->ier & 2) p->thre = true;
        break;
    case 2:
        p->fcr = v;
        if (v & 2) p->loop_has = false;                 // сброс приёмной очереди
        break;
    case 3: p->lcr = v; break;
    case 4: p->mcr = v & 0x1F; break;
    case 7: p->scr = v; break;
    default: break;
    }
    uart_irq(p);
}

static uint8_t uart_read(struct pc* p, int reg) {
    bool dlab = p->lcr & 0x80;
    uint8_t v = 0;
    switch (reg) {
    case 0:
        if (dlab) { v = p->dll; break; }
        if (p->loop_has) { v = p->loop_ch; p->loop_has = false; }
        else if (p->in_r != __atomic_load_n(&p->in_w, __ATOMIC_ACQUIRE)) {
            v = (uint8_t)p->in[p->in_r & (IN_RING - 1)];
            __atomic_store_n(&p->in_r, p->in_r + 1, __ATOMIC_RELEASE);
            p->st.rx++;
        }
        break;
    case 1: v = dlab ? p->dlm : p->ier; break;
    case 2:
        if ((p->ier & 1) && uart_rx_avail(p)) v = 0x04;             // есть принятый байт
        else if ((p->ier & 2) && p->thre) { v = 0x02; p->thre = false; }  // чтение снимает причину
        else v = 0x01;                                              // прерываний нет
        if (p->fcr & 1) v |= 0xC0;                                  // очереди включены: это 16550A
        break;
    case 3: v = p->lcr; break;
    case 4: v = p->mcr; break;
    case 5: v = 0x60 | (uart_rx_avail(p) ? 1 : 0); break;           // передатчик всегда пуст
    case 6:
        // В режиме петли входы модема замкнуты на выходы — так ядро
        // проверяет, что порт настоящий.
        if (p->mcr & 0x10)
            v = (uint8_t)(((p->mcr & 2) ? 0x10 : 0) | ((p->mcr & 1) ? 0x20 : 0) |
                          ((p->mcr & 4) ? 0x40 : 0) | ((p->mcr & 8) ? 0x80 : 0));
        else v = 0xB0;                                              // CTS, DSR, DCD активны
        break;
    case 7: v = p->scr; break;
    default: break;
    }
    uart_irq(p);
    return v;
}

// ----------------------------------------------------------------------------
// [50.5] Контроллер клавиатуры 8042 (порты 0x60/0x64, IRQ1) и клавиатура.
// ----------------------------------------------------------------------------
// Хост отдаёт нам скан-коды набора 1 — ровно то, что выдаёт настоящий 8042 с
// включённым «переводом» (бит 6 командного байта): клавиатура шлёт набор 2,
// контроллер переводит в набор 1. BIOS оставляет перевод включённым, Linux
// его не трогает и ждёт набор 1 — поэтому коды идут гостю как есть. Гость,
// выключивший перевод, получил бы не тот набор: этого режима у нас нет.
// Мыши (второго порта) нет: её проверки отвечают «устройства нет» сразу,
// без таймаутов.
//
// Выходной буфер один, как у микросхемы. В него попадает, по старшинству:
// ответ контроллера на команду (без прерывания — гость ждёт его опросом),
// ответ клавиатуры на команду, очередной скан-код. Чтение порта 0x60
// освобождает буфер, и следующий байт даёт НОВЫЙ фронт на линии IRQ1.
static void kbd_refill(struct pc* p) {
    if (!p->kb_obf) {
        if (p->kb_rq_n) {
            p->kb_out = p->kb_rq[p->kb_rq_r];
            p->kb_rq_r = (uint8_t)((p->kb_rq_r + 1) % KB_RESP);
            p->kb_rq_n--;
            p->kb_out_kbd = true;
            p->kb_obf = true;
        } else if (p->kb_scan && !(p->kb_ctr & 0x10) &&
                   p->kq_r != __atomic_load_n(&p->kq_w, __ATOMIC_ACQUIRE)) {
            p->kb_out = p->kq[p->kq_r & (KB_RING - 1)];
            __atomic_store_n(&p->kq_r, p->kq_r + 1, __ATOMIC_RELEASE);
            p->kb_out_kbd = true;
            p->kb_obf = true;
            p->st.kbd++;
        }
    }
    bool lvl = p->kb_obf && p->kb_out_kbd && (p->kb_ctr & 1);
    if (lvl && !((p->line >> 1) & 1)) p->st.irq1++;
    pic_line(p, 1, lvl);
}

// Ответ самого контроллера: занимает выходной буфер сразу, вытесняя то, что
// там лежало (так ведёт себя микросхема; Linux перед командой буфер чистит).
static void kbd_ctl_resp(struct pc* p, uint8_t v) {
    p->kb_out = v;
    p->kb_out_kbd = false;
    p->kb_obf = true;
}

// Ответ клавиатуры — в очередь: их бывает несколько подряд (ACK, два байта
// идентификатора), и каждый гость забирает отдельным прерыванием.
static void kbd_dev_resp(struct pc* p, uint8_t v) {
    if (p->kb_rq_n >= KB_RESP) return;
    p->kb_rq[(p->kb_rq_r + p->kb_rq_n) % KB_RESP] = v;
    p->kb_rq_n++;
}

static uint8_t kbd_read_data(struct pc* p) {
    uint8_t v = p->kb_out;                              // пустой буфер отдаёт прежний байт
    p->kb_obf = false;
    pic_line(p, 1, false);                              // линия падает — следующий байт даст фронт
    kbd_refill(p);
    return v;
}

static void kbd_write_cmd(struct pc* p, uint8_t v) {
    p->kb_last_cmd = true;
    p->kb_ctl_cmd = 0;
    switch (v) {
    case 0x20: kbd_ctl_resp(p, p->kb_ctr); break;       // прочитать командный байт
    case 0x60: case 0xD1: case 0xD2: case 0xD3: case 0xD4:
        p->kb_ctl_cmd = v;                              // дальше придёт байт данных
        break;
    case 0xA7: p->kb_ctr |= 0x20; break;                // порт мыши выключить / включить:
    case 0xA8: p->kb_ctr &= (uint8_t)~0x20; break;      // бит храним, порта за ним нет
    case 0xA9: kbd_ctl_resp(p, 0x01); break;            // проверка линии мыши: «линия не отвечает»
    case 0xAA: kbd_ctl_resp(p, 0x55); break;            // самопроверка контроллера: исправен
    case 0xAB: kbd_ctl_resp(p, 0x00); break;            // проверка линии клавиатуры: исправна
    case 0xAD: p->kb_ctr |= 0x10; break;                // клавиатуру выключить
    case 0xAE: p->kb_ctr &= (uint8_t)~0x10; break;      // … включить
    case 0xD0: kbd_ctl_resp(p, 0xDF); break;            // выходной порт: A20 включена, сброса нет
    case 0xFE: p->st.reset = 1; break;                  // импульс на линию сброса процессора
    default: break;
    }
    kbd_refill(p);
}

static void kbd_write_data(struct pc* p, uint8_t v) {
    p->kb_last_cmd = false;
    if (p->kb_ctl_cmd) {                                // байт данных к команде контроллера
        uint8_t c = p->kb_ctl_cmd;
        p->kb_ctl_cmd = 0;
        if (c == 0x60) p->kb_ctr = v;
        else if (c == 0xD1) { if (!(v & 1)) p->st.reset = 1; }     // бит 0 выходного порта — сброс
        else if (c == 0xD2) kbd_dev_resp(p, v);                    // «как будто от клавиатуры»
        // 0xD3 — «петля» порта мыши. Порта нет: байт возвращается обычными
        // данными, без признака «от мыши» в порту состояния — по этому Linux
        // сразу понимает, что мыши нет, и не ждёт её полсекунды.
        else if (c == 0xD3) kbd_ctl_resp(p, v);
        kbd_refill(p);                                  // 0xD4 (байт для мыши) — в никуда
        return;
    }
    if (p->kb_dev_cmd) {                                // параметр команды клавиатуры
        uint8_t c = p->kb_dev_cmd;
        p->kb_dev_cmd = 0;
        kbd_dev_resp(p, 0xFA);
        // F0 с параметром 0 — «какой набор кодов сейчас»: второй, а через
        // перевод контроллера он читается как 0x41.
        if (c == 0xF0 && v == 0) kbd_dev_resp(p, (p->kb_ctr & 0x40) ? 0x41 : 0x02);
        kbd_refill(p);
        return;
    }
    switch (v) {                                        // команда клавиатуре
    case 0xFF:                                          // сброс: ACK и «самопроверка пройдена»
        p->kb_scan = true;
        kbd_dev_resp(p, 0xFA); kbd_dev_resp(p, 0xAA);
        break;
    case 0xF2:                                          // идентификатор: клавиатура AT/PS2
        kbd_dev_resp(p, 0xFA); kbd_dev_resp(p, 0xAB);
        kbd_dev_resp(p, (p->kb_ctr & 0x40) ? 0x41 : 0x83);
        break;
    case 0xED: case 0xF0: case 0xF3:                    // индикаторы, набор кодов, автоповтор:
        p->kb_dev_cmd = v;                              // ACK и ждём параметр
        kbd_dev_resp(p, 0xFA);
        break;
    case 0xEE: kbd_dev_resp(p, 0xEE); break;            // эхо
    case 0xF4: p->kb_scan = true;  kbd_dev_resp(p, 0xFA); break;
    case 0xF5: p->kb_scan = false; kbd_dev_resp(p, 0xFA); break;
    default:   kbd_dev_resp(p, 0xFA); break;            // остальное принимаем молча
    }
    kbd_refill(p);
}

// ----------------------------------------------------------------------------
// Часы CMOS: время на стене при создании + прошедшее по TSC.
// ----------------------------------------------------------------------------
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

uint64_t pc_unix_time(int y, int m, int d, int hh, int mm, int ss) {
    // Дни от 1970-01-01 (алгоритм «дни из гражданской даты»).
    y -= m <= 2;
    int era = y / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return (uint64_t)days * 86400 + (uint64_t)hh * 3600 + (uint64_t)mm * 60 + (uint64_t)ss;
}

static uint8_t cmos_read(struct pc* p, uint64_t now) {
    uint64_t t = p->unix0 + (now - p->t0) / p->tsc_hz;
    int ss = (int)(t % 60), mm = (int)(t / 60 % 60), hh = (int)(t / 3600 % 24);
    int64_t z = (int64_t)(t / 86400) + 719468;
    int era = (int)(z / 146097);
    int doe = (int)(z - (int64_t)era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    int d = doy - (153 * mp + 2) / 5 + 1;
    int m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
    switch (p->cmos_idx & 0x7F) {
        case 0x00: return bcd(ss);
        case 0x02: return bcd(mm);
        case 0x04: return bcd(hh);
        case 0x06: return (uint8_t)((t / 86400 + 4) % 7 + 1);       // 1970-01-01 — четверг
        case 0x07: return bcd(d);
        case 0x08: return bcd(m);
        case 0x09: return bcd(y % 100);
        case 0x32: return bcd(y / 100);
        case 0x0A: return 0x26;                                     // обновление не идёт
        case 0x0B: return 0x02;                                     // 24 часа, двоично-десятичный
        case 0x0D: return 0x80;                                     // батарея в порядке
        default:   return 0;
    }
}

// ----------------------------------------------------------------------------
// [50.4] Шина PCI: ровно столько, чтобы ядро гостя нашло один диск.
// Устройства: 00:00.0 — мост «процессор-PCI» (без него Linux не верит, что
// шина есть), 00:01.0 — диск virtio-blk. Доступ — «механизм 1»: адрес в порт
// 0xCF8, данные через 0xCFC..0xCFF.
//
// Диск — virtio «старого вида» (legacy): его регистры лежат в портах
// ввода-вывода (BAR0), а не в памяти — эмуляции обращений к памяти устройств
// (MMIO) у нас нет. Очередь запросов живёт в памяти гостя; гость кладёт запрос
// и пишет в порт «проверь очередь», мы исполняем его тут же, на ядре гостя:
// это копирование из образа ISO в памяти хоста, диск хоста не трогается.
// ----------------------------------------------------------------------------
#define VQ_SIZE        128
#define VBLK_F_SEG_MAX (1u << 2)
#define VBLK_F_RO      (1u << 5)

static uint16_t g16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t g32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t g64(const uint8_t* p) { return g32(p) | ((uint64_t)g32(p + 4) << 32); }
static void p16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void p32(uint8_t* p, uint32_t v) { p16(p, (uint16_t)v); p16(p + 2, (uint16_t)(v >> 16)); }
static void p64(uint8_t* p, uint64_t v) { p32(p, (uint32_t)v); p32(p + 4, (uint32_t)(v >> 32)); }

static void vblk_irq(struct pc* p) {
    // Бит 10 регистра команд — «прерывания запрещены».
    pic_line(p, p->vb_irq & 15, p->vb_isr && !(p->vb_cmd & 0x400));
}

static void vblk_reset(struct pc* p) {
    p->vb_feat = 0; p->vb_pfn = 0; p->vb_sel = 0;
    p->vb_status = 0; p->vb_isr = 0;
    p->vb_last_avail = 0; p->vb_used_idx = 0;
    vblk_irq(p);
}

// Исполнить один запрос — цепочку описателей с головой head. Возвращает,
// сколько байт записано в память гостя (это уходит в кольцо «использовано»).
static uint32_t vblk_request(struct pc* p, const uint8_t* desc, uint16_t head) {
    // Цепочка: заголовок (тип, сектор) -> буферы данных -> байт состояния.
    struct { uint64_t addr; uint32_t len; uint16_t flags; } d[VQ_SIZE];
    int n = 0;
    uint16_t i = head;
    for (;;) {
        if (i >= VQ_SIZE || n >= VQ_SIZE) return 0;             // испорченная цепочка
        const uint8_t* e = desc + (uint32_t)i * 16;
        d[n].addr = g64(e); d[n].len = g32(e + 8); d[n].flags = g16(e + 12);
        n++;
        if (!(d[n - 1].flags & 1)) break;                        // флаг «есть следующий»
        i = g16(e + 14);
    }
    if (n < 2 || d[0].len < 16) return 0;
    const uint8_t* hdr = (const uint8_t*)hv_gpa(p->vm, d[0].addr, 16);
    uint8_t* status = (uint8_t*)hv_gpa(p->vm, d[n - 1].addr, 1);
    if (!hdr || !status) return 0;
    uint32_t type = g32(hdr);
    uint64_t off = g64(hdr + 8) * 512;
    uint32_t written = 0;
    uint8_t st = 0;                                              // 0 — успех

    p->st.blk_reqs++;
    if (type == 0) {                                             // чтение
        for (int k = 1; k < n - 1; k++) {
            uint8_t* dst = (uint8_t*)hv_gpa(p->vm, d[k].addr, d[k].len);
            if (!dst || off + d[k].len > p->disk_bytes) { st = 1; break; }      // ошибка ввода-вывода
            const uint8_t* src = p->disk + off;
            for (uint32_t b = 0; b < d[k].len; b++) dst[b] = src[b];
            off += d[k].len;
            written += d[k].len;
            p->st.blk_sectors += d[k].len / 512;
        }
    } else if (type == 8) {                                      // «назови себя»
        static const char id[20] = "phantom-iso";
        uint8_t* dst = n >= 3 ? (uint8_t*)hv_gpa(p->vm, d[1].addr, 20) : 0;
        if (dst && d[1].len >= 20) { for (int b = 0; b < 20; b++) dst[b] = (uint8_t)id[b]; written = 20; }
        else st = 1;
    } else if (type == 4) {                                      // сброс кэша: нечего сбрасывать
        st = 0;
    } else if (type == 1) {                                      // запись: диск только для чтения
        st = 1;
    } else {
        st = 2;                                                  // не поддерживается
    }
    if (st) p->st.blk_errors++;
    *status = st;
    return written + 1;
}

// Гость написал «проверь очередь».
static void vblk_notify(struct pc* p) {
    if (!p->vb_pfn) return;
    // Раскладка старой очереди: описатели, кольцо «доступно», а с границы
    // следующей страницы — кольцо «использовано».
    uint64_t base = (uint64_t)p->vb_pfn << 12;
    uint32_t avail_off = VQ_SIZE * 16;
    uint32_t used_off = (avail_off + 4 + VQ_SIZE * 2 + 2 + 4095) & ~4095u;
    uint8_t* ring = (uint8_t*)hv_gpa(p->vm, base, used_off + 4 + VQ_SIZE * 8 + 2);
    if (!ring) return;
    uint8_t* avail = ring + avail_off;
    uint8_t* used = ring + used_off;
    uint16_t avail_idx = g16(avail + 2);
    bool did = false;
    while (p->vb_last_avail != avail_idx) {
        uint16_t head = g16(avail + 4 + (p->vb_last_avail % VQ_SIZE) * 2);
        uint32_t len = vblk_request(p, ring, head);
        uint8_t* ue = used + 4 + (p->vb_used_idx % VQ_SIZE) * 8;
        p32(ue, head);
        p32(ue + 4, len);
        p->vb_used_idx++;
        p16(used + 2, p->vb_used_idx);          // индекс — после записи элемента
        p->vb_last_avail++;
        did = true;
    }
    // Бит 0 флагов кольца «доступно» — «прерывание не нужно».
    if (did && !(g16(avail) & 1)) { p->vb_isr |= 1; vblk_irq(p); }
}

static void vblk_io(struct pc* p, hv_exit_t* ex, uint32_t off) {
    if (ex->in) {
        // Регистры собираются в один массив: гость читает их и по байту
        // (вместимость диска), и словами.
        uint8_t r[0x40];
        for (int i = 0; i < 0x40; i++) r[i] = 0;
        p32(r + 0x00, VBLK_F_SEG_MAX | VBLK_F_RO);
        p32(r + 0x04, p->vb_feat);
        p32(r + 0x08, p->vb_sel == 0 ? p->vb_pfn : 0);
        p16(r + 0x0C, p->vb_sel == 0 ? VQ_SIZE : 0);    // у диска одна очередь
        p16(r + 0x0E, p->vb_sel);
        r[0x12] = p->vb_status;
        r[0x13] = p->vb_isr;
        p64(r + 0x14, p->disk_bytes / 512);             // вместимость в секторах по 512
        p32(r + 0x14 + 12, VQ_SIZE - 2);                // буферов данных в одном запросе
        p32(r + 0x14 + 20, 512);
        uint32_t v = 0;
        for (int i = 0; i < ex->size && off + (uint32_t)i < 0x40; i++) v |= (uint32_t)r[off + i] << (i * 8);
        ex->value = v;
        // Чтение причины прерывания её снимает — и запрос с линии тоже.
        if (off <= 0x13 && off + ex->size > 0x13) { p->vb_isr = 0; vblk_irq(p); }
        return;
    }
    uint32_t v = ex->value;
    switch (off) {
    case 0x04: p->vb_feat = v; break;
    case 0x08:
        if (p->vb_sel == 0) { p->vb_pfn = v; p->vb_last_avail = 0; p->vb_used_idx = 0; }
        break;
    case 0x0E: p->vb_sel = (uint16_t)v; break;
    case 0x10: if ((uint16_t)v == 0) vblk_notify(p); break;
    case 0x12:
        p->vb_status = (uint8_t)v;
        if (!p->vb_status) vblk_reset(p);                // запись нуля — сброс устройства
        break;
    default: break;
    }
}

// Одно 32-битное слово пространства конфигурации устройства dev.
static uint32_t pci_cfg(struct pc* p, int dev, int reg) {
    if (dev == 0) {
        if (reg == 0x00) return 0x12378086u;            // мост i440FX
        if (reg == 0x08) return 0x06000002u;            // класс 06.00 — мост «хост-PCI»
        return 0;
    }
    if (dev == 1) {
        switch (reg) {
        case 0x00: return 0x10011AF4u;                  // virtio, блочное устройство (старый вид)
        case 0x04: return p->vb_cmd;                    // состояние 0: списка возможностей нет
        case 0x08: return 0x01000000u;                  // класс 01 — накопитель; ревизия 0
        case 0x10: return p->vb_bar;
        case 0x2C: return 0x00021AF4u;                  // подсистема 2 = диск
        case 0x3C: return 0x00000100u | p->vb_irq;      // вывод INTA и линия прерывания
        default:   return 0;
        }
    }
    return 0xFFFFFFFFu;                                  // пустое место на шине
}

static void pci_data(struct pc* p, hv_exit_t* ex) {
    uint32_t a = p->cf8;
    int bus = (a >> 16) & 0xFF, dev = (a >> 11) & 31, fn = (a >> 8) & 7, reg = a & 0xFC;
    int sh = (ex->port & 3) * 8;
    uint32_t mask = ex->size == 4 ? 0xFFFFFFFFu : ((1u << (ex->size * 8)) - 1);
    if (!(a & 0x80000000u) || bus || fn || dev > 1) return;     // чтение уже содержит единицы
    if (ex->in) {
        ex->value = (pci_cfg(p, dev, reg) >> sh) & mask;
        return;
    }
    if (dev != 1) return;                                // у моста писать нечего
    uint32_t nv = (pci_cfg(p, dev, reg) & ~(mask << sh)) | ((ex->value & mask) << sh);
    if (reg == 0x04) p->vb_cmd = (uint16_t)(nv & 0x0407);
    // BAR0: 64 порта. Запись всех единиц и чтение обратно — так гость узнаёт размер.
    else if (reg == 0x10) p->vb_bar = (nv & 0xFFFFFFC0u) | 1;
    else if (reg == 0x3C) p->vb_irq = (uint8_t)nv;
}

// ----------------------------------------------------------------------------
// Порты.
// ----------------------------------------------------------------------------
static void pc_io(struct pc* p, hv_exit_t* ex) {
    uint16_t port = ex->port;
    uint8_t v = (uint8_t)ex->value;

    if (port >= 0x3F8 && port <= 0x3FF) {
        if (ex->in) ex->value = uart_read(p, port - 0x3F8);
        else uart_write(p, port - 0x3F8, v);
        return;
    }
    if (p->disk) {
        // Регистры диска — там, куда указывает BAR0 (гость вправе его передвинуть).
        uint32_t base = p->vb_bar & 0xFFFCu;
        if ((p->vb_cmd & 1) && port >= base && port < base + 0x40) { vblk_io(p, ex, port - base); return; }
        if (port == 0xCF8) {
            // Регистр адреса существует только как целое 32-битное слово.
            if (ex->size == 4) { if (ex->in) ex->value = p->cf8; else p->cf8 = ex->value; }
            return;
        }
        if (port >= 0xCFC && port <= 0xCFF) { pci_data(p, ex); return; }
    }
    switch (port) {
    case 0x20: case 0x21:
        if (ex->in) ex->value = pic_read(p, 0, port & 1); else pic_write(p, 0, port & 1, v);
        return;
    case 0xA0: case 0xA1:
        if (ex->in) ex->value = pic_read(p, 1, port & 1); else pic_write(p, 1, port & 1, v);
        return;
    case 0x40: case 0x41: case 0x42: case 0x43:
        if (ex->in) ex->value = pit_read(p, port - 0x40, rdtsc()); else pit_write(p, port - 0x40, v, rdtsc());
        return;
    case 0x61:
        if (ex->in) {
            p->refresh ^= 0x10;                         // «тикает» бит регенерации памяти
            ex->value = (uint8_t)((p->port61 & 3) | p->refresh | (pit_out2(p, rdtsc()) ? 0x20 : 0));
        } else {
            p->port61 = v & 0x0F;
        }
        return;
    case 0x70:
        if (!ex->in) p->cmos_idx = v;
        return;
    case 0x71:
        if (ex->in) ex->value = cmos_read(p, rdtsc());
        return;
    case 0x92:
        if (ex->in) ex->value = p->port92 | 2;          // A20 включён всегда
        else { p->port92 = v & ~1u; if (v & 1) p->st.reset = 1; }
        return;
    case 0x60:                                          // [50.5] 8042: данные
        if (ex->in) ex->value = kbd_read_data(p); else kbd_write_data(p, v);
        return;
    case 0x64:                                          // … состояние и команды
        // Состояние: бит 0 — есть байт для чтения; бит 2 — «система прошла
        // самопроверку»; бит 3 — последняя запись была командой; бит 4 —
        // замок клавиатуры открыт. «Занят» (бит 1) и «от мыши» (бит 5) — нет.
        // Команда 0xFE (сброс процессора) — так перезагружается Linux.
        if (ex->in) ex->value = (uint8_t)(0x14 | (p->kb_obf ? 1 : 0) | (p->kb_last_cmd ? 8 : 0));
        else kbd_write_cmd(p, v);
        return;
    case 0xCF9:
        if (!ex->in && (v & 4)) p->st.reset = 1;
        return;
    case 0x4D0: case 0x4D1:
        // Режим линий «фронт/уровень». Храним, но линии у нас всегда работают
        // по фронту: диск снимает запрос, как только драйвер прочёл причину,
        // так что новый запрос — всегда новый фронт.
        if (ex->in) ex->value = p->elcr[port & 1];
        else p->elcr[port & 1] = v;
        return;
    case 0x80: case 0xED:                               // «порты задержки»
        return;
    case 0x3D4:                                         // видеоконтроллер: номер регистра
        if (ex->in) { ex->value = p->crtc_idx; return; }
        p->crtc_idx = v & 31;
        // Ядро Linux пишет сюда СЛОВОМ: младший байт — номер регистра (порт
        // 3D4), старший — значение (порт 3D5). Запись слова в порт — это два
        // соседних байтовых порта; приняв только младший байт, мы теряли все
        // значения, и начало экрана навсегда оставалось нулём.
        if (ex->size >= 2) p->crtc[p->crtc_idx] = (uint8_t)(ex->value >> 8);
        return;
    case 0x3D5:                                         // … и его значение
        if (ex->in) ex->value = p->crtc[p->crtc_idx]; else p->crtc[p->crtc_idx] = v;
        return;
    default:
        p->st.unknown_io++;
        p->st.last_unknown_port = port;
        return;                                         // чтение уже содержит единицы
    }
}

// ----------------------------------------------------------------------------
// Модель для hv (исполняется на AP гостя).
// ----------------------------------------------------------------------------
static int pc_exit(hv_vm_t* vm, hv_exit_t* ex, void* ctx) {
    struct pc* p = (struct pc*)ctx;
    p->st.end_reason = ex->reason;
    if (ex->reason == HV_EXIT_IO) {
        pc_io(p, ex);
        p->st.end_reason = 0;
        return p->st.reset ? HV_STOP : HV_RESUME;
    }
    if (ex->reason == HV_EXIT_VMCALL && p->vmcall) {
        int r = p->vmcall(vm, p->vmcall_ctx);
        if (r == HV_RESUME) p->st.end_reason = 0;
        return r;
    }
    return HV_STOP;                     // HLT с запретом прерываний, сбой, отказ
}

static bool pc_irq_pending(hv_vm_t* vm, void* ctx, uint64_t now) {
    (void)vm;
    struct pc* p = (struct pc*)ctx;
    if (p->irq0_next && now >= p->irq0_next) {
        pic_pulse(p, 0);
        p->st.irq0++;
        if (p->irq0_period) {
            p->irq0_next += p->irq0_period;
            // Отстали больше чем на период (гостя не было на ядре): пропущенные
            // тики не копим — время гость считает по своим часам, не по тикам.
            if (now >= p->irq0_next) p->irq0_next = now + p->irq0_period;
        } else {
            p->irq0_next = 0;
        }
    }
    uart_irq(p);                        // ввод кладёт BSP — замечаем его здесь
    kbd_refill(p);                      // … и скан-коды клавиатуры тоже
    return pic_master_out(p) >= 0;
}

static int pc_irq_ack(hv_vm_t* vm, void* ctx) {
    (void)vm;
    return pic_ack((struct pc*)ctx);
}

static uint64_t pc_deadline(hv_vm_t* vm, void* ctx) {
    (void)vm;
    return ((struct pc*)ctx)->irq0_next;
}

static const hv_model_t g_model = { pc_exit, pc_irq_pending, pc_irq_ack, pc_deadline };

const hv_model_t* pc_model(void) { return &g_model; }

// ----------------------------------------------------------------------------
// BSP.
// ----------------------------------------------------------------------------
pc_t* pc_create(hv_vm_t* vm, uint64_t unix_now) {
    if (g_pc_used) return 0;
    struct pc* p = &g_pc;
    uint8_t* z = (uint8_t*)p;
    for (size_t i = 0; i < sizeof(*p); i++) z[i] = 0;
    p->vm = vm;
    p->tsc_hz = hv_tsc_per_ms() * 1000;
    if (!p->tsc_hz) p->tsc_hz = 2000000000ull;
    p->t0 = rdtsc();
    p->unix0 = unix_now;
    // Как после сброса: всё замаскировано, пока гость сам не настроит
    // контроллеры; базы векторов — те, что ставит BIOS.
    p->pic[0].imr = 0xFF; p->pic[0].base = 0x08;
    p->pic[1].imr = 0xFF; p->pic[1].base = 0x70;
    p->thre = false;
    // [50.5] 8042 в том виде, в каком его оставляет BIOS: перевод набора 2 в
    // набор 1 включён, прерывание клавиатуры разрешено, порт мыши выключен,
    // клавиатура шлёт коды.
    p->kb_ctr = 0x65;
    p->kb_scan = true;
    g_pc_used = true;
    return p;
}

void pc_destroy(pc_t* pc) {
    if (pc == &g_pc) g_pc_used = false;
}

const pc_stats_t* pc_stats(pc_t* pc) { return &pc->st; }

int pc_console_read(pc_t* pc, char* buf, int max) {
    int n = 0;
    uint32_t r = pc->out_r, w = __atomic_load_n(&pc->out_w, __ATOMIC_ACQUIRE);
    while (r != w && n < max) buf[n++] = pc->out[r++ & (OUT_RING - 1)];
    __atomic_store_n(&pc->out_r, r, __ATOMIC_RELEASE);
    return n;
}

bool pc_console_input(pc_t* pc, const char* s, int n) {
    uint32_t w = pc->in_w;
    if (IN_RING - (w - __atomic_load_n(&pc->in_r, __ATOMIC_ACQUIRE)) < (uint32_t)n) return false;
    for (int i = 0; i < n; i++) pc->in[(w + (uint32_t)i) & (IN_RING - 1)] = s[i];
    __atomic_store_n(&pc->in_w, w + (uint32_t)n, __ATOMIC_RELEASE);
    hv_vcpu_kick(pc->vm);               // гость мог спать по HLT
    return true;
}

// ----------------------------------------------------------------------------
// [50.3] Для BIOS гостя.
// ----------------------------------------------------------------------------
void pc_bios_defaults(pc_t* pc) {
    uint64_t now = rdtsc();
    pc->pic[0].imr = 0xF8;                  // открыты таймер (IRQ0), клавиатура (IRQ1), каскад (IRQ2)
    pc->pic[1].imr = 0xFF;
    pit_ch_t* c = &pc->ch[0];
    c->mode = 3; c->rw = 3;
    c->reload = 0x10000;                    // 1193182 / 65536 = 18.2 Гц — «тик BIOS»
    c->start = now;
    c->armed = true;
    pc->irq0_period = pit_to_tsc(pc, c->reload);
    pc->irq0_next = now + pc->irq0_period;
}

void pc_set_vmcall(pc_t* pc, pc_vmcall_fn fn, void* ctx) {
    pc->vmcall = fn;
    pc->vmcall_ctx = ctx;
}

void pc_con_putc(pc_t* pc, char ch) {
    // Загрузчик с директивой SERIAL пишет каждый символ дважды — в порт и на
    // экран BIOS. Консоль у гостя одна: раз он заговорил в порт, поток с
    // экрана в неё больше не идёт (сам экран остаётся, его видно `phantom
    // screen`), иначе текст шёл бы вперемешку с собственной копией.
    if (pc->uart_tx_seen) return;
    if (pc->con_src == 2) con_put(pc, '\n');
    pc->con_src = 1;
    con_put(pc, ch);
}

int pc_con_peek(pc_t* pc) {
    if (pc->in_r == __atomic_load_n(&pc->in_w, __ATOMIC_ACQUIRE)) return -1;
    return (uint8_t)pc->in[pc->in_r & (IN_RING - 1)];
}

int pc_con_getc(pc_t* pc) {
    int ch = pc_con_peek(pc);
    if (ch < 0) return -1;
    __atomic_store_n(&pc->in_r, pc->in_r + 1, __ATOMIC_RELEASE);
    pc->st.rx++;
    return ch;
}

bool pc_irq_in_service(pc_t* pc, int irq) {
    return irq < 8 ? (pc->pic[0].isr >> irq) & 1 : (pc->pic[1].isr >> (irq - 8)) & 1;
}

void pc_irq_eoi(pc_t* pc, int irq) {
    if (irq < 8) pc->pic[0].isr &= (uint8_t)~(1u << irq);
    else { pc->pic[1].isr &= (uint8_t)~(1u << (irq - 8)); pc->pic[0].isr &= (uint8_t)~4u; }
}

uint64_t pc_unix_now(pc_t* pc) {
    return pc->unix0 + (rdtsc() - pc->t0) / pc->tsc_hz;
}

// ----------------------------------------------------------------------------
// [50.4] Диск гостя (BSP).
// ----------------------------------------------------------------------------
void pc_attach_disk(pc_t* pc, const uint8_t* data, uint64_t bytes) {
    pc->disk = data;
    pc->disk_bytes = bytes;
    // То, что на настоящей машине делает BIOS: дать устройству адрес и линию
    // прерывания и включить его. Ядро гостя это уважает.
    pc->vb_cmd = 0x0001;                        // отвечать на порты ввода-вывода
    pc->vb_bar = 0xC000 | 1;
    pc->vb_irq = 11;
    pc->cf8 = 0;
    vblk_reset(pc);
}

uint32_t pc_vga_origin(pc_t* pc) { return ((uint32_t)pc->crtc[0x0C] << 8) | pc->crtc[0x0D]; }
uint32_t pc_vga_cursor(pc_t* pc) { return ((uint32_t)pc->crtc[0x0E] << 8) | pc->crtc[0x0F]; }
// Регистр 0Ah, бит 5 — «курсор выключен» (так его прячет Linux и загрузчики).
bool     pc_vga_cursor_on(pc_t* pc) { return !(pc->crtc[0x0A] & 0x20); }

void pc_vga_set_cursor(pc_t* pc, uint32_t pos) {
    pos += pc_vga_origin(pc);
    pc->crtc[0x0E] = (uint8_t)(pos >> 8);
    pc->crtc[0x0F] = (uint8_t)pos;
}

// ----------------------------------------------------------------------------
// [50.5] Клавиатура гостя.
// ----------------------------------------------------------------------------
// BSP (в том числе из обработчика прерывания клавиатуры хоста). Писатель у
// кольца один — ядро BSP; главный поток обязан звать это с IF=0, чтобы его не
// прервал тот же обработчик посреди записи.
bool pc_kbd_scancode(pc_t* pc, uint8_t sc) {
    uint32_t w = pc->kq_w;
    if (w - __atomic_load_n(&pc->kq_r, __ATOMIC_ACQUIRE) >= KB_RING) return false;
    pc->kq[w & (KB_RING - 1)] = sc;
    __atomic_store_n(&pc->kq_w, w + 1, __ATOMIC_RELEASE);
    hv_vcpu_kick(pc->vm);               // гость мог спать по HLT
    return true;
}

// AP гостя, служба BIOS INT 09h: прочитать байт клавиатуры, как это сделал бы
// обработчик настоящего BIOS чтением порта 0x60. -1 — буфер пуст.
int pc_kbd_take(pc_t* pc) {
    if (!pc->kb_obf || !pc->kb_out_kbd) return -1;
    return kbd_read_data(pc);
}

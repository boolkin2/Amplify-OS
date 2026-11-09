// ============================================================================
// idt.c — построение и установка IDT64.
// ============================================================================
#include "idt.h"
#include "gdt.h"

__attribute__((aligned(16))) static idt_entry_t idt[256];
static idt_ptr_t idt_ptr;

extern void idt64_load(uint64_t idt_ptr_addr);

// Стабы из isr.asm — 32 exception + 16 IRQ + 1 syscall (0x80).
// Объявляем как extern void, реальная адресация в asm.
#define DECL(n) extern void isr##n(void)
DECL(0);  DECL(1);  DECL(2);  DECL(3);  DECL(4);  DECL(5);  DECL(6);  DECL(7);
DECL(8);  DECL(9);  DECL(10); DECL(11); DECL(12); DECL(13); DECL(14); DECL(15);
DECL(16); DECL(17); DECL(18); DECL(19); DECL(20); DECL(21); DECL(22); DECL(23);
DECL(24); DECL(25); DECL(26); DECL(27); DECL(28); DECL(29); DECL(30); DECL(31);
DECL(32); DECL(33); DECL(34); DECL(35); DECL(36); DECL(37); DECL(38); DECL(39);
DECL(40); DECL(41); DECL(42); DECL(43); DECL(44); DECL(45); DECL(46); DECL(47);
DECL(80); DECL(81); DECL(82); DECL(83); DECL(84); DECL(85); DECL(86); DECL(87);
DECL(88); DECL(89); DECL(90); DECL(91); DECL(92); DECL(93); DECL(94); DECL(95);
extern void isr128(void);
extern void isr239(void);
extern void isr240(void);
extern void isr254(void);
extern void isr255(void);
#undef DECL

static void* isr_table[] = {
    isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7,
    isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15,
    isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23,
    isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31,
    isr32, isr33, isr34, isr35, isr36, isr37, isr38, isr39,
    isr40, isr41, isr42, isr43, isr44, isr45, isr46, isr47,
};

// Таблица C-обработчиков: одна запись на вектор.
static int_handler_t handlers[256];

static void set_gate(uint8_t vec, void* handler, uint16_t sel, uint8_t type_attr, uint8_t ist) {
    uint64_t addr = (uint64_t)handler;
    idt[vec].offset_low  = addr & 0xFFFF;
    idt[vec].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vec].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vec].selector    = sel;
    idt[vec].ist         = ist;
    idt[vec].type_attr   = type_attr;
    idt[vec].reserved    = 0;
}

void idt64_init(void) {
    // Все 256 — заглушка по умолчанию (gate type 0 = not present).
    for (int i = 0; i < 256; i++) {
        idt[i].offset_low = idt[i].offset_mid = idt[i].selector = 0;
        idt[i].offset_high = idt[i].reserved = 0;
        idt[i].ist = idt[i].type_attr = 0;
    }
    for (int i = 0; i < 256; i++) handlers[i] = 0;

    // Exception 0..31 — interrupt gate, DPL=0.
    //   type_attr = 1000_1110 = 0x8E (P=1, DPL=0, type=E (interrupt gate))
    for (int i = 0; i < 32; i++) {
        set_gate(i, isr_table[i], GDT_SEL_KERNEL_CODE, 0x8E, 0);
    }
    // IRQ 32..47 (legacy PIC после ремапа) — interrupt gate.
    for (int i = 32; i < 48; i++) {
        set_gate(i, isr_table[i], GDT_SEL_KERNEL_CODE, 0x8E, 0);
    }
    // [41.2] 80..95 — динамические векторы MSI-X (irq_alloc_vector).
    {
        static void* const msi_stubs[IRQ_DYN_COUNT] = {
            isr80, isr81, isr82, isr83, isr84, isr85, isr86, isr87,
            isr88, isr89, isr90, isr91, isr92, isr93, isr94, isr95,
        };
        for (int i = 0; i < IRQ_DYN_COUNT; i++)
            set_gate((uint8_t)(IRQ_DYN_BASE + i), msi_stubs[i], GDT_SEL_KERNEL_CODE, 0x8E, 0);
    }
    // Системный вызов 0x80 — interrupt gate с DPL=3, чтобы его мог вызывать
    // user mode через `int 0x80`.
    //   type_attr = 1110_1110 = 0xEE
    set_gate(0x80, isr128, GDT_SEL_KERNEL_CODE, 0xEE, 0);

    // LAPIC vectors (этап 37.3).
    set_gate(0xEF, isr239, GDT_SEL_KERNEL_CODE, 0x8E, 0);     // timer
    set_gate(0xF0, isr240, GDT_SEL_KERNEL_CODE, 0x8E, 0);     // [49] wake-IPI пула AP
    set_gate(0xFE, isr254, GDT_SEL_KERNEL_CODE, 0x8E, 0);     // error
    set_gate(0xFF, isr255, GDT_SEL_KERNEL_CODE, 0x8E, 0);     // spurious

    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)idt;

    idt64_load((uint64_t)&idt_ptr);
}

void idt64_set_handler(uint8_t vec, int_handler_t h) {
    handlers[vec] = h;
}

// [41.2] Выдать свободный вектор из 80..95 и повесить на него обработчик.
// 0 — векторы кончились (драйвер тогда остаётся на опросе). Вызывать с BSP
// при инициализации драйверов: счётчик без блокировок.
uint8_t irq_alloc_vector(int_handler_t h) {
    static int next = 0;
    if (next >= IRQ_DYN_COUNT) return 0;
    uint8_t vec = (uint8_t)(IRQ_DYN_BASE + next++);
    handlers[vec] = h;
    return vec;
}

// [этап 49] AP стартуют без IDT (trampoline его не грузит). Таблица общая
// для всех ядер: гейты и handlers[] одни и те же, поэтому AP достаточно
// выполнить lidt на уже построенный idt_ptr. Вызывать только после
// idt64_init() на BSP.
void idt64_load_on_this_cpu(void) {
    idt64_load((uint64_t)&idt_ptr);
}

// Эта функция вызывается из isr_common_stub в isr.asm.
// rdi (1-й аргумент SysV) = указатель на registers64_t на стеке.
// [41.2] Счётчик аппаратных прерываний, кроме таймера LAPIC (0xEF). Главный
// цикл WM спит в hlt и просыпается 1000 раз в секунду от таймера; по этому
// счётчику он отличает «просто тик» (сразу спать дальше) от настоящего
// события (клавиатура, мышь, virtio).
static volatile uint64_t g_irq_events = 0;
uint64_t irq_event_count(void) { return g_irq_events; }

void isr_dispatch(registers64_t* r) {
    uint64_t vec = r->int_no & 0xFF;
    if (vec >= 32 && vec != 0xEF) g_irq_events++;
    int_handler_t h = handlers[vec];
    if (h) h(r);
}

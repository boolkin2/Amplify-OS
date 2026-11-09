// ============================================================================
// timer.c — реализация высокоуровневого таймер-адаптера.
// ============================================================================
#include "timer.h"
#include "pit.h"
#include "io.h"
#include "hv.h"
#include "rune.h"

// PIT инициализирован на 100 Hz → 1 тик = 10 мс.
#define TICKS_PER_SECOND   100u
#define MS_PER_TICK        10u

// ----------------------------------------------------------------------------
// Callback-таблица. Обновляется по PIT-тикам.
// ----------------------------------------------------------------------------
typedef struct {
    timer_callback_fn cb;
    uint32_t          period_ticks;     // период в тиках (>=1)
    uint32_t          counter;          // счётчик до следующего вызова
    bool              active;
} timer_slot_t;

static timer_slot_t  g_slots[TIMER_MAX_CALLBACKS];
static volatile bool g_in_callback = false;

void timer_init(void) {
    for (int i = 0; i < TIMER_MAX_CALLBACKS; i++) {
        g_slots[i].cb           = 0;
        g_slots[i].period_ticks = 0;
        g_slots[i].counter      = 0;
        g_slots[i].active       = false;
    }
}

// pit.c вызывает нас на каждом тике (после ticks++).
// Защита от ре-входа: если callback внутри сам дёрнул timer_tick (теоретически
// возможно через nested IRQ), не зацикливаемся.
void timer_tick(void) {
    // [50.2] Если миллисекундного таймера LAPIC нет, гостя будит хотя бы
    // этот тик (100 Гц); когда есть — лишний вызов ничего не стоит.
    hv_tick_1ms();
    rune_tick_1ms();                // [51.1] предел времени программы WebAssembly
    if (g_in_callback) return;
    g_in_callback = true;
    for (int i = 0; i < TIMER_MAX_CALLBACKS; i++) {
        if (!g_slots[i].active) continue;
        if (g_slots[i].counter > 0) g_slots[i].counter--;
        if (g_slots[i].counter == 0) {
            g_slots[i].counter = g_slots[i].period_ticks;
            if (g_slots[i].cb) g_slots[i].cb();
        }
    }
    g_in_callback = false;
}

uint64_t timer_get_ticks(void) {
    return pit_ticks();
}

// [41.2] Миллисекундный счётчик таймера LAPIC (1000 Гц). Пока LAPIC-таймер
// не запущен (ранняя загрузка, неудачная калибровка) — прежнее разрешение
// 10 мс от PIT. Обе шкалы согласованы: g_ms стартует с pit_ticks()*10.
static volatile uint64_t g_ms = 0;
static volatile bool     g_ms_active = false;
static volatile uint8_t  g_ms_sub = 0;         // 0..9 внутри тика 10 мс

void timer_ms_source_start(void) {
    g_ms = pit_ticks() * (uint64_t)MS_PER_TICK;
    g_ms_sub = 0;
    g_ms_active = true;
}

// Зовётся из обработчика таймера LAPIC раз в миллисекунду.
void timer_ms_tick(void) {
    g_ms++;
    // [50.2] Гость phantom, уснувший по HLT, ждёт своего таймера: будит его
    // этот тик. Без гостей — одно чтение переменной.
    hv_tick_1ms();
    rune_tick_1ms();                // [51.1] предел времени программы WebAssembly
    if (++g_ms_sub >= MS_PER_TICK) {
        g_ms_sub = 0;
        pit_tick_from_lapic();          // тик 100 Гц + callback'и timer_tick
    }
}

bool timer_ms_active(void) { return g_ms_active; }

uint64_t timer_get_uptime_ms(void) {
    if (g_ms_active) return g_ms;
    return pit_ticks() * (uint64_t)MS_PER_TICK;
}

void timer_sleep(uint32_t ms) {
    uint64_t target = timer_get_uptime_ms() + ms;
    while (timer_get_uptime_ms() < target) {
        __asm__ __volatile__("sti; hlt");
    }
}

int timer_register_callback(timer_callback_fn cb, uint32_t period_ms) {
    if (!cb) return -1;
    uint32_t period_ticks = (period_ms + MS_PER_TICK - 1) / MS_PER_TICK;
    if (period_ticks == 0) period_ticks = 1;

    for (int i = 0; i < TIMER_MAX_CALLBACKS; i++) {
        if (!g_slots[i].active) {
            g_slots[i].cb           = cb;
            g_slots[i].period_ticks = period_ticks;
            g_slots[i].counter      = period_ticks;
            g_slots[i].active       = true;
            return i;
        }
    }
    return -1;
}

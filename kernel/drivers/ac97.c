// ============================================================================
// ac97.c — реализация AC'97 драйвера для MyOS
// ----------------------------------------------------------------------------
// Подход: один "пул" из 32 BDL-дескрипторов, каждый указывает на
// собственный 8K-байтный буфер (4096 samples × 2 байта). Последовательно
// записываем новые данные в следующий свободный буфер, продвигаем LVI.
// После завершения playback флаг FIFOE/DCH в SR говорит "всё сыграно".
//
// IRQ-обработчик снимает флаги (write-1-to-clear), считает кол-во
// проигранных буферов и будит producer (через флаг ac97_state.irq_bumped).
//
// Синхронизация: буфер пишется в обычном контексте, IRQ-обработчик
// только очищает статус и продвигает tail. Конкуренция минимальна —
// блокировки не нужны, доступ к hardware регистрам атомарен (outX/inX).
// ============================================================================

#include "ac97.h"
#include "pci.h"
#include "../io.h"
#include "../memory/heap.h"
#include "../arch/i386/isr.h"
#include "../debug_serial.h"

#include <stddef.h>

// --- VendorID/DeviceID для QEMU ICH AC'97 ---
#define AC97_VENDOR_INTEL   0x8086
#define AC97_DEVICE_ICH     0x2415   // 82801AA
#define AC97_DEVICE_ICH_2   0x2425   // 82801AB
#define AC97_DEVICE_ICH_3   0x2445   // 82801BA
#define AC97_DEVICE_ICH_4   0x2485   // 82801CA
#define AC97_DEVICE_ICH_5   0x24C5   // 82801DB

// ============================================================================
// Состояние драйвера
// ============================================================================
typedef struct {
    bool      ready;
    uint16_t  nam_base;      // BAR0 — mixer
    uint16_t  nabm_base;     // BAR1 — bus master
    uint8_t   irq;

    // BDL (выравнивание 8, 32 элемента = 256 байт).
    ac97_bdl_entry_t* bdl;

    // Указатели на сами аудиобуферы (каждый — AC97_SAMPLES_PER_ENTRY × 2 байт).
    int16_t*  buffers[AC97_BDL_ENTRIES];

    // Индексы "producer → consumer" для управления очередью.
    volatile uint8_t head;    // куда будем писать следующий блок
    volatile uint8_t tail;    // какой блок сейчас (или последний) играет
    volatile bool    running; // выставлен бит RPBM

    // Счётчик сработавших IRQ — для отладки.
    volatile uint32_t irq_count;
} ac97_state_t;

static ac97_state_t ac97 = {0};

// ============================================================================
// Низкоуровневые IO-обёртки
// ============================================================================
static inline void nam_w16(uint16_t off, uint16_t val) { outw(ac97.nam_base + off, val); }
static inline uint16_t nam_r16(uint16_t off) { return inw(ac97.nam_base + off); }

static inline void nabm_w8 (uint16_t off, uint8_t  v) { outb(ac97.nabm_base + off, v); }
static inline void nabm_w16(uint16_t off, uint16_t v) { outw(ac97.nabm_base + off, v); }
static inline void nabm_w32(uint16_t off, uint32_t v) { outl(ac97.nabm_base + off, v); }
static inline uint8_t  nabm_r8 (uint16_t off) { return inb(ac97.nabm_base + off); }
static inline uint16_t nabm_r16(uint16_t off) { return inw(ac97.nabm_base + off); }
static inline uint32_t nabm_r32(uint16_t off) { return inl(ac97.nabm_base + off); }

// ============================================================================
// IRQ handler — вызывается из isr_register_handler(32+irq, ...)
// ============================================================================
static uint32_t ac97_irq_handler(registers_t* regs) {
    (void)regs;
    uint16_t sr = nabm_r16(AC97_PO_SR);
    // Любые "липкие" биты статуса write-1-to-clear.
    // BCIS = Buffer Completion Interrupt Status.
    // LVBCI = Last Valid Buffer Completion Interrupt.
    // FIFOE = FIFO Error.
    uint16_t clear = sr & (AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFOE);
    if (clear) {
        nabm_w16(AC97_PO_SR, clear);
    }

    // Продвинем tail на текущий CIV — это "консьюмер" знает, где сейчас DMA.
    ac97.tail = nabm_r8(AC97_PO_CIV);
    ac97.irq_count++;

    if (sr & AC97_SR_DCH) {
        // Controller Halted — буфер кончился. Следующий ac97_play поднимет RPBM заново.
        ac97.running = false;
    }

    return (uint32_t)regs;
}

// ============================================================================
// Инициализация
// ============================================================================
bool ac97_init(void) {
    // Ищем любой из поддерживаемых AC'97 (QEMU по умолчанию 0x2415).
    pci_device_t dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_2);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_3);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_4);
    if (!dev.found) dev = pci_find_device(AC97_VENDOR_INTEL, AC97_DEVICE_ICH_5);
    // Последний шанс — любой class=4 subclass=1 (multimedia audio, AC'97).
    if (!dev.found) dev = pci_find_by_class(0x04, 0x01);

    if (!dev.found) {
        dbg_puts("[AC97] controller not found — sound disabled\n");
        return false;
    }

    ac97.nam_base  = (uint16_t)(pci_read_bar(&dev, 0) & 0xFFFF);
    ac97.nabm_base = (uint16_t)(pci_read_bar(&dev, 1) & 0xFFFF);
    ac97.irq       = dev.irq_line;

    dbg_puts("[AC97] found: NAM=");
    dbg_hex(ac97.nam_base);
    dbg_puts(" NABM=");
    dbg_hex(ac97.nabm_base);
    dbg_puts(" IRQ=");
    dbg_hex(ac97.irq);
    dbg_puts("\n");

    pci_enable_bus_master(&dev);

    // --- Cold reset ---
    // GCR: Cold Reset = bit 1 — записываем "1", устройство само сбросит в "1" когда готово.
    nabm_w32(AC97_GLOB_CNT, 0x00000002);
    // Маленькая задержка через чтение порта 0x80 (1 μs на реальном железе, но для QEMU неважно).
    for (int i = 0; i < 1000; i++) io_wait();

    // --- Reset PCM OUT box ---
    nabm_w8(AC97_PO_CR, AC97_CR_RR);
    for (int i = 0; i < 1000; i++) io_wait();

    // --- Reset codec mixer ---
    nam_w16(AC97_NAM_RESET, 0x0000);

    // Power up: очистим PCM powerdown.
    nam_w16(AC97_NAM_POWERDOWN, 0x0000);

    // Громкость на максимум (0x0000 = 0 дБ, громко).
    nam_w16(AC97_NAM_MASTER_VOLUME, 0x0000);
    nam_w16(AC97_NAM_PCM_OUT_VOL,   0x0000);

    // --- Аллокация BDL + 32 буферов ---
    ac97.bdl = (ac97_bdl_entry_t*)kmalloc_aligned(
        sizeof(ac97_bdl_entry_t) * AC97_BDL_ENTRIES, 16);
    if (!ac97.bdl) {
        dbg_puts("[AC97] BDL alloc failed\n");
        return false;
    }
    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        size_t bufsz = AC97_SAMPLES_PER_ENTRY * sizeof(int16_t);
        ac97.buffers[i] = (int16_t*)kmalloc_aligned(bufsz, 16);
        if (!ac97.buffers[i]) {
            dbg_puts("[AC97] buffer alloc failed\n");
            return false;
        }
        // Заполним тишиной
        for (uint32_t j = 0; j < AC97_SAMPLES_PER_ENTRY; j++) {
            ac97.buffers[i][j] = 0;
        }
        ac97.bdl[i].buf_ptr     = (uint32_t)ac97.buffers[i];  // identity-mapped
        ac97.bdl[i].num_samples = AC97_SAMPLES_PER_ENTRY;
        ac97.bdl[i].flags       = 0;  // IOC будем выставлять по месту
    }

    // --- Сообщаем контроллеру адрес BDL ---
    nabm_w32(AC97_PO_BDBAR, (uint32_t)ac97.bdl);

    // --- Регистрируем IRQ ---
    // irq_line у QEMU AC'97 обычно 9, но берём из PCI config.
    if (ac97.irq < 16) {
        isr_register_handler((uint8_t)(32 + ac97.irq), ac97_irq_handler);
    }

    ac97.head = 0;
    ac97.tail = 0;
    ac97.running = false;
    ac97.irq_count = 0;
    ac97.ready = true;

    dbg_puts("[AC97] init OK\n");
    return true;
}

bool ac97_is_ready(void) { return ac97.ready; }

// ============================================================================
// Установка громкости (0..100 %)
// ============================================================================
void ac97_set_volume(uint8_t percent) {
    if (!ac97.ready) return;
    if (percent > 100) percent = 100;
    // AC'97 Master Volume: 6 бит на канал, 0 = громко, 63 = тихо/mute.
    uint8_t att = (uint8_t)(((100 - percent) * 63) / 100);
    uint16_t val = ((uint16_t)att << 8) | att;
    nam_w16(AC97_NAM_MASTER_VOLUME, val);
    nam_w16(AC97_NAM_PCM_OUT_VOL,   val);
}

// ============================================================================
// Остановка воспроизведения
// ============================================================================
void ac97_stop(void) {
    if (!ac97.ready) return;
    nabm_w8(AC97_PO_CR, 0);  // снимаем RPBM
    // Сбросим статус
    nabm_w16(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFOE);
    ac97.running = false;
    ac97.head = 0;
    ac97.tail = 0;
}

// ============================================================================
// Главный вход: копируем блок в следующий свободный buffer-дескриптор
// и продвигаем LVI. Запустим RPBM, если он ещё не поднят.
//
// sample_count = общее число samples (обоих каналов). Значение нечётное
// безопасно — последний sample L без R заполнит нулём.
// ============================================================================
size_t ac97_play(const int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    (void)sample_rate;  // ICH AC'97 фиксированный 48 kHz
    if (!ac97.ready || !samples || sample_count == 0) return 0;

    size_t written = 0;
    while (written < sample_count) {
        // Проверка переполнения ring buffer: head+1 == tail (mod 32) — все буферы заняты.
        uint8_t next = (uint8_t)((ac97.head + 1) & (AC97_BDL_ENTRIES - 1));
        if (next == ac97.tail && ac97.running) {
            // Очередь полна — ждём, пока IRQ продвинет tail.
            // В учебной ОС здесь часто делают busy-wait:
            for (volatile int i = 0; i < 100000; i++) { }
            continue;
        }

        int16_t* dst = ac97.buffers[ac97.head];
        size_t   copy = sample_count - written;
        if (copy > AC97_SAMPLES_PER_ENTRY) copy = AC97_SAMPLES_PER_ENTRY;

        for (size_t i = 0; i < copy; i++) {
            dst[i] = samples[written + i];
        }
        // Остаток буфера — тишина (чтобы "хвост" не трещал).
        for (size_t i = copy; i < AC97_SAMPLES_PER_ENTRY; i++) {
            dst[i] = 0;
        }
        // Обновляем BDL-запись: кол-во samples = полный размер (выровнено),
        // IOC=1 чтобы получить IRQ в конце.
        ac97.bdl[ac97.head].num_samples = AC97_SAMPLES_PER_ENTRY;
        ac97.bdl[ac97.head].flags       = (1 << 15);  // IOC

        ac97.head = next;
        written += copy;

        // Обновляем LVI на "последний только что заполненный" индекс (ac97.head-1).
        uint8_t lvi = (uint8_t)((ac97.head + AC97_BDL_ENTRIES - 1) & (AC97_BDL_ENTRIES - 1));
        nabm_w8(AC97_PO_LVI, lvi);

        // Если DMA ещё не запущен — запускаем.
        if (!ac97.running) {
            // Сбросим status perekryshki (BCIS/LVBCI/FIFOE).
            nabm_w16(AC97_PO_SR, AC97_SR_BCIS | AC97_SR_LVBCI | AC97_SR_FIFOE);
            // RPBM=1 + IOCE=1 (interrupt on completion).
            nabm_w8(AC97_PO_CR, AC97_CR_RPBM | AC97_CR_IOCE | AC97_CR_FEIE);
            ac97.running = true;
        }
    }

    return written;
}

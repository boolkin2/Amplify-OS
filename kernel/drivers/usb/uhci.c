// ============================================================================
// uhci.c — Реализация UHCI host controller (Этап 28)
// ----------------------------------------------------------------------------
// Pure-polling, без IRQ. Достаточно для одной флешки в QEMU.
// ============================================================================

#include "uhci.h"
#include "../pci.h"
#include "../../io.h"
#include "../timer.h"
#include "../../memory/pmm.h"
#include "../../memory/heap.h"
#include "../../vga.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// PCI class для USB host controllers
#define PCI_CLASS_SERIAL_BUS   0x0C
#define PCI_SUBCLASS_USB       0x03
#define PCI_PROG_IF_UHCI       0x00

// ----------------------------------------------------------------------------
// Глобальный экземпляр контроллера (в учебной ОС нам хватит одного).
// ----------------------------------------------------------------------------
static uhci_controller_t g_uhci;

// ----------------------------------------------------------------------------
// Регистровые хелперы
// ----------------------------------------------------------------------------
static inline uint16_t uhci_in16(uint16_t off) { return inw(g_uhci.io_base + off); }
static inline uint32_t uhci_in32(uint16_t off) { return inl(g_uhci.io_base + off); }
static inline void     uhci_out16(uint16_t off, uint16_t v) { outw(g_uhci.io_base + off, v); }
static inline void     uhci_out32(uint16_t off, uint32_t v) { outl(g_uhci.io_base + off, v); }

// ----------------------------------------------------------------------------
// Простой busy-wait (микросекунды). У нас timer 100Hz, поэтому делаем
// «грубый» цикл NOP-ов плюс при необходимости сон через timer_get_ticks.
// ----------------------------------------------------------------------------
static void uhci_udelay(uint32_t us) {
    // Очень грубо — для QEMU достаточно. Реальная задержка может быть больше,
    // но это безопасно: USB спецификация требует МИНИМУМЫ.
    volatile uint32_t i;
    for (i = 0; i < us * 100; i++) __asm__ volatile("nop");
}
static void uhci_mdelay(uint32_t ms) {
    uint64_t start = timer_get_ticks();
    uint64_t need  = (uint64_t)ms / 10 + 1;  // 100Hz → 10мс/тик
    while (timer_get_ticks() - start < need) {
        __asm__ volatile("pause");
    }
}

// ----------------------------------------------------------------------------
// Аллокация TD/QH из статических пулов в DMA-памяти.
// ----------------------------------------------------------------------------
static uhci_td_t* uhci_td_alloc(void) {
    for (int i = 0; i < UHCI_TD_POOL_SIZE; i++) {
        if (!g_uhci.td_used[i]) {
            g_uhci.td_used[i] = true;
            uhci_td_t* td = &g_uhci.td_pool[i];
            td->link = UHCI_PTR_TERMINATE;
            td->ctrl_status = 0;
            td->token = 0;
            td->buffer = 0;
            td->sw_next = 0;
            return td;
        }
    }
    return NULL;
}

static void uhci_td_free(uhci_td_t* td) {
    if (!td) return;
    int idx = (int)(td - g_uhci.td_pool);
    if (idx < 0 || idx >= UHCI_TD_POOL_SIZE) return;
    g_uhci.td_used[idx] = false;
}

static uint32_t uhci_td_phys(uhci_td_t* td) {
    if (!td) return 0;
    int idx = (int)(td - g_uhci.td_pool);
    return g_uhci.td_pool_phys + (uint32_t)(idx * sizeof(uhci_td_t));
}

static uhci_qh_t* uhci_qh_alloc(void) {
    for (int i = 0; i < UHCI_QH_POOL_SIZE; i++) {
        if (!g_uhci.qh_used[i]) {
            g_uhci.qh_used[i] = true;
            uhci_qh_t* qh = &g_uhci.qh_pool[i];
            qh->head_link = UHCI_PTR_TERMINATE;
            qh->element = UHCI_PTR_TERMINATE;
            return qh;
        }
    }
    return NULL;
}

static uint32_t uhci_qh_phys(uhci_qh_t* qh) {
    if (!qh) return 0;
    int idx = (int)(qh - g_uhci.qh_pool);
    return g_uhci.qh_pool_phys + (uint32_t)(idx * sizeof(uhci_qh_t));
}

// ----------------------------------------------------------------------------
// Установка одного TD под конкретный pid/data
// ----------------------------------------------------------------------------
static void uhci_td_setup(uhci_td_t* td,
                          uint32_t   next_phys_link, // уже с флагами
                          uint8_t    pid,
                          uint8_t    addr, uint8_t ep,
                          bool       data_toggle,
                          uint16_t   length,         // 0 → no data, иначе фактич. длина
                          uint32_t   buffer_phys,
                          bool       low_speed)
{
    td->link = next_phys_link;

    uint32_t cs = UHCI_TD_ACTIVE | (3u << 27); // 3 retry
    if (low_speed) cs |= UHCI_TD_LS;
    td->ctrl_status = cs;

    uint32_t tok = (uint32_t)pid;
    tok |= UHCI_TOKEN_DEVADDR(addr);
    tok |= UHCI_TOKEN_ENDPT(ep);
    if (data_toggle) tok |= UHCI_TOKEN_DATA_TOGGLE;
    if (length == 0) {
        tok |= UHCI_TOKEN_NULL_LEN;
    } else {
        tok |= UHCI_TOKEN_MAXLEN(length);
    }
    td->token  = tok;
    td->buffer = buffer_phys;
}

// ----------------------------------------------------------------------------
// Поллинг цепочки TD до завершения / ошибки. Возвращает 0 = OK, -1 = ошибка.
// ----------------------------------------------------------------------------
static int uhci_wait_chain(uhci_td_t* head, uint32_t timeout_ms) {
    uint64_t start = timer_get_ticks();
    uint64_t deadline = start + (uint64_t)(timeout_ms / 10 + 2);

    while (timer_get_ticks() < deadline) {
        // Проверяем все TD в цепочке (до 8 максимум — control, bulk хватает)
        uhci_td_t* td = head;
        bool all_done = true;
        for (int i = 0; td && i < 16; i++) {
            uint32_t cs = td->ctrl_status;
            if (cs & UHCI_TD_ANY_ERROR) {
                return -1;
            }
            if (cs & UHCI_TD_ACTIVE) {
                all_done = false;
                break;
            }
            // Перейти к следующему
            uint32_t next = td->link;
            if (next & UHCI_PTR_TERMINATE) break;
            // next — физический; найти по индексу
            uint32_t idx = (next - g_uhci.td_pool_phys) / sizeof(uhci_td_t);
            if (idx >= UHCI_TD_POOL_SIZE) break;
            td = &g_uhci.td_pool[idx];
        }
        if (all_done) return 0;
        __asm__ volatile("pause");
    }
    return -1; // timeout
}

// ----------------------------------------------------------------------------
// Отцепить цепочку от QH и освободить все TD.
// ----------------------------------------------------------------------------
static void uhci_chain_release(uhci_td_t* head) {
    g_uhci.root_qh->element = UHCI_PTR_TERMINATE;
    while (head) {
        uhci_td_t* next = NULL;
        uint32_t link = head->link;
        if (!(link & UHCI_PTR_TERMINATE)) {
            uint32_t idx = (link - g_uhci.td_pool_phys) / sizeof(uhci_td_t);
            if (idx < UHCI_TD_POOL_SIZE) next = &g_uhci.td_pool[idx];
        }
        uhci_td_free(head);
        head = next;
    }
}

// ============================================================================
// Public API
// ============================================================================

bool uhci_is_present(void) {
    return g_uhci.present;
}

// Поиск UHCI на PCI и инициализация.
bool uhci_init(void) {
    g_uhci.present = false;

    pci_device_t dev = pci_find_by_class(PCI_CLASS_SERIAL_BUS, PCI_SUBCLASS_USB);
    if (!dev.found) return false;
    if (dev.prog_if != PCI_PROG_IF_UHCI) {
        // На QEMU PIIX3 prog_if = 0x00, значит UHCI; иначе пропускаем
        // (могут быть EHCI/OHCI/xHCI).
        return false;
    }

    // BAR4 — IO base (UHCI всегда в IO space). Берём 32 байта.
    uint32_t bar4 = pci_read_bar(&dev, 4);
    if (!bar4 || !pci_bar_is_io(&dev, 4)) return false;
    g_uhci.io_base = (uint16_t)(bar4 & 0xFFFC);

    pci_enable_bus_master(&dev);

    // Глобальный сброс контроллера (HCRESET)
    uhci_out16(UHCI_USBCMD, UHCI_CMD_HCRESET);
    uhci_mdelay(50);
    uint16_t cmd = uhci_in16(UHCI_USBCMD);
    if (cmd & UHCI_CMD_HCRESET) {
        // Сброс не отработал — оборудование не отвечает.
        return false;
    }

    // Маскируем все прерывания (мы используем поллинг).
    uhci_out16(UHCI_USBINTR, 0);
    uhci_out16(UHCI_USBSTS, 0xFFFF);   // очистить все sticky-биты статуса

    // Аллоцируем frame list (4KB, page-aligned).
    g_uhci.frame_list_phys = pmm_alloc_page();
    if (!g_uhci.frame_list_phys) return false;
    g_uhci.frame_list = (uint32_t*)g_uhci.frame_list_phys;

    // Аллоцируем TD pool (≈ 64 × 32 = 2 KB) и QH pool (≈ 8 × 16 = 128 B).
    // Для простоты — на отдельной странице каждый.
    g_uhci.td_pool_phys = pmm_alloc_page();
    g_uhci.qh_pool_phys = pmm_alloc_page();
    if (!g_uhci.td_pool_phys || !g_uhci.qh_pool_phys) return false;
    g_uhci.td_pool = (uhci_td_t*)g_uhci.td_pool_phys;
    g_uhci.qh_pool = (uhci_qh_t*)g_uhci.qh_pool_phys;

    // Очистить пулы и метки.
    for (int i = 0; i < UHCI_TD_POOL_SIZE; i++) {
        g_uhci.td_used[i] = false;
    }
    for (int i = 0; i < UHCI_QH_POOL_SIZE; i++) {
        g_uhci.qh_used[i] = false;
    }
    // Зануляем DMA-память
    for (uint32_t i = 0; i < 4096 / 4; i++) {
        ((uint32_t*)g_uhci.td_pool)[i] = 0;
        ((uint32_t*)g_uhci.qh_pool)[i] = 0;
    }

    // Создаём root QH.
    g_uhci.root_qh = uhci_qh_alloc();
    if (!g_uhci.root_qh) return false;
    g_uhci.root_qh_phys = uhci_qh_phys(g_uhci.root_qh);
    g_uhci.root_qh->head_link = UHCI_PTR_TERMINATE;
    g_uhci.root_qh->element   = UHCI_PTR_TERMINATE;

    // Все 1024 frame list entry указывают на root QH.
    uint32_t link_to_root = g_uhci.root_qh_phys | UHCI_PTR_QH;
    for (int i = 0; i < UHCI_FRAME_LIST_LEN; i++) {
        g_uhci.frame_list[i] = link_to_root;
    }

    // Программируем регистры контроллера.
    uhci_out32(UHCI_FRBASEADD, g_uhci.frame_list_phys);
    uhci_out16(UHCI_FRNUM, 0);
    uhci_out16(UHCI_USBSTS, 0xFFFF);

    // Запуск!
    uhci_out16(UHCI_USBCMD, UHCI_CMD_RUN | UHCI_CMD_CF | UHCI_CMD_MAXP_64);
    uhci_mdelay(50);

    g_uhci.present = true;

    // Сбрасываем оба порта и фиксируем подключения.
    for (int p = 0; p < 2; p++) {
        g_uhci.port_connected[p] = uhci_port_reset(p);
    }
    return true;
}

// ----------------------------------------------------------------------------
// Сброс одного порта. Возвращает true, если устройство подключено.
// ----------------------------------------------------------------------------
bool uhci_port_reset(int port) {
    if (!g_uhci.present) return false;
    if (port < 0 || port > 1) return false;
    uint16_t reg = (port == 0) ? UHCI_PORTSC1 : UHCI_PORTSC2;

    // Поднять reset на ~50ms
    uint16_t v = uhci_in16(reg);
    uhci_out16(reg, v | UHCI_PORT_RESET);
    uhci_mdelay(50);
    // Снять reset
    v = uhci_in16(reg);
    uhci_out16(reg, v & ~UHCI_PORT_RESET);
    uhci_udelay(10);

    // Очистить change-биты
    v = uhci_in16(reg);
    uhci_out16(reg, v | UHCI_PORT_CSC | UHCI_PORT_PEC);

    // Проверяем подключение и включаем enable
    v = uhci_in16(reg);
    if (!(v & UHCI_PORT_CCS)) {
        g_uhci.port_connected[port] = false;
        return false;
    }
    // Запоминаем low/full speed
    g_uhci.port_low_speed[port] = (v & UHCI_PORT_LSDA) != 0;
    g_uhci.port_connected[port] = true;

    // Включить порт.
    v = uhci_in16(reg);
    uhci_out16(reg, v | UHCI_PORT_PE);
    uhci_mdelay(20);

    return true;
}

bool uhci_port_connected(int port, bool* low_speed_out) {
    if (port < 0 || port > 1) return false;
    if (low_speed_out) *low_speed_out = g_uhci.port_low_speed[port];
    return g_uhci.port_connected[port];
}

// ----------------------------------------------------------------------------
// Control transfer
// ----------------------------------------------------------------------------
int uhci_control_transfer(uint8_t addr, uint8_t max_packet,
                          const void* setup,
                          void* data, int data_len, bool data_in,
                          bool low_speed)
{
    if (!g_uhci.present) return -1;
    if (max_packet == 0) max_packet = 8;

    // Собираем цепочку TD.
    // 1. SETUP TD (8 байт, PID=SETUP, toggle=0)
    // 2. DATA TD's (по max_packet, PID=IN/OUT, toggle alternates 1,0,1,...)
    // 3. STATUS TD (PID противоположный data, toggle=1, без данных)

    // Аллоцируем буферы для setup и data в DMA-памяти.
    uint32_t setup_phys = pmm_alloc_page();
    if (!setup_phys) return -1;
    uint8_t* setup_buf = (uint8_t*)setup_phys;
    for (int i = 0; i < 8; i++) setup_buf[i] = ((const uint8_t*)setup)[i];

    uint32_t data_phys = 0;
    uint8_t* data_buf  = NULL;
    if (data_len > 0) {
        data_phys = pmm_alloc_page();
        if (!data_phys) { pmm_free_page(setup_phys); return -1; }
        data_buf = (uint8_t*)data_phys;
        if (!data_in) {
            for (int i = 0; i < data_len; i++) data_buf[i] = ((const uint8_t*)data)[i];
        }
    }

    // Считаем число DATA-TD'ов.
    int n_data_tds = 0;
    if (data_len > 0) {
        n_data_tds = (data_len + max_packet - 1) / max_packet;
    }
    int n_total = 1 /*setup*/ + n_data_tds + 1 /*status*/;
    if (n_total > 16) {
        if (data_phys)  pmm_free_page(data_phys);
        pmm_free_page(setup_phys);
        return -1;
    }

    uhci_td_t* tds[16];
    for (int i = 0; i < n_total; i++) {
        tds[i] = uhci_td_alloc();
        if (!tds[i]) {
            for (int j = 0; j < i; j++) uhci_td_free(tds[j]);
            if (data_phys)  pmm_free_page(data_phys);
            pmm_free_page(setup_phys);
            return -1;
        }
    }

    // Заполняем TD'ы.
    // SETUP
    uint32_t next_phys = (n_total >= 2) ? (uhci_td_phys(tds[1]) | UHCI_PTR_VF)
                                        : UHCI_PTR_TERMINATE;
    uhci_td_setup(tds[0], next_phys, UHCI_TOKEN_PID_SETUP,
                  addr, 0, false, 8, setup_phys, low_speed);

    // DATA TDs
    bool toggle = true;  // первый DATA — toggle=1
    int  remaining = data_len;
    uint32_t buf_off = 0;
    for (int i = 0; i < n_data_tds; i++) {
        int chunk = (remaining > max_packet) ? max_packet : remaining;
        uint32_t this_phys = data_phys + buf_off;
        int idx = 1 + i;
        next_phys = (idx + 1 < n_total) ? (uhci_td_phys(tds[idx + 1]) | UHCI_PTR_VF)
                                        : UHCI_PTR_TERMINATE;
        uhci_td_setup(tds[idx], next_phys,
                      data_in ? UHCI_TOKEN_PID_IN : UHCI_TOKEN_PID_OUT,
                      addr, 0, toggle, (uint16_t)chunk, this_phys, low_speed);
        toggle = !toggle;
        remaining -= chunk;
        buf_off += chunk;
    }

    // STATUS
    int status_idx = n_total - 1;
    uhci_td_setup(tds[status_idx], UHCI_PTR_TERMINATE,
                  data_in ? UHCI_TOKEN_PID_OUT : UHCI_TOKEN_PID_IN,
                  addr, 0, true, 0, 0, low_speed);

    // Прицепить цепочку к root QH
    g_uhci.root_qh->element = uhci_td_phys(tds[0]) | UHCI_PTR_VF;

    // Ждём
    int rc = uhci_wait_chain(tds[0], 1000);

    int actual = 0;
    if (rc == 0) {
        // Считаем actual_length
        for (int i = 1; i <= n_data_tds; i++) {
            uint32_t cs = tds[i]->ctrl_status;
            uint32_t got = UHCI_TD_ACTLEN(cs);
            if (got == 0x7FF) got = 0;
            actual += (int)((got + 1) & 0x7FF);
        }
        if (data_in && data_len > 0) {
            int copy = (actual < data_len) ? actual : data_len;
            for (int i = 0; i < copy; i++) ((uint8_t*)data)[i] = data_buf[i];
        }
    }

    // Очистить
    g_uhci.root_qh->element = UHCI_PTR_TERMINATE;
    for (int i = 0; i < n_total; i++) uhci_td_free(tds[i]);
    if (data_phys)  pmm_free_page(data_phys);
    pmm_free_page(setup_phys);

    return (rc == 0) ? actual : -1;
}

// ----------------------------------------------------------------------------
// Bulk transfer: дробим на TD по max_packet байт.
// ----------------------------------------------------------------------------
int uhci_bulk_transfer(uint8_t addr, uint8_t endpoint, uint16_t max_packet,
                       void* data, int data_len, bool data_in,
                       bool* data_toggle)
{
    if (!g_uhci.present) return -1;
    if (data_len <= 0 || !data) return 0;
    if (max_packet == 0) max_packet = 64;

    int n_tds = (data_len + max_packet - 1) / max_packet;
    if (n_tds > 16) return -1;

    uint32_t buf_phys = pmm_alloc_page();
    if (!buf_phys) return -1;
    uint8_t* buf = (uint8_t*)buf_phys;
    if (!data_in) {
        for (int i = 0; i < data_len; i++) buf[i] = ((const uint8_t*)data)[i];
    }

    uhci_td_t* tds[16];
    for (int i = 0; i < n_tds; i++) {
        tds[i] = uhci_td_alloc();
        if (!tds[i]) {
            for (int j = 0; j < i; j++) uhci_td_free(tds[j]);
            pmm_free_page(buf_phys);
            return -1;
        }
    }

    bool toggle = data_toggle ? *data_toggle : false;
    int  remaining = data_len;
    uint32_t off = 0;
    for (int i = 0; i < n_tds; i++) {
        int chunk = (remaining > max_packet) ? max_packet : remaining;
        uint32_t next_phys = (i + 1 < n_tds) ? (uhci_td_phys(tds[i + 1]) | UHCI_PTR_VF)
                                             : UHCI_PTR_TERMINATE;
        // SPD ставим на последний IN, чтобы short-packet корректно завершал
        // транзакцию.
        uhci_td_setup(tds[i], next_phys,
                      data_in ? UHCI_TOKEN_PID_IN : UHCI_TOKEN_PID_OUT,
                      addr, endpoint, toggle, (uint16_t)chunk,
                      buf_phys + off, false);
        if (data_in && (i + 1 == n_tds)) {
            tds[i]->ctrl_status |= UHCI_TD_SPD;
        }
        toggle = !toggle;
        remaining -= chunk;
        off += chunk;
    }

    g_uhci.root_qh->element = uhci_td_phys(tds[0]) | UHCI_PTR_VF;

    int rc = uhci_wait_chain(tds[0], 5000);

    int actual = 0;
    if (rc == 0) {
        for (int i = 0; i < n_tds; i++) {
            uint32_t cs = tds[i]->ctrl_status;
            uint32_t got = UHCI_TD_ACTLEN(cs);
            if (got == 0x7FF) {
                // нулевой пакет
            } else {
                actual += (int)((got + 1) & 0x7FF);
            }
            if (cs & UHCI_TD_ANY_ERROR) { rc = -1; break; }
        }
        if (data_in && actual > 0) {
            int copy = (actual < data_len) ? actual : data_len;
            for (int i = 0; i < copy; i++) ((uint8_t*)data)[i] = buf[i];
        }
        if (data_toggle) *data_toggle = toggle;
    }

    g_uhci.root_qh->element = UHCI_PTR_TERMINATE;
    for (int i = 0; i < n_tds; i++) uhci_td_free(tds[i]);
    pmm_free_page(buf_phys);

    return (rc == 0) ? actual : -1;
}

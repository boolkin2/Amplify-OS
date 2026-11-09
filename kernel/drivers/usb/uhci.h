// ============================================================================
// uhci.h — UHCI (Universal Host Controller Interface) для MyOS (Этап 28)
// ----------------------------------------------------------------------------
// UHCI — это контроллер USB 1.1, реализованный Intel. На большинстве QEMU-
// машин (PIIX3) UHCI присутствует «из коробки» как PCI-устройство класса
// 0x0C / subclass 0x03 / prog_if 0x00. IO BAR — всегда BAR4. 32 порта IO.
//
// Архитектура (упрощённая, под учебный kernel):
//
//   Frame List (1024 × 4 байта, 4 KB, page-aligned)
//         ↓ (все 1024 фрейма указывают на один и тот же ROOT_QH)
//   ROOT_QH ── horizontal ──→ NULL
//      │
//      vertical
//      ↓
//      Цепочка TD (transfer descriptors) для активной транзакции
//      [SETUP] → [DATA0] → [DATA1] → ... → [STATUS]
//
// Когда мы хотим выполнить control- или bulk-transfer, мы:
//   1. Аллоцируем нужное число TD'ов и связываем их (TD.link → next TD).
//   2. Прицепляем первый TD к ROOT_QH.element.
//   3. Поллим TD'ы пока не сбросится бит «Active». Контроллер сам пройдёт
//      по списку и пометит каждый TD как завершённый.
//   4. Отвязываем цепочку, освобождаем TD'ы.
//
// Без preempt'a и IRQ; всё через busy-wait (что приемлемо для USB 1.1
// при флешке как единственном устройстве).
//
// PHYS = VIRT гарантируется тем, что все DMA-структуры берутся из
// pmm_alloc_page() — нижние страницы у нас identity-mapped.
// ============================================================================

#ifndef USB_UHCI_H
#define USB_UHCI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// I/O Регистры UHCI (смещения от BAR4)
// ----------------------------------------------------------------------------
#define UHCI_USBCMD      0x00   // 16 bit — Command
#define UHCI_USBSTS      0x02   // 16 bit — Status
#define UHCI_USBINTR     0x04   // 16 bit — Interrupt Enable
#define UHCI_FRNUM       0x06   // 16 bit — Frame Number
#define UHCI_FRBASEADD   0x08   // 32 bit — Frame List Base Address (page-aligned)
#define UHCI_SOFMOD      0x0C   // 8  bit — Start Of Frame Modify
#define UHCI_PORTSC1     0x10   // 16 bit — Port 1 Status / Control
#define UHCI_PORTSC2     0x12   // 16 bit — Port 2 Status / Control

// USBCMD биты
#define UHCI_CMD_RUN       (1 << 0)
#define UHCI_CMD_HCRESET   (1 << 1)
#define UHCI_CMD_GRESET    (1 << 2)
#define UHCI_CMD_EGSM      (1 << 3)
#define UHCI_CMD_FGR       (1 << 4)
#define UHCI_CMD_SWDBG     (1 << 5)
#define UHCI_CMD_CF        (1 << 6)
#define UHCI_CMD_MAXP_64   (1 << 7)

// USBSTS биты
#define UHCI_STS_USBINT    (1 << 0)
#define UHCI_STS_ERRINT    (1 << 1)
#define UHCI_STS_RD        (1 << 2)
#define UHCI_STS_HSE       (1 << 3)
#define UHCI_STS_HCPE      (1 << 4)
#define UHCI_STS_HCH       (1 << 5)   // Halted

// PORTSC биты
#define UHCI_PORT_CCS      (1 << 0)   // Current Connect Status
#define UHCI_PORT_CSC      (1 << 1)   // Connect Status Change
#define UHCI_PORT_PE       (1 << 2)   // Port Enable
#define UHCI_PORT_PEC      (1 << 3)   // Port Enable Change
#define UHCI_PORT_LS_MASK  (3 << 4)   // Line Status
#define UHCI_PORT_RD       (1 << 6)   // Resume Detect
#define UHCI_PORT_LSDA     (1 << 8)   // Low Speed Device Attached
#define UHCI_PORT_RESET    (1 << 9)   // Port Reset
#define UHCI_PORT_SUSPEND  (1 << 12)

// ----------------------------------------------------------------------------
// Структуры в DMA-памяти (физический формат UHCI)
// ----------------------------------------------------------------------------

// Frame List Pointer / TD link / QH link флаги (младшие биты)
#define UHCI_PTR_TERMINATE (1 << 0)   // T = 1 → нет следующего
#define UHCI_PTR_QH        (1 << 1)   // Q = 1 → указатель на QH; 0 → на TD
#define UHCI_PTR_VF        (1 << 2)   // Vertical First (только для TD)

// Transfer Descriptor (UHCI требует 16-байт выравнивание для каждого TD).
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t link;          // Pointer to next TD/QH + флаги T/Q/Vf
    uint32_t ctrl_status;   // Биты состояния + длина
    uint32_t token;          // PID + addr + ep + maxlen + DataToggle
    uint32_t buffer;        // Физический адрес буфера данных
    // 16 байт software-зоны — не трогаются контроллером, мы храним свои
    // вспомогательные поля.
    uint32_t sw_next;       // Программная цепочка свободных TD
    uint32_t sw_user1;
    uint32_t sw_user2;
    uint32_t sw_user3;
} uhci_td_t;

// Биты ctrl_status
#define UHCI_TD_BITSTUFF   (1 << 17)
#define UHCI_TD_CRC        (1 << 18)
#define UHCI_TD_NAK        (1 << 19)
#define UHCI_TD_BABBLE     (1 << 20)
#define UHCI_TD_DBUFFER    (1 << 21)
#define UHCI_TD_STALL      (1 << 22)
#define UHCI_TD_ACTIVE     (1 << 23)
#define UHCI_TD_IOC        (1 << 24)
#define UHCI_TD_ISO        (1 << 25)
#define UHCI_TD_LS         (1 << 26)
#define UHCI_TD_SPD        (1 << 29)

#define UHCI_TD_ACTLEN(cs) ((cs) & 0x7FF)            // фактически передано
#define UHCI_TD_ANY_ERROR  (UHCI_TD_BITSTUFF | UHCI_TD_CRC | \
                            UHCI_TD_NAK | UHCI_TD_BABBLE | \
                            UHCI_TD_DBUFFER | UHCI_TD_STALL)

// Биты token
#define UHCI_TOKEN_PID_OUT    0xE1
#define UHCI_TOKEN_PID_IN     0x69
#define UHCI_TOKEN_PID_SETUP  0x2D

// max_len в token: hardware-format (n-1), 0x7FF = «нет данных».
#define UHCI_TOKEN_MAXLEN(len)  (((((uint32_t)(len)) - 1) & 0x7FF) << 21)
#define UHCI_TOKEN_NULL_LEN     ((0x7FFu) << 21)
#define UHCI_TOKEN_DATA_TOGGLE  (1u << 19)
#define UHCI_TOKEN_ENDPT(ep)    (((ep) & 0xF) << 15)
#define UHCI_TOKEN_DEVADDR(a)   (((a) & 0x7F) << 8)

// Queue Head (8 байт hardware + software-зона, 16-байт выровнен).
typedef struct __attribute__((packed, aligned(16))) {
    uint32_t head_link;     // Horizontal: следующий QH в schedule
    uint32_t element;       // Vertical: первый TD текущей транзакции
    uint32_t sw_user1;
    uint32_t sw_user2;
} uhci_qh_t;

// ----------------------------------------------------------------------------
// Состояние одного UHCI-контроллера
// ----------------------------------------------------------------------------
#define UHCI_FRAME_LIST_LEN  1024
#define UHCI_TD_POOL_SIZE    64
#define UHCI_QH_POOL_SIZE    8

typedef struct {
    bool      present;
    uint16_t  io_base;       // BAR4 — 32 байта IO

    // Frame list (физ. адрес == виртуальному, страница из pmm).
    uint32_t* frame_list;
    uint32_t  frame_list_phys;

    // Пулы TD/QH в DMA-памяти.
    uhci_td_t* td_pool;
    uint32_t   td_pool_phys;
    bool       td_used[UHCI_TD_POOL_SIZE];

    uhci_qh_t* qh_pool;
    uint32_t   qh_pool_phys;
    bool       qh_used[UHCI_QH_POOL_SIZE];

    // Корневой QH — все 1024 frame list entry указывают на него.
    uhci_qh_t* root_qh;
    uint32_t   root_qh_phys;

    // Состояние портов (последний known status).
    bool      port_connected[2];
    bool      port_low_speed[2];
} uhci_controller_t;

// ----------------------------------------------------------------------------
// Публичный API
// ----------------------------------------------------------------------------

// Найти, инициализировать и запустить UHCI-контроллер. Возвращает true,
// если контроллер найден и поднят. Сохраняет состояние в g_uhci.
bool uhci_init(void);

// Был ли uhci_init успешным.
bool uhci_is_present(void);

// Сбросить указанный порт (0 или 1). После reset порт включается, и можно
// читать его статус через uhci_port_status. Возвращает true, если на порту
// обнаружено устройство.
bool uhci_port_reset(int port);

// Прочитать «есть ли устройство на порту» и low/full speed.
bool uhci_port_connected(int port, bool* low_speed_out);

// Универсальный control-transfer (SETUP + DATA(in/out)? + STATUS).
//   addr      — адрес устройства (0 для default; реальное — после SET_ADDRESS)
//   max_packet— max packet size endpoint 0 (8 для default, потом из device desc)
//   setup     — указатель на 8 байт setup-пакета
//   data      — буфер данных (или NULL если no-data stage)
//   data_len  — длина data-stage; <0 для in, >0 для out, 0 — no data.
// Возвращает фактически переданное число байт, или -1 при ошибке.
int  uhci_control_transfer(uint8_t addr, uint8_t max_packet,
                           const void* setup,
                           void* data, int data_len, bool data_in,
                           bool low_speed);

// Bulk transfer: один TD за раз (нашей флешке хватит). data_in=true → IN.
// max_packet ставится из endpoint descriptor (обычно 64 для bulk).
// Возвращает фактически переданное число байт, или -1 при ошибке.
int  uhci_bulk_transfer(uint8_t addr, uint8_t endpoint, uint16_t max_packet,
                        void* data, int data_len, bool data_in,
                        bool* data_toggle /* in/out, начинается с 0 */);

#endif // USB_UHCI_H

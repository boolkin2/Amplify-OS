// ============================================================================
// usb.h — Ядро USB-стека MyOS (Этап 28)
// ----------------------------------------------------------------------------
// Поверх драйвера хост-контроллера (UHCI) этот слой реализует:
//   • Стандартные дескрипторы USB (device / configuration / interface / endpoint)
//   • Стандартные запросы из главы 9 спецификации USB:
//       GET_DESCRIPTOR, SET_ADDRESS, SET_CONFIGURATION, GET_STATUS, ...
//   • Простой энумератор: один порт UHCI → одно устройство → один интерфейс.
//
// Цель этапа 28 — флешка USB Mass Storage; класс-специфичная логика (BBB +
// SCSI) живёт в usb_msd.c и scsi.c. Этот файл — только «универсальный USB».
// ============================================================================

#ifndef USB_USB_H
#define USB_USB_H

#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// USB Setup-пакет (8 байт, всегда little-endian)
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} usb_setup_t;

// bmRequestType биты (см. USB 2.0 §9.3)
#define USB_DIR_OUT          0x00
#define USB_DIR_IN           0x80
#define USB_TYPE_STANDARD    (0 << 5)
#define USB_TYPE_CLASS       (1 << 5)
#define USB_TYPE_VENDOR      (2 << 5)
#define USB_RECIP_DEVICE     0x00
#define USB_RECIP_INTERFACE  0x01
#define USB_RECIP_ENDPOINT   0x02

// Стандартные запросы (bRequest)
#define USB_REQ_GET_STATUS         0x00
#define USB_REQ_CLEAR_FEATURE      0x01
#define USB_REQ_SET_FEATURE        0x03
#define USB_REQ_SET_ADDRESS        0x05
#define USB_REQ_GET_DESCRIPTOR     0x06
#define USB_REQ_SET_DESCRIPTOR     0x07
#define USB_REQ_GET_CONFIGURATION  0x08
#define USB_REQ_SET_CONFIGURATION  0x09

// Типы дескрипторов (старший байт wValue в GET_DESCRIPTOR)
#define USB_DT_DEVICE        0x01
#define USB_DT_CONFIG        0x02
#define USB_DT_STRING        0x03
#define USB_DT_INTERFACE     0x04
#define USB_DT_ENDPOINT      0x05

// ----------------------------------------------------------------------------
// Стандартные дескрипторы (как лежат на проводе)
// ----------------------------------------------------------------------------

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       // 0x01
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;       // EP0 max packet (8/16/32/64)
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       // 0x02
    uint16_t wTotalLength;          // суммарно с interface/endpoint descriptors
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       // 0x04
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;       // 0x08 = Mass Storage
    uint8_t  bInterfaceSubClass;    // 0x06 = SCSI transparent
    uint8_t  bInterfaceProtocol;    // 0x50 = Bulk-Only Transport
    uint8_t  iInterface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;       // 0x05
    uint8_t  bEndpointAddress;      // bit7=dir (1=IN, 0=OUT), bits3:0 ep#
    uint8_t  bmAttributes;          // 0=control, 2=bulk, 3=interrupt
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} usb_endpoint_descriptor_t;

#define USB_EP_DIR_IN(addr)    (((addr) & 0x80) != 0)
#define USB_EP_NUM(addr)       ((addr) & 0x0F)
#define USB_EP_TYPE_MASK       0x03
#define USB_EP_TYPE_BULK       0x02

// Класс / sub-class / protocol для Mass Storage (используется при поиске
// «нашего» интерфейса).
#define USB_CLASS_MASS_STORAGE   0x08
#define USB_MSD_SUBCLASS_SCSI    0x06
#define USB_MSD_PROTOCOL_BBB     0x50

// ----------------------------------------------------------------------------
// Логическое представление подключённого устройства
// ----------------------------------------------------------------------------
typedef struct {
    bool      present;
    uint8_t   address;                // 1..127 (0 — default)
    bool      low_speed;
    uint8_t   max_packet0;            // EP0 max packet
    int       port;                   // UHCI-порт (0 или 1)

    usb_device_descriptor_t    devdesc;
    usb_interface_descriptor_t ifdesc;
    usb_endpoint_descriptor_t  ep_in;
    usb_endpoint_descriptor_t  ep_out;

    // Bulk-only data toggle для каждого endpoint (UHCI хранит его сам через
    // нас — драйвер не имеет аппаратного toggle, мы ведём программно).
    bool      toggle_in;
    bool      toggle_out;

    uint8_t   configuration;          // bConfigurationValue, активной конфиги
} usb_device_t;

// ----------------------------------------------------------------------------
// API ядра USB
// ----------------------------------------------------------------------------

// Полная инициализация USB-подсистемы:
//   1. uhci_init();
//   2. для каждого порта: reset → enumerate → сохранить usb_device_t.
// Возвращает количество найденных устройств (0..2).
int  usb_init(void);

// Получить указатель на единственное устройство (или NULL).
usb_device_t* usb_get_first_device(void);

// Низкоуровневый wrap над uhci_control_transfer для класс-/MSD-кода.
int  usb_control(usb_device_t* dev, uint8_t bmReqType, uint8_t bReq,
                 uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                 void* data);

// Bulk transfer на endpoint устройства (toggle ведётся внутри usb_device_t).
int  usb_bulk_in(usb_device_t* dev, void* data, int len);
int  usb_bulk_out(usb_device_t* dev, const void* data, int len);

// Сбросить toggle (используется после Clear Feature / reset recovery в MSD).
void usb_reset_toggle_in(usb_device_t* dev);
void usb_reset_toggle_out(usb_device_t* dev);

#endif // USB_USB_H

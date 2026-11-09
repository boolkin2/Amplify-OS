// ============================================================================
// virtio_gpu.h — драйвер virtio-gpu 2D-mode для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Назначение: ускорение swap'а кадра из RAM-буфера doublebuf'а во framebuffer
// окна QEMU. Вместо memcpy в VESA VRAM мы:
//   1) Создаём GPU resource_id=1 формата VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM.
//   2) Привязываем backing — наш doublebuf RAM-буфер (1 DMA-scatter-entry).
//   3) Назначаем resource на scanout 0 (set_scanout).
//   4) На каждом swap: TRANSFER_TO_HOST_2D (CPU-копию заливает в GPU)
//      + RESOURCE_FLUSH (GPU выталкивает на дисплей).
//
// Это даёт два выигрыша:
//   * QEMU host пишет напрямую в окно SDL/GTK без VESA-эмуляции -> быстрее.
//   * Z-order атомарен: scanout всегда смотрит на единый ресурс, host
//     никогда не видит наполовину обновлённый кадр.
//
// Спецификация: virtio-spec 1.1 §5.7 (GPU Device).
// ============================================================================
#ifndef AOS_X86_64_VIRTIO_GPU_H
#define AOS_X86_64_VIRTIO_GPU_H

#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Команды virtio-gpu (control queue).
// ----------------------------------------------------------------------------
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO         0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D       0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF           0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT              0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH           0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D      0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING  0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING  0x0107
// [41.2] Команды очереди курсора (cursorq).
#define VIRTIO_GPU_CMD_UPDATE_CURSOR            0x0300
#define VIRTIO_GPU_CMD_MOVE_CURSOR              0x0301

// Ответы.
#define VIRTIO_GPU_RESP_OK_NODATA               0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO         0x1101
#define VIRTIO_GPU_RESP_ERR_UNSPEC              0x1200
#define VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       0x1201
#define VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  0x1202
#define VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  0x1204
#define VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   0x1205

// Форматы пикселей (для resource_create_2d).
//   Naming: <byte0><byte1><byte2><byte3> в little-endian памяти.
//   Наш doublebuf хранит в памяти 0xAARRGGBB → байты [BB, GG, RR, AA] в LE
//   = именно "B8G8R8A8" в virtio-gpu номенклатуре.
#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM        1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM        2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM        3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM        4
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM       67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM       68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM      121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM      134

#define VIRTIO_GPU_MAX_SCANOUTS                16
#define VIRTIO_GPU_FLAG_FENCE                  (1u << 0)

// ----------------------------------------------------------------------------
// Заголовки команд (см. virtio spec 5.7.6.1).
// Все структуры идут little-endian.
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint32_t type;          // VIRTIO_GPU_CMD_* / VIRTIO_GPU_RESP_*
    uint32_t flags;         // VIRTIO_GPU_FLAG_FENCE
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} virtio_gpu_ctrl_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t x, y, width, height;
} virtio_gpu_rect_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_rect_t r;
    uint32_t enabled;
    uint32_t flags;
} virtio_gpu_display_one_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_display_one_t pmodes[VIRTIO_GPU_MAX_SCANOUTS];
} virtio_gpu_resp_display_info_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t format;
    uint32_t width;
    uint32_t height;
} virtio_gpu_resource_create_2d_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_unref_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t scanout_id;
    uint32_t resource_id;
} virtio_gpu_set_scanout_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_resource_flush_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    virtio_gpu_rect_t r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
} virtio_gpu_transfer_to_host_2d_t;

typedef struct __attribute__((packed)) {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
} virtio_gpu_mem_entry_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t hdr;
    uint32_t resource_id;
    uint32_t nr_entries;
    // virtio_gpu_mem_entry_t entries[nr_entries] — пишется отдельным
    // дескриптором в той же chain'е.
} virtio_gpu_resource_attach_backing_t;

// [41.2] UPDATE_CURSOR / MOVE_CURSOR (5.7.6.10). MOVE использует ту же
// структуру, устройство читает из неё только pos.
typedef struct __attribute__((packed)) {
    uint32_t scanout_id;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
} virtio_gpu_cursor_pos_t;

typedef struct __attribute__((packed)) {
    virtio_gpu_ctrl_hdr_t   hdr;
    virtio_gpu_cursor_pos_t pos;
    uint32_t                resource_id;   // 0 — спрятать курсор
    uint32_t                hot_x;
    uint32_t                hot_y;
    uint32_t                padding;
} virtio_gpu_update_cursor_t;

#define VIRTIO_GPU_CURSOR_SIZE  64         // сторона ресурса курсора, пиксели

// ----------------------------------------------------------------------------
// Public API.
// ----------------------------------------------------------------------------

// Полная инициализация устройства. Возвращает true если всё ок.
// При false — фолбэк на VESA остаётся валидным.
bool virtio_gpu_init(void);

// Готовность (после init).
bool virtio_gpu_is_ready(void);

// Текущее разрешение scanout 0. Возвращает 0,0 если не готово.
uint32_t virtio_gpu_width(void);
uint32_t virtio_gpu_height(void);
void     virtio_gpu_note_mode(uint32_t w, uint32_t h);   // [41.2] после drm_set_mode

// ----------------------------------------------------------------------------
// Высокоуровневые операции — вызываются из doublebuf.c и/или DRM-syscall'ов.
// ----------------------------------------------------------------------------

// Создать 2D-ресурс. Формат фиксирован B8G8R8X8 (наш doublebuf).
// Возвращает resource_id > 0 либо 0 при ошибке.
uint32_t virtio_gpu_resource_create_2d(uint32_t width, uint32_t height);

// Привязать backing (linear буфер). Один сегмент. Возвращает true/false.
bool virtio_gpu_resource_attach_backing(uint32_t resource_id,
                                        uint64_t phys_addr, uint32_t size);

// Назначить ресурс на scanout 0. Возвращает true/false.
bool virtio_gpu_set_scanout_default(uint32_t resource_id,
                                    uint32_t width, uint32_t height);

// TRANSFER_TO_HOST_2D + RESOURCE_FLUSH в одной паре команд.
// Если r == NULL — full-frame.
bool virtio_gpu_present(uint32_t resource_id,
                        uint32_t width, uint32_t height,
                        const virtio_gpu_rect_t* dirty);

// Освободить ресурс.
bool virtio_gpu_resource_unref(uint32_t resource_id);

// ----------------------------------------------------------------------------
// [41.2] Аппаратный курсор (аналог того, что делает viogpu в Windows).
// Картинка курсора — отдельный ресурс 64x64 с альфой; положение меняется
// командой MOVE_CURSOR в очереди курсора. Сцена при движении мыши не
// перерисовывается и на хост ничего, кроме 56 байт команды, не уходит; клиент
// VNC/SPICE рисует курсор сам, без задержки на кадр.
//   virtio_gpu_cursor_define — задать картинку (ARGB, 64x64, hot-spot) и
//       показать курсор в (x, y). false — очереди курсора нет.
//   virtio_gpu_cursor_move   — передвинуть; не ждёт ответа устройства. Если
//       свободных слотов команд нет, запоминает последнюю позицию и отправит
//       её из virtio_gpu_cursor_flush.
//   virtio_gpu_cursor_flush  — дослать отложенную позицию (звать раз за проход
//       главного цикла).
//   virtio_gpu_cursor_hide   — убрать курсор с экрана.
// Вызывать только с BSP из главного потока.
// ----------------------------------------------------------------------------
bool virtio_gpu_cursor_define(const uint32_t* argb, uint32_t hot_x, uint32_t hot_y,
                              int x, int y);
bool virtio_gpu_cursor_ready(void);
void virtio_gpu_cursor_move(int x, int y);
void virtio_gpu_cursor_flush(void);
void virtio_gpu_cursor_hide(void);
uint64_t virtio_gpu_cursor_moves(void);     // сколько MOVE_CURSOR отправлено

// [41.2] Прерывания: true — ответы controlq приходят по MSI-X (ядро спит в
// hlt, пока хост исполняет команду), false — опрос.
bool virtio_gpu_irq_active(void);
// Проверить, что прерывание controlq реально доставляется, и если нет —
// вернуться к опросу. Звать, когда прерывания уже разрешены (drm_init).
void virtio_gpu_irq_selftest(void);

// ----------------------------------------------------------------------------
// PBT-friendly accessors: счётчики команд для тестов "после N submit вернётся
// N response, очередь снова пустая".
// ----------------------------------------------------------------------------
uint64_t virtio_gpu_cmd_count(void);
uint64_t virtio_gpu_resp_count(void);
uint16_t virtio_gpu_inflight(void);

#endif

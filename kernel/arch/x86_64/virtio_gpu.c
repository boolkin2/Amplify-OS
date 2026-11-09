// ============================================================================
// virtio_gpu.c — реализация драйвера virtio-gpu (2D-режим, legacy transport).
// ----------------------------------------------------------------------------
// На этап 41 используем POLL-MODE: после submit_kick крутим pit_ticks-таймер
// до появления used-элемента. IRQ от virtio-gpu МЫ НЕ ПРИНИМАЕМ — это упрощает
// модель синхронизации и совместимо с тем, что doublebuf_swap может вызываться
// из контекста с cli (ранние стадии boot, до IRQ enable). Все команды короткие
// (микросекунды на host), 100 ms таймаута за глаза.
//
// Идея использования:
//   * При init: создаём один 2D-ресурс под весь экран и attach_backing к
//     RAM-буферу doublebuf. Назначаем его на scanout 0.
//   * При swap: TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. Без unref/recreate.
// ============================================================================
#include "virtio_gpu.h"
#include "virtio.h"
#include "fb.h"
#include "doublebuf.h"
#include "heap.h"
#include "memory.h"
#include "io.h"
#include "serial.h"
#include "framestat.h"
#include "lapic.h"
#include "idt.h"

// ----------------------------------------------------------------------------
// Состояние.
// ----------------------------------------------------------------------------
typedef struct {
    bool         ready;
    virtio_dev_t dev;
    virtq_t      controlq;     // queue 0
    virtq_t      cursorq;      // queue 1 (создаём, но не используем — host требует)
    uint32_t     width;
    uint32_t     height;
    uint64_t     cmd_count;
    uint64_t     resp_count;
    // [41.2] Прерывания и курсор.
    volatile uint64_t irq_count;      // сколько раз сработал вектор controlq
    bool         cursor_q;            // очередь курсора создана
    bool         cursor_on;           // курсор задан и показан
    uint32_t     cursor_res;
    uint64_t     cursor_moves;
} vgpu_state_t;

static vgpu_state_t g = {0};

bool     virtio_gpu_is_ready(void) { return g.ready; }
uint32_t virtio_gpu_width(void)    { return g.width; }
uint32_t virtio_gpu_height(void)   { return g.height; }
void     virtio_gpu_note_mode(uint32_t w, uint32_t h) { g.width = w; g.height = h; }
uint64_t virtio_gpu_cmd_count(void)  { return g.cmd_count; }
uint64_t virtio_gpu_resp_count(void) { return g.resp_count; }
uint16_t virtio_gpu_inflight(void) {
    return (uint16_t)(g.cmd_count - g.resp_count);
}

static bool send_command_2desc(void* cmd_buf,  uint32_t cmd_len,
                               void* resp_buf, uint32_t resp_len);

// ----------------------------------------------------------------------------
// [41.2] Обработчики MSI-X. Вся работа с очередями остаётся в главном потоке:
// прерывание нужно только затем, чтобы разбудить BSP из hlt в virtq_wait_used
// (вместо busy-wait на ~100 тысяч оборотов за кадр).
// ----------------------------------------------------------------------------
static void vgpu_ctrl_irq(registers64_t* r)   { (void)r; g.irq_count++; lapic_eoi(); }
static void vgpu_cursor_irq(registers64_t* r) { (void)r; lapic_eoi(); }

bool virtio_gpu_irq_active(void) { return g.ready && g.controlq.irq; }

void virtio_gpu_irq_selftest(void) {
    if (!g.ready || !g.controlq.irq) return;
    // Одна безобидная команда: если к её концу счётчик прерываний не вырос,
    // MSI-X до нас не доходит — каждое ожидание кончалось бы только по тику
    // таймера (10 мс на команду, 20+ мс на кадр). Тогда честно уходим на опрос.
    uint64_t before = g.irq_count;
    virtio_gpu_ctrl_hdr_t cmd = {0};
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    static virtio_gpu_resp_display_info_t info;
    (void)send_command_2desc(&cmd, sizeof(cmd), &info, sizeof(info));
    // Прерывание могло прийти чуть позже ответа — дадим ему один тик.
    for (int i = 0; i < 2 && g.irq_count == before; i++) __asm__ __volatile__("sti; hlt");
    if (g.irq_count == before) {
        g.controlq.irq = false;
        serial_puts("[vgpu] MSI-X interrupt is not delivered -> polling\n");
    } else {
        serial_puts("[vgpu] controlq completion by MSI-X interrupt: OK\n");
    }
}

// ----------------------------------------------------------------------------
// Низкоуровневая обвязка: send_command_2desc — самый частый паттерн:
//   desc[0] = cmd-buffer (driver → device, READ)
//   desc[1] = resp-buffer (device → driver, WRITE)
// ----------------------------------------------------------------------------
static bool send_command_2desc(void* cmd_buf,  uint32_t cmd_len,
                               void* resp_buf, uint32_t resp_len) {
    if (!g.dev.present) return false;

    uint16_t head = virtq_alloc_chain(&g.controlq, 2);
    if (head == 0xFFFF) {
        serial_puts("[vgpu] alloc_chain fail\n");
        return false;
    }
    uint16_t d0 = head;
    uint16_t d1 = g.controlq.desc[d0].next;

    g.controlq.desc[d0].addr  = virt_to_phys(cmd_buf);
    g.controlq.desc[d0].len   = cmd_len;
    g.controlq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    g.controlq.desc[d0].next  = d1;

    g.controlq.desc[d1].addr  = virt_to_phys(resp_buf);
    g.controlq.desc[d1].len   = resp_len;
    g.controlq.desc[d1].flags = VIRTQ_DESC_F_WRITE;
    g.controlq.desc[d1].next  = 0;

    g.cmd_count++;
    virtq_submit_kick(&g.dev, &g.controlq, head);

    uint32_t got_len = 0;
    bool ok = virtq_wait_used(&g.controlq, head, /*timeout_ticks=*/10, &got_len);
    virtq_free_chain(&g.controlq, head);

    if (!ok) {
        serial_puts("[vgpu] command timeout\n");
        return false;
    }
    g.resp_count++;
    if (got_len < sizeof(virtio_gpu_ctrl_hdr_t)) {
        serial_puts("[vgpu] response too short\n");
        return false;
    }
    return true;
}

// Отдельный вариант для attach_backing: 3-desc chain (cmd + entries + resp).
static bool send_command_3desc(void* cmd_buf,    uint32_t cmd_len,
                               void* extra_buf,  uint32_t extra_len,
                               void* resp_buf,   uint32_t resp_len) {
    if (!g.dev.present) return false;

    uint16_t head = virtq_alloc_chain(&g.controlq, 3);
    if (head == 0xFFFF) {
        serial_puts("[vgpu] alloc_chain(3) fail\n");
        return false;
    }
    uint16_t d0 = head;
    uint16_t d1 = g.controlq.desc[d0].next;
    uint16_t d2 = g.controlq.desc[d1].next;

    g.controlq.desc[d0].addr  = virt_to_phys(cmd_buf);
    g.controlq.desc[d0].len   = cmd_len;
    g.controlq.desc[d0].flags = VIRTQ_DESC_F_NEXT;
    g.controlq.desc[d0].next  = d1;

    g.controlq.desc[d1].addr  = virt_to_phys(extra_buf);
    g.controlq.desc[d1].len   = extra_len;
    g.controlq.desc[d1].flags = VIRTQ_DESC_F_NEXT;
    g.controlq.desc[d1].next  = d2;

    g.controlq.desc[d2].addr  = virt_to_phys(resp_buf);
    g.controlq.desc[d2].len   = resp_len;
    g.controlq.desc[d2].flags = VIRTQ_DESC_F_WRITE;
    g.controlq.desc[d2].next  = 0;

    g.cmd_count++;
    virtq_submit_kick(&g.dev, &g.controlq, head);

    uint32_t got_len = 0;
    bool ok = virtq_wait_used(&g.controlq, head, 10, &got_len);
    virtq_free_chain(&g.controlq, head);

    if (!ok) return false;
    g.resp_count++;
    return got_len >= sizeof(virtio_gpu_ctrl_hdr_t);
}

// ----------------------------------------------------------------------------
// High-level API.
// ----------------------------------------------------------------------------
static uint32_t g_next_resource_id = 1;

static uint32_t resource_create_2d_fmt(uint32_t width, uint32_t height, uint32_t format);

uint32_t virtio_gpu_resource_create_2d(uint32_t width, uint32_t height) {
    return resource_create_2d_fmt(width, height, VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM);
}

static uint32_t resource_create_2d_fmt(uint32_t width, uint32_t height, uint32_t format) {
    if (!g.ready || width == 0 || height == 0) return 0;

    virtio_gpu_resource_create_2d_t cmd = {0};
    cmd.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id  = g_next_resource_id;
    cmd.format       = format;
    cmd.width        = width;
    cmd.height       = height;

    virtio_gpu_ctrl_hdr_t resp = {0};

    if (!send_command_2desc(&cmd, sizeof(cmd), &resp, sizeof(resp))) return 0;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("[vgpu] create_2d failed, resp=0x");
        serial_print_hex(resp.type, 8);
        serial_puts("\n");
        return 0;
    }
    return g_next_resource_id++;
}

bool virtio_gpu_resource_attach_backing(uint32_t resource_id,
                                        uint64_t phys_addr, uint32_t size) {
    if (!g.ready) return false;

    virtio_gpu_resource_attach_backing_t cmd = {0};
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd.resource_id = resource_id;
    cmd.nr_entries  = 1;

    virtio_gpu_mem_entry_t entry = {0};
    entry.addr   = phys_addr;
    entry.length = size;

    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!send_command_3desc(&cmd, sizeof(cmd),
                            &entry, sizeof(entry),
                            &resp, sizeof(resp))) return false;
    if (resp.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("[vgpu] attach_backing failed\n");
        return false;
    }
    return true;
}

bool virtio_gpu_set_scanout_default(uint32_t resource_id,
                                    uint32_t width, uint32_t height) {
    if (!g.ready) return false;

    virtio_gpu_set_scanout_t cmd = {0};
    cmd.hdr.type     = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.scanout_id   = 0;
    cmd.resource_id  = resource_id;
    cmd.r.x = 0; cmd.r.y = 0;
    cmd.r.width = width; cmd.r.height = height;

    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!send_command_2desc(&cmd, sizeof(cmd), &resp, sizeof(resp))) return false;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

bool virtio_gpu_present(uint32_t resource_id,
                        uint32_t width, uint32_t height,
                        const virtio_gpu_rect_t* dirty) {
    if (!g.ready) return false;

    virtio_gpu_rect_t r;
    if (dirty) {
        r = *dirty;
    } else {
        r.x = 0; r.y = 0; r.width = width; r.height = height;
    }

    // 1) TRANSFER_TO_HOST_2D
    virtio_gpu_transfer_to_host_2d_t t = {0};
    t.hdr.type     = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t.r            = r;
    // offset в backing'е: y*pitch + x*4. pitch == width*4 (мы создавали
    // ресурс ровно size = width*height*4).
    t.offset       = (uint64_t)r.y * (uint64_t)width * 4 + (uint64_t)r.x * 4;
    t.resource_id  = resource_id;
    virtio_gpu_ctrl_hdr_t resp_t = {0};
    // [41.2] TRANSFER и FLUSH меряем раздельно: первый — копия пикселей на
    // стороне хоста (растёт с площадью), второй — обновление дисплея.
    uint64_t fs_t0 = fs_now();
    bool sent = send_command_2desc(&t, sizeof(t), &resp_t, sizeof(resp_t));
    fs_add(FS_XFER, fs_t0);
    if (!sent) return false;
    if (resp_t.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("[vgpu] transfer failed\n");
        return false;
    }

    // 2) RESOURCE_FLUSH
    virtio_gpu_resource_flush_t f = {0};
    f.hdr.type     = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    f.r            = r;
    f.resource_id  = resource_id;
    virtio_gpu_ctrl_hdr_t resp_f = {0};
    fs_t0 = fs_now();
    sent = send_command_2desc(&f, sizeof(f), &resp_f, sizeof(resp_f));
    fs_add(FS_FLUSH, fs_t0);
    fs_pixels((uint64_t)r.width * r.height);
    if (!sent) return false;
    if (resp_f.type != VIRTIO_GPU_RESP_OK_NODATA) {
        serial_puts("[vgpu] flush failed\n");
        return false;
    }
    return true;
}

bool virtio_gpu_resource_unref(uint32_t resource_id) {
    if (!g.ready) return false;
    virtio_gpu_resource_unref_t cmd = {0};
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.resource_id = resource_id;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!send_command_2desc(&cmd, sizeof(cmd), &resp, sizeof(resp))) return false;
    return resp.type == VIRTIO_GPU_RESP_OK_NODATA;
}

// ============================================================================
// [41.2] Аппаратный курсор. Очередь курсора — «выстрелил и забыл»: ответа на
// команду нет, устройство просто возвращает буфер. Поэтому у команд свои
// постоянные слоты (дескриптор i <-> g_cur_cmd[i]) и свой список свободных;
// общий send_command_* с ожиданием тут не годится — движение мыши не должно
// ничего ждать.
// ============================================================================
#define CURSOR_SLOTS 8
static virtio_gpu_update_cursor_t g_cur_cmd[CURSOR_SLOTS];
static uint8_t  g_cur_free[CURSOR_SLOTS];
static int      g_cur_nfree = 0;
static bool     g_cur_pending = false;       // есть неотправленная позиция
static int      g_cur_px = 0, g_cur_py = 0;
static uint32_t g_cur_pixels[VIRTIO_GPU_CURSOR_SIZE * VIRTIO_GPU_CURSOR_SIZE];

static void cursor_reclaim(void) {
    virtq_t* q = &g.cursorq;
    while (q->last_used_idx != q->used->idx) {
        uint32_t id = q->used->ring[q->last_used_idx % q->num].id;
        q->last_used_idx = (uint16_t)(q->last_used_idx + 1);
        if (id < CURSOR_SLOTS && g_cur_nfree < CURSOR_SLOTS) g_cur_free[g_cur_nfree++] = (uint8_t)id;
    }
}

// Отправить команду курсора. false — нет свободного слота (устройство ещё не
// вернуло прежние буферы).
static bool cursor_send(uint32_t type, uint32_t res, uint32_t hot_x, uint32_t hot_y, int x, int y) {
    cursor_reclaim();
    if (g_cur_nfree == 0) return false;
    uint8_t i = g_cur_free[--g_cur_nfree];
    virtio_gpu_update_cursor_t* c = &g_cur_cmd[i];
    c->hdr.type = type; c->hdr.flags = 0; c->hdr.fence_id = 0; c->hdr.ctx_id = 0; c->hdr.padding = 0;
    c->pos.scanout_id = 0;
    c->pos.x = (uint32_t)(x < 0 ? 0 : x);
    c->pos.y = (uint32_t)(y < 0 ? 0 : y);
    c->pos.padding = 0;
    c->resource_id = res; c->hot_x = hot_x; c->hot_y = hot_y; c->padding = 0;

    virtq_t* q = &g.cursorq;
    q->desc[i].addr  = virt_to_phys(c);
    q->desc[i].len   = sizeof(*c);
    q->desc[i].flags = 0;
    q->desc[i].next  = 0;
    q->avail->ring[q->avail->idx % q->num] = i;
    __asm__ __volatile__("sfence" ::: "memory");
    q->avail->idx = (uint16_t)(q->avail->idx + 1);
    __asm__ __volatile__("mfence" ::: "memory");
    virtq_notify(&g.dev, q);
    return true;
}

bool virtio_gpu_cursor_ready(void) { return g.ready && g.cursor_q && g.cursor_on; }
uint64_t virtio_gpu_cursor_moves(void) { return g.cursor_moves; }

bool virtio_gpu_cursor_define(const uint32_t* argb, uint32_t hot_x, uint32_t hot_y, int x, int y) {
    if (!g.ready || !g.cursor_q || !argb) return false;
    for (int i = 0; i < VIRTIO_GPU_CURSOR_SIZE * VIRTIO_GPU_CURSOR_SIZE; i++) g_cur_pixels[i] = argb[i];
    if (!g.cursor_res) {
        g.cursor_res = resource_create_2d_fmt(VIRTIO_GPU_CURSOR_SIZE, VIRTIO_GPU_CURSOR_SIZE,
                                              VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM);
        if (!g.cursor_res) return false;
        if (!virtio_gpu_resource_attach_backing(g.cursor_res, virt_to_phys(g_cur_pixels),
                                                sizeof(g_cur_pixels))) return false;
    }
    virtio_gpu_transfer_to_host_2d_t t = {0};
    t.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t.r.x = 0; t.r.y = 0; t.r.width = VIRTIO_GPU_CURSOR_SIZE; t.r.height = VIRTIO_GPU_CURSOR_SIZE;
    t.offset      = 0;
    t.resource_id = g.cursor_res;
    virtio_gpu_ctrl_hdr_t resp = {0};
    if (!send_command_2desc(&t, sizeof(t), &resp, sizeof(resp)) ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA) return false;
    if (!cursor_send(VIRTIO_GPU_CMD_UPDATE_CURSOR, g.cursor_res, hot_x, hot_y, x, y)) return false;
    g.cursor_on = true;
    g_cur_pending = false;
    return true;
}

void virtio_gpu_cursor_move(int x, int y) {
    if (!g.cursor_on) return;
    // Позиция не изменилась (кадр сцены при неподвижной мыши) — команду не шлём:
    // каждая — это уведомление устройства, то есть выход в гипервизор.
    static int last_x = -1, last_y = -1;
    if (x == last_x && y == last_y && !g_cur_pending) return;
    last_x = x; last_y = y;
    if (cursor_send(VIRTIO_GPU_CMD_MOVE_CURSOR, g.cursor_res, 0, 0, x, y)) {
        g.cursor_moves++;
        g_cur_pending = false;
    } else {
        g_cur_pending = true; g_cur_px = x; g_cur_py = y;
    }
}

void virtio_gpu_cursor_flush(void) {
    if (g.cursor_on && g_cur_pending) virtio_gpu_cursor_move(g_cur_px, g_cur_py);
}

void virtio_gpu_cursor_hide(void) {
    if (!g.cursor_on) return;
    if (cursor_send(VIRTIO_GPU_CMD_UPDATE_CURSOR, 0, 0, 0, 0, 0)) g.cursor_on = false;
}

// ----------------------------------------------------------------------------
// Init: probe + setup virtqueues + display info.
// ----------------------------------------------------------------------------
bool virtio_gpu_init(void) {
    serial_puts("[vgpu] init begin\n");

    virtio_pci_probe(&g.dev, VIRTIO_DEV_GPU_TRANS);
    if (!g.dev.present) {
        // Сообщение строго ASCII — иначе grep в test_stage41_gpu.sh
        // считает serial-лог бинарным (из-за CP1251 0x97) и не находит строку.
        serial_puts("[vgpu] virtio-gpu PCI device not found -- fallback to VESA\n");
        return false;
    }

    if (!virtio_pci_init_begin(&g.dev)) {
        serial_puts("[vgpu] init_begin failed\n");
        return false;
    }
    // Для 2D mode не запрашиваем virgl/edid — features=0.
    virtio_pci_negotiate(&g.dev, 0);

    // [41.2] MSI-X: запись 0 — controlq, запись 1 — cursorq. Нет MSI-X или
    // векторов — обе очереди остаются на опросе, как на этапе 41.
    uint16_t ctrl_entry = VIRTIO_MSI_NO_VECTOR, cur_entry = VIRTIO_MSI_NO_VECTOR;
    if (virtio_pci_msix_setup(&g.dev)) {
        if (virtio_pci_msix_route(&g.dev, 0, vgpu_ctrl_irq))   ctrl_entry = 0;
        if (virtio_pci_msix_route(&g.dev, 1, vgpu_cursor_irq)) cur_entry  = 1;
    }

    // Очередь 0 — controlq, очередь 1 — cursorq. Размер берём из device
    // (в QEMU для virtio-gpu обычно 256).
    if (!virtq_init_irq(&g.dev, &g.controlq, 0, 0, ctrl_entry)) {
        serial_puts("[vgpu] controlq init failed\n");
        return false;
    }
    // cursorq — аппаратный курсор (41.2). Если устройство сообщит size=0 —
    // обходимся программным курсором.
    g.cursor_q = virtq_init_irq(&g.dev, &g.cursorq, 1, 0, cur_entry);
    if (!g.cursor_q) {
        serial_puts("[vgpu] cursorq missing - software cursor only\n");
        // не fatal
    } else {
        g.cursorq.free_count = 0;           // дескрипторами очереди курсора управляем сами
        g.cursorq.irq = false;              // и никогда не ждём на ней в hlt
        g_cur_nfree = 0;
        for (int i = 0; i < CURSOR_SLOTS && i < g.cursorq.num; i++) g_cur_free[g_cur_nfree++] = (uint8_t)i;
    }

    virtio_pci_init_end(&g.dev);
    g.ready = true;

    // Запросим display info — узнаем размер scanout 0.
    virtio_gpu_ctrl_hdr_t cmd = {0};
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    virtio_gpu_resp_display_info_t info = {0};
    if (send_command_2desc(&cmd, sizeof(cmd), &info, sizeof(info))) {
        if (info.hdr.type == VIRTIO_GPU_RESP_OK_DISPLAY_INFO &&
            info.pmodes[0].enabled) {
            g.width  = info.pmodes[0].r.width;
            g.height = info.pmodes[0].r.height;
            serial_puts("[vgpu] scanout 0: ");
            serial_print_dec(g.width);
            serial_puts("x");
            serial_print_dec(g.height);
            serial_puts("\n");
        }
    }
    // Если не дал размер — берём из VESA-fb (host обычно стартует в том же
    // разрешении, в котором запрошен GOP / Multiboot2 framebuffer).
    if (g.width == 0 || g.height == 0) {
        const aos64_fb_t* st = fb_state();
        if (st && st->ok) {
            g.width  = st->width;
            g.height = st->height;
            serial_puts("[vgpu] using VESA size as scanout: ");
            serial_print_dec(g.width); serial_puts("x"); serial_print_dec(g.height);
            serial_puts("\n");
        }
    }

    // Дублирующее сообщение в формате `[virtio_gpu] display WxH` — этого
    // требует спецификация stage 41 (CLAUDE.md §15), и автотест
    // test_stage41_gpu.sh ищет именно эту подстроку. Под старым префиксом
    // `[vgpu]` оставляем для совместимости с прошлыми скриптами и CLAUDE.md.
    serial_puts("[virtio_gpu] display ");
    serial_print_dec(g.width); serial_puts("x"); serial_print_dec(g.height);
    serial_puts("\n");

    serial_puts("[vgpu] init OK\n");
    return true;
}

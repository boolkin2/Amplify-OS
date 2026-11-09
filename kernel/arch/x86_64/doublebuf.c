// ============================================================================
// doublebuf.c — реализация двойной буферизации.
// ----------------------------------------------------------------------------
// Этап 41: doublebuf_swap имеет два пути:
//   1) GPU-путь (drm_active() == true): вызываем drm_present_primary(),
//      который через virtio-gpu делает TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
//      RAM-буфер doublebuf ужe attached как backing scanout-ресурса, поэтому
//      memcpy в VRAM НЕ ТРЕБУЕТСЯ.
//   2) VESA-путь (fallback): qword-копирование в линейный VESA framebuffer.
//      Сохраняется на 100% поведение этапа 37.6c-5.
// ============================================================================
#include "doublebuf.h"
#include "fb.h"
#include "heap.h"
#include "serial.h"
#include "drm.h"
#include "framestat.h"

static gfx_target_t g_db = {0, 0, 0, 0};
static bool         g_init = false;
// [41.2] Счётчик выводов на экран (полных и частичных). WM выводит кадр
// кусками и держит курсор поверх сцены, поэтому обязан знать, что экраном
// пользовался кто-то ещё (экран блокировки из desktop_handle_click, шторка
// выключения): счётчик ушёл вперёд — следующий кадр рисуется и выводится целиком.
static uint64_t     g_presents = 0;
uint64_t doublebuf_present_count(void) { return g_presents; }

// [41.2] Размер настоящего framebuffer прошивки. Запасной вывод копией в LFB
// допустим только пока буфер кадра того же размера: после смены режима
// virtio-gpu копия ушла бы мимо экрана (или за пределы LFB).
static int g_lfb_w = 0, g_lfb_h = 0;
int doublebuf_native_width(void)  { return g_lfb_w; }
int doublebuf_native_height(void) { return g_lfb_h; }

// Подменить буфер кадра (новый режим). Возвращает прежний буфер — освободить
// его можно только после того, как GPU-ресурс, смотревший на него, удалён.
uint32_t* doublebuf_adopt(uint32_t* pixels, int w, int h) {
    uint32_t* old = g_db.pixels;
    g_db.pixels = pixels;
    g_db.width  = w;
    g_db.height = h;
    g_db.pitch  = w * 4;
    g_presents++;                 // WM увидит «чужой» вывод и перерисует всё
    return old;
}

bool doublebuf_init(void) {
    const aos64_fb_t* st = fb_state();
    if (!st->ok) {
        serial_puts("[doublebuf] fb not ready\n");
        return false;
    }

    int w = (int)st->width;
    int h = (int)st->height;
    size_t bytes = (size_t)w * (size_t)h * 4;

    uint32_t* buf = (uint32_t*)kmalloc(bytes);
    if (!buf) {
        serial_puts("[doublebuf] kmalloc failed\n");
        return false;
    }
    // Очищаем (чёрный непрозрачный).
    for (size_t i = 0; i < bytes / 4; i++) buf[i] = 0xFF000000u;

    g_db.pixels = buf;
    g_db.width  = w;
    g_db.height = h;
    g_db.pitch  = w * 4;
    g_init      = true;
    g_lfb_w     = w;
    g_lfb_h     = h;

    serial_puts("[doublebuf] initialized: ");
    serial_print_dec(w); serial_puts("x"); serial_print_dec(h);
    serial_puts(" (");
    serial_print_dec(bytes / 1024); serial_puts(" KB)\n");
    return true;
}

const gfx_target_t* doublebuf_target(void) {
    return g_init ? &g_db : 0;
}

void doublebuf_clear(uint32_t color) {
    if (!g_init) return;
    size_t total = (size_t)g_db.width * (size_t)g_db.height;
    uint32_t* p = g_db.pixels;
    for (size_t i = 0; i < total; i++) p[i] = color;
}

// ----------------------------------------------------------------------------
// Полный swap (qword-блок). Если pitch буфера совпадает с pitch fb (что
// нормально на bpp=32 без padding), копируем целиком одним проходом.
// ----------------------------------------------------------------------------
void doublebuf_swap(void) {
    if (!g_init) return;
    g_presents++;

    // Этап 41: если активен GPU-путь — отдаём кадр через virtio-gpu.
    // RAM-буфер doublebuf ужe привязан как backing GPU-ресурса, поэтому
    // достаточно дёрнуть TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
    if (drm_active()) {
        if (drm_present_primary()) return;
        // GPU вернул ошибку — деградируем в VESA-копию (один кадр), а на
        // следующем swap снова попробуем GPU. Так мы не теряем картинку.
        serial_puts("[doublebuf] drm_present failed, fallback this frame to VESA\n");
    }

    const aos64_fb_t* st = fb_state();
    if (!st->ok) return;
    if (g_db.width != g_lfb_w || g_db.height != g_lfb_h) return;   // режим сменён — LFB не наш

    uint64_t fs_t0 = fs_now();   // [41.2] цена копии RAM -> LFB (путь VESA)
    if ((int)st->pitch == g_db.pitch) {
        // Самый быстрый путь: единый блок qword-копии.
        size_t qwords = ((size_t)g_db.height * (size_t)g_db.pitch) / 8;
        uint64_t* dst = (uint64_t*)st->vaddr;
        uint64_t* src = (uint64_t*)g_db.pixels;
        for (size_t i = 0; i < qwords; i++) dst[i] = src[i];
    } else {
        // Различающийся pitch — копируем построчно.
        size_t row_bytes = (size_t)g_db.width * 4;
        for (int y = 0; y < g_db.height; y++) {
            uint8_t* dst = st->vaddr + (size_t)y * st->pitch;
            uint8_t* src = (uint8_t*)g_db.pixels + (size_t)y * g_db.pitch;
            for (size_t i = 0; i < row_bytes; i++) dst[i] = src[i];
        }
    }
    fs_add(FS_COPY, fs_t0);
    fs_pixels((uint64_t)g_db.width * (uint64_t)g_db.height);
}

void doublebuf_swap_rect(int x, int y, int w, int h) {
    if (!g_init) return;
    g_presents++;

    // GPU-путь умеет частичный flush — используем его.
    if (drm_active()) {
        if (drm_present_primary_rect(x, y, w, h)) return;
        // fall through to VESA
    }

    const aos64_fb_t* st = fb_state();
    if (!st->ok) return;
    if (g_db.width != g_lfb_w || g_db.height != g_lfb_h) return;   // режим сменён — LFB не наш

    // Clip
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_db.width)  w = g_db.width  - x;
    if (y + h > g_db.height) h = g_db.height - y;
    if (w <= 0 || h <= 0) return;

    uint64_t fs_t0 = fs_now();
    // [41.2] Строка копируется 32-битными словами (обе стороны выровнены по
    // пикселю) — побайтовый цикл был в 4 раза медленнее на записи в LFB.
    for (int yy = 0; yy < h; yy++) {
        uint32_t* dst = (uint32_t*)(st->vaddr + (size_t)(y + yy) * st->pitch) + x;
        const uint32_t* src = (const uint32_t*)((const uint8_t*)g_db.pixels + (size_t)(y + yy) * g_db.pitch) + x;
        for (int i = 0; i < w; i++) dst[i] = src[i];
    }
    fs_add(FS_COPY, fs_t0);
    fs_pixels((uint64_t)w * (uint64_t)h);
}

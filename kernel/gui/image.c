// ============================================================================
// image.c - Загрузка PNG/JPEG и отрисовка в MyOS (Этап 21)
// ============================================================================
// stb_image возвращает массив пикселей RGBA/RGB. Мы всегда запрашиваем
// 4 канала (RGBA), чтобы упростить блиттер.
// ============================================================================

#include "image.h"

// Те же флаги, что в stb_impl.c — чтобы stb_image.h не тянул <stdio.h>/HDR
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_TGA
#define STBI_NO_BMP
#define STBI_NO_SIMD
#define STBI_NO_FAILURE_STRINGS
#include "stb_image.h"
#include "framebuffer.h"
#include "doublebuf.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../fs/vfs.h"
#include "../vga.h"

// ============================================================================
// Загрузка
// ============================================================================

image_t* image_load_from_memory(const uint8_t* data, size_t size) {
    if (!data || size == 0) return NULL;

    int w = 0, h = 0, comp = 0;
    // desired_channels = 4 -> всегда получаем RGBA
    unsigned char* pixels = stbi_load_from_memory(
        (const stbi_uc*)data, (int)size, &w, &h, &comp, 4);

    if (!pixels) return NULL;

    image_t* img = (image_t*)kmalloc(sizeof(image_t));
    if (!img) {
        kfree(pixels);
        return NULL;
    }

    img->width  = w;
    img->height = h;
    img->pixels = pixels;  // уже выделено через STBI_MALLOC=kmalloc
    return img;
}

image_t* image_load(const char* path) {
    int fd = vfs_open_file(path, VFS_O_RDONLY);
    if (fd < 0) return NULL;

    int sz = vfs_get_filesize(fd);
    if (sz <= 0) {
        vfs_close_file(fd);
        return NULL;
    }

    uint8_t* buf = (uint8_t*)kmalloc(sz);
    if (!buf) {
        vfs_close_file(fd);
        return NULL;
    }

    int rd = vfs_read_file(fd, buf, sz);
    vfs_close_file(fd);
    if (rd != sz) {
        kfree(buf);
        return NULL;
    }

    image_t* img = image_load_from_memory(buf, sz);
    kfree(buf);

    if (img) {
        vga_puts("[IMG] Zagruzheno: ");
        vga_puts(path);
        vga_puts(" (");
        vga_print_dec(img->width);
        vga_puts("x");
        vga_print_dec(img->height);
        vga_println(")");
    }
    return img;
}

void image_free(image_t* img) {
    if (!img) return;
    if (img->pixels) kfree(img->pixels);
    kfree(img);
}

// ============================================================================
// Отрисовка
// ============================================================================

static inline void put_bb(int32_t x, int32_t y, uint32_t color) {
    uint32_t w = framebuffer_get_width();
    uint32_t h = framebuffer_get_height();
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) return;

    uint32_t* buf = doublebuf_get_buffer();
    if (!buf) {
        framebuffer_put_pixel(x, y, color);
        return;
    }
    uint32_t pitch_px = framebuffer_get_pitch() / 4;
    buf[(uint32_t)y * pitch_px + (uint32_t)x] = color;
}

static inline void blend_bb(int32_t x, int32_t y, uint32_t src, uint8_t alpha) {
    if (alpha == 0) return;
    if (alpha == 255) { put_bb(x, y, src); return; }

    uint32_t w = framebuffer_get_width();
    uint32_t h = framebuffer_get_height();
    if (x < 0 || y < 0 || (uint32_t)x >= w || (uint32_t)y >= h) return;

    uint32_t* buf = doublebuf_get_buffer();
    if (!buf) {
        framebuffer_put_pixel(x, y, src);
        return;
    }
    uint32_t pitch_px = framebuffer_get_pitch() / 4;
    uint32_t* p = &buf[(uint32_t)y * pitch_px + (uint32_t)x];

    uint32_t dst = *p;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db = (dst      ) & 0xFF;
    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >>  8) & 0xFF;
    uint32_t sb = (src      ) & 0xFF;
    uint32_t inv = 255 - alpha;

    uint32_t r = (sr * alpha + dr * inv) / 255;
    uint32_t g = (sg * alpha + dg * inv) / 255;
    uint32_t b = (sb * alpha + db * inv) / 255;

    *p = (r << 16) | (g << 8) | b;
}

void image_draw(const image_t* img, int32_t x, int32_t y) {
    if (!img || !img->pixels) return;
    const uint8_t* p = img->pixels;

    for (int32_t py = 0; py < img->height; py++) {
        for (int32_t px = 0; px < img->width; px++) {
            uint8_t r = p[0];
            uint8_t g = p[1];
            uint8_t b = p[2];
            // alpha = p[3] — игнорируем
            put_bb(x + px, y + py, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
            p += 4;
        }
    }
}

void image_draw_alpha(const image_t* img, int32_t x, int32_t y) {
    if (!img || !img->pixels) return;
    const uint8_t* p = img->pixels;

    for (int32_t py = 0; py < img->height; py++) {
        for (int32_t px = 0; px < img->width; px++) {
            uint8_t r = p[0];
            uint8_t g = p[1];
            uint8_t b = p[2];
            uint8_t a = p[3];
            blend_bb(x + px, y + py, ((uint32_t)r << 16) | ((uint32_t)g << 8) | b, a);
            p += 4;
        }
    }
}

void image_draw_scaled(const image_t* img, int32_t x, int32_t y,
                       int32_t dst_w, int32_t dst_h)
{
    if (!img || !img->pixels || dst_w <= 0 || dst_h <= 0) return;

    // Фикс-поинт соотношения исходник/цель (16.16)
    int32_t rx = (img->width  << 16) / dst_w;
    int32_t ry = (img->height << 16) / dst_h;

    for (int32_t dy = 0; dy < dst_h; dy++) {
        int32_t sy = (dy * ry) >> 16;
        if (sy >= img->height) sy = img->height - 1;
        const uint8_t* row = &img->pixels[sy * img->width * 4];

        for (int32_t dx = 0; dx < dst_w; dx++) {
            int32_t sx = (dx * rx) >> 16;
            if (sx >= img->width) sx = img->width - 1;
            const uint8_t* p = &row[sx * 4];
            uint8_t a = p[3];
            uint32_t c = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            if (a == 255) put_bb(x + dx, y + dy, c);
            else          blend_bb(x + dx, y + dy, c, a);
        }
    }
}

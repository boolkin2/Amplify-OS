// ============================================================================
// doublebuf.c — Двойная буферизация + scene-cache для MyOS (Этап 22+)
// ============================================================================
// Схема:
//   compose_buf — текущий буфер для рисования (WM компонует в него кадр).
//   scene_buf   — кэш «готовой сцены» (рабочий стол + окна) БЕЗ курсора.
//                 Позволяет при движении мыши обойтись одним memcpy вместо
//                 полной пересборки кадра (см. doublebuf_restore_scene).
//   framebuffer — GPU-видимая память, принимает готовый кадр одним блитом.
//
// Логика swap(): memcpy(fb ← compose) — атомарная «презентация». Никаких
// ротаций ролей больше не делаем: scene_buf выполняет роль «persistence»,
// а compose пересобирается каждый кадр (либо с нуля, либо из scene_buf).
// ============================================================================

#include "doublebuf.h"
#include "framebuffer.h"
#include "../memory/heap.h"
#include "../memory/memory.h"

// Буфер, в котором WM рисует текущий кадр.
static uint32_t* compose_buf = NULL;

// Scene cache — готовый кадр без курсора.
static uint32_t* scene_buf   = NULL;
static bool      scene_ready = false;

static uint32_t buffer_size   = 0;  // в байтах
static uint32_t buffer_pixels = 0;
static bool     initialized   = false;

bool doublebuf_init(void) {
    framebuffer_t* fb = framebuffer_get();
    if (!fb->initialized) return false;

    buffer_pixels = (fb->pitch / 4) * fb->height;
    buffer_size   = buffer_pixels * 4;

    compose_buf = (uint32_t*)kmalloc(buffer_size);
    scene_buf   = (uint32_t*)kmalloc(buffer_size);
    if (!compose_buf || !scene_buf) return false;

    for (uint32_t i = 0; i < buffer_pixels; i++) {
        compose_buf[i] = 0;
        scene_buf[i]   = 0;
    }

    scene_ready = false;
    initialized = true;
    return true;
}

bool doublebuf_is_initialized(void) { return initialized; }

uint32_t* doublebuf_get_buffer(void) { return compose_buf; }

void doublebuf_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!initialized) return;
    framebuffer_t* fb = framebuffer_get();
    if (x >= fb->width || y >= fb->height) return;
    uint32_t pitch_px = fb->pitch / 4;
    compose_buf[y * pitch_px + x] = color;
}

void doublebuf_clear(uint32_t color) {
    if (!initialized) return;
    for (uint32_t i = 0; i < buffer_pixels; i++) compose_buf[i] = color;
}

// Презентовать composed-кадр во фреймбуфер (один атомарный блит).
void doublebuf_swap(void) {
    if (!initialized) return;
    framebuffer_t* fb = framebuffer_get();
    if (!fb->initialized || !fb->address) return;

    memcpy(fb->address, compose_buf, buffer_size);
}

// ----------------------------------------------------------------------------
// Scene cache
// ----------------------------------------------------------------------------

void doublebuf_save_scene(void) {
    if (!initialized || !scene_buf || !compose_buf) return;
    memcpy(scene_buf, compose_buf, buffer_size);
    scene_ready = true;
}

void doublebuf_restore_scene(void) {
    if (!initialized || !scene_buf || !compose_buf || !scene_ready) return;
    memcpy(compose_buf, scene_buf, buffer_size);
}

void doublebuf_present_scene(void) {
    if (!initialized || !scene_ready) return;
    framebuffer_t* fb = framebuffer_get();
    if (!fb->initialized || !fb->address) return;
    memcpy(fb->address, scene_buf, buffer_size);
}

bool doublebuf_has_scene(void) {
    return scene_ready;
}

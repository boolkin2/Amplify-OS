// ============================================================================
// framebuffer.c - Реализация управления фреймбуфером VESA для MyOS
// ============================================================================
// Инициализирует фреймбуфер из данных Multiboot, маппит физический адрес
// видеопамяти в виртуальное адресное пространство через paging.
// ============================================================================

#include "framebuffer.h"
#include "../memory/paging.h"
#include "../memory/memory.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Структура фреймбуфера (единственный экземпляр)
static framebuffer_t fb = {
    .address = 0,
    .width = 0,
    .height = 0,
    .pitch = 0,
    .bpp = 0,
    .size = 0,
    .initialized = false
};

// ============================================================================
// Реализация функций
// ============================================================================

// Инициализация фреймбуфера
bool framebuffer_init(uint32_t fb_addr, uint32_t fb_width, uint32_t fb_height,
                      uint32_t fb_pitch, uint32_t fb_bpp) {
    // Проверяем параметры
    if (fb_addr == 0 || fb_width == 0 || fb_height == 0) {
        return false;
    }

    // Проверяем поддерживаемую глубину цвета
    if (fb_bpp != 32) {
        // Поддерживаем только 32-битный цвет
        return false;
    }

    // Сохраняем параметры фреймбуфера
    fb.width = fb_width;
    fb.height = fb_height;
    fb.pitch = fb_pitch;
    fb.bpp = fb_bpp;
    fb.size = fb_pitch * fb_height;

    // =====================================================================
    // Маппинг физического адреса фреймбуфера в виртуальное пространство
    // =====================================================================
    // Фреймбуфер находится в видеопамяти GPU (обычно по адресу 0xFD000000
    // или аналогичному). Этот адрес выше 16 MB identity mapping,
    // поэтому нужно явно отобразить эти страницы.
    //
    // Используем identity mapping: виртуальный адрес == физический адрес
    // Это позволяет обращаться к фреймбуферу напрямую по физическому адресу.
    // =====================================================================

    page_directory_t* dir = paging_get_current_directory();

    // Маппим весь фреймбуфер (с запасом — выравниваем до страниц)
    // Флаги: присутствует + записываемая + отключить кэш (для MMIO видеопамяти)
    uint32_t flags = PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE;
    paging_map_range(dir, fb_addr, fb_addr, fb.size, flags);

    // Устанавливаем указатель на видеопамять
    fb.address = (uint32_t*)fb_addr;
    fb.initialized = true;

    return true;
}

// Получение указателя на структуру фреймбуфера
framebuffer_t* framebuffer_get(void) {
    return &fb;
}

// Получение ширины экрана
uint32_t framebuffer_get_width(void) {
    return fb.width;
}

// Получение высоты экрана
uint32_t framebuffer_get_height(void) {
    return fb.height;
}

// Получение pitch
uint32_t framebuffer_get_pitch(void) {
    return fb.pitch;
}

// Проверка инициализации
bool framebuffer_is_initialized(void) {
    return fb.initialized;
}

// Установка пикселя напрямую во фреймбуфер
void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    // Проверка границ
    if (x >= fb.width || y >= fb.height || !fb.initialized) {
        return;
    }

    // Вычисляем смещение в байтах:
    // pitch — количество байт в строке (может включать выравнивание)
    // Для 32-bit: каждый пиксель занимает 4 байта
    uint32_t offset = y * (fb.pitch / 4) + x;
    fb.address[offset] = color;
}

// Очистка экрана заданным цветом
void framebuffer_clear(uint32_t color) {
    if (!fb.initialized) return;

    // Заполняем весь фреймбуфер заданным цветом
    uint32_t pixels = fb.pitch / 4 * fb.height;
    for (uint32_t i = 0; i < pixels; i++) {
        fb.address[i] = color;
    }
}

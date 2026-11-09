// ============================================================================
// fb.h — VESA-framebuffer драйвер для скелетного 64-битного ядра.
// ----------------------------------------------------------------------------
// На этапе 37.1 framebuffer берётся из Multiboot2 framebuffer-тэга, который
// GRUB заполняет, прочитав запрос 1024×768×32 из заголовка ядра.
// Поддержан только линейный 32-bpp RGB. Другие режимы → fallback на VGA-text.
// ============================================================================
#ifndef AOS_X86_64_FB_H
#define AOS_X86_64_FB_H

#include <stdint.h>
#include <stdbool.h>
#include "multiboot2.h"

typedef struct {
    bool      ok;             // true — линейный fb доступен; false — fallback в VGA-text
    uint64_t  phys;
    uint8_t*  vaddr;          // виртуальный (= phys, identity-map в boot.asm)
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    uint8_t   bpp;
} aos64_fb_t;

void     fb_init(const mb2_tag_framebuffer_t* tag);
const aos64_fb_t* fb_state(void);
// [41.2] Сменить логический размер экрана (режим virtio-gpu), см. fb.c.
void fb_set_logical_mode(uint32_t w, uint32_t h);

void     fb_clear(uint32_t color);
void     fb_put_pixel(int x, int y, uint32_t color);
void     fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void     fb_draw_string(int x, int y, const char* s, uint32_t fg, uint32_t bg);

// Курсор — номер строки/колонки в 8x16-сетке для вывода "println"-стилем.
void     fb_console_reset(void);
void     fb_console_set_color(uint32_t fg, uint32_t bg);
void     fb_console_putc(char c);
void     fb_console_puts(const char* s);
void     fb_console_print_hex(uint64_t v, int digits);
void     fb_console_print_dec(uint64_t v);

// ----------------------------------------------------------------------------
// Хук-перехват вывода консоли — используется GUI-терминалом для
// перенаправления всего вывода shell-команд в окно (в собственный scrollback).
// Если putc_hook != NULL, fb_console_putc вызывает только хук (плюс serial),
// не пишет в framebuffer. Аналогично reset_hook замещает очистку экрана.
// Установка NULL восстанавливает поведение по умолчанию.
// ----------------------------------------------------------------------------
typedef void (*fb_console_putc_hook_fn)(char c);
typedef void (*fb_console_reset_hook_fn)(void);
void     fb_console_set_hooks(fb_console_putc_hook_fn putc_hook,
                              fb_console_reset_hook_fn reset_hook);

#endif

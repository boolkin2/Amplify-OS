// ============================================================================
// doublebuf.h — двойная буферизация для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Идея: WM рисует кадр в RAM-буфер размера width×height×4. Когда кадр
// готов — doublebuf_swap() копирует его в VESA fb одной qword-копией. Это
// устраняет мерцание при анимациях.
//
// Формат пикселя — 32-bit ARGB (alpha обычно 0xFF).
// ============================================================================
#ifndef AOS_X86_64_DOUBLEBUF_H
#define AOS_X86_64_DOUBLEBUF_H

#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"

// Инициализация. Аллоцирует heap-буфер. Вызывать ПОСЛЕ heap_init() и fb_init().
bool doublebuf_init(void);

// Возвращает gfx_target_t*, в который надо рисовать. NULL если не init.
const gfx_target_t* doublebuf_target(void);

// Очистить весь буфер заданным цветом (используется как старт кадра).
void doublebuf_clear(uint32_t color);

// Скопировать буфер в VRAM (полный кадр).
void doublebuf_swap(void);

// «Частичный» swap — только указанный rect (оптимизация для cursor движения).
// [41.2] Сколько раз буфер выводили на экран (см. doublebuf.c).
uint64_t doublebuf_present_count(void);
// [41.2] Смена режима: размер исходного LFB и подмена буфера кадра (doublebuf.c).
int       doublebuf_native_width(void);
int       doublebuf_native_height(void);
uint32_t* doublebuf_adopt(uint32_t* pixels, int w, int h);

void doublebuf_swap_rect(int x, int y, int w, int h);

#endif

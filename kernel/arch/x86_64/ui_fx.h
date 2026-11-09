// ============================================================================
// ui_fx.h — быстрые пиксельные операции и «снимки» для анимаций (48.2).
// ----------------------------------------------------------------------------
// Анимация — это кадры сцены (CLAUDE.md §4 «Кадр WM»), и на кадр есть ~16 мс.
// Перерисовывать на каждом шаге содержимое панели или окна (текст, карточки,
// иконки — у workspace-окна на 1080p это 6–8 мс) нельзя; затемнение экрана
// попиксельным gfx_blend с тремя делениями на пиксель — тоже. Здесь:
//   * fx_dim / fx_blit / fx_blit_scaled — смешивание с постоянной
//     прозрачностью на SSE2, 4 пикселя за шаг, без делений (2 Мпикс ~ 2 мс);
//   * «снимок»: панель один раз рисуется в запасной буфер размером с экран,
//     а дальше анимируется готовая картинка — сдвиг, масштаб, прозрачность;
//   * fx_panel_t — общий сценарий появления/исчезновения оверлея (MIND,
//     Beacon, Constellation, шторка): затемнение + снимок панели.
// Запасной буфер один на всех (8 МБ на 1080p): им владеет тот, кто первым
// начал анимацию; второй претендент рисует по-старому, без снимка.
// При reduced_motion в теме переходы мгновенные (ui_transition_start).
// ============================================================================
#ifndef AOS_X86_64_UI_FX_H
#define AOS_X86_64_UI_FX_H

#include "graphics.h"
#include "ui_motion.h"
#include <stdbool.h>
#include <stdint.h>

// dst = dst*(255-alpha)/255 + rgb*alpha/255 по прямоугольнику.
void fx_dim(const gfx_target_t* t, int x, int y, int w, int h, uint32_t rgb, uint8_t alpha);

// Копия прямоугольника src (w x h пикселей, шаг строки src_stride ПИКСЕЛЕЙ) в
// dst с общей прозрачностью opacity. keyed: пиксели src с альфой 0 пропускаются
// (скруглённые углы снимка); частичная альфа src считается непрозрачной.
void fx_blit(const gfx_target_t* dst, int dx, int dy,
             const uint32_t* src, int src_stride, int w, int h,
             uint8_t opacity, bool keyed);

// То же с масштабированием (ближайший сосед) в прямоугольник dw x dh.
void fx_blit_scaled(const gfx_target_t* dst, int dx, int dy, int dw, int dh,
                    const uint32_t* src, int src_stride, int sw, int sh,
                    uint8_t opacity, bool keyed);

// ---- снимок панели ----------------------------------------------------------
typedef struct { bool valid; int x, y, w, h; } fx_sprite_t;

// Начать снимок: *out — мишень размером с экран, в которую владелец рисует
// панель на её обычном месте. false — буфер занят или не выделился.
bool fx_capture_begin(const void* owner, const gfx_target_t* screen, gfx_target_t* out);
// Закончить: прямоугольник панели (экранные пиксели) и радиус скругления.
void fx_capture_end(const void* owner, fx_sprite_t* sp, int x, int y, int w, int h, int radius);
// Нарисовать снимок со сдвигом (dx, dy), масштабом scale/1000 от центра и
// прозрачностью.
void fx_sprite_draw(const void* owner, const fx_sprite_t* sp, const gfx_target_t* dst,
                    int dx, int dy, int scale, uint8_t opacity);
void fx_release(const void* owner);

// ---- появление/исчезновение оверлея ----------------------------------------
typedef struct {
    const char*     name;          // для статистики в serial
    ui_transition_t tr;
    bool            shown;         // оверлей открыт (или открывается)
    bool            closing;       // идёт анимация исчезновения
    fx_sprite_t     sp;
    uint32_t        frames;        // кадров в текущей анимации
    uint64_t        t0;            // начало анимации
    uint64_t        t_last;        // последний кадр анимации (для статистики)
} fx_panel_t;

typedef void (*fx_panel_draw_fn)(const gfx_target_t* t, void* arg);

void fx_panel_show(fx_panel_t* p, uint32_t ms);
void fx_panel_hide(fx_panel_t* p, uint32_t ms);
// Видим ли оверлей (открыт или ещё исчезает) — рисовать ли его.
bool fx_panel_visible(const fx_panel_t* p);
// Идёт ли анимация. Когда исчезновение закончилось, сама просит WM о кадре.
bool fx_panel_moving(fx_panel_t* p);
// Степень проявления 0..255 (с учётом направления).
uint8_t fx_panel_level(fx_panel_t* p);
// Кадр оверлея: затемнение экрана цветом dim_rgb до dim_alpha и панель —
// из снимка, пока идёт анимация, иначе прямой вызов draw. Прямоугольник
// панели — в экранных пикселях. slide_x/slide_y — откуда панель «приезжает»
// (смещение в пикселях в начале появления).
void fx_panel_frame(fx_panel_t* p, const gfx_target_t* t,
                    uint32_t dim_rgb, uint8_t dim_alpha,
                    int x, int y, int w, int h, int radius,
                    int slide_x, int slide_y,
                    fx_panel_draw_fn draw, void* arg);

// Оверлей без собственной панели (Constellation): draw рисует весь слой —
// вуаль и содержимое. На время анимации снимается ВЕСЬ экран (сцена под
// оверлеем + слой), и этот снимок проявляется поверх живой сцены.
void fx_layer_frame(fx_panel_t* p, const gfx_target_t* t, fx_panel_draw_fn draw, void* arg);

// Идёт ли переход хотя бы одного оверлея (без побочных действий).
bool fx_motion_active(void);

// Строка статистики анимации в serial (если включён fstat в perf.conf).
void fx_log_anim(const char* kind, const char* name, const char* phase,
                 uint32_t frames, uint32_t ms);

#endif

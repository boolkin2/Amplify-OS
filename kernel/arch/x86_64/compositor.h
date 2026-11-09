// ============================================================================
// compositor.h — композитор окон AMPLIFY-OS x86_64 (этап 42).
// ----------------------------------------------------------------------------
// Композитор отвечает за:
//   * Тройную буферизацию (front / back / compose)
//   * Per-pixel alpha-blend окон с учётом window_t.opacity
//   * Gaussian-подобные тени (3-pass box blur)
//   * Скруглённые углы окон с антиалиасингом
//   * Анимации открытия/закрытия (scale + fade)
//   * Dirty-rect tracking для partial GPU flush
//
// Архитектура:
//   WM вызывает compositor_begin_frame() → рисует desktop/окна/курсор →
//   compositor_end_frame() → compositor делает swap (GPU или VESA).
//   Между begin/end WM может вызывать compositor_mark_dirty() для
//   отслеживания изменённых зон.
// ============================================================================
#ifndef AOS_X86_64_COMPOSITOR_H
#define AOS_X86_64_COMPOSITOR_H

#include <stdint.h>
#include <stdbool.h>
#include "graphics.h"
#include "window.h"

// ----------------------------------------------------------------------------
// Максимальное количество dirty-rect'ов за кадр. Если превышено — flush
// всего экрана (full-frame present).
// ----------------------------------------------------------------------------
#define COMPOSITOR_MAX_DIRTY  32

// ----------------------------------------------------------------------------
// Состояние анимации окна.
// ----------------------------------------------------------------------------
typedef enum {
    ANIM_NONE = 0,       // нет анимации
    ANIM_OPEN,           // открытие: scale 0.8→1.0 + fade 0→255
    ANIM_CLOSE,          // закрытие: scale 1.0→0.9 + fade 255→0
    ANIM_MINIMIZE,       // сворачивание (будущее)
} anim_type_t;

typedef struct {
    anim_type_t type;
    uint64_t    start_ms;      // [48.2] миллисекунды (раньше тики PIT по 10 мс —
                               // на 60 кадрах/с прогресс шёл ступеньками)
    uint32_t    duration_ms;   // длительность в мс
    uint8_t     progress;      // 0..255 (линейный прогресс)
    bool        done;          // анимация завершена
    bool        snapped;       // [48.2] содержимое окна уже снято для анимации
    uint32_t    frames;        // [48.2] кадров в этой анимации (статистика)
} anim_state_t;

// Длительности анимаций (в миллисекундах). [48.2] Workspace-окно занимает
// почти весь экран — ему дольше «подниматься»; обычное окно — короткий «хлопок».
#define ANIM_OPEN_DURATION_MS        260
#define ANIM_CLOSE_DURATION_MS       200
#define ANIM_WS_OPEN_DURATION_MS     340
#define ANIM_WS_CLOSE_DURATION_MS    230

// ----------------------------------------------------------------------------
// Dirty rect.
// ----------------------------------------------------------------------------
typedef struct {
    int x, y, w, h;
} dirty_rect_t;

// ----------------------------------------------------------------------------
// API композитора.
// ----------------------------------------------------------------------------

// Инициализация. Вызывать ПОСЛЕ doublebuf_init(). Аллоцирует compose-буфер
// (третий буфер для тройной буферизации) и shadow-буфер.
// Возвращает true при успехе.
bool compositor_init(void);

// Начало кадра: очищает dirty-список, подготавливает compose-буфер.
void compositor_begin_frame(void);

// Пометить прямоугольник как «грязный» (нужен flush).
void compositor_mark_dirty(int x, int y, int w, int h);

// Пометить весь экран как dirty (полный flush).
void compositor_mark_full_dirty(void);

// Конец кадра: выполняет swap (GPU partial или full, VESA fallback).
void compositor_end_frame(void);

// [41.2] Частичный вывод (см. compositor.c). compositor_frame_bbox возвращает
// false, если ущерб кадра — весь экран; пустой ущерб — w == 0.
bool compositor_frame_bbox(int* x, int* y, int* w, int* h);
void compositor_present_full(void);
void compositor_present_rect(int x, int y, int w, int h);
void compositor_frame_done(bool partial);

// Получить target для рисования (compose-буфер). Используется WM вместо
// doublebuf_target() напрямую.
const gfx_target_t* compositor_target(void);

// ----------------------------------------------------------------------------
// Рендеринг окна с эффектами (вызывается из wm.c вместо window_render).
// Рисует: тень → скруглённые углы → title bar → content → border.
// Учитывает opacity и анимацию.
// ----------------------------------------------------------------------------
void compositor_render_window(struct window* w, const gfx_target_t* tgt);

// ----------------------------------------------------------------------------
// Анимации.
// ----------------------------------------------------------------------------

// Запустить анимацию открытия для окна.
void compositor_anim_open(struct window* w);

// Запустить анимацию закрытия для окна.
void compositor_anim_close(struct window* w);

// Обновить состояние анимации (вызывается каждый кадр из WM).
// Возвращает true если хотя бы одна анимация активна (нужен
// принудительный redraw).
//
// ВАЖНО: вызывать ровно ОДИН раз за кадр — функция продвигает progress
// и помечает done. Для проверки «есть ли анимация» из main-loop без
// продвижения состояния используйте compositor_peek_animation_active().
bool compositor_update_animations(void);

// Read-only проверка: есть ли хотя бы одна незавершённая анимация.
// Не модифицирует прогресс анимаций — безопасно вызывать сколько угодно
// раз за кадр. Нужна wm_main_loop'у, чтобы решить «надо ли рендерить»,
// не сдвигая фазу анимации.
bool compositor_peek_animation_active(void);

// [48.2] Заморозить содержимое окон на время анимации: кадр сцены берёт
// готовые буферы окон, не вызывая on_draw (кроме окон с
// wants_continuous_redraw и ещё ни разу не нарисованных).
void compositor_set_frozen(bool frozen);

// Проверить, анимируется ли окно.
bool compositor_is_animating(struct window* w);

// Получить текущий прогресс анимации (0..255). 0 = начало, 255 = конец.
uint8_t compositor_anim_progress(struct window* w);

// ----------------------------------------------------------------------------
// Тени (улучшенные, Gaussian-подобные через 3-pass box blur).
// ----------------------------------------------------------------------------

// Нарисовать мягкую тень под прямоугольником (x,y,w,h) с радиусом blur_r
// и смещением (off_x, off_y). Цвет тени — shadow_color (ARGB).
void compositor_draw_shadow(const gfx_target_t* tgt,
                            int x, int y, int w, int h,
                            int blur_r, int off_x, int off_y,
                            uint32_t shadow_color);

// ----------------------------------------------------------------------------
// Скруглённые углы с антиалиасингом.
// ----------------------------------------------------------------------------

// Заполнить скруглённый прямоугольник с subpixel-антиалиасингом на краях.
// Качественнее чем gfx_fill_round_rect (который использует circle-test
// без AA).
void compositor_fill_round_rect_aa(const gfx_target_t* tgt,
                                   int x, int y, int w, int h,
                                   int radius, uint32_t color);

// Нарисовать скруглённую рамку (1px) с антиалиасингом.
void compositor_stroke_round_rect_aa(const gfx_target_t* tgt,
                                     int x, int y, int w, int h,
                                     int radius, uint32_t color);

// ----------------------------------------------------------------------------
// Утилиты.
// ----------------------------------------------------------------------------

// Проверить, активен ли композитор (после успешного compositor_init).
bool compositor_active(void);
void compositor_resize(int w, int h);      // [41.2] после смены режима

// Статистика.
typedef struct {
    uint64_t frames_total;
    uint64_t frames_partial;     // кадры с partial flush
    uint64_t frames_full;        // кадры с full flush
    uint64_t dirty_rects_total;  // суммарное количество dirty rect'ов
    uint64_t anims_started;
    uint64_t anims_completed;
} compositor_stats_t;

compositor_stats_t compositor_get_stats(void);

// Этап F: забыть окно перед window_destroy(). Иначе слот анимации
// держит висячий указатель, и новое окно с тем же адресом в heap
// унаследует чужую анимацию.
void compositor_forget_window(struct window* w);

#endif
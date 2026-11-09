// ============================================================================
// gles.h — программный OpenGL ES 2.0-подобный стек AMPLIFY-OS x86_64 (этап 44).
// ----------------------------------------------------------------------------
// Эта подсистема даёт оконным приложениям возможность рисовать настоящую
// 3D-графику (треугольники, Z-буфер, текстуры, шейдеры) поверх обычного
// gfx_target_t. Никакой аппаратной поддержки 3D в virtio-gpu 2D mode нет
// (см. ОГРАНИЧЕНИЕ Z-ORDER в drm.h), поэтому растеризация полностью
// программная — на CPU, как llvmpipe из Mesa.
//
//   ─────── Арифметика ───────
// До этапа 44 ядро AMPLIFY-OS собиралось с -mno-sse -mno-sse2 — FPU был
// в принципе недоступен. В этом этапе мы это исправили: boot.asm
// активирует CR0.MP|NE + CR4.OSFXSR|OSXMMEXCPT + LDMXCSR(0x1F80), а
// scheduler / IRQ entry / syscall entry делают FXSAVE/FXRSTOR. GCC
// теперь свободно использует xmm-регистры в коде ядра, и мы пишем
// 3D-математику честно через float (single precision).
//
//   ─────── Подход к шейдерам ───────
// Полноценный GLSL-компилятор в kernel-mode — это десятки тысяч строк, что
// абсолютно нецелесообразно. Вместо этого мы повторяем приём ANGLE/SwiftShader
// для embedded-сценариев: «шейдер» — это указатель на C-функцию с
// фиксированной сигнатурой. У приложения остаётся возможность писать
// произвольную логику per-vertex / per-fragment, но без рантайм-компиляции.
//
//   ─────── Поток данных ───────
//   1. App создаёт контекст:                aos_egl_create(&cfg).
//   2. App переключается на контекст:       aos_egl_make_current(ctx).
//   3. Каждый кадр:
//        gl_viewport / gl_clear_color / gl_clear(...)
//        gl_use_program(program)
//        gl_set_uniforms(&my_uniforms)        // app-specific блок
//        gl_vertex_attrib_pointer(...)
//        gl_draw_arrays(GL_TRIANGLES, 0, n)
//        aos_egl_swap_buffers(ctx)
//   4. App уничтожает контекст:             aos_egl_destroy(ctx).
//
//   Все пиксели уходят в `gfx_target_t` контекста — это offscreen-буфер
//   окна, так что мы автоматически интегрируемся с WM/compositor.
// ============================================================================
#ifndef AOS_X86_64_GLES_H
#define AOS_X86_64_GLES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "graphics.h"

// ----------------------------------------------------------------------------
// Векторы и матрицы — float (32-bit single precision).
//   Все API совместимы по layout'у с GLES2 GLfloat: 4-байтное FP.
// ----------------------------------------------------------------------------
typedef struct { float x, y;                  } gx_vec2_t;
typedef struct { float x, y, z;               } gx_vec3_t;
typedef struct { float x, y, z, w;            } gx_vec4_t;

// Матрица 4×4. Хранится column-major (стандарт OpenGL): m[col*4 + row].
// Чтение элемента: M(r,c) = m[c*4 + r].
typedef struct { float m[16]; } gx_mat4_t;

// ----------------------------------------------------------------------------
// Тригонометрия / квадратный корень.
//   Реализованы вручную в gles_math.c (CRT не подключаем — мы freestanding).
//   sqrtf использует SSE SQRTSS — это одна инструкция.
// ----------------------------------------------------------------------------
float gx_sinf (float x);
float gx_cosf (float x);
float gx_tanf (float x);
float gx_sqrtf(float x);
float gx_fabsf(float x);
float gx_fmodf(float x, float y);

#define GX_PI       3.14159265358979323846f
#define GX_HALF_PI  1.57079632679489661923f
#define GX_2PI      6.28318530717958647692f
#define GX_DEG2RAD  0.01745329251994329577f

// ----------------------------------------------------------------------------
// Геометрические утилиты (см. gles_math.c).
// ----------------------------------------------------------------------------
void gx_mat4_identity   (gx_mat4_t* out);
void gx_mat4_translate  (gx_mat4_t* out, float tx, float ty, float tz);
void gx_mat4_scale      (gx_mat4_t* out, float sx, float sy, float sz);
void gx_mat4_rotate_x   (gx_mat4_t* out, float angle_rad);
void gx_mat4_rotate_y   (gx_mat4_t* out, float angle_rad);
void gx_mat4_rotate_z   (gx_mat4_t* out, float angle_rad);

// Перспективная проекция: fovy_rad — vertical FOV, aspect = w/h.
void gx_mat4_perspective(gx_mat4_t* out, float fovy_rad, float aspect,
                         float near_z, float far_z);

// LookAt (eye, center, up). up ожидается нормализованным.
void gx_mat4_lookat     (gx_mat4_t* out,
                         const gx_vec3_t* eye,
                         const gx_vec3_t* center,
                         const gx_vec3_t* up);

// out = a * b (output может не совпадать с a/b).
void gx_mat4_mul        (gx_mat4_t* out, const gx_mat4_t* a, const gx_mat4_t* b);

// out = m * v.
void gx_mat4_mul_vec4   (gx_vec4_t* out, const gx_mat4_t* m, const gx_vec4_t* v);

// Векторные операции.
void  gx_vec3_normalize (gx_vec3_t* v);
float gx_vec3_dot       (const gx_vec3_t* a, const gx_vec3_t* b);
void  gx_vec3_cross     (gx_vec3_t* out, const gx_vec3_t* a, const gx_vec3_t* b);
void  gx_vec3_sub       (gx_vec3_t* out, const gx_vec3_t* a, const gx_vec3_t* b);

// ----------------------------------------------------------------------------
// Перечисления, эмулирующие GLES2 enums.
// ----------------------------------------------------------------------------
#define GL_FALSE            0
#define GL_TRUE             1

// Буферные биты для glClear (можно OR'ить).
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100

// Capability'ы для glEnable/glDisable.
#define GL_DEPTH_TEST       0x0B71
#define GL_CULL_FACE        0x0B44
#define GL_BLEND            0x0BE2
#define GL_SCISSOR_TEST     0x0C11

// Режимы примитивов.
#define GL_POINTS           0x0000
#define GL_LINES            0x0001
#define GL_TRIANGLES        0x0004
#define GL_TRIANGLE_STRIP   0x0005

// Front face / cull mode.
#define GL_CW               0x0900
#define GL_CCW              0x0901
#define GL_FRONT            0x0404
#define GL_BACK             0x0405

// Текстурные параметры.
#define GL_NEAREST          0x2600
#define GL_LINEAR           0x2601
#define GL_REPEAT           0x2901
#define GL_CLAMP_TO_EDGE    0x812F

// Blend factors (минимум для α-blend src-over-dst).
#define GL_SRC_ALPHA           0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE                 0x0001
#define GL_ZERO                0x0000

// Тип атрибутов вершин: только float в нашей реализации.
#define GL_FLOAT            0x1406

// ----------------------------------------------------------------------------
// Шейдеры — C-функции.  Подход SwiftShader-Lite / ANGLE-style:
//   VS:   in:  attribs  (n_attrib_floats * float per vertex)
//              uniforms (void*, opaque для рендерера)
//         out: gl_Position (gx_vec4_t),
//              varyings    (n_varying * float)
//
//   FS:   in:  varyings (n_varying * float, интерполированные)
//              uniforms (void*)
//              sampler0 (gx_sampler_t)
//         out: gl_FragColor (uint32_t ARGB)
// ----------------------------------------------------------------------------
struct gx_texture;
typedef struct gx_texture* gx_sampler_t;

typedef void (*gx_vertex_shader_fn)(
    const float*  attribs,
    const void*   uniforms,
    gx_vec4_t*    gl_Position,
    float*        varyings
);

typedef void (*gx_fragment_shader_fn)(
    const float*  varyings,
    const void*   uniforms,
    gx_sampler_t  sampler0,
    uint32_t*     gl_FragColor
);

// ----------------------------------------------------------------------------
// Программа — связка VS + FS + размеры interface'ов.
// ----------------------------------------------------------------------------
typedef struct gx_program {
    gx_vertex_shader_fn   vs;
    gx_fragment_shader_fn fs;
    int                   n_attrib_floats;   // сколько float на вершину суммарно
    int                   n_varying_floats;  // сколько varying float'ов
} gx_program_t;

// ----------------------------------------------------------------------------
// Текстура — простой 2D-битмап в нашем ARGB формате.
// ----------------------------------------------------------------------------
typedef struct gx_texture {
    int       width;
    int       height;
    uint32_t* pixels;    // ARGB; владение НЕ передаётся (caller освобождает)
    int       wrap_s;    // GL_REPEAT / GL_CLAMP_TO_EDGE
    int       wrap_t;
    int       min_filter;
    int       mag_filter;
} gx_texture_t;

// ----------------------------------------------------------------------------
// Состояние рендерера. У контекста ровно одно (как в OpenGL: state machine).
// Приложения НЕ должны лазать сюда напрямую — только через gl_* функции.
// ----------------------------------------------------------------------------
#define GX_MAX_ATTRIBS    8
#define GX_MAX_VARYINGS  16

typedef struct gx_attrib_slot {
    bool         enabled;
    const float* base;           // указатель на массив атрибутов
    int          stride_floats;  // шаг в float'ах между вершинами
    int          size_floats;    // сколько float'ов на вершину для этого slot'а
} gx_attrib_slot_t;

typedef struct gx_context {
    // Рендер-таргет (обычно это окно или offscreen-буфер).
    const gfx_target_t* target;

    // Z-buffer — float [0..1] нормализованная глубина. Аллоцируется при resize.
    float*       zbuffer;
    int          zbuf_w;
    int          zbuf_h;

    // Viewport / clear-state.
    int          vp_x, vp_y, vp_w, vp_h;
    uint32_t     clear_color;      // ARGB
    float        clear_depth;      // 0..1

    // Capabilities.
    bool         depth_test;
    bool         cull_face;
    bool         blend;
    int          front_face;       // GL_CW / GL_CCW
    int          cull_mode;        // GL_FRONT / GL_BACK
    int          blend_src;        // GL_SRC_ALPHA / GL_ONE / ...
    int          blend_dst;        // GL_ONE_MINUS_SRC_ALPHA / GL_ZERO / ...

    // Программа + uniforms.
    const gx_program_t* program;
    const void*  uniforms;

    // Сэмплер 0 (только один в нашей реализации).
    gx_sampler_t sampler0;

    // Атрибуты вершин.
    gx_attrib_slot_t attribs[GX_MAX_ATTRIBS];

    // Статистика.
    uint64_t     draw_calls;
    uint64_t     triangles_total;
    uint64_t     triangles_culled;
    uint64_t     triangles_drawn;
    uint64_t     pixels_shaded;
} gx_context_t;

// ----------------------------------------------------------------------------
// EGL-подобный API.  Один контекст = один render-target (gfx_target_t).
// ----------------------------------------------------------------------------

typedef struct {
    const gfx_target_t* target;     // куда рисуем (NULL = screen)
    bool                want_depth; // выделить Z-buffer?
} aos_egl_config_t;

// Создать контекст под указанный target. Возвращает NULL при OOM/ошибке.
// Контекст ВЛАДЕЕТ zbuffer'ом, но НЕ владеет target'ом.
gx_context_t* aos_egl_create     (const aos_egl_config_t* cfg);

// Освободить контекст.
void          aos_egl_destroy    (gx_context_t* ctx);

// Переключить текущий контекст (вызывается перед gl_*).
void          aos_egl_make_current(gx_context_t* ctx);

// Получить текущий.
gx_context_t* aos_egl_current    (void);

// Сообщить, что кадр готов. В нашей реализации НЕ копирует никуда —
// у нас render-target это уже окно, swap делает WM/compositor.
void          aos_egl_swap_buffers(gx_context_t* ctx);

// Изменить размер render-target'а (вызывается, когда окно изменило размер
// или впервые получило render-target). Пересоздаёт Z-буфер.
bool          aos_egl_resize     (gx_context_t* ctx, int new_w, int new_h);

// ----------------------------------------------------------------------------
// GLES2-подобные функции состояния.
// ----------------------------------------------------------------------------
void gl_viewport       (int x, int y, int w, int h);
void gl_clear_color    (uint32_t argb);
void gl_clear_depth    (float d);
void gl_clear          (uint32_t mask);
void gl_enable         (uint32_t cap);
void gl_disable        (uint32_t cap);
void gl_front_face     (uint32_t mode);
void gl_cull_face      (uint32_t mode);
void gl_blend_func     (uint32_t src, uint32_t dst);

// Программа и uniforms.
void gl_use_program    (const gx_program_t* prog);
void gl_set_uniforms   (const void* uniforms);

// Текстура (один глобальный сэмплер).
void gl_bind_texture   (gx_sampler_t tex);

// Атрибуты.
//   slot          — индекс 0..GX_MAX_ATTRIBS-1.
//   size_floats   — сколько float на вершину для этого слота (1..4).
//   type          — должен быть GL_FLOAT.
//   stride_floats — шаг между вершинами в float'ах (не байтах!).
//   base          — указатель на нулевую вершину этого атрибута.
void gl_vertex_attrib_pointer(int slot, int size_floats, int type,
                              int stride_floats, const float* base);
void gl_enable_vertex_attrib (int slot);
void gl_disable_vertex_attrib(int slot);

// Рисование.
void gl_draw_arrays    (uint32_t mode, int first, int count);
void gl_draw_elements  (uint32_t mode, int count, const uint16_t* indices);

// ----------------------------------------------------------------------------
// Утилиты.
// ----------------------------------------------------------------------------

// Создать gx_texture (caller владеет pixels).
gx_texture_t gx_texture_make(int w, int h, uint32_t* pixels);

// Сгенерировать в pixels-буфере шахматную доску w×h.
void         gx_texture_make_checker(gx_texture_t* out, int w, int h,
                                     uint32_t c1, uint32_t c2, int cell);

// Получить статистику контекста.
typedef struct {
    uint64_t draw_calls;
    uint64_t triangles_total;
    uint64_t triangles_culled;
    uint64_t triangles_drawn;
    uint64_t pixels_shaded;
} gx_stats_t;

gx_stats_t gx_get_stats(gx_context_t* ctx);
void       gx_reset_stats(gx_context_t* ctx);

// ----------------------------------------------------------------------------
// Сэмплинг текстуры (доступен фрагментным шейдерам).
// u, v в диапазоне [0..1] (за пределами — режим wrap).
// ----------------------------------------------------------------------------
uint32_t gx_sample_texture(gx_sampler_t s, float u, float v);

#endif // AOS_X86_64_GLES_H

// ============================================================================
// gles.c — программный GLES2-подобный рендерер AMPLIFY-OS (этап 44).
// ----------------------------------------------------------------------------
// Состоит из:
//
//   1. State machine: один глобальный `current` context.  gl_* функции
//      пишут в него (как в OpenGL: глобальный state, set-once-then-draw).
//
//   2. EGL-подобный API: aos_egl_create / make_current / destroy / swap.
//      Контекст владеет Z-буфером (если запросили).
//
//   3. Растеризатор треугольников:
//        - Vertex shader (через указатель на C-функцию).
//        - Clip-space → NDC (perspective divide) → screen-space viewport.
//        - Backface culling по знаку 2D-определителя.
//        - Edge-function триангуляция (Pineda 1988) с барицентриками.
//        - Перспективно-корректная интерполяция varying'ов через 1/w.
//        - Z-test против depth-buffer'а контекста.
//        - Fragment shader (через указатель на C-функцию).
//        - Optional α-blend src-over-dst (один режим — самый частый).
//
//   4. Sampler: gx_sample_texture с GL_NEAREST/GL_LINEAR и GL_REPEAT/
//      GL_CLAMP_TO_EDGE.
//
// Производительность.  Целевой FPS — 30+ при кубе 12 треугольников на
// 320×240 viewport'е.  На QEMU TCG это даёт ~250 µs/кадр — комфортно.
// Полноэкранный рендер (1024×768) уже медленнее (~30 мс на demo cube
// при текстуре).  Будущие оптимизации (SIMD-fragment-shader, tile-based
// rendering) — отдельный этап.
// ============================================================================

#include "gles.h"
#include "graphics.h"
#include "heap.h"
#include "serial.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// Глобальное состояние.  Один current на всё ядро — у нас единственный
// рендеринг-поток (WM main loop), так что race conditions не страшны.
// ----------------------------------------------------------------------------
static gx_context_t* g_current = 0;

// ----------------------------------------------------------------------------
// Утилиты.
// ----------------------------------------------------------------------------
static inline float clampf(float x, float lo, float hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

static inline int min_i(int a, int b) { return a < b ? a : b; }
static inline int max_i(int a, int b) { return a > b ? a : b; }

// ----------------------------------------------------------------------------
// EGL-подобный API.
// ----------------------------------------------------------------------------
gx_context_t* aos_egl_create(const aos_egl_config_t* cfg) {
    if (!cfg) return 0;
    const gfx_target_t* tgt = cfg->target;
    if (!tgt) {
        serial_puts("[gles] aos_egl_create: NULL target\n");
        return 0;
    }

    gx_context_t* ctx = (gx_context_t*)kmalloc(sizeof(gx_context_t));
    if (!ctx) {
        serial_puts("[gles] aos_egl_create: OOM ctx\n");
        return 0;
    }
    // Обнуляем всё.
    uint8_t* p = (uint8_t*)ctx;
    for (size_t i = 0; i < sizeof(gx_context_t); i++) p[i] = 0;

    ctx->target = tgt;
    ctx->vp_x = 0; ctx->vp_y = 0;
    ctx->vp_w = tgt->width; ctx->vp_h = tgt->height;
    ctx->clear_color = 0xFF000000;
    ctx->clear_depth = 1.0f;
    ctx->depth_test  = false;
    ctx->cull_face   = false;
    ctx->blend       = false;
    ctx->front_face  = GL_CCW;
    ctx->cull_mode   = GL_BACK;
    ctx->blend_src   = GL_SRC_ALPHA;
    ctx->blend_dst   = GL_ONE_MINUS_SRC_ALPHA;

    if (cfg->want_depth) {
        ctx->zbuf_w  = tgt->width;
        ctx->zbuf_h  = tgt->height;
        size_t n     = (size_t)ctx->zbuf_w * (size_t)ctx->zbuf_h;
        ctx->zbuffer = (float*)kmalloc(n * sizeof(float));
        if (!ctx->zbuffer) {
            serial_puts("[gles] aos_egl_create: OOM zbuf\n");
            kfree(ctx);
            return 0;
        }
        for (size_t i = 0; i < n; i++) ctx->zbuffer[i] = 1.0f;
    }

    serial_puts("[gles] context created: ");
    serial_print_dec((uint64_t)tgt->width); serial_puts("x");
    serial_print_dec((uint64_t)tgt->height);
    serial_puts(cfg->want_depth ? " +Z\n" : "\n");
    return ctx;
}

void aos_egl_destroy(gx_context_t* ctx) {
    if (!ctx) return;
    if (g_current == ctx) g_current = 0;
    if (ctx->zbuffer) kfree(ctx->zbuffer);
    kfree(ctx);
}

void aos_egl_make_current(gx_context_t* ctx) {
    g_current = ctx;
}

gx_context_t* aos_egl_current(void) {
    return g_current;
}

void aos_egl_swap_buffers(gx_context_t* ctx) {
    // У нас target — это offscreen-буфер окна, swap делает WM поверх
    // через compositor.  Никакого собственного swap'а нет.
    (void)ctx;
}

bool aos_egl_resize(gx_context_t* ctx, int new_w, int new_h) {
    if (!ctx) return false;
    if (new_w <= 0 || new_h <= 0) return false;
    if (ctx->zbuffer) {
        if (new_w == ctx->zbuf_w && new_h == ctx->zbuf_h) return true;
        kfree(ctx->zbuffer);
        ctx->zbuffer = 0;
    }
    size_t n = (size_t)new_w * (size_t)new_h;
    ctx->zbuffer = (float*)kmalloc(n * sizeof(float));
    if (!ctx->zbuffer) return false;
    ctx->zbuf_w = new_w;
    ctx->zbuf_h = new_h;
    for (size_t i = 0; i < n; i++) ctx->zbuffer[i] = 1.0f;
    ctx->vp_x = 0; ctx->vp_y = 0;
    ctx->vp_w = new_w; ctx->vp_h = new_h;
    return true;
}

// ----------------------------------------------------------------------------
// gl_* state functions.
// ----------------------------------------------------------------------------
#define CTX (g_current)

void gl_viewport(int x, int y, int w, int h) {
    if (!CTX) return;
    CTX->vp_x = x;
    CTX->vp_y = y;
    CTX->vp_w = w;
    CTX->vp_h = h;
}

void gl_clear_color(uint32_t argb) {
    if (!CTX) return;
    CTX->clear_color = argb;
}

void gl_clear_depth(float d) {
    if (!CTX) return;
    CTX->clear_depth = clampf(d, 0.0f, 1.0f);
}

void gl_clear(uint32_t mask) {
    if (!CTX || !CTX->target) return;
    const gfx_target_t* tgt = CTX->target;

    if (mask & GL_COLOR_BUFFER_BIT) {
        uint32_t c = CTX->clear_color;
        for (int y = 0; y < tgt->height; y++) {
            uint32_t* row = (uint32_t*)((uint8_t*)tgt->pixels + (size_t)y * tgt->pitch);
            for (int x = 0; x < tgt->width; x++) row[x] = c;
        }
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && CTX->zbuffer) {
        float d = CTX->clear_depth;
        size_t n = (size_t)CTX->zbuf_w * (size_t)CTX->zbuf_h;
        for (size_t i = 0; i < n; i++) CTX->zbuffer[i] = d;
    }
}

void gl_enable(uint32_t cap) {
    if (!CTX) return;
    switch (cap) {
        case GL_DEPTH_TEST: CTX->depth_test = true; break;
        case GL_CULL_FACE:  CTX->cull_face  = true; break;
        case GL_BLEND:      CTX->blend      = true; break;
        default: break;
    }
}

void gl_disable(uint32_t cap) {
    if (!CTX) return;
    switch (cap) {
        case GL_DEPTH_TEST: CTX->depth_test = false; break;
        case GL_CULL_FACE:  CTX->cull_face  = false; break;
        case GL_BLEND:      CTX->blend      = false; break;
        default: break;
    }
}

void gl_front_face(uint32_t mode) {
    if (!CTX) return;
    CTX->front_face = (mode == GL_CW) ? GL_CW : GL_CCW;
}

void gl_cull_face(uint32_t mode) {
    if (!CTX) return;
    CTX->cull_mode = (mode == GL_FRONT) ? GL_FRONT : GL_BACK;
}

void gl_blend_func(uint32_t src, uint32_t dst) {
    if (!CTX) return;
    CTX->blend_src = src;
    CTX->blend_dst = dst;
}

void gl_use_program(const gx_program_t* prog) {
    if (!CTX) return;
    CTX->program = prog;
}

void gl_set_uniforms(const void* u) {
    if (!CTX) return;
    CTX->uniforms = u;
}

void gl_bind_texture(gx_sampler_t tex) {
    if (!CTX) return;
    CTX->sampler0 = tex;
}

void gl_vertex_attrib_pointer(int slot, int size_floats, int type,
                              int stride_floats, const float* base) {
    if (!CTX) return;
    if (slot < 0 || slot >= GX_MAX_ATTRIBS) return;
    if (type != GL_FLOAT) return;
    gx_attrib_slot_t* a = &CTX->attribs[slot];
    a->base          = base;
    a->stride_floats = stride_floats;
    a->size_floats   = size_floats;
}

void gl_enable_vertex_attrib(int slot) {
    if (!CTX || slot < 0 || slot >= GX_MAX_ATTRIBS) return;
    CTX->attribs[slot].enabled = true;
}

void gl_disable_vertex_attrib(int slot) {
    if (!CTX || slot < 0 || slot >= GX_MAX_ATTRIBS) return;
    CTX->attribs[slot].enabled = false;
}

// ----------------------------------------------------------------------------
// Текстуры.
// ----------------------------------------------------------------------------
gx_texture_t gx_texture_make(int w, int h, uint32_t* pixels) {
    gx_texture_t t;
    t.width      = w;
    t.height     = h;
    t.pixels     = pixels;
    t.wrap_s     = GL_REPEAT;
    t.wrap_t     = GL_REPEAT;
    t.min_filter = GL_NEAREST;
    t.mag_filter = GL_LINEAR;
    return t;
}

void gx_texture_make_checker(gx_texture_t* out, int w, int h,
                             uint32_t c1, uint32_t c2, int cell) {
    if (!out || !out->pixels) return;
    out->width  = w;
    out->height = h;
    if (cell < 1) cell = 1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int cx = x / cell;
            int cy = y / cell;
            out->pixels[y * w + x] = ((cx ^ cy) & 1) ? c1 : c2;
        }
    }
}

uint32_t gx_sample_texture(gx_sampler_t s, float u, float v) {
    if (!s || !s->pixels) return 0xFF00FFFFu;     // magenta-ish "missing texture"

    // Wrap S.
    if (s->wrap_s == GL_REPEAT) {
        u = u - (float)((int)u);     // [0..1) для положительных
        if (u < 0.0f) u += 1.0f;
    } else {
        u = clampf(u, 0.0f, 0.999999f);
    }
    // Wrap T.
    if (s->wrap_t == GL_REPEAT) {
        v = v - (float)((int)v);
        if (v < 0.0f) v += 1.0f;
    } else {
        v = clampf(v, 0.0f, 0.999999f);
    }

    if (s->mag_filter == GL_LINEAR) {
        // Bilinear: 4 texel'а.
        float fx = u * (float)s->width  - 0.5f;
        float fy = v * (float)s->height - 0.5f;
        int   x0 = (int)fx; if (x0 < 0) x0 = 0;
        int   y0 = (int)fy; if (y0 < 0) y0 = 0;
        int   x1 = x0 + 1; if (x1 >= s->width)  x1 = s->width  - 1;
        int   y1 = y0 + 1; if (y1 >= s->height) y1 = s->height - 1;
        float dx = fx - (float)x0;
        float dy = fy - (float)y0;
        uint32_t c00 = s->pixels[y0 * s->width + x0];
        uint32_t c10 = s->pixels[y0 * s->width + x1];
        uint32_t c01 = s->pixels[y1 * s->width + x0];
        uint32_t c11 = s->pixels[y1 * s->width + x1];
        // По-канальная интерполяция.
        float a = (float)((c00 >> 24) & 0xFF) * (1-dx)*(1-dy)
                + (float)((c10 >> 24) & 0xFF) *    dx *(1-dy)
                + (float)((c01 >> 24) & 0xFF) * (1-dx)*   dy
                + (float)((c11 >> 24) & 0xFF) *    dx *   dy;
        float r = (float)((c00 >> 16) & 0xFF) * (1-dx)*(1-dy)
                + (float)((c10 >> 16) & 0xFF) *    dx *(1-dy)
                + (float)((c01 >> 16) & 0xFF) * (1-dx)*   dy
                + (float)((c11 >> 16) & 0xFF) *    dx *   dy;
        float g = (float)((c00 >>  8) & 0xFF) * (1-dx)*(1-dy)
                + (float)((c10 >>  8) & 0xFF) *    dx *(1-dy)
                + (float)((c01 >>  8) & 0xFF) * (1-dx)*   dy
                + (float)((c11 >>  8) & 0xFF) *    dx *   dy;
        float b = (float)( c00        & 0xFF) * (1-dx)*(1-dy)
                + (float)( c10        & 0xFF) *    dx *(1-dy)
                + (float)( c01        & 0xFF) * (1-dx)*   dy
                + (float)( c11        & 0xFF) *    dx *   dy;
        uint32_t ai = (uint32_t)a; if (ai > 255) ai = 255;
        uint32_t ri = (uint32_t)r; if (ri > 255) ri = 255;
        uint32_t gi = (uint32_t)g; if (gi > 255) gi = 255;
        uint32_t bi = (uint32_t)b; if (bi > 255) bi = 255;
        return (ai << 24) | (ri << 16) | (gi << 8) | bi;
    }

    // GL_NEAREST.
    int x = (int)(u * (float)s->width);
    int y = (int)(v * (float)s->height);
    if (x < 0) x = 0; if (x >= s->width)  x = s->width  - 1;
    if (y < 0) y = 0; if (y >= s->height) y = s->height - 1;
    return s->pixels[y * s->width + x];
}

// ----------------------------------------------------------------------------
// Сбор атрибутов одной вершины в плоский массив float'ов.
// Возвращает количество float'ов, скопированных в out.
// ----------------------------------------------------------------------------
static int gather_attribs(int vertex_index, float* out, int max_out) {
    int written = 0;
    for (int slot = 0; slot < GX_MAX_ATTRIBS; slot++) {
        const gx_attrib_slot_t* a = &CTX->attribs[slot];
        if (!a->enabled || !a->base || a->size_floats <= 0) continue;
        const float* src = a->base + (size_t)vertex_index * (size_t)a->stride_floats;
        for (int j = 0; j < a->size_floats; j++) {
            if (written >= max_out) return written;
            out[written++] = src[j];
        }
    }
    return written;
}

// ----------------------------------------------------------------------------
// Состояние одной обработанной вершины: clip-space позиция + varyings.
// ----------------------------------------------------------------------------
typedef struct {
    gx_vec4_t pos;                       // clip-space
    float     screen_x, screen_y;        // после perspective divide + viewport
    float     screen_z;                  // [0..1]
    float     inv_w;                     // 1 / clip.w (для perspective-correct interp)
    float     varying[GX_MAX_VARYINGS];  // pre-divided varyings (= varying * inv_w)
    int       n_varying;
    bool      visible;                   // false → за near или клиппирован простой
} processed_vertex_t;

// ----------------------------------------------------------------------------
// Преобразование одной вершины: VS → screen-space.
// Очень упрощённый клипинг: если w <= 0 либо z вне [-w..w] — visible=false
// (мы отбрасываем треугольник целиком).  Это "near plane клиппинг через
// reject" — самое грубое, что бывает; но для куба в центре сцены этого
// достаточно.  Полноценный clip-triangulation — будущее.
// ----------------------------------------------------------------------------
static void process_vertex(int vidx, processed_vertex_t* out) {
    float attrib_buf[GX_MAX_ATTRIBS * 4];
    int   n_attr = gather_attribs(vidx, attrib_buf, sizeof(attrib_buf)/sizeof(float));
    (void)n_attr;

    float vary_buf[GX_MAX_VARYINGS];
    for (int i = 0; i < GX_MAX_VARYINGS; i++) vary_buf[i] = 0.0f;

    gx_vec4_t pos;
    CTX->program->vs(attrib_buf, CTX->uniforms, &pos, vary_buf);
    out->pos       = pos;
    out->n_varying = CTX->program->n_varying_floats;
    if (out->n_varying > GX_MAX_VARYINGS) out->n_varying = GX_MAX_VARYINGS;

    // Простой rejection-клипинг.
    if (pos.w <= 0.001f) { out->visible = false; return; }
    if (pos.z < -pos.w || pos.z > pos.w) { out->visible = false; return; }
    if (pos.x < -pos.w * 1.5f || pos.x > pos.w * 1.5f) { out->visible = false; return; }
    if (pos.y < -pos.w * 1.5f || pos.y > pos.w * 1.5f) { out->visible = false; return; }

    float inv_w = 1.0f / pos.w;
    float ndc_x = pos.x * inv_w;          // [-1..1]
    float ndc_y = pos.y * inv_w;
    float ndc_z = pos.z * inv_w;

    // Viewport-transform.
    out->screen_x = (float)CTX->vp_x + (ndc_x * 0.5f + 0.5f) * (float)CTX->vp_w;
    // OpenGL: +Y вверх. Наш framebuffer: +Y вниз. Инвертируем.
    out->screen_y = (float)CTX->vp_y + (1.0f - (ndc_y * 0.5f + 0.5f)) * (float)CTX->vp_h;
    out->screen_z = ndc_z * 0.5f + 0.5f;   // [0..1]

    out->inv_w = inv_w;
    // Pre-multiply varying'ов на 1/w для perspective-correct interpolation.
    for (int i = 0; i < out->n_varying; i++) {
        out->varying[i] = vary_buf[i] * inv_w;
    }
    out->visible = true;
}

// ----------------------------------------------------------------------------
// Помощник: ARGB color blend (для GL_BLEND, src-over-dst).
// ----------------------------------------------------------------------------
static inline uint32_t blend_argb(uint32_t dst, uint32_t src,
                                  int src_factor, int dst_factor) {
    uint32_t sa = (src >> 24) & 0xFF;
    if (src_factor == GL_ONE && dst_factor == GL_ZERO) return src;

    uint32_t sr = (src >> 16) & 0xFF;
    uint32_t sg = (src >>  8) & 0xFF;
    uint32_t sb =  src        & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF;
    uint32_t dg = (dst >>  8) & 0xFF;
    uint32_t db =  dst        & 0xFF;

    // Src factor.
    uint32_t fsr, fsg, fsb;
    if (src_factor == GL_SRC_ALPHA) {
        fsr = (sr * sa) / 255;
        fsg = (sg * sa) / 255;
        fsb = (sb * sa) / 255;
    } else if (src_factor == GL_ONE) {
        fsr = sr; fsg = sg; fsb = sb;
    } else { /* GL_ZERO */
        fsr = 0; fsg = 0; fsb = 0;
    }

    // Dst factor.
    uint32_t fdr, fdg, fdb;
    if (dst_factor == GL_ONE_MINUS_SRC_ALPHA) {
        uint32_t inv = 255 - sa;
        fdr = (dr * inv) / 255;
        fdg = (dg * inv) / 255;
        fdb = (db * inv) / 255;
    } else if (dst_factor == GL_ONE) {
        fdr = dr; fdg = dg; fdb = db;
    } else { /* GL_ZERO */
        fdr = 0; fdg = 0; fdb = 0;
    }

    uint32_t r = fsr + fdr; if (r > 255) r = 255;
    uint32_t g = fsg + fdg; if (g > 255) g = 255;
    uint32_t b = fsb + fdb; if (b > 255) b = 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

// ----------------------------------------------------------------------------
// Растеризация одного треугольника.  Алгоритм: edge functions Pineda 1988.
//
//   Для трёх вершин (v0, v1, v2) определяем ориентированную площадь
//   area = (v1 - v0) × (v2 - v0).  Если |area| ≈ 0 — вырожденный.
//   Если area * front_face_sign < 0 — backface, cull (если cull_face on).
//
//   Для каждого пикселя (px, py) в bounding box:
//     w0 = edge(v1, v2, p)
//     w1 = edge(v2, v0, p)
//     w2 = edge(v0, v1, p)
//   Если все одного знака — внутри.  Барицентрики = (w0, w1, w2) / area.
//
// Perspective-correct interp:
//   b'_i = b_i * inv_w_i / sum(b_j * inv_w_j)
//   varying_at_pixel = sum(b'_i * varying_i)
//
// Z-test: linear interpolation screen_z (which is already in [0..1]).
// Это не perspective-correct для Z, но Z и так пишется в depth-buffer
// после divide, и линейная интерполяция в screen-space ≈ корректна.
// ----------------------------------------------------------------------------
static inline float edge_fn(float x0, float y0,
                            float x1, float y1,
                            float xp, float yp) {
    return (x1 - x0) * (yp - y0) - (y1 - y0) * (xp - x0);
}

// ============================================================================
// [HOTFIX 20.05.2026 PERF] Растеризация с инкрементальным шагом edge-functions
// и линейной интерполяции. Узкое место исходной версии (после профилирования
// 3D-куба на 15 FPS) — per-pixel пересчёт edge_fn (6 mul + 6 sub × 3 = 24 ops)
// и numerator варингов (3 mul + 2 add × N varying = ~30 ops для N=6). Поскольку
// w0/w1/w2 ЛИНЕЙНЫ по (px, py), их можно проинициализировать в углу bounding
// box и просто прибавлять step_x на пиксель + step_y на строку. То же самое
// работает для z (линейная интерполяция в screen-space), interp_inv_w и
// числителей варингов = inv_area * (w0*var0 + w1*var1 + w2*var2). Per-pixel
// работа сводится к ~15 ops + FS-вызов вместо ~60 ops + FS.
// Z-тест поднят перед расчётом w_pix и варингов (early-Z) — экономит
// деление + N mul'ов на каждый occlude'нутый пиксель.
// ============================================================================
static void rasterize_triangle(const processed_vertex_t* v0,
                               const processed_vertex_t* v1,
                               const processed_vertex_t* v2) {
    if (!v0->visible || !v1->visible || !v2->visible) {
        CTX->triangles_culled++;
        return;
    }

    float area = edge_fn(v0->screen_x, v0->screen_y,
                         v1->screen_x, v1->screen_y,
                         v2->screen_x, v2->screen_y);
    if (gx_fabsf(area) < 0.5f) {
        CTX->triangles_culled++;
        return;     // вырожденный треугольник
    }

    // Backface culling. Знак area соответствует ориентации треугольника
    // в screen-space. У нас экранный Y инвертирован относительно OpenGL,
    // поэтому "CCW в clip-space" даёт отрицательную area в screen-space.
    if (CTX->cull_face) {
        bool front_is_negative = (CTX->front_face == GL_CCW);
        bool is_back = front_is_negative ? (area > 0.0f) : (area < 0.0f);
        if (is_back) {
            if (CTX->cull_mode == GL_BACK)  { CTX->triangles_culled++; return; }
        } else {
            if (CTX->cull_mode == GL_FRONT) { CTX->triangles_culled++; return; }
        }
    }

    // Bounding box (clipped to viewport + target).
    int minx = (int)(v0->screen_x < v1->screen_x ?
                     (v0->screen_x < v2->screen_x ? v0->screen_x : v2->screen_x) :
                     (v1->screen_x < v2->screen_x ? v1->screen_x : v2->screen_x));
    int maxx = (int)(v0->screen_x > v1->screen_x ?
                     (v0->screen_x > v2->screen_x ? v0->screen_x : v2->screen_x) :
                     (v1->screen_x > v2->screen_x ? v1->screen_x : v2->screen_x)) + 1;
    int miny = (int)(v0->screen_y < v1->screen_y ?
                     (v0->screen_y < v2->screen_y ? v0->screen_y : v2->screen_y) :
                     (v1->screen_y < v2->screen_y ? v1->screen_y : v2->screen_y));
    int maxy = (int)(v0->screen_y > v1->screen_y ?
                     (v0->screen_y > v2->screen_y ? v0->screen_y : v2->screen_y) :
                     (v1->screen_y > v2->screen_y ? v1->screen_y : v2->screen_y)) + 1;

    int vp_r = CTX->vp_x + CTX->vp_w;
    int vp_b = CTX->vp_y + CTX->vp_h;
    minx = max_i(minx, CTX->vp_x);  miny = max_i(miny, CTX->vp_y);
    maxx = min_i(maxx, vp_r);       maxy = min_i(maxy, vp_b);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > CTX->target->width)  maxx = CTX->target->width;
    if (maxy > CTX->target->height) maxy = CTX->target->height;
    if (minx >= maxx || miny >= maxy) {
        CTX->triangles_culled++;
        return;
    }

    float inv_area = 1.0f / area;

    int n_var = v0->n_varying;
    if (n_var > GX_MAX_VARYINGS) n_var = GX_MAX_VARYINGS;

    // ── Шаги edge-functions ──
    // edge(a, b, p) = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)
    //   d/dpx = -(b.y - a.y),  d/dpy = (b.x - a.x).
    //
    // w0 = edge(v1, v2, p), w1 = edge(v2, v0, p), w2 = edge(v0, v1, p).
    float w0_step_x = -(v2->screen_y - v1->screen_y);
    float w0_step_y =  (v2->screen_x - v1->screen_x);
    float w1_step_x = -(v0->screen_y - v2->screen_y);
    float w1_step_y =  (v0->screen_x - v2->screen_x);
    float w2_step_x = -(v1->screen_y - v0->screen_y);
    float w2_step_y =  (v1->screen_x - v0->screen_x);

    // Стартовые значения в центре пикселя (minx + 0.5, miny + 0.5).
    float start_fx = (float)minx + 0.5f;
    float start_fy = (float)miny + 0.5f;
    float w0_row = edge_fn(v1->screen_x, v1->screen_y,
                           v2->screen_x, v2->screen_y, start_fx, start_fy);
    float w1_row = edge_fn(v2->screen_x, v2->screen_y,
                           v0->screen_x, v0->screen_y, start_fx, start_fy);
    float w2_row = edge_fn(v0->screen_x, v0->screen_y,
                           v1->screen_x, v1->screen_y, start_fx, start_fy);

    // ── Линейные шаги для интерполированных величин ──
    // z = inv_area * (w0*z0 + w1*z1 + w2*z2). Линеен по px,py — тоже инкрементируется.
    float z0 = v0->screen_z, z1 = v1->screen_z, z2 = v2->screen_z;
    float z_step_x = inv_area * (w0_step_x * z0 + w1_step_x * z1 + w2_step_x * z2);
    float z_step_y = inv_area * (w0_step_y * z0 + w1_step_y * z1 + w2_step_y * z2);
    float z_row    = inv_area * (w0_row    * z0 + w1_row    * z1 + w2_row    * z2);

    // interp_inv_w = inv_area * (w0*iw0 + w1*iw1 + w2*iw2). Тоже линеен.
    float iw0 = v0->inv_w, iw1 = v1->inv_w, iw2 = v2->inv_w;
    float iw_step_x = inv_area * (w0_step_x * iw0 + w1_step_x * iw1 + w2_step_x * iw2);
    float iw_step_y = inv_area * (w0_step_y * iw0 + w1_step_y * iw1 + w2_step_y * iw2);
    float iw_row    = inv_area * (w0_row    * iw0 + w1_row    * iw1 + w2_row    * iw2);

    // Numerators варингов (pre-multiplied на inv_w в process_vertex).
    // numer[i] = inv_area * (w0*v0.var[i] + w1*v1.var[i] + w2*v2.var[i]).
    // varying_at_pixel[i] = numer[i] / interp_inv_w.
    float vn_step_x[GX_MAX_VARYINGS];
    float vn_step_y[GX_MAX_VARYINGS];
    float vn_row   [GX_MAX_VARYINGS];
    for (int i = 0; i < n_var; i++) {
        float a = v0->varying[i];
        float b = v1->varying[i];
        float c = v2->varying[i];
        vn_step_x[i] = inv_area * (w0_step_x * a + w1_step_x * b + w2_step_x * c);
        vn_step_y[i] = inv_area * (w0_step_y * a + w1_step_y * b + w2_step_y * c);
        vn_row[i]    = inv_area * (w0_row    * a + w1_row    * b + w2_row    * c);
    }

    bool area_positive = (area > 0.0f);
    int  pixels_drawn  = 0;

    for (int py = miny; py < maxy; py++) {
        float w0 = w0_row;
        float w1 = w1_row;
        float w2 = w2_row;
        float z  = z_row;
        float iw = iw_row;
        float vn[GX_MAX_VARYINGS];
        for (int i = 0; i < n_var; i++) vn[i] = vn_row[i];

        uint32_t* row = (uint32_t*)((uint8_t*)CTX->target->pixels +
                                    (size_t)py * (size_t)CTX->target->pitch);
        float* zrow = CTX->zbuffer ?
                      (CTX->zbuffer + (size_t)py * (size_t)CTX->zbuf_w) : 0;

        for (int px = minx; px < maxx; px++) {
            // Inside-test: один знак с area.
            bool inside = area_positive ? (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
                                        : (w0 <= 0.0f && w1 <= 0.0f && w2 <= 0.0f);
            // Early Z-test и Z-границы. Делаются ДО perspective recovery, потому что
            // div + N mul'ов на occlude'нутый пиксель — самое дорогое, что у нас есть.
            if (inside && z >= 0.0f && z <= 1.0f) {
                bool zpass = true;
                if (CTX->depth_test && zrow && z >= zrow[px]) zpass = false;

                if (zpass && iw > 1e-20f) {
                    float w_pix = 1.0f / iw;
                    float varying_at_pixel[GX_MAX_VARYINGS];
                    for (int i = 0; i < n_var; i++) {
                        varying_at_pixel[i] = vn[i] * w_pix;
                    }

                    uint32_t out_color = 0xFFFF00FFu;     // bright pink fallback
                    CTX->program->fs(varying_at_pixel, CTX->uniforms,
                                     CTX->sampler0, &out_color);

                    if (CTX->blend) {
                        out_color = blend_argb(row[px], out_color,
                                               CTX->blend_src, CTX->blend_dst);
                    }
                    row[px] = out_color;
                    if (zrow) zrow[px] = z;
                    pixels_drawn++;
                }
            }

            // Инкрементальный шаг по px.
            w0 += w0_step_x;
            w1 += w1_step_x;
            w2 += w2_step_x;
            z  += z_step_x;
            iw += iw_step_x;
            for (int i = 0; i < n_var; i++) vn[i] += vn_step_x[i];
        }

        // Шаг по py.
        w0_row += w0_step_y;
        w1_row += w1_step_y;
        w2_row += w2_step_y;
        z_row  += z_step_y;
        iw_row += iw_step_y;
        for (int i = 0; i < n_var; i++) vn_row[i] += vn_step_y[i];
    }

    CTX->pixels_shaded += (uint64_t)pixels_drawn;
    CTX->triangles_drawn++;
}

// ----------------------------------------------------------------------------
// gl_draw_arrays — основная draw-call функция.
// ----------------------------------------------------------------------------
void gl_draw_arrays(uint32_t mode, int first, int count) {
    if (!CTX || !CTX->program || !CTX->target) return;
    if (count <= 0) return;
    CTX->draw_calls++;

    if (mode == GL_TRIANGLES) {
        int n_tris = count / 3;
        for (int t = 0; t < n_tris; t++) {
            processed_vertex_t v0, v1, v2;
            process_vertex(first + t*3 + 0, &v0);
            process_vertex(first + t*3 + 1, &v1);
            process_vertex(first + t*3 + 2, &v2);
            CTX->triangles_total++;
            rasterize_triangle(&v0, &v1, &v2);
        }
        return;
    }
    if (mode == GL_TRIANGLE_STRIP) {
        if (count < 3) return;
        processed_vertex_t v[3];
        process_vertex(first + 0, &v[0]);
        process_vertex(first + 1, &v[1]);
        for (int i = 2; i < count; i++) {
            process_vertex(first + i, &v[2]);
            CTX->triangles_total++;
            // Чередуем порядок для сохранения front-face ориентации.
            if (i & 1) {
                rasterize_triangle(&v[1], &v[0], &v[2]);
            } else {
                rasterize_triangle(&v[0], &v[1], &v[2]);
            }
            // Скользящее окно.
            v[0] = v[1];
            v[1] = v[2];
        }
        return;
    }
    // Других режимов не поддерживаем (GL_LINES/POINTS — будущее).
}

void gl_draw_elements(uint32_t mode, int count, const uint16_t* indices) {
    if (!CTX || !CTX->program || !CTX->target || !indices) return;
    if (count <= 0) return;
    CTX->draw_calls++;

    if (mode == GL_TRIANGLES) {
        int n_tris = count / 3;
        for (int t = 0; t < n_tris; t++) {
            processed_vertex_t v0, v1, v2;
            process_vertex(indices[t*3 + 0], &v0);
            process_vertex(indices[t*3 + 1], &v1);
            process_vertex(indices[t*3 + 2], &v2);
            CTX->triangles_total++;
            rasterize_triangle(&v0, &v1, &v2);
        }
        return;
    }
}

// ----------------------------------------------------------------------------
// Статистика.
// ----------------------------------------------------------------------------
gx_stats_t gx_get_stats(gx_context_t* ctx) {
    gx_stats_t s = {0, 0, 0, 0, 0};
    if (!ctx) return s;
    s.draw_calls       = ctx->draw_calls;
    s.triangles_total  = ctx->triangles_total;
    s.triangles_culled = ctx->triangles_culled;
    s.triangles_drawn  = ctx->triangles_drawn;
    s.pixels_shaded    = ctx->pixels_shaded;
    return s;
}

void gx_reset_stats(gx_context_t* ctx) {
    if (!ctx) return;
    ctx->draw_calls       = 0;
    ctx->triangles_total  = 0;
    ctx->triangles_culled = 0;
    ctx->triangles_drawn  = 0;
    ctx->pixels_shaded    = 0;
}

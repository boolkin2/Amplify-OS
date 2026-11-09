// ============================================================================
// app_glcube.c — приложение «3D куб» для AMPLIFY-OS (этап 44).
// ----------------------------------------------------------------------------
// Демонстрационный апп, который доказывает работу 3D-стека:
//   * Окно 360×280 с собственным GLES-контекстом.
//   * Текстурированный куб (6 граней × 2 треугольника = 12 треугольников).
//   * Каждый кадр поворачивается вокруг осей Y и X, обновляются uniforms.
//   * Перспективная проекция + lookAt-камера.
//   * Z-буфер (правильное перекрытие граней).
//   * Backface culling.
//   * Шахматная процедурная текстура 64×64.
//   * Простое освещение по нормали в фрагменте (lambert).
//   * Внизу окна — счётчик FPS + статистика рендера (треугольники,
//     пиксели, draw-calls).
//
// Управление:
//   ESC закрывает окно (стандартное поведение close-button).
//   Любой клавишный ввод — переключает wireframe-флаг (заготовка на будущее,
//   фактически просто меняет цвет грани).
// ============================================================================

#include "apps.h"
#include "wm.h"
#include "heap.h"
#include "graphics.h"
#include "rtc.h"
#include "timer.h"
#include "serial.h"
#include "gles.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ----------------------------------------------------------------------------
// Геометрия куба.
//   Каждая вершина: position(3) + normal(3) + uv(2)  = 8 float'ов.
//   6 граней × 4 вершины = 24 вершины.
//   12 треугольников (по 3 индекса) = 36 индексов.
// ----------------------------------------------------------------------------
#define VTX_STRIDE_FLOATS  8

static const float g_cube_verts[24 * VTX_STRIDE_FLOATS] = {
    //  pos               normal           uv
    // ---- +X face -----
    +0.5f,-0.5f,-0.5f,  +1, 0, 0,   0,0,
    +0.5f,+0.5f,-0.5f,  +1, 0, 0,   1,0,
    +0.5f,+0.5f,+0.5f,  +1, 0, 0,   1,1,
    +0.5f,-0.5f,+0.5f,  +1, 0, 0,   0,1,
    // ---- -X face -----
    -0.5f,-0.5f,+0.5f,  -1, 0, 0,   0,0,
    -0.5f,+0.5f,+0.5f,  -1, 0, 0,   1,0,
    -0.5f,+0.5f,-0.5f,  -1, 0, 0,   1,1,
    -0.5f,-0.5f,-0.5f,  -1, 0, 0,   0,1,
    // ---- +Y face -----
    -0.5f,+0.5f,-0.5f,   0,+1, 0,   0,0,
    -0.5f,+0.5f,+0.5f,   0,+1, 0,   1,0,
    +0.5f,+0.5f,+0.5f,   0,+1, 0,   1,1,
    +0.5f,+0.5f,-0.5f,   0,+1, 0,   0,1,
    // ---- -Y face -----
    -0.5f,-0.5f,+0.5f,   0,-1, 0,   0,0,
    -0.5f,-0.5f,-0.5f,   0,-1, 0,   1,0,
    +0.5f,-0.5f,-0.5f,   0,-1, 0,   1,1,
    +0.5f,-0.5f,+0.5f,   0,-1, 0,   0,1,
    // ---- +Z face -----
    +0.5f,-0.5f,+0.5f,   0, 0,+1,   0,0,
    +0.5f,+0.5f,+0.5f,   0, 0,+1,   1,0,
    -0.5f,+0.5f,+0.5f,   0, 0,+1,   1,1,
    -0.5f,-0.5f,+0.5f,   0, 0,+1,   0,1,
    // ---- -Z face -----
    -0.5f,-0.5f,-0.5f,   0, 0,-1,   0,0,
    -0.5f,+0.5f,-0.5f,   0, 0,-1,   1,0,
    +0.5f,+0.5f,-0.5f,   0, 0,-1,   1,1,
    +0.5f,-0.5f,-0.5f,   0, 0,-1,   0,1,
};

static const uint16_t g_cube_indices[36] = {
    // CCW-ориентация при взгляде с внешней стороны → видны как фронт.
    0, 1, 2,   0, 2, 3,
    4, 5, 6,   4, 6, 7,
    8, 9,10,   8,10,11,
   12,13,14,  12,14,15,
   16,17,18,  16,18,19,
   20,21,22,  20,22,23,
};

// ----------------------------------------------------------------------------
// Uniforms.
// ----------------------------------------------------------------------------
typedef struct {
    gx_mat4_t mvp;        // model-view-projection
    gx_mat4_t model;      // только модель (для трансформации нормалей)
    gx_vec3_t light_dir;  // нормализованный, в world-space
} cube_uniforms_t;

// ----------------------------------------------------------------------------
// Vertex shader: position(3) + normal(3) + uv(2) → gl_Position + varying[6]:
//   varying[0..2] = world-space normal
//   varying[3..4] = uv
//   varying[5]    = depth-fog (по w) — для лёгкого эффекта.
// ----------------------------------------------------------------------------
static void cube_vs(const float* attribs, const void* uniforms,
                    gx_vec4_t* gl_Position, float* varying) {
    const cube_uniforms_t* u = (const cube_uniforms_t*)uniforms;

    gx_vec4_t pos4 = { attribs[0], attribs[1], attribs[2], 1.0f };
    gx_mat4_mul_vec4(gl_Position, &u->mvp, &pos4);

    // Нормаль (без учёта translation: w=0).
    gx_vec4_t n4   = { attribs[3], attribs[4], attribs[5], 0.0f };
    gx_vec4_t nw;
    gx_mat4_mul_vec4(&nw, &u->model, &n4);
    // Нормализуем в world-space.
    gx_vec3_t nn = { nw.x, nw.y, nw.z };
    gx_vec3_normalize(&nn);
    varying[0] = nn.x;
    varying[1] = nn.y;
    varying[2] = nn.z;

    // UV.
    varying[3] = attribs[6];
    varying[4] = attribs[7];

    // Fog-факт: будем затемнять с глубиной.
    varying[5] = gl_Position->w;
}

// ----------------------------------------------------------------------------
// Fragment shader: lambert × шахматная текстура × лёгкая дымка по глубине.
// ----------------------------------------------------------------------------
static inline uint32_t pack_argb(int a, int r, int g, int b) {
    if (a < 0) a = 0; if (a > 255) a = 255;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return ((uint32_t)a << 24) | ((uint32_t)r << 16)
         | ((uint32_t)g <<  8) |  (uint32_t)b;
}

static void cube_fs(const float* varying, const void* uniforms,
                    gx_sampler_t sampler0, uint32_t* gl_FragColor) {
    const cube_uniforms_t* u = (const cube_uniforms_t*)uniforms;

    // Сэмплим текстуру (теперь GL_NEAREST — см. комментарий в ensure_context).
    uint32_t tex = gx_sample_texture(sampler0, varying[3], varying[4]);

    // Lambert (диффуз × нормаль). Без gx_vec3_t (избегаем лишний struct copy).
    float ndotl = varying[0] * u->light_dir.x
                + varying[1] * u->light_dir.y
                + varying[2] * u->light_dir.z;
    if (ndotl < 0.0f) ndotl = 0.0f;
    // Ambient 0.25 + diffuse 0.75 → диапазон [0.25 .. 1.0].
    float lit = 0.25f + 0.75f * ndotl;

    // Fog по camera-space w. Замена `/5.0f` на `*0.2f` экономит div per-pixel.
    float fog = (10.0f - varying[5]) * 0.2f;
    if (fog < 0.0f) fog = 0.0f;
    if (fog > 1.0f) fog = 1.0f;

    float k = lit * fog;
    int tr = (int)((float)((tex >> 16) & 0xFF) * k);
    int tg = (int)((float)((tex >>  8) & 0xFF) * k);
    int tb = (int)((float)( tex        & 0xFF) * k);

    *gl_FragColor = pack_argb(255, tr, tg, tb);
}

static const gx_program_t g_cube_program = {
    .vs               = cube_vs,
    .fs               = cube_fs,
    .n_attrib_floats  = VTX_STRIDE_FLOATS,
    .n_varying_floats = 6,
};

// ----------------------------------------------------------------------------
// Состояние приложения (живёт в win->user_data).
// ----------------------------------------------------------------------------
typedef struct {
    gx_context_t* gl;
    cube_uniforms_t  uniforms;
    gx_texture_t  texture;
    uint32_t*     tex_pixels;        // отдельная аллокация (64×64×4 = 16 KB)
    int           tex_w, tex_h;

    // Состояние анимации.
    float    angle_y;
    float    angle_x;
    uint64_t last_tick_ms;

    // FPS-измерение.
    uint64_t fps_window_start_ms;
    int      frames_in_window;
    int      last_fps;

    // Последняя статистика.
    gx_stats_t last_stats;
} cube_app_state_t;

// ----------------------------------------------------------------------------
// Helper: вывод строки + числа на оверлей.
// ----------------------------------------------------------------------------
static void utoa(uint64_t v, char* buf) {
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    char tmp[24]; int t = 0;
    while (v) { tmp[t++] = '0' + (v % 10); v /= 10; }
    int n = 0;
    while (t--) buf[n++] = tmp[t];
    buf[n] = 0;
}

static int strn(const char* s) { int n = 0; while (s[n]) n++; return n; }

// ----------------------------------------------------------------------------
// Создание контекста: вызывается лениво на первом on_draw, потому что
// размер window->target доступен только в render-time (хотя в нашей
// реализации он фиксирован при window_create — но всё равно).
// ----------------------------------------------------------------------------
static bool ensure_context(cube_app_state_t* st, const gfx_target_t* tgt) {
    if (st->gl) return true;

    aos_egl_config_t cfg = { .target = tgt, .want_depth = true };
    st->gl = aos_egl_create(&cfg);
    if (!st->gl) {
        serial_puts("[glcube] aos_egl_create failed\n");
        return false;
    }

    // Текстура 64×64 шахматная.
    st->tex_w = 64;
    st->tex_h = 64;
    st->tex_pixels = (uint32_t*)kmalloc((size_t)st->tex_w * (size_t)st->tex_h * 4);
    if (!st->tex_pixels) {
        serial_puts("[glcube] tex kmalloc failed\n");
        return false;
    }
    st->texture = gx_texture_make(st->tex_w, st->tex_h, st->tex_pixels);
    // [HOTFIX 20.05.2026 PERF] GL_LINEAR делает биленейный сэмплинг по 4 текселям
    // на пиксель = +64 операции на каждый пиксель куба. На 1080p FullHD c
    // программным рендером это даёт лаг. Шахматный паттерн + Lambert + fog
    // визуально маскируют разницу между NEAREST и LINEAR, разница в качестве
    // на 64×64 текстуре куба практически невидима.
    st->texture.min_filter = GL_NEAREST;
    st->texture.mag_filter = GL_NEAREST;
    st->texture.wrap_s     = GL_REPEAT;
    st->texture.wrap_t     = GL_REPEAT;
    // Тёплая палитра в стилистике AMPLIFY-OS: оранжевый + золотой.
    gx_texture_make_checker(&st->texture, st->tex_w, st->tex_h,
                            0xFFF5A037, 0xFFFFD200, 8);

    return true;
}

// ----------------------------------------------------------------------------
// Основной on_draw.
// ----------------------------------------------------------------------------
static void cube_on_draw(window_t* w, const gfx_target_t* tgt) {
    cube_app_state_t* st = (cube_app_state_t*)w->user_data;
    if (!st) return;

    if (!ensure_context(st, tgt)) {
        gfx_clear(tgt, 0xFF400000);
        gfx_draw_string(tgt, 10, 10, "GL init FAILED — see serial",
                        0xFFFFFFFF, 0xFF400000);
        return;
    }

    aos_egl_make_current(st->gl);

    // Обновляем угол поворота.
    uint64_t now = timer_get_uptime_ms();
    if (st->last_tick_ms == 0) st->last_tick_ms = now;
    uint64_t dt_ms = now - st->last_tick_ms;
    st->last_tick_ms = now;
    float dt = (float)dt_ms / 1000.0f;
    if (dt > 0.1f) dt = 0.1f;             // защита от больших скачков
    st->angle_y += dt * 0.9f;             // ~0.9 rad/sec ≈ 51 °/sec
    st->angle_x += dt * 0.5f;

    // Считаем uniforms.
    gx_mat4_t mat_rot_y, mat_rot_x, mat_model, mat_view, mat_proj, mat_mv, mat_mvp;
    gx_mat4_rotate_y(&mat_rot_y, st->angle_y);
    gx_mat4_rotate_x(&mat_rot_x, st->angle_x);
    gx_mat4_mul(&mat_model, &mat_rot_y, &mat_rot_x);

    gx_vec3_t eye    = { 0.0f, 0.0f, 3.0f };
    gx_vec3_t center = { 0.0f, 0.0f, 0.0f };
    gx_vec3_t up     = { 0.0f, 1.0f, 0.0f };
    gx_mat4_lookat(&mat_view, &eye, &center, &up);

    float aspect = (float)tgt->width / (float)tgt->height;
    gx_mat4_perspective(&mat_proj, 60.0f * GX_DEG2RAD, aspect, 0.1f, 50.0f);

    gx_mat4_mul(&mat_mv,  &mat_view, &mat_model);
    gx_mat4_mul(&mat_mvp, &mat_proj, &mat_mv);

    st->uniforms.mvp       = mat_mvp;
    st->uniforms.model     = mat_model;
    st->uniforms.light_dir = (gx_vec3_t){ 0.577f, 0.577f, 0.577f };
    gx_vec3_normalize(&st->uniforms.light_dir);

    // Render!
    // Зарезервируем нижние 18 px под HUD.
    int hud_h = 18;
    int gl_h  = tgt->height - hud_h;
    if (gl_h < 1) gl_h = 1;
    aos_egl_resize(st->gl, tgt->width, gl_h);
    gl_viewport(0, 0, tgt->width, gl_h);
    gl_clear_color(0xFF0A1028);           // UI_BG_DEEP
    gl_clear_depth(1.0f);
    gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    gl_enable(GL_DEPTH_TEST);
    gl_enable(GL_CULL_FACE);
    gl_cull_face(GL_BACK);
    gl_front_face(GL_CCW);

    gl_use_program(&g_cube_program);
    gl_set_uniforms(&st->uniforms);
    gl_bind_texture(&st->texture);

    gx_reset_stats(st->gl);

    gl_vertex_attrib_pointer(0, 3, GL_FLOAT, VTX_STRIDE_FLOATS, g_cube_verts + 0);
    gl_vertex_attrib_pointer(1, 3, GL_FLOAT, VTX_STRIDE_FLOATS, g_cube_verts + 3);
    gl_vertex_attrib_pointer(2, 2, GL_FLOAT, VTX_STRIDE_FLOATS, g_cube_verts + 6);
    gl_enable_vertex_attrib(0);
    gl_enable_vertex_attrib(1);
    gl_enable_vertex_attrib(2);

    gl_draw_elements(GL_TRIANGLES, 36, g_cube_indices);

    st->last_stats = gx_get_stats(st->gl);
    aos_egl_swap_buffers(st->gl);

    // HUD — рисуем "поверх" нижней полосы окна.
    gfx_fill_rect(tgt, 0, gl_h, tgt->width, hud_h, 0xFF101830);

    // FPS — обновляем раз в секунду.
    st->frames_in_window++;
    if (st->fps_window_start_ms == 0) st->fps_window_start_ms = now;
    if (now - st->fps_window_start_ms >= 1000) {
        st->last_fps = st->frames_in_window;
        st->frames_in_window = 0;
        st->fps_window_start_ms = now;
    }

    char buf[80];
    int n = 0;
    const char* p = "FPS:";
    while (*p) buf[n++] = *p++;
    char num[16]; utoa((uint64_t)st->last_fps, num);
    int nl = strn(num);
    for (int i = 0; i < nl; i++) buf[n++] = num[i];
    buf[n++] = ' ';
    p = "tris:";
    while (*p) buf[n++] = *p++;
    utoa(st->last_stats.triangles_drawn, num);
    nl = strn(num);
    for (int i = 0; i < nl; i++) buf[n++] = num[i];
    buf[n++] = '/';
    utoa(st->last_stats.triangles_total, num);
    nl = strn(num);
    for (int i = 0; i < nl; i++) buf[n++] = num[i];
    buf[n++] = ' ';
    p = "px:";
    while (*p) buf[n++] = *p++;
    utoa(st->last_stats.pixels_shaded, num);
    nl = strn(num);
    for (int i = 0; i < nl; i++) buf[n++] = num[i];
    buf[n] = 0;
    gfx_draw_string(tgt, 6, gl_h + 2, buf, 0xFFFFD200, 0xFF101830);
}

// ----------------------------------------------------------------------------
// Открытие приложения.  Используем тот же шаблон, что в apps.c
// (но spawn_app_window там static — у нас своя версия).
// ----------------------------------------------------------------------------
static int g_glcube_spawn_x = 200;
static int g_glcube_spawn_y = 120;

window_t* app_glcube_open(void) {
    cube_app_state_t* st = (cube_app_state_t*)kmalloc(sizeof(cube_app_state_t));
    if (!st) return 0;

    // Обнуляем всё.
    uint8_t* p = (uint8_t*)st;
    for (size_t i = 0; i < sizeof(cube_app_state_t); i++) p[i] = 0;

    window_t* win = window_create("3D куб (GLES)",
                                  g_glcube_spawn_x, g_glcube_spawn_y,
                                  360, 280);
    if (!win) { kfree(st); return 0; }

    win->on_draw      = cube_on_draw;
    win->bg_color     = 0xFF0A1028;
    win->accent_color = 0xFFF5A037;       // UI_ACCENT_ORANGE
    win->user_data    = st;
    win->owns_user_data = true;
    // [HOTFIX 20.05.2026] Куб вращается сам по таймеру — нужны непрерывные
    // кадры. Без этого WM с HOTFIX 19.05.2026 уходит в idle 10 FPS и
    // куб дёргается с проскоками до 30 FPS только на курсорных событиях.
    win->wants_continuous_redraw = true;

    // ВАЖНО: без wm_add_window окно создано, but WM о нём не знает —
    // оно невидимо, on_draw не вызывается. Регрессия 20.05.2026 — была
    // случайно дропнута при добавлении wants_continuous_redraw флага.
    wm_add_window(win);

    g_glcube_spawn_x += 30;
    g_glcube_spawn_y += 30;
    if (g_glcube_spawn_x > 580) g_glcube_spawn_x = 200;
    if (g_glcube_spawn_y > 360) g_glcube_spawn_y = 120;

    serial_puts("[glcube] window opened (GLES app)\n");
    return win;
}

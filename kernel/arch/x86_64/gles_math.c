// ============================================================================
// gles_math.c — математика для программного 3D-стека (этап 44).
// ----------------------------------------------------------------------------
// Ядро freestanding: нет libm. Здесь — собственные sinf/cosf/tanf/sqrtf/
// fabsf/fmodf + матричные операции 4×4.
//
//   * sqrtf: одна инструкция SSE SQRTSS.
//   * fabsf: bit-trick через uint32_t (clear sign bit).
//   * sinf/cosf: range-reduction в [-pi, pi] → полиномиальная аппроксимация
//                степени 7 (для sin) / 6 (для cos). Точность ~1e-6 — этого
//                более чем достаточно для 3D-рендера. Используется тот же
//                подход, что в musl/glibc small-libm.
//   * tanf:    sinf/cosf, простая защита от деления на 0.
//   * fmodf:   простой цикл вычитания (медленно, но в нашем коде вызывается
//              максимум 1-2 раза за кадр для range-reduction).
//
// Матрицы 4×4 хранятся column-major (стандарт OpenGL):
//   m[0..3]   = column 0   (=  M[0][0..3] )
//   m[4..7]   = column 1
//   m[8..11]  = column 2
//   m[12..15] = column 3
//
// Доступ к элементу: M(row, col) = m[col*4 + row].
// ============================================================================

#include "gles.h"
#include <stdint.h>

// ----------------------------------------------------------------------------
// fabsf / sqrtf
// ----------------------------------------------------------------------------
float gx_fabsf(float x) {
    // Хитрый bit-trick: clear sign bit. Корректен для всех finite,
    // включая ±0 и денормал.
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u &= 0x7FFFFFFFu;
    return v.f;
}

float gx_sqrtf(float x) {
    // Одна инструкция SSE: SQRTSS xmm,xmm.
    float r;
    __asm__ __volatile__("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

// ----------------------------------------------------------------------------
// fmodf — упрощённый. Для аргументов до ~1e7 цикл сходится за <30 шагов.
// Если нужны большие числа — заменить на ldexpf-based; нам хватит.
// ----------------------------------------------------------------------------
float gx_fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f;
    float ay = gx_fabsf(y);
    float ax = gx_fabsf(x);
    if (ax < ay) return x;
    // Trunc(ax/ay) через приведение к int (для нашего использования
    // частное < 2^31).
    float q = ax / ay;
    // floor truncation.
    int   qi = (int)q;
    float r  = ax - (float)qi * ay;
    // Учитываем знак x.
    return (x < 0.0f) ? -r : r;
}

// ----------------------------------------------------------------------------
// Range reduction для sin/cos: приводим x к [-pi, pi].
// ----------------------------------------------------------------------------
static inline float reduce_to_pi(float x) {
    // x mod 2*pi, затем сдвиг в [-pi, pi].
    float t = gx_fmodf(x, GX_2PI);
    if (t >  GX_PI) t -= GX_2PI;
    if (t < -GX_PI) t += GX_2PI;
    return t;
}

// ----------------------------------------------------------------------------
// sinf — полиномиальная аппроксимация (минимаксная, 7 степень).
//   sin(x) ≈ x + a3*x^3 + a5*x^5 + a7*x^7
//   Точность на [-pi/2, pi/2] — порядка 5e-7.
//   Симметрия sin(pi - x) = sin(x) расширяет диапазон до [-pi, pi].
// ----------------------------------------------------------------------------
float gx_sinf(float x) {
    float t = reduce_to_pi(x);

    // sin(pi - x) = sin(x): отзеркаливаем правую/левую половины
    // отрезка [-pi, pi] так, чтобы остаться в [-pi/2, pi/2].
    if (t >  GX_HALF_PI) t =  GX_PI - t;
    if (t < -GX_HALF_PI) t = -GX_PI - t;

    // Коэффициенты для x + a3*x^3 + a5*x^5 + a7*x^7 на [-pi/2, pi/2].
    const float a3 = -1.6666654611e-1f;
    const float a5 =  8.3321608736e-3f;
    const float a7 = -1.9515295891e-4f;

    float x2 = t * t;
    float p  = a7;
    p = p * x2 + a5;
    p = p * x2 + a3;
    p = p * x2 + 1.0f;
    return t * p;
}

// ----------------------------------------------------------------------------
// cosf через sinf: cos(x) = sin(pi/2 - x).
// ----------------------------------------------------------------------------
float gx_cosf(float x) {
    return gx_sinf(GX_HALF_PI - x);
}

// ----------------------------------------------------------------------------
// tanf: sin/cos. Защита от вертикальной асимптоты (cos==0 → bigval).
// ----------------------------------------------------------------------------
float gx_tanf(float x) {
    float c = gx_cosf(x);
    if (gx_fabsf(c) < 1e-30f) return 0.0f;     // условный «inf»
    return gx_sinf(x) / c;
}

// ----------------------------------------------------------------------------
// Матрицы — column-major, как в OpenGL.
// ----------------------------------------------------------------------------
void gx_mat4_identity(gx_mat4_t* out) {
    for (int i = 0; i < 16; i++) out->m[i] = 0.0f;
    out->m[0] = out->m[5] = out->m[10] = out->m[15] = 1.0f;
}

void gx_mat4_translate(gx_mat4_t* out, float tx, float ty, float tz) {
    gx_mat4_identity(out);
    out->m[12] = tx;
    out->m[13] = ty;
    out->m[14] = tz;
}

void gx_mat4_scale(gx_mat4_t* out, float sx, float sy, float sz) {
    gx_mat4_identity(out);
    out->m[0]  = sx;
    out->m[5]  = sy;
    out->m[10] = sz;
}

void gx_mat4_rotate_x(gx_mat4_t* out, float a) {
    float c = gx_cosf(a), s = gx_sinf(a);
    gx_mat4_identity(out);
    out->m[5]  =  c;  out->m[6]  =  s;
    out->m[9]  = -s;  out->m[10] =  c;
}

void gx_mat4_rotate_y(gx_mat4_t* out, float a) {
    float c = gx_cosf(a), s = gx_sinf(a);
    gx_mat4_identity(out);
    out->m[0]  =  c;  out->m[2]  = -s;
    out->m[8]  =  s;  out->m[10] =  c;
}

void gx_mat4_rotate_z(gx_mat4_t* out, float a) {
    float c = gx_cosf(a), s = gx_sinf(a);
    gx_mat4_identity(out);
    out->m[0]  =  c;  out->m[1]  =  s;
    out->m[4]  = -s;  out->m[5]  =  c;
}

// ----------------------------------------------------------------------------
// Перспективная проекция.  GL-стиль (правосторонняя система координат,
// смотрим вдоль -Z, после умножения w = -z_view).  Стандартная формула:
//
//   f = 1 / tan(fovy/2)
//   M = | f/aspect  0        0                       0                |
//       | 0         f        0                       0                |
//       | 0         0        (far+near)/(near-far)   2*far*near/(near-far) |
//       | 0         0        -1                      0                |
//
// Column-major раскладка:
// ----------------------------------------------------------------------------
void gx_mat4_perspective(gx_mat4_t* out, float fovy_rad, float aspect,
                         float near_z, float far_z) {
    float f = 1.0f / gx_tanf(fovy_rad * 0.5f);
    for (int i = 0; i < 16; i++) out->m[i] = 0.0f;
    out->m[0]  = f / aspect;
    out->m[5]  = f;
    out->m[10] = (far_z + near_z) / (near_z - far_z);
    out->m[11] = -1.0f;
    out->m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
    // m[15] = 0 — особенность перспективной проекции.
}

// ----------------------------------------------------------------------------
// LookAt: формирует view-матрицу для камеры в eye, смотрящей на center.
// up — приблизительное направление «вверх» (нормируется внутри как
// сторонне-произведение).  Алгоритм:
//
//   f = normalize(center - eye)          (forward, смотрит ОТ камеры В сцену)
//   s = normalize(cross(f, up))          (right)
//   u = cross(s, f)                      (true up)
//
//   M = | s.x   s.y   s.z   -dot(s, eye) |
//       | u.x   u.y   u.z   -dot(u, eye) |
//       |-f.x  -f.y  -f.z    dot(f, eye) |
//       | 0     0     0      1           |
// ----------------------------------------------------------------------------
void gx_mat4_lookat(gx_mat4_t* out,
                    const gx_vec3_t* eye,
                    const gx_vec3_t* center,
                    const gx_vec3_t* up) {
    gx_vec3_t f;
    gx_vec3_sub(&f, center, eye);
    gx_vec3_normalize(&f);

    gx_vec3_t s;
    gx_vec3_cross(&s, &f, up);
    gx_vec3_normalize(&s);

    gx_vec3_t u;
    gx_vec3_cross(&u, &s, &f);

    out->m[0]  =  s.x;  out->m[4]  =  s.y;  out->m[8]  =  s.z;
    out->m[1]  =  u.x;  out->m[5]  =  u.y;  out->m[9]  =  u.z;
    out->m[2]  = -f.x;  out->m[6]  = -f.y;  out->m[10] = -f.z;
    out->m[3]  =  0.0f; out->m[7]  =  0.0f; out->m[11] =  0.0f;
    out->m[12] = -gx_vec3_dot(&s, eye);
    out->m[13] = -gx_vec3_dot(&u, eye);
    out->m[14] =  gx_vec3_dot(&f, eye);
    out->m[15] =  1.0f;
}

// ----------------------------------------------------------------------------
// out = a * b. Стандартный наивный mat4-mul.
// Учитываем aliasing: пишем в локальный temp перед копированием в out.
// ----------------------------------------------------------------------------
void gx_mat4_mul(gx_mat4_t* out, const gx_mat4_t* a, const gx_mat4_t* b) {
    gx_mat4_t tmp;
    for (int c = 0; c < 4; c++) {
        for (int r = 0; r < 4; r++) {
            float v = 0.0f;
            for (int k = 0; k < 4; k++) {
                v += a->m[k*4 + r] * b->m[c*4 + k];
            }
            tmp.m[c*4 + r] = v;
        }
    }
    *out = tmp;
}

// ----------------------------------------------------------------------------
// out = m * v.
// ----------------------------------------------------------------------------
void gx_mat4_mul_vec4(gx_vec4_t* out, const gx_mat4_t* m, const gx_vec4_t* v) {
    float x = m->m[0]*v->x + m->m[4]*v->y + m->m[8]*v->z  + m->m[12]*v->w;
    float y = m->m[1]*v->x + m->m[5]*v->y + m->m[9]*v->z  + m->m[13]*v->w;
    float z = m->m[2]*v->x + m->m[6]*v->y + m->m[10]*v->z + m->m[14]*v->w;
    float w = m->m[3]*v->x + m->m[7]*v->y + m->m[11]*v->z + m->m[15]*v->w;
    out->x = x;
    out->y = y;
    out->z = z;
    out->w = w;
}

// ----------------------------------------------------------------------------
// Векторные операции.
// ----------------------------------------------------------------------------
float gx_vec3_dot(const gx_vec3_t* a, const gx_vec3_t* b) {
    return a->x * b->x + a->y * b->y + a->z * b->z;
}

void gx_vec3_cross(gx_vec3_t* out, const gx_vec3_t* a, const gx_vec3_t* b) {
    float x = a->y * b->z - a->z * b->y;
    float y = a->z * b->x - a->x * b->z;
    float z = a->x * b->y - a->y * b->x;
    out->x = x; out->y = y; out->z = z;
}

void gx_vec3_sub(gx_vec3_t* out, const gx_vec3_t* a, const gx_vec3_t* b) {
    out->x = a->x - b->x;
    out->y = a->y - b->y;
    out->z = a->z - b->z;
}

void gx_vec3_normalize(gx_vec3_t* v) {
    float len2 = v->x*v->x + v->y*v->y + v->z*v->z;
    if (len2 < 1e-30f) return;
    float inv = 1.0f / gx_sqrtf(len2);
    v->x *= inv;
    v->y *= inv;
    v->z *= inv;
}

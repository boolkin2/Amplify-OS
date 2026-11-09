// ============================================================================
// mind_math.c — вычислительные ядра MIND (этап 49). Контракт — mind_math.h.
// ----------------------------------------------------------------------------
// Горячая точка всего инференса — скалярное произведение строки весов Q8_0
// с квантованной активацией: на SmolLM2-135M это ~135 млн умножений int8 на
// каждый токен. Блок Q8_0 = fp16-масштаб d + 32 знаковых байта.
//
// SSSE3-путь (как в ggml): pmaddubsw умеет только «беззнаковый × знаковый»,
// поэтому берём |w| (psignb w,w) и x·sign(w) (psignb x,w) — произведение то
// же. pmaddubsw даёт суммы пар в int16: максимум 2·128·127 = 32512 — влезает
// без насыщения (x квантуется в [-127, 127]). Дальше pmaddwd с единицами —
// суммы в int32.
//
// fp16-масштабы переводятся во float через таблицу на 65536 значений (256 КБ,
// строится один раз): иначе на каждый блок из 32 умножений приходился бы
// десяток битовых операций конвертации.
// ============================================================================
#include "mind_math.h"
#include "mind_gguf.h"
#include "mind_util.h"
#include "smp_pool.h"
#include "heap.h"

#include <emmintrin.h>
#include <tmmintrin.h>

static bool   g_ssse3;         // какие ядра сейчас в ходу
static bool   g_cpu_ssse3;     // что умеет CPU
static bool   g_tcg;           // QEMU без аппаратной виртуализации
static bool   g_scalar;        // simd=scalar (эксперимент)
static float* g_f16;           // LUT fp16 -> fp32

static inline void cpuid(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ __volatile__("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

// Почему важно знать про TCG: эмулятор QEMU переводит pmaddwd/paddd в
// быстрые векторные операции хоста, а pmaddubsw/psignb (SSSE3) — в вызовы
// вспомогательных C-функций. Замер (этап 49, SmolLM2-135M, 4 ядра):
// SSSE3 0.8 ток/с против SSE2 3.8 ток/с. На железе и под KVM всё наоборот —
// там SSSE3 короче. QEMU TCG подписывается в гипервизорном листе CPUID
// 0x40000000 строкой "TCGTCGTCGTCG"; на реальном CPU там её нет.
static bool detect_tcg(void) {
    uint32_t a, b, c, d;
    cpuid(0x40000000u, &a, &b, &c, &d);
    return b == 0x54474354u && c == 0x43544743u && d == 0x47435447u;   // "TCGT" "CGTC" "GTCG"
}

void mind_math_init(void) {
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    g_cpu_ssse3 = false;
    if (a >= 1) {
        cpuid(1, &a, &b, &c, &d);
        g_cpu_ssse3 = (c & (1u << 9)) != 0;
    }
    g_tcg = detect_tcg();
    g_ssse3 = g_cpu_ssse3 && !g_tcg;
    if (!g_f16) {
        g_f16 = (float*)kmalloc(65536 * sizeof(float));
        if (g_f16) for (uint32_t h = 0; h < 65536; h++) g_f16[h] = m_fp16_to_f32((uint16_t)h);
    }
}

bool mind_math_ssse3(void) { return g_ssse3; }
bool mind_math_tcg(void) { return g_tcg; }
const char* mind_math_kernels(void) { return g_scalar ? "scalar" : g_ssse3 ? "ssse3" : "sse2"; }

void mind_math_set_simd(int mode) {
    g_scalar = (mode == MIND_SIMD_SCALAR);
    if (mode == MIND_SIMD_SSE2 || mode == MIND_SIMD_SCALAR) g_ssse3 = false;
    else if (mode == MIND_SIMD_SSSE3) g_ssse3 = g_cpu_ssse3;
    else g_ssse3 = g_cpu_ssse3 && !g_tcg;
}

static inline float f16(uint16_t h) { return g_f16 ? g_f16[h] : m_fp16_to_f32(h); }
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ----------------------------------------------------------------------------
// Квантование активации (quantize_row_q8_0_ref из ggml).
// ----------------------------------------------------------------------------
void mind_quantize_q8(const float* x, int n, mind_qact_t* qa) {
    for (int b = 0; b < n / 32; b++) {
        const float* xb = x + b * 32;
        float amax = 0.0f;
        for (int i = 0; i < 32; i++) {
            float a = m_fabsf(xb[i]);
            if (a > amax) amax = a;
        }
        float dd = amax / 127.0f;
        float id = dd != 0.0f ? 1.0f / dd : 0.0f;
        qa->d[b] = f16(m_f32_to_fp16(dd));      // в блок идёт fp16-значение
        int8_t* q = qa->q + b * 32;
        for (int i = 0; i < 32; i++) {
            float v = xb[i] * id;
            // roundf: половинки — от нуля
            int r = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
            if (r > 127) r = 127;
            if (r < -127) r = -127;
            q[i] = (int8_t)r;
        }
    }
}

// ----------------------------------------------------------------------------
// Скалярные произведения.
// ----------------------------------------------------------------------------
__attribute__((target("ssse3")))
static float dot_q8_ssse3(const uint8_t* w, const int8_t* xq, const float* xd, int nb) {
    const __m128i ones = _mm_set1_epi16(1);
    float sum = 0.0f;
    for (int b = 0; b < nb; b++, w += 34, xq += 32) {
        __m128i w0 = _mm_loadu_si128((const __m128i*)(w + 2));
        __m128i w1 = _mm_loadu_si128((const __m128i*)(w + 18));
        __m128i x0 = _mm_loadu_si128((const __m128i*)xq);
        __m128i x1 = _mm_loadu_si128((const __m128i*)(xq + 16));
        __m128i p0 = _mm_maddubs_epi16(_mm_sign_epi8(w0, w0), _mm_sign_epi8(x0, w0));
        __m128i p1 = _mm_maddubs_epi16(_mm_sign_epi8(w1, w1), _mm_sign_epi8(x1, w1));
        __m128i s = _mm_add_epi32(_mm_madd_epi16(p0, ones), _mm_madd_epi16(p1, ones));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
        sum += (float)_mm_cvtsi128_si32(s) * (f16(rd16(w)) * xd[b]);
    }
    return sum;
}

__attribute__((target("ssse3")))
static float dot_q4_ssse3(const uint8_t* w, const int8_t* xq, const float* xd, int nb) {
    const __m128i ones = _mm_set1_epi16(1);
    const __m128i m4 = _mm_set1_epi8(0x0F);
    const __m128i off = _mm_set1_epi8(8);
    float sum = 0.0f;
    for (int b = 0; b < nb; b++, w += 18, xq += 32) {
        __m128i q = _mm_loadu_si128((const __m128i*)(w + 2));
        __m128i w0 = _mm_sub_epi8(_mm_and_si128(q, m4), off);                    // элементы 0..15
        __m128i w1 = _mm_sub_epi8(_mm_and_si128(_mm_srli_epi16(q, 4), m4), off); // 16..31
        __m128i x0 = _mm_loadu_si128((const __m128i*)xq);
        __m128i x1 = _mm_loadu_si128((const __m128i*)(xq + 16));
        __m128i p0 = _mm_maddubs_epi16(_mm_sign_epi8(w0, w0), _mm_sign_epi8(x0, w0));
        __m128i p1 = _mm_maddubs_epi16(_mm_sign_epi8(w1, w1), _mm_sign_epi8(x1, w1));
        __m128i s = _mm_add_epi32(_mm_madd_epi16(p0, ones), _mm_madd_epi16(p1, ones));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
        s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
        sum += (float)_mm_cvtsi128_si32(s) * (f16(rd16(w)) * xd[b]);
    }
    return sum;
}

// SSE2: знаковое расширение int8 -> int16 (unpack сам с собой + сдвиг).
static inline __m128i sx_lo(__m128i v) { return _mm_srai_epi16(_mm_unpacklo_epi8(v, v), 8); }
static inline __m128i sx_hi(__m128i v) { return _mm_srai_epi16(_mm_unpackhi_epi8(v, v), 8); }

static inline int isum_sse2(__m128i w0, __m128i w1, const int8_t* xq) {
    __m128i x0 = _mm_loadu_si128((const __m128i*)xq);
    __m128i x1 = _mm_loadu_si128((const __m128i*)(xq + 16));
    __m128i s = _mm_add_epi32(_mm_madd_epi16(sx_lo(w0), sx_lo(x0)), _mm_madd_epi16(sx_hi(w0), sx_hi(x0)));
    s = _mm_add_epi32(s, _mm_madd_epi16(sx_lo(w1), sx_lo(x1)));
    s = _mm_add_epi32(s, _mm_madd_epi16(sx_hi(w1), sx_hi(x1)));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0x4E));
    s = _mm_add_epi32(s, _mm_shuffle_epi32(s, 0xB1));
    return _mm_cvtsi128_si32(s);
}

static float dot_q8_sse2(const uint8_t* w, const int8_t* xq, const float* xd, int nb) {
    float sum = 0.0f;
    for (int b = 0; b < nb; b++, w += 34, xq += 32) {
        __m128i w0 = _mm_loadu_si128((const __m128i*)(w + 2));
        __m128i w1 = _mm_loadu_si128((const __m128i*)(w + 18));
        sum += (float)isum_sse2(w0, w1, xq) * (f16(rd16(w)) * xd[b]);
    }
    return sum;
}

static float dot_q4_sse2(const uint8_t* w, const int8_t* xq, const float* xd, int nb) {
    const __m128i m4 = _mm_set1_epi8(0x0F);
    const __m128i off = _mm_set1_epi8(8);
    float sum = 0.0f;
    for (int b = 0; b < nb; b++, w += 18, xq += 32) {
        __m128i q = _mm_loadu_si128((const __m128i*)(w + 2));
        __m128i w0 = _mm_sub_epi8(_mm_and_si128(q, m4), off);
        __m128i w1 = _mm_sub_epi8(_mm_and_si128(_mm_srli_epi16(q, 4), m4), off);
        sum += (float)isum_sse2(w0, w1, xq) * (f16(rd16(w)) * xd[b]);
    }
    return sum;
}

float mind_dot_f32(const float* a, const float* b, int n) {
    __m128 s0 = _mm_setzero_ps(), s1 = _mm_setzero_ps();
    int i = 0;
    for (; i + 8 <= n; i += 8) {
        s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i)));
        s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_loadu_ps(a + i + 4), _mm_loadu_ps(b + i + 4)));
    }
    s0 = _mm_add_ps(s0, s1);
    float t[4];
    _mm_storeu_ps(t, s0);
    float sum = (t[0] + t[1]) + (t[2] + t[3]);
    for (; i < n; i++) sum += a[i] * b[i];
    return sum;
}

static float dot_f16(const uint8_t* w, const float* x, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += f16(rd16(w + 2 * i)) * x[i];
    return sum;
}

static float dot_bf16(const uint8_t* w, const float* x, int n) {
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += m_bf16_to_f32(rd16(w + 2 * i)) * x[i];
    return sum;
}

// Скалярный вариант (без SIMD и без автовекторизации) — для замеров под TCG.
__attribute__((optimize("no-tree-vectorize")))
static float dot_q8_scalar(const uint8_t* w, const int8_t* xq, const float* xd, int nb) {
    float sum = 0.0f;
    for (int b = 0; b < nb; b++, w += 34, xq += 32) {
        const int8_t* q = (const int8_t*)(w + 2);
        int s = 0;
        for (int i = 0; i < 32; i++) s += (int)q[i] * (int)xq[i];
        sum += (float)s * (f16(rd16(w)) * xd[b]);
    }
    return sum;
}

static inline float row_dot(const mind_mat_t* W, int r, const float* x, const mind_qact_t* qa) {
    const uint8_t* row = W->data + (uint64_t)r * W->row_bytes;
    switch (W->type) {
        case GGML_T_Q8_0:
            if (g_scalar) return dot_q8_scalar(row, qa->q, qa->d, W->cols / 32);
            return g_ssse3 ? dot_q8_ssse3(row, qa->q, qa->d, W->cols / 32)
                           : dot_q8_sse2(row, qa->q, qa->d, W->cols / 32);
        case GGML_T_Q4_0:
            return g_ssse3 ? dot_q4_ssse3(row, qa->q, qa->d, W->cols / 32)
                           : dot_q4_sse2(row, qa->q, qa->d, W->cols / 32);
        case GGML_T_F32:  return mind_dot_f32((const float*)row, x, W->cols);
        case GGML_T_F16:  return dot_f16(row, x, W->cols);
        case GGML_T_BF16: return dot_bf16(row, x, W->cols);
        default: return 0.0f;
    }
}

static bool is_quant(uint32_t t) { return t == GGML_T_Q8_0 || t == GGML_T_Q4_0; }

// ----------------------------------------------------------------------------
// matmul: строки всех матриц склеены в один диапазон [0, total); на каждую
// строку — все T входов пакета (этап 49.1).
// ----------------------------------------------------------------------------
// Почему строка снаружи, а токены внутри: строка весов Q8_0 (2 КБ при 2048
// столбцах) остаётся в L1, пока по ней проходят все T активаций, — на prefill
// веса читаются из памяти один раз на пакет, а не на каждый токен. Сама
// арифметика для пары (строка, токен) — ровно та же row_dot, что у matvec,
// поэтому пакетный prefill даёт побитово те же числа, что поштучный.
typedef struct {
    const mind_mat_t* const* W;
    float* const*            y;
    int                      count;
    int                      start[5];   // префиксные суммы строк
    const float*             x;
    int                      T, ldx;
    int                      ldy[4];     // шаг строк выхода каждой матрицы
    const mind_qact_t*       qa;         // qa[t] — квантованный вход t
} mv_ctx_t;

static void mv_range(void* c, int b, int e) {
    mv_ctx_t* m = (mv_ctx_t*)c;
    int k = 0;
    while (k + 1 < m->count && b >= m->start[k + 1]) k++;
    for (int r = b; r < e; r++) {
        while (r >= m->start[k + 1]) k++;
        int lr = r - m->start[k];
        float* yk = m->y[k];
        const uint64_t ld = (uint64_t)m->ldy[k];
        for (int t = 0; t < m->T; t++)
            yk[(uint64_t)t * ld + (uint64_t)lr] =
                row_dot(m->W[k], lr, m->x + (uint64_t)t * (uint64_t)m->ldx, &m->qa[t]);
    }
}

void mind_matmul_multi(const mind_mat_t* const* W, float* const* y, const int* ldy, int count,
                       const float* x, int T, int ldx, mind_qact_t* qa) {
    if (count <= 0 || count > 4 || T <= 0) return;
    mv_ctx_t c;
    c.W = W; c.y = y; c.count = count; c.x = x; c.qa = qa;
    c.T = T; c.ldx = ldx;
    for (int k = 0; k < count; k++) c.ldy[k] = ldy ? ldy[k] : 0;
    c.start[0] = 0;
    bool need_q = false;
    for (int k = 0; k < count; k++) {
        c.start[k + 1] = c.start[k] + W[k]->rows;
        if (is_quant(W[k]->type)) need_q = true;
    }
    // Все матрицы в группе имеют одинаковое число столбцов (общий вход x).
    if (need_q)
        for (int t = 0; t < T; t++) mind_quantize_q8(x + (uint64_t)t * (uint64_t)ldx, W[0]->cols, &qa[t]);

    int total = c.start[count];
    int parts = (smp_pool_workers() + 1) * 4;
    int grain = total / parts;
    if (grain < 4) grain = 4;
    if (grain > 512) grain = 512;
    smp_pool_parallel_for(mv_range, &c, total, grain);
}

void mind_matvec_multi(const mind_mat_t* const* W, float* const* y, int count,
                       const float* x, mind_qact_t* qa) {
    mind_matmul_multi(W, y, 0, count, x, 1, 0, qa);
}

void mind_matvec(const mind_mat_t* W, float* y, const float* x, mind_qact_t* qa) {
    const mind_mat_t* ws[1] = { W };
    float* ys[1] = { y };
    mind_matmul_multi(ws, ys, 0, 1, x, 1, 0, qa);
}

void mind_mat_row_f32(const mind_mat_t* W, int row, float* out) {
    const uint8_t* p = W->data + (uint64_t)row * W->row_bytes;
    int n = W->cols;
    switch (W->type) {
        case GGML_T_F32: m_memcpy(out, p, (uint64_t)n * 4); break;
        case GGML_T_F16: for (int i = 0; i < n; i++) out[i] = f16(rd16(p + 2 * i)); break;
        case GGML_T_BF16: for (int i = 0; i < n; i++) out[i] = m_bf16_to_f32(rd16(p + 2 * i)); break;
        case GGML_T_Q8_0:
            for (int b = 0; b < n / 32; b++, p += 34) {
                float d = f16(rd16(p));
                const int8_t* q = (const int8_t*)(p + 2);
                for (int i = 0; i < 32; i++) out[b * 32 + i] = (float)q[i] * d;
            }
            break;
        case GGML_T_Q4_0:
            for (int b = 0; b < n / 32; b++, p += 18) {
                float d = f16(rd16(p));
                for (int i = 0; i < 16; i++) {
                    out[b * 32 + i]      = (float)((int)(p[2 + i] & 0x0F) - 8) * d;
                    out[b * 32 + i + 16] = (float)((int)(p[2 + i] >> 4) - 8) * d;
                }
            }
            break;
        default: m_memset(out, 0, (uint64_t)n * 4); break;
    }
}

// ----------------------------------------------------------------------------
// Поэлементные операции.
// ----------------------------------------------------------------------------
void mind_rmsnorm(float* out, const float* x, const float* w, int n, float eps) {
    // Сумма квадратов в double — как ggml (ggml_float) и эталон.
    double ss = 0.0;
    for (int i = 0; i < n; i++) ss += (double)x[i] * (double)x[i];
    float scale = (float)(1.0 / m_sqrt(ss / (double)n + (double)eps));
    for (int i = 0; i < n; i++) out[i] = (x[i] * scale) * w[i];
}

void mind_softmax(float* x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    double sum = 0.0;
    for (int i = 0; i < n; i++) { x[i] = m_expf(x[i] - mx); sum += x[i]; }
    float inv = (float)(1.0 / sum);
    for (int i = 0; i < n; i++) x[i] *= inv;
}

void mind_axpy(float* y, float a, const float* x, int n) {
    __m128 va = _mm_set1_ps(a);
    int i = 0;
    for (; i + 4 <= n; i += 4)
        _mm_storeu_ps(y + i, _mm_add_ps(_mm_loadu_ps(y + i), _mm_mul_ps(va, _mm_loadu_ps(x + i))));
    for (; i < n; i++) y[i] += a * x[i];
}

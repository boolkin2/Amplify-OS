// ============================================================================
// mind_math.h — вычислительные ядра AMPLIFY MIND (этап 49).
// ----------------------------------------------------------------------------
// «ggml-lite»: ровно те операции, что нужны forward-pass'у llama:
//   * матрица-на-вектор для весов F32 / F16 / BF16 / Q8_0 / Q4_0;
//   * для квантованных весов активация сначала квантуется в Q8_0 (блок 32,
//     d = amax/127, масштаб округляется до fp16) и считается целочисленное
//     скалярное произведение — так же поступает ggml_vec_dot_q8_0_q8_0,
//     поэтому числа совпадают с llama.cpp с точностью до порядка суммирования;
//   * RMSNorm, softmax, SiLU.
// Строки матрицы раздаются ядрам пула (smp_pool_parallel_for).
// SIMD: SSSE3 (pmaddubsw/psignb) — если CPU умеет, иначе SSE2 (pmaddwd).
// Оба пути работают только с XMM-регистрами, которые уже сохраняет FXSAVE в
// isr_common/task_yield, — никаких новых битов CR4/XCR0 (см. CLAUDE.md §5.2).
// ============================================================================
#ifndef AOS_X86_64_MIND_MATH_H
#define AOS_X86_64_MIND_MATH_H

#include <stdint.h>
#include <stdbool.h>

// Матрица [rows × cols]; строка = cols элементов, row_bytes байт.
typedef struct {
    uint32_t       type;       // GGML_T_*
    const uint8_t* data;
    int            rows;
    int            cols;
    uint64_t       row_bytes;
} mind_mat_t;

// Буфер квантованной активации (до MIND_MAX_COLS элементов).
#define MIND_MAX_COLS  16384
typedef struct {
    int8_t q[MIND_MAX_COLS];
    float  d[MIND_MAX_COLS / 32];
} mind_qact_t;

void  mind_math_init(void);            // CPUID: выбор SSSE3/SSE2
bool  mind_math_ssse3(void);          // SSSE3-ядра в ходу
bool  mind_math_tcg(void);            // работаем под эмулятором QEMU TCG
const char* mind_math_kernels(void);  // "ssse3" / "sse2" / "scalar"

// mind.conf simd=auto|sse2|ssse3. auto: SSSE3, если CPU умеет, но под TCG —
// SSE2 (эмулятор исполняет SSSE3 в разы медленнее, см. mind_math.c).
enum { MIND_SIMD_AUTO = 0, MIND_SIMD_SSE2, MIND_SIMD_SSSE3, MIND_SIMD_SCALAR };
void  mind_math_set_simd(int mode);

// y[i] = W[i] · x для одной или нескольких матриц с общим входом x
// (q/k/v и gate/up склеиваются в один parallel-for — меньше барьеров).
// qa — рабочий буфер для квантованного x (не на стеке: 16 КБ).
void  mind_matvec_multi(const mind_mat_t* const* W, float* const* y, int count,
                        const float* x, mind_qact_t* qa);
void  mind_matvec(const mind_mat_t* W, float* y, const float* x, mind_qact_t* qa);

// Пакет (этап 49.1): входы x + t*ldx (t < T) -> выходы y[k] + t*ldy[k].
// qa — массив из T буферов. Для каждой пары (строка, вход) — та же
// арифметика, что у matvec: пакетный prefill побитово равен поштучному.
void  mind_matmul_multi(const mind_mat_t* const* W, float* const* y, const int* ldy, int count,
                        const float* x, int T, int ldx, mind_qact_t* qa);

// Строка матрицы (эмбеддинг токена) в float.
void  mind_mat_row_f32(const mind_mat_t* W, int row, float* out);

void  mind_rmsnorm(float* out, const float* x, const float* w, int n, float eps);
void  mind_softmax(float* x, int n);
float mind_dot_f32(const float* a, const float* b, int n);
void  mind_axpy(float* y, float a, const float* x, int n);   // y += a*x

// Квантование активации в Q8_0 (n кратно 32).
void  mind_quantize_q8(const float* x, int n, mind_qact_t* qa);

#endif

// ============================================================================
// mind_model.h — трансформер для AMPLIFY MIND (этапы 49, 49.1).
// ----------------------------------------------------------------------------
// Архитектуры (general.architecture):
//   * "llama" — LLaMA/TinyLlama/SmolLM2: RMSNorm, RoPE «NORM» (вращаются
//     соседние пары — так convert_hf_to_gguf раскладывает Q/K для llama),
//     GQA, SwiGLU;
//   * "qwen3" (этап 49.1, Qwen3-0.6B/1.7B/4B...): то же плюс QK-norm —
//     RMSNorm каждой головы q и k (attn_q_norm/attn_k_norm) ДО RoPE, RoPE
//     «NEOX» (вращаются половины головы: x[i] и x[i + n_rot/2]), head_dim
//     явно из attention.key_length (у 0.6B n_head·head_dim != n_embd), без
//     bias в attention (в отличие от qwen2).
// Выход — через output.weight или связанные эмбеддинги.
//
// Веса не копируются: матрицы указывают прямо в образ GGUF-файла в памяти.
// Всё остальное (KV-кэш, таблицы RoPE, рабочие буферы) — одна арена из
// физических страниц (pmm_alloc_big: с 49.1 — и выше 4 ГБ).
//
// Пакетный forward (49.1): до MIND_BATCH токенов промпта за один проход
// матриц. Для каждого токена арифметика та же, что при поштучном проходе,
// поэтому логиты побитово совпадают (это проверяют тест и эталон).
// ============================================================================
#ifndef AOS_X86_64_MIND_MODEL_H
#define AOS_X86_64_MIND_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include "mind_gguf.h"
#include "mind_math.h"

#define MIND_MAX_LAYERS  64
#define MIND_BATCH       32          // токенов в одном пакетном проходе prefill

enum { MIND_ARCH_LLAMA = 1, MIND_ARCH_QWEN3 = 2 };

typedef struct {
    mind_mat_t   wq, wk, wv, wo, w_gate, w_up, w_down;
    const float* attn_norm;
    const float* ffn_norm;
    const float* q_norm;     // qwen3: веса RMSNorm головы q (head_dim), иначе 0
    const float* k_norm;
} mind_layer_t;

typedef struct {
    int   arch;
    bool  rope_neox;
    int   n_embd, n_layer, n_head, n_head_kv, head_dim, kv_dim;
    int   n_ff, n_vocab, n_ctx, n_ctx_train, n_rot;
    float eps, rope_base;

    mind_mat_t   tok_embd, output;
    const float* out_norm;
    mind_layer_t layers[MIND_MAX_LAYERS];

    float* k_cache;          // [n_layer][n_ctx][kv_dim]
    float* v_cache;
    float* rope_cos;         // [n_ctx][n_rot/2]
    float* rope_sin;

    // Рабочие буферы пакета: [MIND_BATCH][ширина].
    float *x, *xb, *xb2, *q, *k, *v, *att, *hb, *hb2, *logits;
    mind_qact_t* qa;         // [MIND_BATCH]
    int    wide;             // ширина строк x/xb/xb2 = max(n_embd, n_head·head_dim)

    uint8_t* arena;
    uint64_t arena_pages;
    uint64_t n_params;
    uint32_t main_type;      // тип большинства весов (для строки статуса)
} mind_model_t;

// Построить модель поверх разобранного GGUF. n_ctx — желаемый размер
// контекста (обрезается до обучающего; если арена не помещается в память —
// уменьшается вдвое, но не ниже 256). 0 — успех, иначе текст в err.
int    mind_model_init(mind_model_t* m, const gguf_file_t* g, int n_ctx, char* err, int errlen);
void   mind_model_free(mind_model_t* m);

// Один шаг: токен на позиции pos -> логиты (m->logits, n_vocab штук).
// Можно звать с любого ядра (параллелится через smp_pool).
float* mind_model_forward(mind_model_t* m, int token, int pos);

// Пакет: tokens[0..n) на позициях pos..pos+n-1 (n <= MIND_BATCH). Логиты
// считаются только для последнего токена и только если want_logits.
float* mind_model_forward_batch(mind_model_t* m, const int32_t* tokens, int n, int pos,
                                bool want_logits);

// Сколько байт арены (KV + таблицы + буферы) нужно под n_ctx.
uint64_t mind_model_arena_bytes(const mind_model_t* m);

// Грубая оценка памяти под модель до разбора файла (веса + KV + запас),
// для выбора модели по объёму RAM: file_size + file_size/4 + 32 МБ.
uint64_t mind_model_estimate_bytes(uint64_t file_size);

#endif

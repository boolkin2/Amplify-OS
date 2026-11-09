// ============================================================================
// mind_model.c — forward-pass llama / qwen3 (этапы 49, 49.1). Контракт — mind_model.h.
// ----------------------------------------------------------------------------
// Пакет из n токенов на позициях pos..pos+n-1 (n = 1 — обычный шаг генерации):
//   x[t] = embd[token[t]]
//   для каждого слоя:
//     h = RMSNorm(x)·attn_norm;  q,k,v = Wq·h, Wk·h, Wv·h      (один parallel-for
//                                    по строкам весов; внутри строки — все t)
//     qwen3: q,k = RMSNorm по каждой голове (q_norm / k_norm)
//     RoPE(q), RoPE(k) — NORM (llama) или NEOX (qwen3); k,v -> KV-кэш[pos+t]
//     для каждой пары (t, голова) (parallel-for):
//       a = softmax(q·K[0..pos+t] / sqrt(hd));  out = a·V[0..pos+t]
//     x += Wo·out
//     h = RMSNorm(x)·ffn_norm;  g,u = Wgate·h, Wup·h
//     x += Wdown·(silu(g)*u)
//   logits = Wout·RMSNorm(x[n-1])·out_norm        (только последний токен)
// GQA: голове h соответствует kv-голова h / (n_head / n_head_kv).
//
// Порядок операций над числами для каждого токена не зависит от n: пакетный
// prefill побитово равен поштучному (это же проверяет tools/mind_ref.py,
// который повторяет арифметику ядра).
// ============================================================================
#include "mind_model.h"
#include "mind_util.h"
#include "smp_pool.h"
#include "pmm.h"
#include "memory.h"

static void set_err(char* err, int errlen, const char* a, const char* b) {
    if (!err || errlen <= 0) return;
    err[0] = 0;
    m_append(err, (uint64_t)errlen, a);
    if (b) m_append(err, (uint64_t)errlen, b);
}

// Имя тензора слоя: "blk.<i>.<suffix>".
static void blk_name(char* out, int cap, int i, const char* suffix) {
    out[0] = 0;
    m_append(out, (uint64_t)cap, "blk.");
    m_append_dec(out, (uint64_t)cap, i);
    m_append(out, (uint64_t)cap, ".");
    m_append(out, (uint64_t)cap, suffix);
}

static bool type_ok(uint32_t t) {
    return t == GGML_T_F32 || t == GGML_T_F16 || t == GGML_T_BF16 ||
           t == GGML_T_Q8_0 || t == GGML_T_Q4_0;
}

// Матрица с ожидаемой формой [rows × cols] (ne[0] = cols, ne[1] = rows).
static bool get_mat(const gguf_file_t* g, const char* name, int cols, int rows,
                    mind_mat_t* out, char* err, int errlen) {
    const gguf_tensor_t* t = gguf_tensor(g, name);
    if (!t) { set_err(err, errlen, "missing tensor ", name); return false; }
    if (!type_ok(t->type) || !t->data) {
        set_err(err, errlen, "unsupported tensor type ", gguf_type_name(t->type));
        return false;
    }
    if ((int)t->ne[0] != cols || (int)t->ne[1] != rows || t->ne[2] != 1) {
        set_err(err, errlen, "unexpected shape of ", name);
        return false;
    }
    if (cols > MIND_MAX_COLS) { set_err(err, errlen, "matrix too wide: ", name); return false; }
    out->type = t->type;
    out->data = t->data;
    out->rows = rows;
    out->cols = cols;
    out->row_bytes = gguf_type_bytes(t->type, (uint64_t)cols);
    return true;
}

// ----------------------------------------------------------------------------
// Арена: один проход «посчитать» (base == 0) и один «нарезать».
// ----------------------------------------------------------------------------
typedef struct {
    uint8_t* base;
    uint64_t used;
} carve_t;

static void* carve(carve_t* c, uint64_t bytes) {
    uint64_t off = (c->used + 63) & ~63ull;        // выравнивание под кэш-линию
    c->used = off + bytes;
    return c->base ? (void*)(c->base + off) : (void*)0;
}

// Вектор нормализации во float: F32 — прямо из файла, иначе конвертируем
// в арену (указатель *slot заполняется только на проходе нарезки).
static bool norm_vec(const gguf_file_t* g, const char* name, int n, carve_t* c,
                     const float** slot, char* err, int errlen) {
    const gguf_tensor_t* t = gguf_tensor(g, name);
    if (!t) { set_err(err, errlen, "missing tensor ", name); return false; }
    if ((int)(t->ne[0] * t->ne[1]) != n || !t->data) { set_err(err, errlen, "bad norm tensor ", name); return false; }
    if (t->type == GGML_T_F32 && !((uintptr_t)t->data & 3)) {
        *slot = (const float*)t->data;
        return true;
    }
    if (t->type != GGML_T_F16 && t->type != GGML_T_BF16 && t->type != GGML_T_F32) {
        set_err(err, errlen, "unsupported norm type ", gguf_type_name(t->type));
        return false;
    }
    float* dst = (float*)carve(c, (uint64_t)n * 4);
    if (dst) {
        mind_mat_t tmp = { t->type, t->data, 1, n, gguf_type_bytes(t->type, (uint64_t)n) };
        mind_mat_row_f32(&tmp, 0, dst);
        *slot = dst;
    }
    return true;
}

static bool layout(mind_model_t* m, const gguf_file_t* g, carve_t* c, char* err, int errlen) {
    char nm[64];
    const uint64_t B = MIND_BATCH;
    uint64_t kv = (uint64_t)m->n_layer * (uint64_t)m->n_ctx * (uint64_t)m->kv_dim * 4;
    m->k_cache = (float*)carve(c, kv);
    m->v_cache = (float*)carve(c, kv);
    uint64_t rope = (uint64_t)m->n_ctx * (uint64_t)(m->n_rot / 2) * 4;
    m->rope_cos = (float*)carve(c, rope);
    m->rope_sin = (float*)carve(c, rope);
    int qd = m->n_head * m->head_dim;
    m->wide = m->n_embd > qd ? m->n_embd : qd;
    m->x   = (float*)carve(c, B * (uint64_t)m->n_embd * 4);
    m->xb  = (float*)carve(c, B * (uint64_t)m->wide * 4);
    m->xb2 = (float*)carve(c, B * (uint64_t)qd * 4);
    m->q   = (float*)carve(c, B * (uint64_t)qd * 4);
    m->k   = (float*)carve(c, B * (uint64_t)m->kv_dim * 4);
    m->v   = (float*)carve(c, B * (uint64_t)m->kv_dim * 4);
    m->att = (float*)carve(c, B * (uint64_t)m->n_head * (uint64_t)m->n_ctx * 4);
    m->hb  = (float*)carve(c, B * (uint64_t)m->n_ff * 4);
    m->hb2 = (float*)carve(c, B * (uint64_t)m->n_ff * 4);
    m->logits = (float*)carve(c, (uint64_t)m->n_vocab * 4);
    m->qa  = (mind_qact_t*)carve(c, B * sizeof(mind_qact_t));
    if (!norm_vec(g, "output_norm.weight", m->n_embd, c, &m->out_norm, err, errlen)) return false;
    for (int i = 0; i < m->n_layer; i++) {
        mind_layer_t* L = &m->layers[i];
        blk_name(nm, sizeof(nm), i, "attn_norm.weight");
        if (!norm_vec(g, nm, m->n_embd, c, &L->attn_norm, err, errlen)) return false;
        blk_name(nm, sizeof(nm), i, "ffn_norm.weight");
        if (!norm_vec(g, nm, m->n_embd, c, &L->ffn_norm, err, errlen)) return false;
        if (m->arch == MIND_ARCH_QWEN3) {
            blk_name(nm, sizeof(nm), i, "attn_q_norm.weight");
            if (!norm_vec(g, nm, m->head_dim, c, &L->q_norm, err, errlen)) return false;
            blk_name(nm, sizeof(nm), i, "attn_k_norm.weight");
            if (!norm_vec(g, nm, m->head_dim, c, &L->k_norm, err, errlen)) return false;
        }
    }
    return true;
}

uint64_t mind_model_arena_bytes(const mind_model_t* m) {
    return m->arena_pages * PAGE_SIZE;
}

uint64_t mind_model_estimate_bytes(uint64_t file_size) {
    return file_size + file_size / 4 + 32ull * 1024 * 1024;
}

// "<arch>.<suffix>" — ключ метаданных GGUF.
static const char* akey(char* buf, int cap, const char* arch, const char* suffix) {
    buf[0] = 0;
    m_append(buf, (uint64_t)cap, arch);
    m_append(buf, (uint64_t)cap, ".");
    m_append(buf, (uint64_t)cap, suffix);
    return buf;
}

int mind_model_init(mind_model_t* m, const gguf_file_t* g, int n_ctx, char* err, int errlen) {
    m_memset(m, 0, sizeof(*m));
    mind_str_t as;
    if (!gguf_get_str(g, "general.architecture", &as)) { set_err(err, errlen, "no general.architecture", 0); return -1; }
    char arch[32];
    {
        uint64_t n = as.len < 31 ? as.len : 31;
        m_memcpy(arch, as.p, n);
        arch[n] = 0;
    }
    if (m_streq(arch, "llama")) m->arch = MIND_ARCH_LLAMA;
    else if (m_streq(arch, "qwen3")) m->arch = MIND_ARCH_QWEN3;
    else { set_err(err, errlen, "unsupported architecture: ", arch); return -1; }
    m->rope_neox = (m->arch == MIND_ARCH_QWEN3);

    char k[96];
    uint32_t u = 0;
#define NEED(suffix, dst) do { if (!gguf_get_u32(g, akey(k, sizeof(k), arch, suffix), &u)) { set_err(err, errlen, "missing ", k); return -1; } dst = (int)u; } while (0)
    NEED("embedding_length", m->n_embd);
    NEED("block_count", m->n_layer);
    NEED("feed_forward_length", m->n_ff);
    NEED("attention.head_count", m->n_head);
#undef NEED
    m->n_head_kv = m->n_head;
    if (gguf_get_u32(g, akey(k, sizeof(k), arch, "attention.head_count_kv"), &u)) m->n_head_kv = (int)u;
    m->n_ctx_train = 2048;
    if (gguf_get_u32(g, akey(k, sizeof(k), arch, "context_length"), &u)) m->n_ctx_train = (int)u;
    m->eps = 1e-5f;
    (void)gguf_get_f32(g, akey(k, sizeof(k), arch, "attention.layer_norm_rms_epsilon"), &m->eps);
    m->rope_base = 10000.0f;
    (void)gguf_get_f32(g, akey(k, sizeof(k), arch, "rope.freq_base"), &m->rope_base);

    if (m->n_layer <= 0 || m->n_layer > MIND_MAX_LAYERS || m->n_head <= 0 ||
        m->n_head_kv <= 0 || m->n_head % m->n_head_kv) {
        set_err(err, errlen, "bad model hyperparameters", 0);
        return -1;
    }
    // head_dim: у qwen3 задан явно (attention.key_length), у llama — n_embd/n_head.
    m->head_dim = m->n_embd / m->n_head;
    if (gguf_get_u32(g, akey(k, sizeof(k), arch, "attention.key_length"), &u)) m->head_dim = (int)u;
    if (m->head_dim <= 0 || (m->arch == MIND_ARCH_LLAMA && m->n_embd % m->n_head)) {
        set_err(err, errlen, "bad head dimension", 0);
        return -1;
    }
    uint32_t vl = 0;
    if (gguf_get_u32(g, akey(k, sizeof(k), arch, "attention.value_length"), &vl) && (int)vl != m->head_dim) {
        set_err(err, errlen, "key_length != value_length is not supported", 0);
        return -1;
    }
    m->kv_dim = m->head_dim * m->n_head_kv;
    m->n_rot = m->head_dim;
    if (gguf_get_u32(g, akey(k, sizeof(k), arch, "rope.dimension_count"), &u)) m->n_rot = (int)u;
    if (m->n_rot > m->head_dim || (m->n_rot & 1)) { set_err(err, errlen, "bad rope dimension", 0); return -1; }
    if (m->n_ff > MIND_MAX_COLS || m->n_head * m->head_dim > MIND_MAX_COLS) {
        set_err(err, errlen, "model is too wide", 0);
        return -1;
    }

    const gguf_tensor_t* te = gguf_tensor(g, "token_embd.weight");
    if (!te) { set_err(err, errlen, "missing tensor token_embd.weight", 0); return -1; }
    m->n_vocab = (int)te->ne[1];

    m->n_ctx = n_ctx > 0 ? n_ctx : 1024;
    if (m->n_ctx > m->n_ctx_train) m->n_ctx = m->n_ctx_train;
    if (m->n_ctx < 16) m->n_ctx = 16;

    // ---- матрицы -------------------------------------------------------------
    char nm[64];
    if (!get_mat(g, "token_embd.weight", m->n_embd, m->n_vocab, &m->tok_embd, err, errlen)) return -1;
    if (gguf_tensor(g, "output.weight")) {
        if (!get_mat(g, "output.weight", m->n_embd, m->n_vocab, &m->output, err, errlen)) return -1;
    } else {
        m->output = m->tok_embd;        // связанные эмбеддинги (SmolLM2, Qwen3 до 4B)
    }
    const int qd = m->n_head * m->head_dim;
    for (int i = 0; i < m->n_layer; i++) {
        mind_layer_t* L = &m->layers[i];
        struct { const char* s; mind_mat_t* d; int cols, rows; } mats[] = {
            { "attn_q.weight",      &L->wq,     m->n_embd, qd },
            { "attn_k.weight",      &L->wk,     m->n_embd, m->kv_dim },
            { "attn_v.weight",      &L->wv,     m->n_embd, m->kv_dim },
            { "attn_output.weight", &L->wo,     qd,        m->n_embd },
            { "ffn_gate.weight",    &L->w_gate, m->n_embd, m->n_ff },
            { "ffn_up.weight",      &L->w_up,   m->n_embd, m->n_ff },
            { "ffn_down.weight",    &L->w_down, m->n_ff,   m->n_embd },
        };
        for (unsigned q = 0; q < sizeof(mats) / sizeof(mats[0]); q++) {
            blk_name(nm, sizeof(nm), i, mats[q].s);
            if (!get_mat(g, nm, mats[q].cols, mats[q].rows, mats[q].d, err, errlen)) return -1;
        }
        // Квантованные строки требуют длину, кратную 32: у Q8_0/Q4_0 это
        // гарантирует gguf_type_bytes (иначе он вернул бы 0 и data=0).
        // bias в attention у qwen3 нет; если встретится (qwen2) — честно отказать.
        blk_name(nm, sizeof(nm), i, "attn_q.bias");
        if (gguf_tensor(g, nm)) { set_err(err, errlen, "attention bias is not supported", 0); return -1; }
    }
    // Подсчёт параметров и «главного» типа весов.
    uint32_t cnt[32] = { 0 };
    for (uint64_t i = 0; i < g->n_tensors; i++) {
        const gguf_tensor_t* t = &g->tensors[i];
        m->n_params += t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3];
        if (t->n_dims >= 2 && t->type < 32) cnt[t->type]++;
    }
    for (uint32_t t = 0; t < 32; t++) if (cnt[t] > cnt[m->main_type]) m->main_type = t;

    // ---- арена: KV-кэш под n_ctx; если не помещается — контекст вдвое меньше --
    for (;;) {
        carve_t dry = { 0, 0 };
        if (!layout(m, g, &dry, err, errlen)) return -1;
        uint64_t pages = (dry.used + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t phys = pmm_alloc_big(pages);
        if (phys) {
            m->arena = (uint8_t*)(uintptr_t)phys;       // вся RAM отображена 1:1
            m->arena_pages = pages;
            break;
        }
        if (m->n_ctx <= 256) { set_err(err, errlen, "out of memory (KV cache)", 0); return -1; }
        m->n_ctx /= 2;
    }
    carve_t real = { m->arena, 0 };
    if (!layout(m, g, &real, err, errlen)) { mind_model_free(m); return -1; }

    // ---- таблицы RoPE: theta = pos * base^(-2i/n_rot), в double ----------------
    const int half = m->n_rot / 2;
    double lb = m_log((double)m->rope_base);
    for (int i = 0; i < half; i++) {
        double inv = m_exp(-(2.0 * (double)i / (double)m->n_rot) * lb);
        for (int p = 0; p < m->n_ctx; p++) {
            double s, c;
            m_sincos((double)p * inv, &s, &c);
            m->rope_cos[(uint64_t)p * half + i] = (float)c;
            m->rope_sin[(uint64_t)p * half + i] = (float)s;
        }
    }
    return 0;
}

void mind_model_free(mind_model_t* m) {
    if (m->arena) pmm_free_big((uint64_t)(uintptr_t)m->arena, m->arena_pages);
    m->arena = 0;
    m->arena_pages = 0;
}

// ----------------------------------------------------------------------------
// forward
// ----------------------------------------------------------------------------
static void rope_apply(const mind_model_t* m, float* v, int n_heads, int pos) {
    const int half = m->n_rot / 2;
    const float* cs = m->rope_cos + (uint64_t)pos * half;
    const float* sn = m->rope_sin + (uint64_t)pos * half;
    for (int h = 0; h < n_heads; h++) {
        float* x = v + h * m->head_dim;
        if (m->rope_neox) {
            // NEOX (qwen3): пара — элементы i и i + n_rot/2 одной головы.
            for (int i = 0; i < half; i++) {
                float x0 = x[i], x1 = x[i + half];
                x[i]        = x0 * cs[i] - x1 * sn[i];
                x[i + half] = x0 * sn[i] + x1 * cs[i];
            }
        } else {
            // NORM (llama): пара — соседние элементы 2i и 2i+1.
            for (int i = 0; i < half; i++) {
                float x0 = x[2 * i], x1 = x[2 * i + 1];
                x[2 * i]     = x0 * cs[i] - x1 * sn[i];
                x[2 * i + 1] = x0 * sn[i] + x1 * cs[i];
            }
        }
    }
}

typedef struct {
    mind_model_t* m;
    int           layer;
    int           pos0;
} attn_ctx_t;

// Элемент работы — пара (токен пакета t, голова h).
static void attn_range(void* c, int ib, int ie) {
    attn_ctx_t* a = (attn_ctx_t*)c;
    mind_model_t* m = a->m;
    const int hd = m->head_dim, gq = m->n_head / m->n_head_kv, qd = m->n_head * hd;
    const float scale = 1.0f / m_sqrtf((float)hd);
    const uint64_t lofs = (uint64_t)a->layer * (uint64_t)m->n_ctx * (uint64_t)m->kv_dim;
    for (int i = ib; i < ie; i++) {
        const int t = i / m->n_head, h = i % m->n_head;
        const int n = a->pos0 + t + 1;                 // причинно: позиции 0..pos0+t
        const float* q = m->q + (uint64_t)t * qd + (uint64_t)h * hd;
        float* att = m->att + (uint64_t)i * (uint64_t)m->n_ctx;
        const int kvh = h / gq;
        for (int p = 0; p < n; p++) {
            const float* k = m->k_cache + lofs + (uint64_t)p * m->kv_dim + (uint64_t)kvh * hd;
            att[p] = mind_dot_f32(q, k, hd) * scale;
        }
        mind_softmax(att, n);
        float* out = m->xb2 + (uint64_t)t * qd + (uint64_t)h * hd;
        m_memset(out, 0, (uint64_t)hd * 4);
        for (int p = 0; p < n; p++) {
            const float* v = m->v_cache + lofs + (uint64_t)p * m->kv_dim + (uint64_t)kvh * hd;
            mind_axpy(out, att[p], v, hd);
        }
    }
}

float* mind_model_forward_batch(mind_model_t* m, const int32_t* tokens, int n, int pos0,
                                bool want_logits) {
    if (n <= 0 || n > MIND_BATCH || pos0 < 0 || pos0 + n > m->n_ctx) return 0;
    const int D = m->n_embd, hd = m->head_dim, qd = m->n_head * hd, kvd = m->kv_dim, F = m->n_ff;
    for (int t = 0; t < n; t++) {
        int tok = tokens[t];
        if (tok < 0 || tok >= m->n_vocab) tok = 0;
        mind_mat_row_f32(&m->tok_embd, tok, m->x + (uint64_t)t * D);
    }

    for (int l = 0; l < m->n_layer; l++) {
        mind_layer_t* L = &m->layers[l];

        for (int t = 0; t < n; t++)
            mind_rmsnorm(m->xb + (uint64_t)t * D, m->x + (uint64_t)t * D, L->attn_norm, D, m->eps);
        {
            const mind_mat_t* W[3] = { &L->wq, &L->wk, &L->wv };
            float* Y[3] = { m->q, m->k, m->v };
            const int ld[3] = { qd, kvd, kvd };
            mind_matmul_multi(W, Y, ld, 3, m->xb, n, D, m->qa);
        }
        for (int t = 0; t < n; t++) {
            float* qt = m->q + (uint64_t)t * qd;
            float* kt = m->k + (uint64_t)t * kvd;
            if (L->q_norm) {
                // QK-norm (qwen3): каждая голова — отдельный вектор head_dim.
                for (int h = 0; h < m->n_head; h++) mind_rmsnorm(qt + h * hd, qt + h * hd, L->q_norm, hd, m->eps);
                for (int h = 0; h < m->n_head_kv; h++) mind_rmsnorm(kt + h * hd, kt + h * hd, L->k_norm, hd, m->eps);
            }
            rope_apply(m, qt, m->n_head, pos0 + t);
            rope_apply(m, kt, m->n_head_kv, pos0 + t);
            const uint64_t cofs = ((uint64_t)l * m->n_ctx + (uint64_t)(pos0 + t)) * kvd;
            m_memcpy(m->k_cache + cofs, kt, (uint64_t)kvd * 4);
            m_memcpy(m->v_cache + cofs, m->v + (uint64_t)t * kvd, (uint64_t)kvd * 4);
        }

        attn_ctx_t ac = { m, l, pos0 };
        smp_pool_parallel_for(attn_range, &ac, n * m->n_head, 1);

        {
            const mind_mat_t* W[1] = { &L->wo };
            float* Y[1] = { m->xb };
            const int ld[1] = { D };
            mind_matmul_multi(W, Y, ld, 1, m->xb2, n, qd, m->qa);
        }
        for (uint64_t i = 0; i < (uint64_t)n * D; i++) m->x[i] += m->xb[i];

        for (int t = 0; t < n; t++)
            mind_rmsnorm(m->xb + (uint64_t)t * D, m->x + (uint64_t)t * D, L->ffn_norm, D, m->eps);
        {
            const mind_mat_t* W[2] = { &L->w_gate, &L->w_up };
            float* Y[2] = { m->hb, m->hb2 };
            const int ld[2] = { F, F };
            mind_matmul_multi(W, Y, ld, 2, m->xb, n, D, m->qa);
        }
        for (uint64_t i = 0; i < (uint64_t)n * F; i++) {
            float g = m->hb[i];
            m->hb[i] = (g / (1.0f + m_expf(-g))) * m->hb2[i];
        }
        {
            const mind_mat_t* W[1] = { &L->w_down };
            float* Y[1] = { m->xb };
            const int ld[1] = { D };
            mind_matmul_multi(W, Y, ld, 1, m->hb, n, F, m->qa);
        }
        for (uint64_t i = 0; i < (uint64_t)n * D; i++) m->x[i] += m->xb[i];
    }

    if (!want_logits) return m->logits;
    float* last = m->x + (uint64_t)(n - 1) * D;
    mind_rmsnorm(last, last, m->out_norm, D, m->eps);
    mind_matvec(&m->output, m->logits, last, m->qa);
    return m->logits;
}

float* mind_model_forward(mind_model_t* m, int token, int pos) {
    int32_t t = token;
    return mind_model_forward_batch(m, &t, 1, pos, true);
}

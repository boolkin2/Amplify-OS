// ============================================================================
// mind_gguf.c — парсер GGUF (этап 49). Контракт — в mind_gguf.h.
// ----------------------------------------------------------------------------
// Раскладка файла (little-endian):
//   u32 magic 'GGUF' · u32 version · u64 n_tensors · u64 n_kv
//   n_kv × { str key · u32 type · value }        str = u64 len + байты
//   n_tensors × { str name · u32 n_dims · u64 ne[n_dims] · u32 type · u64 off }
//   паддинг до general.alignment (по умолчанию 32) · данные тензоров
// Версия 1 (u32-счётчики) давно вымерла — не поддерживаем.
// ============================================================================
#include "mind_gguf.h"
#include "mind_util.h"
#include "heap.h"

typedef struct {
    const uint8_t* p;
    const uint8_t* end;
    bool           bad;
} rd_t;

static bool rd_need(rd_t* r, uint64_t n) {
    if (r->bad || (uint64_t)(r->end - r->p) < n) { r->bad = true; return false; }
    return true;
}
static uint32_t rd_u32(rd_t* r) {
    if (!rd_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->p[0] | ((uint32_t)r->p[1] << 8) |
                 ((uint32_t)r->p[2] << 16) | ((uint32_t)r->p[3] << 24);
    r->p += 4;
    return v;
}
static uint64_t rd_u64(rd_t* r) {
    uint64_t lo = rd_u32(r);
    uint64_t hi = rd_u32(r);
    return lo | (hi << 32);
}
static mind_str_t rd_str(rd_t* r) {
    mind_str_t s = { 0, 0 };
    uint64_t n = rd_u64(r);
    if (!rd_need(r, n)) return s;
    s.p = r->p;
    s.len = n;
    r->p += n;
    return s;
}

static uint64_t scalar_size(uint32_t t) {
    switch (t) {
        case GGUF_T_U8: case GGUF_T_I8: case GGUF_T_BOOL: return 1;
        case GGUF_T_U16: case GGUF_T_I16: return 2;
        case GGUF_T_U32: case GGUF_T_I32: case GGUF_T_F32: return 4;
        case GGUF_T_U64: case GGUF_T_I64: case GGUF_T_F64: return 8;
        default: return 0;
    }
}

// Пропустить значение типа t (для массивов — рекурсия глубиной 1).
static void rd_skip_value(rd_t* r, uint32_t t, int depth) {
    if (t == GGUF_T_STR) { (void)rd_str(r); return; }
    if (t == GGUF_T_ARR) {
        if (depth > 1) { r->bad = true; return; }
        uint32_t et = rd_u32(r);
        uint64_t n = rd_u64(r);
        uint64_t es = scalar_size(et);
        if (es) {
            if (n > (uint64_t)(r->end - r->p) / es) { r->bad = true; return; }
            r->p += n * es;
        } else {
            for (uint64_t i = 0; i < n && !r->bad; i++) rd_skip_value(r, et, depth + 1);
        }
        return;
    }
    uint64_t s = scalar_size(t);
    if (!s) { r->bad = true; return; }
    if (rd_need(r, s)) r->p += s;
}

static void set_err(char* err, int errlen, const char* a, const char* b) {
    if (!err || errlen <= 0) return;
    err[0] = 0;
    m_append(err, (uint64_t)errlen, a);
    if (b) m_append(err, (uint64_t)errlen, b);
}

uint64_t gguf_type_bytes(uint32_t type, uint64_t n) {
    switch (type) {
        case GGML_T_F32:  return n * 4;
        case GGML_T_F16:  return n * 2;
        case GGML_T_BF16: return n * 2;
        case GGML_T_Q8_0: return (n % 32) ? 0 : n / 32 * 34;
        case GGML_T_Q4_0: return (n % 32) ? 0 : n / 32 * 18;
        default: return 0;
    }
}

const char* gguf_type_name(uint32_t type) {
    switch (type) {
        case GGML_T_F32: return "F32";
        case GGML_T_F16: return "F16";
        case GGML_T_BF16: return "BF16";
        case GGML_T_Q8_0: return "Q8_0";
        case GGML_T_Q4_0: return "Q4_0";
        case 3: return "Q4_1";
        case 6: return "Q5_0";
        case 7: return "Q5_1";
        case 10: return "Q2_K";
        case 11: return "Q3_K";
        case 12: return "Q4_K";
        case 13: return "Q5_K";
        case 14: return "Q6_K";
        default: return "?";
    }
}

int gguf_parse(gguf_file_t* g, const uint8_t* buf, uint64_t size, char* err, int errlen) {
    m_memset(g, 0, sizeof(*g));
    rd_t r = { buf, buf + size, false };
    if (size < 24 || rd_u32(&r) != 0x46554747u) { set_err(err, errlen, "not a GGUF file", 0); return -1; }
    g->version = rd_u32(&r);
    if (g->version < 2 || g->version > 3) { set_err(err, errlen, "unsupported GGUF version", 0); return -1; }
    g->n_tensors = rd_u64(&r);
    g->n_kv = rd_u64(&r);
    if (g->n_kv > 100000 || g->n_tensors > 100000) { set_err(err, errlen, "GGUF header is corrupt", 0); return -1; }

    g->base = buf;
    g->size = size;
    g->alignment = 32;
    g->kv = (gguf_kv_t*)kmalloc((g->n_kv ? g->n_kv : 1) * sizeof(gguf_kv_t));
    g->tensors = (gguf_tensor_t*)kmalloc((g->n_tensors ? g->n_tensors : 1) * sizeof(gguf_tensor_t));
    if (!g->kv || !g->tensors) { set_err(err, errlen, "out of memory (gguf tables)", 0); gguf_free(g); return -1; }

    for (uint64_t i = 0; i < g->n_kv; i++) {
        gguf_kv_t* kv = &g->kv[i];
        kv->key = rd_str(&r);
        kv->type = rd_u32(&r);
        if (kv->type == GGUF_T_ARR) {
            kv->arr_type = rd_u32(&r);
            kv->arr_n = rd_u64(&r);
            kv->val = r.p;
            // Откатываемся на заголовок массива и пропускаем его целиком.
            r.p -= 12;
            rd_skip_value(&r, GGUF_T_ARR, 0);
        } else {
            kv->val = r.p;
            rd_skip_value(&r, kv->type, 0);
        }
        if (r.bad) { set_err(err, errlen, "GGUF metadata is truncated", 0); gguf_free(g); return -1; }
        if (m_lstreq(kv->key.p, kv->key.len, "general.alignment") && kv->type == GGUF_T_U32) {
            uint32_t a = (uint32_t)kv->val[0] | ((uint32_t)kv->val[1] << 8) |
                         ((uint32_t)kv->val[2] << 16) | ((uint32_t)kv->val[3] << 24);
            if (a && !(a & (a - 1))) g->alignment = a;
        }
    }

    for (uint64_t i = 0; i < g->n_tensors; i++) {
        gguf_tensor_t* t = &g->tensors[i];
        t->name = rd_str(&r);
        t->n_dims = rd_u32(&r);
        if (t->n_dims == 0 || t->n_dims > 4) { set_err(err, errlen, "GGUF tensor has bad rank", 0); gguf_free(g); return -1; }
        for (int d = 0; d < 4; d++) t->ne[d] = 1;
        for (uint32_t d = 0; d < t->n_dims; d++) t->ne[d] = rd_u64(&r);
        t->type = rd_u32(&r);
        t->offset = rd_u64(&r);
        if (r.bad) { set_err(err, errlen, "GGUF tensor table is truncated", 0); gguf_free(g); return -1; }
    }

    uint64_t pos = (uint64_t)(r.p - buf);
    g->data_off = (pos + g->alignment - 1) / g->alignment * g->alignment;
    if (g->data_off > size) { set_err(err, errlen, "GGUF has no data section", 0); gguf_free(g); return -1; }

    // Тензоры: считаем размеры и проверяем, что они целиком внутри файла.
    // Неизвестный тип — не ошибка разбора: модель решит, нужен ли ей тензор.
    for (uint64_t i = 0; i < g->n_tensors; i++) {
        gguf_tensor_t* t = &g->tensors[i];
        uint64_t n = t->ne[0] * t->ne[1] * t->ne[2] * t->ne[3];
        t->nbytes = gguf_type_bytes(t->type, n);
        if (t->nbytes) {
            if (t->offset > size - g->data_off || t->nbytes > size - g->data_off - t->offset) {
                set_err(err, errlen, "GGUF tensor out of file bounds", 0);
                gguf_free(g);
                return -1;
            }
            t->data = buf + g->data_off + t->offset;
        }
    }
    return 0;
}

void gguf_free(gguf_file_t* g) {
    if (g->kv) kfree(g->kv);
    if (g->tensors) kfree(g->tensors);
    g->kv = 0;
    g->tensors = 0;
}

const gguf_kv_t* gguf_find(const gguf_file_t* g, const char* key) {
    for (uint64_t i = 0; i < g->n_kv; i++) {
        if (m_lstreq(g->kv[i].key.p, g->kv[i].key.len, key)) return &g->kv[i];
    }
    return 0;
}

const gguf_tensor_t* gguf_tensor(const gguf_file_t* g, const char* name) {
    for (uint64_t i = 0; i < g->n_tensors; i++) {
        if (m_lstreq(g->tensors[i].name.p, g->tensors[i].name.len, name)) return &g->tensors[i];
    }
    return 0;
}

static uint64_t le_n(const uint8_t* p, int n) {
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

bool gguf_get_u32(const gguf_file_t* g, const char* key, uint32_t* out) {
    const gguf_kv_t* kv = gguf_find(g, key);
    if (!kv) return false;
    switch (kv->type) {
        case GGUF_T_U8: case GGUF_T_I8: *out = (uint32_t)kv->val[0]; return true;
        case GGUF_T_U16: case GGUF_T_I16: *out = (uint32_t)le_n(kv->val, 2); return true;
        case GGUF_T_U32: case GGUF_T_I32: *out = (uint32_t)le_n(kv->val, 4); return true;
        case GGUF_T_U64: case GGUF_T_I64: *out = (uint32_t)le_n(kv->val, 8); return true;
        default: return false;
    }
}

bool gguf_get_f32(const gguf_file_t* g, const char* key, float* out) {
    const gguf_kv_t* kv = gguf_find(g, key);
    if (!kv) return false;
    if (kv->type == GGUF_T_F32) {
        union { uint32_t u; float f; } v;
        v.u = (uint32_t)le_n(kv->val, 4);
        *out = v.f;
        return true;
    }
    if (kv->type == GGUF_T_F64) {
        union { uint64_t u; double d; } v;
        v.u = le_n(kv->val, 8);
        *out = (float)v.d;
        return true;
    }
    uint32_t u;
    if (gguf_get_u32(g, key, &u)) { *out = (float)u; return true; }
    return false;
}

bool gguf_get_bool(const gguf_file_t* g, const char* key, bool* out) {
    const gguf_kv_t* kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_T_BOOL) return false;
    *out = kv->val[0] != 0;
    return true;
}

bool gguf_get_str(const gguf_file_t* g, const char* key, mind_str_t* out) {
    const gguf_kv_t* kv = gguf_find(g, key);
    if (!kv || kv->type != GGUF_T_STR) return false;
    uint64_t n = le_n(kv->val, 8);
    out->p = kv->val + 8;
    out->len = n;
    return true;
}

bool gguf_arr_next_str(const gguf_file_t* g, const uint8_t** cursor, mind_str_t* out) {
    const uint8_t* p = *cursor;
    const uint8_t* end = g->base + g->size;
    if ((uint64_t)(end - p) < 8) return false;
    uint64_t n = le_n(p, 8);
    if ((uint64_t)(end - p - 8) < n) return false;
    out->p = p + 8;
    out->len = n;
    *cursor = p + 8 + n;
    return true;
}

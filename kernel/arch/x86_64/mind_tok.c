// ============================================================================
// mind_tok.c — токенизаторы SPM и GPT2-BPE (этапы 49, 49.1). Контракт — mind_tok.h.
// ----------------------------------------------------------------------------
// Таблицы строятся один раз при загрузке модели (kmalloc, несколько МБ):
//   hash   — строка токена -> id (FNV-1a, открытая адресация);
//   merges — пара (id_left, id_right) -> (ранг, id результата) для BPE.
// Кодирование вызывается только на BSP (память под символы — kmalloc на
// время вызова). Декодирование (mind_tok_piece) — чистая функция без
// аллокаций: её зовёт и генератор на AP, чтобы заметить тег инструмента.
// ============================================================================
#include "mind_tok.h"
#include "mind_util.h"
#include "mind_unicode.h"
#include "heap.h"

static void set_err(char* err, int errlen, const char* a) {
    if (!err || errlen <= 0) return;
    err[0] = 0;
    m_append(err, (uint64_t)errlen, a);
}

static uint32_t fnv(const uint8_t* p, uint64_t n) {
    uint32_t h = 2166136261u;
    for (uint64_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static int lookup(const mind_tok_t* t, const uint8_t* p, uint64_t n) {
    uint32_t i = fnv(p, n) & t->hash_mask;
    for (;;) {
        int32_t id = t->hash[i];
        if (id < 0) return -1;
        const mind_str_t* s = &t->pieces[id];
        if (s->len == n) {
            uint64_t k = 0;
            while (k < n && s->p[k] == p[k]) k++;
            if (k == n) return id;
        }
        i = (i + 1) & t->hash_mask;
    }
}

int mind_tok_find(const mind_tok_t* t, const char* s) {
    return lookup(t, (const uint8_t*)s, m_strlen(s));
}

static uint64_t mkey(int a, int b) { return ((uint64_t)(uint32_t)a << 32) | (uint32_t)b; }

static bool merge_lookup(const mind_tok_t* t, int a, int b, int* rank, int* res) {
    uint64_t k = mkey(a, b);
    uint32_t i = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & t->mmask;
    for (;;) {
        if (t->mkey[i] == ~0ull) return false;
        if (t->mkey[i] == k) { *rank = t->mrank[i]; *res = t->mres[i]; return true; }
        i = (i + 1) & t->mmask;
    }
}

static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Кодовая точка -> UTF-8.
static int utf8_put(uint32_t cp, uint8_t* o) {
    if (cp < 0x80) { o[0] = (uint8_t)cp; return 1; }
    if (cp < 0x800) { o[0] = (uint8_t)(0xC0 | (cp >> 6)); o[1] = (uint8_t)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (uint8_t)(0xE0 | (cp >> 12)); o[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (uint8_t)(0x80 | (cp & 0x3F)); return 3;
    }
    o[0] = (uint8_t)(0xF0 | (cp >> 18)); o[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (uint8_t)(0x80 | (cp & 0x3F)); return 4;
}

// UTF-8 -> кодовая точка; *adv — длина. Битые байты отдаются как есть.
static uint32_t utf8_get(const uint8_t* p, int n, int* adv) {
    uint8_t c = p[0];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (len > n) len = 1;
    for (int i = 1; i < len; i++) if ((p[i] & 0xC0) != 0x80) { len = 1; break; }
    *adv = len;
    if (len == 1) return c;
    uint32_t cp = c & (0x7F >> len);
    for (int i = 1; i < len; i++) cp = (cp << 6) | (p[i] & 0x3F);
    return cp;
}

// ---- юникод-категории \p{L}, \p{N}, \s --------------------------------------
// L и N — по таблицам диапазонов из unicodedata (mind_unicode.h, этап 49.1),
// \s — свойство White_Space (как в регулярках onig/HF и llama.cpp).
static bool is_space(uint32_t c) {
    return c == ' ' || (c >= 9 && c <= 13) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
           c == 0x205F || c == 0x3000;
}
static bool in_ranges(const uint32_t (*r)[2], int n, uint32_t c) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c < r[mid][0]) hi = mid - 1;
        else if (c > r[mid][1]) lo = mid + 1;
        else return true;
    }
    return false;
}
static bool is_number(uint32_t c) {
    if (c < 0x80) return c >= '0' && c <= '9';
    return in_ranges(k_uni_N, (int)(sizeof(k_uni_N) / sizeof(k_uni_N[0])), c);
}
static bool is_letter(uint32_t c) {
    if (c < 0x80) return (c | 0x20) >= 'a' && (c | 0x20) <= 'z';
    return in_ranges(k_uni_L, (int)(sizeof(k_uni_L) / sizeof(k_uni_L[0])), c);
}

// ---- bytes_to_unicode (GPT-2) --------------------------------------------------
static void build_byte_map(uint16_t* b2u) {
    int n = 0;
    for (int b = 0; b < 256; b++) {
        bool keep = (b >= 0x21 && b <= 0x7E) || (b >= 0xA1 && b <= 0xAC) || (b >= 0xAE);
        b2u[b] = keep ? (uint16_t)b : (uint16_t)(256 + n++);
    }
}

int mind_tok_init(mind_tok_t* t, const gguf_file_t* g, char* err, int errlen) {
    m_memset(t, 0, sizeof(*t));
    mind_str_t model;
    if (!gguf_get_str(g, "tokenizer.ggml.model", &model)) { set_err(err, errlen, "no tokenizer in model"); return -1; }
    if (m_lstreq(model.p, model.len, "llama")) t->kind = MIND_TOK_SPM;
    else if (m_lstreq(model.p, model.len, "gpt2")) t->kind = MIND_TOK_BPE;
    else { set_err(err, errlen, "unsupported tokenizer (need llama or gpt2)"); return -1; }

    const gguf_kv_t* tk = gguf_find(g, "tokenizer.ggml.tokens");
    if (!tk || tk->type != GGUF_T_ARR || tk->arr_type != GGUF_T_STR || tk->arr_n == 0 || tk->arr_n > 1000000) {
        set_err(err, errlen, "bad tokenizer.ggml.tokens");
        return -1;
    }
    t->n_vocab = (int)tk->arr_n;
    uint32_t hsize = 1;
    while (hsize < (uint32_t)t->n_vocab * 2) hsize <<= 1;
    t->hash_mask = hsize - 1;
    t->pieces = (mind_str_t*)kmalloc((uint64_t)t->n_vocab * sizeof(mind_str_t));
    t->types = (int32_t*)kmalloc((uint64_t)t->n_vocab * 4);
    t->hash = (int32_t*)kmalloc((uint64_t)hsize * 4);
    if (!t->pieces || !t->types || !t->hash) { set_err(err, errlen, "out of memory (vocab)"); mind_tok_free(t); return -1; }
    for (uint32_t i = 0; i < hsize; i++) t->hash[i] = -1;

    const uint8_t* cur = tk->val;
    for (int i = 0; i < t->n_vocab; i++) {
        if (!gguf_arr_next_str(g, &cur, &t->pieces[i])) { set_err(err, errlen, "truncated vocab"); mind_tok_free(t); return -1; }
        t->types[i] = 1;
        // Первый токен с данной строкой выигрывает (как в llama.cpp token_to_id).
        if (lookup(t, t->pieces[i].p, t->pieces[i].len) < 0) {
            uint32_t h = fnv(t->pieces[i].p, t->pieces[i].len) & t->hash_mask;
            while (t->hash[h] >= 0) h = (h + 1) & t->hash_mask;
            t->hash[h] = i;
        }
    }
    const gguf_kv_t* tt = gguf_find(g, "tokenizer.ggml.token_type");
    if (tt && tt->type == GGUF_T_ARR && (tt->arr_type == GGUF_T_I32 || tt->arr_type == GGUF_T_U32) &&
        tt->arr_n == (uint64_t)t->n_vocab) {
        for (int i = 0; i < t->n_vocab; i++) t->types[i] = (int32_t)rd32(tt->val + 4 * (uint64_t)i);
    }

    uint32_t u;
    t->bos = gguf_get_u32(g, "tokenizer.ggml.bos_token_id", &u) ? (int)u : 1;
    t->eos = gguf_get_u32(g, "tokenizer.ggml.eos_token_id", &u) ? (int)u : 2;
    t->unk = gguf_get_u32(g, "tokenizer.ggml.unknown_token_id", &u) ? (int)u : 0;
    if (t->bos >= t->n_vocab) t->bos = -1;
    if (t->eos >= t->n_vocab) t->eos = -1;
    if (t->unk >= t->n_vocab) t->unk = 0;
    t->im_start = mind_tok_find(t, "<|im_start|>");
    t->im_end = mind_tok_find(t, "<|im_end|>");
    t->endoftext = mind_tok_find(t, "<|endoftext|>");
    t->think_open = mind_tok_find(t, "<think>");
    t->think_close = mind_tok_find(t, "</think>");
    t->tool_call_open = mind_tok_find(t, "<tool_call>");
    t->tool_call_close = mind_tok_find(t, "</tool_call>");

    bool b;
    t->add_bos = (t->kind == MIND_TOK_SPM);
    if (gguf_get_bool(g, "tokenizer.ggml.add_bos_token", &b)) t->add_bos = b;
    t->add_space_prefix = (t->kind == MIND_TOK_SPM);
    if (gguf_get_bool(g, "tokenizer.ggml.add_space_prefix", &b)) t->add_space_prefix = b;

    for (int i = 0; i < 512; i++) t->cp_to_byte[i] = -1;
    if (t->kind == MIND_TOK_SPM) {
        const gguf_kv_t* sc = gguf_find(g, "tokenizer.ggml.scores");
        t->scores = (float*)kmalloc((uint64_t)t->n_vocab * 4);
        if (!t->scores) { set_err(err, errlen, "out of memory (scores)"); mind_tok_free(t); return -1; }
        for (int i = 0; i < t->n_vocab; i++) t->scores[i] = 0.0f;
        if (sc && sc->type == GGUF_T_ARR && sc->arr_type == GGUF_T_F32 && sc->arr_n == (uint64_t)t->n_vocab) {
            for (int i = 0; i < t->n_vocab; i++) {
                union { uint32_t u; float f; } v;
                v.u = rd32(sc->val + 4 * (uint64_t)i);
                t->scores[i] = v.f;
            }
        }
        static const char hx[] = "0123456789ABCDEF";
        for (int bb = 0; bb < 256; bb++) {
            char nm[7] = { '<', '0', 'x', hx[bb >> 4], hx[bb & 15], '>', 0 };
            t->byte_tok[bb] = mind_tok_find(t, nm);
        }
        return 0;
    }

    // ---- BPE --------------------------------------------------------------------
    mind_str_t pre;
    t->pre = MIND_PRE_GPT2;
    if (gguf_get_str(g, "tokenizer.ggml.pre", &pre)) {
        if (m_lstreq(pre.p, pre.len, "smollm")) t->pre = MIND_PRE_SMOLLM;
        else if (m_lstreq(pre.p, pre.len, "qwen2")) t->pre = MIND_PRE_QWEN2;
    }
    // Спецтокены для разбиения текста: CONTROL и USER_DEFINED длиной > 1,
    // по убыванию длины (жадно — самый длинный подходящий).
    for (int i = 0; i < t->n_vocab && t->n_special < MIND_TOK_MAX_SPECIAL; i++) {
        if ((t->types[i] != 3 && t->types[i] != 4) || t->pieces[i].len < 2) continue;
        int k = t->n_special++;
        while (k > 0 && t->pieces[t->special[k - 1]].len < t->pieces[i].len) {
            t->special[k] = t->special[k - 1];
            k--;
        }
        t->special[k] = i;
        uint8_t fb = t->pieces[i].p[0];
        t->special_first[fb >> 3] |= (uint8_t)(1u << (fb & 7));
    }
    uint16_t b2u[256];
    build_byte_map(b2u);
    for (int bb = 0; bb < 256; bb++) {
        uint8_t o[4];
        int n = utf8_put(b2u[bb], o);
        t->byte_tok[bb] = lookup(t, o, (uint64_t)n);
        t->cp_to_byte[b2u[bb]] = (int16_t)bb;
    }
    const gguf_kv_t* mg = gguf_find(g, "tokenizer.ggml.merges");
    if (!mg || mg->type != GGUF_T_ARR || mg->arr_type != GGUF_T_STR || mg->arr_n > 2000000) {
        set_err(err, errlen, "bad tokenizer.ggml.merges");
        mind_tok_free(t);
        return -1;
    }
    uint32_t msize = 1;
    while (msize < (uint32_t)mg->arr_n * 2 + 2) msize <<= 1;
    t->mmask = msize - 1;
    t->mkey = (uint64_t*)kmalloc((uint64_t)msize * 8);
    t->mrank = (int32_t*)kmalloc((uint64_t)msize * 4);
    t->mres = (int32_t*)kmalloc((uint64_t)msize * 4);
    if (!t->mkey || !t->mrank || !t->mres) { set_err(err, errlen, "out of memory (merges)"); mind_tok_free(t); return -1; }
    for (uint32_t i = 0; i < msize; i++) t->mkey[i] = ~0ull;
    cur = mg->val;
    uint8_t cat[256];
    for (uint64_t r = 0; r < mg->arr_n; r++) {
        mind_str_t s;
        if (!gguf_arr_next_str(g, &cur, &s)) break;
        uint64_t sp = 0;
        while (sp < s.len && s.p[sp] != ' ') sp++;
        if (sp == 0 || sp >= s.len - 1 || s.len - 1 > sizeof(cat)) continue;
        int a = lookup(t, s.p, sp);
        int bid = lookup(t, s.p + sp + 1, s.len - sp - 1);
        m_memcpy(cat, s.p, sp);
        m_memcpy(cat + sp, s.p + sp + 1, s.len - sp - 1);
        int res = lookup(t, cat, s.len - 1);
        if (a < 0 || bid < 0 || res < 0) continue;
        uint64_t k = mkey(a, bid);
        uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 40) & t->mmask;
        while (t->mkey[h] != ~0ull && t->mkey[h] != k) h = (h + 1) & t->mmask;
        if (t->mkey[h] == k) continue;                 // дубль: остаётся меньший ранг
        t->mkey[h] = k;
        t->mrank[h] = (int32_t)r;
        t->mres[h] = res;
    }
    return 0;
}

void mind_tok_free(mind_tok_t* t) {
    if (t->pieces) kfree(t->pieces);
    if (t->types) kfree(t->types);
    if (t->hash) kfree(t->hash);
    if (t->scores) kfree(t->scores);
    if (t->mkey) kfree(t->mkey);
    if (t->mrank) kfree(t->mrank);
    if (t->mres) kfree(t->mres);
    m_memset(t, 0, sizeof(*t));
}

bool mind_tok_is_eog(const mind_tok_t* t, int id) {
    return id >= 0 && (id == t->eos || id == t->im_end || id == t->endoftext);
}

// ----------------------------------------------------------------------------
// SentencePiece
// ----------------------------------------------------------------------------
static int encode_spm(const mind_tok_t* t, const uint8_t* text, int len, int32_t* out, int max) {
    int cap = len * 3 + 4;
    uint8_t* buf = (uint8_t*)kmalloc((uint64_t)cap);
    int* st = (int*)kmalloc((uint64_t)cap * 4);
    int* sl = (int*)kmalloc((uint64_t)cap * 4);
    int* nx = (int*)kmalloc((uint64_t)cap * 4);
    int n = 0, ns = 0, cnt = 0;
    if (!buf || !st || !sl || !nx) { cnt = -1; goto out; }
    if (t->add_space_prefix) { buf[n++] = 0xE2; buf[n++] = 0x96; buf[n++] = 0x81; }
    for (int i = 0; i < len; i++) {
        if (text[i] == ' ') { buf[n++] = 0xE2; buf[n++] = 0x96; buf[n++] = 0x81; }
        else buf[n++] = text[i];
    }
    for (int i = 0; i < n;) {
        int adv;
        (void)utf8_get(buf + i, n - i, &adv);
        st[ns] = i; sl[ns] = adv; nx[ns] = ns + 1; ns++;
        i += adv;
    }
    if (ns) nx[ns - 1] = -1;
    for (;;) {
        int best = -1;
        float best_sc = 0.0f;
        for (int i = 0; i >= 0 && i < ns && nx[i] >= 0; i = nx[i]) {
            int j = nx[i];
            int id = lookup(t, buf + st[i], (uint64_t)(sl[i] + sl[j]));
            if (id >= 0 && (best < 0 || t->scores[id] > best_sc)) { best = i; best_sc = t->scores[id]; }
        }
        if (best < 0) break;
        int j = nx[best];
        sl[best] += sl[j];
        nx[best] = nx[j];
    }
    for (int i = 0; ns && i >= 0; i = nx[i]) {
        int id = lookup(t, buf + st[i], (uint64_t)sl[i]);
        if (id >= 0) {
            if (cnt >= max) { cnt = -1; goto out; }
            out[cnt++] = id;
        } else {
            for (int k = 0; k < sl[i]; k++) {
                if (cnt >= max) { cnt = -1; goto out; }
                int bt = t->byte_tok[buf[st[i] + k]];
                out[cnt++] = bt >= 0 ? bt : t->unk;
            }
        }
    }
out:
    if (buf) kfree(buf);
    if (st) kfree(st);
    if (sl) kfree(sl);
    if (nx) kfree(nx);
    return cnt;
}

// ----------------------------------------------------------------------------
// GPT2 byte-level BPE
// ----------------------------------------------------------------------------
#define BPE_MAX_WORD 256

static int bpe_word(const mind_tok_t* t, const uint8_t* w, int n, int32_t* out, int max, int cnt) {
    int32_t sym[BPE_MAX_WORD];
    int ns = 0;
    for (int i = 0; i < n; i++) {
        int id = t->byte_tok[w[i]];
        sym[ns++] = id >= 0 ? id : t->unk;
    }
    while (ns > 1) {
        int best = -1, best_rank = 0, best_res = 0;
        for (int i = 0; i + 1 < ns; i++) {
            int rank, res;
            if (merge_lookup(t, sym[i], sym[i + 1], &rank, &res) && (best < 0 || rank < best_rank)) {
                best = i; best_rank = rank; best_res = res;
            }
        }
        if (best < 0) break;
        sym[best] = best_res;
        for (int i = best + 1; i + 1 < ns; i++) sym[i] = sym[i + 1];
        ns--;
    }
    for (int i = 0; i < ns; i++) {
        if (cnt >= max) return -1;
        out[cnt++] = sym[i];
    }
    return cnt;
}

// Слово [a, b) в кодовых точках -> токены (длинные слова режем по 256 байт).
static int emit_word(const mind_tok_t* t, const uint8_t* text, const int* offs, int a, int b,
                     int32_t* out, int max, int cnt) {
    int bs = offs[a], be = offs[b];
    while (bs < be && cnt >= 0) {
        int n = be - bs;
        if (n > BPE_MAX_WORD) {
            n = BPE_MAX_WORD;
            while (n > 1 && (text[bs + n] & 0xC0) == 0x80) n--;   // не рвать UTF-8
        }
        cnt = bpe_word(t, text + bs, n, out, max, cnt);
        bs += n;
    }
    return cnt;
}

// Регулярка GPT-2 по куску [a, b) (порт unicode_regex_split_custom_gpt2):
// 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
static int split_gpt2(const mind_tok_t* t, const uint8_t* text, const uint32_t* cp, const int* offs,
                      int a, int b, int32_t* out, int max, int cnt) {
    int pos = a;
    while (pos < b && cnt >= 0) {
        uint32_t c = cp[pos];
        if (c == '\'' && pos + 1 < b) {
            uint32_t n1 = cp[pos + 1];
            if (n1 == 's' || n1 == 't' || n1 == 'm' || n1 == 'd') {
                cnt = emit_word(t, text, offs, pos, pos + 2, out, max, cnt); pos += 2; continue;
            }
            if (pos + 2 < b) {
                uint32_t n2 = cp[pos + 2];
                if ((n1 == 'r' && n2 == 'e') || (n1 == 'v' && n2 == 'e') || (n1 == 'l' && n2 == 'l')) {
                    cnt = emit_word(t, text, offs, pos, pos + 3, out, max, cnt); pos += 3; continue;
                }
            }
        }
        int j = (c == ' ' && pos + 1 < b) ? pos + 1 : pos;
        uint32_t cj = (j < b) ? cp[j] : 0;
        bool jin = j < b;
        if (jin && is_letter(cj)) {
            int k = j;
            while (k < b && is_letter(cp[k])) k++;
            cnt = emit_word(t, text, offs, pos, k, out, max, cnt); pos = k; continue;
        }
        if (jin && is_number(cj)) {
            int k = j;
            while (k < b && is_number(cp[k])) k++;
            cnt = emit_word(t, text, offs, pos, k, out, max, cnt); pos = k; continue;
        }
        if (jin && !is_space(cj) && !is_letter(cj) && !is_number(cj)) {
            int k = j;
            while (k < b && !is_space(cp[k]) && !is_letter(cp[k]) && !is_number(cp[k])) k++;
            cnt = emit_word(t, text, offs, pos, k, out, max, cnt); pos = k; continue;
        }
        int ws = 0;
        while (pos + ws < b && is_space(cp[pos + ws])) ws++;
        if (ws > 1 && pos + ws < b) {
            cnt = emit_word(t, text, offs, pos, pos + ws - 1, out, max, cnt); pos += ws - 1; continue;
        }
        if (ws > 0) {
            cnt = emit_word(t, text, offs, pos, pos + ws, out, max, cnt); pos += ws; continue;
        }
        cnt = emit_word(t, text, offs, pos, pos + 1, out, max, cnt); pos += 1;
    }
    return cnt;
}

// Регулярка Qwen2 (tokenizer.json Qwen2/2.5/3; llama.cpp PRE_TYPE_QWEN2):
//   (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}
//   | ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
// Отличия от GPT-2: сокращения без учёта регистра; перед буквами может
// стоять ЛЮБОЙ один не-буквенный, не-цифровой символ, кроме \r\n (не только
// пробел); цифры — строго по одной; пунктуация забирает хвостовые \r\n;
// пробелы, кончающиеся переводом строки, — отдельный кусок до последнего \n.
// Разбор — по альтернативам слева направо, как бэктрекинг регулярки
// (тот же алгоритм, что unicode_regex_split_custom_llama3 в llama.cpp, но с
// \p{N} вместо \p{N}{1,3}).
static bool is_crlf(uint32_t c) { return c == '\r' || c == '\n'; }
static uint32_t ascii_lower(uint32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int split_qwen2(const mind_tok_t* t, const uint8_t* text, const uint32_t* cp, const int* offs,
                       int a, int b, int32_t* out, int max, int cnt) {
    int pos = a;
    while (pos < b && cnt >= 0) {
        uint32_t c = cp[pos];
        // 1) сокращения
        if (c == '\'' && pos + 1 < b) {
            uint32_t n1 = ascii_lower(cp[pos + 1]);
            if (n1 == 's' || n1 == 't' || n1 == 'm' || n1 == 'd') {
                cnt = emit_word(t, text, offs, pos, pos + 2, out, max, cnt); pos += 2; continue;
            }
            if (pos + 2 < b) {
                uint32_t n2 = ascii_lower(cp[pos + 2]);
                if ((n1 == 'r' && n2 == 'e') || (n1 == 'v' && n2 == 'e') || (n1 == 'l' && n2 == 'l')) {
                    cnt = emit_word(t, text, offs, pos, pos + 3, out, max, cnt); pos += 3; continue;
                }
            }
        }
        // 2) [^\r\n\p{L}\p{N}]?\p{L}+
        if (is_letter(c) || (!is_crlf(c) && !is_number(c) && pos + 1 < b && is_letter(cp[pos + 1]))) {
            int k = pos + 1;
            while (k < b && is_letter(cp[k])) k++;
            cnt = emit_word(t, text, offs, pos, k, out, max, cnt); pos = k; continue;
        }
        // 3) \p{N} — одна цифра
        if (is_number(c)) { cnt = emit_word(t, text, offs, pos, pos + 1, out, max, cnt); pos++; continue; }
        // 4) ' ?[^\s\p{L}\p{N}]+[\r\n]*'
        int j = (c == ' ' && pos + 1 < b) ? pos + 1 : pos;
        if (j < b && !is_space(cp[j]) && !is_letter(cp[j]) && !is_number(cp[j])) {
            int k = j;
            while (k < b && !is_space(cp[k]) && !is_letter(cp[k]) && !is_number(cp[k])) k++;
            while (k < b && is_crlf(cp[k])) k++;
            cnt = emit_word(t, text, offs, pos, k, out, max, cnt); pos = k; continue;
        }
        // 5..7) пробельные
        int k = pos, last_nl = -1;
        while (k < b && is_space(cp[k])) { if (is_crlf(cp[k])) last_nl = k; k++; }
        if (last_nl >= 0) {                                        // \s*[\r\n]+
            cnt = emit_word(t, text, offs, pos, last_nl + 1, out, max, cnt); pos = last_nl + 1; continue;
        }
        if (k - pos > 1 && k < b) {                                // \s+(?!\S)
            cnt = emit_word(t, text, offs, pos, k - 1, out, max, cnt); pos = k - 1; continue;
        }
        if (k > pos) { cnt = emit_word(t, text, offs, pos, k, out, max, cnt); pos = k; continue; }   // \s+
        cnt = emit_word(t, text, offs, pos, pos + 1, out, max, cnt); pos++;                       // иначе
    }
    return cnt;
}

// Кусок текста без спецтокенов -> претокенизация по tokenizer.ggml.pre -> BPE.
static int encode_bpe_plain(const mind_tok_t* t, const uint8_t* text, int len, int32_t* out, int max, int cnt) {
    if (len <= 0) return cnt;
    uint32_t* cp = (uint32_t*)kmalloc((uint64_t)(len + 1) * 4);
    int* offs = (int*)kmalloc((uint64_t)(len + 1) * 4);
    if (!cp || !offs) { cnt = -1; goto out; }
    int n = 0;
    for (int i = 0; i < len;) {
        int adv;
        cp[n] = utf8_get(text + i, len - i, &adv);
        offs[n++] = i;
        i += adv;
    }
    offs[n] = len;
    if (t->pre == MIND_PRE_QWEN2) {
        cnt = split_qwen2(t, text, cp, offs, 0, n, out, max, cnt);
        goto out;
    }
    // \p{N} отдельными кусками (pre = smollm), остальное — регуляркой GPT-2.
    int start = 0;
    for (int i = 0; i <= n && cnt >= 0; i++) {
        bool cut = (i == n) || (t->pre == MIND_PRE_SMOLLM && is_number(cp[i]));
        if (!cut) continue;
        if (i > start) cnt = split_gpt2(t, text, cp, offs, start, i, out, max, cnt);
        if (i < n && cnt >= 0) cnt = emit_word(t, text, offs, i, i + 1, out, max, cnt);
        start = i + 1;
    }
out:
    if (cp) kfree(cp);
    if (offs) kfree(offs);
    return cnt;
}

// Спецтокен, начинающийся в text[i] (самый длинный), или -1.
static int special_at(const mind_tok_t* t, const uint8_t* text, int len, int i, int flags) {
    uint8_t fb = text[i];
    if (!(t->special_first[fb >> 3] & (1u << (fb & 7)))) return -1;
    for (int k = 0; k < t->n_special; k++) {
        int id = t->special[k];
        if (t->types[id] == 3 && !(flags & MIND_TOK_PARSE_CONTROL)) continue;
        const mind_str_t* s = &t->pieces[id];
        if ((uint64_t)(len - i) < s->len) continue;
        uint64_t q = 0;
        while (q < s->len && s->p[q] == text[i + (int)q]) q++;
        if (q == s->len) return id;
    }
    return -1;
}

static int encode_bpe(const mind_tok_t* t, const uint8_t* text, int len, int flags,
                      int32_t* out, int max, int cnt) {
    // Разбиение по спецтокенам: куски между ними кодируются независимо —
    // как tokenizer_st_partition в llama.cpp и added_tokens в HF.
    int start = 0;
    for (int i = 0; i < len && cnt >= 0;) {
        int id = t->n_special ? special_at(t, text, len, i, flags) : -1;
        if (id < 0) { i++; continue; }
        cnt = encode_bpe_plain(t, text + start, i - start, out, max, cnt);
        if (cnt < 0) return -1;
        if (cnt >= max) return -1;
        out[cnt++] = id;
        i += (int)t->pieces[id].len;
        start = i;
    }
    if (cnt >= 0) cnt = encode_bpe_plain(t, text + start, len - start, out, max, cnt);
    return cnt;
}

static int encode_any(const mind_tok_t* t, const char* text, int len, bool add_bos, int flags,
                      int32_t* out, int max) {
    int cnt = 0;
    if (add_bos && t->bos >= 0) {
        if (max < 1) return -1;
        out[cnt++] = t->bos;
    }
    if (len <= 0) return cnt;
    if (t->kind == MIND_TOK_SPM) {
        int r = encode_spm(t, (const uint8_t*)text, len, out + cnt, max - cnt);
        return r < 0 ? -1 : cnt + r;
    }
    return encode_bpe(t, (const uint8_t*)text, len, flags, out, max, cnt);
}

int mind_tok_encode(const mind_tok_t* t, const char* text, int len, bool add_bos,
                    int32_t* out, int max) {
    return encode_any(t, text, len, add_bos, 0, out, max);
}

int mind_tok_encode_ex(const mind_tok_t* t, const char* text, int len, int flags,
                       int32_t* out, int max) {
    return encode_any(t, text, len, false, flags, out, max);
}

int mind_tok_piece(const mind_tok_t* t, int id, char* out, int cap) {
    if (id < 0 || id >= t->n_vocab || cap <= 0) return 0;
    int ty = t->types[id];
    if (ty == 3 || ty == 5) return 0;                        // control / unused (PAD)
    const mind_str_t* s = &t->pieces[id];
    int n = 0;
    if (t->kind == MIND_TOK_SPM) {
        if (ty == 6 && s->len == 6) {
            uint8_t v = 0;
            for (int k = 3; k < 5; k++) {
                uint8_t c = s->p[k];
                v = (uint8_t)(v * 16 + (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10));
            }
            out[n++] = (char)v;
            return n;
        }
        for (uint64_t i = 0; i < s->len && n < cap; i++) {
            if (i + 2 < s->len && s->p[i] == 0xE2 && s->p[i + 1] == 0x96 && s->p[i + 2] == 0x81) {
                out[n++] = ' ';
                i += 2;
            } else {
                out[n++] = (char)s->p[i];
            }
        }
        return n;
    }
    if (ty == 4) {                                           // user-defined — как есть
        for (uint64_t i = 0; i < s->len && n < cap; i++) out[n++] = (char)s->p[i];
        return n;
    }
    for (uint64_t i = 0; i < s->len && n < cap;) {
        int adv;
        uint32_t c = utf8_get(s->p + i, (int)(s->len - i), &adv);
        int bv = c < 512 ? t->cp_to_byte[c] : -1;
        out[n++] = bv >= 0 ? (char)bv : '?';
        i += (uint64_t)adv;
    }
    return n;
}

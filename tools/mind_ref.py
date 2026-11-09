#!/usr/bin/env python3
# ============================================================================
# tools/mind_ref.py — эталонная реализация инференса AMPLIFY MIND (этапы 49, 49.1).
# ----------------------------------------------------------------------------
# Независимая от ядра реализация на numpy: свой GGUF-ридер, деквантизация,
# forward-pass архитектур llama и qwen3, токенизаторы SentencePiece и
# GPT2-BPE (претокенизация gpt2 / smollm / qwen2). Ядро
# (kernel/arch/x86_64/mind_*.c) и этот файл написаны по формулам
# llama.cpp/HF, а не друг из друга; тесты сравнивают ids токенов
# greedy-генерации ядра (и его хост-сборки tools/mind_host.c) с этим скриптом.
#
# Числа как в ядре (а оно — как llama.cpp): для Q8_0/Q4_0-весов активации
# перед умножением квантуются в Q8_0 (блок 32, d = amax/127, округление
# «+0.5 и отбросить» во float32), d хранится в fp16.
#
# ПОБИТОВАЯ АРИФМЕТИКА (с 49.1). Формулы слоёв написаны здесь независимо
# (по llama.cpp/HF), а ПОРЯДОК операций float32 повторяет ядро: сумма блоков
# — последовательно во float32, скалярное произведение внимания — как SSE
# (2×4 аккумулятора), expf — тот же ряд, что m_expf, суммы softmax/RMSNorm
# — последовательно в double, таблицы RoPE — те же m_exp/m_log/m_sincos.
# Зачем: квантование активаций превращает разницу в 1 ulp в целую единицу
# q, и к 30-му слою расхождения «эквивалентных» реализаций доходят до
# ~0.15 логита — greedy-ids 1.7B-модели на длинном промпте совпадали бы
# случайно. Побитовое совпадение делает сверку детерминированной, а
# логические ошибки (NEOX/NORM, головы GQA, QK-norm) по-прежнему видны сразу.
#
# Этап 49.1 (Qwen3-1.7B, 1.8 ГБ весов):
#   * веса не копируются: GGUF открывается через np.memmap, Q8_0 хранится
#     как int8-вид на файл (раньше int32 — для 1.7B это было бы ~7 ГБ);
#   * пакетный forward: весь промпт — одним проходом матриц (numpy/BLAS),
#     причём результат тот же, что по одному токену (квантование — по
#     строкам, внимание — причинное);
#   * qwen3: QK-norm (RMSNorm по каждой голове q и k до RoPE), RoPE NEOX
#     (вращаются x[i] и x[i + n_rot/2]), head_dim из attention.key_length;
#   * токенизатор pre=qwen2 (регулярка Qwen2, см. pretok_qwen2) и разбиение
#     текста по спецтокенам: USER_DEFINED (<tool_call>, <think>...) — всегда,
#     CONTROL (<|im_start|>...) — только в доверенном тексте шаблона;
#   * промпт Qwen3 без «размышления» (пустой <think></think>, как
#     enable_thinking=False) и с описанием инструментов <tools>; команда
#     render сверяет его с ОФИЦИАЛЬНЫМ jinja-шаблоном из самого GGUF.
#
# Использование (Windows-python, numpy; для render — jinja2, для tokcheck —
# пакет tokenizers и tokenizer.json модели):
#   python tools/mind_ref.py info  <model.gguf>
#   python tools/mind_ref.py tok   <model.gguf> "<text>"
#   python tools/mind_ref.py gen   <model.gguf> "<prompt>" <n> [--chat] [--system=...]
#          [--tools=tools/mind_tools.json] [--think] [--prefix=<начало ответа>]
#          [--tool-response=<файл>]  (после </tool_call> — ответ инструмента и продолжение)
#   python tools/mind_ref.py gen-ids <model.gguf> <id,id,...> <n>
#   python tools/mind_ref.py render <model.gguf> "<user>" [--system=...] [--tools=...] [--think]
#   python tools/mind_ref.py tokcheck <model.gguf> <tokenizer.json> [файл-с-текстами]
# Вывод gen: строки "prompt: ...", "ids: ...", "text: ..." — их парсят тесты.
# ============================================================================
import json
import struct
import sys
import unicodedata

import numpy as np

# ---------------------------------------------------------------------------
# GGUF
# ---------------------------------------------------------------------------
SCALAR_FMT = {0: '<B', 1: '<b', 2: '<H', 3: '<h', 4: '<I', 5: '<i', 6: '<f',
              7: '<?', 10: '<Q', 11: '<q', 12: '<d'}

GGML_F32, GGML_F16, GGML_Q4_0, GGML_Q8_0, GGML_BF16 = 0, 1, 2, 8, 30
TYPE_NAME = {0: 'F32', 1: 'F16', 2: 'Q4_0', 3: 'Q4_1', 6: 'Q5_0', 7: 'Q5_1',
             8: 'Q8_0', 10: 'Q2_K', 11: 'Q3_K', 12: 'Q4_K', 13: 'Q5_K',
             14: 'Q6_K', 30: 'BF16'}


class GGUF:
    def __init__(self, path):
        # memmap: 1.8 ГБ весов не читаются в память, пока не понадобятся.
        self.raw = np.memmap(path, dtype=np.uint8, mode='r')
        self.buf = memoryview(self.raw)
        magic, self.version = struct.unpack_from('<II', self.buf, 0)
        assert magic == 0x46554747, 'not GGUF'
        self.pos = 8
        n_tensors = self.u64()
        n_kv = self.u64()
        self.meta = {}
        for _ in range(n_kv):
            k = self.string()
            t = self.u32()
            self.meta[k] = self.value(t)
        self.tensors = {}
        for _ in range(n_tensors):
            name = self.string()
            nd = self.u32()
            dims = [self.u64() for _ in range(nd)]
            ty = self.u32()
            off = self.u64()
            self.tensors[name] = (dims, ty, off)
        align = self.meta.get('general.alignment', 32)
        self.data_off = (self.pos + align - 1) // align * align

    def u32(self):
        v = struct.unpack_from('<I', self.buf, self.pos)[0]; self.pos += 4; return v

    def u64(self):
        v = struct.unpack_from('<Q', self.buf, self.pos)[0]; self.pos += 8; return v

    def string(self):
        n = self.u64()
        s = bytes(self.buf[self.pos:self.pos + n]); self.pos += n
        return s.decode('utf-8', errors='surrogateescape')

    def value(self, t):
        if t == 8:
            return self.string()
        if t == 9:
            et = self.u32(); n = self.u64()
            if et in SCALAR_FMT:
                fmt = SCALAR_FMT[et]; sz = struct.calcsize(fmt)
                arr = np.frombuffer(self.buf, dtype=np.dtype(fmt), count=n, offset=self.pos)
                self.pos += sz * n
                return arr
            return [self.value(et) for _ in range(n)]
        fmt = SCALAR_FMT[t]
        v = struct.unpack_from(fmt, self.buf, self.pos)[0]
        self.pos += struct.calcsize(fmt)
        return v

    # (тип, сырые байты — вид на memmap, dims). Строка матрицы = dims[0] элементов.
    def raw_tensor(self, name):
        dims, ty, off = self.tensors[name]
        n = int(np.prod(dims))
        if ty == GGML_F32:
            nb = n * 4
        elif ty in (GGML_F16, GGML_BF16):
            nb = n * 2
        elif ty == GGML_Q8_0:
            nb = n // 32 * 34
        elif ty == GGML_Q4_0:
            nb = n // 32 * 18
        else:
            raise SystemExit('unsupported tensor type %s for %s' % (TYPE_NAME.get(ty, ty), name))
        b = self.raw[self.data_off + off: self.data_off + off + nb]
        return ty, b, dims


def dequant(ty, b, dims):
    n = int(np.prod(dims))
    b = np.asarray(b)
    if ty == GGML_F32:
        x = b.view(np.float32).copy()
    elif ty == GGML_F16:
        x = b.view(np.float16).astype(np.float32)
    elif ty == GGML_BF16:
        x = (b.view(np.uint16).astype(np.uint32) << 16).view(np.float32)
    elif ty == GGML_Q8_0:
        blk = b.reshape(-1, 34)
        d = blk[:, :2].copy().view(np.float16).astype(np.float32)
        q = blk[:, 2:].view(np.int8).astype(np.float32)
        x = (q * d).reshape(-1)
    elif ty == GGML_Q4_0:
        blk = b.reshape(-1, 18)
        d = blk[:, :2].copy().view(np.float16).astype(np.float32)
        qs = blk[:, 2:]
        lo = (qs & 0x0F).astype(np.int8) - 8
        hi = (qs >> 4).astype(np.int8) - 8
        q = np.concatenate([lo, hi], axis=1).astype(np.float32)
        x = (q * d).reshape(-1)
    x = x[:n]
    if len(dims) == 1:
        return x
    return x.reshape(int(np.prod(dims[1:])), dims[0])   # [строки, dims[0]]


F32 = np.float32


def m_expf(x):
    # Точная копия m_expf (mind_util.h): x = n·ln2 + r, ряд до r^7, 2^n.
    x = np.minimum(np.asarray(x, dtype=F32), F32(88.0))
    zero = x < F32(-87.0)
    fn = (x * F32(1.44269504088896341)).astype(F32)
    n = np.trunc(np.where(fn >= 0, fn + F32(0.5), fn - F32(0.5)).astype(F32)).astype(np.int32)
    n = np.where(zero, 0, n)
    nf = n.astype(F32)
    r = (x - (nf * F32(0.693145751953125)).astype(F32)).astype(F32)
    r = (r - (nf * F32(1.428606765330187045e-06)).astype(F32)).astype(F32)
    t = (F32(0.0013888888889) + (r * F32(0.00019841269841)).astype(F32)).astype(F32)
    for c in (0.0083333333333, 0.041666666666, 0.16666666666, 0.5, 1.0, 1.0):
        t = (F32(c) + (r * t).astype(F32)).astype(F32)
    s = np.ldexp(F32(1.0), n).astype(F32)
    return np.where(zero, F32(0.0), (t * s).astype(F32)).astype(F32)


def dot_sse(A, B):
    # mind_dot_f32: два 4-лучевых SSE-аккумулятора по 8 элементов, затем
    # s0+s1, (t0+t1)+(t2+t3), хвост — последовательно. Широковещание A·B.
    n = A.shape[-1]
    m8 = n // 8 * 8
    prod = (A[..., :m8] * B[..., :m8]).astype(F32)
    prod = prod.reshape(prod.shape[:-1] + (m8 // 8, 2, 4))
    acc = np.zeros(prod.shape[:-3] + (2, 4), F32)
    for j in range(m8 // 8):
        acc = (acc + prod[..., j, :, :]).astype(F32)
    s0 = (acc[..., 0, :] + acc[..., 1, :]).astype(F32)
    tot = ((s0[..., 0] + s0[..., 1]).astype(F32) + (s0[..., 2] + s0[..., 3]).astype(F32)).astype(F32)
    for i in range(m8, n):
        tot = (tot + (A[..., i] * B[..., i]).astype(F32)).astype(F32)
    return tot


def seq_sum64(X):
    # Последовательная сумма в double по последней оси (как цикл в ядре).
    return np.cumsum(np.asarray(X, dtype=np.float64), axis=-1)[..., -1]


# ---- double-функции ядра (mind_util.h) — для таблиц RoPE -------------------
def c_exp(x):
    import math
    if x > 700.0:
        x = 700.0
    if x < -700.0:
        return 0.0
    fn = x * 1.4426950408889634
    n = int(fn + (0.5 if fn >= 0.0 else -0.5))
    r = x - float(n) * 6.93147180369123816490e-01
    r = r - float(n) * 1.90821492927058770002e-10
    term, acc = 1.0, 1.0
    for k in range(1, 17):
        term *= r / float(k)
        acc += term
    return acc * math.ldexp(1.0, n)


def c_log(x):
    import math
    if x <= 0.0:
        return -1e300
    mant, ex = math.frexp(x)
    m, e = mant * 2.0, ex - 1
    if m > 1.4142135623730951:
        m *= 0.5
        e += 1
    s = (m - 1.0) / (m + 1.0)
    s2, t, acc = s * s, s, 0.0
    for k in range(1, 40, 2):
        acc += t / float(k)
        t *= s2
    return 2.0 * acc + float(e) * 0.69314718055994530942


def c_sincos(x):
    pio2_hi = 1.57079632679489655800e+00
    pio2_lo = 6.12323399573676603587e-17
    kf = x * 0.63661977236758134308
    k = int(kf + (0.5 if kf >= 0.0 else -0.5))
    r = (x - float(k) * pio2_hi) - float(k) * pio2_lo
    r2 = r * r
    s, c, ts, tc = r, 1.0, r, 1.0
    for i in range(1, 11):
        ts *= -r2 / float((2 * i) * (2 * i + 1))
        tc *= -r2 / float((2 * i - 1) * (2 * i))
        s += ts
        c += tc
    q = k & 3
    if q == 0:
        return s, c
    if q == 1:
        return c, -s
    if q == 2:
        return -s, -c
    return -c, s


def quant_rows(X):
    # Квантование активаций построчно — ровно как mind_quantize_q8 в ядре:
    # dd = amax/127 (float32), id = 1/dd, q = (int)(v ± 0.5f), d -> fp16.
    T, n = X.shape
    xb = X.reshape(T, n // 32, 32).astype(F32)
    amax = np.abs(xb).max(axis=2).astype(F32)
    dd = (amax / F32(127.0)).astype(F32)
    safe = np.where(dd != 0, dd, F32(1.0)).astype(F32)
    idd = np.where(dd != 0, (F32(1.0) / safe).astype(F32), F32(0.0)).astype(F32)
    v = (xb * idd[:, :, None]).astype(F32)
    r = np.where(v >= 0, (v + F32(0.5)).astype(F32), (v - F32(0.5)).astype(F32))
    q = np.clip(np.trunc(r), -127, 127).astype(F32)
    d16 = dd.astype(np.float16).astype(F32)
    return q, d16                       # q: (T, nb, 32) целые во float32; d16: (T, nb)


class Weight:
    """Матрица [out, in] с семантикой умножения как у ядра (и ggml)."""

    def __init__(self, g, name):
        ty, b, dims = g.raw_tensor(name)
        self.ty = ty
        self.dims = dims
        self.cols = dims[0]
        self.nrows = int(np.prod(dims[1:])) if len(dims) > 1 else 1
        if ty in (GGML_Q8_0, GGML_Q4_0):
            nb = self.cols // 32
            if ty == GGML_Q8_0:
                blk = b.reshape(self.nrows, nb, 34)
                self.d = blk[:, :, :2].copy().view(np.float16).astype(F32)[:, :, 0]
                self.q8 = blk[:, :, 2:].view(np.int8)      # вид на memmap, без копии
                self.q4 = None
            else:
                blk = b.reshape(self.nrows, nb, 18)
                self.d = blk[:, :, :2].copy().view(np.float16).astype(F32)[:, :, 0]
                self.q4 = blk[:, :, 2:]
                self.q8 = None
            self.w = None
        else:
            self.w = dequant(ty, b, dims)

    def qrows(self, r0, r1):
        # Целые веса строк [r0, r1) во float32: (R, nb, 32).
        if self.q8 is not None:
            return self.q8[r0:r1].astype(F32)
        qs = self.q4[r0:r1]
        lo = (qs & 0x0F).astype(np.int16) - 8
        hi = (qs >> 4).astype(np.int16) - 8
        return np.concatenate([lo, hi], axis=2).astype(F32)

    def matmul(self, X):
        """X: (T, cols) -> (T, rows) во float32."""
        X = np.asarray(X, dtype=F32)
        if X.ndim == 1:
            X = X[None, :]
        if self.w is not None:
            return self.matmul_float(X)
        T = X.shape[0]
        nb = self.cols // 32
        qx, dx = quant_rows(X)
        qxt = np.ascontiguousarray(qx.transpose(1, 0, 2))          # (nb, T, 32)
        out = np.empty((T, self.nrows), F32)
        R = max(16, int(12_000_000 // max(1, nb * T)))              # кусок строк под память
        for r0 in range(0, self.nrows, R):
            r1 = min(self.nrows, r0 + R)
            q = self.qrows(r0, r1)                                  # (R, nb, 32)
            # Целые суммы блоков: (nb, T, 32) @ (nb, 32, R) -> (nb, T, R).
            # Во float32 они точны (|сумма| <= 32·127·127 < 2^24).
            isum = np.matmul(qxt, q.transpose(1, 2, 0))
            scale = (self.d[r0:r1].T[:, None, :] * dx.T[:, :, None]).astype(F32)   # d_w·d_x
            prod = (isum * scale).astype(F32)
            acc = np.zeros((T, r1 - r0), F32)
            for b in range(nb):                    # sum += isum·(d_w·d_x) — по блокам
                acc = (acc + prod[b]).astype(F32)
            out[:, r0:r1] = acc
        return out

    def matmul_float(self, X):
        # F32 — mind_dot_f32 (SSE), F16/BF16 — последовательный скалярный цикл.
        T = X.shape[0]
        out = np.empty((T, self.nrows), F32)
        R = max(1, int(8_000_000 // max(1, T * self.cols)))
        for r0 in range(0, self.nrows, R):
            W = self.w[r0:r0 + R].astype(F32)
            if self.ty == GGML_F32:
                out[:, r0:r0 + R] = dot_sse(W[None, :, :], X[:, None, :])
            else:
                acc = np.zeros((T, W.shape[0]), F32)
                for i in range(self.cols):
                    acc = (acc + (W[None, :, i] * X[:, None, i]).astype(F32)).astype(F32)
                out[:, r0:r0 + R] = acc
        return out

    def row(self, i):
        if self.w is not None:
            return self.w[i].copy()
        return (self.qrows(i, i + 1)[0] * self.d[i][:, None]).reshape(-1).astype(F32)


# ---------------------------------------------------------------------------
# Модель: llama и qwen3
# ---------------------------------------------------------------------------
def rmsnorm_rows(X, w, eps):
    # Как mind_rmsnorm: сумма квадратов последовательно в double,
    # scale = (float)(1/sqrt(ss/n + eps)), out = (x·scale)·w во float32.
    X = np.asarray(X, dtype=F32)
    x64 = X.astype(np.float64)
    ss = seq_sum64(x64 * x64)
    scale = (1.0 / np.sqrt(ss / float(X.shape[-1]) + float(F32(eps)))).astype(F32)
    return ((X * scale[..., None]).astype(F32) * w).astype(F32)


class Model:
    def __init__(self, g, n_ctx=512):
        m = g.meta
        a = m['general.architecture']
        assert a in ('llama', 'qwen3'), 'arch %s not supported' % a
        self.arch = a
        self.g = g
        self.n_embd = int(m[a + '.embedding_length'])
        self.n_layer = int(m[a + '.block_count'])
        self.n_ff = int(m[a + '.feed_forward_length'])
        self.n_head = int(m[a + '.attention.head_count'])
        self.n_head_kv = int(m.get(a + '.attention.head_count_kv', self.n_head))
        self.eps = float(m.get(a + '.attention.layer_norm_rms_epsilon', 1e-5))
        self.base = float(m.get(a + '.rope.freq_base', 10000.0))
        self.hd = int(m.get(a + '.attention.key_length', self.n_embd // self.n_head))
        self.n_rot = int(m.get(a + '.rope.dimension_count', self.hd))
        # Раскладка RoPE: llama — NORM (пары соседей), qwen3 — NEOX (половины).
        self.neox = (a == 'qwen3')
        self.n_ctx = n_ctx
        self.tok_embd = Weight(g, 'token_embd.weight')
        self.out_norm = dequant(*g.raw_tensor('output_norm.weight'))
        self.output = Weight(g, 'output.weight') if 'output.weight' in g.tensors else self.tok_embd
        self.layers = []
        for i in range(self.n_layer):
            p = 'blk.%d.' % i
            L = {k: Weight(g, p + k + '.weight') for k in
                 ('attn_q', 'attn_k', 'attn_v', 'attn_output', 'ffn_gate', 'ffn_up', 'ffn_down')}
            L['attn_norm'] = dequant(*g.raw_tensor(p + 'attn_norm.weight'))
            L['ffn_norm'] = dequant(*g.raw_tensor(p + 'ffn_norm.weight'))
            if p + 'attn_q_norm.weight' in g.tensors:
                L['q_norm'] = dequant(*g.raw_tensor(p + 'attn_q_norm.weight'))
                L['k_norm'] = dequant(*g.raw_tensor(p + 'attn_k_norm.weight'))
            self.layers.append(L)
        kvd = self.n_head_kv * self.hd
        self.kc = np.zeros((self.n_layer, n_ctx, kvd), F32)
        self.vc = np.zeros((self.n_layer, n_ctx, kvd), F32)
        # Таблицы RoPE теми же double-функциями, что в ядре (mind_model_init).
        half = self.n_rot // 2
        lb = c_log(float(F32(self.base)))
        self.cos = np.empty((n_ctx, half), F32)       # [pos][i]
        self.sin = np.empty((n_ctx, half), F32)
        for i in range(half):
            inv = c_exp(-(2.0 * float(i) / float(self.n_rot)) * lb)
            for p in range(n_ctx):
                s_, c_ = c_sincos(float(p) * inv)
                self.cos[p, i] = c_
                self.sin[p, i] = s_

    def rope(self, V, pos0, nh):
        # V: (T, nh*hd). Углы — pos * base^(-2i/n_rot), таблица в double -> float32.
        T = V.shape[0]
        V = V.reshape(T, nh, self.hd).copy()
        half = self.n_rot // 2
        c = self.cos[pos0:pos0 + T][:, None, :]
        s = self.sin[pos0:pos0 + T][:, None, :]
        if self.neox:
            x0 = V[:, :, :half].copy(); x1 = V[:, :, half:self.n_rot].copy()
            V[:, :, :half] = (x0 * c).astype(F32) - (x1 * s).astype(F32)
            V[:, :, half:self.n_rot] = (x0 * s).astype(F32) + (x1 * c).astype(F32)
        else:
            i = np.arange(0, self.n_rot, 2)
            x0 = V[:, :, i].copy(); x1 = V[:, :, i + 1].copy()
            V[:, :, i] = (x0 * c).astype(F32) - (x1 * s).astype(F32)
            V[:, :, i + 1] = (x0 * s).astype(F32) + (x1 * c).astype(F32)
        return V.reshape(T, nh * self.hd)

    def attention(self, li, Q, pos0, T):
        # Причинное внимание по арифметике attn_range ядра: score = dot·scale,
        # softmax (m_expf, сумма в double, умножение на (float)(1/sum)),
        # выход — axpy по позициям по порядку. Токены пакета — кусками по 16.
        hd, gq, KVH = self.hd, self.n_head // self.n_head_kv, self.n_head_kv
        scale = (F32(1.0) / np.sqrt(F32(hd)).astype(F32)).astype(F32)
        P = pos0 + T
        Kc = self.kc[li, :P].reshape(P, KVH, hd)
        Vc = self.vc[li, :P].reshape(P, KVH, hd)
        att = np.zeros((T, self.n_head * hd), F32)
        for t0 in range(0, T, 16):
            t1 = min(T, t0 + 16)
            Pc = pos0 + t1                                   # позиции, нужные куску
            qg = Q[t0:t1].reshape(t1 - t0, KVH, gq, 1, hd)
            kk = Kc[:Pc].transpose(1, 0, 2)[None, :, None, :, :]    # (1,KVH,1,P,hd)
            sc = (dot_sse(qg, kk) * scale).astype(F32)               # (t,KVH,gq,P)
            valid = (np.arange(Pc)[None, :] <= (pos0 + np.arange(t0, t1))[:, None])  # (t,P)
            vm = valid[:, None, None, :]
            mx = np.where(vm, sc, -np.inf).max(axis=3, keepdims=True).astype(F32)
            ex = np.where(vm, m_expf(np.where(vm, (sc - mx).astype(F32), F32(0.0))), F32(0.0)).astype(F32)
            sm = seq_sum64(ex)
            inv = (1.0 / sm).astype(F32)
            a = (ex * inv[..., None]).astype(F32)                     # (t,KVH,gq,P)
            out = np.zeros((t1 - t0, KVH, gq, hd), F32)
            for p in range(Pc):
                out = (out + (a[:, :, :, p, None] * Vc[p][None, :, None, :]).astype(F32)).astype(F32)
            att[t0:t1] = out.reshape(t1 - t0, -1)
        return att

    def forward(self, toks, pos0):
        """Пакет токенов на позициях pos0.. -> логиты ПОСЛЕДНЕГО токена."""
        T = len(toks)
        X = np.stack([self.tok_embd.row(int(t)) for t in toks]).astype(F32)
        hd = self.hd
        for li, L in enumerate(self.layers):
            H = rmsnorm_rows(X, L['attn_norm'], self.eps)
            Q = L['attn_q'].matmul(H)
            K = L['attn_k'].matmul(H)
            Vv = L['attn_v'].matmul(H)
            if 'q_norm' in L:
                # QK-norm: RMSNorm каждой головы (hd чисел) со своими весами.
                Q = rmsnorm_rows(Q.reshape(T, self.n_head, hd), L['q_norm'], self.eps).reshape(T, -1)
                K = rmsnorm_rows(K.reshape(T, self.n_head_kv, hd), L['k_norm'], self.eps).reshape(T, -1)
            Q = self.rope(Q, pos0, self.n_head)
            K = self.rope(K, pos0, self.n_head_kv)
            self.kc[li, pos0:pos0 + T] = K
            self.vc[li, pos0:pos0 + T] = Vv
            att = self.attention(li, Q, pos0, T)
            X = (X + L['attn_output'].matmul(att)).astype(F32)
            H = rmsnorm_rows(X, L['ffn_norm'], self.eps)
            G = L['ffn_gate'].matmul(H)
            U = L['ffn_up'].matmul(H)
            A = ((G / (F32(1.0) + m_expf(-G)).astype(F32)).astype(F32) * U).astype(F32)
            X = (X + L['ffn_down'].matmul(A)).astype(F32)
        x = rmsnorm_rows(X[-1:], self.out_norm, self.eps)
        return self.output.matmul(x)[0]


# ---------------------------------------------------------------------------
# Токенизаторы
# ---------------------------------------------------------------------------
def bytes_to_unicode():
    bs = list(range(ord('!'), ord('~') + 1)) + list(range(0xA1, 0xAC + 1)) + list(range(0xAE, 0xFF + 1))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b); cs.append(256 + n); n += 1
    return dict(zip(bs, [chr(c) for c in cs]))


# Юникод-категории как в регулярках токенизаторов: \p{L}, \p{N}, \s (White_Space).
_WS = set([0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0x85, 0xA0, 0x1680, 0x2028, 0x2029,
           0x202F, 0x205F, 0x3000] + list(range(0x2000, 0x200B)))


def is_L(ch):
    return unicodedata.category(ch)[0] == 'L'


def is_N(ch):
    return unicodedata.category(ch)[0] == 'N'


def is_S(ch):
    return ord(ch) in _WS


def pretok_gpt2(p):
    # 's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
    out = []
    i = 0
    n = len(p)
    while i < n:
        matched = False
        for c in ("'s", "'t", "'re", "'ve", "'m", "'ll", "'d"):
            if p.startswith(c, i):
                out.append(c); i += len(c); matched = True; break
        if matched:
            continue
        j = i + 1 if (p[i] == ' ' and i + 1 < n) else i
        if j < n and is_L(p[j]):
            k = j
            while k < n and is_L(p[k]):
                k += 1
            out.append(p[i:k]); i = k; continue
        if j < n and is_N(p[j]):
            k = j
            while k < n and is_N(p[k]):
                k += 1
            out.append(p[i:k]); i = k; continue
        if j < n and not is_S(p[j]):
            k = j
            while k < n and not is_S(p[k]) and not is_L(p[k]) and not is_N(p[k]):
                k += 1
            out.append(p[i:k]); i = k; continue
        k = i
        while k < n and is_S(p[k]):
            k += 1
        if k < n and k - i > 1:
            out.append(p[i:k - 1]); i = k - 1
        else:
            out.append(p[i:k if k > i else i + 1]); i = k if k > i else i + 1
    return out


def pretok_qwen2(p):
    # Регулярка Qwen2 (tokenizer.json модели и llama.cpp LLAMA_VOCAB_PRE_TYPE_QWEN2):
    # (?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}
    #  | ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+
    # Разбор — по альтернативам слева направо с семантикой бэктрекинга.
    out = []
    i = 0
    n = len(p)
    while i < n:
        c = p[i]
        # 1) сокращения без учёта регистра
        if c == "'" and i + 1 < n:
            c1 = p[i + 1].lower()
            if c1 in ('s', 't', 'm', 'd'):
                out.append(p[i:i + 2]); i += 2; continue
            if i + 2 < n:
                c2 = p[i + 2].lower()
                if (c1, c2) in (('r', 'e'), ('v', 'e'), ('l', 'l')):
                    out.append(p[i:i + 3]); i += 3; continue
        # 2) [^\r\n\p{L}\p{N}]?\p{L}+
        if is_L(c) or (c not in '\r\n' and not is_N(c) and i + 1 < n and is_L(p[i + 1])):
            k = i + 1
            while k < n and is_L(p[k]):
                k += 1
            out.append(p[i:k]); i = k; continue
        # 3) \p{N} — одна цифра
        if is_N(c):
            out.append(c); i += 1; continue
        # 4) ' ?[^\s\p{L}\p{N}]+[\r\n]*'
        j = i + 1 if (c == ' ' and i + 1 < n) else i
        cj = p[j] if j < n else None
        if cj is not None and not is_S(cj) and not is_L(cj) and not is_N(cj):
            k = j
            while k < n and not is_S(p[k]) and not is_L(p[k]) and not is_N(p[k]):
                k += 1
            while k < n and p[k] in '\r\n':
                k += 1
            out.append(p[i:k]); i = k; continue
        # 5..7) пробельные
        k = i
        last_nl = -1
        while k < n and is_S(p[k]):
            if p[k] in '\r\n':
                last_nl = k
            k += 1
        if last_nl >= 0:                       # \s*[\r\n]+
            out.append(p[i:last_nl + 1]); i = last_nl + 1; continue
        if k - i > 1 and k < n:                # \s+(?!\S)
            out.append(p[i:k - 1]); i = k - 1; continue
        if k > i:                              # \s+
            out.append(p[i:k]); i = k; continue
        out.append(c); i += 1                  # ничего не подошло
    return out


class Tokenizer:
    def __init__(self, g):
        m = g.meta
        self.model = m['tokenizer.ggml.model']
        self.tokens = list(m['tokenizer.ggml.tokens'])
        self.index = {}
        for i, t in enumerate(self.tokens):           # первый выигрывает (как в ядре)
            self.index.setdefault(t, i)
        self.types = m.get('tokenizer.ggml.token_type')
        self.bos = int(m.get('tokenizer.ggml.bos_token_id', 1))
        self.eos = int(m.get('tokenizer.ggml.eos_token_id', 2))
        self.pre = m.get('tokenizer.ggml.pre', '')
        # Спецтокены для разбиения текста: USER_DEFINED(4) — всегда (как
        # llama.cpp и HF), CONTROL(3) — только в доверенном тексте шаблона.
        self.user_def = []
        self.control = []
        if self.types is not None:
            for i, t in enumerate(self.tokens):
                ty = int(self.types[i])
                if ty == 4 and len(t) > 0:
                    self.user_def.append((t, i))
                elif ty == 3 and len(t) > 1:
                    self.control.append((t, i))
        self.user_def.sort(key=lambda x: -len(x[0]))
        self.control.sort(key=lambda x: -len(x[0]))
        if self.model == 'llama':
            self.scores = np.asarray(m['tokenizer.ggml.scores'], dtype=F32)
            self.add_bos = bool(m.get('tokenizer.ggml.add_bos_token', True))
        elif self.model == 'gpt2':
            self.ranks = {}
            for i, s in enumerate(m['tokenizer.ggml.merges']):
                self.ranks.setdefault(tuple(s.split(' ', 1)), i)
            self.add_bos = bool(m.get('tokenizer.ggml.add_bos_token', False))
            self.b2u = bytes_to_unicode()
            self.u2b = {v: k for k, v in self.b2u.items()}
        else:
            raise SystemExit('tokenizer %s not supported' % self.model)

    def tid(self, s):
        return self.index[s]

    # ---- SentencePiece (llama): жадное слияние пар с наибольшим score ----
    def spm(self, text):
        text = ' ' + text                      # add_space_prefix
        text = text.replace(' ', '▁')
        sym = list(text)
        while True:
            best = None
            for i in range(len(sym) - 1):
                j = self.index.get(sym[i] + sym[i + 1])
                if j is not None:
                    sc = self.scores[j]
                    if best is None or sc > best[0]:
                        best = (sc, i)
            if best is None:
                break
            i = best[1]
            sym[i:i + 2] = [sym[i] + sym[i + 1]]
        out = []
        for s in sym:
            j = self.index.get(s)
            if j is not None:
                out.append(j)
            else:
                for b in s.encode('utf-8'):
                    out.append(self.index['<0x%02X>' % b])
        return out

    def pretokenize(self, text):
        if self.pre == 'qwen2':
            return pretok_qwen2(text)
        if self.pre == 'smollm':
            # \p{N} — каждая цифра отдельным куском, остальное — регуляркой GPT-2.
            parts, cur = [], ''
            for ch in text:
                if is_N(ch):
                    if cur:
                        parts.append(cur); cur = ''
                    parts.append(ch)
                else:
                    cur += ch
            if cur:
                parts.append(cur)
            out = []
            for p in parts:
                out += pretok_gpt2(p)
            return out
        return pretok_gpt2(text)

    def bpe(self, word):
        sym = [self.b2u[b] for b in word.encode('utf-8', errors='surrogateescape')]
        while len(sym) > 1:
            best = None
            for i in range(len(sym) - 1):
                r = self.ranks.get((sym[i], sym[i + 1]))
                if r is not None and (best is None or r < best[0]):
                    best = (r, i)
            if best is None:
                break
            i = best[1]
            sym[i:i + 2] = [sym[i] + sym[i + 1]]
        return [self.index[s] for s in sym]

    def split_special(self, text, control=False):
        sp = self.user_def + (self.control if control else [])
        if not sp:
            return [text] if text else []
        firsts = set(s[0] for s, _ in sp)
        out, i, start = [], 0, 0
        n = len(text)
        while i < n:
            if text[i] in firsts:
                hit = None
                for s, t in sp:
                    if text.startswith(s, i):
                        hit = (s, t); break
                if hit:
                    if start < i:
                        out.append(text[start:i])
                    out.append(hit[1])
                    i += len(hit[0]); start = i
                    continue
            i += 1
        if start < n:
            out.append(text[start:])
        return out

    def encode_plain(self, text):
        if not text:
            return []
        if self.model == 'llama':
            return self.spm(text)
        out = []
        for w in self.pretokenize(text):
            out += self.bpe(w)
        return out

    def encode(self, text, bos=None, control=False):
        if bos is None:
            bos = self.add_bos
        out = [self.bos] if bos else []
        if self.model == 'llama':
            if text:
                out += self.spm(text)
            return out
        for seg in self.split_special(text, control):
            out += [seg] if isinstance(seg, int) else self.encode_plain(seg)
        return out

    def piece_bytes(self, i):
        t = self.tokens[i]
        if self.model == 'llama':
            if len(t) == 6 and t.startswith('<0x') and t.endswith('>'):
                return bytes([int(t[3:5], 16)])
            return t.replace('▁', ' ').encode('utf-8', errors='surrogateescape')
        if self.types is not None and int(self.types[i]) in (3, 4):   # control / user-defined
            return t.encode('utf-8')
        return bytes(self.u2b.get(c, ord('?')) for c in t)

    def eog(self):
        s = {self.eos}
        for t in ('<|im_end|>', '<|endoftext|>'):
            if t in self.index:
                s.add(self.index[t])
        return s


# ---------------------------------------------------------------------------
# Промпты
# ---------------------------------------------------------------------------
def load_tools(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def tools_block(tools):
    # Ровно то, что пишет jinja-шаблон Qwen3 при tools (tojson = json.dumps
    # с ensure_ascii=False и разделителями по умолчанию).
    s = ('# Tools\n\nYou may call one or more functions to assist with the user query.\n\n'
         'You are provided with function signatures within <tools></tools> XML tags:\n<tools>')
    for t in tools:
        s += '\n' + json.dumps(t, ensure_ascii=False)
    s += ('\n</tools>\n\nFor each function call, return a json object with function name and '
          'arguments within <tool_call></tool_call> XML tags:\n<tool_call>\n'
          '{"name": <function-name>, "arguments": <args-json-object>}\n</tool_call>')
    return s


def chat_prompt(tok, user, system=None, arch='llama', think=False, tools=None, prefix=''):
    # Спецтокены ChatML вставляются id'ами, текст между ними токенизируется
    # ОДНИМ куском (с разбиением по USER_DEFINED) — так же, как HF/llama.cpp
    # токенизируют отрендеренный шаблон.
    ims = tok.index['<|im_start|>']
    ime = tok.index['<|im_end|>']
    ids = []
    sys_text = system
    if arch == 'qwen3' and tools:
        sys_text = (system + '\n\n' if system else '') + tools_block(tools)
    if sys_text:
        ids += [ims] + tok.encode('system\n' + sys_text, bos=False) + [ime] + tok.encode('\n', bos=False)
    ids += [ims] + tok.encode('user\n' + user, bos=False) + [ime] + tok.encode('\n', bos=False)
    gen = 'assistant\n'
    if arch == 'qwen3' and not think:
        gen += '<think>\n\n</think>\n\n'
    ids += [ims] + tok.encode(gen + prefix, bos=False)
    return ids


def tool_response_ids(tok, out_text, arch='qwen3', think=False):
    # Продолжение после </tool_call>: закрыть реплику, ответ инструмента в
    # реплике user (формат шаблона Qwen3), новая реплика ассистента.
    ims = tok.index['<|im_start|>']
    ime = tok.index['<|im_end|>']
    gen = 'assistant\n' + ('' if think else '<think>\n\n</think>\n\n')
    return ([ime] + tok.encode('\n', bos=False) + [ims] +
            tok.encode('user\n<tool_response>\n' + out_text + '\n</tool_response>', bos=False) +
            [ime] + tok.encode('\n', bos=False) + [ims] + tok.encode(gen, bos=False))


def render_official(g, tok, user, system=None, tools=None, think=False):
    # Официальный шаблон из метаданных GGUF (tokenizer.chat_template), как
    # его рендерит transformers: tojson без экранирования не-ASCII.
    import jinja2
    tmpl = g.meta['tokenizer.chat_template']
    env = jinja2.Environment(trim_blocks=True, lstrip_blocks=True)
    env.filters['tojson'] = lambda x, **kw: json.dumps(x, ensure_ascii=False)
    msgs = []
    if system:
        msgs.append({'role': 'system', 'content': system})
    msgs.append({'role': 'user', 'content': user})
    kw = dict(messages=msgs, add_generation_prompt=True, enable_thinking=think)
    if tools:
        kw['tools'] = tools
    text = env.from_string(tmpl).render(**kw)
    return text, tok.encode(text, bos=False, control=True)


def generate(model, tok, ids, n, pos0=0, stop_ids=()):
    logits = model.forward(ids, pos0)
    pos = pos0 + len(ids)
    out, gaps = [], []
    eog = tok.eog() | set(stop_ids)
    for _ in range(n):
        top2 = np.argpartition(-logits, 2)[:2]
        a, b = sorted(top2, key=lambda i: -logits[i])
        gaps.append(float(logits[a] - logits[b]))
        nxt = int(a)
        out.append(nxt)
        if nxt in eog:
            break
        logits = model.forward([nxt], pos); pos += 1
    return out, gaps, pos


def fmt_text(tok, ids):
    txt = b''.join(tok.piece_bytes(i) for i in ids)
    return txt.decode('utf-8', errors='replace').encode('ascii', 'backslashreplace').decode()


def opt(name, default=None):
    for a in sys.argv:
        if a.startswith('--' + name + '='):
            return a[len(name) + 3:]
    return default


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    g = GGUF(path)
    if cmd == 'info':
        print('version', g.version, 'tensors', len(g.tensors), 'data_off', g.data_off)
        for k, v in g.meta.items():
            if isinstance(v, (list, np.ndarray)):
                print('%-45s [%d] %s' % (k, len(v), list(v[:5])))
            elif isinstance(v, str) and len(v) > 200:
                print('%-45s %r...' % (k, v[:300]))
            else:
                print('%-45s %r' % (k, v))
        types = {}
        for n, (d, t, o) in g.tensors.items():
            types[TYPE_NAME.get(t, t)] = types.get(TYPE_NAME.get(t, t), 0) + 1
        print('tensor types:', types)
        seen = set()
        for n, (d, t, o) in g.tensors.items():
            key = n.split('.', 2)[-1] if n.startswith('blk.') else n
            if key in seen:
                continue
            seen.add(key)
            print('  %-32s %-16s %s' % (n, d, TYPE_NAME.get(t, t)))
        return
    tok = Tokenizer(g)
    arch = g.meta['general.architecture']
    if cmd == 'tok':
        ids = tok.encode(sys.argv[3])
        print('ids:', ','.join(map(str, ids)))
        print('pieces:', [tok.tokens[i] for i in ids])
        return
    if cmd == 'tokcheck':
        from tokenizers import Tokenizer as HFTok
        hf = HFTok.from_file(sys.argv[3])
        texts = SAMPLE_TEXTS[:]
        if len(sys.argv) > 4 and not sys.argv[4].startswith('--'):
            with open(sys.argv[4], encoding='utf-8') as f:
                texts += [ln.rstrip('\n').replace('\\n', '\n') for ln in f if ln.strip()]
        texts += random_texts()
        bad = 0
        for t in texts:
            a = tok.encode(t, bos=False, control=True)
            b = hf.encode(t, add_special_tokens=False).ids
            if a != b:
                bad += 1
                if bad <= 10:
                    print('MISMATCH %r\n  ref %s\n  hf  %s' % (t, a, b))
        print('tokcheck: %d texts, %d mismatches' % (len(texts), bad))
        return
    if cmd == 'tokdump':
        # Тексты (hex UTF-8, по строке) и их ids — для сверки токенизатора
        # ядра: build/mind_host tokfile <model> <texts.hex> > ids.txt
        texts = SAMPLE_TEXTS[:] + random_texts()
        with open(sys.argv[3], 'w', encoding='ascii', newline='\n') as ft, \
             open(sys.argv[4], 'w', encoding='ascii', newline='\n') as fi:
            for t in texts:
                ft.write(t.encode('utf-8').hex() + '\n')
                fi.write(','.join(map(str, tok.encode(t, bos=False))) + '\n')
        print('tokdump: %d texts' % len(texts))
        return
    tools = load_tools(opt('tools')) if opt('tools') else None
    if tools and opt('tool-names'):
        # Подмножество инструментов (mind.conf tools=ls,...): порядок — как в файле.
        names = opt('tool-names').split(',')
        tools = [t for t in tools if t['function']['name'] in names]
    think = '--think' in sys.argv
    system = opt('system')
    if cmd == 'render':
        user = sys.argv[3]
        text, off_ids = render_official(g, tok, user, system, tools, think)
        our = chat_prompt(tok, user, system, arch, think, tools)
        print('official:', ','.join(map(str, off_ids)))
        print('ours:    ', ','.join(map(str, our)))
        print('match:', 'yes' if off_ids == our else 'NO')
        if off_ids != our:
            print('official text: %r' % text[:3000])
        return
    if cmd in ('gen', 'gen-ids'):
        n = int(sys.argv[4])
        if cmd == 'gen':
            if '--chat' in sys.argv:
                ids = chat_prompt(tok, sys.argv[3], system, arch, think, tools, opt('prefix', ''))
            else:
                ids = tok.encode(sys.argv[3])
        else:
            ids = [int(x) for x in sys.argv[3].split(',')]
        resp = opt('tool-response')
        ctx = len(ids) + n + 1 + (400 if resp else 0)
        model = Model(g, n_ctx=ctx)
        stop = [tok.index['</tool_call>']] if '</tool_call>' in tok.index else []
        out, gaps, pos = generate(model, tok, ids, n, 0, stop)
        print('prompt:', ','.join(map(str, ids)))
        print('ids:', ','.join(map(str, out)))
        print('text:', fmt_text(tok, out))
        print('gaps:', ','.join('%.3f' % x for x in gaps))
        if resp and out and stop and out[-1] == stop[0]:
            with open(resp, encoding='utf-8') as f:
                rtext = f.read().rstrip('\n')
            # </tool_call> сэмплирован, но ещё не прогнан через модель — он первый.
            more = [out[-1]] + tool_response_ids(tok, rtext, arch, think)
            out2, gaps2, _ = generate(model, tok, more, n, pos, stop)
            print('prompt2:', ','.join(map(str, more)))
            print('ids2:', ','.join(map(str, out2)))
            print('text2:', fmt_text(tok, out2))
            print('gaps2:', ','.join('%.3f' % x for x in gaps2))
        return
    raise SystemExit('unknown command')


def random_texts(n=3000):
    # Случайные строки из «трудных» символов: пробелы, \r\n, апострофы,
    # кириллица, цифры разных систем, эмодзи, NBSP.
    rng = np.random.default_rng(49)
    alphabet = (list("abcXYZ019 ,.!?'\"-_()[]{}<>/\\:;\t\n\r  ") + list('абвЁёЯя ') +
                ['\u2019', '\u2026', '\u00bd', '\u0663', '\u4e2d', '\U0001F600', '\u00a0', 'S', 'T'])
    out = []
    for _ in range(n):
        k = int(rng.integers(1, 24))
        out.append(''.join(alphabet[int(i)] for i in rng.integers(0, len(alphabet), k)))
    return out


SAMPLE_TEXTS = [
    "What is the capital of France?",
    "Сколько памяти свободно на этом компьютере?",
    "Какие процессы сейчас запущены? Ответь кратко.",
    "I'm here, you're there, WE'LL see. It's DON'T 'S 'Ll",
    "1234567 + 89 = 1234656; 3.14159, 1,000,000",
    "hello   world\n\n\nnext\r\nline  \n  indented\ttab",
    "   leading spaces and trailing   ",
    "<tool_call>\n{\"name\": \"ls\", \"arguments\": {\"path\": \"/disk\"}}\n</tool_call>",
    "<think>\n\n</think>\n\nThe answer is 42.",
    "path /disk/vault/mind/Qwen3-1.7B-Q8_0.gguf size=1834426016 bytes",
    "uptime: 812 s\ncpu: 4 cores online\n  cpu0 (BSP): window manager, desktop, drivers\n",
    "memory: 7123 MB free of 8012 MB; kernel heap 12/128 MB",
    "  \"Pulse\" focused\n  \"Files\" visible\n",
    "Ёлки-палки, ёжик! «Кавычки» — тире… №5",
    "mixed English и русский text123abc",
    "emoji 😀 and 中文字符 and عربى ١٢٣",
    "?!\n\n...\n---\n",
    "a  b   c    d",
    "\n",
    " ",
    "",
]


if __name__ == '__main__':
    main()

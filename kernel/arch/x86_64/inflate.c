// ============================================================================
// inflate.c — распаковка DEFLATE / gzip / zlib (см. inflate.h).
// ----------------------------------------------------------------------------
// Устройство — классическое (как в учебном puff из zlib): поток читается
// побитно, младшим битом вперёд; код Хаффмана задаётся двумя таблицами —
// сколько кодов каждой длины (count) и символы в порядке кодов (symbol);
// разбор идёт по одному биту, сравнивая накопленный код с диапазоном кодов
// текущей длины. Таблицы строятся на стеке (несколько сотен байт).
// ============================================================================
#include "inflate.h"

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

typedef struct {
    const uint8_t* in;  size_t in_len,  in_pos;
    uint8_t*       out; size_t out_cap, out_pos;
    uint32_t bitbuf;
    int      bitcnt;
    bool     in_short;       // вход кончился посреди потока
    bool     out_full;       // выход кончился (обрезка)
} istate_t;

typedef struct {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[FIXLCODES];
} huff_t;

// need бит (0..16), младший бит потока — младший бит результата.
static uint32_t bits(istate_t* s, int need) {
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->in_pos >= s->in_len) { s->in_short = true; return 0; }
        val |= (uint32_t)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return val & ((1u << need) - 1);
}

// Один символ по коду h. -1 — кода нет (повреждённый поток).
static int decode(istate_t* s, const huff_t* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)bits(s, 1);
        if (s->in_short) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code  <<= 1;
    }
    return -1;
}

// Построить таблицы по длинам кодов. Возврат: 0 — полный код, >0 — неполный
// (допустим только для одиночного кода расстояний), <0 — переопределён.
static int construct(huff_t* h, const uint16_t* length, int n) {
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;                  // ни одного кода

    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    uint16_t offs[MAXBITS + 1];
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (uint16_t)sym;
    return left;
}

static inline void put(istate_t* s, uint8_t b) {
    if (s->out_pos < s->out_cap) s->out[s->out_pos++] = b;
    else s->out_full = true;
}

// Тело блока: литералы и пары (длина, расстояние).
static bool codes(istate_t* s, const huff_t* lencode, const huff_t* distcode) {
    static const uint16_t lens[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                       35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint16_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                       3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t dists[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577 };
    static const uint16_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int symbol = decode(s, lencode);
        if (symbol < 0) return false;
        if (symbol < 256) {
            put(s, (uint8_t)symbol);
            if (s->out_full) return true;
        } else if (symbol == 256) {
            return true;                              // конец блока
        } else {
            symbol -= 257;
            if (symbol >= 29) return false;
            int len = lens[symbol] + (int)bits(s, lext[symbol]);
            symbol = decode(s, distcode);
            if (symbol < 0 || symbol >= 30) return false;
            size_t dist = dists[symbol] + bits(s, dext[symbol]);
            if (s->in_short) return false;
            if (dist > s->out_pos) return false;      // ссылка до начала вывода
            while (len--) {
                if (s->out_pos >= s->out_cap) { s->out_full = true; return true; }
                s->out[s->out_pos] = s->out[s->out_pos - dist];
                s->out_pos++;
            }
        }
    }
}

static bool stored(istate_t* s) {
    s->bitbuf = 0; s->bitcnt = 0;                     // до границы байта
    if (s->in_pos + 4 > s->in_len) { s->in_short = true; return false; }
    unsigned len  = s->in[s->in_pos] | ((unsigned)s->in[s->in_pos + 1] << 8);
    unsigned nlen = s->in[s->in_pos + 2] | ((unsigned)s->in[s->in_pos + 3] << 8);
    s->in_pos += 4;
    if (len != (~nlen & 0xFFFF)) return false;
    if (s->in_pos + len > s->in_len) { s->in_short = true; return false; }
    while (len--) {
        put(s, s->in[s->in_pos++]);
        if (s->out_full) return true;
    }
    return true;
}

static bool fixed(istate_t* s) {
    static huff_t lencode, distcode;
    static bool built = false;
    if (!built) {
        uint16_t lengths[FIXLCODES];
        int sym = 0;
        for (; sym < 144; sym++) lengths[sym] = 8;
        for (; sym < 256; sym++) lengths[sym] = 9;
        for (; sym < 280; sym++) lengths[sym] = 7;
        for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
        construct(&lencode, lengths, FIXLCODES);
        for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
        construct(&distcode, lengths, MAXDCODES);
        built = true;
    }
    return codes(s, &lencode, &distcode);
}

static bool dynamic(istate_t* s) {
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    static uint16_t lengths[MAXLCODES + MAXDCODES];
    static huff_t lencode, distcode;                  // static: по 600 байт, стек беречь

    int nlen  = (int)bits(s, 5) + 257;
    int ndist = (int)bits(s, 5) + 1;
    int ncode = (int)bits(s, 4) + 4;
    if (s->in_short || nlen > MAXLCODES || ndist > MAXDCODES) return false;

    int index;
    for (index = 0; index < ncode; index++) lengths[order[index]] = (uint16_t)bits(s, 3);
    for (; index < 19; index++) lengths[order[index]] = 0;
    if (construct(&lencode, lengths, 19) != 0) return false;

    index = 0;
    while (index < nlen + ndist) {
        int symbol = decode(s, &lencode);
        if (symbol < 0) return false;
        if (symbol < 16) lengths[index++] = (uint16_t)symbol;
        else {
            int len = 0, repeat;
            if (symbol == 16) {
                if (index == 0) return false;
                len = lengths[index - 1];
                repeat = 3 + (int)bits(s, 2);
            } else if (symbol == 17) repeat = 3 + (int)bits(s, 3);
            else                     repeat = 11 + (int)bits(s, 7);
            if (s->in_short || index + repeat > nlen + ndist) return false;
            while (repeat--) lengths[index++] = (uint16_t)len;
        }
    }
    if (lengths[256] == 0) return false;              // без кода конца блока нельзя

    int err = construct(&lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = construct(&distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return codes(s, &lencode, &distcode);
}

inflate_result_t inflate_raw(const uint8_t* in, size_t in_len,
                             uint8_t* out, size_t out_cap, size_t* out_len) {
    istate_t s;
    s.in = in;   s.in_len = in_len;   s.in_pos = 0;
    s.out = out; s.out_cap = out_cap; s.out_pos = 0;
    s.bitbuf = 0; s.bitcnt = 0; s.in_short = false; s.out_full = false;

    bool ok = true;
    int last;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (s.in_short) { ok = false; break; }
        if      (type == 0) ok = stored(&s);
        else if (type == 1) ok = fixed(&s);
        else if (type == 2) ok = dynamic(&s);
        else                ok = false;
        if (!ok || s.out_full) break;
    } while (!last);

    if (out_len) *out_len = s.out_pos;
    if (s.out_full) return INFLATE_TRUNCATED;
    // Вход оборвался (страницу обрезали по размеру буфера): что распаковалось —
    // годное начало документа.
    if (s.in_short) return s.out_pos ? INFLATE_TRUNCATED : INFLATE_ERR_DATA;
    return ok ? INFLATE_OK : INFLATE_ERR_DATA;
}

inflate_result_t inflate_http(const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t out_cap, size_t* out_len) {
    if (out_len) *out_len = 0;
    if (in_len < 2) return INFLATE_ERR_FORMAT;

    // gzip: 1F 8B 08 FLG MTIME(4) XFL OS [extra] [name] [comment] [hcrc]
    if (in[0] == 0x1F && in[1] == 0x8B) {
        if (in_len < 18 || in[2] != 8) return INFLATE_ERR_FORMAT;
        uint8_t flg = in[3];
        size_t p = 10;
        if (flg & 0x04) {                                    // FEXTRA
            if (p + 2 > in_len) return INFLATE_ERR_DATA;
            size_t xlen = in[p] | ((size_t)in[p + 1] << 8);
            p += 2 + xlen;
        }
        if (flg & 0x08) { while (p < in_len && in[p]) p++; p++; }   // FNAME
        if (flg & 0x10) { while (p < in_len && in[p]) p++; p++; }   // FCOMMENT
        if (flg & 0x02) p += 2;                              // FHCRC
        if (p >= in_len) return INFLATE_ERR_DATA;
        // Хвост (CRC32 + ISIZE, 8 байт) не сверяем: целостность уже обеспечил
        // TLS, а на открытом HTTP повреждение покажет сам разбор потока.
        return inflate_raw(in + p, in_len - p, out, out_cap, out_len);
    }
    // zlib: CMF FLG, (CMF*256 + FLG) кратно 31, метод 8.
    if ((in[0] & 0x0F) == 8 && ((((unsigned)in[0] << 8) | in[1]) % 31) == 0 && !(in[1] & 0x20))
        return inflate_raw(in + 2, in_len - 2, out, out_cap, out_len);
    // Иначе — «deflate» без обёртки (так отвечают некоторые старые серверы).
    return inflate_raw(in, in_len, out, out_cap, out_len);
}

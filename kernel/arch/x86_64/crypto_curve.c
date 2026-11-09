// ============================================================================
// crypto_curve.c — Curve25519 операции: X25519 (RFC 7748) + Ed25519 (RFC 8032).
// ----------------------------------------------------------------------------
// Реализация в стиле ref10 (Bernstein) — самая распространённая reference
// implementation, тщательно проверенная на test vectors.
//
// Field arithmetic: GF(p), p = 2^255 - 19.
// Field-элементы представлены как 16 × int64_t (saturated 16-bit limbs).
// Это менее эффективно, чем 5x51 или 10x25.5, но НАМНОГО проще в реализации
// и легко проверяется глазами.  Все операции (add/sub/mul/sqr/inv) выполняются
// модулем p через "reduce-carry" passes.
//
// X25519: Montgomery ladder на curve y^2 = x^3 + 486662*x^2 + x.
//         scalar mult от точки x → возвращает x-координату.
//
// Ed25519: точки на twisted Edwards curve -x^2 + y^2 = 1 + (-121665/121666) x^2 y^2.
//          verify: вычислить 8R, 8S*B, 8h*A; check R + h*A == S*B (на 8-torsion).
//
// Вариант, что я тут пишу — упрощённая (и более медленная) реализация tweetnacl
// от Bernstein, public domain (https://tweetnacl.cr.yp.to/).
// ============================================================================
#include "crypto.h"

const uint8_t crypto_x25519_basepoint[32] = {
    9,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
    0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
};

// ============================================================================
// Field GF(2^255 - 19) — 16 × int64_t limbs.
// ============================================================================
typedef int64_t gf[16];

static const gf gf0   = {0};
static const gf gf1   = {1};
static const gf _121665 = {0xDB41, 1};   // 121665 = 0x1DB41 = 0xDB41 | (1<<16)

static void set25519(gf r, const gf a) {
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void car25519(gf o) {
    // Carry propagation: каждый limb имеет ≤16 effective bits после reduce.
    for (int i = 0; i < 16; i++) {
        o[i] += (int64_t)1 << 16;
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    // CT swap: if b: swap(p, q).
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t* o, const gf n) {
    gf t, m;
    set25519(t, n);
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i-1] >> 16) & 1);
            m[i-1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int64_t b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - (int)b);
    }
    for (int i = 0; i < 16; i++) {
        o[2*i]   = (uint8_t)(t[i] & 0xff);
        o[2*i+1] = (uint8_t)((t[i] >> 8) & 0xff);
    }
}

static int neq25519(const gf a, const gf b) {
    uint8_t c[32], d[32];
    pack25519(c, a);
    pack25519(d, b);
    return !crypto_eq_const(c, d, 32);
}

static uint8_t par25519(const gf a) {
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static void unpack25519(gf o, const uint8_t* n) {
    for (int i = 0; i < 16; i++) {
        o[i] = (int64_t)n[2*i] + ((int64_t)n[2*i+1] << 8);
    }
    o[15] &= 0x7fff;
}

static void A(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void M(gf o, const gf a, const gf b) {
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void S(gf o, const gf a) { M(o, a, a); }

static void inv25519(gf o, const gf i) {
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4) M(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

static void pow2523(gf o, const gf i) {
    gf c;
    int a;
    for (a = 0; a < 16; a++) c[a] = i[a];
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1) M(c, c, i);
    }
    for (a = 0; a < 16; a++) o[a] = c[a];
}

// ============================================================================
// X25519 (RFC 7748): Montgomery ladder.
// ============================================================================
void crypto_x25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t u_in[32]) {
    uint8_t z[32];
    for (int i = 0; i < 32; i++) z[i] = scalar[i];
    // Clamp per RFC 7748 §5: clear bits 0,1,2 of byte 0; clear bit 7 of byte 31;
    // set bit 6 of byte 31.
    z[0]  &= 248;
    z[31] &= 127;
    z[31] |=  64;

    gf x, a, b, c, d;
    unpack25519(x, u_in);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; a[i] = c[i] = d[i] = 0; }
    a[0] = d[0] = 1;

    int r;
    for (int i = 254; i >= 0; i--) {
        r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        gf e, f;
        A(e, a, c);
        Z(a, a, c);
        A(c, b, d);
        Z(b, b, d);
        S(d, e);
        S(f, a);
        M(a, c, a);
        M(c, b, e);
        A(e, a, c);
        Z(a, a, c);
        S(b, a);
        Z(c, d, f);
        M(a, c, _121665);
        A(a, a, d);
        M(c, c, a);
        M(a, d, f);
        M(d, b, x);
        S(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    inv25519(c, c);
    M(a, a, c);
    pack25519(out, a);
}

// ============================================================================
// Ed25519 verify (RFC 8032 §5.1.7).
// Точки на twisted Edwards.  Verify-only — нам не нужен private key sign,
// только проверка сертификата.
// ============================================================================
typedef gf ge[4];   // (x, y, z, t) — extended coordinates

// Constants: d (curve param), I (sqrt(-1)), L (group order).
static const gf D = {
    0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141, 0x0a4d, 0x0070,
    0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203
};
static const gf D2 = {
    0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
    0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406
};
static const gf X = {
    0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
    0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169
};
static const gf Y = {
    0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
    0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666
};
static const gf I = {
    0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f, 0x1806, 0x2f43,
    0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83
};

static void add_ed(ge p, ge q) {
    gf a, b, c, d, t, e, f, g, h;
    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);
    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void cswap(ge p, ge q, uint8_t b) {
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void pack(uint8_t* r, ge p) {
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= par25519(tx) << 7;
}

static void scalarmult(ge p, ge q, const uint8_t* s) {
    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1;
        cswap(p, q, b);
        add_ed(q, p);
        add_ed(p, p);
        cswap(p, q, b);
    }
}

static void scalarbase(ge p, const uint8_t* s) {
    ge q;
    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

static int unpackneg(ge r, const uint8_t* p) {
    gf t, chk, num, den, den2, den4, den6;
    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t,    den6, num);
    M(t,    t,    den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) M(r[0], r[0], I);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num)) return -1;

    if (par25519(r[0]) == (p[31] >> 7)) Z(r[0], gf0, r[0]);
    M(r[3], r[0], r[1]);
    return 0;
}

// L (group order) = 2^252 + 27742317777372353535851937790883648493.
static const uint64_t L_LIMBS[32] = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x10
};

static void modL(uint8_t* r, int64_t* x) {
    int64_t carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        int j;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * (int64_t)L_LIMBS[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * (int64_t)L_LIMBS[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * (int64_t)L_LIMBS[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i]      = (uint8_t)(x[i] & 255);
    }
}

static void reduce(uint8_t* r) {
    int64_t x[64];
    for (int i = 0; i < 64; ++i) x[i] = (int64_t)(uint64_t)r[i];
    for (int i = 0; i < 64; ++i) r[i] = 0;
    modL(r, x);
}

bool crypto_ed25519_verify(const uint8_t pub_key[32],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t signature[64]) {
    if (signature[63] & 224) return false;       // s слишком большое

    ge q;
    if (unpackneg(q, pub_key) != 0) return false;

    // h = SHA-512(R || pub || msg)
    uint8_t h[64];
    crypto_sha512_ctx_t sh;
    crypto_sha512_init(&sh);
    crypto_sha512_update(&sh, signature, 32);
    crypto_sha512_update(&sh, pub_key,   32);
    crypto_sha512_update(&sh, msg,       msg_len);
    crypto_sha512_final (&sh, h);

    reduce(h);

    ge p;
    scalarmult(p, q, h);

    ge sb;
    uint8_t s[32];
    for (int i = 0; i < 32; ++i) s[i] = signature[32 + i];
    scalarbase(sb, s);

    add_ed(p, sb);

    // Сравниваем pack(p) == signature[0..32].
    uint8_t t[32];
    pack(t, p);

    return crypto_eq_const(t, signature, 32);
}

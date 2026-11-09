// ============================================================================
// crypto_p384.c — проверка подписей ECDSA на кривой NIST P-384 (подэтап 48.2).
// ----------------------------------------------------------------------------
// Зачем. Почти все «эллиптические» цепочки сертификатов сегодня держатся на
// P-384: промежуточные Let's Encrypt E5..E8, корни ISRG Root X2, GTS Root R4,
// GlobalSign ECC R5, DigiCert Global Root G3. Листовой сертификат у сайта
// обычно на P-256 (его мы умели), но подписан он ключом P-384 — и проверка
// цепочки обрывалась на разборе промежуточного сертификата («parse error»,
// значок «HTTPS ?» у Википедии, Google, GitHub, Cloudflare).
//
// Что здесь есть: проверка подписи и точки (открытые данные, постоянное время
// не требуется) и обмен ключами ECDH. Последний нужен редко — серверы обычно
// выбирают x25519 или P-256, — но объявить secp384r1 в supported_groups
// приходится: часть серверов (lite.cnn.com) без него отвечает alert 40, даже
// если сам обмен потом идёт на x25519. А объявленное надо уметь.
// Умножение на скаляр здесь — простое «удвоить и сложить», не за постоянное
// время; ключ ECDHE одноразовый (одно умножение на соединение), так что
// утечка по времени практической атаки не даёт. Для долгоживущих ключей
// этот код не годится.
//
// Устройство. Числа — 6 слов по 64 бита (младшее первым). Умножение по модулю
// — Монтгомери (CIOS), одна и та же функция для поля (модуль p) и для
// скаляров (модуль n — порядок группы); константы R^2 и -m^-1 считаются при
// первом вызове. Точки — в якобиевых координатах (a = -3), u1*G + u2*Q —
// приём Шамира (общее удвоение, сложение с G, Q или G+Q по паре бит).
// Деление 128-битных чисел не используется (libgcc нет — CLAUDE.md §5.10),
// только умножение 64x64 -> 128, которое компилятор разворачивает в `mul`.
// ============================================================================
#include "crypto.h"

typedef unsigned __int128 u128;
typedef struct { uint64_t v[6]; } n384_t;
typedef struct { n384_t x, y, z; } pt384_t;      // z == 0 — бесконечно удалённая точка

typedef struct {
    n384_t   m;        // модуль
    uint64_t m0inv;    // -m^-1 mod 2^64
    n384_t   r2;       // R^2 mod m, R = 2^384
    n384_t   one;      // R mod m (единица в форме Монтгомери)
} mod384_t;

// p = 2^384 - 2^128 - 2^96 + 2^32 - 1
static const n384_t P384_P = {{ 0x00000000FFFFFFFFULL, 0xFFFFFFFF00000000ULL, 0xFFFFFFFFFFFFFFFEULL,
                                0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL }};
// n — порядок базовой точки
static const n384_t P384_N = {{ 0xECEC196ACCC52973ULL, 0x581A0DB248B0A77AULL, 0xC7634D81F4372DDFULL,
                                0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL }};
static const n384_t P384_B = {{ 0x2A85C8EDD3EC2AEFULL, 0xC656398D8A2ED19DULL, 0x0314088F5013875AULL,
                                0x181D9C6EFE814112ULL, 0x988E056BE3F82D19ULL, 0xB3312FA7E23EE7E4ULL }};
static const n384_t P384_GX = {{ 0x3A545E3872760AB7ULL, 0x5502F25DBF55296CULL, 0x59F741E082542A38ULL,
                                 0x6E1D3B628BA79B98ULL, 0x8EB1C71EF320AD74ULL, 0xAA87CA22BE8B0537ULL }};
static const n384_t P384_GY = {{ 0x7A431D7C90EA0E5FULL, 0x0A60B1CE1D7E819DULL, 0xE9DA3113B5F0B8C0ULL,
                                 0xF8F41DBD289A147CULL, 0x5D9E98BF9292DC29ULL, 0x3617DE4A96262C6FULL }};

static mod384_t g_fp, g_fn;       // поле и скаляры
static bool     g_ready = false;

// ---- простая арифметика -----------------------------------------------------
static bool n_is_zero(const n384_t* a) {
    uint64_t z = 0;
    for (int i = 0; i < 6; i++) z |= a->v[i];
    return z == 0;
}
static int n_cmp(const n384_t* a, const n384_t* b) {
    for (int i = 5; i >= 0; i--) {
        if (a->v[i] > b->v[i]) return 1;
        if (a->v[i] < b->v[i]) return -1;
    }
    return 0;
}
// r = a + b, возвращает перенос
static uint64_t n_add(n384_t* r, const n384_t* a, const n384_t* b) {
    u128 c = 0;
    for (int i = 0; i < 6; i++) { c += (u128)a->v[i] + b->v[i]; r->v[i] = (uint64_t)c; c >>= 64; }
    return (uint64_t)c;
}
// r = a - b, возвращает заём
static uint64_t n_sub(n384_t* r, const n384_t* a, const n384_t* b) {
    uint64_t borrow = 0;
    for (int i = 0; i < 6; i++) {
        uint64_t x = a->v[i], y = b->v[i];
        uint64_t d = x - y - borrow;
        borrow = (x < y) || (x == y && borrow) ? 1 : 0;
        r->v[i] = d;
    }
    return borrow;
}
static void m_add(n384_t* r, const n384_t* a, const n384_t* b, const mod384_t* f) {
    uint64_t c = n_add(r, a, b);
    if (c || n_cmp(r, &f->m) >= 0) n_sub(r, r, &f->m);
}
static void m_sub(n384_t* r, const n384_t* a, const n384_t* b, const mod384_t* f) {
    if (n_sub(r, a, b)) n_add(r, r, &f->m);
}

// r = a * b * R^-1 mod m (Монтгомери, CIOS). r может совпадать с a и b.
static void m_mul(n384_t* r, const n384_t* a, const n384_t* b, const mod384_t* f) {
    uint64_t t[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 6; i++) {
        u128 c = 0;
        for (int j = 0; j < 6; j++) {
            c += (u128)a->v[j] * b->v[i] + t[j];
            t[j] = (uint64_t)c; c >>= 64;
        }
        c += t[6];
        t[6] = (uint64_t)c; t[7] = (uint64_t)(c >> 64);

        uint64_t q = t[0] * f->m0inv;
        c = (u128)q * f->m.v[0] + t[0];
        c >>= 64;
        for (int j = 1; j < 6; j++) {
            c += (u128)q * f->m.v[j] + t[j];
            t[j - 1] = (uint64_t)c; c >>= 64;
        }
        c += t[6];
        t[5] = (uint64_t)c;
        t[6] = t[7] + (uint64_t)(c >> 64);
        t[7] = 0;
    }
    n384_t out;
    for (int i = 0; i < 6; i++) out.v[i] = t[i];
    if (t[6] || n_cmp(&out, &f->m) >= 0) n_sub(&out, &out, &f->m);
    *r = out;
}

static void mod_setup(mod384_t* f, const n384_t* m) {
    f->m = *m;
    // -m^-1 mod 2^64: итерация Ньютона удваивает число верных бит.
    uint64_t inv = m->v[0];
    for (int i = 0; i < 6; i++) inv *= 2 - m->v[0] * inv;
    f->m0inv = (uint64_t)0 - inv;
    // R mod m = 2^384 - m (m > 2^383), R^2 mod m — ещё 384 удвоения по модулю.
    n384_t x, zero = {{ 0, 0, 0, 0, 0, 0 }};
    n_sub(&x, &zero, m);
    f->one = x;
    for (int i = 0; i < 384; i++) m_add(&x, &x, &x, f);
    f->r2 = x;
}

static void p384_init(void) {
    if (g_ready) return;
    mod_setup(&g_fp, &P384_P);
    mod_setup(&g_fn, &P384_N);
    g_ready = true;
}

static void to_mont(n384_t* r, const n384_t* a, const mod384_t* f)   { m_mul(r, a, &f->r2, f); }
static void from_mont(n384_t* r, const n384_t* a, const mod384_t* f) {
    n384_t one = {{ 1, 0, 0, 0, 0, 0 }};
    m_mul(r, a, &one, f);
}

// r = a^(m-2) mod m — обратный элемент (m простое). a и r — в форме Монтгомери.
static void m_inv(n384_t* r, const n384_t* a, const mod384_t* f) {
    n384_t e, two = {{ 2, 0, 0, 0, 0, 0 }};
    n_sub(&e, &f->m, &two);
    n384_t acc = f->one, base = *a;
    for (int i = 0; i < 384; i++) {
        if ((e.v[i / 64] >> (i % 64)) & 1) m_mul(&acc, &acc, &base, f);
        m_mul(&base, &base, &base, f);
    }
    *r = acc;
}

static void n_from_be(n384_t* r, const uint8_t* b, size_t len) {
    for (int i = 0; i < 6; i++) r->v[i] = 0;
    for (size_t i = 0; i < len && i < 48; i++) {
        size_t pos = len - 1 - i;                     // i-й байт от младшего конца
        r->v[i / 8] |= (uint64_t)b[pos] << (8 * (i % 8));
    }
}

// ---- точки (якобиевы координаты, значения — в форме Монтгомери по p) --------
static void pt_double(pt384_t* r, const pt384_t* p) {
    const mod384_t* f = &g_fp;
    if (n_is_zero(&p->z)) { *r = *p; return; }
    n384_t delta, gamma, beta, alpha, t1, t2, x3, y3, z3;
    m_mul(&delta, &p->z, &p->z, f);
    m_mul(&gamma, &p->y, &p->y, f);
    m_mul(&beta, &p->x, &gamma, f);
    m_sub(&t1, &p->x, &delta, f);
    m_add(&t2, &p->x, &delta, f);
    m_mul(&t1, &t1, &t2, f);
    m_add(&alpha, &t1, &t1, f);
    m_add(&alpha, &alpha, &t1, f);                    // alpha = 3*(x-delta)*(x+delta)
    m_mul(&x3, &alpha, &alpha, f);
    m_add(&t1, &beta, &beta, f);                      // 2*beta
    m_add(&t1, &t1, &t1, f);                          // 4*beta
    m_add(&t2, &t1, &t1, f);                          // 8*beta
    m_sub(&x3, &x3, &t2, f);
    m_add(&z3, &p->y, &p->z, f);
    m_mul(&z3, &z3, &z3, f);
    m_sub(&z3, &z3, &gamma, f);
    m_sub(&z3, &z3, &delta, f);
    m_sub(&t1, &t1, &x3, f);                          // 4*beta - x3
    m_mul(&y3, &alpha, &t1, f);
    m_mul(&t2, &gamma, &gamma, f);
    m_add(&t2, &t2, &t2, f);
    m_add(&t2, &t2, &t2, f);
    m_add(&t2, &t2, &t2, f);                          // 8*gamma^2
    m_sub(&y3, &y3, &t2, f);
    r->x = x3; r->y = y3; r->z = z3;
}

static void pt_add(pt384_t* r, const pt384_t* p, const pt384_t* q) {
    const mod384_t* f = &g_fp;
    if (n_is_zero(&p->z)) { *r = *q; return; }
    if (n_is_zero(&q->z)) { *r = *p; return; }
    n384_t z1z1, z2z2, u1, u2, s1, s2, h, i, j, rr, v, t, x3, y3, z3;
    m_mul(&z1z1, &p->z, &p->z, f);
    m_mul(&z2z2, &q->z, &q->z, f);
    m_mul(&u1, &p->x, &z2z2, f);
    m_mul(&u2, &q->x, &z1z1, f);
    m_mul(&s1, &p->y, &q->z, f);  m_mul(&s1, &s1, &z2z2, f);
    m_mul(&s2, &q->y, &p->z, f);  m_mul(&s2, &s2, &z1z1, f);
    m_sub(&h, &u2, &u1, f);
    m_sub(&rr, &s2, &s1, f);
    if (n_is_zero(&h)) {
        if (n_is_zero(&rr)) { pt_double(r, p); return; }      // одна и та же точка
        for (int k = 0; k < 6; k++) r->z.v[k] = 0;            // P + (-P)
        r->x = f->one; r->y = f->one;
        return;
    }
    m_add(&rr, &rr, &rr, f);                          // r = 2*(s2-s1)
    m_add(&i, &h, &h, f);
    m_mul(&i, &i, &i, f);                             // I = (2h)^2
    m_mul(&j, &h, &i, f);
    m_mul(&v, &u1, &i, f);
    m_mul(&x3, &rr, &rr, f);
    m_sub(&x3, &x3, &j, f);
    m_sub(&x3, &x3, &v, f);
    m_sub(&x3, &x3, &v, f);
    m_sub(&t, &v, &x3, f);
    m_mul(&y3, &rr, &t, f);
    m_mul(&t, &s1, &j, f);
    m_add(&t, &t, &t, f);
    m_sub(&y3, &y3, &t, f);
    m_add(&z3, &p->z, &q->z, f);
    m_mul(&z3, &z3, &z3, f);
    m_sub(&z3, &z3, &z1z1, f);
    m_sub(&z3, &z3, &z2z2, f);
    m_mul(&z3, &z3, &h, f);
    r->x = x3; r->y = y3; r->z = z3;
}

// Точка из несжатого вида 0x04 || X(48) || Y(48); проверяет y^2 = x^3 - 3x + b.
static bool pt_from_pub(pt384_t* out, const uint8_t pub[97]) {
    const mod384_t* f = &g_fp;
    if (pub[0] != 0x04) return false;
    n384_t x, y;
    n_from_be(&x, pub + 1, 48);
    n_from_be(&y, pub + 49, 48);
    if (n_cmp(&x, &P384_P) >= 0 || n_cmp(&y, &P384_P) >= 0) return false;
    n384_t xm, ym, bm, lhs, rhs, t;
    to_mont(&xm, &x, f); to_mont(&ym, &y, f); to_mont(&bm, &P384_B, f);
    m_mul(&lhs, &ym, &ym, f);
    m_mul(&rhs, &xm, &xm, f);
    m_mul(&rhs, &rhs, &xm, f);
    m_add(&t, &xm, &xm, f);
    m_add(&t, &t, &xm, f);
    m_sub(&rhs, &rhs, &t, f);
    m_add(&rhs, &rhs, &bm, f);
    if (n_cmp(&lhs, &rhs) != 0) return false;
    out->x = xm; out->y = ym; out->z = f->one;
    return true;
}

bool crypto_p384_validate_pub(const uint8_t pub[97]) {
    p384_init();
    pt384_t p;
    return pt_from_pub(&p, pub);
}

// ECDSA (FIPS 186-4 §6.4.2): w = s^-1, u1 = e*w, u2 = r*w, X = u1*G + u2*Q,
// подпись верна, если X.x mod n == r.
bool crypto_p384_ecdsa_verify(const uint8_t pub[97],
                              const uint8_t* hash, size_t hash_len,
                              const uint8_t* r_bytes, size_t r_len,
                              const uint8_t* s_bytes, size_t s_len) {
    p384_init();
    const mod384_t* fn = &g_fn;
    const mod384_t* fp = &g_fp;
    if (r_len == 0 || s_len == 0 || r_len > 48 || s_len > 48) return false;

    pt384_t Q;
    if (!pt_from_pub(&Q, pub)) return false;

    n384_t r, s, e;
    n_from_be(&r, r_bytes, r_len);
    n_from_be(&s, s_bytes, s_len);
    if (n_is_zero(&r) || n_is_zero(&s)) return false;
    if (n_cmp(&r, &P384_N) >= 0 || n_cmp(&s, &P384_N) >= 0) return false;

    // e — левые 384 бита хеша (SHA-512 обрезается, SHA-256 дополняется нулями слева).
    n_from_be(&e, hash, hash_len > 48 ? 48 : hash_len);
    if (n_cmp(&e, &P384_N) >= 0) n_sub(&e, &e, &P384_N);

    n384_t sm, rm, em, w, u1, u2;
    to_mont(&sm, &s, fn); to_mont(&rm, &r, fn); to_mont(&em, &e, fn);
    m_inv(&w, &sm, fn);
    m_mul(&u1, &em, &w, fn);  from_mont(&u1, &u1, fn);
    m_mul(&u2, &rm, &w, fn);  from_mont(&u2, &u2, fn);

    pt384_t G, GQ, acc;
    to_mont(&G.x, &P384_GX, fp); to_mont(&G.y, &P384_GY, fp); G.z = fp->one;
    pt_add(&GQ, &G, &Q);
    acc.x = fp->one; acc.y = fp->one;
    for (int i = 0; i < 6; i++) acc.z.v[i] = 0;

    for (int i = 383; i >= 0; i--) {
        pt_double(&acc, &acc);
        int b1 = (int)((u1.v[i / 64] >> (i % 64)) & 1);
        int b2 = (int)((u2.v[i / 64] >> (i % 64)) & 1);
        if (b1 && b2)  pt_add(&acc, &acc, &GQ);
        else if (b1)   pt_add(&acc, &acc, &G);
        else if (b2)   pt_add(&acc, &acc, &Q);
    }
    if (n_is_zero(&acc.z)) return false;

    // Аффинная x = X / Z^2, затем mod n.
    n384_t zi, zi2, x;
    m_inv(&zi, &acc.z, fp);
    m_mul(&zi2, &zi, &zi, fp);
    m_mul(&x, &acc.x, &zi2, fp);
    from_mont(&x, &x, fp);
    if (n_cmp(&x, &P384_N) >= 0) n_sub(&x, &x, &P384_N);
    return n_cmp(&x, &r) == 0;
}

static void n_to_be(const n384_t* a, uint8_t out[48]) {
    for (int i = 0; i < 48; i++) out[i] = (uint8_t)(a->v[(47 - i) / 8] >> (8 * ((47 - i) % 8)));
}

// out = k * P в аффинных координатах (48 байт X, 48 байт Y). k — 48 байт BE.
static bool pt_mul_affine(const uint8_t k_be[48], const pt384_t* P, uint8_t ox[48], uint8_t oy[48]) {
    const mod384_t* fp = &g_fp;
    n384_t k;
    n_from_be(&k, k_be, 48);
    if (n_is_zero(&k) || n_cmp(&k, &P384_N) >= 0) return false;
    pt384_t acc;
    acc.x = fp->one; acc.y = fp->one;
    for (int i = 0; i < 6; i++) acc.z.v[i] = 0;
    for (int i = 383; i >= 0; i--) {
        pt_double(&acc, &acc);
        if ((k.v[i / 64] >> (i % 64)) & 1) pt_add(&acc, &acc, P);
    }
    if (n_is_zero(&acc.z)) return false;
    n384_t zi, zi2, zi3, x, y;
    m_inv(&zi, &acc.z, fp);
    m_mul(&zi2, &zi, &zi, fp);
    m_mul(&zi3, &zi2, &zi, fp);
    m_mul(&x, &acc.x, &zi2, fp);  from_mont(&x, &x, fp);
    m_mul(&y, &acc.y, &zi3, fp);  from_mont(&y, &y, fp);
    n_to_be(&x, ox);
    if (oy) n_to_be(&y, oy);
    return true;
}

// Открытый ключ ECDHE: pub = scalar * G (несжатая точка 97 байт).
bool crypto_p384_derive_pub(const uint8_t scalar[48], uint8_t out_pub[97]) {
    p384_init();
    pt384_t G;
    to_mont(&G.x, &P384_GX, &g_fp); to_mont(&G.y, &P384_GY, &g_fp); G.z = g_fp.one;
    out_pub[0] = 0x04;
    return pt_mul_affine(scalar, &G, out_pub + 1, out_pub + 49);
}

// Общий секрет ECDH: координата X точки scalar * peer_pub (48 байт).
bool crypto_p384_ecdh(const uint8_t scalar[48], const uint8_t peer_pub[97], uint8_t shared[48]) {
    p384_init();
    pt384_t Q;
    if (!pt_from_pub(&Q, peer_pub)) return false;
    return pt_mul_affine(scalar, &Q, shared, 0);
}

// Самопроверка: базовая точка лежит на кривой (ловит опечатку в константах и
// ошибку в арифметике поля). Полная проверка подписи — в crypto_self_test.c.
bool crypto_p384_selftest(void) {
    uint8_t g[97];
    g[0] = 0x04;
    for (int i = 0; i < 48; i++) {
        g[1 + i]  = (uint8_t)(P384_GX.v[(47 - i) / 8] >> (8 * ((47 - i) % 8)));
        g[49 + i] = (uint8_t)(P384_GY.v[(47 - i) / 8] >> (8 * ((47 - i) % 8)));
    }
    if (!crypto_p384_validate_pub(g)) return false;
    // ECDH: (a*G)*b == (b*G)*a для двух произвольных скаляров.
    uint8_t a[48], b[48], pa[97], pb[97], s1[48], s2[48];
    for (int i = 0; i < 48; i++) { a[i] = (uint8_t)(0x11 + 3 * i); b[i] = (uint8_t)(0x7A - 2 * i); }
    if (!crypto_p384_derive_pub(a, pa) || !crypto_p384_derive_pub(b, pb)) return false;
    if (!crypto_p384_validate_pub(pa) || !crypto_p384_validate_pub(pb)) return false;
    if (!crypto_p384_ecdh(a, pb, s1) || !crypto_p384_ecdh(b, pa, s2)) return false;
    for (int i = 0; i < 48; i++) if (s1[i] != s2[i]) return false;
    return true;
}

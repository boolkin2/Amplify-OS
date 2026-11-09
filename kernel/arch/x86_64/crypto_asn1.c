// ============================================================================
// crypto_asn1.c — DER парсер (этап 47.3).
// ----------------------------------------------------------------------------
// Реализация ровно того, что нужно X.509 v3:
//   * TAG = 1 byte (мы не поддерживаем multi-byte tags).
//   * LENGTH = short form (<128) или long form (top bit = 1, next n bytes
//     это big-endian длина).
//   * VALUE — payload.
//
// Helper'ы для INTEGER (uint32 + bytes), OID, UTCTime/GeneralizedTime.
// ============================================================================
#include "crypto_asn1.h"

bool asn1_parse(const uint8_t* buf, size_t buf_len, asn1_t* out) {
    if (buf_len < 2) return false;
    out->tag = buf[0];

    // Length parsing.
    size_t pos = 1;
    uint32_t length;
    if (buf[1] < 0x80) {
        length = buf[1];
        pos    = 2;
    } else {
        uint32_t nbytes = buf[1] & 0x7F;
        if (nbytes == 0 || nbytes > 4) return false;       // indefinite or >4G — нет
        if (buf_len < 2u + nbytes) return false;
        length = 0;
        for (uint32_t i = 0; i < nbytes; i++) length = (length << 8) | buf[2 + i];
        pos = 2 + nbytes;
    }
    if (buf_len < pos + length) return false;
    out->val = buf + pos;
    out->len = length;
    out->end = out->val + out->len;
    return true;
}

bool asn1_parse_at(const uint8_t** p, const uint8_t* end, asn1_t* out) {
    if (*p >= end) return false;
    if (!asn1_parse(*p, (size_t)(end - *p), out)) return false;
    *p = out->end;
    return true;
}

bool asn1_oid_eq(const asn1_t* n, const uint8_t* oid, size_t oid_len) {
    if (n->tag != ASN1_TAG_OID) return false;
    if (n->len != oid_len) return false;
    for (size_t i = 0; i < oid_len; i++) {
        if (n->val[i] != oid[i]) return false;
    }
    return true;
}

bool asn1_int_to_u32(const asn1_t* n, uint32_t* out) {
    if (n->tag != ASN1_TAG_INTEGER) return false;
    if (n->len == 0 || n->len > 5) return false;
    const uint8_t* p   = n->val;
    size_t         len = n->len;
    // BER кодирует positive integers с возможным leading 0x00 для
    // disambiguation от negative (high-bit = 0).
    if (len == 5) {
        if (p[0] != 0x00) return false;
        p++; len--;
    }
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) v = (v << 8) | p[i];
    *out = v;
    return true;
}

bool asn1_int_as_bytes(const asn1_t* n, const uint8_t** bytes, size_t* len) {
    if (n->tag != ASN1_TAG_INTEGER) return false;
    if (n->len == 0) return false;
    const uint8_t* p   = n->val;
    size_t         l   = n->len;
    // Пропустить leading 0x00 padding (uses unsigned representation).
    if (l > 1 && p[0] == 0x00) { p++; l--; }
    *bytes = p;
    *len   = l;
    return true;
}

// Преобразование YYYYMMDDHHMMSS в unix time (с алгоритмом Howard Hinnant).
static uint64_t to_unix(int y, int m, int d, int hh, int mm, int ss) {
    // Хрестоматийный civil → days algorithm.
    int  yy = (m <= 2) ? y - 1 : y;
    int  era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = (unsigned)(yy - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t  days = (int64_t)era * 146097 + (int64_t)doe - 719468;   // дней с 1970-01-01
    return (uint64_t)(days * 86400 + hh * 3600 + mm * 60 + ss);
}

static int two_digits(const uint8_t* p) {
    if (p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

bool asn1_time_to_unix(const asn1_t* n, uint64_t* out) {
    // UTCTime  = YYMMDDHHMMSSZ        (13 байт)
    // GenTime  = YYYYMMDDHHMMSSZ      (15 байт)
    int y, m, d, hh, mm, ss;
    const uint8_t* p = n->val;
    if (n->tag == ASN1_TAG_UTCTIME && n->len == 13 && p[12] == 'Z') {
        int yy = two_digits(p);
        if (yy < 0) return false;
        y  = (yy < 50) ? 2000 + yy : 1900 + yy;
        m  = two_digits(p + 2);
        d  = two_digits(p + 4);
        hh = two_digits(p + 6);
        mm = two_digits(p + 8);
        ss = two_digits(p + 10);
    } else if (n->tag == ASN1_TAG_GENTIME && n->len == 15 && p[14] == 'Z') {
        int y_hi = two_digits(p);
        int y_lo = two_digits(p + 2);
        if (y_hi < 0 || y_lo < 0) return false;
        y  = y_hi * 100 + y_lo;
        m  = two_digits(p + 4);
        d  = two_digits(p + 6);
        hh = two_digits(p + 8);
        mm = two_digits(p + 10);
        ss = two_digits(p + 12);
    } else {
        return false;
    }
    if (m < 1 || m > 12 || d < 1 || d > 31) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59 || ss < 0 || ss > 60) return false;
    *out = to_unix(y, m, d, hh, mm, ss);
    return true;
}

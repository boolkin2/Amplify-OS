// ============================================================================
// tls.c — TLS 1.2 client (этап 47.4).
// ----------------------------------------------------------------------------
// Структура файла:
//   1) Утилиты (BE-encoding, send_all / recv_exact через TCP).
//   2) Record layer: send_record / recv_record (с GCM encrypt/decrypt).
//   3) Handshake message builders/parsers.
//   4) RSA encrypt premaster (PKCS#1 v1.5 type 2).
//   5) KDF: master_secret + key_block через TLS 1.2 PRF SHA-256.
//   6) Главный handshake driver.
//   7) tls_send / tls_recv / tls_close.
//
// Используем глобальный tcp.c (одно TCP-соединение разом) и
// crypto_rand_bytes / crypto_sha256 / crypto_gcm_* / crypto_rsa_*
// + crypto_x509 + crypto_ca из 47.1..47.3.
// ============================================================================
#include "tls.h"
#include "tcp.h"
#include "netif.h"
#include "crypto.h"
#include "crypto_x509.h"
#include "crypto_ca.h"
#include "crypto_asn1.h"
#include "serial.h"
#include "rtc.h"
#include "timer.h"

// ---- Утилиты ---------------------------------------------------------------

static void put_be16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_be24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
}
static void put_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; i--) { p[i] = (uint8_t)v; v >>= 8; }
}
static uint16_t get_be16(const uint8_t* p) { return ((uint16_t)p[0] << 8) | p[1]; }
static uint32_t get_be24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
}

static size_t s_strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void   s_memcpy(void* d, const void* s, size_t n) {
    uint8_t* dd = (uint8_t*)d; const uint8_t* ss = (const uint8_t*)s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
}

// ---- Низкоуровневая отправка/приём через TCP -------------------------------

// send_all: возвращает true если все байты отправлены.
static bool tcp_send_all(const uint8_t* buf, size_t len) {
    while (len > 0) {
        size_t sent = tcp_send(buf, len);
        if (sent == 0) return false;
        buf += sent;
        len -= sent;
    }
    return true;
}

// [48.2] «Сервер не сказал ни слова»: TCP-соединение есть, ClientHello ушёл,
// а в ответ — тишина до таймаута. Так выглядит фильтрация по имени сайта на
// пути к нему (пакет с SNI отбрасывается), и это стоит отличать от «не
// договорились о шифре»: g_rx_any — пришёл ли хоть байт с начала рукопожатия.
static bool g_rx_any = false;
static bool g_silent = false;
// Сколько ждать очередных байт: 10 с на рукопожатие (молчащий сервер — отказ),
// 30 с на данные страницы — сервер, который собирает ответ на ходу (wttr.in),
// между кусками думает дольше 10 секунд; загрузку всегда можно прервать Esc.
static uint32_t g_rx_wait_ms = 10000;
bool tls_handshake_silent(void) { return g_silent; }

// recv_exact: блокирует до получения ровно `len` байт.  Таймаут 10 сек.
static bool tcp_recv_exact(uint8_t* buf, size_t len) {
    uint64_t deadline = timer_get_uptime_ms() + g_rx_wait_ms;
    while (len > 0) {
        size_t got = 0;
        if (tcp_recv(buf, len, &got) && got > 0) {
            g_rx_any = true;
            buf += got;
            len -= got;
            deadline = timer_get_uptime_ms() + g_rx_wait_ms;
        } else {
            // [48.2] Сервер закрыл соединение (FIN) и буфер пуст — данных больше
            // не будет. Раньше проверялось только состояние CLOSED, а после FIN
            // сокет в CLOSE_WAIT: сервер, закрывший TCP без close_notify,
            // заставлял ждать все 10 секунд.
            // Причина — в serial: без неё «error tls» через 10 секунд
            // (сервер молчит: фильтр на пути съел ClientHello) не отличить от
            // обрыва соединения сервером.
            if (tcp_peer_closed()) { serial_puts("[tls] recv: peer closed the connection\n"); return false; }
            if (net_cancelled()) return false;
            if (timer_get_uptime_ms() > deadline) {
                serial_puts("[tls] recv: no data for ");
                serial_print_dec(g_rx_wait_ms / 1000);
                serial_puts(" s, still need ");
                serial_print_dec(len); serial_puts(" bytes\n");
                if (!g_rx_any) g_silent = true;
                return false;
            }
            net_idle();
        }
    }
    return true;
}

// ---- Record layer ----------------------------------------------------------
// TLS Record (RFC 5246 §6.2):
//   ContentType type;             1 byte
//   ProtocolVersion version;      2 bytes (0x0303 for TLS 1.2)
//   uint16 length;                2 bytes (length of payload, ≤ 2^14 + overhead)
//   opaque fragment[length];      payload
//
// Encrypted records (after CCS) have payload =
//   nonce_explicit[8] || ciphertext || tag[16]
// AEAD = GCM, AAD = seq_num[8] || type[1] || version[2] || plaintext_length[2].
// Nonce = implicit_iv[4] || explicit_nonce[8].

#define TLS_RECORD_CHANGE_CIPHER  20
#define TLS_RECORD_ALERT          21
#define TLS_RECORD_HANDSHAKE      22
#define TLS_RECORD_APPLICATION    23

#define TLS_VERSION_1_2           0x0303

// Запись уходит ОДНИМ куском: заголовок (5 байт) и тело собираются в общем
// буфере. Раньше заголовок шёл отдельным вызовом tcp_send — то есть отдельным
// TCP-сегментом в 5 байт с ожиданием его ACK (tcp_send ждёт подтверждения
// каждого сегмента): лишний круг по сети на каждую запись, а ClientHello,
// разрезанный после пятого байта, часть серверов и фильтров перед ними
// (www.bbc.com) оставляет без ответа.
//
// «Пробка» (g_cork): три записи второго полёта клиента — ClientKeyExchange,
// ChangeCipherSpec, Finished — копятся и уходят одним сегментом, как у
// браузеров. По отдельности это три сегмента по 6–110 байт, и каждый ждал бы
// своего ACK: два лишних круга до сервера на каждое рукопожатие.
static bool    g_cork = false;
static uint8_t g_flight[1024];
static size_t  g_flight_len = 0;

static bool send_record_internal(tls_ctx_t* ctx, uint8_t type,
                                 const uint8_t* payload, size_t plen) {
    static uint8_t rec[5 + TLS_MAX_RECORD];
    uint8_t* body = rec + 5;
    size_t   total;

    rec[0] = type;
    put_be16(rec + 1, TLS_VERSION_1_2);

    if (!ctx->write_encrypted) {
        // Cleartext record. Запись с ClientHello помечаем версией 1.0 (0x0301),
        // как все браузеры и OpenSSL: старые серверы и фильтры на пути
        // «не понимают» первую запись с версией выше и молча её отбрасывают
        // (RFC 5246, прил. E.1). Версия рукопожатия внутри — по-прежнему 1.2.
        if (plen > TLS_MAX_RECORD) return false;
        if (ctx->state == TLS_STATE_INIT) put_be16(rec + 1, 0x0301);
        s_memcpy(body, payload, plen);
        total = plen;
    } else {
        // Encrypted: AES-GCM или ChaCha20-Poly1305.
        // AAD: seq(8) | type(1) | ver(2) | len(2)
        uint8_t aad[13];
        put_be64(aad, ctx->client_seq);
        aad[8] = type;
        put_be16(aad + 9, TLS_VERSION_1_2);
        put_be16(aad + 11, (uint16_t)plen);
        uint8_t tag[16];
        if (plen + 24 > TLS_MAX_RECORD) return false;

        if (ctx->aead == TLS_AEAD_CHACHA20) {
            // RFC 7905: nonce = IV(12) XOR (0^4 || seq(8)); явной части в
            // записи нет, запись = шифртекст || тег.
            uint8_t cn[12];
            s_memcpy(cn, ctx->client_write_iv, 12);
            for (int i = 0; i < 8; i++) cn[4 + i] ^= (uint8_t)(ctx->client_seq >> (56 - 8 * i));
            crypto_chacha20_poly1305_seal(ctx->client_write_key, cn, aad, 13,
                                          payload, body, plen, tag);
            s_memcpy(body + plen, tag, 16);
            total = plen + 16;
        } else {
            // GCM: nonce = implicit_iv[4] | explicit_nonce[8] (номер записи);
            // запись = explicit_nonce[8] || шифртекст || тег[16].
            uint8_t nonce[12];
            s_memcpy(nonce, ctx->client_write_iv, 4);
            put_be64(nonce + 4, ctx->client_seq);
            put_be64(body, ctx->client_seq);
            s_memcpy(body + 8, payload, plen);
            crypto_gcm_encrypt(&ctx->client_gcm, nonce, aad, 13,
                               body + 8, body + 8, plen, tag);
            s_memcpy(body + 8 + plen, tag, 16);
            total = 8 + plen + 16;
        }
        ctx->client_seq++;
    }

    put_be16(rec + 3, (uint16_t)total);
    if (g_cork) {
        if (g_flight_len + 5 + total <= sizeof(g_flight)) {
            s_memcpy(g_flight + g_flight_len, rec, 5 + total);
            g_flight_len += 5 + total;
            return true;
        }
        // Не влезло (ключ RSA-8192?) — сначала накопленное, порядок записей важен.
        if (g_flight_len && !tcp_send_all(g_flight, g_flight_len)) return false;
        g_flight_len = 0;
    }
    return tcp_send_all(rec, 5 + total);
}

// recv_record: блокирует до получения ОДНОГО record'а.  Возвращает type и
// pointer в buf+5 + len (без header'а).  Decrypt'ит если read_encrypted.
// out_payload может указывать в ctx->rx_buf (статический шеренный буфер).
static bool recv_record_internal(tls_ctx_t* ctx, uint8_t* type_out,
                                 uint8_t** payload_out, size_t* plen_out) {
    uint8_t hdr[5];
    if (!tcp_recv_exact(hdr, 5)) return false;
    uint8_t  type = hdr[0];
    uint16_t ver  = get_be16(hdr + 1);
    uint16_t rlen = get_be16(hdr + 3);
    if (ver != TLS_VERSION_1_2 && !(ver == 0x0301)) {
        // Некоторые серверы шлют 0x0301 в первом ServerHello (compat).  Допустим.
        // Но в дальнейшем ожидаем 0x0303.
    }
    if (rlen > TLS_MAX_RECORD - 5) return false;

    if (!tcp_recv_exact(ctx->rx_buf, rlen)) return false;

    if (!ctx->read_encrypted) {
        *type_out    = type;
        *payload_out = ctx->rx_buf;
        *plen_out    = rlen;
        return true;
    }

    if (ctx->aead == TLS_AEAD_CHACHA20) {
        // Запись = шифртекст || тег[16], nonce = IV XOR номер записи.
        if (rlen < 16) return false;
        size_t clen = rlen - 16;
        uint8_t cn[12];
        s_memcpy(cn, ctx->server_write_iv, 12);
        for (int i = 0; i < 8; i++) cn[4 + i] ^= (uint8_t)(ctx->server_seq >> (56 - 8 * i));
        uint8_t caad[13];
        put_be64(caad, ctx->server_seq);
        caad[8] = type;
        put_be16(caad + 9, TLS_VERSION_1_2);
        put_be16(caad + 11, (uint16_t)clen);
        if (!crypto_chacha20_poly1305_open(ctx->server_write_key, cn, caad, 13,
                                           ctx->rx_buf, ctx->rx_buf, clen, ctx->rx_buf + clen)) {
            serial_puts("[tls] ChaCha20-Poly1305 decrypt failed (bad tag)\n");
            return false;
        }
        *type_out    = type;
        *payload_out = ctx->rx_buf;
        *plen_out    = clen;
        ctx->server_seq++;
        return true;
    }

    // Decrypt: payload = explicit_nonce[8] || ciphertext || tag[16].
    if (rlen < 8 + 16) return false;
    size_t ct_len = rlen - 8 - 16;

    uint8_t nonce[12];
    s_memcpy(nonce, ctx->server_write_iv, 4);
    s_memcpy(nonce + 4, ctx->rx_buf, 8);     // server explicit_nonce

    uint8_t aad[13];
    put_be64(aad, ctx->server_seq);
    aad[8] = type;
    put_be16(aad + 9, TLS_VERSION_1_2);
    put_be16(aad + 11, (uint16_t)ct_len);

    uint8_t* ct  = ctx->rx_buf + 8;
    uint8_t* tag = ctx->rx_buf + 8 + ct_len;
    if (!crypto_gcm_decrypt(&ctx->server_gcm, nonce, aad, 13, ct, ct, ct_len, tag)) {
        serial_puts("[tls] GCM decrypt failed (bad tag)\n");
        return false;
    }
    *type_out    = type;
    *payload_out = ct;
    *plen_out    = ct_len;
    ctx->server_seq++;
    return true;
}

// ---- Handshake hash update -------------------------------------------------
// Оба хеша считаются параллельно: нужный (SHA-256 или SHA-384) определяет
// набор шифров из ServerHello, а ClientHello к тому времени уже учтён.
static void hs_hash_update(tls_ctx_t* ctx, const uint8_t* data, size_t len) {
    crypto_sha256_update(&ctx->handshake_hash, data, len);
    crypto_sha512_update(&ctx->handshake_hash384, data, len);
}

// Хеш всех сообщений рукопожатия на данный момент (копия контекста —
// сам поток продолжается). Возвращает длину: 32 или 48.
static size_t hs_hash_now(tls_ctx_t* ctx, uint8_t out[48]) {
    if (ctx->prf_sha384) {
        crypto_sha512_ctx_t c = ctx->handshake_hash384;
        crypto_sha384_final(&c, out);
        return 48;
    }
    crypto_sha256_ctx_t c = ctx->handshake_hash;
    crypto_sha256_final(&c, out);
    return 32;
}

static void tls_prf(tls_ctx_t* ctx, const void* secret, size_t secret_len, const char* label,
                    const void* seed, size_t seed_len, uint8_t* out, size_t out_len) {
    if (ctx->prf_sha384) crypto_tls12_prf_sha384(secret, secret_len, label, seed, seed_len, out, out_len);
    else                 crypto_tls12_prf_sha256(secret, secret_len, label, seed, seed_len, out, out_len);
}

// ---- Handshake message dispatch --------------------------------------------
// HS message: 1 byte type + 3 byte length + body.

#define HS_CLIENT_HELLO         1
#define HS_SERVER_HELLO         2
#define HS_CERTIFICATE          11
#define HS_SERVER_KEY_EXCHANGE  12
#define HS_CERT_REQUEST         13
#define HS_SERVER_HELLO_DONE    14
#define HS_CLIENT_KEY_EXCHANGE  16
#define HS_FINISHED             20

// ---- ClientHello -----------------------------------------------------------
// RFC 5246 §7.4.1.2:
//   ProtocolVersion         client_version;        // 0x0303
//   Random                  random;                // 32 bytes
//   SessionID               session_id;            // <= 32 bytes
//   CipherSuite             cipher_suites<2..2^16-2>;
//   CompressionMethod       compression_methods<1..2^8-1>;
//   Extension               extensions<0..2^16-1>;
//
// Extensions (этап 47.6):
//   * SNI (server_name)         — обязательно для multi-tenant hosts.
//   * signature_algorithms      — RSA+SHA-256/384/512 + ECDSA+SHA-256/384.
//   * supported_groups          — secp256r1 (для ECDHE).
//   * ec_point_formats          — uncompressed (RFC 8422).
//   * ALPN                      — broadcast "h2" + "http/1.1".
//
// Cipher suites (this client): мы предлагаем оба вариант, серверу выбирать:
//   * 0xC02F TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (предпочтительно, modern)
//   * 0x009C TLS_RSA_WITH_AES_128_GCM_SHA256       (fallback для legacy)
static bool send_client_hello(tls_ctx_t* ctx) {
    // Generate client_random.
    crypto_rand_bytes(ctx->client_random, 32);

    // Buffer: достаточно большой запас для всех extensions.
    size_t host_len = s_strlen(ctx->hostname);
    uint8_t  body[1024];
    size_t   p = 0;

    body[p++] = 0x03; body[p++] = 0x03;             // client_version 1.2
    s_memcpy(body + p, ctx->client_random, 32); p += 32;
    body[p++] = 0;                                  // session_id length = 0

    // cipher_suites (этап 47.7): 3 entries (6 bytes total).
    // Порядок = preference, server должен выбрать ближайший к началу,
    // совместимый со своим cert'ом.  ECDHE-ECDSA добавлен первым —
    // он наиболее современный (forward secrecy + ECDSA leaf cert от Cloudflare etc.)
    // [48.2] Восемь наборов. AES-128-GCM впереди (быстрее всего с AES-NI),
    // затем ChaCha20 и AES-256 — для серверов, у которых AES-128 выключен;
    // обмен ключами RSA (без прямой секретности) — в самом конце.
    static const uint16_t suites[] = {
        TLS_CS_ECDHE_ECDSA_AES128_GCM_SHA256, TLS_CS_ECDHE_RSA_AES128_GCM_SHA256,
        TLS_CS_ECDHE_ECDSA_CHACHA20_POLY1305, TLS_CS_ECDHE_RSA_CHACHA20_POLY1305,
        TLS_CS_ECDHE_ECDSA_AES256_GCM_SHA384, TLS_CS_ECDHE_RSA_AES256_GCM_SHA384,
        TLS_CS_RSA_AES128_GCM_SHA256,         TLS_CS_RSA_AES256_GCM_SHA384,
    };
    put_be16(body + p, (uint16_t)sizeof(suites)); p += 2;      // cipher_suites length
    for (size_t i = 0; i < sizeof(suites) / sizeof(suites[0]); i++) {
        put_be16(body + p, suites[i]); p += 2;
    }

    body[p++] = 1;                                  // compression methods length
    body[p++] = 0;                                  //   = null

    // Extensions length placeholder
    size_t ext_len_off = p; p += 2;
    size_t ext_start   = p;

    // --- SNI extension (0x0000) ---
    put_be16(body + p, 0x0000); p += 2;             // ext type
    put_be16(body + p, (uint16_t)(host_len + 5)); p += 2;  // ext data len
    put_be16(body + p, (uint16_t)(host_len + 3)); p += 2;  // server_name_list length
    body[p++] = 0;                                  // name_type = host_name
    put_be16(body + p, (uint16_t)host_len); p += 2;
    s_memcpy(body + p, ctx->hostname, host_len); p += host_len;

    // --- signature_algorithms extension (0x000D) ---
    // RFC 5246 §7.4.1.4.1: SignatureAndHashAlgorithm = (hash:1, sig:1).
    //   hash:  0x02 sha1, 0x04 sha256, 0x05 sha384, 0x06 sha512.
    //   sig:   0x01 RSA, 0x03 ECDSA.
    // Список (предпочтения сверху вниз): RSA-SHA256, RSA-SHA384, RSA-SHA512,
    //                                    ECDSA-SHA256, ECDSA-SHA384.
    put_be16(body + p, 0x000D); p += 2;
    put_be16(body + p, 14); p += 2;                 // ext data len = 2 + 12
    put_be16(body + p, 12); p += 2;                 // sig_algs length = 6*2
    body[p++] = 0x04; body[p++] = 0x03;             // sha256 + ecdsa
    body[p++] = 0x05; body[p++] = 0x03;             // sha384 + ecdsa
    body[p++] = 0x06; body[p++] = 0x03;             // sha512 + ecdsa
    body[p++] = 0x04; body[p++] = 0x01;             // sha256 + rsa
    body[p++] = 0x05; body[p++] = 0x01;             // sha384 + rsa
    body[p++] = 0x06; body[p++] = 0x01;             // sha512 + rsa

    // --- supported_groups (0x000A) — для ECDHE ---
    // RFC 8422: NamedCurve secp256r1 = 0x0017, secp384r1 = 0x0018.
    // Мы поддерживаем только secp256r1.
    put_be16(body + p, 0x000A); p += 2;             // ext type
    put_be16(body + p, 8); p += 2;                  // ext data len
    put_be16(body + p, 6); p += 2;                  // groups list length
    put_be16(body + p, TLS_GROUP_X25519);    p += 2;   // [48.2] x25519 — первым
    put_be16(body + p, TLS_GROUP_SECP256R1); p += 2;   // secp256r1
    // secp384r1 — последним: серверы на OpenSSL сверяют кривые цепочки
    // сертификатов с этим списком и без P-384 отвечают alert 40
    // (lite.cnn.com), хотя сам обмен ключами потом идёт на x25519.
    put_be16(body + p, TLS_GROUP_SECP384R1); p += 2;

    // --- ec_point_formats (0x000B) — RFC 8422 §5.1.2 ---
    //   uncompressed = 0
    put_be16(body + p, 0x000B); p += 2;
    put_be16(body + p, 2); p += 2;                  // ext data len
    body[p++] = 1;                                  // formats list length
    body[p++] = 0;                                  // uncompressed

    // --- ALPN (0x0010) — RFC 7301 ---
    // ProtocolNameList: list of <length, name> pairs.
    // Этап 47.7: предлагаем ТОЛЬКО "http/1.1".  Раньше предлагали ещё "h2",
    // но если сервер выбирал HTTP/2 — наш HTTP/1.x parser не понимал binary
    // frame'ы и возвращал status=0.  HTTP/2 — отдельный большой проект
    // (HPACK, frames, streams), отложен.
    {
        const char* protos[] = { "http/1.1" };
        size_t list_total = 0;
        for (size_t i = 0; i < sizeof(protos)/sizeof(protos[0]); i++) {
            list_total += 1 + s_strlen(protos[i]);
        }
        put_be16(body + p, 0x0010); p += 2;
        put_be16(body + p, (uint16_t)(list_total + 2)); p += 2;  // ext data len
        put_be16(body + p, (uint16_t)list_total); p += 2;        // list len
        for (size_t i = 0; i < sizeof(protos)/sizeof(protos[0]); i++) {
            size_t n = s_strlen(protos[i]);
            body[p++] = (uint8_t)n;
            s_memcpy(body + p, protos[i], n);
            p += n;
        }
    }

    // --- [48.2] extended_master_secret (0x0017, RFC 7627) — пустое ---
    // Привязывает master_secret к хешу рукопожатия; строгие серверы без него
    // соединение не принимают. Применяем, только если сервер ответил тем же.
    put_be16(body + p, 0x0017); p += 2;
    put_be16(body + p, 0); p += 2;

    // --- [48.2] renegotiation_info (0xFF01, RFC 5746) — пустое ---
    // «Я знаю о безопасном пересогласовании» (само пересогласование не
    // поддерживаем): часть серверов отвергает клиентов без этой отметки.
    put_be16(body + p, 0xFF01); p += 2;
    put_be16(body + p, 1); p += 2;
    body[p++] = 0;

    size_t ext_total = p - ext_start;
    put_be16(body + ext_len_off, (uint16_t)ext_total);

    // Wrap in handshake message header.
    static uint8_t hs[4 + 1024];
    hs[0] = HS_CLIENT_HELLO;
    put_be24(hs + 1, (uint32_t)p);
    s_memcpy(hs + 4, body, p);

    hs_hash_update(ctx, hs, 4 + p);
    return send_record_internal(ctx, TLS_RECORD_HANDSHAKE, hs, 4 + p);
}

// ---- Parse ServerHello -----------------------------------------------------
// Парсим cipher_suite и extensions (особенно ALPN — этап 47.6).
static bool parse_server_hello(tls_ctx_t* ctx, const uint8_t* p, size_t len) {
    if (len < 2 + 32 + 1) return false;
    uint16_t ver = get_be16(p); p += 2; len -= 2;
    if (ver != TLS_VERSION_1_2) {
        serial_puts("[tls] server uses non-TLS-1.2 version\n");
        return false;
    }
    s_memcpy(ctx->server_random, p, 32); p += 32; len -= 32;
    uint8_t sid_len = *p++; len--;
    if (sid_len > len) return false;
    p += sid_len; len -= sid_len;
    if (len < 2 + 1) return false;
    uint16_t cs = get_be16(p); p += 2; len -= 2;
    ctx->cipher_suite = cs;
    // Поддерживаем три cipher suite (47.7).  Если сервер выбрал что-то ещё —
    // отказ.
    ctx->aead = TLS_AEAD_AES128_GCM;
    ctx->prf_sha384 = false;
    switch (cs) {
    case TLS_CS_RSA_AES128_GCM_SHA256:
        ctx->is_ecdhe = false; ctx->is_ecdsa = false; break;
    case TLS_CS_ECDHE_RSA_AES128_GCM_SHA256:
        ctx->is_ecdhe = true;  ctx->is_ecdsa = false; break;
    case TLS_CS_ECDHE_ECDSA_AES128_GCM_SHA256:
        ctx->is_ecdhe = true;  ctx->is_ecdsa = true;  break;
    case TLS_CS_RSA_AES256_GCM_SHA384:
        ctx->is_ecdhe = false; ctx->is_ecdsa = false;
        ctx->aead = TLS_AEAD_AES256_GCM; ctx->prf_sha384 = true; break;
    case TLS_CS_ECDHE_RSA_AES256_GCM_SHA384:
        ctx->is_ecdhe = true;  ctx->is_ecdsa = false;
        ctx->aead = TLS_AEAD_AES256_GCM; ctx->prf_sha384 = true; break;
    case TLS_CS_ECDHE_ECDSA_AES256_GCM_SHA384:
        ctx->is_ecdhe = true;  ctx->is_ecdsa = true;
        ctx->aead = TLS_AEAD_AES256_GCM; ctx->prf_sha384 = true; break;
    case TLS_CS_ECDHE_RSA_CHACHA20_POLY1305:
        ctx->is_ecdhe = true;  ctx->is_ecdsa = false;
        ctx->aead = TLS_AEAD_CHACHA20; break;
    case TLS_CS_ECDHE_ECDSA_CHACHA20_POLY1305:
        ctx->is_ecdhe = true;  ctx->is_ecdsa = true;
        ctx->aead = TLS_AEAD_CHACHA20; break;
    default:
        serial_puts("[tls] server chose unsupported cipher suite\n");
        return false;
    }
    if (len < 1) return false;
    uint8_t cm = *p++; len--;
    if (cm != 0) {
        serial_puts("[tls] server chose compression\n");
        return false;
    }

    // ---- Extensions ----
    if (len < 2) return true;        // нет extensions — допустимо (RFC 5246)
    uint16_t ext_total = get_be16(p); p += 2; len -= 2;
    if (ext_total > len) return false;
    const uint8_t* ext_end = p + ext_total;
    while (p + 4 <= ext_end) {
        uint16_t ext_type = get_be16(p); p += 2;
        uint16_t ext_len  = get_be16(p); p += 2;
        if (p + ext_len > ext_end) return false;
        if (ext_type == 0x0010) {
            // ALPN: server-selected protocol.
            // ProtocolNameList = uint16 list_len; opaque<1..255>+
            if (ext_len >= 3) {
                // Внутри: 2 байта list_len, затем 1 байт name_len, затем name.
                uint16_t list_len = get_be16(p);
                if (list_len + 2 == ext_len && list_len >= 1) {
                    uint8_t name_len = p[2];
                    if (3 + name_len <= ext_len &&
                        name_len <= TLS_MAX_ALPN_PROTO) {
                        for (int i = 0; i < name_len; i++) {
                            ctx->alpn_selected[i] = (char)p[3 + i];
                        }
                        ctx->alpn_selected[name_len] = 0;
                    }
                }
            }
        }
        if (ext_type == 0x0017) ctx->ems = true;      // [48.2] extended_master_secret принят
        // Игнорируем все прочие extensions (key_share, supported_versions —
        // это TLS 1.3, но серверы могут шлепнуть и в 1.2).
        p += ext_len;
    }
    return true;
}

// ---- helper: RTC → unix time (этап 47.6) -----------------------------------
// Простая конверсия для chain validation.  Не строгая (без leap-seconds),
// но достаточная для проверки X.509 not_before / not_after.
static uint64_t rtc_now_unix(void) {
    rtc_time_t t;
    rtc_read(&t);
    if (t.year < 1970 || t.year > 2099) return 0;
    // Days от 1970-01-01 до t.year-01-01:
    static const int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    uint64_t days = 0;
    for (int y = 1970; y < (int)t.year; y++) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        days += leap ? 366 : 365;
    }
    bool leap = (t.year % 4 == 0 && (t.year % 100 != 0 || t.year % 400 == 0));
    for (int m = 1; m < t.month; m++) {
        days += days_in_month[m - 1];
        if (m == 2 && leap) days += 1;
    }
    days += t.day - 1;
    return days * 86400ULL + t.hour * 3600ULL + t.minute * 60ULL + t.second;
}

// ---- Parse Certificate -----------------------------------------------------
// RFC 5246 §7.4.2:
//   opaque ASN.1Cert<1..2^24-1>;
//   struct {
//       ASN.1Cert certificate_list<0..2^24-1>;
//   } Certificate;
// Список (3-byte length, then sequence of [3-byte length, then DER cert]).
//
// Этап 47.6: сохраняем ВСЮ цепочку (до TLS_MAX_CERTS_IN_CHAIN сертификатов)
// и валидируем через x509_verify_chain — strict проверка с RTC time + KeyUsage
// + BasicConstraints + ExtKeyUsage.  Без trusted chain — fail handshake
// (раньше был best-effort warning, теперь по умолчанию строго).
static bool parse_certificate(tls_ctx_t* ctx, const uint8_t* p, size_t len) {
    if (len < 3) return false;
    uint32_t list_len = get_be24(p); p += 3; len -= 3;
    if (list_len > len) return false;
    if (list_len < 3) return false;

    ctx->cert_chain_count = 0;

    // Walk все cert'ы и сохраняем в ctx->cert_chain_raw[].
    while (list_len >= 3) {
        if (ctx->cert_chain_count >= TLS_MAX_CERTS_IN_CHAIN) {
            serial_puts("[tls] cert chain truncated (too many intermediates)\n");
            break;
        }
        uint32_t cert_len = get_be24(p); p += 3; list_len -= 3;
        if (cert_len > list_len) return false;
        if (cert_len > TLS_MAX_CERT_RAW) {
            serial_puts("[tls] one of cert's too large\n");
            return false;
        }
        size_t idx = ctx->cert_chain_count;
        s_memcpy(ctx->cert_chain_raw[idx], p, cert_len);
        ctx->cert_chain_lens[idx] = cert_len;
        ctx->cert_chain_count++;
        p += cert_len; list_len -= cert_len;
    }

    if (ctx->cert_chain_count == 0) return false;

    // Парсим leaf отдельно — он понадобится в derive_keys (RSA encrypt premaster
    // или подпись verify в SKE).
    if (!x509_parse(ctx->cert_chain_raw[0], ctx->cert_chain_lens[0],
                    &ctx->server_cert)) {
        serial_puts("[tls] x509_parse failed on leaf\n");
        return false;
    }

    // Этап 47.7: теперь поддерживаем оба типа leaf:
    //   * RSA  для cipher suites 0x009C, 0xC02F.
    //   * EC P-256 для 0xC02B (ECDHE-ECDSA).
    if (ctx->server_cert.pk_type != X509_PK_RSA &&
        ctx->server_cert.pk_type != X509_PK_EC_P256 &&
        ctx->server_cert.pk_type != X509_PK_EC_P384) {
        serial_puts("[tls] unsupported leaf public key type\n");
        return false;
    }
    // Соответствие cipher_suite ↔ pk_type:
    if (ctx->is_ecdsa) {
        if (ctx->server_cert.pk_type != X509_PK_EC_P256 &&
            ctx->server_cert.pk_type != X509_PK_EC_P384) {
            serial_puts("[tls] ECDHE-ECDSA chosen, but leaf cert is not ECDSA P-256/P-384\n");
            return false;
        }
    } else {
        // RSA premaster (0x009C) или ECDHE-RSA (0xC02F) — leaf обязан быть RSA.
        if (ctx->server_cert.pk_type != X509_PK_RSA) {
            serial_puts("[tls] cipher suite needs RSA leaf, but cert is non-RSA\n");
            return false;
        }
    }

    // Strict chain validation (этап 47.6).  Если время не доступно (RTC=0),
    // skip time check.
    const uint8_t* ders[TLS_MAX_CERTS_IN_CHAIN];
    size_t lens[TLS_MAX_CERTS_IN_CHAIN];
    for (size_t i = 0; i < ctx->cert_chain_count; i++) {
        ders[i] = ctx->cert_chain_raw[i];
        lens[i] = ctx->cert_chain_lens[i];
    }
    uint64_t now = rtc_now_unix();
    ctx->chain_result = x509_verify_chain(ders, lens, ctx->cert_chain_count,
                                          ctx->hostname, now);
    ctx->server_cert_trusted = (ctx->chain_result == X509_CHAIN_OK);

    if (!ctx->server_cert_trusted) {
        serial_puts("[tls] chain validation: ");
        serial_puts(x509_chain_result_str(ctx->chain_result));
        serial_puts("\n");
        // По умолчанию НЕ ломаем handshake — оставляем caller'у решать через
        // флаг require_trust в https_get.  Жёстко fail'им ТОЛЬКО при
        // SIG_INVALID и EXPIRED (доказанные ошибки безопасности).
        // PARSE-error на intermediate'е — best-effort: проверяем leaf через
        // x509_parse выше (он уже прошёл), для production usage caller
        // должен включить require_trust=true.
        if (ctx->chain_result == X509_CHAIN_ERR_SIG_INVALID ||
            ctx->chain_result == X509_CHAIN_ERR_EXPIRED) {
            return false;
        }
    } else {
        serial_puts("[tls] cert chain trusted (");
        serial_print_dec(ctx->cert_chain_count);
        serial_puts(" certs)\n");
    }
    return true;
}

// ---- Parse ServerKeyExchange (этапы 47.6 + 47.7) ---------------------------
// RFC 8422 §5.4 + RFC 5246 §7.4.3:
//
//   struct {
//       ECCurveType    curve_type;       // 1 byte: 0x03 = named_curve
//       NamedCurve     named_curve;      // 2 bytes: 0x0017 = secp256r1
//       ECPoint        public;           // opaque point<1..2^8-1>
//       SignatureAndHashAlgorithm  algorithm;  // 2 bytes (TLS 1.2)
//       opaque         signature<0..2^16-1>;
//   } ServerKeyExchange;
//
// Подпись покрывает: ClientHello.random || ServerHello.random || ECParameters
//                    (curve_type + named_curve) || public.
//
// Поддерживаем:
//   * curve_type = 0x03 (named_curve)
//   * named_curve = 0x0017 (secp256r1)
//   * sig_id = 0x01 (RSA) c hash sha256/sha384/sha512 — для ECDHE-RSA (0xC02F).
//   * sig_id = 0x03 (ECDSA) c hash sha256 — для ECDHE-ECDSA (0xC02B, этап 47.7).
static bool parse_server_key_exchange(tls_ctx_t* ctx, const uint8_t* p, size_t len) {
    if (len < 4) return false;
    const uint8_t* p_start = p;

    // ECCurveType
    if (p[0] != 0x03) {
        serial_puts("[tls] SKE: non-named-curve type\n");
        return false;
    }
    p += 1; len -= 1;

    // NamedCurve
    uint16_t curve = get_be16(p); p += 2; len -= 2;
    if (curve != TLS_GROUP_SECP256R1 && curve != TLS_GROUP_X25519 && curve != TLS_GROUP_SECP384R1) {
        serial_puts("[tls] SKE: server picked unsupported curve ");
        serial_print_dec(curve); serial_puts("\n");
        return false;
    }
    ctx->ecdhe_group      = curve;
    ctx->ecdhe_curve_p256 = (curve == TLS_GROUP_SECP256R1);

    // ECPoint: 1 byte length + bytes. P-256 — несжатая точка 65 байт,
    // x25519 — 32 байта координаты u (RFC 8422 §5.4).
    uint8_t pt_len = p[0]; p++; len--;
    size_t want = (curve == TLS_GROUP_SECP256R1) ? 65 : (curve == TLS_GROUP_SECP384R1) ? 97 : 32;
    if (pt_len != want || len < want) {
        serial_puts("[tls] SKE: bad EC point length\n");
        return false;
    }
    if (curve != TLS_GROUP_X25519 && p[0] != 0x04) {
        serial_puts("[tls] SKE: non-uncompressed EC point\n");
        return false;
    }
    s_memcpy(ctx->ecdhe_server_pub, p, want);
    p += want; len -= want;

    // ECParameters + ECPoint = "params" — это байты с p_start до текущего p.
    size_t params_len = (size_t)(p - p_start);

    // SignatureAndHashAlgorithm (TLS 1.2): hash | sig.
    if (len < 2 + 2) return false;
    uint8_t sha_id = p[0];   // 4=sha256, 5=sha384, 6=sha512
    uint8_t sig_id = p[1];   // 1=RSA, 3=ECDSA
    p += 2; len -= 2;

    // Этап 47.7: поддерживаем ECDSA (0x03) для ECDHE-ECDSA cipher suite.
    if (sig_id != 0x01 && sig_id != 0x03) {
        serial_puts("[tls] SKE: unsupported signature algorithm\n");
        return false;
    }
    // Согласованность: ECDSA-suite требует ECDSA подпись; RSA-suite — RSA.
    if (ctx->is_ecdsa && sig_id != 0x03) {
        serial_puts("[tls] SKE: ECDHE-ECDSA but server sent RSA sig\n");
        return false;
    }
    if (!ctx->is_ecdsa && sig_id != 0x01) {
        serial_puts("[tls] SKE: ECDHE-RSA but server sent ECDSA sig\n");
        return false;
    }

    crypto_hash_id_t hid;
    size_t           hl;
    if      (sha_id == 4) { hid = CRYPTO_HASH_SHA256; hl = 32; }
    else if (sha_id == 5) { hid = CRYPTO_HASH_SHA384; hl = 48; }
    else if (sha_id == 6) { hid = CRYPTO_HASH_SHA512; hl = 64; }
    else {
        serial_puts("[tls] SKE: unsupported hash in signature\n");
        return false;
    }
    // [48.2] ECDSA: хеш любой из трёх — длиннее порядка кривой он просто
    // обрезается слева (это делает сама проверка подписи).

    // Signature
    uint16_t sig_len = get_be16(p); p += 2; len -= 2;
    if (sig_len > len) return false;
    const uint8_t* sig_bytes = p;

    // Compute hash of: client_random(32) || server_random(32) || params.
    uint8_t hash[64];
    if (hid == CRYPTO_HASH_SHA256) {
        crypto_sha256_ctx_t h;
        crypto_sha256_init(&h);
        crypto_sha256_update(&h, ctx->client_random, 32);
        crypto_sha256_update(&h, ctx->server_random, 32);
        crypto_sha256_update(&h, p_start, params_len);
        crypto_sha256_final(&h, hash);
    } else if (hid == CRYPTO_HASH_SHA384) {
        crypto_sha512_ctx_t h;
        crypto_sha384_init(&h);
        crypto_sha512_update(&h, ctx->client_random, 32);
        crypto_sha512_update(&h, ctx->server_random, 32);
        crypto_sha512_update(&h, p_start, params_len);
        crypto_sha384_final(&h, hash);
    } else { // SHA-512
        crypto_sha512_ctx_t h;
        crypto_sha512_init(&h);
        crypto_sha512_update(&h, ctx->client_random, 32);
        crypto_sha512_update(&h, ctx->server_random, 32);
        crypto_sha512_update(&h, p_start, params_len);
        crypto_sha512_final(&h, hash);
    }

    bool ok;
    if (sig_id == 0x01) {
        // RSA verify под server cert RSA pub key.
        ok = crypto_rsa_pkcs1_verify(ctx->server_cert.rsa_modulus,
                                     ctx->server_cert.rsa_modulus_len,
                                     ctx->server_cert.rsa_exponent,
                                     hid, hash, hl,
                                     sig_bytes, sig_len);
        if (!ok) {
            serial_puts("[tls] SKE: RSA signature INVALID\n");
            return false;
        }
    } else {
        // sig_id == 0x03 — ECDSA-P256.  Подпись приходит в DER формате:
        //   SEQUENCE { INTEGER r, INTEGER s }.
        //
        // INTEGER в DER может иметь leading 0x00 (если high bit = 1, чтобы
        // отличить unsigned значение от отрицательного).  ECDSA-P256 r/s —
        // unsigned 256-bit числа, для которых это очень частый случай.
        // crypto_p256_ecdsa_verify умеет принимать любые r_len/s_len ≤ 32.
        const uint8_t* sp     = sig_bytes;
        const uint8_t* sp_end = sig_bytes + sig_len;

        // Внешний SEQUENCE tag = 0x30.
        if (sp_end - sp < 2 || sp[0] != 0x30) {
            serial_puts("[tls] SKE: ECDSA sig not SEQUENCE\n");
            return false;
        }
        sp++;
        // DER length: short form (1 байт, <128) или long form (0x8N || N байт).
        size_t seq_len_value;
        if ((sp[0] & 0x80) == 0) {
            seq_len_value = sp[0];
            sp++;
        } else {
            int nb = sp[0] & 0x7F;
            sp++;
            if (nb < 1 || nb > 2 || sp_end - sp < nb) {
                serial_puts("[tls] SKE: ECDSA seq bad length\n");
                return false;
            }
            seq_len_value = 0;
            for (int i = 0; i < nb; i++) seq_len_value = (seq_len_value << 8) | *sp++;
        }
        if ((size_t)(sp_end - sp) < seq_len_value) {
            serial_puts("[tls] SKE: ECDSA seq overruns\n");
            return false;
        }
        const uint8_t* body_end = sp + seq_len_value;

        // Helper-парс одного INTEGER из [sp..body_end].  Возвращает указатель
        // на тело числа (после трима leading 0x00) и его длину.  Двигает sp.
        // Не парсим > 1-байт length внутри INTEGER — для ECDSA-P256 длина ≤ 33
        // (256-bit + опциональный leading 0x00), помещается в 1 байт.
        const uint8_t* r_eff = NULL; size_t r_eff_len = 0;
        const uint8_t* s_eff = NULL; size_t s_eff_len = 0;
        for (int idx = 0; idx < 2; idx++) {
            if (body_end - sp < 2 || sp[0] != 0x02) {
                serial_puts("[tls] SKE: ECDSA INTEGER expected\n");
                return false;
            }
            size_t int_len = sp[1];
            sp += 2;
            if ((size_t)(body_end - sp) < int_len) {
                serial_puts("[tls] SKE: ECDSA INTEGER overruns\n");
                return false;
            }
            const uint8_t* val = sp;
            size_t          vl = int_len;
            // Убираем leading 0x00 (DER signed → unsigned semantics).
            while (vl > 1 && val[0] == 0x00) { val++; vl--; }
            if (vl > 48) {
                serial_puts("[tls] SKE: ECDSA INTEGER > 48 bytes\n");
                return false;
            }
            sp += int_len;
            if (idx == 0) { r_eff = val; r_eff_len = vl; }
            else          { s_eff = val; s_eff_len = vl; }
        }

        if (ctx->server_cert.pk_type == X509_PK_EC_P384)
            ok = crypto_p384_ecdsa_verify(ctx->server_cert.ec_p384_pk,
                                          hash, hl,
                                          r_eff, r_eff_len,
                                          s_eff, s_eff_len);
        else
            ok = (r_eff_len <= 32 && s_eff_len <= 32) &&
                 crypto_p256_ecdsa_verify(ctx->server_cert.ec_p256_pk,
                                          hash, hl,
                                          r_eff, r_eff_len,
                                          s_eff, s_eff_len);
        if (!ok) {
            serial_puts("[tls] SKE: ECDSA signature INVALID\n");
            return false;
        }
    }

    // Также проверим, что server_pub лежит на curve (у x25519 годится любая
    // строка из 32 байт; нулевой общий секрет отсекается при обмене).
    if (curve == TLS_GROUP_SECP384R1 && !crypto_p384_validate_pub(ctx->ecdhe_server_pub)) {
        serial_puts("[tls] SKE: server pub key NOT on curve P-384\n");
        return false;
    }
    if (ctx->ecdhe_curve_p256 && !crypto_p256_validate_pub(ctx->ecdhe_server_pub)) {
        serial_puts("[tls] SKE: server pub key NOT on curve\n");
        return false;
    }
    return true;
}

// ---- RSA encrypt premaster (PKCS#1 v1.5 type 2) ----------------------------
// EM = 0x00 || 0x02 || PS (random non-zero, >=8 bytes) || 0x00 || M
// Output длиной k (= размер modulus в bytes).
static bool rsa_encrypt_premaster(const uint8_t* modulus, size_t mod_len,
                                  uint32_t e,
                                  const uint8_t* pt, size_t pt_len,
                                  uint8_t* ct_out) {
    if (pt_len + 11 > mod_len) return false;
    uint8_t EM[CRYPTO_BN_BYTES];
    EM[0] = 0x00;
    EM[1] = 0x02;
    size_t ps_len = mod_len - 3 - pt_len;
    // Generate non-zero random bytes.
    for (size_t i = 0; i < ps_len; i++) {
        do {
            crypto_rand_bytes(&EM[2 + i], 1);
        } while (EM[2 + i] == 0);
    }
    EM[2 + ps_len] = 0x00;
    s_memcpy(EM + 3 + ps_len, pt, pt_len);

    // c = EM^e mod n.
    crypto_bn_t n_bn, m_bn, c_bn;
    crypto_bn_from_bytes_be(&n_bn, modulus, mod_len);
    crypto_bn_from_bytes_be(&m_bn, EM, mod_len);
    crypto_bn_modexp(&c_bn, &m_bn, e, &n_bn);
    crypto_bn_to_bytes_be(&c_bn, ct_out, mod_len);

    crypto_wipe(EM, sizeof(EM));
    return true;
}

// ---- KDF: master_secret + key_block ----------------------------------------
// PRF(secret, "master secret",  client_random || server_random)[0..48]
// PRF(secret, "key expansion",  server_random || client_random)[0..40]
//   { client_mac(0) || server_mac(0) || client_key(16) || server_key(16)
//     || client_iv(4)  || server_iv(4) }   — для AES-128-GCM, без MAC keys.
//
// Этап 47.6: premaster длина теперь зависит от cipher suite —
//   * RSA  : 48 байт (TLS-фиксировано: 0x03 0x03 || 46 random bytes)
//   * ECDH : 32 байта (X-coord shared secret).
static void derive_keys(tls_ctx_t* ctx) {
    // master_secret: классический — PRF(premaster, "master secret",
    // client_random || server_random); с extended_master_secret (RFC 7627) —
    // PRF(premaster, "extended master secret", хеш рукопожатия по
    // ClientKeyExchange включительно).
    uint8_t seed[64];
    if (ctx->ems) {
        uint8_t sh[48];
        size_t shl = hs_hash_now(ctx, sh);
        tls_prf(ctx, ctx->premaster, ctx->premaster_len,
                "extended master secret", sh, shl, ctx->master_secret, 48);
    } else {
        s_memcpy(seed, ctx->client_random, 32);
        s_memcpy(seed + 32, ctx->server_random, 32);
        tls_prf(ctx, ctx->premaster, ctx->premaster_len,
                "master secret", seed, 64, ctx->master_secret, 48);
    }

    // key_block: MAC-ключей у AEAD нет; ключи записи 16 или 32 байта,
    // IV — 4 байта (GCM, неявная часть) или 12 (ChaCha20-Poly1305).
    size_t kl = (ctx->aead == TLS_AEAD_AES128_GCM) ? 16 : 32;
    size_t il = (ctx->aead == TLS_AEAD_CHACHA20) ? 12 : 4;
    uint8_t seed2[64];
    s_memcpy(seed2, ctx->server_random, 32);
    s_memcpy(seed2 + 32, ctx->client_random, 32);
    uint8_t key_block[2 * 32 + 2 * 12];
    tls_prf(ctx, ctx->master_secret, 48, "key expansion", seed2, 64,
            key_block, 2 * kl + 2 * il);
    s_memcpy(ctx->client_write_key, key_block,               kl);
    s_memcpy(ctx->server_write_key, key_block + kl,          kl);
    s_memcpy(ctx->client_write_iv,  key_block + 2 * kl,      il);
    s_memcpy(ctx->server_write_iv,  key_block + 2 * kl + il, il);

    // Pre-init GCM contexts (ChaCha20 расписания ключа не требует).
    if (ctx->aead != TLS_AEAD_CHACHA20) {
        crypto_gcm_init(&ctx->client_gcm, ctx->client_write_key, kl);
        crypto_gcm_init(&ctx->server_gcm, ctx->server_write_key, kl);
    }

    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(seed2, sizeof(seed2));
    crypto_wipe(key_block, sizeof(key_block));
}

// ---- send ClientKeyExchange + ChangeCipherSpec + Finished ------------------
// Этап 47.6: ветвление по cipher suite —
//   RSA   : генерируем 48-байтный premaster, шифруем RSA-PKCS#1 под server pub.
//   ECDHE : генерируем ephemeral key pair P-256, отправляем pub server'у,
//           считаем shared = X(client_priv * server_pub_received).
static bool send_cke_ccs_finished_records(tls_ctx_t* ctx);

static bool send_cke_ccs_finished(tls_ctx_t* ctx) {
    g_cork = true; g_flight_len = 0;
    bool ok = send_cke_ccs_finished_records(ctx);
    g_cork = false;
    if (ok && g_flight_len) ok = tcp_send_all(g_flight, g_flight_len);
    g_flight_len = 0;
    return ok;
}

static bool send_cke_ccs_finished_records(tls_ctx_t* ctx) {
    if (!ctx->is_ecdhe) {
        // ---- RSA path ----
        // 1) Generate premaster: 0x03 0x03 || 46 random bytes.
        ctx->premaster[0] = 0x03; ctx->premaster[1] = 0x03;
        crypto_rand_bytes(ctx->premaster + 2, 46);
        ctx->premaster_len = 48;

        // 2) RSA-encrypt premaster под server pub key.
        size_t mod_len = ctx->server_cert.rsa_modulus_len;
        uint8_t encrypted[CRYPTO_BN_BYTES];
        if (!rsa_encrypt_premaster(ctx->server_cert.rsa_modulus, mod_len,
                                   ctx->server_cert.rsa_exponent,
                                   ctx->premaster, 48, encrypted)) {
            serial_puts("[tls] rsa encrypt premaster failed\n");
            return false;
        }

        // 3) Build & send ClientKeyExchange.  Тело CKE для RSA suite:
        //    uint16 enc_len; opaque enc[enc_len];
        uint8_t cke[4 + 2 + CRYPTO_BN_BYTES];
        cke[0] = HS_CLIENT_KEY_EXCHANGE;
        put_be24(cke + 1, (uint32_t)(2 + mod_len));
        put_be16(cke + 4, (uint16_t)mod_len);
        s_memcpy(cke + 6, encrypted, mod_len);
        hs_hash_update(ctx, cke, 4 + 2 + mod_len);
        if (!send_record_internal(ctx, TLS_RECORD_HANDSHAKE, cke, 4 + 2 + mod_len)) return false;
    } else if (ctx->ecdhe_group == TLS_GROUP_SECP384R1) {
        // ---- [48.2] ECDHE на P-384: как P-256, только числа по 48 байт ----
        crypto_rand_bytes(ctx->ecdhe_client_priv, 48);
        ctx->ecdhe_client_priv[0] &= 0x7F;            // заведомо меньше порядка группы
        ctx->ecdhe_client_priv[47] |= 0x01;           // и не ноль
        if (!crypto_p384_derive_pub(ctx->ecdhe_client_priv, ctx->ecdhe_client_pub) ||
            !crypto_p384_ecdh(ctx->ecdhe_client_priv, ctx->ecdhe_server_pub, ctx->premaster)) {
            serial_puts("[tls] ECDHE P-384 failed\n");
            return false;
        }
        ctx->premaster_len = 48;
        uint8_t cke[4 + 1 + 97];
        cke[0] = HS_CLIENT_KEY_EXCHANGE;
        put_be24(cke + 1, 1 + 97);
        cke[4] = 97;
        s_memcpy(cke + 5, ctx->ecdhe_client_pub, 97);
        hs_hash_update(ctx, cke, 4 + 1 + 97);
        if (!send_record_internal(ctx, TLS_RECORD_HANDSHAKE, cke, 4 + 1 + 97)) return false;
    } else if (ctx->ecdhe_group == TLS_GROUP_X25519) {
        // ---- [48.2] ECDHE на x25519 (RFC 7748, RFC 8422 §5.10) ----
        // Закрытый ключ — 32 случайных байта (функция сама «зажимает» биты),
        // открытый = X25519(priv, 9), общий секрет = X25519(priv, pub сервера).
        crypto_rand_bytes(ctx->ecdhe_client_priv, 32);
        crypto_x25519(ctx->ecdhe_client_pub, ctx->ecdhe_client_priv, crypto_x25519_basepoint);
        crypto_x25519(ctx->premaster, ctx->ecdhe_client_priv, ctx->ecdhe_server_pub);
        ctx->premaster_len = 32;
        // Нулевой результат — сервер прислал точку малого порядка: отказ.
        {
            uint8_t z = 0;
            for (int i = 0; i < 32; i++) z |= ctx->premaster[i];
            if (z == 0) { serial_puts("[tls] x25519: all-zero shared secret\n"); return false; }
        }
        uint8_t cke[4 + 1 + 32];
        cke[0] = HS_CLIENT_KEY_EXCHANGE;
        put_be24(cke + 1, 1 + 32);
        cke[4] = 32;
        s_memcpy(cke + 5, ctx->ecdhe_client_pub, 32);
        hs_hash_update(ctx, cke, 4 + 1 + 32);
        if (!send_record_internal(ctx, TLS_RECORD_HANDSHAKE, cke, 4 + 1 + 32)) return false;
    } else {
        // ---- ECDHE path (этап 47.6) ----
        // 1) Generate ephemeral keypair: priv = 32 random bytes mod n,
        //    pub = priv * G.
        // RFC 8422 + FIPS 186-4: scalar должен быть в [1, n-1].
        // crypto_p256_derive_pub принимает скаляр без normalization;
        // мы генерируем 32 random байта — статистически очень близко к
        // равномерному распределению по [0, 2^256), и шанс попасть в
        // 0 или ≥ n пренебрежимо мал.
        crypto_rand_bytes(ctx->ecdhe_client_priv, 32);
        // Гарантируем не-ноль: если так получилось, замешаем 1.
        {
            uint8_t z = 0;
            for (int i = 0; i < 32; i++) z |= ctx->ecdhe_client_priv[i];
            if (z == 0) ctx->ecdhe_client_priv[31] = 1;
        }
        if (!crypto_p256_derive_pub(ctx->ecdhe_client_priv,
                                    ctx->ecdhe_client_pub)) {
            serial_puts("[tls] ECDHE: derive_pub failed\n");
            return false;
        }

        // 2) Compute shared = X(client_priv * server_pub) — 32 bytes.
        if (!crypto_p256_ecdh(ctx->ecdhe_client_priv,
                              ctx->ecdhe_server_pub,
                              ctx->premaster)) {
            serial_puts("[tls] ECDHE: ecdh failed\n");
            return false;
        }
        ctx->premaster_len = 32;

        // 3) Send ClientKeyExchange = uint8 length || ECPoint(65 bytes).
        uint8_t cke[4 + 1 + 65];
        cke[0] = HS_CLIENT_KEY_EXCHANGE;
        put_be24(cke + 1, (uint32_t)(1 + 65));
        cke[4] = 65;
        s_memcpy(cke + 5, ctx->ecdhe_client_pub, 65);
        hs_hash_update(ctx, cke, 4 + 1 + 65);
        if (!send_record_internal(ctx, TLS_RECORD_HANDSHAKE, cke, 4 + 1 + 65)) return false;
    }

    // 4) Derive keys (premaster → master_secret → key_block).
    derive_keys(ctx);

    // 5) Send ChangeCipherSpec record (1 byte: 0x01).  CCS НЕ участвует в
    //    handshake hash и НЕ под GCM-шифрованием (но сразу после неё мы
    //    flip'ем write_encrypted).
    uint8_t ccs = 0x01;
    if (!send_record_internal(ctx, TLS_RECORD_CHANGE_CIPHER, &ccs, 1)) return false;
    ctx->write_encrypted = true;
    ctx->client_seq = 0;

    // 6) Build Finished: PRF(master_secret, "client finished", SHA256(handshake_msgs))[0..12].
    uint8_t hs_hash[48];
    size_t  hs_hash_len = hs_hash_now(ctx, hs_hash);       // snapshot
    uint8_t verify_data[12];
    tls_prf(ctx, ctx->master_secret, 48,
            "client finished", hs_hash, hs_hash_len,
            verify_data, 12);
    uint8_t fin[4 + 12];
    fin[0] = HS_FINISHED;
    put_be24(fin + 1, 12);
    s_memcpy(fin + 4, verify_data, 12);
    hs_hash_update(ctx, fin, 4 + 12);
    if (!send_record_internal(ctx, TLS_RECORD_HANDSHAKE, fin, 4 + 12)) return false;
    return true;
}

// ---- Read & verify server's ChangeCipherSpec + Finished --------------------
static bool wait_server_ccs_finished(tls_ctx_t* ctx) {
    // CCS first (cleartext).
    uint8_t type;
    uint8_t* p;
    size_t plen;
    if (!recv_record_internal(ctx, &type, &p, &plen)) return false;
    if (type == TLS_RECORD_ALERT && plen >= 2) {
        // Сервер отверг наши ClientKeyExchange/Finished.
        serial_puts("[tls] server sent ALERT after our Finished: level=");
        serial_print_dec(p[0]); serial_puts(" desc="); serial_print_dec(p[1]); serial_puts("\n");
        return false;
    }
    if (type != TLS_RECORD_CHANGE_CIPHER || plen != 1 || p[0] != 0x01) {
        serial_puts("[tls] expected server CCS\n");
        return false;
    }
    ctx->read_encrypted = true;
    ctx->server_seq = 0;

    // Now ServerFinished (encrypted).  Read decrypted handshake message.
    if (!recv_record_internal(ctx, &type, &p, &plen)) return false;
    if (type != TLS_RECORD_HANDSHAKE) {
        serial_puts("[tls] expected server Finished\n");
        return false;
    }
    if (plen < 4 || p[0] != HS_FINISHED) {
        serial_puts("[tls] bad finished msg type\n");
        return false;
    }
    uint32_t flen = get_be24(p + 1);
    if (flen != 12 || plen != 4 + 12) {
        serial_puts("[tls] bad finished length\n");
        return false;
    }
    // Compute expected verify_data.
    uint8_t hs_hash[48];
    size_t  hs_hash_len = hs_hash_now(ctx, hs_hash);
    uint8_t expected[12];
    tls_prf(ctx, ctx->master_secret, 48,
            "server finished", hs_hash, hs_hash_len,
            expected, 12);
    if (!crypto_eq_const(expected, p + 4, 12)) {
        serial_puts("[tls] server Finished mismatch\n");
        return false;
    }
    hs_hash_update(ctx, p, plen);   // важно для post-handshake (renego), но мы не используем
    return true;
}

// ---- Главный handshake driver ----------------------------------------------
void tls_client_init(tls_ctx_t* ctx, int fd, const char* hostname) {
    for (size_t i = 0; i < sizeof(*ctx); i++) ((uint8_t*)ctx)[i] = 0;
    ctx->fd = fd;
    size_t hn = s_strlen(hostname);
    if (hn >= TLS_MAX_HOSTNAME) hn = TLS_MAX_HOSTNAME - 1;
    s_memcpy(ctx->hostname, hostname, hn);
    ctx->hostname[hn] = 0;
    ctx->state = TLS_STATE_INIT;
    g_rx_any = false; g_silent = false; g_rx_wait_ms = 10000;
    crypto_sha256_init(&ctx->handshake_hash);
    crypto_sha384_init(&ctx->handshake_hash384);
}

static const char* suite_name(uint16_t cs) {
    switch (cs) {
    case TLS_CS_RSA_AES128_GCM_SHA256:         return "TLS_RSA_WITH_AES_128_GCM_SHA256";
    case TLS_CS_ECDHE_RSA_AES128_GCM_SHA256:   return "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256";
    case TLS_CS_ECDHE_ECDSA_AES128_GCM_SHA256: return "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256";
    case TLS_CS_RSA_AES256_GCM_SHA384:         return "TLS_RSA_WITH_AES_256_GCM_SHA384";
    case TLS_CS_ECDHE_RSA_AES256_GCM_SHA384:   return "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384";
    case TLS_CS_ECDHE_ECDSA_AES256_GCM_SHA384: return "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384";
    case TLS_CS_ECDHE_RSA_CHACHA20_POLY1305:   return "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256";
    case TLS_CS_ECDHE_ECDSA_CHACHA20_POLY1305: return "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256";
    }
    return "?";
}

bool tls_client_handshake(tls_ctx_t* ctx) {
    if (ctx->state != TLS_STATE_INIT) return false;

    // 1. Send ClientHello.
    if (!send_client_hello(ctx)) { ctx->state = TLS_STATE_ERROR; return false; }
    ctx->state = TLS_STATE_WAIT_SERVER_HELLO;

    // 2. Receive ServerHello + Certificate + (optional ServerKeyExchange) + ServerHelloDone.
    //    Может прийти в одном record'е (concat'нутые handshake messages)
    //    или в нескольких.  Парсим handshake messages потоком из records.
    // 64 КБ: сообщение Certificate с длинной цепочкой (4-5 сертификатов с
    // сотнями имён в SAN) бывает больше 16 КБ и приходит в нескольких записях.
    static uint8_t rx_acc[65536];     // accumulator из handshake-payload'ов
    size_t   rx_acc_len = 0;
    bool     got_hello = false, got_cert = false, got_done = false;
    bool     got_ske   = false;       // ServerKeyExchange (только для ECDHE, эт. 47.6)
    while (!got_done) {
        uint8_t type;
        uint8_t* p;
        size_t plen;
        if (!recv_record_internal(ctx, &type, &p, &plen)) { ctx->state = TLS_STATE_ERROR; return false; }
        if (type == TLS_RECORD_ALERT) {
            serial_puts("[tls] server sent ALERT\n");
            if (plen >= 2) {
                serial_puts("       level=");
                serial_print_dec(p[0]);
                serial_puts(" desc=");
                serial_print_dec(p[1]);
                serial_puts("\n");
            }
            ctx->state = TLS_STATE_ERROR;
            return false;
        }
        if (type != TLS_RECORD_HANDSHAKE) {
            serial_puts("[tls] unexpected record type during handshake\n");
            ctx->state = TLS_STATE_ERROR;
            return false;
        }
        // Append to accumulator.
        if (rx_acc_len + plen > sizeof(rx_acc)) {
            serial_puts("[tls] handshake accumulator overflow\n");
            ctx->state = TLS_STATE_ERROR;
            return false;
        }
        s_memcpy(rx_acc + rx_acc_len, p, plen);
        rx_acc_len += plen;

        // Parse all complete HS messages.
        size_t off = 0;
        while (off + 4 <= rx_acc_len) {
            uint8_t hs_type = rx_acc[off];
            uint32_t hs_len = get_be24(rx_acc + off + 1);
            if (off + 4 + hs_len > rx_acc_len) break;          // не хватает данных
            const uint8_t* body = rx_acc + off + 4;
            // Update handshake hash with FULL HS message header+body.
            hs_hash_update(ctx, rx_acc + off, 4 + hs_len);
            switch (hs_type) {
                case HS_SERVER_HELLO:
                    if (!parse_server_hello(ctx, body, hs_len)) { ctx->state = TLS_STATE_ERROR; return false; }
                    got_hello = true;
                    break;
                case HS_CERTIFICATE:
                    if (!parse_certificate(ctx, body, hs_len)) { ctx->state = TLS_STATE_ERROR; return false; }
                    got_cert = true;
                    break;
                case HS_SERVER_KEY_EXCHANGE:
                    // Этап 47.6: для ECDHE парсим обязательно.  Для RSA — игнорируем.
                    if (ctx->is_ecdhe) {
                        if (!parse_server_key_exchange(ctx, body, hs_len)) {
                            ctx->state = TLS_STATE_ERROR; return false;
                        }
                        got_ske = true;
                    }
                    break;
                case HS_CERT_REQUEST:
                    // Игнорируем — не поддерживаем client cert auth.
                    break;
                case HS_SERVER_HELLO_DONE:
                    got_done = true;
                    break;
                default:
                    serial_puts("[tls] unexpected HS type ");
                    serial_print_dec(hs_type);
                    serial_puts("\n");
                    ctx->state = TLS_STATE_ERROR;
                    return false;
            }
            off += 4 + hs_len;
        }
        // Shift unfinished tail down.
        if (off > 0) {
            for (size_t i = 0; i < rx_acc_len - off; i++) rx_acc[i] = rx_acc[off + i];
            rx_acc_len -= off;
        }
    }
    if (!got_hello || !got_cert) { ctx->state = TLS_STATE_ERROR; return false; }
    if (ctx->is_ecdhe && !got_ske) {
        serial_puts("[tls] ECDHE: missing ServerKeyExchange\n");
        ctx->state = TLS_STATE_ERROR;
        return false;
    }

    // 3. Send ClientKeyExchange + ChangeCipherSpec + Finished.
    if (!send_cke_ccs_finished(ctx)) { ctx->state = TLS_STATE_ERROR; return false; }

    // 4. Wait server CCS + Finished.
    if (!wait_server_ccs_finished(ctx)) { ctx->state = TLS_STATE_ERROR; return false; }

    ctx->state = TLS_STATE_ESTABLISHED;
    g_rx_wait_ms = 30000;
    serial_puts("[tls] handshake complete: ");
    serial_puts(suite_name(ctx->cipher_suite));
    if (ctx->is_ecdhe) serial_puts(ctx->ecdhe_group == TLS_GROUP_X25519 ? " x25519" :
                                   ctx->ecdhe_group == TLS_GROUP_SECP384R1 ? " p384" : " p256");
    if (ctx->ems) serial_puts(" ems");
    if (ctx->server_cert_trusted) serial_puts(" (cert: trusted");
    else { serial_puts(" (cert: "); serial_puts(x509_chain_result_str(ctx->chain_result)); }
    if (ctx->alpn_selected[0]) {
        serial_puts(", alpn=");
        serial_puts(ctx->alpn_selected);
    }
    serial_puts(")\n");
    return true;
}

// ---- Application data send/recv --------------------------------------------
int tls_send(tls_ctx_t* ctx, const void* data, size_t len) {
    if (ctx->state != TLS_STATE_ESTABLISHED) return -1;
    // Может отправить chunk'ами по TLS_MAX_RECORD - overhead.
    size_t max_chunk = 16384;          // RFC 5246 §6.2.1
    size_t total = 0;
    const uint8_t* p = (const uint8_t*)data;
    while (len > 0) {
        size_t n = len > max_chunk ? max_chunk : len;
        if (!send_record_internal(ctx, TLS_RECORD_APPLICATION, p, n)) return -1;
        p   += n;
        len -= n;
        total += n;
    }
    return (int)total;
}

int tls_recv(tls_ctx_t* ctx, void* buf, size_t cap) {
    if (ctx->state == TLS_STATE_CLOSED) return 0;
    if (ctx->state != TLS_STATE_ESTABLISHED) return -1;
    uint8_t* dest = (uint8_t*)buf;

    // Если есть остатки от прошлого record'а — отдадим сначала.
    if (ctx->rx_off < ctx->rx_len) {
        size_t avail = ctx->rx_len - ctx->rx_off;
        size_t take  = avail < cap ? avail : cap;
        s_memcpy(dest, ctx->rx_buf + ctx->rx_off, take);
        ctx->rx_off += take;
        return (int)take;
    }
    ctx->rx_len = ctx->rx_off = 0;

    // Иначе — читаем новый record.
    uint8_t  type;
    uint8_t* p;
    size_t   plen;
    // [48.2] Записи рукопожатия после его конца (HelloRequest — приглашение к
    // пересогласованию, которого мы не делаем) и пустые записи данных —
    // не конец потока: пропускаем и читаем дальше.
    for (int skipped = 0; ; skipped++) {
        if (!recv_record_internal(ctx, &type, &p, &plen)) return -1;
        if (type == TLS_RECORD_HANDSHAKE && skipped < 8) continue;
        if (type == TLS_RECORD_APPLICATION && plen == 0 && skipped < 8) continue;
        break;
    }
    if (type == TLS_RECORD_ALERT) {
        if (plen >= 2 && p[1] == 0) {
            // close_notify
            ctx->state = TLS_STATE_CLOSED;
            return 0;
        }
        serial_puts("[tls] received non-close ALERT during recv\n");
        return -1;
    }
    if (type != TLS_RECORD_APPLICATION) {
        serial_puts("[tls] non-app record after handshake\n");
        return -1;
    }
    // p указывает в ctx->rx_buf+8 (после nonce).  Скопируем в buffer.
    if (plen > cap) {
        // Не помещается: даём `cap` сейчас, остаток сохраняем как rx-tail.
        s_memcpy(dest, p, cap);
        // Перенесём остаток в начало rx_buf для следующих вызовов.
        size_t tail = plen - cap;
        for (size_t i = 0; i < tail; i++) ctx->rx_buf[i] = p[cap + i];
        ctx->rx_len = tail;
        ctx->rx_off = 0;
        return (int)cap;
    }
    s_memcpy(dest, p, plen);
    return (int)plen;
}

void tls_close(tls_ctx_t* ctx) {
    if (ctx->state == TLS_STATE_ESTABLISHED) {
        // Send close_notify alert: level=warning(1), desc=close_notify(0).
        uint8_t alert[2] = { 1, 0 };
        send_record_internal(ctx, TLS_RECORD_ALERT, alert, 2);
    }
    ctx->state = TLS_STATE_CLOSED;
    crypto_wipe(ctx->premaster,        sizeof(ctx->premaster));
    crypto_wipe(ctx->master_secret,    sizeof(ctx->master_secret));
    crypto_wipe(ctx->client_write_key, sizeof(ctx->client_write_key));
    crypto_wipe(ctx->server_write_key, sizeof(ctx->server_write_key));
    crypto_wipe(ctx->ecdhe_client_priv, sizeof(ctx->ecdhe_client_priv));
}

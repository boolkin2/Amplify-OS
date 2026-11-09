// ============================================================================
// tls.h — TLS 1.2 client для AMPLIFY-OS (этапы 47.4 + 47.6).
// ----------------------------------------------------------------------------
// Минимально-достаточная реализация для HTTPS GET к современным серверам:
//   * TLS версия 1.2 (0x0303).  TLS 1.3 — отдельный этап (47.7), требует полного
//     переписывания record layer'а.
//   * Cipher suites (этап 47.6 расширил с 1 до 2):
//     - 0x009C TLS_RSA_WITH_AES_128_GCM_SHA256       (классический legacy)
//     - 0xC02F TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (modern, github/google).
//
//     Оба используют:
//       - AES-128-GCM (AEAD)
//       - SHA-256 для PRF + Finished
//
//     Key exchange:
//       - RSA  : клиент шифрует premaster RSA-PKCS#1 v1.5 под server pub key.
//       - ECDHE: сервер генерирует ephemeral keypair на secp256r1, шлёт public
//                в ServerKeyExchange + RSA-подписывает (client_random ||
//                server_random || params).  Клиент проверяет подпись через
//                cert.RSA, генерирует свой ephemeral keypair, считает
//                shared = X(client_priv * server_pub).  Premaster = shared.
//   * Server certificate chain verify: ПОЛНАЯ strict chain validation
//     через x509_verify_chain (этап 47.6) — hostname в SAN, time-check
//     против RTC, BasicConstraints + KeyUsage + ExtKeyUsage, путь к root
//     trust store (crypto_ca.c).
//   * SNI extension (Server Name Indication) — почти везде требуется.
//   * supported_groups + ec_point_formats extensions — для ECDHE.
//   * signature_algorithms — RSA-SHA256/384/512, ECDSA-SHA256/384.
//   * ALPN extension (этап 47.6) — broadcast "h2" + "http/1.1", парсим
//     выбор сервера из EncryptedExtensions.
//   * Никаких client certificates, session resumption (etap 47.7+).
//
// Подэтап 48.2 (настоящие сайты): часть серверов не держит AES-128-GCM вовсе
// (lite.cnn.com отвечал alert 40), поэтому добавлены
//   - ECDHE + AES-256-GCM-SHA384 (0xC02C, 0xC030; PRF и хеш рукопожатия — SHA-384),
//   - ECDHE + ChaCha20-Poly1305  (0xCCA9, 0xCCA8; RFC 7905),
//   - RSA + AES-256-GCM-SHA384   (0x009D),
//   - обмен ключами на x25519 (многие серверы предпочитают его P-256),
//   - extended_master_secret (RFC 7627) и renegotiation_info (RFC 5746),
//   - ECDSA-подписи SKE с SHA-384/512, листовые сертификаты на P-384.
//
// Поведение:
//   tls_client_init(ctx, fd, hostname) — set up state, ctx должен жить до close.
//   tls_client_handshake(ctx)          — синхронный, блокирует пока handshake
//                                         не завершится (или fail).
//   tls_send(ctx, buf, len)            — encrypted send, возвращает байты.
//   tls_recv(ctx, buf, cap)            — encrypted recv, возвращает байты.
//   tls_close(ctx)                     — отсылает close_notify + закрывает TCP.
//
// Один TLS-context на один TCP-socket.  Внутри используется глобальный tcp.c
// (т.е. одновременно может работать только один TLS на ядро — тот же лимит,
// что был у HTTP в этапе 41).  Future: переписать когда tcp.c станет multi-socket.
// ============================================================================
#ifndef AOS_X86_64_TLS_H
#define AOS_X86_64_TLS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "crypto.h"
#include "crypto_x509.h"

#define TLS_MAX_RECORD       (16384 + 256)   // RFC 5246 §6.2.1: 2^14 + overhead
#define TLS_MAX_HOSTNAME     256
#define TLS_MAX_CERT_RAW     16384           // достаточно для leaf + 1-2 intermediates
#define TLS_MAX_CERTS_IN_CHAIN  6            // chain depth limit (эт. 47.6)
#define TLS_MAX_ALPN_PROTO   16              // max length выбранного ALPN protocol

typedef enum {
    TLS_STATE_INIT = 0,
    TLS_STATE_WAIT_SERVER_HELLO,
    TLS_STATE_WAIT_CERT,
    TLS_STATE_WAIT_HELLO_DONE,
    TLS_STATE_WAIT_SERVER_CCS,
    TLS_STATE_WAIT_SERVER_FINISHED,
    TLS_STATE_ESTABLISHED,
    TLS_STATE_CLOSED,
    TLS_STATE_ERROR,
} tls_state_t;

// Поддерживаемые cipher suites (этап 47.7 — 3 штуки):
//   0x009C — RSA premaster + AES-128-GCM (legacy, любой сервер).
//   0xC02F — ECDHE-RSA, AES-128-GCM (большинство современных сайтов с RSA-cert).
//   0xC02B — ECDHE-ECDSA, AES-128-GCM (Cloudflare/Google/всё что выдает CA с
//            ECDSA leaf-сертификатом — это >50% современного веба).
#define TLS_CS_RSA_AES128_GCM_SHA256          0x009C
#define TLS_CS_ECDHE_RSA_AES128_GCM_SHA256    0xC02F
#define TLS_CS_ECDHE_ECDSA_AES128_GCM_SHA256  0xC02B
// [48.2] наборы для серверов без AES-128-GCM:
#define TLS_CS_RSA_AES256_GCM_SHA384          0x009D
#define TLS_CS_ECDHE_RSA_AES256_GCM_SHA384    0xC030
#define TLS_CS_ECDHE_ECDSA_AES256_GCM_SHA384  0xC02C
#define TLS_CS_ECDHE_RSA_CHACHA20_POLY1305    0xCCA8
#define TLS_CS_ECDHE_ECDSA_CHACHA20_POLY1305  0xCCA9

// Шифр записей выбранного набора.
typedef enum {
    TLS_AEAD_AES128_GCM = 0,
    TLS_AEAD_AES256_GCM,
    TLS_AEAD_CHACHA20,
} tls_aead_t;

// Группы обмена ключами (supported_groups).
#define TLS_GROUP_SECP256R1  0x0017
#define TLS_GROUP_SECP384R1  0x0018
#define TLS_GROUP_X25519     0x001D

typedef struct {
    int            fd;                              // TCP socket (через sock.c)
    char           hostname[TLS_MAX_HOSTNAME];

    tls_state_t    state;

    // Handshake randoms (32 байта каждый)
    uint8_t        client_random[32];
    uint8_t        server_random[32];

    // Cipher suite (выбирается сервером из нашего ClientHello).
    uint16_t       cipher_suite;
    bool           is_ecdhe;                 // true для ECDHE-* suites (47.6+)
    bool           is_ecdsa;                 // true для *_ECDSA_* suite (47.7) —
                                             // leaf-сертификат ECDSA-P256, SKE-signature
                                             // тоже ECDSA-P256 (а не RSA).

    // Premaster (48 байт для RSA, 32 для P-256 ECDH; буфер 48 хватает на оба)
    // и master_secret (48 байт)
    uint8_t        premaster[48];
    size_t         premaster_len;
    uint8_t        master_secret[48];

    // [48.2] что выбрал сервер: шифр записей, хеш PRF/Finished, EMS.
    tls_aead_t     aead;
    bool           prf_sha384;               // наборы *_SHA384
    bool           ems;                      // сервер подтвердил extended_master_secret

    // Key block: ключи 16 (AES-128) или 32 байта (AES-256, ChaCha20); IV —
    // 4 байта неявной части у GCM или все 12 у ChaCha20-Poly1305.
    uint8_t        client_write_key[32];
    uint8_t        server_write_key[32];
    uint8_t        client_write_iv[12];
    uint8_t        server_write_iv[12];

    // Пре-init AES-GCM контексты, чтобы не делать key schedule на каждой записи.
    crypto_gcm_ctx_t  client_gcm;
    crypto_gcm_ctx_t  server_gcm;

    // Sequence numbers — 64-bit BE, входят в AAD для GCM (RFC 5246 §6.2.3.3).
    uint64_t       client_seq;
    uint64_t       server_seq;

    bool           write_encrypted;     // после своего CCS
    bool           read_encrypted;      // после server CCS

    // SHA-256 hash всех handshake messages (для Finished verify_data).
    crypto_sha256_ctx_t  handshake_hash;
    // [48.2] тот же поток сообщений на SHA-384: какой хеш понадобится, станет
    // известно только из ServerHello, а ClientHello к тому времени уже ушёл.
    crypto_sha512_ctx_t  handshake_hash384;

    // Server certificate chain (этап 47.6 — теперь храним до 6 cert'ов
    // в chain для полноценной валидации).  Первый — leaf.
    size_t         cert_chain_count;
    uint8_t        cert_chain_raw[TLS_MAX_CERTS_IN_CHAIN][TLS_MAX_CERT_RAW];
    size_t         cert_chain_lens[TLS_MAX_CERTS_IN_CHAIN];
    x509_cert_t    server_cert;              // leaf parsed
    bool           server_cert_trusted;      // прошёл strict chain validation
    x509_chain_result_t  chain_result;       // подробный код результата

    // ECDHE-state (этап 47.6).  Используется только для ECDHE cipher suites.
    bool           ecdhe_curve_p256;         // выбранная кривая = secp256r1
    uint16_t       ecdhe_group;              // [48.2] TLS_GROUP_SECP256R1 / TLS_GROUP_X25519
    uint8_t        ecdhe_client_priv[48];    // наш ephemeral private (32 байта; P-384 — 48)
    uint8_t        ecdhe_client_pub[97];     // P-256: 0x04||X||Y = 65; P-384: 97; x25519: 32 байта
    uint8_t        ecdhe_server_pub[97];     // от сервера (тот же формат)

    // ALPN (этап 47.6) — выбранный протокол ("h2", "http/1.1") или пусто.
    char           alpn_selected[TLS_MAX_ALPN_PROTO + 1];

    // Внутренние буферы (положены в struct, чтобы не аллоцировать на хип):
    uint8_t        rx_buf[TLS_MAX_RECORD];
    size_t         rx_len;                  // данные между tls_recv'ами (после decrypt)
    size_t         rx_off;
} tls_ctx_t;

// [48.2] true, если последнее рукопожатие сорвалось потому, что сервер не
// прислал НИ БАЙТА в ответ на ClientHello (а не из-за шифров/сертификата).
bool tls_handshake_silent(void);

// Инициализация: fd должен быть подключённым TCP socket'ом.  hostname — для SNI.
void tls_client_init(tls_ctx_t* ctx, int fd, const char* hostname);

// Выполнить полный handshake.  Возвращает true в случае успеха.
// При false ctx->state == TLS_STATE_ERROR (или CLOSED).
bool tls_client_handshake(tls_ctx_t* ctx);

// Отправить data через TLS.  Возвращает число отправленных байт или -1.
int  tls_send(tls_ctx_t* ctx, const void* data, size_t len);

// Получить данные.  Блокирует до получения хотя бы одного record'а.
// Возвращает количество decrypted plaintext байт.  0 = clean close_notify.
// -1 = ошибка.
int  tls_recv(tls_ctx_t* ctx, void* buf, size_t cap);

// Послать close_notify и перейти в CLOSED.  Не закрывает TCP — это caller's job.
void tls_close(tls_ctx_t* ctx);

#endif

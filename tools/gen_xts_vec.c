// ============================================================================
// tools/gen_xts_vec.c — host-утилита: генерирует reference-векторы AES-XTS
// через OpenSSL EVP (авторитетная реализация IEEE 1619).  Используется ОДИН
// РАЗ при разработке этапа 48 для получения KAT-констант в
// crypto_self_test.c.  В сборку ядра не входит.
//
// Сборка:  gcc -O2 -o /tmp/gen_xts_vec tools/gen_xts_vec.c -lcrypto
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>

static void dump(const char* label, const unsigned char* p, int n) {
    printf("%s (%d bytes):\n", label, n);
    for (int i = 0; i < n; i++) {
        printf("0x%02x,", p[i]);
        if ((i & 15) == 15) printf("\n");
    }
    if (n & 15) printf("\n");
}

static int xts(const EVP_CIPHER* c, const unsigned char* key,
               const unsigned char* iv, const unsigned char* in,
               unsigned char* out, int len) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int outl = 0, tmpl = 0;
    if (!EVP_EncryptInit_ex(ctx, c, NULL, key, iv)) return -1;
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    if (!EVP_EncryptUpdate(ctx, out, &outl, in, len)) return -1;
    if (!EVP_EncryptFinal_ex(ctx, out + outl, &tmpl)) return -1;
    EVP_CIPHER_CTX_free(ctx);
    return outl + tmpl;
}

int main(void) {
    // --- Вектор A: AES-256-XTS, 32 байта (2 блока), sector # = 0x33 ---
    unsigned char key64[64];   // key1(32) || key2(32)
    for (int i = 0; i < 64; i++) key64[i] = (unsigned char)i;
    unsigned char iv[16] = {0};  // tweak = LE128(sector)
    iv[0] = 0x33;
    unsigned char pt32[32];
    for (int i = 0; i < 32; i++) pt32[i] = (unsigned char)i;
    unsigned char ct32[32];
    if (xts(EVP_aes_256_xts(), key64, iv, pt32, ct32, 32) != 32) {
        printf("aes-256-xts failed\n"); return 1;
    }
    printf("=== AES-256-XTS: key1=00..1f key2=20..3f sector=0x33 pt=00..1f ===\n");
    dump("ct32", ct32, 32);

    // --- Вектор B: AES-256-XTS, полный сектор 512 байт, sector # = 1 ---
    unsigned char iv1[16] = {0};
    iv1[0] = 0x01;
    unsigned char pt512[512], ct512[512];
    for (int i = 0; i < 512; i++) pt512[i] = (unsigned char)(i & 0xFF);
    if (xts(EVP_aes_256_xts(), key64, iv1, pt512, ct512, 512) != 512) {
        printf("aes-256-xts 512 failed\n"); return 1;
    }
    printf("=== AES-256-XTS: 512-byte sector #1 (first16 + last16) ===\n");
    dump("ct512 first16", ct512, 16);
    dump("ct512 last16", ct512 + 496, 16);

    // --- Вектор C: AES-128-XTS, 32 байта, sector # = 2 ---
    unsigned char key32[32];
    for (int i = 0; i < 32; i++) key32[i] = (unsigned char)(0xA0 + i);
    unsigned char iv2[16] = {0};
    iv2[0] = 0x02;
    unsigned char ct32b[32];
    if (xts(EVP_aes_128_xts(), key32, iv2, pt32, ct32b, 32) != 32) {
        printf("aes-128-xts failed\n"); return 1;
    }
    printf("=== AES-128-XTS: key1=a0..af key2=b0..bf sector=2 pt=00..1f ===\n");
    dump("ct32b", ct32b, 32);
    return 0;
}

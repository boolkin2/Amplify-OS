// ============================================================================
// tools/luks_interop.c — независимая проверка LUKS1-томов AMPLIFY-OS (этап 48).
// ----------------------------------------------------------------------------
// Открывает заголовок НАСТОЯЩЕЙ библиотекой cryptsetup (libcryptsetup.so.12,
// тот же код, что у `cryptsetup open`), достаёт мастер-ключ по паролю и
// расшифровывает первый сектор payload'а через OpenSSL (EVP aes-256-xts).
// Ни одной строчки криптографии ядра AMPLIFY-OS здесь нет — поэтому успех
// означает совместимость формата, а не согласие двух копий одной ошибки.
//
// Заголовочного файла libcryptsetup-dev в системе может не быть: нужные
// функции — стабильный публичный ABI, объявляем их вручную.
//
// Сборка:  gcc -O2 -o build/luks_interop tools/luks_interop.c
//              -l:libcryptsetup.so.12 -lcrypto        (одной строкой)
// Запуск:  build/luks_interop <образ-раздела> <пароль>
//          (образ должен начинаться с заголовка LUKS и включать хотя бы
//          первый сектор payload'а)
// Выход:   0 — ключ получен и payload[0] расшифровывается в FAT32 BPB.
// ============================================================================
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <openssl/evp.h>

struct crypt_device;
int  crypt_init(struct crypt_device** cd, const char* device);
int  crypt_load(struct crypt_device* cd, const char* requested_type, void* params);
int  crypt_volume_key_get(struct crypt_device* cd, int keyslot, char* volume_key,
                          size_t* volume_key_size, const char* passphrase,
                          size_t passphrase_size);
const char* crypt_get_cipher(struct crypt_device* cd);
const char* crypt_get_cipher_mode(struct crypt_device* cd);
const char* crypt_get_uuid(struct crypt_device* cd);
uint64_t    crypt_get_data_offset(struct crypt_device* cd);
int         crypt_get_volume_key_size(struct crypt_device* cd);
void        crypt_free(struct crypt_device* cd);

#define CRYPT_LUKS1   "LUKS1"
#define CRYPT_ANY_SLOT -1

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <luks-partition-image> <passphrase>\n", argv[0]);
        return 2;
    }
    struct crypt_device* cd = NULL;
    int r = crypt_init(&cd, argv[1]);
    if (r < 0) { fprintf(stderr, "crypt_init failed: %d\n", r); return 1; }
    r = crypt_load(cd, CRYPT_LUKS1, NULL);
    if (r < 0) { fprintf(stderr, "crypt_load(LUKS1) failed: %d\n", r); crypt_free(cd); return 1; }

    uint64_t data_off = crypt_get_data_offset(cd);
    printf("libcryptsetup: LUKS1 %s-%s, key %d bytes, payload @ %llu sectors, uuid %s\n",
           crypt_get_cipher(cd), crypt_get_cipher_mode(cd), crypt_get_volume_key_size(cd),
           (unsigned long long)data_off, crypt_get_uuid(cd));

    char   vk[64];
    size_t vk_size = sizeof(vk);
    r = crypt_volume_key_get(cd, CRYPT_ANY_SLOT, vk, &vk_size, argv[2], strlen(argv[2]));
    if (r < 0) {
        fprintf(stderr, "crypt_volume_key_get failed: %d (wrong passphrase?)\n", r);
        crypt_free(cd);
        return 1;
    }
    printf("libcryptsetup: passphrase accepted by key slot %d, volume key %zu bytes\n", r, vk_size);
    crypt_free(cd);

    // Первый сектор payload'а → OpenSSL aes-256-xts, IV = plain64(0).
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    unsigned char ct[512], pt[512];
    if (fseek(f, (long)(data_off * 512), SEEK_SET) != 0 || fread(ct, 1, 512, f) != 512) {
        fprintf(stderr, "cannot read payload sector 0\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    unsigned char iv[16] = { 0 };
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int outl = 0, finl = 0;
    if (!ctx || EVP_DecryptInit_ex(ctx, EVP_aes_256_xts(), NULL, (unsigned char*)vk, iv) != 1 ||
        EVP_DecryptUpdate(ctx, pt, &outl, ct, 512) != 1 ||
        EVP_DecryptFinal_ex(ctx, pt + outl, &finl) != 1) {
        fprintf(stderr, "OpenSSL aes-256-xts failed\n");
        return 1;
    }
    EVP_CIPHER_CTX_free(ctx);
    memset(vk, 0, sizeof(vk));

    int fat32 = memcmp(pt + 82, "FAT32   ", 8) == 0 && pt[510] == 0x55 && pt[511] == 0xAA;
    char label[12] = { 0 };
    memcpy(label, pt + 71, 11);
    printf("OpenSSL: payload sector 0 -> %s (label '%s')\n",
           fat32 ? "FAT32 boot sector" : "NOT a FAT32 boot sector", label);
    return fat32 ? 0 : 1;
}

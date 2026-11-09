// ============================================================================
// blkcrypt.h — dm-crypt-аналог AMPLIFY-OS (этап 48).
// ----------------------------------------------------------------------------
// Отображение (mapping) превращает диапазон секторов нижнего устройства в
// виртуальное устройство с номером DISK_VDEV_CRYPT_BASE + i:
//
//     чтение  vdev[N]  =  XTS-decrypt( under[start + N], tweak = N )
//     запись  vdev[N]  →  under[start + N] = XTS-encrypt( data, tweak = N )
//
// Шифр — aes-xts-plain64, ключ 512 бит (key1 — данные, key2 — tweak), IV —
// номер сектора ВНУТРИ отображения (iv_offset = 0).  Ровно так Linux
// dm-crypt открывает LUKS1-том с `cipher aes-xts-plain64`, поэтому раздел,
// созданный AMPLIFY-OS, читается `cryptsetup open` без конвертации.
//
// Ключ живёт только в расписаниях AES внутри отображения; unmap затирает их.
// ============================================================================
#ifndef AOS_X86_64_BLKCRYPT_H
#define AOS_X86_64_BLKCRYPT_H

#include <stdint.h>
#include <stdbool.h>

#define BLKCRYPT_KEY_BYTES 64

// Создать отображение.  Возвращает номер виртуального устройства или -1.
int      blkcrypt_map(const char* name, int under_dev, uint64_t start_lba,
                      uint64_t sectors, const uint8_t key[BLKCRYPT_KEY_BYTES]);
// Удалить отображение и затереть ключ.
bool     blkcrypt_unmap(int vdev);

bool     blkcrypt_is(int vdev);
uint64_t blkcrypt_sectors(int vdev);
bool     blkcrypt_read (int vdev, uint64_t lba, uint64_t count, void* buf);
bool     blkcrypt_write(int vdev, uint64_t lba, uint64_t count, const void* buf);
bool     blkcrypt_flush(int vdev);

// Описание для `seal status`: false, если слота нет.
typedef struct {
    int         vdev;
    const char* name;
    int         under_dev;
    uint64_t    start_lba;
    uint64_t    sectors;
    uint64_t    reads, writes;     // секторов прочитано/записано через слой
} blkcrypt_info_t;

bool     blkcrypt_info(int index, blkcrypt_info_t* out);   // index 0..MAX-1

#endif

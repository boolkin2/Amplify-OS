// ============================================================================
// luks.h — заголовок LUKS1 на разделе (этап 48, FDE).
// ----------------------------------------------------------------------------
// Формат — LUKS1 On-Disk Format Specification 1.2.3, байт в байт как у
// cryptsetup, чтобы раздел, зашифрованный AMPLIFY-OS, открывался в Linux:
//
//   sector 0..1   phdr (1024 байта, все целые — big-endian):
//                 "LUKS\xBA\xBE", version 1, "aes", "xts-plain64", "sha256",
//                 payload_offset, key_bytes = 64, mk_digest (PBKDF2, 20 байт),
//                 mk_digest_salt, mk_digest_iter, uuid, 8 key slot'ов.
//   sector 8..    материал ключа слота 0 (500 секторов), слоты 1..7 — дальше
//                 с выравниванием на 4 КиБ.
//   sector 4096   payload: данные, aes-xts-plain64 под мастер-ключом (MK),
//                 IV = номер сектора внутри payload.
//
// Мастер-ключ (64 случайных байта) не зависит от пароля.  В слоте хранится
// MK, «размазанный» AF-splitter'ом на 4000 полос (стирание одной полосы
// делает слот невосстановимым) и зашифрованный ключом PBKDF2(пароль).
// Смена пароля = новый слот + уничтожение старого, данные не трогаются.
//
// Поддерживается ровно то, что создаём сами: aes / xts-plain64 / sha256 /
// 64-байтный ключ.  Чужой LUKS1 с другими параметрами распознаётся
// (luks_probe → supported = false), но не открывается.  LUKS2 — нет.
// ============================================================================
#ifndef AOS_X86_64_LUKS_H
#define AOS_X86_64_LUKS_H

#include <stdint.h>
#include <stdbool.h>

#define LUKS_NUMKEYS        8
#define LUKS_KEY_BYTES      64
#define LUKS_UUID_LEN       40

// Первый сектор payload при нашей раскладке (1 МиБ-выравнивание, как у
// cryptsetup по умолчанию): 8 слотов по 504 сектора заканчиваются на 4040.
#define LUKS_PAYLOAD_OFFSET 4096

typedef struct {
    bool     valid;                        // магия и версия LUKS1 на месте
    bool     supported;                    // параметры, которые мы умеем открыть
    char     uuid[LUKS_UUID_LEN + 1];
    char     cipher[72];                   // "aes-xts-plain64"
    char     hash[33];
    uint32_t payload_offset;               // в секторах от начала раздела
    uint32_t key_bytes;
    uint32_t mk_digest_iter;
    int      active_slots;
    uint32_t slot_iter[LUKS_NUMKEYS];      // 0 — слот выключен
} luks_info_t;

// Прогресс длинных операций (PBKDF2, запись материала ключа): 0..100.
typedef void (*luks_progress_cb)(int percent, const char* message, void* user);

// Коды возврата luks_open.
#define LUKS_OK          0
#define LUKS_BAD_PASS   -1
#define LUKS_IO_ERROR   -2
#define LUKS_BAD_HEADER -3

// Прочитать заголовок раздела (dev, lba).  true — на разделе LUKS1.
bool luks_probe(int dev, uint64_t lba, luks_info_t* out);

// Создать LUKS1 на разделе: новый MK, слот 0 под passphrase.  iter_ms —
// сколько миллисекунд этой машины должна занимать проверка пароля (0 —
// минимально допустимые 1000 итераций, только для self-test'ов).
// mk_out (может быть 0) получает мастер-ключ для немедленного отображения.
int  luks_format(int dev, uint64_t lba, uint64_t sectors, const char* passphrase,
                 uint32_t iter_ms, uint8_t mk_out[LUKS_KEY_BYTES],
                 luks_progress_cb cb, void* user);

// Проверить пароль по всем активным слотам и вернуть MK.
int  luks_open(int dev, uint64_t lba, const char* passphrase,
               uint8_t mk_out[LUKS_KEY_BYTES], int* slot_out);

// Добавить слот с новым паролем (нужен уже известный MK).  Возвращает номер
// слота или -1.
int  luks_add_key(int dev, uint64_t lba, const uint8_t mk[LUKS_KEY_BYTES],
                  const char* passphrase, uint32_t iter_ms);

// Выключить слот и затереть его материал.  Последний активный слот
// уничтожить нельзя (том стал бы невосстановимым) — возвращает -1.
int  luks_kill_slot(int dev, uint64_t lba, int slot);

// Итераций PBKDF2-HMAC-SHA256 в секунду на этой машине для ключа key_len.
uint32_t luks_benchmark(uint32_t key_len);

#endif

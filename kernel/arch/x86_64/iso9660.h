// ============================================================================
// iso9660.h — чтение файлов с установочного CD/DVD (ISO9660 + Rock Ridge).
// ----------------------------------------------------------------------------
// Зачем: веса AMPLIFY MIND (SmolLM2, 138 МБ) слишком велики для initrd —
// тот целиком грузится в RAM при КАЖДОЙ загрузке и копируется на ESP. Модель
// лежит на ISO отдельным файлом /mind/*.gguf; установщик переносит её на
// системный раздел (/vault/mind), а live-сессия может загрузить её прямо с
// диска. Только чтение; каталоги любой глубины; имена — Rock Ridge (NM) или,
// если их нет, ISO9660 без «;1» и без учёта регистра.
// ============================================================================
#ifndef AOS_X86_64_ISO9660_H
#define AOS_X86_64_ISO9660_H

#include <stdint.h>
#include <stdbool.h>

// Найти ATAPI-устройство с ISO9660 и /boot/kernel-x86_64.bin (наш носитель).
// Повторные вызовы дешёвые: результат кэшируется.
bool    iso_mount_boot(void);
bool    iso_present(void);
int     iso_device(void);

typedef struct {
    uint32_t lba;          // первый сектор (2048 байт)
    uint32_t size;         // байт
    bool     is_dir;
} iso_entry_t;

bool    iso_lookup(const char* path, iso_entry_t* out);
int64_t iso_read(const iso_entry_t* f, uint64_t off, void* buf, uint64_t len);

// Перечисление каталога; cb возвращает != 0, чтобы остановиться.
typedef int (*iso_dir_cb)(const char* name, const iso_entry_t* e, void* user);
int     iso_list(const char* path, iso_dir_cb cb, void* user);

#endif

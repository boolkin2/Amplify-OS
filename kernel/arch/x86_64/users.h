// ============================================================================
// users.h — простая база пользователей AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Этап 37.6c-4: добавляем минимальный механизм аутентификации.
//
// Хранение: в памяти + persist на /disk/users.txt (FAT32). Каждая строка:
//   <username>:<fnv1a_hash_hex16>\n
//
// Без соли (это учебная ОС, не банковский продукт). FNV-1a 64-bit достаточно
// для предотвращения случайного подбора в QEMU.
// ============================================================================
#ifndef AOS_X86_64_USERS_H
#define AOS_X86_64_USERS_H

#include <stdint.h>
#include <stdbool.h>

#define USERS_MAX_USERS  8
#define USERS_NAME_LEN   24

typedef struct {
    char     name[USERS_NAME_LEN];
    uint64_t pw_hash;
} user_record_t;

// Инициализирует базу: пытается прочитать /disk/users.txt. Если FAT32 не
// смонтирован или файла нет — база остаётся пустой.
void users_init(void);

// Сериализует базу в /disk/users.txt (если FAT32 доступен). Возвращает true
// при успехе. На noop FS — возвращает false но не считается ошибкой.
bool users_save(void);

// Текущее число пользователей.
int  users_count(void);

// Получить запись (read-only).
const user_record_t* users_at(int idx);

// Найти по имени; NULL если не найден.
const user_record_t* users_find(const char* name);

// Добавить пользователя. password — открытый текст, хешируется FNV-1a.
// Возвращает индекс или -1 если переполнено / уже есть.
int  users_add(const char* name, const char* password);

// Проверить пару (name, password). true если совпала или база пустая
// (legacy guest mode).
bool users_verify(const char* name, const char* password);

// FNV-1a 64-bit. Экспортирован для использования в firstboot.
uint64_t users_hash(const char* str);

#endif

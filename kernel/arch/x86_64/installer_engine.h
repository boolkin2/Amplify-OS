// ============================================================================
// installer_engine.h — движок установки AMPLIFY-OS на физический диск.
// ----------------------------------------------------------------------------
// Этап 57.  UI (installer.c) отвечает только за экраны; вся работа с железом
// живёт здесь:
//
//   1. Перечисление кандидатов-дисков (ATA, не-ATAPI, достаточного объёма).
//   2. Планирование разметки (part_plan) и запись GPT (part_write_gpt).
//   3. Форматирование ESP / SYSTEM / VAULT (fat32_mkfs).
//   4. Копирование payload'а из initrd на ESP:
//        /EFI/BOOT/BOOTX64.EFI, /kernel.bin, /initrd.img
//   5. Создание системного дерева на SYSTEM (Sanctum, nexus, vault, forge,
//      fonts) + users.txt + конфиг Sanctum.
//   6. Верификация: перечитать GPT, примонтировать SYSTEM, прочитать маркер.
//
// Движок синхронный и блокирующий: UI вызывает install_run() и получает
// прогресс через callback, который сам рисует кадр.
// ============================================================================
#ifndef AOS_X86_64_INSTALLER_ENGINE_H
#define AOS_X86_64_INSTALLER_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#define INSTALL_HOSTNAME_MAX 32
#define INSTALL_USER_MAX     24
#define INSTALL_PASS_MAX     32
#define INSTALL_MAX_TARGETS  4

// Кандидат на установку — то, что показываем на экране DESTINATION.
typedef struct {
    int      dev;              // индекс ATA-устройства
    uint64_t total_sectors;
    uint64_t size_mb;
    char     model[41];
    const char* bus;           // "IDE0 master" и т.п.
    bool     has_partitions;   // на диске уже есть таблица разделов
    bool     amplify;          // похоже, AMPLIFY-OS уже установлена
} install_target_t;

typedef struct {
    int  target_dev;
    bool vault;                // выделить отдельный раздел под /vault
    bool dark;                 // тема по умолчанию — тёмная
    bool encrypt;              // [этап 48] LUKS1 на системном (и vault) разделе
    char hostname[INSTALL_HOSTNAME_MAX];
    char username[INSTALL_USER_MAX];
    char passphrase[INSTALL_PASS_MAX];
} install_config_t;

typedef enum {
    INSTALL_STEP_PLAN = 0,
    INSTALL_STEP_PARTITION,
    INSTALL_STEP_FORMAT_ESP,
    INSTALL_STEP_ENCRYPT,          // [этап 48] LUKS1-заголовок + отображение
    INSTALL_STEP_FORMAT_SYSTEM,
    INSTALL_STEP_FORMAT_VAULT,
    INSTALL_STEP_BOOTLOADER,
    INSTALL_STEP_KERNEL,
    INSTALL_STEP_SYSTEM_TREE,
    INSTALL_STEP_IDENTITY,
    INSTALL_STEP_MIND,             // модель AMPLIFY MIND с носителя -> /vault/mind
    INSTALL_STEP_VERIFY,
    INSTALL_STEP_COUNT
} install_step_t;

// percent — 0..100 внутри текущего шага.  message может быть 0.
typedef void (*install_progress_cb)(install_step_t step, int percent,
                                    const char* message, void* user);

// Пересканировать ATA и собрать список кандидатов.  Возвращает их число.
int  install_scan_targets(void);
int  install_target_count(void);
const install_target_t* install_target_at(int index);

// Требуемый минимум места (в МБ) — показываем на экране WELCOME.
uint64_t install_required_mb(void);

const char* install_step_name(install_step_t step);

// Полный цикл установки.  true при успехе; иначе см. install_last_error().
bool install_run(const install_config_t* cfg, install_progress_cb cb, void* user);

const char* install_last_error(void);

// Предпросмотр разметки для UI: размеры разделов в МБ (0 если раздела нет).
void install_preview_layout(int dev, bool vault,
                            uint64_t* esp_mb, uint64_t* sys_mb, uint64_t* vault_mb);

// Применить конфигурацию установленной системы (hostname из
// /disk/Sanctum/system.conf, светлая/тёмная тема из
// /disk/Sanctum/system/ui-config.ini).  Вызывается на каждой загрузке;
// если файлов нет (dev-образ, live-сессия) — ничего не делает.
void install_apply_system_config(void);

#endif

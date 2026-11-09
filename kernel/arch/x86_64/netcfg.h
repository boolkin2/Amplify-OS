// ============================================================================
// netcfg.h — настройка сети AMPLIFY-OS (подэтап 48.2).
// ----------------------------------------------------------------------------
// Единственное место, которое назначает адрес интерфейсу. Хранит выбор
// пользователя в /disk/Sanctum/network.conf:
//
//     mode=auto | dhcp | static
//     ip=192.168.1.50
//     mask=255.255.255.0
//     gateway=192.168.1.1
//     dns=192.168.1.1
//     dns2=1.1.1.1
//
//   dhcp   — только DHCP (клиент повторяет запросы, пока сервер не ответит);
//   static — только адрес из файла, DHCP выключен;
//   auto   — DHCP, а если он молчит NETCFG_FALLBACK_MS — адрес из файла
//            («запасная статика»); DHCP при этом продолжает спрашивать, и как
//            только сервер ответит, система переходит на выданный адрес.
// Файла нет (live-сессия, чистая установка) — режим auto без запасного адреса.
// ============================================================================
#ifndef AOS_X86_64_NETCFG_H
#define AOS_X86_64_NETCFG_H

#include <stdint.h>
#include <stdbool.h>

#define NETCFG_PATH        "/disk/Sanctum/network.conf"
#define NETCFG_FALLBACK_MS 3000     // сколько auto ждёт DHCP до запасной статики
#define NETCFG_BOOT_WAIT_MS 5000    // сколько загрузка ждёт адрес от DHCP

typedef enum { NETCFG_AUTO = 0, NETCFG_DHCP, NETCFG_STATIC } netcfg_mode_t;

typedef struct {
    netcfg_mode_t mode;
    uint32_t ip, mask, gateway, dns, dns2;      // статическая часть (0 — не задано)
} netcfg_t;

// Откуда взят действующий адрес.
typedef enum {
    NETCFG_SRC_NONE = 0,     // адреса нет
    NETCFG_SRC_DHCP,
    NETCFG_SRC_STATIC,       // режим static
    NETCFG_SRC_FALLBACK,     // режим auto, DHCP молчит — адрес из файла
} netcfg_source_t;

// При загрузке (после net_init, прерывания разрешены): прочитать файл,
// применить, для DHCP подождать адрес.
void netcfg_boot(void);
// Перед входом в цикл WM: если при загрузке /disk ещё не был доступен
// (зашифрованный том открывается позже), прочитать файл теперь.
void netcfg_late_load(void);
// Из цикла WM: DHCP-автомат, продление аренды, запасная статика.
void netcfg_tick(void);

const netcfg_t* netcfg_get(void);
netcfg_source_t netcfg_source(void);
const char*     netcfg_source_name(void);
const char*     netcfg_mode_name(netcfg_mode_t m);
bool            netcfg_loaded_from_file(void);

// Проверить настройку. При ошибке *why — короткое пояснение (CP1251).
bool netcfg_validate(const netcfg_t* c, const char** why);
// Применить (и запомнить как текущую). false — не прошла проверку.
bool netcfg_apply(const netcfg_t* c, const char** why);
// Записать текущую настройку в network.conf. false — диск недоступен.
bool netcfg_save(void);

// "192.168.1.10" <-> uint32_t. parse: false, если строка не адрес.
bool netcfg_parse_ip(const char* s, uint32_t* out);
void netcfg_format_ip(uint32_t ip, char out[16]);
// Длина префикса маски (255.255.255.0 → 24) или -1, если маска «с дырами».
int  netcfg_mask_bits(uint32_t mask);

#endif

// ============================================================================
// dhcp.h — DHCP-клиент (RFC 2131) для AMPLIFY-OS x86_64.
// [48.2] Автомат с повторами и продлением аренды — описание в dhcp.c.
// Адрес интерфейсу назначает НЕ клиент, а netcfg.c: он забирает события
// (dhcp_take_events) и решает, что применять (DHCP, статика, запасная статика).
// ============================================================================
#ifndef AOS_X86_64_DHCP_H
#define AOS_X86_64_DHCP_H

#include <stdint.h>
#include <stdbool.h>

#define DHCP_CLIENT_PORT 68
#define DHCP_SERVER_PORT 67

// События для netcfg (битовая маска).
#define DHCP_EV_BOUND    1     // получен адрес
#define DHCP_EV_RENEWED  2     // аренда продлена (адрес мог смениться)
#define DHCP_EV_LOST     4     // аренда истекла или сервер отказал (NAK)

// Начать поиск сервера; если аренда уже есть — попросить продлить её сейчас.
bool dhcp_start(void);
// Выключить клиент (режим «статика»).
void dhcp_stop(void);
// Автомат: звать из главного потока каждые ~10 мс.
void dhcp_tick(void);
int  dhcp_take_events(void);

bool dhcp_is_bound(void);
bool dhcp_is_enabled(void);
bool dhcp_is_selecting(void);
const char* dhcp_state_name(void);
void dhcp_dump_state(void);

// Параметры действующей аренды.
uint32_t dhcp_get_ip(void);
uint32_t dhcp_get_mask(void);
uint32_t dhcp_get_router(void);
uint32_t dhcp_get_dns(void);
uint32_t dhcp_get_dns2(void);
uint32_t dhcp_get_server(void);
uint32_t dhcp_get_lease(void);        // срок аренды от сервера, с
uint32_t dhcp_lease_left_s(void);     // сколько осталось
uint32_t dhcp_renew_count(void);

// Потолок срока аренды (perf.conf: dhcplease=N). Нужен, чтобы продление можно
// было проверить за секунды, а не ждать сутки, которые выдаёт slirp.
void dhcp_set_lease_cap(uint32_t seconds);

// Старое имя шага «REQUEST после OFFER» — теперь это dhcp_tick.
bool dhcp_pump(void);

#endif

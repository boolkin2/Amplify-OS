// ============================================================================
// virtio_net.h — драйвер virtio-net (подэтап 41.2): паравиртуальная сетевая
// карта QEMU/Proxmox (`net0: virtio=...`) — аналог NetKVM из virtio-win.
// ----------------------------------------------------------------------------
// e1000 эмулирует настоящую карту Intel: каждое обращение к её регистрам —
// выход в гипервизор. virtio-net передаёт кадры через очереди в общей памяти:
// приём — по прерыванию MSI-X, передача — одной записью в очередь.
// Для стека (dhcp/dns/tcp/tls/https) это тот же net_interface_t, что и e1000.
// ============================================================================
#ifndef AOS_X86_64_VIRTIO_NET_H
#define AOS_X86_64_VIRTIO_NET_H

#include <stdint.h>
#include <stdbool.h>

// Найти карту virtio-net и зарегистрировать её как eth0. false — устройства нет
// (тогда net_init пробует e1000 и rtl8139). Звать после lapic_init.
bool           virtio_net_init(void);
bool           virtio_net_is_ready(void);
const uint8_t* virtio_net_get_mac(void);
// Счётчики для диагностики и тестов.
uint64_t       virtio_net_rx_frames(void);
uint64_t       virtio_net_tx_frames(void);
uint64_t       virtio_net_irqs(void);
uint64_t       virtio_net_tx_drops(void);

#endif

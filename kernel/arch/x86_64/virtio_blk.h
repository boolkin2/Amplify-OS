// ============================================================================
// virtio_blk.h — драйвер virtio-blk (подэтап 41.2): паравиртуальный диск
// QEMU/Proxmox (`virtioN:` в конфиге ВМ) — аналог viostor из virtio-win.
// ----------------------------------------------------------------------------
// Зачем: IDE под KVM — это выход в гипервизор на каждое обращение к порту
// (несколько на сектор), AHCI — эмуляция целого контроллера и копия через
// bounce-буфер 64 КБ. virtio-blk получает запрос одной записью в очередь:
// заголовок + указатель на буфер вызывающего (без копии, до 1 МиБ за запрос) +
// байт статуса; завершение — по прерыванию MSI-X.
//
// Диск виден системе как обычный слот ata.c (kind = ATA_KIND_VIRTIO), поэтому
// установщик, GPT, FAT32, LUKS/FDE и MIND работают с ним без правок.
// Вызывать только с BSP из главного потока (как весь дисковый стек).
// ============================================================================
#ifndef AOS_X86_64_VIRTIO_BLK_H
#define AOS_X86_64_VIRTIO_BLK_H

#include <stdint.h>
#include <stdbool.h>

#define VIRTIO_BLK_MAX_DISKS  4

// Найти диски virtio-blk. Возвращает их число. Можно звать до инициализации
// LAPIC и разрешения прерываний: до тех пор запросы ждут ответа опросом.
int      virtio_blk_init(void);
int      virtio_blk_count(void);
uint64_t virtio_blk_sectors(int disk);          // размер в секторах по 512 байт
bool     virtio_blk_read (int disk, uint64_t lba, uint32_t count, void* buf);
bool     virtio_blk_write(int disk, uint64_t lba, uint32_t count, const void* buf);
bool     virtio_blk_flush(int disk);

// Убедиться, что прерывание MSI-X очереди запросов реально приходит; если
// нет — вернуть диск на опрос. Звать, когда прерывания уже разрешены.
void     virtio_blk_irq_selftest(void);

#endif

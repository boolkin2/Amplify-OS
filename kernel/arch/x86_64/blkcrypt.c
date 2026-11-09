// ============================================================================
// blkcrypt.c — прозрачное XTS-шифрование секторов (dm-crypt-аналог, этап 48).
// ============================================================================
#include "blkcrypt.h"
#include "disk_io.h"
#include "crypto.h"
#include "serial.h"

typedef struct {
    bool             used;
    char             name[24];
    int              under_dev;
    uint64_t         start_lba;
    uint64_t         sectors;
    uint64_t         reads, writes;
    crypto_xts_ctx_t xts;
} bc_map_t;

static bc_map_t g_maps[DISK_VDEV_CRYPT_MAX];

// Буфер для записи: входные данные const, а шифровать надо не на месте.
// Кусок = 64 КиБ — тот же размер, что и PIO-кусок disk_io.
#define BC_CHUNK 128
static uint8_t g_bounce[DISK_SECTOR_SIZE * BC_CHUNK];

static bc_map_t* map_of(int vdev) {
    int i = vdev - DISK_VDEV_CRYPT_BASE;
    if (i < 0 || i >= DISK_VDEV_CRYPT_MAX || !g_maps[i].used) return 0;
    return &g_maps[i];
}

int blkcrypt_map(const char* name, int under_dev, uint64_t start_lba,
                 uint64_t sectors, const uint8_t key[BLKCRYPT_KEY_BYTES]) {
    if (!key || sectors == 0 || !disk_present_dev(under_dev)) return -1;
    uint64_t total = disk_total_sectors_dev(under_dev);
    if (total && start_lba + sectors > total) return -1;

    for (int i = 0; i < DISK_VDEV_CRYPT_MAX; i++) {
        if (g_maps[i].used) continue;
        bc_map_t* m = &g_maps[i];
        int k = 0;
        for (; name && name[k] && k < (int)sizeof(m->name) - 1; k++) m->name[k] = name[k];
        m->name[k]   = 0;
        m->under_dev = under_dev;
        m->start_lba = start_lba;
        m->sectors   = sectors;
        m->reads     = 0;
        m->writes    = 0;
        crypto_cpu_init();                      // AES-NI, если есть
        crypto_xts256_init(&m->xts, key, key + 32);
        m->used = true;

        serial_puts("[blkcrypt] mapped ");
        serial_puts(m->name);
        serial_puts(" -> vdev ");
        serial_print_dec((uint64_t)(DISK_VDEV_CRYPT_BASE + i));
        serial_puts(" (dev ");
        serial_print_dec((uint64_t)under_dev);
        serial_puts(" @ lba ");
        serial_print_dec(start_lba);
        serial_puts(", ");
        serial_print_dec(sectors);
        serial_puts(" sectors, aes-xts-plain64)\n");
        return DISK_VDEV_CRYPT_BASE + i;
    }
    return -1;
}

bool blkcrypt_unmap(int vdev) {
    bc_map_t* m = map_of(vdev);
    if (!m) return false;
    crypto_wipe(&m->xts, sizeof(m->xts));
    m->used = false;
    serial_puts("[blkcrypt] unmapped vdev ");
    serial_print_dec((uint64_t)vdev);
    serial_puts("\n");
    return true;
}

bool blkcrypt_is(int vdev) { return map_of(vdev) != 0; }

uint64_t blkcrypt_sectors(int vdev) {
    bc_map_t* m = map_of(vdev);
    return m ? m->sectors : 0;
}

bool blkcrypt_read(int vdev, uint64_t lba, uint64_t count, void* buf) {
    bc_map_t* m = map_of(vdev);
    if (!m || !buf || lba + count > m->sectors) return false;
    // Читаем шифротекст прямо в буфер вызывающего и расшифровываем на месте:
    // XTS работает поблочно, вход и выход могут совпадать.
    if (!disk_read_sectors_dev(m->under_dev, m->start_lba + lba, count, buf)) return false;
    uint8_t* p = (uint8_t*)buf;
    for (uint64_t s = 0; s < count; s++)
        crypto_xts_decrypt(&m->xts, lba + s, p + s * DISK_SECTOR_SIZE,
                           p + s * DISK_SECTOR_SIZE, DISK_SECTOR_SIZE);
    m->reads += count;
    return true;
}

bool blkcrypt_write(int vdev, uint64_t lba, uint64_t count, const void* buf) {
    bc_map_t* m = map_of(vdev);
    if (!m || !buf || lba + count > m->sectors) return false;
    const uint8_t* src = (const uint8_t*)buf;
    while (count > 0) {
        uint64_t n = count > BC_CHUNK ? BC_CHUNK : count;
        for (uint64_t s = 0; s < n; s++)
            crypto_xts_encrypt(&m->xts, lba + s, src + s * DISK_SECTOR_SIZE,
                               g_bounce + s * DISK_SECTOR_SIZE, DISK_SECTOR_SIZE);
        if (!disk_write_sectors_dev(m->under_dev, m->start_lba + lba, n, g_bounce)) {
            crypto_wipe(g_bounce, sizeof(g_bounce));
            return false;
        }
        m->writes += n;
        lba   += n;
        src   += n * DISK_SECTOR_SIZE;
        count -= n;
    }
    // В bounce-буфере остался только шифротекст, но чистим всё равно —
    // дёшево, а «никаких следов данных вне отображения» проще проверять.
    crypto_wipe(g_bounce, sizeof(g_bounce));
    return true;
}

bool blkcrypt_flush(int vdev) {
    bc_map_t* m = map_of(vdev);
    return m && disk_flush_dev(m->under_dev);
}

bool blkcrypt_info(int index, blkcrypt_info_t* out) {
    if (index < 0 || index >= DISK_VDEV_CRYPT_MAX || !g_maps[index].used || !out) return false;
    const bc_map_t* m = &g_maps[index];
    out->vdev      = DISK_VDEV_CRYPT_BASE + index;
    out->name      = m->name;
    out->under_dev = m->under_dev;
    out->start_lba = m->start_lba;
    out->sectors   = m->sectors;
    out->reads     = m->reads;
    out->writes    = m->writes;
    return true;
}

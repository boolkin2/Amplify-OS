// ============================================================================
// fat32.c - Реализация драйвера FAT32
// ============================================================================
#include "fat32.h"
#include "../drivers/ata.h"
#include "disk_io.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../vga.h"
#include <stdint.h>

// ============================================================================
// Этап 28: поддержка нескольких одновременных монтирований FAT32 (например
// /disk = ATA и /usb = USB-флешка). Старая реализация хранила всё состояние
// в singleton'е g_fs-> Теперь у нас пул из FAT32_MAX_INSTANCES слотов;
// активный выбирается перед каждой VFS-операцией функцией fs_from_node().
// Глобальный указатель g_fs указывает на «текущий» слот для всех helper-ов
// (cluster_to_lba, read_fat_entry, alloc_cluster, ...).
// ============================================================================
#define FAT32_MAX_INSTANCES 2

static fat32_fs_t  fat_fs_pool[FAT32_MAX_INSTANCES];
static int         fat_fs_count = 0;
static fat32_fs_t* g_fs = &fat_fs_pool[0];
static uint8_t     sector_buf[512];

// Найти, какому слоту принадлежит данный VFS-узел: либо это root_node одного
// из слотов, либо узел из его пула nodes[]. Если ничего не подходит — даём
// текущий g_fs как fallback (не должно случаться).
static fat32_fs_t* fs_from_node(struct vfs_node* node) {
    for (int i = 0; i < FAT32_MAX_INSTANCES; i++) {
        fat32_fs_t* f = &fat_fs_pool[i];
        if (!f->mounted) continue;
        if (node == &f->root_node) return f;
        if (node >= &f->nodes[0] && node < &f->nodes[FAT32_MAX_NODES]) return f;
    }
    return g_fs;
}

// ============================================================================
// Вспомогательные функции
// ============================================================================

static uint32_t cluster_to_lba(uint32_t cluster) {
    if (cluster < 2) return 0;
    return g_fs->data_start_lba + (cluster - 2) * g_fs->sectors_per_cluster;
}

// ВАЖНО: read_fat_entry / write_fat_entry / alloc_cluster раньше использовали
// общий буфер sector_buf, что приводило к критической порче данных:
// при создании файла create_entry() заполнял dirent в sector_buf, затем
// вызывал alloc_cluster(), который ВНУТРИ перечитывал FAT в тот же sector_buf
// и затем затирал его memset'ом для очистки нового кластера. После этого
// create_entry писал в обнулённый sector_buf обратно в корневой каталог,
// уничтожая все ранее существовавшие записи (vault/nexus/forge/...).
// Чиним: даём этим функциям отдельные локальные буферы.
static uint32_t read_fat_entry(uint32_t cluster) {
    uint8_t fat_buf[512];
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fs->fat_start_lba + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;
    if (!disk_read_sectors(g_fs->drive, fat_sector, 1, fat_buf)) return 0;
    uint32_t next_cluster = *(uint32_t*)&fat_buf[ent_offset];
    return next_cluster & FAT32_CLUSTER_MASK;
}

static void write_fat_entry(uint32_t cluster, uint32_t value) {
    uint8_t fat_buf[512];
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = g_fs->fat_start_lba + (fat_offset / 512);
    uint32_t ent_offset = fat_offset % 512;
    if (!disk_read_sectors(g_fs->drive, fat_sector, 1, fat_buf)) return;
    uint32_t* entry = (uint32_t*)&fat_buf[ent_offset];
    *entry = (*entry & ~FAT32_CLUSTER_MASK) | (value & FAT32_CLUSTER_MASK);
    disk_write_sectors(g_fs->drive, fat_sector, 1, fat_buf);

    // Зеркалируем во вторую FAT
    if (g_fs->num_fats > 1) {
        disk_write_sectors(g_fs->drive, fat_sector + g_fs->fat_size, 1, fat_buf);
    }
}

static uint32_t alloc_cluster(void) {
    for (uint32_t i = 2; i < g_fs->total_clusters; i++) {
        if (read_fat_entry(i) == FAT32_FREE_CLUSTER) {
            write_fat_entry(i, FAT32_EOC);
            // Очищаем кластер. ВАЖНО: используем локальный буфер, а не общий
            // sector_buf — иначе вызывающий create_entry() потеряет dirent,
            // который он только что положил в sector_buf.
            uint8_t zero_buf[512];
            memset(zero_buf, 0, 512);
            uint32_t lba = cluster_to_lba(i);
            for (uint8_t j = 0; j < g_fs->sectors_per_cluster; j++) {
                disk_write_sectors(g_fs->drive, lba + j, 1, zero_buf);
            }
            return i;
        }
    }
    return 0; // Нет места
}

// Функция очистки цепочки кластеров временно удалена, так как пока не используется в unlink

static void format_short_name(const char* name, char* short_name) {
    memset(short_name, ' ', 11);
    int i = 0, j = 0;
    while (name[i] && name[i] != '.' && j < 8) {
        char c = name[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        short_name[j++] = c;
    }
    if (name[i] == '.') {
        i++;
        j = 8;
        while (name[i] && j < 11) {
            char c = name[i++];
            if (c >= 'a' && c <= 'z') c -= 32;
            short_name[j++] = c;
        }
    }
}

static bool cmp_short_name(const char* short_name, const char* name) {
    char fmt[11];
    format_short_name(name, fmt);
    return memcmp(short_name, fmt, 11) == 0;
}

// Обновить поле file_size в dirent родительского каталога.
// Используется после записи/truncate, чтобы на диске размер совпадал с node->length.
static void update_dirent_size(uint32_t parent_cluster,
                               uint32_t file_cluster,
                               uint32_t new_size)
{
    if (parent_cluster < 2 || file_cluster < 2) return;

    uint32_t cluster = parent_cluster;
    while (cluster >= 2 && cluster < FAT32_BAD_CLUSTER) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t i = 0; i < g_fs->sectors_per_cluster; i++) {
            if (!disk_read_sectors(g_fs->drive, lba + i, 1, sector_buf)) return;
            for (int j = 0; j < 512; j += 32) {
                fat32_dirent_t* d = (fat32_dirent_t*)&sector_buf[j];
                if (d->name[0] == 0x00) return;                    // конец
                if (d->name[0] == (char)0xE5 || d->attr == 0x0F)   // удалён / LFN
                    continue;
                uint32_t c = ((uint32_t)d->first_cluster_hi << 16) | d->first_cluster_lo;
                if (c == file_cluster) {
                    d->file_size = new_size;
                    disk_write_sectors(g_fs->drive, lba + i, 1, sector_buf);
                    return;
                }
            }
        }
        cluster = read_fat_entry(cluster);
    }
}

// ============================================================================
// VFS Callbacks
// ============================================================================

static uint32_t fat32_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    g_fs = fs_from_node(node);
    if (offset >= node->length) return 0;
    if (offset + size > node->length) size = node->length - offset;

    uint32_t cluster = node->impl;
    uint32_t clus_size = g_fs->cluster_size;
    
    // Пропускаем кластеры до offset
    uint32_t skip = offset / clus_size;
    for (uint32_t i = 0; i < skip; i++) {
        cluster = read_fat_entry(cluster);
        if (cluster >= FAT32_BAD_CLUSTER) return 0;
    }
    
    uint32_t clus_offset = offset % clus_size;
    uint32_t bytes_read = 0;
    
    while (bytes_read < size && cluster >= 2 && cluster < FAT32_BAD_CLUSTER) {
        uint32_t lba = cluster_to_lba(cluster);
        uint32_t chunk = clus_size - clus_offset;
        if (chunk > size - bytes_read) chunk = size - bytes_read;
        
        for (uint8_t i = 0; i < g_fs->sectors_per_cluster; i++) {
            uint32_t sec_off = i * 512;
            if (sec_off >= clus_offset + chunk) break;
            if (sec_off + 512 <= clus_offset) continue;
            
            disk_read_sectors(g_fs->drive, lba + i, 1, sector_buf);
            
            uint32_t start = (sec_off < clus_offset) ? clus_offset - sec_off : 0;
            uint32_t end = (sec_off + 512 > clus_offset + chunk) ? (clus_offset + chunk - sec_off) : 512;
            uint32_t len = end - start;
            
            memcpy(buffer + bytes_read, sector_buf + start, len);
            bytes_read += len;
        }
        
        cluster = read_fat_entry(cluster);
        clus_offset = 0;
    }
    return bytes_read;
}

static uint32_t fat32_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    g_fs = fs_from_node(node);
    if (size == 0) return 0;

    uint32_t cluster = node->impl;
    if (cluster == 0) {
        cluster = alloc_cluster();
        if (!cluster) return 0;
        node->impl = cluster;
    }
    
    uint32_t clus_size = g_fs->cluster_size;
    
    // Добавляем кластеры если нужно
    uint32_t needed_clusters = (offset + size + clus_size - 1) / clus_size;
    uint32_t curr = cluster;
    uint32_t count = 1;
    while (read_fat_entry(curr) < FAT32_BAD_CLUSTER) {
        curr = read_fat_entry(curr);
        count++;
    }
    while (count < needed_clusters) {
        uint32_t next = alloc_cluster();
        if (!next) break;
        write_fat_entry(curr, next);
        curr = next;
        count++;
    }
    
    uint32_t skip = offset / clus_size;
    curr = cluster;
    for (uint32_t i = 0; i < skip; i++) {
        curr = read_fat_entry(curr);
    }
    
    uint32_t clus_offset = offset % clus_size;
    uint32_t bytes_written = 0;
    
    while (bytes_written < size && curr >= 2 && curr < FAT32_BAD_CLUSTER) {
        uint32_t lba = cluster_to_lba(curr);
        uint32_t chunk = clus_size - clus_offset;
        if (chunk > size - bytes_written) chunk = size - bytes_written;
        
        for (uint8_t i = 0; i < g_fs->sectors_per_cluster; i++) {
            uint32_t sec_off = i * 512;
            if (sec_off >= clus_offset + chunk) break;
            if (sec_off + 512 <= clus_offset) continue;
            
            uint32_t start = (sec_off < clus_offset) ? clus_offset - sec_off : 0;
            uint32_t end = (sec_off + 512 > clus_offset + chunk) ? (clus_offset + chunk - sec_off) : 512;
            uint32_t len = end - start;
            
            if (len < 512) disk_read_sectors(g_fs->drive, lba + i, 1, sector_buf);
            memcpy(sector_buf + start, buffer + bytes_written, len);
            disk_write_sectors(g_fs->drive, lba + i, 1, sector_buf);
            
            bytes_written += len;
        }
        
        curr = read_fat_entry(curr);
        clus_offset = 0;
    }
    
    if (offset + bytes_written > node->length) {
        node->length = offset + bytes_written;
        // Синхронизируем размер с dirent в родительском каталоге.
        // Родительский cluster сохранён в fs_data при lookup через finddir().
        uint32_t parent_cluster = (uint32_t)(uintptr_t)node->fs_data;
        if (parent_cluster >= 2) {
            update_dirent_size(parent_cluster, node->impl, node->length);
        }
    }
    return bytes_written;
}

static vfs_dirent_t dirent;

static vfs_dirent_t* fat32_readdir(vfs_node_t* node, uint32_t index) {
    g_fs = fs_from_node(node);
    uint32_t cluster = node->impl;
    uint32_t curr_idx = 0;
    
    while (cluster >= 2 && cluster < FAT32_BAD_CLUSTER) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t i = 0; i < g_fs->sectors_per_cluster; i++) {
            disk_read_sectors(g_fs->drive, lba + i, 1, sector_buf);
            for (int j = 0; j < 512; j += 32) {
                fat32_dirent_t* d = (fat32_dirent_t*)&sector_buf[j];
                if (d->name[0] == 0x00) return NULL; // Конец директории
                if (d->name[0] == (char)0xE5) continue; // Удалён
                if (d->attr == 0x0F) continue; // LFN
                
                if (curr_idx == index) {
                    int k, p = 0;
                    for (k = 0; k < 8 && d->name[k] != ' '; k++) dirent.name[p++] = d->name[k];
                    if (d->name[8] != ' ') {
                        dirent.name[p++] = '.';
                        for (k = 8; k < 11 && d->name[k] != ' '; k++) dirent.name[p++] = d->name[k];
                    }
                    dirent.name[p] = '\0';
                    dirent.inode = (d->first_cluster_hi << 16) | d->first_cluster_lo;
                    return &dirent;
                }
                curr_idx++;
            }
        }
        cluster = read_fat_entry(cluster);
    }
    return NULL;
}

static vfs_node_t* alloc_vfs_node(void) {
    if (g_fs->node_count >= FAT32_MAX_NODES) return NULL;
    return &g_fs->nodes[g_fs->node_count++];
}

static int fat32_create(vfs_node_t* parent, const char* name, uint32_t flags);
static int fat32_mkdir(vfs_node_t* parent, const char* name);

static vfs_node_t* fat32_finddir(vfs_node_t* node, const char* name) {
    g_fs = fs_from_node(node);
    uint32_t cluster = node->impl;
    
    while (cluster >= 2 && cluster < FAT32_BAD_CLUSTER) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t i = 0; i < g_fs->sectors_per_cluster; i++) {
            disk_read_sectors(g_fs->drive, lba + i, 1, sector_buf);
            for (int j = 0; j < 512; j += 32) {
                fat32_dirent_t* d = (fat32_dirent_t*)&sector_buf[j];
                if (d->name[0] == 0x00) return NULL;
                if (d->name[0] == (char)0xE5) continue;
                if (d->attr == 0x0F) continue;
                
                if (cmp_short_name(d->name, name)) {
                    vfs_node_t* ret = alloc_vfs_node();
                    if (!ret) return NULL;
                    
                    int k, p = 0;
                    for (k = 0; k < 8 && d->name[k] != ' '; k++) ret->name[p++] = d->name[k];
                    if (d->name[8] != ' ') {
                        ret->name[p++] = '.';
                        for (k = 8; k < 11 && d->name[k] != ' '; k++) ret->name[p++] = d->name[k];
                    }
                    ret->name[p] = '\0';
                    
                    ret->flags = (d->attr & FAT32_ATTR_DIRECTORY) ? VFS_DIRECTORY : VFS_FILE;
                    ret->impl = (d->first_cluster_hi << 16) | d->first_cluster_lo;
                    ret->length = d->file_size;
                    // Сохраняем родительский cluster для последующего update_dirent_size
                    ret->fs_data = (void*)(uintptr_t)node->impl;
                    ret->parent = node;

                    ret->read = fat32_read;
                    ret->write = fat32_write;
                    ret->readdir = fat32_readdir;
                    ret->finddir = fat32_finddir;
                    ret->create = fat32_create;
                    ret->mkdir = fat32_mkdir;

                    return ret;
                }
            }
        }
        cluster = read_fat_entry(cluster);
    }
    return NULL;
}

static int create_entry(vfs_node_t* parent, const char* name, uint8_t attr, uint32_t* out_cluster) {
    char short_name[11];
    format_short_name(name, short_name);
    
    uint32_t cluster = parent->impl;
    uint32_t last_cluster = cluster;
    
    while (cluster >= 2 && cluster < FAT32_BAD_CLUSTER) {
        uint32_t lba = cluster_to_lba(cluster);
        for (uint8_t i = 0; i < g_fs->sectors_per_cluster; i++) {
            disk_read_sectors(g_fs->drive, lba + i, 1, sector_buf);
            for (int j = 0; j < 512; j += 32) {
                fat32_dirent_t* d = (fat32_dirent_t*)&sector_buf[j];
                if (d->name[0] == 0x00 || d->name[0] == (char)0xE5) {
                    // Нашли свободное место
                    memset(d, 0, 32);
                    memcpy(d->name, short_name, 11);
                    d->attr = attr;
                    uint32_t new_cluster = alloc_cluster();
                    if (!new_cluster) return VFS_ERR_NOSPC;
                    d->first_cluster_hi = new_cluster >> 16;
                    d->first_cluster_lo = new_cluster & 0xFFFF;
                    d->file_size = 0;
                    disk_write_sectors(g_fs->drive, lba + i, 1, sector_buf);
                    if (out_cluster) *out_cluster = new_cluster;
                    return VFS_OK;
                }
            }
        }
        last_cluster = cluster;
        cluster = read_fat_entry(cluster);
    }
    
    // Если места нет, выделяем новый кластер для директории
    uint32_t new_dir_cluster = alloc_cluster();
    if (!new_dir_cluster) return VFS_ERR_NOSPC;
    write_fat_entry(last_cluster, new_dir_cluster);
    
    // И пишем туда
    memset(sector_buf, 0, 512);
    fat32_dirent_t* d = (fat32_dirent_t*)sector_buf;
    memcpy(d->name, short_name, 11);
    d->attr = attr;
    uint32_t new_cluster = alloc_cluster();
    if (!new_cluster) return VFS_ERR_NOSPC;
    d->first_cluster_hi = new_cluster >> 16;
    d->first_cluster_lo = new_cluster & 0xFFFF;
    d->file_size = 0;
    disk_write_sectors(g_fs->drive, cluster_to_lba(new_dir_cluster), 1, sector_buf);
    
    if (out_cluster) *out_cluster = new_cluster;
    return VFS_OK;
}

static int fat32_create(vfs_node_t* parent, const char* name, uint32_t flags) {
    g_fs = fs_from_node(parent);
    (void)flags;
    return create_entry(parent, name, 0, NULL);
}

static int fat32_mkdir(vfs_node_t* parent, const char* name) {
    g_fs = fs_from_node(parent);
    uint32_t new_cluster = 0;
    int rc = create_entry(parent, name, FAT32_ATTR_DIRECTORY, &new_cluster);
    if (rc == VFS_OK && new_cluster) {
        // Создаём . и ..
        memset(sector_buf, 0, 512);
        fat32_dirent_t* dot = (fat32_dirent_t*)&sector_buf[0];
        memset(dot->name, ' ', 11);
        dot->name[0] = '.';
        dot->attr = FAT32_ATTR_DIRECTORY;
        dot->first_cluster_hi = new_cluster >> 16;
        dot->first_cluster_lo = new_cluster & 0xFFFF;
        
        fat32_dirent_t* dotdot = (fat32_dirent_t*)&sector_buf[32];
        memset(dotdot->name, ' ', 11);
        dotdot->name[0] = '.'; dotdot->name[1] = '.';
        dotdot->attr = FAT32_ATTR_DIRECTORY;
        dotdot->first_cluster_hi = parent->impl >> 16;
        dotdot->first_cluster_lo = parent->impl & 0xFFFF;
        
        disk_write_sectors(g_fs->drive, cluster_to_lba(new_cluster), 1, sector_buf);
    }
    return rc;
}

// ============================================================================
// ============================================================================
// Initsializatsiya i Montirovanie
// ============================================================================

void fat32_init(void) {
    for (int i = 0; i < FAT32_MAX_INSTANCES; i++) {
        fat_fs_pool[i].mounted = false;
        fat_fs_pool[i].node_count = 0;
    }
    fat_fs_count = 0;
    g_fs = &fat_fs_pool[0];
}

vfs_node_t* fat32_mount(uint8_t drive, uint32_t partition_lba) {
    if (!disk_present(drive)) return NULL;
    if (!disk_read_sectors(drive, partition_lba, 1, sector_buf)) return NULL;

    fat32_bpb_t* bpb = (fat32_bpb_t*)sector_buf;
    if (bpb->bytes_per_sector != 512) return NULL;
    if (bpb->num_fats == 0) return NULL;

    if (fat_fs_count >= FAT32_MAX_INSTANCES) return NULL;
    fat32_fs_t* slot = &fat_fs_pool[fat_fs_count++];
    g_fs = slot;
    slot->node_count = 0;

    slot->drive = drive;
    slot->partition_lba = partition_lba;
    slot->bytes_per_sector = bpb->bytes_per_sector;
    slot->sectors_per_cluster = bpb->sectors_per_cluster;
    slot->reserved_sectors = bpb->reserved_sectors;
    slot->num_fats = bpb->num_fats;
    slot->fat_size = bpb->fat_size_32;
    slot->root_cluster = bpb->root_cluster;
    slot->total_sectors = bpb->total_sectors_32;

    slot->fat_start_lba = partition_lba + slot->reserved_sectors;
    slot->data_start_lba = slot->fat_start_lba + (slot->num_fats * slot->fat_size);
    slot->cluster_size = slot->sectors_per_cluster * 512;

    // ИСПРАВЛЕНО (bug fix): без total_clusters alloc_cluster() возвращает 0,
    // и любое создание/расширение файла на FAT32 падает с NOSPC. Из-за этого
    // на этапе 32 БД пользователей /disk/users.db никогда не записывалась,
    // и при перезапуске мастер первого запуска появлялся снова.
    // total_clusters = (всего секторов раздела − reserved − num_fats * fat_size) /
    //                  sectors_per_cluster.
    if (slot->sectors_per_cluster == 0) slot->sectors_per_cluster = 1;
    uint32_t reserved_overhead = slot->reserved_sectors + slot->num_fats * slot->fat_size;
    slot->total_clusters = (slot->total_sectors > reserved_overhead)
        ? (slot->total_sectors - reserved_overhead) / slot->sectors_per_cluster
        : 0;
    const char* disk_name = (drive >= 0xF0) ? "usb" : "disk";
    int n = 0;
    while (disk_name[n] && n < VFS_MAX_FILENAME - 1) {
        slot->root_node.name[n] = disk_name[n];
        n++;
    }
    slot->root_node.name[n] = '\0';

    slot->root_node.flags = VFS_DIRECTORY;
    slot->root_node.impl = slot->root_cluster;
    slot->root_node.read = fat32_read;
    slot->root_node.write = fat32_write;
    slot->root_node.readdir = fat32_readdir;
    slot->root_node.finddir = fat32_finddir;
    slot->root_node.create = fat32_create;
    slot->root_node.mkdir = fat32_mkdir;
    slot->root_node.fs_data = NULL;
    slot->root_node.parent  = NULL;

    slot->mounted = true;
    return &slot->root_node;
}

const fat32_fs_t* fat32_get_info(void) {
    return g_fs;
}

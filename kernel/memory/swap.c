#include "swap.h"
#include "../fs/vfs.h"
#include "../vga.h"
#include "memory.h"
#include <stddef.h>

static int swap_fd = -1;
static uint8_t swap_bitmap[SWAP_MAX_PAGES / 8];
static bool swap_enabled = false;

void swap_init(void) {
    // Пытаемся создать файл подкачки
    swap_fd = vfs_open_file("/disk/pagefile.sys", VFS_O_CREAT | VFS_O_RDWR);
    if (swap_fd < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[SWAP] Oshibka sozdaniya pagefile.sys");
        return;
    }

    // Инициализируем битмап нулями
    for (size_t i = 0; i < sizeof(swap_bitmap); i++) {
        swap_bitmap[i] = 0;
    }

    swap_enabled = true;
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_println("[SWAP] Fайл podkachki inicializirovan (/disk/pagefile.sys)");
}

uint32_t swap_out_page(uint32_t phys_addr) {
    if (!swap_enabled || swap_fd < 0) return 0xFFFFFFFF;

    // Ищем свободный слот
    int slot = -1;
    for (int i = 0; i < SWAP_MAX_PAGES; i++) {
        if (!(swap_bitmap[i / 8] & (1 << (i % 8)))) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        // Swap переполнен
        return 0xFFFFFFFF;
    }

    // Занимаем слот
    swap_bitmap[slot / 8] |= (1 << (slot % 8));

    // Пишем в файл
    vfs_seek_file(swap_fd, slot * SWAP_PAGE_SIZE, VFS_SEEK_SET);
    vfs_write_file(swap_fd, (void*)phys_addr, SWAP_PAGE_SIZE);

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("[SWAP] Stranitsa vyigruzhena v swap, slot: ");
    vga_print_dec(slot);
    vga_println("");

    return (uint32_t)slot;
}

int swap_in_page(uint32_t swap_index, uint32_t phys_addr) {
    if (!swap_enabled || swap_fd < 0 || swap_index >= SWAP_MAX_PAGES) return -1;
    
    // Проверяем, занят ли слот
    if (!(swap_bitmap[swap_index / 8] & (1 << (swap_index % 8)))) {
        return -1;
    }

    // Читаем из файла
    vfs_seek_file(swap_fd, swap_index * SWAP_PAGE_SIZE, VFS_SEEK_SET);
    vfs_read_file(swap_fd, (void*)phys_addr, SWAP_PAGE_SIZE);

    // Освобождаем слот
    swap_bitmap[swap_index / 8] &= ~(1 << (swap_index % 8));

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("[SWAP] Stranitsa zagruzhena iz swap, slot: ");
    vga_print_dec(swap_index);
    vga_println("");

    return 0;
}

void swap_free_slot(uint32_t swap_index) {
    if (swap_index < SWAP_MAX_PAGES) {
        swap_bitmap[swap_index / 8] &= ~(1 << (swap_index % 8));
    }
}

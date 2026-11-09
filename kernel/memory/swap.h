#ifndef SWAP_H
#define SWAP_H

#include <stdint.h>
#include <stdbool.h>

#define SWAP_PAGE_SIZE 4096
#define SWAP_MAX_PAGES 1024 // 4 MB of swap

// Инициализация подсистемы Swap
void swap_init(void);

// Выгрузить страницу в Swap (возвращает индекс слота или 0xFFFFFFFF при ошибке)
uint32_t swap_out_page(uint32_t phys_addr);

// Загрузить страницу из Swap (возвращает 0 при успехе)
int swap_in_page(uint32_t swap_index, uint32_t phys_addr);

// Освободить слот в Swap (без загрузки)
void swap_free_slot(uint32_t swap_index);

#endif

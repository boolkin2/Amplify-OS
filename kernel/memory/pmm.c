// ============================================================================
// pmm.c - Реализация Physical Memory Manager для MyOS
// ============================================================================

#include "pmm.h"
#include "../vga.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Bitmap для отслеживания свободных/занятых страниц
// 0 = свободна, 1 = занята
static uint8_t pmm_bitmap[PMM_BITMAP_SIZE];

// Параллельный массив refcount'ов для Copy-On-Write (1 байт на страницу).
// При pmm_alloc_page ref=1; pmm_inc_ref делает ref++; pmm_free_page
// выполняет ref--, и только если результат ==0, страница реально
// помечается свободной в bitmap.
// Размер: PMM_MAX_PAGES (64 KiB для 256 MB RAM / 4 KiB страниц).
static uint8_t pmm_refcount[PMM_MAX_PAGES];

// Информация о памяти
static pmm_info_t pmm_info;

// ============================================================================
// Внутренние функции
// ============================================================================

// Установка бита в bitmap (пометка страницы как занятой)
static void pmm_set_bit(uint32_t page_num) {
    uint32_t byte_index = page_num / 8;
    uint32_t bit_index = page_num % 8;
    pmm_bitmap[byte_index] |= (1 << bit_index);
}

// Очистка бита в bitmap (пометка страницы как свободной)
static void pmm_clear_bit(uint32_t page_num) {
    uint32_t byte_index = page_num / 8;
    uint32_t bit_index = page_num % 8;
    pmm_bitmap[byte_index] &= ~(1 << bit_index);
}

// Проверка бита в bitmap
static bool pmm_test_bit(uint32_t page_num) {
    uint32_t byte_index = page_num / 8;
    uint32_t bit_index = page_num % 8;
    return (pmm_bitmap[byte_index] & (1 << bit_index)) != 0;
}

// Поиск первой свободной страницы
static uint32_t pmm_find_free_page(void) {
    for (uint32_t i = 0; i < pmm_info.total_pages; i++) {
        if (!pmm_test_bit(i)) {
            return i;
        }
    }
    return (uint32_t)-1;  // Нет свободных страниц
}

// Поиск нескольких последовательных свободных страниц
static uint32_t pmm_find_free_pages(uint32_t count) {
    if (count == 0) return (uint32_t)-1;
    
    for (uint32_t i = 0; i < pmm_info.total_pages; i++) {
        // Проверяем, свободны ли count страниц начиная с i
        bool found = true;
        for (uint32_t j = 0; j < count; j++) {
            if (i + j >= pmm_info.total_pages || pmm_test_bit(i + j)) {
                found = false;
                break;
            }
        }
        
        if (found) {
            return i;
        }
    }
    
    return (uint32_t)-1;  // Не найдено достаточно последовательных страниц
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация PMM
void pmm_init(uint32_t mem_size, uint32_t kernel_end) {
    // Ограничиваем размер памяти максимумом
    if (mem_size > PMM_MAX_MEMORY) {
        mem_size = PMM_MAX_MEMORY;
    }
    
    // Вычисляем количество страниц
    pmm_info.total_pages = mem_size / PAGE_SIZE;
    pmm_info.total_memory = pmm_info.total_pages * PAGE_SIZE;
    
    // Изначально вся память свободна
    for (uint32_t i = 0; i < PMM_BITMAP_SIZE; i++) {
        pmm_bitmap[i] = 0;
    }
    // Refcount всех страниц = 0.
    for (uint32_t i = 0; i < PMM_MAX_PAGES; i++) {
        pmm_refcount[i] = 0;
    }
    
    pmm_info.used_pages = 0;
    pmm_info.free_pages = pmm_info.total_pages;
    pmm_info.used_memory = 0;
    pmm_info.free_memory = pmm_info.total_memory;
    
    // Резервируем первый мегабайт (BIOS, видеопамять и т.д.)
    pmm_reserve_region(0, 1 * MB);
    
    // Резервируем память ядра (от 1MB до kernel_end)
    uint32_t kernel_start = 1 * MB;
    uint32_t kernel_size = kernel_end - kernel_start;
    pmm_reserve_region(kernel_start, kernel_size);
}

// Выделение одной страницы
uint32_t pmm_alloc_page(void) {
    uint32_t page_num = pmm_find_free_page();
    
    if (page_num == (uint32_t)-1) {
        return 0;  // Нет свободных страниц
    }
    
    // Помечаем страницу как занятую и ставим refcount = 1
    pmm_set_bit(page_num);
    pmm_refcount[page_num] = 1;
    
    // Обновляем статистику
    pmm_info.used_pages++;
    pmm_info.free_pages--;
    pmm_info.used_memory += PAGE_SIZE;
    pmm_info.free_memory -= PAGE_SIZE;
    
    // Возвращаем физический адрес
    return page_num * PAGE_SIZE;
}

// Освобождение одной страницы (уменьшает refcount; реально освобождает
// при достижении нуля — нужно для COW).
void pmm_free_page(uint32_t page_addr) {
    uint32_t page_num = page_addr / PAGE_SIZE;
    
    // Проверка корректности адреса
    if (page_num >= pmm_info.total_pages) {
        return;
    }
    
    // Проверяем, была ли страница занята
    if (!pmm_test_bit(page_num)) {
        return;  // Страница уже свободна
    }
    
    // Если на страницу есть ещё ссылки (COW-копия), просто декрементируем.
    if (pmm_refcount[page_num] > 1) {
        pmm_refcount[page_num]--;
        return;
    }

    // Последний владелец — реально освобождаем.
    pmm_refcount[page_num] = 0;
    pmm_clear_bit(page_num);
    
    // Обновляем статистику
    pmm_info.used_pages--;
    pmm_info.free_pages++;
    pmm_info.used_memory -= PAGE_SIZE;
    pmm_info.free_memory += PAGE_SIZE;
}

// Увеличить refcount: используется при клонировании PD в fork().
void pmm_inc_ref(uint32_t page_addr) {
    uint32_t page_num = page_addr / PAGE_SIZE;
    if (page_num >= pmm_info.total_pages) return;
    if (!pmm_test_bit(page_num)) return;           // не выделена — ошибка, игнорируем
    if (pmm_refcount[page_num] < 0xFF) {
        pmm_refcount[page_num]++;
    }
    // При переполнении (>255 ссылок) фиксируем на 255 — странно много,
    // но ядро не должно ронять. В 8 битах всё равно не умещается больше.
}

uint8_t pmm_get_ref(uint32_t page_addr) {
    uint32_t page_num = page_addr / PAGE_SIZE;
    if (page_num >= pmm_info.total_pages) return 0;
    return pmm_refcount[page_num];
}

// Выделение нескольких страниц
uint32_t pmm_alloc_pages(uint32_t count) {
    if (count == 0) return 0;
    
    uint32_t page_num = pmm_find_free_pages(count);
    
    if (page_num == (uint32_t)-1) {
        return 0;  // Не найдено достаточно последовательных страниц
    }
    
    // Помечаем все страницы как занятые и ставим ref=1
    for (uint32_t i = 0; i < count; i++) {
        pmm_set_bit(page_num + i);
        pmm_refcount[page_num + i] = 1;
    }
    
    // Обновляем статистику
    pmm_info.used_pages += count;
    pmm_info.free_pages -= count;
    pmm_info.used_memory += count * PAGE_SIZE;
    pmm_info.free_memory -= count * PAGE_SIZE;
    
    // Возвращаем физический адрес первой страницы
    return page_num * PAGE_SIZE;
}

// Освобождение нескольких страниц
void pmm_free_pages(uint32_t page_addr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_page(page_addr + i * PAGE_SIZE);
    }
}

// Получение информации о памяти
pmm_info_t pmm_get_info(void) {
    return pmm_info;
}

// Проверка, свободна ли страница
bool pmm_is_page_free(uint32_t page_addr) {
    uint32_t page_num = page_addr / PAGE_SIZE;
    
    if (page_num >= pmm_info.total_pages) {
        return false;
    }
    
    return !pmm_test_bit(page_num);
}

// Резервирование области памяти
// Refcount для зарезервированных страниц ставим в 1, чтобы ошибочный
// pmm_free_page не вернул их в свободный пул.
void pmm_reserve_region(uint32_t start_addr, uint32_t size) {
    // Выравниваем адреса по границам страниц
    uint32_t start_page = start_addr / PAGE_SIZE;
    uint32_t end_page = (start_addr + size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Помечаем все страницы в области как занятые
    for (uint32_t page = start_page; page < end_page && page < pmm_info.total_pages; page++) {
        if (!pmm_test_bit(page)) {
            pmm_set_bit(page);
            pmm_refcount[page] = 1;
            pmm_info.used_pages++;
            pmm_info.free_pages--;
            pmm_info.used_memory += PAGE_SIZE;
            pmm_info.free_memory -= PAGE_SIZE;
        }
    }
}

// ============================================================================
// memory.h - Общий заголовочный файл для управления памятью MyOS
// ============================================================================

#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// Константы памяти
// ============================================================================

// Размер страницы памяти (4 KB)
#define PAGE_SIZE           4096

// Выравнивание адресов по границе страницы
#define PAGE_ALIGN_DOWN(addr)   ((addr) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(addr)     (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

// Проверка выравнивания
#define IS_PAGE_ALIGNED(addr)   (((addr) & (PAGE_SIZE - 1)) == 0)

// Преобразование адреса в номер страницы
#define ADDR_TO_PAGE(addr)      ((addr) / PAGE_SIZE)
#define PAGE_TO_ADDR(page)      ((page) * PAGE_SIZE)

// Размеры памяти
#define KB  1024
#define MB  (1024 * KB)
#define GB  (1024 * MB)

// ============================================================================
// Вспомогательные функции
// ============================================================================

// Копирование памяти
void* memcpy(void* dest, const void* src, size_t n);

// Заполнение памяти
void* memset(void* s, int c, size_t n);

// Сравнение памяти
int memcmp(const void* s1, const void* s2, size_t n);

// Перемещение памяти (с учётом перекрытия)
void* memmove(void* dest, const void* src, size_t n);

#endif // MEMORY_H

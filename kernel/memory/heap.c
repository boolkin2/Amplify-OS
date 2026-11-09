// ============================================================================
// heap.c - Реализация Heap Allocator для MyOS
// ============================================================================

#include "heap.h"
#include "pmm.h"
#include "../vga.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Указатель на первый блок кучи
static heap_block_t* heap_first_block = NULL;

// Статистика кучи
static heap_info_t heap_info;

// ============================================================================
// Внутренние функции
// ============================================================================

// Получение указателя на данные блока (после заголовка)
static inline void* block_to_ptr(heap_block_t* block) {
    return (void*)((uint8_t*)block + sizeof(heap_block_t));
}

// Получение указателя на блок по указателю на данные
static inline heap_block_t* ptr_to_block(void* ptr) {
    return (heap_block_t*)((uint8_t*)ptr - sizeof(heap_block_t));
}

// Разделение блока на два (если блок слишком большой)
static void split_block(heap_block_t* block, size_t size) {
    // Проверяем, достаточно ли места для разделения
    if (block->size < size + sizeof(heap_block_t) + HEAP_MIN_BLOCK_SIZE) {
        return;  // Блок слишком мал для разделения
    }
    
    // Создаём новый блок из оставшейся части
    heap_block_t* new_block = (heap_block_t*)((uint8_t*)block + sizeof(heap_block_t) + size);
    new_block->magic = HEAP_MAGIC;
    new_block->size = block->size - size - sizeof(heap_block_t);
    new_block->is_free = true;
    new_block->next = block->next;
    new_block->prev = block;
    
    // Обновляем связи
    if (block->next) {
        block->next->prev = new_block;
    }
    block->next = new_block;
    block->size = size;
    
    // Обновляем статистику
    heap_info.block_count++;
    heap_info.free_blocks++;
}

// Объединение соседних свободных блоков
static void merge_free_blocks(heap_block_t* block) {
    // Объединяем со следующим блоком, если он свободен
    while (block->next && block->next->is_free) {
        heap_block_t* next = block->next;
        
        // Проверяем магическое число
        if (next->magic != HEAP_MAGIC) {
            break;
        }
        
        // Объединяем блоки
        block->size += sizeof(heap_block_t) + next->size;
        block->next = next->next;
        
        if (next->next) {
            next->next->prev = block;
        }
        
        // Обновляем статистику
        heap_info.block_count--;
        heap_info.free_blocks--;
    }
    
    // Объединяем с предыдущим блоком, если он свободен
    if (block->prev && block->prev->is_free) {
        merge_free_blocks(block->prev);
    }
}

// Поиск свободного блока (First-Fit алгоритм)
static heap_block_t* find_free_block(size_t size) {
    heap_block_t* current = heap_first_block;
    
    while (current) {
        if (current->is_free && current->size >= size) {
            return current;
        }
        current = current->next;
    }
    
    return NULL;  // Не найдено подходящего блока
}

// ============================================================================
// Публичные функции
// ============================================================================

// Инициализация кучи
void heap_init(void) {
    // Резервируем регион heap в PMM, чтобы pmm_alloc_page() никогда не отдал
    // страницы, которые kmalloc считает своими. До этого шага HEAP_START и
    // pmm могли бы одновременно считать одни и те же физические страницы
    // «своими», что приводило к случайным порчам памяти.
    pmm_reserve_region(HEAP_START, HEAP_SIZE);

    // Создаём первый блок, занимающий всю кучу
    heap_first_block = (heap_block_t*)HEAP_START;
    heap_first_block->magic = HEAP_MAGIC;
    heap_first_block->size = HEAP_SIZE - sizeof(heap_block_t);
    heap_first_block->is_free = true;
    heap_first_block->next = NULL;
    heap_first_block->prev = NULL;
    
    // Инициализируем статистику
    heap_info.total_size = HEAP_SIZE;
    heap_info.used_size = 0;
    heap_info.free_size = HEAP_SIZE - sizeof(heap_block_t);
    heap_info.block_count = 1;
    heap_info.free_blocks = 1;
    heap_info.used_blocks = 0;
}

// Выделение памяти
void* kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    
    // Выравниваем размер по 8 байт
    size = (size + 7) & ~7;
    
    // Ищем свободный блок
    heap_block_t* block = find_free_block(size);
    
    if (!block) {
        return NULL;  // Нет свободной памяти
    }
    
    // Разделяем блок, если он слишком большой
    split_block(block, size);
    
    // Помечаем блок как занятый
    block->is_free = false;
    
    // Обновляем статистику
    heap_info.used_size += block->size;
    heap_info.free_size -= block->size;
    heap_info.free_blocks--;
    heap_info.used_blocks++;
    
    // Возвращаем указатель на данные (после заголовка)
    return block_to_ptr(block);
}

// Освобождение памяти
void kfree(void* ptr) {
    if (!ptr) {
        return;
    }
    
    // Получаем блок по указателю
    heap_block_t* block = ptr_to_block(ptr);
    
    // Проверяем магическое число
    if (block->magic != HEAP_MAGIC) {
        return;  // Некорректный указатель
    }
    
    // Проверяем, не был ли блок уже освобождён
    if (block->is_free) {
        return;  // Двойное освобождение
    }
    
    // Помечаем блок как свободный
    block->is_free = true;
    
    // Обновляем статистику
    heap_info.used_size -= block->size;
    heap_info.free_size += block->size;
    heap_info.free_blocks++;
    heap_info.used_blocks--;
    
    // Объединяем с соседними свободными блоками
    merge_free_blocks(block);
}

// Выделение памяти с выравниванием
void* kmalloc_aligned(size_t size, size_t align) {
    if (size == 0 || align == 0) {
        return NULL;
    }
    
    // Проверяем, что align - степень двойки
    if ((align & (align - 1)) != 0) {
        return NULL;
    }
    
    // Выделяем больше памяти для выравнивания
    size_t total_size = size + align + sizeof(heap_block_t);
    void* ptr = kmalloc(total_size);
    
    if (!ptr) {
        return NULL;
    }
    
    // Выравниваем адрес
    uintptr_t addr = (uintptr_t)ptr;
    uintptr_t aligned_addr = (addr + align - 1) & ~(align - 1);
    
    return (void*)aligned_addr;
}

// Выделение и обнуление памяти
void* kcalloc(size_t num, size_t size) {
    size_t total_size = num * size;
    void* ptr = kmalloc(total_size);
    
    if (ptr) {
        // Обнуляем память
        uint8_t* bytes = (uint8_t*)ptr;
        for (size_t i = 0; i < total_size; i++) {
            bytes[i] = 0;
        }
    }
    
    return ptr;
}

// Изменение размера памяти
void* krealloc(void* ptr, size_t new_size) {
    if (!ptr) {
        return kmalloc(new_size);
    }
    
    if (new_size == 0) {
        kfree(ptr);
        return NULL;
    }
    
    // Получаем блок
    heap_block_t* block = ptr_to_block(ptr);
    
    // Проверяем магическое число
    if (block->magic != HEAP_MAGIC) {
        return NULL;
    }
    
    // Если новый размер меньше или равен текущему, ничего не делаем
    if (new_size <= block->size) {
        return ptr;
    }
    
    // Выделяем новый блок
    void* new_ptr = kmalloc(new_size);
    
    if (!new_ptr) {
        return NULL;
    }
    
    // Копируем данные
    uint8_t* src = (uint8_t*)ptr;
    uint8_t* dst = (uint8_t*)new_ptr;
    for (size_t i = 0; i < block->size; i++) {
        dst[i] = src[i];
    }
    
    // Освобождаем старый блок
    kfree(ptr);
    
    return new_ptr;
}

// Получение информации о куче
heap_info_t heap_get_info(void) {
    return heap_info;
}

// Проверка целостности кучи
bool heap_check_integrity(void) {
    heap_block_t* current = heap_first_block;
    
    while (current) {
        // Проверяем магическое число
        if (current->magic != HEAP_MAGIC) {
            return false;
        }
        
        // Проверяем связи
        if (current->next && current->next->prev != current) {
            return false;
        }
        
        current = current->next;
    }
    
    return true;
}

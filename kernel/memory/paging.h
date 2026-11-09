// ============================================================================
// paging.h - Виртуальная память (Paging) для MyOS
// ============================================================================
// Управление страничной адресацией x86:
//   - Page Directory (каталог страниц) — 1024 записи по 4 байта
//   - Page Table (таблица страниц)     — 1024 записи по 4 байта
//   - Каждая страница — 4 KB
//   - Одна Page Table покрывает 4 MB
//   - Один Page Directory покрывает 4 GB
// ============================================================================

#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>
#include <stdbool.h>
#include "memory.h"

// ============================================================================
// Константы постраничной адресации
// ============================================================================

// Количество записей в одной таблице/каталоге
#define PAGE_ENTRIES         1024

// Размер памяти, покрываемый одной Page Table (4 MB)
#define PAGE_TABLE_COVERAGE  (PAGE_ENTRIES * PAGE_SIZE)

// ============================================================================
// Флаги записей Page Directory / Page Table (младшие 12 бит адреса)
// ============================================================================

#define PTE_PRESENT          0x001   // Страница присутствует в памяти
#define PTE_WRITABLE         0x002   // Страница доступна для записи
#define PTE_USER             0x004   // Страница доступна из ring 3
#define PTE_WRITE_THROUGH    0x008   // Сквозная запись кэша
#define PTE_CACHE_DISABLE    0x010   // Отключить кэширование
#define PTE_ACCESSED         0x020   // Страница была прочитана
#define PTE_DIRTY            0x040   // Страница была записана (только PTE)
#define PTE_PAGE_SIZE        0x080   // 4 MB страницы (только PDE)
#define PTE_GLOBAL           0x100   // Глобальная страница (не сбрасывается при переключении CR3)
#define PTE_SWAPPED          0x200   // Страница выгружена в Swap
#define PTE_MMAPPED          0x400   // Страница спроецирована (mmap)
#define PTE_COW              0x800   // Copy-On-Write: читаем общую физ. страницу,
                                     // при записи — #PF, копируем в приватную

// Маска для извлечения физического адреса из записи (старшие 20 бит)
#define PTE_FRAME_MASK       0xFFFFF000

// ============================================================================
// Макросы для работы с адресами
// ============================================================================

// Извлекаем индекс в Page Directory из виртуального адреса (старшие 10 бит)
#define PAGE_DIR_INDEX(vaddr)    (((uint32_t)(vaddr)) >> 22)

// Извлекаем индекс в Page Table из виртуального адреса (средние 10 бит)
#define PAGE_TABLE_INDEX(vaddr)  ((((uint32_t)(vaddr)) >> 12) & 0x3FF)

// Извлекаем смещение внутри страницы (младшие 12 бит)
#define PAGE_OFFSET(vaddr)       (((uint32_t)(vaddr)) & 0xFFF)

// ============================================================================
// Типы данных
// ============================================================================

// Запись в Page Directory или Page Table — это просто 32-битное целое
typedef uint32_t page_entry_t;

// Page Directory — массив из 1024 записей
typedef struct {
    page_entry_t entries[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
} page_directory_t;

// Page Table — массив из 1024 записей
typedef struct {
    page_entry_t entries[PAGE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
} page_table_t;

// Информация о пейджинге
typedef struct {
    bool enabled;               // Пейджинг включён?
    uint32_t page_faults;       // Количество ошибок страниц
    uint32_t pages_mapped;      // Количество отображённых страниц
    page_directory_t* current_dir;  // Текущий каталог страниц
} paging_info_t;

// ============================================================================
// Функции управления пейджингом
// ============================================================================

// Инициализация системы пейджинга
// Создаёт начальный Page Directory с identity mapping первых N MB
void paging_init(void);

// Отображение одной виртуальной страницы на физическую
// Параметры:
//   dir        — каталог страниц
//   virt_addr  — виртуальный адрес (выровнен по 4 KB)
//   phys_addr  — физический адрес (выровнен по 4 KB)
//   flags      — флаги PTE_* (PRESENT, WRITABLE и т.д.)
void paging_map_page(page_directory_t* dir, uint32_t virt_addr,
                     uint32_t phys_addr, uint32_t flags);

// Снятие отображения виртуальной страницы
// Параметры:
//   dir        — каталог страниц
//   virt_addr  — виртуальный адрес
void paging_unmap_page(page_directory_t* dir, uint32_t virt_addr);

// Отображение диапазона адресов (identity mapping или произвольное)
// Параметры:
//   dir        — каталог страниц
//   virt_start — начало виртуального диапазона
//   phys_start — начало физического диапазона
//   size       — размер области в байтах (будет выровнен вверх до страниц)
//   flags      — флаги PTE_*
void paging_map_range(page_directory_t* dir, uint32_t virt_start,
                      uint32_t phys_start, uint32_t size, uint32_t flags);

// Получение физического адреса по виртуальному
// Параметры:
//   dir        — каталог страниц
//   virt_addr  — виртуальный адрес
// Возвращает физический адрес или 0 при ошибке
uint32_t paging_get_physical(page_directory_t* dir, uint32_t virt_addr);

// Проверка, отображена ли страница
// Параметры:
//   dir        — каталог страниц
//   virt_addr  — виртуальный адрес
// Возвращает true если страница отображена
bool paging_is_mapped(page_directory_t* dir, uint32_t virt_addr);

// Переключение на другой каталог страниц
// Параметры:
//   dir — указатель на каталог страниц
void paging_switch_directory(page_directory_t* dir);

// Получение текущего каталога страниц
page_directory_t* paging_get_current_directory(void);

// Выделение страниц памяти для пользователя (eager: сразу pmm_alloc_page)
void paging_alloc_user_page(uint32_t vaddr, uint32_t count);

// Пометить диапазон виртуальных адресов как lazy-mmap: физическая страница
// не выделяется до первого обращения. PTE получают PTE_MMAPPED | PTE_USER |
// нужные биты защиты. Обработчик page-fault выделит zero-filled страницу.
void paging_reserve_lazy_range(uint32_t vaddr, uint32_t count, uint32_t extra_flags);

// Освобождение диапазона виртуальных адресов, возвращая физические страницы
// в PMM (только для PRESENT-страниц). Также освобождает слоты swap для
// PTE_SWAPPED-страниц и просто забывает PTE_MMAPPED-резервации.
void paging_free_user_range(uint32_t vaddr, uint32_t count);

// Создание нового каталога страниц (для нового процесса)
// Клонирует ядерные отображения из текущего каталога
page_directory_t* paging_create_directory(void);

// Клонирование пользовательской части PD с Copy-On-Write.
// Создаёт новый PD с копиями ядерных PDE, затем проходит по user-PTE
// родителя: снимает бит W, ставит PTE_COW в обоих (src и dst), копирует
// фрейм в новый PD, вызывает pmm_inc_ref на физ. странице. Для PTE_SWAPPED
// и PTE_MMAPPED просто копирует запись. Возвращает NULL при OOM.
page_directory_t* paging_clone_user_cow(page_directory_t* src);

// Инвалидация записи TLB для конкретного адреса
// Используется после изменения записей Page Table
void paging_invalidate(uint32_t virt_addr);

// Сброс всего TLB (перезагрузка CR3)
void paging_flush_tlb(void);

// Получение информации о пейджинге
paging_info_t paging_get_info(void);

// Обработчик Page Fault (ISR 14)
// Вызывается из interrupt.c при ошибке страницы
void paging_page_fault_handler(uint32_t fault_addr, uint32_t error_code);

#endif // PAGING_H

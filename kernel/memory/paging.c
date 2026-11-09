// ============================================================================
// paging.c - Реализация виртуальной памяти (Paging) для MyOS
// ============================================================================
// Настройка страничной адресации x86 (32-bit):
//   1. Создаём Page Directory (каталог страниц)
//   2. Создаём Page Table для identity mapping первых 16 MB
//   3. Загружаем адрес Page Directory в CR3
//   4. Включаем пейджинг установкой бита PG в CR0
//
// Identity mapping означает: виртуальный адрес == физический адрес
// Это необходимо, чтобы код продолжил работать после включения пейджинга
// ============================================================================

#include "paging.h"
#include "pmm.h"
#include "swap.h"
#include "../vga.h"
#include "../debug_serial.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Каталог страниц ядра (выровнен по 4 KB)
// Используем статический массив, чтобы гарантировать выравнивание
static page_directory_t kernel_page_directory __attribute__((aligned(PAGE_SIZE)));

// Таблицы страниц для identity mapping первых 128 MB (32 таблицы по 4 MB).
// Покрывает: BIOS, видеопамять, ядро, кучу, ACPI-таблицы QEMU, а также
// достаточный диапазон для pmm_alloc_page() — PMM может выдавать адреса
// во всём доступном RAM (до 256 MB в QEMU), и их нужно уметь читать
// из ядра при разрушении пользовательских адресных пространств.
#define KERNEL_ID_MAP_TABLES  16
static page_table_t kernel_page_tables[KERNEL_ID_MAP_TABLES] __attribute__((aligned(PAGE_SIZE)));

// Указатель на текущий активный каталог страниц
static page_directory_t* current_directory = NULL;

// Статистика пейджинга
static paging_info_t paging_info;

// ============================================================================
// Вспомогательные низкоуровневые функции (работа с регистрами CR0, CR2, CR3)
// ============================================================================

// Загрузка адреса Page Directory в регистр CR3
// CR3 содержит физический адрес текущего Page Directory
static inline void paging_load_cr3(uint32_t page_dir_phys_addr) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(page_dir_phys_addr) : "memory");
}

// Чтение регистра CR3
static inline uint32_t paging_read_cr3(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(val));
    return val;
}

// Включение пейджинга: установка бита PG (бит 31) в регистре CR0
static inline void paging_enable(void) {
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;  // Устанавливаем бит PG (Paging Enable)
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

// Чтение регистра CR0
static inline uint32_t paging_read_cr0(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(val));
    return val;
}

// Чтение регистра CR2 (содержит адрес, вызвавший Page Fault)
static inline uint32_t paging_read_cr2(void) {
    uint32_t val;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(val));
    return val;
}

// ============================================================================
// Вспомогательные функции для работы с таблицами страниц
// ============================================================================

// Получение (или создание) Page Table для заданного виртуального адреса
// Параметры:
//   dir    — каталог страниц
//   vaddr  — виртуальный адрес
//   create — если true, создать новую таблицу при отсутствии
// Возвращает указатель на Page Table или NULL
static page_table_t* get_page_table(page_directory_t* dir, uint32_t vaddr, bool create) {
    uint32_t dir_index = PAGE_DIR_INDEX(vaddr);
    
    // Проверяем, есть ли уже таблица для этого индекса
    if (dir->entries[dir_index] & PTE_PRESENT) {
        // Таблица существует — извлекаем её физический адрес
        uint32_t table_phys = dir->entries[dir_index] & PTE_FRAME_MASK;
        // В identity mapping физический адрес == виртуальному
        return (page_table_t*)table_phys;
    }
    
    if (!create) {
        return NULL;  // Таблица не существует и создавать не нужно
    }
    
    // Выделяем физическую страницу для новой таблицы
    uint32_t new_table_phys = pmm_alloc_page();
    if (new_table_phys == 0) {
        // Не удалось выделить память
        return NULL;
    }
    
    // Заполняем таблицу нулями (все страницы не присутствуют)
    page_table_t* new_table = (page_table_t*)new_table_phys;
    for (int i = 0; i < PAGE_ENTRIES; i++) {
        new_table->entries[i] = 0;
    }
    
    // Добавляем запись в Page Directory
    // Флаги: присутствует, доступна для записи, доступна из user mode
    dir->entries[dir_index] = new_table_phys | PTE_PRESENT | PTE_WRITABLE | PTE_USER;
    
    return new_table;
}

// ============================================================================
// Основные функции пейджинга
// ============================================================================

// Инициализация системы пейджинга
void paging_init(void) {
    // Инициализируем статистику
    paging_info.enabled = false;
    paging_info.page_faults = 0;
    paging_info.pages_mapped = 0;
    paging_info.current_dir = &kernel_page_directory;
    
    // =====================================================================
    // Шаг 1: Очищаем каталог страниц ядра (все записи = 0, не присутствуют)
    // =====================================================================
    for (int i = 0; i < PAGE_ENTRIES; i++) {
        kernel_page_directory.entries[i] = 0;
    }
    
    // =====================================================================
    // Шаг 2: Заполняем таблицы страниц для identity mapping
    // Отображаем первые 16 MB: virt addr == phys addr
    // Это покрывает:
    //   0x00000000 - 0x000FFFFF : IVT, BDA, BIOS (1-й MB)
    //   0x00100000 - 0x001FFFFF : Ядро (загружено по 1 MB)
    //   0x00200000 - 0x003FFFFF : Расширение ядра
    //   0x00400000 - 0x00BFFFFF : Куча ядра (8 MB)
    //   0x000B8000              : VGA видеопамять
    // =====================================================================
    
    for (int table_idx = 0; table_idx < KERNEL_ID_MAP_TABLES; table_idx++) {
        // Заполняем каждую таблицу (1024 записи, покрывает 4 MB)
        for (int page_idx = 0; page_idx < PAGE_ENTRIES; page_idx++) {
            // Физический адрес = (таблица * 4MB) + (страница * 4KB)
            uint32_t phys_addr = (table_idx * PAGE_TABLE_COVERAGE) +
                                 (page_idx * PAGE_SIZE);

            // Запись: физический адрес + флаги (присутствует, записываемая)
            kernel_page_tables[table_idx].entries[page_idx] =
                phys_addr | PTE_PRESENT | PTE_WRITABLE;
        }

        // Регистрируем таблицу в каталоге страниц
        uint32_t table_phys = (uint32_t)&kernel_page_tables[table_idx];
        kernel_page_directory.entries[table_idx] =
            table_phys | PTE_PRESENT | PTE_WRITABLE;
    }

    // Обновляем счётчик отображённых страниц
    paging_info.pages_mapped = KERNEL_ID_MAP_TABLES * PAGE_ENTRIES;  // 16K страниц = 64 MB
    
    // =====================================================================
    // Шаг 3: Загружаем адрес каталога страниц в CR3 и включаем пейджинг
    // =====================================================================
    
    current_directory = &kernel_page_directory;
    
    // Загружаем физический адрес каталога в CR3
    paging_load_cr3((uint32_t)&kernel_page_directory);
    
    // Включаем пейджинг (устанавливаем бит PG в CR0)
    paging_enable();
    
    paging_info.enabled = true;

    // Маппим Local APIC (Этап 21)
    paging_map_page(current_directory, 0xFEE00000, 0xFEE00000, PTE_PRESENT | PTE_WRITABLE | PTE_CACHE_DISABLE);
}

// Отображение одной виртуальной страницы на физическую
void paging_map_page(page_directory_t* dir, uint32_t virt_addr,
                     uint32_t phys_addr, uint32_t flags) {
    // Выравниваем адреса по границам страниц
    virt_addr = PAGE_ALIGN_DOWN(virt_addr);
    phys_addr = PAGE_ALIGN_DOWN(phys_addr);
    
    // Получаем (или создаём) таблицу страниц для этого адреса
    page_table_t* table = get_page_table(dir, virt_addr, true);
    if (!table) {
        // Ошибка: не удалось создать таблицу страниц
        return;
    }
    
    // Индекс страницы в таблице
    uint32_t table_index = PAGE_TABLE_INDEX(virt_addr);
    
    // Устанавливаем запись: физический адрес + флаги
    table->entries[table_index] = phys_addr | (flags & 0xFFF) | PTE_PRESENT;
    
    // Инвалидируем TLB для этого адреса
    paging_invalidate(virt_addr);
    
    // Обновляем статистику
    paging_info.pages_mapped++;
}

// Снятие отображения виртуальной страницы
void paging_unmap_page(page_directory_t* dir, uint32_t virt_addr) {
    virt_addr = PAGE_ALIGN_DOWN(virt_addr);
    
    // Получаем таблицу страниц (не создаём, если нет)
    page_table_t* table = get_page_table(dir, virt_addr, false);
    if (!table) {
        return;  // Таблица не существует — нечего снимать
    }
    
    uint32_t table_index = PAGE_TABLE_INDEX(virt_addr);
    
    // Очищаем запись (страница больше не присутствует)
    table->entries[table_index] = 0;
    
    // Инвалидируем TLB
    paging_invalidate(virt_addr);
    
    // Обновляем статистику
    if (paging_info.pages_mapped > 0) {
        paging_info.pages_mapped--;
    }
}

// Отображение диапазона адресов
void paging_map_range(page_directory_t* dir, uint32_t virt_start,
                      uint32_t phys_start, uint32_t size, uint32_t flags) {
    // Выравниваем адреса
    virt_start = PAGE_ALIGN_DOWN(virt_start);
    phys_start = PAGE_ALIGN_DOWN(phys_start);
    
    // Вычисляем количество страниц
    uint32_t num_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    
    // Отображаем каждую страницу
    for (uint32_t i = 0; i < num_pages; i++) {
        paging_map_page(dir, virt_start + i * PAGE_SIZE,
                        phys_start + i * PAGE_SIZE, flags);
    }
}

// Получение физического адреса по виртуальному
uint32_t paging_get_physical(page_directory_t* dir, uint32_t virt_addr) {
    // Получаем таблицу страниц
    page_table_t* table = get_page_table(dir, virt_addr, false);
    if (!table) {
        return 0;  // Таблица не существует
    }
    
    uint32_t table_index = PAGE_TABLE_INDEX(virt_addr);
    page_entry_t entry = table->entries[table_index];
    
    // Проверяем, присутствует ли страница
    if (!(entry & PTE_PRESENT)) {
        return 0;  // Страница не отображена
    }
    
    // Возвращаем физический адрес + смещение внутри страницы
    return (entry & PTE_FRAME_MASK) | PAGE_OFFSET(virt_addr);
}

// Проверка, отображена ли страница
bool paging_is_mapped(page_directory_t* dir, uint32_t virt_addr) {
    page_table_t* table = get_page_table(dir, virt_addr, false);
    if (!table) {
        return false;
    }
    
    uint32_t table_index = PAGE_TABLE_INDEX(virt_addr);
    return (table->entries[table_index] & PTE_PRESENT) != 0;
}

// Переключение на другой каталог страниц
void paging_switch_directory(page_directory_t* dir) {
    current_directory = dir;
    paging_info.current_dir = dir;
    paging_load_cr3((uint32_t)dir);
}

// Получение текущего каталога страниц
page_directory_t* paging_get_current_directory(void) {
    return current_directory;
}

// Выделение страниц памяти для пользователя (eager: сразу pmm_alloc_page)
void paging_alloc_user_page(uint32_t vaddr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t phys = pmm_alloc_page();
        if (phys) {
            paging_map_page(current_directory, vaddr + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        }
    }
}

// Пометить диапазон VA как lazy-mmap (демонд-пейджинг).
// Ничего из PMM сейчас не берём; PTE получают PTE_MMAPPED+USER+запрошенные
// флаги, но без PTE_PRESENT. Первое обращение вызовет #PF, в handler'е
// выделится обнулённая страница и поставит PTE_PRESENT.
void paging_reserve_lazy_range(uint32_t vaddr, uint32_t count, uint32_t extra_flags) {
    uint32_t flags = PTE_MMAPPED | PTE_USER | (extra_flags & (PTE_WRITABLE));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = PAGE_ALIGN_DOWN(vaddr + i * PAGE_SIZE);
        // Создаём page table, если её ещё нет.
        page_table_t* table = get_page_table(current_directory, va, true);
        if (!table) return;  // OOM, дальше тоже не получится
        uint32_t idx = PAGE_TABLE_INDEX(va);
        // Записываем PTE без PTE_PRESENT — это наш «placeholder».
        table->entries[idx] = flags;
        paging_invalidate(va);
    }
}

// Освободить диапазон VA в текущем PD: вернуть физические страницы в PMM
// для PRESENT-PTE, освободить слот swap для SWAPPED-PTE, забыть MMAPPED-PTE.
void paging_free_user_range(uint32_t vaddr, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t va = PAGE_ALIGN_DOWN(vaddr + i * PAGE_SIZE);
        page_table_t* table = get_page_table(current_directory, va, false);
        if (!table) continue;
        uint32_t idx = PAGE_TABLE_INDEX(va);
        page_entry_t pte = table->entries[idx];
        if (pte & PTE_PRESENT) {
            pmm_free_page(pte & PTE_FRAME_MASK);
        } else if (pte & PTE_SWAPPED) {
            uint32_t swap_idx = (pte & PTE_FRAME_MASK) >> 12;
            swap_free_slot(swap_idx);
        }
        // PTE_MMAPPED: физической памяти не выделялось — ничего не освобождаем.
        table->entries[idx] = 0;
        paging_invalidate(va);
    }
}

// Создание нового каталога страниц (для будущих процессов)
// Копирует записи ядра (первые 4 записи) из каталога ядра
page_directory_t* paging_create_directory(void) {
    // Выделяем страницу для нового каталога
    uint32_t phys = pmm_alloc_page();
    if (phys == 0) {
        return NULL;
    }
    
    page_directory_t* dir = (page_directory_t*)phys;
    
    // Очищаем все записи
    for (int i = 0; i < PAGE_ENTRIES; i++) {
        dir->entries[i] = 0;
    }
    
    // Копируем все ядерные PDE: identity-map + любые отдельные маппинги
    // (LAPIC на 0xFEE00000 и т.п.). Это гарантирует, что код ядра и
    // MMIO-регионы доступны из любого адресного пространства, в том числе
    // при обработке прерываний в контексте пользовательского процесса.
    // Пользовательский диапазон PDE (USER_APP_BASE..USER_STACK_TOP,
    // 0x40000000..0xBFFFFFFF) в kernel_pd всегда пуст — исключать его
    // специально не требуется, но для чистоты: копируем только PDE,
    // которые реально присутствуют (PTE_PRESENT).
    for (int i = 0; i < PAGE_ENTRIES; i++) {
        page_entry_t pde = kernel_page_directory.entries[i];
        if (pde & PTE_PRESENT) {
            dir->entries[i] = pde;
        }
    }
    
    return dir;
}

// Forward-declaration: определение ниже. Нужно для paging_clone_user_cow
// (откат при OOM после частичной инициализации dst-PD).
void paging_destroy_user_directory(page_directory_t* dir);

// ============================================================================
// Клонирование пользовательской части PD с Copy-On-Write
// ============================================================================
// Алгоритм:
//   1. Создаём новый PD через paging_create_directory() — он уже содержит
//      копии ядерных PDE (identity-map + LAPIC).
//   2. Для каждого пользовательского PDE родителя (тот, что НЕ совпадает
//      с соответствующим PDE ядра):
//      a) Выделяем новую страницу под page table в child.
//      b) Для каждой записи PTE:
//         - PRESENT  → снимаем W, ставим COW в обоих (src и dst);
//                      dst получает тот же phys; pmm_inc_ref.
//         - SWAPPED  → swap-in в обоих процессах не разделяется, упрощаем:
//                      просто копируем запись (оба указывают на тот же
//                      swap-слот). Корректно, пока ни один не пишет;
//                      при #PF swap-in любой стороны получит содержимое,
//                      а слот станет одиночным — этот случай сейчас даёт
//                      копию данных, но оба потеряют связь с тем же
//                      логическим состоянием. Разумный trade-off для
//                      учебного ядра; полноценно — отдельный refcount
//                      на swap-слоты (TODO в будущем).
//         - MMAPPED  → лени-аллоцированная страница, просто копируем
//                      запись: child самостоятельно материализует
//                      zero-page при первом обращении.
//         - 0        → оставляем 0.
page_directory_t* paging_clone_user_cow(page_directory_t* src) {
    if (!src) return NULL;

    page_directory_t* dst = paging_create_directory();
    if (!dst) return NULL;

    for (int i = 0; i < PAGE_ENTRIES; i++) {
        page_entry_t src_pde = src->entries[i];
        if (!(src_pde & PTE_PRESENT)) continue;
        // Разделяемая ядерная PDE (уже скопирована create_directory) —
        // ничего не делаем, пропускаем.
        if (kernel_page_directory.entries[i] == src_pde) continue;

        // Пользовательский PDE: создаём новый page table в dst.
        uint32_t dst_pt_phys = pmm_alloc_page();
        if (!dst_pt_phys) {
            paging_destroy_user_directory(dst);
            return NULL;
        }
        page_table_t* dst_pt = (page_table_t*)dst_pt_phys;
        for (int j = 0; j < PAGE_ENTRIES; j++) dst_pt->entries[j] = 0;

        page_table_t* src_pt = (page_table_t*)(src_pde & PTE_FRAME_MASK);

        for (int j = 0; j < PAGE_ENTRIES; j++) {
            page_entry_t pte = src_pt->entries[j];
            if (pte == 0) continue;

            if (pte & PTE_PRESENT) {
                // COW: убираем W, ставим COW в обоих. Физ. страница общая.
                uint32_t frame = pte & PTE_FRAME_MASK;
                uint32_t new_flags = (pte & 0xFFFu);
                new_flags &= ~(uint32_t)PTE_WRITABLE;   // read-only
                new_flags |= PTE_COW;

                src_pt->entries[j] = frame | new_flags;
                dst_pt->entries[j] = frame | new_flags;
                pmm_inc_ref(frame);

                // Инвалидируем TLB на родительской стороне
                // (адрес вычисляется из индексов PD/PT).
                uint32_t va = ((uint32_t)i << 22) | ((uint32_t)j << 12);
                paging_invalidate(va);
            } else {
                // SWAPPED / MMAPPED / др. не-PRESENT состояния — копируем как есть.
                dst_pt->entries[j] = pte;
            }
        }

        // Копируем PDE в dst с правами USER|WRITABLE (на уровне PDE это ок;
        // защита на уровне PTE).
        dst->entries[i] = dst_pt_phys | (src_pde & 0xFFFu);
    }

    return dst;
}

// ============================================================================
// Разрушение пользовательского PD
// ============================================================================
// Освобождает:
//   1. Все физические страницы пользовательских PTE (USER | PRESENT)
//   2. Все пользовательские page tables (индексы, не входящие в kernel_pd)
//   3. Сам page directory
// НЕ трогает ядерные отображения (они продолжают жить в kernel_pd).
//
// Вызывается из vm_release() когда refcount vm_space достиг 0.
void paging_destroy_user_directory(page_directory_t* dir) {
    if (!dir) return;

    for (int i = 0; i < PAGE_ENTRIES; i++) {
        page_entry_t pde = dir->entries[i];
        if (!(pde & PTE_PRESENT)) continue;

        // Если этот PDE присутствует и в ядерном каталоге — он разделяется,
        // освобождать его нельзя.
        if (kernel_page_directory.entries[i] == pde) continue;

        // Пользовательский page table — обходим и освобождаем его страницы.
        uint32_t table_phys = pde & PTE_FRAME_MASK;
        page_table_t* table = (page_table_t*)table_phys;

        for (int j = 0; j < PAGE_ENTRIES; j++) {
            page_entry_t pte = table->entries[j];
            if (pte & PTE_PRESENT) {
                pmm_free_page(pte & PTE_FRAME_MASK);
            } else if (pte & PTE_SWAPPED) {
                // Страница жила в swap — освобождаем слот swap.
                uint32_t swap_idx = (pte & PTE_FRAME_MASK) >> 12;
                swap_free_slot(swap_idx);
            }
            table->entries[j] = 0;
        }

        // Сам page table тоже был выделен из PMM.
        pmm_free_page(table_phys);
        dir->entries[i] = 0;
    }

    // Каталог тоже выделялся через pmm_alloc_page() в paging_create_directory().
    pmm_free_page((uint32_t)dir);
}

// Инвалидация записи TLB для конкретного адреса
// Инструкция INVLPG сбрасывает кэш трансляции для одного виртуального адреса
void paging_invalidate(uint32_t virt_addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virt_addr) : "memory");
}

// Сброс всего TLB (перезагрузка CR3)
void paging_flush_tlb(void) {
    uint32_t cr3 = paging_read_cr3();
    paging_load_cr3(cr3);
}

// Получение информации о пейджинге
paging_info_t paging_get_info(void) {
    return paging_info;
}

// ============================================================================
// Обработчик Page Fault (исключение #14)
// ============================================================================
// Вызывается когда процессор пытается обратиться к странице, которая:
//   - Не присутствует в памяти (бит P = 0)
//   - Имеет недостаточные права доступа
//   - Зарезервирована (запись в read-only страницу)
//
// Регистр CR2 содержит виртуальный адрес, вызвавший ошибку
// Код ошибки содержит причину:
//   Бит 0: 0 = страница не присутствует, 1 = нарушение защиты
//   Бит 1: 0 = чтение, 1 = запись
//   Бит 2: 0 = супервизор, 1 = пользователь
// ============================================================================
void paging_page_fault_handler(uint32_t fault_addr, uint32_t error_code) {
    // Увеличиваем счётчик ошибок
    paging_info.page_faults++;

    page_table_t* table = get_page_table(paging_get_current_directory(),
                                         PAGE_ALIGN_DOWN(fault_addr), false);
    uint32_t table_index = table ? PAGE_TABLE_INDEX(fault_addr) : 0;
    page_entry_t entry = table ? table->entries[table_index] : 0;

    // ------------------------------------------------------------------
    // Copy-On-Write: protection violation (bit 0 = 1) + write (bit 1 = 1).
    // Страница присутствует, но read-only с флагом PTE_COW — это fork'нутая
    // страница, которую сейчас пишет один из владельцев.
    // ------------------------------------------------------------------
    if ((error_code & 0x1) && (error_code & 0x2) && table && (entry & PTE_COW)) {
        uint32_t old_frame = entry & PTE_FRAME_MASK;
        uint32_t orig_flags = entry & 0xFFFu;
        orig_flags &= ~(uint32_t)PTE_COW;
        orig_flags |= PTE_WRITABLE;  // восстанавливаем право записи

        if (pmm_get_ref(old_frame) <= 1) {
            // Единственный владелец — никого копировать не нужно,
            // просто разрешаем запись.
            table->entries[table_index] = old_frame | orig_flags;
        } else {
            // Делим страницу с кем-то — делаем приватную копию.
            uint32_t new_frame = pmm_alloc_page();
            if (!new_frame) goto fatal;    // OOM — fatal
            // memcpy 4 KiB: оба адреса в identity-map ядра.
            uint32_t* dst = (uint32_t*)new_frame;
            uint32_t* src = (uint32_t*)old_frame;
            for (int k = 0; k < 1024; k++) dst[k] = src[k];

            // Старая страница теряет одну ссылку; если это была последняя
            // ссылка (маловероятно при COW — минимум 2), pmm_free_page
            // реально её освободит.
            pmm_free_page(old_frame);

            table->entries[table_index] = new_frame | orig_flags;
        }
        paging_invalidate(fault_addr);
        return;
    }

    // Проверяем, не вызвана ли ошибка обращением к Swap или mmap-placeholder.
    if (!(error_code & 0x1)) { // Страница не присутствует
        if (table) {
            // entry уже прочитан выше
            if (entry & PTE_SWAPPED) {
                // Страница в Swap — загружаем обратно.
                // В PTE хранятся: (swap_idx << 12) | original_flags | PTE_SWAPPED.
                uint32_t swap_index = (entry & PTE_FRAME_MASK) >> 12;
                uint32_t orig_flags = entry & 0xFFFu;
                // Маскируем: убираем флаг SWAPPED, оставляем остальные (W/U/...).
                orig_flags &= ~(uint32_t)PTE_SWAPPED;
                
                uint32_t new_phys = pmm_alloc_page();
                if (new_phys) {
                    if (swap_in_page(swap_index, new_phys) == 0) {
                        // Восстанавливаем PTE с оригинальными правами защиты.
                        table->entries[table_index] = new_phys | orig_flags | PTE_PRESENT;
                        paging_invalidate(fault_addr);
                        return;
                    }
                    // swap_in не сработал — вернём страницу обратно в PMM.
                    pmm_free_page(new_phys);
                }
            }

            if (entry & PTE_MMAPPED) {
                // Lazy-allocated mmap-страница: резервирована, но физической
                // памяти нет. Выделяем zero-filled страницу и маппим.
                uint32_t orig_flags = entry & 0xFFFu;
                orig_flags &= ~(uint32_t)PTE_MMAPPED;
                
                uint32_t new_phys = pmm_alloc_page();
                if (new_phys) {
                    // Обнуляем физическую страницу (она identity-mapped в ядре).
                    uint32_t* p = (uint32_t*)new_phys;
                    for (int i = 0; i < 1024; i++) p[i] = 0;

                    table->entries[table_index] = new_phys | orig_flags | PTE_PRESENT;
                    paging_invalidate(fault_addr);
                    return;
                }
            }
        }
    }

    // Если мы дошли сюда, значит это настоящий Page Fault
fatal:
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_println("");
    vga_println("========================================");
    vga_println("  PAGE FAULT (Oshibka stranitsiy #14)  ");
    vga_println("========================================");
    
    // Адрес, вызвавший ошибку
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  Adres: ");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print_hex(fault_addr);
    vga_println("");
    
    // Разбираем код ошибки
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  Prichina: ");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    
    if (!(error_code & 0x1)) {
        vga_puts("Stranitsa otsutstvuet");
    } else {
        vga_puts("Narushenie zashchity");
    }
    vga_println("");
    
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  Operatsiya: ");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    if (error_code & 0x2) {
        vga_puts("Zapis'");
    } else {
        vga_puts("Chtenie");
    }
    vga_println("");
    
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  Rezhim: ");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    if (error_code & 0x4) {
        vga_puts("Pol'zovatel'skij (Ring 3)");
    } else {
        vga_puts("Yadro (Ring 0)");
    }
    vga_println("");
    
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("  Kod oshibki: ");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_print_hex(error_code);
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_println("========================================");
    vga_println("  Sistema ostanovlena.");
    vga_println("========================================");
    
    // Останавливаем процессор — Page Fault в ядре фатален
    while (1) {
        __asm__ volatile ("cli; hlt");
    }
}

// Выгрузка страницы виртуальной памяти в Swap.
// Сохраняет оригинальные биты защиты (W/U/…) в младших 12 битах PTE,
// чтобы при swap-in корректно восстановить их (например, RO ELF-код
// остался RO, а не стал RW).
bool paging_swap_page(uint32_t virt_addr) {
    virt_addr = PAGE_ALIGN_DOWN(virt_addr);
    
    page_table_t* table = get_page_table(current_directory, virt_addr, false);
    if (!table) return false;
    
    uint32_t table_index = PAGE_TABLE_INDEX(virt_addr);
    page_entry_t entry = table->entries[table_index];
    
    if (!(entry & PTE_PRESENT)) return false; // Уже выгружена или не существует
    
    uint32_t phys_addr = entry & PTE_FRAME_MASK;
    uint32_t orig_flags = entry & 0xFFFu;
    
    uint32_t swap_idx = swap_out_page(phys_addr);
    if (swap_idx == 0xFFFFFFFF) return false; // Ошибка swap
    
    // Освобождаем физическую страницу
    pmm_free_page(phys_addr);
    
    // Формируем новый PTE:
    //   верхние 20 бит — индекс слота в swap
    //   нижние 12 бит  — оригинальные флаги, но с PTE_PRESENT=0 и PTE_SWAPPED=1
    uint32_t new_flags = (orig_flags & ~(uint32_t)PTE_PRESENT) | PTE_SWAPPED;
    table->entries[table_index] = (swap_idx << 12) | new_flags;
    
    paging_invalidate(virt_addr);
    return true;
}

// ============================================================================
// task.c - Реализация многозадачности для MyOS (Этап 18)
// ============================================================================
// Расширена загрузкой ELF-программ через полноценный ELF-парсер.
// ============================================================================

#include "task.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../vga.h"
#include "../arch/i386/gdt.h"
#include "../fs/vfs.h"
#include "../memory/paging.h"
#include "../memory/vm.h"
#include "../arch/i386/user_mode.h"
#include "../loader/elf.h"
#include "../debug_serial.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Текущая выполняемая задача
static task_t* current_task = NULL;

// Указатель на начало списка задач
static task_t* task_list_head = NULL;

// Счётчик ID задач
static uint32_t next_task_id = 0;

// Флаг инициализации многозадачности
static bool multitasking_enabled = false;

// Более не используем глобалы для передачи entry/stack новым задачам:
// вместо этого храним поля start_entry/start_stack/start_arg в task_t,
// которые читаются соответствующим stub'ом один раз при первом запуске.
// Это закрывает race-условие: если два task_exec_elf() подряд вызывались
// до того, как первая задача успела стартовать, глобалы перезаписывались.

// ============================================================================
// Внутренние функции
// ============================================================================

// Добавление задачи в кольцевой список
static void append_task(task_t* task) {
    if (!task_list_head) {
        task_list_head = task;
        task->next = task; // Кольцевая ссылка на себя
    } else {
        // Ищем последнюю задачу
        task_t* last = task_list_head;
        while (last->next != task_list_head) {
            last = last->next;
        }
        last->next = task;
        task->next = task_list_head;
    }
}

// Заглушка, если задача возвращается из функции (выход из задачи)
static void task_exit_stub(void) {
    task_exit_current();
}

// ============================================================================
// Публичные функции
// ============================================================================

// Простейшие string-хелперы, чтобы не тянуть libc в task.c.
static void task_strcpy_n(char* dst, const char* src, size_t maxlen) {
    size_t i = 0;
    if (maxlen == 0) return;
    for (; i + 1 < maxlen && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

// Инициализация системы многозадачности
void task_init(void) {
    // Выделяем память под структуру текущей (главной) задачи
    task_t* main_task = (task_t*)kmalloc(sizeof(task_t));

    main_task->id = next_task_id++;
    main_task->state = TASK_RUNNING;
    main_task->stack_base = 0; // Стек главной задачи настроен загрузчиком
    main_task->esp = 0;        // Обновляется при переключении контекста
    main_task->vm = vm_kernel();  // Главная задача живёт в ядерном адресном пространстве
    task_strcpy_n(main_task->cwd, "/", TASK_CWD_MAX);
    task_strcpy_n(main_task->name, "kernel", TASK_NAME_MAX + 1);
    main_task->cpu_ticks = 0;
    main_task->next = main_task;

    current_task = main_task;
    task_list_head = main_task;

    multitasking_enabled = true;
}

// Создание новой задачи (потока ядра)
task_t* task_create(void (*entry)(void)) {
    task_t* task = (task_t*)kmalloc(sizeof(task_t));

    // Выделяем 4KB под стек задачи
    uint32_t stack_base = (uint32_t)kmalloc(TASK_STACK_SIZE);

    // Стек растет вниз, так что вершина = база + размер
    uint32_t stack_top = stack_base + TASK_STACK_SIZE;

    // Подготавливаем стек для переключения контекста:
    // Нам нужно сымитировать то, что оставляет прерывание (PUSHA + iret контекст)
    // Структура registers_t должна лежать на вершине стека

    stack_top -= sizeof(registers_t);
    registers_t* regs = (registers_t*)stack_top;

    // Обнуляем все регистры
    memset(regs, 0, sizeof(registers_t));

    // Настраиваем нужные регистры для старта задачи
    regs->eip = (uint32_t)entry;     // Инструкция, с которой начнется выполнение
    regs->cs = 0x08;                 // Сегмент кода ядра
    regs->ds = 0x10;                 // Сегмент данных ядра

    // EFLAGS: IF=1 (бит 9, включить прерывания), бит 1 всегда 1
    regs->eflags = 0x202;

    // Настройка адреса возврата (если функция задачи завершится)
    // Кладём адрес task_exit_stub над регистрами
    uint32_t* return_addr = (uint32_t*)(stack_top - 4);
    *return_addr = (uint32_t)task_exit_stub;

    // Указатель стека указывает на структуру регистров (как требует наш irq_common_stub)
    task->esp = stack_top;

    task->stack_base = stack_base;
    task->id = next_task_id++;
    task->state = TASK_READY;
    // По умолчанию — ядерное vm. Для пользовательских процессов/потоков
    // вызывающий код перезаписывает поле на нужное vm_space (с retain).
    task->vm = vm_retain(vm_kernel());
    // cwd наследуется от создателя, если он есть; иначе — "/".
    if (current_task) {
        task_strcpy_n(task->cwd, current_task->cwd, TASK_CWD_MAX);
    } else {
        task_strcpy_n(task->cwd, "/", TASK_CWD_MAX);
    }
    // Дефолтное имя — «thread»; вызывающие могут переопределить через
    // task_set_name() (например task_exec_app пишет имя бинарника).
    task_strcpy_n(task->name, "thread", TASK_NAME_MAX + 1);
    task->cpu_ticks = 0;

    // Добавляем в очередь планировщика
    append_task(task);

    return task;
}

// Заглушка для входа в User Mode (старый формат flat binary)
static void task_user_entry_stub(void) {
    // В этот момент мы в Ring 0, но это контекст НОВОЙ задачи.
    // EIP = USER_APP_BASE, ESP = USER_STACK_TOP
    jump_to_user_mode(USER_APP_BASE, USER_STACK_TOP);

    // Сюда мы никогда не вернемся
}

// Заглушка для входа в User Mode из ELF (Этап 18).
// Читает entry/stack из task_t текущей задачи (записываются при создании).
static void task_elf_entry_stub(void) {
    uint32_t entry = current_task->start_entry;
    uint32_t stack = current_task->start_stack;

    current_task->start_entry = 0;
    current_task->start_stack = 0;

    jump_to_user_mode(entry, stack);
    // Сюда мы никогда не вернемся
}

// Заглушка для входа нового POSIX-потока (SYS_CLONE).
// Кладёт arg на пользовательский стек и прыгает в entry в Ring 3.
static void task_user_thread_stub(void) {
    uint32_t entry = current_task->start_entry;
    uint32_t stack = current_task->start_stack;
    uint32_t arg   = current_task->start_arg;

    current_task->start_entry = 0;
    current_task->start_stack = 0;
    current_task->start_arg   = 0;

    // Кладём arg на пользовательский стек, плюс псевдо-return (для C-соглашения).
    // Стек растёт вниз; после этого ESP = stack - 8.
    uint32_t* sp = (uint32_t*)stack;
    *(--sp) = arg;          // argument for pthread start_routine
    *(--sp) = 0;            // fake return address (pthread_exit должен быть вызван явно)

    jump_to_user_mode(entry, (uint32_t)sp);
    // сюда не возвращаемся
}

// Создание пользовательского потока в Ring 3.
// Разделяет адресное пространство с родителем (таблицы страниц ядра общие,
// а пользовательские страницы — в том же текущем каталоге страниц).
//
// Параметры:
//   entry           — адрес функции-потока (Ring 3 address)
//   user_stack_top  — верх стека пользователя (уже замапленный пользователем)
//   arg             — аргумент, будет положен на стек перед входом
//
// Возвращает указатель на task_t или NULL при ошибке.
task_t* task_spawn_user_thread(uint32_t entry, uint32_t user_stack_top, uint32_t arg) {
    if (entry == 0 || user_stack_top == 0) return NULL;

    // Создаём задачу с прерываниями выключенными, чтобы корректно
    // перепривязать vm_space без риска переключения между этими шагами.
    __asm__ volatile ("cli");
    task_t* t = task_create(task_user_thread_stub);
    if (t) {
        // Параметры старта — в самой задаче, без глобалов.
        t->start_entry = entry;
        t->start_stack = user_stack_top;
        t->start_arg   = arg;
        // Новый поток делит адресное пространство с родителем: retain родителя,
        // а дефолтное ядерное vm (присвоенное в task_create) release'им.
        vm_space_t* kvm = t->vm;  // это сейчас vm_kernel (retain'нутый в task_create)
        t->vm = vm_retain(current_task->vm);
        vm_release(kvm);
    }
    __asm__ volatile ("sti");
    return t;
}

// ============================================================================
// POSIX fork() — порождение дочернего процесса с COW-копией vm
// ============================================================================
// Создаёт новый task_t с собственным ядерным стеком и собственным vm_space,
// который делит физические страницы с родителем по схеме Copy-On-Write.
// Состояние регистров родителя копируется в ядерный стек ребёнка так,
// чтобы при первом переключении планировщика ребёнок вернулся в user-mode
// на той же инструкции после int 0x80, но с EAX = 0 (возврат fork в ребёнке).
// Родитель получит PID ребёнка в EAX (это делает вызывающий syscall-handler).
task_t* task_fork(registers_t* parent_regs) {
    if (!parent_regs || !current_task) return NULL;

    // 1. Клонируем адресное пространство родителя (COW).
    vm_space_t* child_vm = vm_clone_cow(current_task->vm);
    if (!child_vm) return NULL;

    // 2. Структура задачи ребёнка.
    task_t* child = (task_t*)kmalloc(sizeof(task_t));
    if (!child) {
        vm_release(child_vm);
        return NULL;
    }

    // 3. Ядерный стек ребёнка (для переключения контекста и обработки
    //    будущих прерываний из Ring 3).
    uint32_t stack_base = (uint32_t)kmalloc(TASK_STACK_SIZE);
    if (!stack_base) {
        kfree(child);
        vm_release(child_vm);
        return NULL;
    }
    uint32_t stack_top = stack_base + TASK_STACK_SIZE;

    // 4. Подготавливаем registers_t на вершине стека ребёнка.
    //    Копия состояния родителя из стека syscall-обработчика.
    //    Изменяем только EAX: ребёнок должен увидеть возврат 0 из fork.
    stack_top -= sizeof(registers_t);
    registers_t* child_regs = (registers_t*)stack_top;
    *child_regs = *parent_regs;
    child_regs->eax = 0;

    // 5. Заполняем поля task_t.
    child->esp        = stack_top;                  // указатель на registers_t
    child->stack_base = stack_base;
    child->id         = next_task_id++;
    child->state      = TASK_READY;
    child->vm         = child_vm;
    // cwd наследуется от родителя.
    task_strcpy_n(child->cwd, current_task->cwd, TASK_CWD_MAX);

    // 6. Добавляем в кольцо планировщика. Делаем это атомарно.
    __asm__ volatile ("cli");
    append_task(child);
    __asm__ volatile ("sti");

    return child;
}

// ============================================================================
// POSIX execve(path) — заменить адресное пространство текущей задачи
// ============================================================================
// Алгоритм:
//   1. Читаем ELF файл в ядерный буфер через VFS.
//   2. Создаём новое vm_space и временно переключаемся в него.
//   3. Загружаем сегменты ELF (elf_load пишет в user-VA в новом vm).
//   4. Выделяем пользовательский стек.
//   5. Модифицируем registers_t на ядерном стеке так, чтобы последующий
//      iret восстановил EIP=entry_point, ESP=user_stack_top, EAX=0,
//      сегменты ring3. После iret процесс начнёт выполняться с _start().
//   6. Заменяем current_task->vm на новое vm и отпускаем старое.
//   7. Возврат 0 при успехе, -1 при ошибке (со всеми откатами).
//
// Важно: вызывающий код (syscall-dispatcher) НЕ должен после этого
// что-либо делать с regs, кроме штатного возврата — isr_common_stub
// сам выполнит iret по нашей подправленной рамке.
int task_execve(const char* path, registers_t* regs) {
    if (!path || !regs || !current_task) return -1;

    // 1. Открыть и прочитать файл.
    int fd = vfs_open_file(path, VFS_O_RDONLY);
    if (fd < 0) return -1;

    int size = vfs_get_filesize(fd);
    if (size <= 0) { vfs_close_file(fd); return -1; }

    uint8_t* buf = (uint8_t*)kmalloc(size);
    if (!buf) { vfs_close_file(fd); return -1; }
    int nread = vfs_read_file(fd, buf, size);
    vfs_close_file(fd);
    if (nread != size) { kfree(buf); return -1; }

    // 2. Валидация — лучше упасть на ошибке до уничтожения старого vm.
    if (!elf_validate(buf, size)) { kfree(buf); return -1; }

    // 3. Новое vm_space для нового образа.
    vm_space_t* new_vm = vm_create_user();
    if (!new_vm) { kfree(buf); return -1; }

    // 4. Переключаемся на новое vm, загружаем в него ELF.
    vm_space_t* old_vm = current_task->vm;   // потом отпустим
    vm_switch(new_vm);

    elf_load_result_t res = elf_load(buf, size);
    kfree(buf);
    if (!res.valid) {
        // Откат: вернуться в старое vm, разрушить новое.
        vm_switch(old_vm);
        vm_release(new_vm);
        return -1;
    }

    // 5. Пользовательский стек в новом vm.
    uint32_t ustack_base = USER_STACK_BASE;
    uint32_t ustack_top  = USER_STACK_TOP;
    if (!paging_is_mapped(new_vm->pd, ustack_base)) {
        paging_alloc_user_page(ustack_base, 1);
    }

    // 6. Модифицируем регистры на ядерном стеке так, чтобы последующий iret
    //    отправил процесс на entry_point в ring3 с чистым стеком.
    regs->eip     = res.entry_point;
    regs->useresp = ustack_top;
    regs->eax     = 0;
    regs->ebx = regs->ecx = regs->edx = 0;
    regs->esi = regs->edi = regs->ebp = 0;
    // CS/SS/DS уже правильные (ring 3), т.к. это была ring3→ring0 транзиция
    // при int 0x80. EFLAGS сохраняется — с IF=1 поток будет прерываемым.

    // 7. Подменяем vm у текущей задачи и отпускаем старое пространство
    //    (refcount--; страницы и PD уничтожаются, если мы были последним).
    __asm__ volatile ("cli");
    current_task->vm = new_vm;
    __asm__ volatile ("sti");
    vm_release(old_vm);

    return 0;
}

// Поиск задачи по id. Возвращает NULL, если не найдена.
task_t* task_find_by_id(uint32_t id) {
    if (!task_list_head) return NULL;
    task_t* t = task_list_head;
    do {
        if (t->id == id) return t;
        t = t->next;
    } while (t != task_list_head);
    return NULL;
}

// Запуск приложения из файла в InitRD (старый способ — flat binary)
task_t* task_exec_app(const char* initrd_path) {
    vfs_node_t* file = vfs_finddir(vfs_get_root(), initrd_path);
    if (!file) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("App not found: ");
        vga_println(initrd_path);
        return NULL;
    }

    uint8_t* buffer = (uint8_t*)kmalloc(file->length);
    if (!buffer) return NULL;

    vfs_read(file, 0, file->length, buffer);

    // Создаём отдельное адресное пространство для нового приложения
    vm_space_t* app_vm = vm_create_user();
    if (!app_vm) {
        kfree(buffer);
        return NULL;
    }

    // Временно переключаемся в новое vm, чтобы мапить и писать в его user-пространство
    vm_space_t* saved_vm = vm_current();
    vm_switch(app_vm);

    // Выделяем память для кода приложения (базовый адрес USER_APP_BASE)
    uint32_t app_pages = (file->length + PAGE_SIZE - 1) / PAGE_SIZE;
    paging_alloc_user_page(USER_APP_BASE, app_pages);

    // Выделяем одну страницу под пользовательский стек (вершина = USER_STACK_TOP)
    paging_alloc_user_page(USER_STACK_BASE, 1);

    // Копируем код приложения в user-VA (сейчас активен app_vm)
    memcpy((void*)USER_APP_BASE, buffer, file->length);
    kfree(buffer);

    // Возвращаемся в исходное vm (ядерное) — иначе родитель потеряет контекст
    vm_switch(saved_vm);

    // Создаём задачу атомарно: task_create ставит vm=kernel, мы переставляем на app_vm.
    __asm__ volatile ("cli");
    task_t* task = task_create(task_user_entry_stub);
    if (task) {
        vm_space_t* kvm = task->vm;
        task->vm = app_vm;   // refcount уже 1 из vm_create_user
        vm_release(kvm);
        task_set_name(task, initrd_path);
    } else {
        vm_release(app_vm);  // откат: задача не создалась, освобождаем vm
    }
    __asm__ volatile ("sti");

    return task;
}

// ============================================================================
// Загрузка и запуск ELF программы из VFS (Этап 18)
// ============================================================================

task_t* task_exec_elf(const char* path) {
    if (!path) return NULL;

    // Открываем файл через VFS
    int fd = vfs_open_file(path, VFS_O_RDONLY);
    if (fd < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("[ELF] Fajl ne najden: ");
        vga_println(path);
        return NULL;
    }

    // Получаем размер файла
    int file_size = vfs_get_filesize(fd);
    if (file_size <= 0) {
        vfs_close_file(fd);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Fajl pustoj");
        return NULL;
    }

    // Читаем файл в буфер
    uint8_t* buffer = (uint8_t*)kmalloc(file_size);
    if (!buffer) {
        vfs_close_file(fd);
        return NULL;
    }

    int bytes_read = vfs_read_file(fd, buffer, file_size);
    vfs_close_file(fd);

    if (bytes_read != file_size) {
        kfree(buffer);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Oshibka chteniya fajla");
        return NULL;
    }

    // Запускаем ELF из памяти
    task_t* task = task_exec_elf_memory(buffer, file_size, path);

    kfree(buffer);
    return task;
}

// ============================================================================
// Загрузка ELF из памяти (Этап 18)
// ============================================================================

task_t* task_exec_elf_memory(const void* data, uint32_t size, const char* name) {
    if (!data || size == 0) return NULL;

    // Валидируем ELF
    if (!elf_validate(data, size)) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_puts("[ELF] Nevalidnyj fajl: ");
        if (name) vga_println(name);
        else vga_println("(bez imeni)");
        return NULL;
    }

    // Выводим информацию о загружаемом ELF
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("[ELF] Zagruzka: ");
    if (name) vga_println(name);
    else vga_println("(programma)");

    // Создаём отдельное адресное пространство для нового процесса.
    // elf_load() пишет сегменты по user-VA через memcpy, поэтому нам
    // нужно временно сделать этот PD активным в CR3.
    vm_space_t* app_vm = vm_create_user();
    if (!app_vm) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Ne udalos' sozdat' vm_space");
        return NULL;
    }

    vm_space_t* saved_vm = vm_current();
    vm_switch(app_vm);

    // Загружаем ELF (парсим программные заголовки, маппим сегменты в app_vm)
    elf_load_result_t result = elf_load(data, size);

    if (!result.valid) {
        vm_switch(saved_vm);
        vm_release(app_vm);
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("[ELF] Oshibka zagruzki segmentov");
        return NULL;
    }

    // Выводим информацию
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("[ELF] Tochka vhoda: 0x");
    vga_print_hex(result.entry_point);
    vga_puts(" Segmentov: ");
    vga_print_dec(result.segment_count);
    vga_println("");

    // Выделяем стек для пользовательского процесса (1 страница = 4 KiB)
    uint32_t user_stack_base = USER_STACK_BASE;
    uint32_t user_stack_top = USER_STACK_TOP;

    // Проверяем, не замаплен ли уже стек (не должен — новое vm пустое)
    page_directory_t* cur_dir = paging_get_current_directory();
    if (!paging_is_mapped(cur_dir, user_stack_base)) {
        paging_alloc_user_page(user_stack_base, 1);
    }

    // Возвращаемся в исходное vm, чтобы родитель (вызывающий код) продолжил
    // работать в своём адресном пространстве.
    vm_switch(saved_vm);

    // Создаём задачу ядра, которая при старте переключится в Ring 3.
    // Параметры start_entry/start_stack записываем в саму задачу (per-task),
    // чтобы не возникало гонки между двумя подряд task_exec_elf, где второй
    // затирал бы глобалы до запуска первого.
    // Атомарно перестыковываем её vm на app_vm, пока прерывания отключены.
    __asm__ volatile ("cli");
    task_t* task = task_create(task_elf_entry_stub);
    if (task) {
        task->start_entry = result.entry_point;
        task->start_stack = user_stack_top;
        vm_space_t* kvm = task->vm;
        task->vm = app_vm;   // refcount = 1 из vm_create_user
        vm_release(kvm);
        if (name) task_set_name(task, name);
    } else {
        vm_release(app_vm);
    }
    __asm__ volatile ("sti");

    if (task) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("[ELF] Zadacha sozdana, PID=");
        vga_print_dec(task->id);
        vga_println("");
    }

    return task;
}

// ============================================================================
// Получение текущей задачи (Этап 18)
// ============================================================================

task_t* task_get_current(void) {
    return current_task;
}

// ============================================================================
// Завершение текущей задачи
// ============================================================================

void task_exit_current(void) {
    if (current_task) {
        current_task->state = TASK_DEAD;
        // Запускаем планировщик
        while(1) {
            __asm__ volatile("sti; hlt");
        }
    }
}

// ============================================================================
// Планировщик - переключает контекст
// ============================================================================

uint32_t task_switch(uint32_t current_esp) {
    if (!multitasking_enabled || !current_task) {
        return current_esp;
    }

    // Сохраняем указатель стека текущей задачи
    current_task->esp = current_esp;
    if (current_task->state == TASK_RUNNING) {
        current_task->state = TASK_READY;
    }

    // Планировщик Round-Robin: берем следующую задачу, по пути жнём мёртвых
    task_t* prev = current_task;
    task_t* next = current_task->next;

    // Собираем мёртвых в список для последующего освобождения.
    // Не освобождаем прямо в цикле обхода, чтобы не попортить кольцевой список.
    task_t* dead_chain = NULL;
    while (next->state == TASK_DEAD && next != prev) {
        task_t* victim = next;
        next = next->next;
        // Временно помечаем звено для reap'а
        victim->next = dead_chain;
        dead_chain = victim;
    }

    // Чиним кольцо: prev.next -> next
    prev->next = next;
    if (task_list_head && task_list_head->state == TASK_DEAD) {
        task_list_head = next;
    }

    current_task = next;
    current_task->state = TASK_RUNNING;
    // Учёт CPU-времени для диспетчера задач: один тик планировщика = одна
    // выбранная задача. Этого достаточно, чтобы построить относительный
    // график «кто сколько работает».
    current_task->cpu_ticks++;

    // Переключаем адресное пространство ДО возврата esp: последующий iret
    // восстановит EIP/ESP, и если CR3 ещё указывает на старый PD, user-VA
    // нового процесса окажутся недоступны, что даст Page Fault.
    vm_switch(current_task->vm);

    // Обновляем TSS для новой задачи (её kernel-стек для прерываний из Ring 3)
    tss_set_stack(0x10, current_task->stack_base + TASK_STACK_SIZE);

    // Освобождаем мёртвых уже после того, как переключились.
    // Это безопасно: ни одна живая задача их не использует.
    while (dead_chain) {
        task_t* nxt = dead_chain->next;
        // Освобождаем адресное пространство (refcount--; при 0 — физически
        // уничтожаем PD и все user-страницы).
        vm_release(dead_chain->vm);
        if (dead_chain->stack_base) {
            kfree((void*)dead_chain->stack_base);
        }
        kfree(dead_chain);
        dead_chain = nxt;
    }
    return current_task->esp;
}

// ============================================================================
// Дополнительные публичные API для диспетчера задач (Этап 33 / aos)
// ============================================================================

// Голова кольцевого списка задач. Используется GUI-приложением «Диспетчер
// задач» для перебора всех task_t. Возвращает NULL, если многозадачность
// ещё не инициализирована.
task_t* task_list_first(void) {
    return task_list_head;
}

// Установить «дружественное» имя задачи. Усекает по TASK_NAME_MAX.
// Если на входе путь типа "/disk/forktest.elf" — отрезаем директорию,
// чтобы в UI оставалось только короткое "forktest.elf".
void task_set_name(task_t* t, const char* name) {
    if (!t || !name) return;
    // ищем последний '/'
    const char* base = name;
    for (const char* p = name; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    if (!*base) base = name; // если строка кончилась на '/'
    task_strcpy_n(t->name, base, TASK_NAME_MAX + 1);
}

// Принудительно завершить задачу по id.
// Возвращает  0 при успехе, -1 если не найдено, -2 если попытка убить себя.
int task_kill_by_id(uint32_t id) {
    task_t* t = task_find_by_id(id);
    if (!t) return -1;
    if (t == current_task) return -2;
    // Достаточно пометить TASK_DEAD: планировщик жнёт мёртвых в task_switch,
    // освобождает vm и стек. Само переключение произойдёт на ближайшем тике.
    t->state = TASK_DEAD;
    return 0;
}

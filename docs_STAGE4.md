# ЭТАП 4: IDT и обработка прерываний - Инструкции

## Что было создано

На этом этапе мы создали полноценную систему обработки прерываний для процессора x86.

### ✓ Созданные файлы:

1. **kernel/arch/i386/idt.h** - Заголовочный файл IDT (структуры, константы)
2. **kernel/arch/i386/idt.c** - Инициализация IDT и перепрограммирование PIC
3. **kernel/arch/i386/idt_flush.asm** - Загрузка IDT в процессор
4. **kernel/arch/i386/isr.h** - Заголовочный файл ISR (прототипы обработчиков)
5. **kernel/arch/i386/isr.asm** - Обработчики исключений CPU (0-31)
6. **kernel/arch/i386/irq.asm** - Обработчики IRQ (32-47)
7. **kernel/arch/i386/interrupt.c** - Общая логика обработки прерываний
8. **kernel/kernel.c** - Обновлён для инициализации IDT
9. **Makefile** - Обновлён для компиляции новых файлов

## Что такое прерывания?

**Прерывания** - это механизм, позволяющий процессору реагировать на события:

### Типы прерываний:

1. **Исключения CPU (0-31)** - генерируются процессором при ошибках:
   - Деление на ноль (INT 0)
   - Page Fault (INT 14)
   - General Protection Fault (INT 13)
   - И другие

2. **IRQ (32-47)** - аппаратные прерывания от устройств:
   - IRQ0 (INT 32): Системный таймер
   - IRQ1 (INT 33): Клавиатура
   - IRQ12 (INT 44): PS/2 мышь
   - И другие

## Архитектура обработки прерываний

### IDT (Interrupt Descriptor Table)

IDT - это таблица из 256 дескрипторов, каждый указывает на обработчик прерывания.

**Структура дескриптора (8 байт):**
```
Биты 63-48: Offset [31:16]  - Старшие биты адреса обработчика
Биты 47-40: Type & Attributes - Тип шлюза и флаги
Биты 39-32: Reserved
Биты 31-16: Selector         - Селектор сегмента кода (0x08)
Биты 15-0:  Offset [15:0]    - Младшие биты адреса обработчика
```

### Процесс обработки прерывания:

```
1. Происходит событие (исключение или IRQ)
   ↓
2. Процессор ищет обработчик в IDT
   ↓
3. Процессор сохраняет EIP, CS, EFLAGS в стек
   ↓
4. Вызывается обработчик (isr0-31 или irq0-15)
   ↓
5. Обработчик сохраняет все регистры
   ↓
6. Вызывается isr_handler() или irq_handler() на C
   ↓
7. Если зарегистрирован пользовательский обработчик - вызывается
   ↓
8. Для IRQ: отправляется EOI в PIC
   ↓
9. Восстанавливаются все регистры
   ↓
10. IRET - возврат из прерывания
```

## PIC (Programmable Interrupt Controller)

PIC управляет аппаратными прерываниями (IRQ).

### Перепрограммирование PIC:

По умолчанию PIC отправляет IRQ 0-7 на прерывания 8-15, что конфликтует с исключениями CPU. Мы переназначаем:

- **IRQ 0-7** → прерывания **32-39** (главный PIC)
- **IRQ 8-15** → прерывания **40-47** (подчинённый PIC)

### Порты PIC:

```
Главный PIC:
  0x20 - командный порт
  0x21 - порт данных

Подчинённый PIC:
  0xA0 - командный порт
  0xA1 - порт данных
```

### EOI (End Of Interrupt):

После обработки IRQ нужно отправить EOI в PIC:
```c
// Для IRQ 0-7 (главный PIC)
outb(0x20, 0x20);

// Для IRQ 8-15 (подчинённый PIC)
outb(0xA0, 0x20);  // Подчинённому
outb(0x20, 0x20);  // Главному
```

## Регистрация обработчиков

Можно регистрировать пользовательские обработчики:

```c
#include "arch/i386/isr.h"

// Обработчик прерывания
void my_handler(registers_t* regs) {
    vga_println("Preryvaniye!");
}

// Регистрация
isr_register_handler(IRQ1, my_handler);  // Клавиатура

// Удаление
isr_unregister_handler(IRQ1);
```

## Обработка исключений

При исключении CPU выводится подробная информация:

```
========================================
   ISKLYUCHENIE PROCESSORA (EXCEPTION)
========================================

Isklyuchenie: Division By Zero

INT:  0  (0x00000000)
ERR:  0x00000000

Sostoyanie registrov:

EAX: 0x00000000  EBX: 0x00000000  ECX: 0x00000000  EDX: 0x00000000
ESI: 0x00000000  EDI: 0x00000000  EBP: 0x00000000  ESP: 0x00000000

EIP: 0x00100123  CS:  0x00000008  DS:  0x00000010
EFLAGS: 0x00000202

Sistema ostanovlena.
```

## Сборка и запуск (Linux/WSL)

### Команды:

```bash
make clean        # Очистка
make run          # Сборка и запуск
```

### Процесс сборки:

```bash
# 1. Ассемблер
nasm -f elf32 kernel/arch/i386/boot.asm -o build/kernel/arch/i386/boot.o
nasm -f elf32 kernel/arch/i386/gdt_flush.asm -o build/kernel/arch/i386/gdt_flush.o
nasm -f elf32 kernel/arch/i386/idt_flush.asm -o build/kernel/arch/i386/idt_flush.o
nasm -f elf32 kernel/arch/i386/isr.asm -o build/kernel/arch/i386/isr.o
nasm -f elf32 kernel/arch/i386/irq.asm -o build/kernel/arch/i386/irq.o

# 2. Компиляция C
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/kernel.c -o build/kernel/kernel.o
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/vga.c -o build/kernel/vga.o
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/arch/i386/gdt.c -o build/kernel/arch/i386/gdt.o
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/arch/i386/idt.c -o build/kernel/arch/i386/idt.o
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/arch/i386/interrupt.c -o build/kernel/arch/i386/interrupt.o

# 3. Линковка
ld -m elf_i386 -T linker.ld -nostdlib -o build/kernel.bin [все .o файлы]

# 4. Создание ISO и запуск
grub-mkrescue -o build/myos.iso iso/
qemu-system-i386 -cdrom build/myos.iso -m 128M
```

## Ожидаемый результат

При запуске `make run` ты должен увидеть:

```
╔════════════════════════════════════════════════════════════╗
║                      MyOS - v0.1                           ║
╚════════════════════════════════════════════════════════════╝

Dobro pozhalovat' v MyOS!

Sistema uspeshno zagruzhena i rabotaet v zashchishchennom rezhime.

=== Informatsiya o sisteme ===

Multiboot magic: 0x2BADB002 [OK]
Multiboot info:  0x00009500
Pamyat' (lower): 639 KB
Pamyat' (upper): 130048 KB

=== Status komponentov ===

[OK] Bootloader (GRUB Multiboot)
[OK] Yadro (kernel.bin)
[OK] VGA Text Mode (80x25)
[OK] GDT (Global Descriptor Table)
[OK] IDT (Interrupt Descriptor Table)      ← НОВОЕ!
[OK] Preryvaniya (ISR + IRQ)               ← НОВОЕ!
[--] Klaviatura (budet na etape 5)

=== Demonstratsiya tsvetov VGA ===

[Цветная палитра]

ETAP 4 ZAVERSHEN USPESHNO!                 ← ОБНОВЛЕНО!

IDT nastroena, preryvaniya rabotayut.
Sleduyushchij etap: Drajver klaviatury

Sistema ostanovlena. Nazhmi Ctrl+C dlya vykhoda iz QEMU.
```

## Возможные проблемы и решения

### Проблема 1: Triple Fault при загрузке IDT

**Причина:** Ошибка в дескрипторах IDT или обработчиках

**Решение:**
1. Проверь что все обработчики (isr0-31, irq0-15) определены
2. Проверь селектор (должен быть 0x08)
3. Проверь флаги (IDT_INT_GATE_32 = 0x8E)
4. Запусти с отладкой: `qemu-system-i386 -d int,cpu_reset`

### Проблема 2: "undefined reference to `isr0`"

**Причина:** isr.asm не скомпилирован или не слинкован

**Решение:**
1. Проверь что isr.asm и irq.asm в ASM_SOURCES
2. Проверь что файлы компилируются: `ls build/kernel/arch/i386/*.o`
3. Пересобери: `make clean && make`

### Проблема 3: Система зависает после idt_init()

**Причина:** Прерывания включены (STI), но обработчики не готовы

**Решение:**
1. Проверь что все 256 дескрипторов инициализированы (хотя бы нулями)
2. Проверь что PIC перепрограммирован до загрузки IDT
3. Временно закомментируй `sti` в idt_flush.asm для отладки

### Проблема 4: Постоянные прерывания от таймера

**Причина:** IRQ0 (таймер) генерируется ~18 раз в секунду

**Решение:**
1. Это нормально - таймер работает
2. Зарегистрируй обработчик для IRQ0 если нужно
3. Или замаскируй IRQ0: `outb(0x21, inb(0x21) | 0x01);`

### Проблема 5: Ошибки компиляции макросов в .asm

**Причина:** Синтаксис NASM

**Решение:**
1. Проверь что используется NASM (не MASM/GAS)
2. Проверь синтаксис макросов (%macro, %endmacro)
3. Убедись что нет пробелов в начале строк с метками

## Технические детали

### Структура registers_t:

```c
typedef struct {
    uint32_t ds;                    // Data Segment
    uint32_t edi, esi, ebp, esp;    // PUSHA
    uint32_t ebx, edx, ecx, eax;    // PUSHA
    uint32_t int_no, err_code;      // Номер и код ошибки
    uint32_t eip, cs, eflags;       // Автоматически
    uint32_t useresp, ss;           // Если смена привилегий
} registers_t;
```

### Исключения с кодом ошибки:

Только некоторые исключения помещают код ошибки в стек:
- 8: Double Fault
- 10: Invalid TSS
- 11: Segment Not Present
- 12: Stack-Segment Fault
- 13: General Protection Fault
- 14: Page Fault
- 17: Alignment Check

Остальные - нет, поэтому мы добавляем фиктивный 0.

### Типы шлюзов IDT:

- **Interrupt Gate (0x8E)** - отключает прерывания (CLI) при входе
- **Trap Gate (0x8F)** - не отключает прерывания
- **Task Gate (0x85)** - переключение задач (не используется)

Мы используем Interrupt Gate для всех прерываний.

## Тестирование

### Тест 1: Проверка символов IDT

```bash
nm build/kernel.bin | grep -E "(idt_init|isr0|irq0)"

# Ожидается:
# 001000XX T idt_init
# 001000XX T isr0
# 001000XX T irq0
```

### Тест 2: Проверка размера ядра

```bash
ls -lh build/kernel.bin

# Ожидается: ~25-30 KB (увеличился на ~10KB)
```

### Тест 3: Запуск в QEMU

```bash
make run

# Ожидается:
# - Система загружается
# - Выводится "[OK] IDT (Interrupt Descriptor Table)"
# - Выводится "[OK] Preryvaniya (ISR + IRQ)"
# - Сообщение "ETAP 4 ZAVERSHEN USPESHNO!"
# - Система работает стабильно
```

### Тест 4: Проверка обработки исключений (опционально)

Добавь в kernel.c перед halt:

```c
// ТЕСТ: деление на ноль
vga_println("Test: delenie na nol'...");
int x = 1 / 0;  // Вызовет исключение INT 0
```

Ожидается: красный экран с информацией об исключении.

## Следующий этап

**ЭТАП 5: ДРАЙВЕР КЛАВИАТУРЫ**

На следующем этапе мы:
- Создадим драйвер клавиатуры PS/2
- Реализуем обработчик IRQ1
- Добавим таблицу скан-кодов → ASCII
- Реализуем поддержку Shift, Caps Lock, Enter, Backspace
- Создадим кольцевой буфер клавиатуры
- Реализуем функции getchar() и gets()

---

**Статус:** ✓ ЭТАП 4 ЗАВЕРШЁН

**Следующий этап:** ЭТАП 5 - Драйвер клавиатуры

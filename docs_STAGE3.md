# ЭТАП 3: Защищённый режим и GDT - Инструкции

## Что было создано

На этом этапе мы настроили собственную GDT (Global Descriptor Table) для полного контроля над сегментацией памяти в Protected Mode.

### ✓ Созданные файлы:

1. **kernel/arch/i386/gdt.h** - Заголовочный файл GDT (структуры, константы)
2. **kernel/arch/i386/gdt.c** - Инициализация и настройка GDT
3. **kernel/arch/i386/gdt_flush.asm** - Загрузка GDT в процессор
4. **kernel/io.h** - Функции работы с портами I/O
5. **kernel/kernel.c** - Обновлён для инициализации GDT
6. **Makefile** - Обновлён для компиляции новых файлов

## Что такое GDT?

**GDT (Global Descriptor Table)** - это таблица дескрипторов сегментов, которая определяет области памяти в Protected Mode.

### Структура дескриптора (8 байт):

```
Биты 63-56: Base [31:24]     - Старшие биты базового адреса
Биты 55-52: Flags            - Флаги (гранулярность, размер)
Биты 51-48: Limit [19:16]    - Старшие биты лимита
Биты 47-40: Access Byte      - Права доступа
Биты 39-32: Base [23:16]     - Средние биты базового адреса
Биты 31-16: Base [15:0]      - Младшие биты базового адреса
Биты 15-0:  Limit [15:0]     - Младшие биты лимита
```

### Наша GDT содержит 5 дескрипторов:

| Индекс | Селектор | Тип | Описание |
|--------|----------|-----|----------|
| 0 | 0x00 | NULL | Нулевой дескриптор (обязателен) |
| 1 | 0x08 | Code | Сегмент кода ядра (Ring 0) |
| 2 | 0x10 | Data | Сегмент данных ядра (Ring 0) |
| 3 | 0x18 | Code | Сегмент кода пользователя (Ring 3) |
| 4 | 0x20 | Data | Сегмент данных пользователя (Ring 3) |

### Параметры сегментов:

- **База**: 0x00000000 (начало памяти)
- **Лимит**: 0xFFFFFFFF (4GB, вся память)
- **Гранулярность**: 4KB (лимит умножается на 4096)
- **Размер**: 32-bit

Это создаёт **flat memory model** - вся память доступна как единое адресное пространство.

## Процесс загрузки GDT

1. **gdt_init()** (C) - создаёт таблицу дескрипторов в памяти
2. **gdt_flush()** (ASM) - загружает GDT в процессор:
   - `LGDT` - загружает адрес GDT в регистр GDTR
   - Обновляет сегментные регистры (DS, ES, FS, GS, SS)
   - Far jump для обновления CS (сегмент кода)

## Порты I/O

Файл `io.h` предоставляет функции для работы с портами ввода-вывода:

```c
uint8_t inb(uint16_t port);      // Чтение байта
void outb(uint16_t port, uint8_t data);  // Запись байта

uint16_t inw(uint16_t port);     // Чтение слова (2 байта)
void outw(uint16_t port, uint16_t data); // Запись слова

uint32_t inl(uint16_t port);     // Чтение dword (4 байта)
void outl(uint16_t port, uint32_t data); // Запись dword

void io_wait(void);              // Задержка I/O
```

Эти функции будут использоваться для работы с:
- PIC (Programmable Interrupt Controller)
- PIT (Programmable Interval Timer)
- Клавиатурой
- Последовательным портом
- И другими устройствами

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

# 2. Компиляция C
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/kernel.c -o build/kernel/kernel.o
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/vga.c -o build/kernel/vga.o
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/arch/i386/gdt.c -o build/kernel/arch/i386/gdt.o

# 3. Линковка
ld -m elf_i386 -T linker.ld -nostdlib -o build/kernel.bin \
   build/kernel/arch/i386/boot.o \
   build/kernel/arch/i386/gdt_flush.o \
   build/kernel/kernel.o \
   build/kernel/vga.o \
   build/kernel/arch/i386/gdt.o

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
[OK] GDT (Global Descriptor Table)        ← НОВОЕ!
[--] IDT (budet na etape 4)
[--] Preryvaniya (budet na etape 5)
[--] Klaviatura (budet na etape 6)

=== Demonstratsiya tsvetov VGA ===

[Цветная палитра]

ETAP 3 ZAVERSHEN USPESHNO!                ← ОБНОВЛЕНО!

GDT nastroena, processor v Protected Mode.
Sleduyushchij etap: IDT i obrabotka preryvanij

Sistema ostanovlena. Nazhmi Ctrl+C dlya vykhoda iz QEMU.
```

## Возможные проблемы и решения

### Проблема 1: Triple Fault (перезагрузка QEMU)

**Причина:** Ошибка при загрузке GDT или обновлении сегментных регистров

**Решение:**
1. Проверь что `gdt_ptr.limit` правильно вычислен
2. Проверь что селекторы корректны (0x08, 0x10)
3. Проверь far jump в gdt_flush.asm
4. Запусти с отладкой: `qemu-system-i386 -cdrom build/myos.iso -d int,cpu_reset`

### Проблема 2: "undefined reference to `gdt_flush`"

**Причина:** gdt_flush.asm не скомпилирован или не слинкован

**Решение:**
1. Проверь что gdt_flush.asm в ASM_SOURCES в Makefile
2. Проверь что файл компилируется: `ls build/kernel/arch/i386/gdt_flush.o`
3. Пересобери: `make clean && make`

### Проблема 3: Чёрный экран после загрузки GDT

**Причина:** Неправильные дескрипторы или селекторы

**Решение:**
1. Проверь что базовый адрес = 0, лимит = 0xFFFFFFFF
2. Проверь флаги доступа (Present bit должен быть установлен)
3. Проверь что гранулярность = 0xCF (4KB, 32-bit)

### Проблема 4: Ошибки компиляции inline функций

**Причина:** Старая версия GCC или неправильные флаги

**Решение:**
1. Убедись что используется GCC 4.8+
2. Проверь флаги: `-ffreestanding -fno-pie`
3. Попробуй добавить `-std=gnu99`

## Технические детали

### Access Byte (байт доступа):

```
Бит 7: Present (P)           - 1 = сегмент в памяти
Бит 6-5: Privilege (DPL)     - 00 = Ring 0, 11 = Ring 3
Бит 4: Descriptor Type (S)   - 1 = код/данные
Бит 3: Executable (E)        - 1 = код, 0 = данные
Бит 2: Direction/Conforming  - направление роста
Бит 1: Readable/Writable     - 1 = чтение/запись
Бит 0: Accessed (A)          - устанавливается CPU
```

### Granularity Byte (байт гранулярности):

```
Бит 7: Granularity (G)       - 1 = лимит в 4KB страницах
Бит 6: Size (D/B)            - 1 = 32-bit сегмент
Бит 5: Long Mode (L)         - 1 = 64-bit (не используется)
Бит 4: Reserved              - должен быть 0
Бит 3-0: Limit [19:16]       - старшие биты лимита
```

### Селекторы сегментов:

```
Биты 15-3: Index             - индекс в GDT (0-8191)
Бит 2: Table Indicator (TI)  - 0 = GDT, 1 = LDT
Биты 1-0: RPL                - Requested Privilege Level
```

Пример: 0x08 = 0000000000001000b
- Index = 1 (второй дескриптор)
- TI = 0 (GDT)
- RPL = 0 (Ring 0)

## Проверка работоспособности

### Тест 1: Проверка символов GDT

```bash
nm build/kernel.bin | grep gdt

# Ожидается:
# 001000XX T gdt_init
# 001000XX T gdt_set_gate
# 001000XX T gdt_flush
```

### Тест 2: Проверка размера ядра

```bash
ls -lh build/kernel.bin

# Ожидается: ~15-20 KB (увеличился на ~5KB)
```

### Тест 3: Запуск в QEMU

```bash
make run

# Ожидается: 
# - Система загружается
# - Выводится "[OK] GDT (Global Descriptor Table)"
# - Сообщение "ETAP 3 ZAVERSHEN USPESHNO!"
```

## Следующий этап

**ЭТАП 4: IDT И ОБРАБОТКА ПРЕРЫВАНИЙ**

На следующем этапе мы:
- Создадим IDT (Interrupt Descriptor Table)
- Реализуем обработчики исключений (0-31)
- Настроим PIC (Programmable Interrupt Controller)
- Реализуем обработчики IRQ (32-47)
- Добавим обработку деления на ноль, page fault и других исключений

---

**Статус:** ✓ ЭТАП 3 ЗАВЕРШЁН

**Следующий этап:** ЭТАП 4 - IDT и обработка прерываний

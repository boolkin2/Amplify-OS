# ЭТАП 2: Загрузчик (Bootloader) - Инструкции

## Что было создано

На этом этапе мы создали минимальное загружаемое ядро с выводом на экран.

### ✓ Созданные файлы:

1. **kernel/arch/i386/boot.asm** - Точка входа в ядро, Multiboot header
2. **kernel/kernel.c** - Главная функция ядра на C
3. **kernel/vga.h** - Заголовочный файл драйвера VGA
4. **kernel/vga.c** - Драйвер VGA text mode (80x25)
5. **Makefile** - Обновлён для сборки новых файлов

## Архитектура

### Процесс загрузки:

```
1. BIOS/UEFI
   ↓
2. GRUB (загрузчик)
   ↓
3. boot.asm (_start)
   - Проверка Multiboot magic
   - Настройка стека (16KB)
   - Передача управления в kernel_main()
   ↓
4. kernel.c (kernel_main)
   - Инициализация VGA
   - Вывод приветствия
   - Вывод информации о системе
   - Остановка процессора (halt)
```

### Multiboot спецификация:

Multiboot - это стандарт загрузки ядра, поддерживаемый GRUB.

**Multiboot header** (в boot.asm):
- Magic number: 0x1BADB002
- Flags: выравнивание + информация о памяти
- Checksum: -(magic + flags)

**При загрузке GRUB передаёт:**
- EAX = 0x2BADB002 (magic для проверки)
- EBX = адрес структуры multiboot_info_t

### VGA Text Mode:

- Видеопамять: 0xB8000
- Размер: 80x25 символов
- Формат: 2 байта на символ (ASCII + атрибут цвета)
- Атрибут: 4 бита фон | 4 бита текст
- Палитра: 16 цветов

## Сборка и запуск (Linux)

### Команды сборки:

```bash
# Полная сборка и запуск
make run

# Только сборка
make

# Очистка
make clean

# Отладка
make debug
```

### Процесс сборки:

```bash
# 1. Компиляция boot.asm
nasm -f elf32 kernel/arch/i386/boot.asm -o build/kernel/arch/i386/boot.o

# 2. Компиляция kernel.c
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/kernel.c -o build/kernel/kernel.o

# 3. Компиляция vga.c
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c kernel/vga.c -o build/kernel/vga.o

# 4. Линковка
ld -m elf_i386 -T linker.ld -nostdlib -o build/kernel.bin \
   build/kernel/arch/i386/boot.o build/kernel/kernel.o build/kernel/vga.o

# 5. Создание ISO
grub-mkrescue -o build/myos.iso iso/

# 6. Запуск в QEMU
qemu-system-i386 -cdrom build/myos.iso -m 128M
```

## Ожидаемый результат

При запуске `make run` ты должен увидеть в окне QEMU:

```
╔════════════════════════════════════════════════════════════╗
║                      MyOS - v0.1                           ║
╚════════════════════════════════════════════════════════════╝

Dobro pozhalovat' v MyOS!

Sistema uspeshno zagruzhena i rabotaet v zashchishchennom rezhime.

=== Informatsiya o sisteme ===

Multiboot magic: 0x2BADB002 [OK]
Multiboot info:  0x000XXXXX
Pamyat' (lower): 639 KB
Pamyat' (upper): 130048 KB

=== Status komponentov ===

[OK] Bootloader (GRUB Multiboot)
[OK] Yadro (kernel.bin)
[OK] VGA Text Mode (80x25)
[--] Preryvaniya (budet na etape 5)
[--] Klaviatura (budet na etape 6)

=== Demonstratsiya tsvetov VGA ===

[Цветная палитра из 16 цветов]

ETAP 2 ZAVERSHEN USPESHNO!

Sistema gotova k dal'nejshej razrabotke.
Sleduyushchij etap: Zashchishchennyj rezhim i GDT

Sistema ostanovlena. Nazhmi Ctrl+C dlya vykhoda iz QEMU.
```

## Возможные проблемы и решения

### Проблема 1: "multiboot header not found"

**Причина:** Multiboot header не в первых 8KB файла

**Решение:** Проверь linker.ld - секция .multiboot должна быть первой:
```ld
.text BLOCK(4K) : ALIGN(4K)
{
    *(.multiboot)    /* Должно быть первым! */
    *(.text)
}
```

### Проблема 2: "undefined reference to `__stack_chk_fail`"

**Причина:** GCC пытается использовать stack protector

**Решение:** Добавь флаг `-fno-stack-protector` в CFLAGS (уже добавлен)

### Проблема 3: Чёрный экран в QEMU

**Причина:** Ядро не загрузилось или зависло

**Решение:**
1. Проверь что ISO создан: `ls -lh build/myos.iso`
2. Проверь GRUB конфиг: `cat boot/grub.cfg`
3. Запусти с отладкой: `qemu-system-i386 -cdrom build/myos.iso -d int,cpu_reset`

### Проблема 4: "Error 13: Invalid or unsupported executable format"

**Причина:** Неправильный формат исполняемого файла

**Решение:**
1. Проверь что используется `-m elf_i386` при линковке
2. Проверь что boot.asm компилируется с `-f elf32`
3. Проверь multiboot header: `readelf -h build/kernel.bin`

### Проблема 5: Ошибки компиляции на Windows

**Причина:** Проект предназначен для Linux

**Решение:** Используй WSL2, MSYS2 или виртуальную машину с Linux

## Структура кода

### boot.asm (точка входа):
- Multiboot header для GRUB
- Настройка стека (16KB)
- Вызов kernel_main(magic, addr)
- Остановка процессора при возврате

### kernel.c (главное ядро):
- Проверка Multiboot magic
- Инициализация VGA
- Вывод информации о системе
- Демонстрация возможностей

### vga.c (драйвер VGA):
- Прямая работа с видеопамятью 0xB8000
- Поддержка 16 цветов
- Функции: putchar, puts, println
- Вывод чисел (hex, dec)
- Прокрутка экрана

## Тестирование

### Проверка Multiboot header:

```bash
# Проверка наличия Multiboot header
hexdump -C build/kernel.bin | head -n 5

# Должно быть:
# 00000000  02 b0 ad 1b 03 00 00 00  fb 4f 52 e4 ...
#           ^^^^^^^^^^^  - magic (0x1BADB002)
```

### Проверка секций:

```bash
readelf -S build/kernel.bin

# Должны быть секции:
# .multiboot
# .text
# .rodata
# .data
# .bss
```

### Проверка символов:

```bash
nm build/kernel.bin | grep -E "(kernel_main|_start)"

# Должно быть:
# 00100000 T _start
# 001000XX T kernel_main
```

## Следующий этап

**ЭТАП 3: Защищённый режим (Protected Mode)**

На следующем этапе мы:
- Настроим GDT (Global Descriptor Table)
- Убедимся что процессор в Protected Mode
- Настроим сегментные регистры
- Подготовимся к обработке прерываний

---

**Статус:** ✓ ЭТАП 2 ЗАВЕРШЁН

**Следующий этап:** ЭТАП 3 - Защищённый режим и GDT

# ЭТАП 5: Драйвер клавиатуры - Инструкции

## Что было создано

На этом этапе мы создали полноценный драйвер клавиатуры PS/2 с поддержкой интерактивного ввода.

### ✓ Созданные файлы:

1. **kernel/drivers/keyboard.h** - Заголовочный файл драйвера клавиатуры
2. **kernel/drivers/keyboard.c** - Реализация драйвера клавиатуры
3. **kernel/kernel.c** - Обновлён для инициализации клавиатуры и теста
4. **Makefile** - Обновлён для компиляции драйвера

## Архитектура драйвера клавиатуры

### Как работает клавиатура PS/2:

1. **Пользователь нажимает клавишу**
2. **Клавиатура генерирует IRQ1** (прерывание)
3. **Процессор вызывает обработчик IRQ1**
4. **Обработчик читает скан-код** из порта 0x60
5. **Скан-код преобразуется в ASCII** (с учётом модификаторов)
6. **Символ добавляется в кольцевой буфер**
7. **Приложение читает символ** из буфера

### Скан-коды:

Клавиатура отправляет **скан-коды** (scan codes), а не ASCII символы:
- **Make code** - код нажатия клавиши (например, 0x1E для 'A')
- **Break code** - код отпускания клавиши (make code + 0x80)

Примеры:
```
Клавиша 'A': нажатие = 0x1E, отпускание = 0x9E
Клавиша 'Enter': нажатие = 0x1C, отпускание = 0x9C
Клавиша 'Shift': нажатие = 0x2A, отпускание = 0xAA
```

### Модификаторы:

Драйвер отслеживает состояние модификаторов:
- **Shift** (левый/правый) - изменяет регистр букв и символы
- **Caps Lock** - фиксирует верхний регистр для букв
- **Ctrl** - для комбинаций клавиш (пока не используется)
- **Alt** - для комбинаций клавиш (пока не используется)
- **Num Lock** - для цифровой клавиатуры
- **Scroll Lock** - для прокрутки (пока не используется)

### Кольцевой буфер:

Буфер размером 256 символов хранит нажатые клавиши:
```
[0] [1] [2] ... [254] [255]
 ^                      ^
 |                      |
read_pos            write_pos
```

Когда `write_pos` достигает конца, он возвращается к началу (кольцевой).

## Реализованные функции

### keyboard_init()
Инициализирует драйвер:
- Очищает буфер
- Сбрасывает состояние модификаторов
- Регистрирует обработчик IRQ1

### keyboard_getchar()
Читает один символ (блокирующее):
- Ожидает нажатия клавиши
- Возвращает ASCII символ

### keyboard_getchar_nonblock()
Читает один символ (неблокирующее):
- Возвращает символ или 0 если буфер пуст

### keyboard_gets()
Читает строку до Enter:
- Поддерживает Backspace (удаление символа)
- Выводит эхо на экран
- Добавляет '\0' в конец строки

### keyboard_has_key()
Проверяет наличие символа в буфере

### keyboard_get_state()
Возвращает состояние модификаторов

### keyboard_clear_buffer()
Очищает буфер клавиатуры

## Таблица скан-кодов

Драйвер использует две таблицы преобразования:

**Без Shift:**
```
0x10 = 'q', 0x11 = 'w', 0x12 = 'e', ...
0x02 = '1', 0x03 = '2', 0x04 = '3', ...
```

**С Shift:**
```
0x10 = 'Q', 0x11 = 'W', 0x12 = 'E', ...
0x02 = '!', 0x03 = '@', 0x04 = '#', ...
```

## Сборка и запуск (Linux/WSL)

### Команды:

```bash
make clean        # Очистка
make run          # Сборка и запуск
```

### Процесс сборки:

```bash
# Компиляция драйвера клавиатуры
gcc -m32 -ffreestanding -O2 -Wall -Wextra -c \
    kernel/drivers/keyboard.c -o build/kernel/drivers/keyboard.o

# Линковка (добавляется keyboard.o)
ld -m elf_i386 -T linker.ld -nostdlib -o build/kernel.bin \
   [все .o файлы включая keyboard.o]
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
[OK] IDT (Interrupt Descriptor Table)
[OK] Preryvaniya (ISR + IRQ)
[OK] Klaviatura (PS/2)                    ← НОВОЕ!

=== Demonstratsiya tsvetov VGA ===

[Цветная палитра]

ETAP 5 ZAVERSHEN USPESHNO!                ← ОБНОВЛЕНО!

Klaviatura rabotaet, mozhno vvodit' tekst.

=== Test klaviatury ===

Vvedite tekst (nazhmi Enter dlya zavershenia):
> _                                        ← КУРСОР МИГАЕТ!

[Здесь можно вводить текст с клавиатуры]
```

**Ты можешь:**
- Печатать буквы (a-z, A-Z)
- Печатать цифры (0-9)
- Использовать Shift для заглавных букв и символов
- Использовать Caps Lock
- Использовать Backspace для удаления
- Нажать Enter для завершения ввода

После Enter система выведет:
```
Vy vveli: [твой текст]
```

## Возможные проблемы и решения

### Проблема 1: Клавиатура не реагирует

**Причина:** IRQ1 не доходит до обработчика

**Решение:**
1. Проверь что `keyboard_init()` вызывается после `idt_init()`
2. Проверь что IRQ1 не замаскирован в PIC
3. Проверь что прерывания включены (STI в idt_flush.asm)
4. Добавь отладочный вывод в `keyboard_irq_handler()`

### Проблема 2: Неправильные символы

**Причина:** Ошибка в таблице скан-кодов

**Решение:**
1. Проверь таблицы `scancode_to_ascii` и `scancode_to_ascii_shift`
2. Добавь вывод скан-кода для отладки: `vga_print_hex(scancode)`
3. Сравни с таблицей скан-кодов PS/2

### Проблема 3: Shift не работает

**Причина:** Состояние Shift не обновляется

**Решение:**
1. Проверь обработку `KEY_LEFT_SHIFT` и `KEY_RIGHT_SHIFT`
2. Проверь что обрабатываются и нажатия и отпускания
3. Добавь вывод состояния: `vga_print_dec(kbd_state.shift)`

### Проблема 4: Backspace не работает

**Причина:** VGA драйвер не поддерживает '\b'

**Решение:**
1. Проверь реализацию `vga_putchar('\b')` в vga.c
2. Backspace должен двигать курсор назад и стирать символ
3. Убедись что `vga_column` уменьшается

### Проблема 5: Буфер переполняется

**Причина:** Приложение не читает символы достаточно быстро

**Решение:**
1. Увеличь `KEYBOARD_BUFFER_SIZE` (сейчас 256)
2. Или игнорируй новые символы при переполнении (уже реализовано)
3. Вызывай `keyboard_clear_buffer()` при необходимости

## Технические детали

### Порты клавиатуры:

```
0x60 - Data Port (чтение скан-кодов)
0x64 - Status Port (чтение статуса)
0x64 - Command Port (отправка команд)
```

### Структура keyboard_state_t:

```c
typedef struct {
    bool shift;         // Shift нажат
    bool ctrl;          // Ctrl нажат
    bool alt;           // Alt нажат
    bool caps_lock;     // Caps Lock включён
    bool num_lock;      // Num Lock включён
    bool scroll_lock;   // Scroll Lock включён
} keyboard_state_t;
```

### Кольцевой буфер:

```c
static char keyboard_buffer[256];
static int buffer_read_pos = 0;
static int buffer_write_pos = 0;
static int buffer_count = 0;
```

Операции:
- **Put**: `buffer[write_pos++] = c; count++;`
- **Get**: `c = buffer[read_pos++]; count--;`
- **Wrap**: `pos = pos % BUFFER_SIZE;`

### Обработка Caps Lock:

```c
if (caps_lock && c >= 'a' && c <= 'z') {
    c = c - 'a' + 'A';  // Нижний → Верхний
}
```

Caps Lock влияет только на буквы, не на цифры и символы.

## Примеры использования

### Пример 1: Чтение одного символа

```c
#include "drivers/keyboard.h"

char c = keyboard_getchar();
vga_putchar(c);
```

### Пример 2: Чтение строки

```c
char buffer[100];
keyboard_gets(buffer, sizeof(buffer));
vga_puts("Vy vveli: ");
vga_println(buffer);
```

### Пример 3: Неблокирующее чтение

```c
while (1) {
    char c = keyboard_getchar_nonblock();
    if (c != 0) {
        vga_putchar(c);
    }
    // Делаем что-то ещё...
}
```

### Пример 4: Проверка модификаторов

```c
keyboard_state_t state = keyboard_get_state();
if (state.shift) {
    vga_println("Shift nazhat");
}
if (state.caps_lock) {
    vga_println("Caps Lock vklyuchen");
}
```

## Тестирование

### Тест 1: Проверка символов

```bash
make run

# В QEMU:
# Введи: Hello World 123!
# Ожидается: Hello World 123!
```

### Тест 2: Проверка Shift

```bash
# Введи с зажатым Shift: hello
# Ожидается: HELLO
```

### Тест 3: Проверка Caps Lock

```bash
# Нажми Caps Lock
# Введи: hello
# Ожидается: HELLO
```

### Тест 4: Проверка Backspace

```bash
# Введи: Hello
# Нажми Backspace 2 раза
# Введи: p
# Ожидается: Help
```

### Тест 5: Проверка специальных символов

```bash
# С Shift введи: 1234567890
# Ожидается: !@#$%^&*()
```

## Следующий этап

**ЭТАП 6: ТАЙМЕР PIT**

На следующем этапе мы:
- Настроим PIT (Programmable Interval Timer)
- Реализуем системный таймер (тики)
- Добавим функции sleep() и delay()
- Реализуем подсчёт uptime системы
- Подготовимся к многозадачности

---

**Статус:** ✓ ЭТАП 5 ЗАВЕРШЁН

**Следующий этап:** ЭТАП 6 - Таймер PIT

# ЭТАП 1: Подготовка окружения - Инструкции

## Что было сделано

На этом этапе мы подготовили полное окружение для разработки операционной системы:

### ✓ Созданные файлы:

1. **setup.sh** - Скрипт автоматической установки всех зависимостей
2. **README.md** - Документация проекта
3. **Makefile** - Система сборки проекта
4. **linker.ld** - Скрипт линковщика для ядра
5. **boot/grub.cfg** - Конфигурация загрузчика GRUB
6. **scripts/check_environment.sh** - Скрипт проверки окружения
7. **.gitignore** - Игнорируемые файлы для Git
8. **quickstart.sh** - Скрипт быстрого старта

### ✓ Созданная структура:

```
MyOS/
├── boot/              # Загрузчик (готово к ЭТАПУ 2)
├── kernel/            # Ядро ОС
│   ├── arch/i386/    # Архитектурно-зависимый код
│   ├── drivers/      # Драйверы
│   ├── fs/           # Файловая система
│   ├── memory/       # Управление памятью
│   └── process/      # Процессы
├── gui/              # Графический интерфейс
├── libc/             # Стандартная библиотека
├── userspace/        # Пользовательские программы
├── build/            # Скомпилированные файлы
├── iso/              # Временные файлы ISO
└── scripts/          # Вспомогательные скрипты
```

## Как запустить (для Linux)

### Вариант 1: Быстрый старт (рекомендуется)

```bash
chmod +x quickstart.sh
./quickstart.sh
```

Этот скрипт автоматически:
- Установит все зависимости
- Проверит окружение
- Создаст структуру проекта
- Подготовит всё к разработке

### Вариант 2: Пошаговая установка

```bash
# 1. Установка зависимостей
chmod +x setup.sh
./setup.sh

# 2. Проверка окружения
chmod +x scripts/check_environment.sh
./scripts/check_environment.sh

# 3. Просмотр информации о проекте
make info
```

## Установленные инструменты

- **NASM** - Ассемблер для x86
- **GCC** - Компилятор C с поддержкой 32-bit
- **LD** - Линкер
- **Make** - Система сборки
- **QEMU** - Эмулятор x86
- **GRUB** - Загрузчик (grub-mkrescue)
- **xorriso** - Утилита создания ISO образов
- **mtools** - Утилиты для работы с FAT

## Проверка установки

После установки выполни:

```bash
# Проверка версий инструментов
nasm -v
gcc --version
qemu-system-i386 --version
grub-mkrescue --version

# Проверка 32-bit поддержки
gcc -m32 -c -x c /dev/null -o /tmp/test.o
echo "32-bit поддержка работает!"
rm /tmp/test.o
```

## Возможные проблемы и решения

### Проблема 1: "gcc: error: unrecognized command line option '-m32'"

**Решение (Ubuntu/Debian):**
```bash
sudo apt-get install gcc-multilib g++-multilib
```

**Решение (Arch Linux):**
```bash
sudo pacman -S lib32-gcc-libs
```

### Проблема 2: "grub-mkrescue: command not found"

**Решение (Ubuntu/Debian):**
```bash
sudo apt-get install grub-pc-bin grub-common
```

**Решение (Arch Linux):**
```bash
sudo pacman -S grub
```

### Проблема 3: "xorriso: command not found"

**Решение (Ubuntu/Debian):**
```bash
sudo apt-get install xorriso
```

**Решение (Arch Linux):**
```bash
sudo pacman -S libisoburn
```

## Следующие шаги

После успешной установки окружения переходим к **ЭТАПУ 2: Загрузчик (Bootloader)**.

На следующем этапе мы создадим:
- Multiboot header для GRUB
- Точку входа в ядро на ассемблере
- Минимальное ядро, выводящее текст на экран
- Загрузочный ISO образ
- Запуск в QEMU

## Команды для работы с проектом

```bash
make info    # Информация о проекте
make         # Сборка (пока ничего не соберёт - нет исходников)
make clean   # Очистка
make run     # Сборка и запуск в QEMU
```

---

**Статус:** ✓ ЭТАП 1 ЗАВЕРШЁН

**Следующий этап:** ЭТАП 2 - Загрузчик (Bootloader)

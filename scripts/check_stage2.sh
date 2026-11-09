#!/bin/bash

# Скрипт проверки ЭТАПА 2 - Загрузчик

echo ""
echo "╔════════════════════════════════════════╗"
echo "║   Проверка ЭТАПА 2: Загрузчик          ║"
echo "╚════════════════════════════════════════╝"
echo ""

# Проверка наличия исходных файлов
echo "📁 Проверка исходных файлов..."
echo ""

FILES=(
    "kernel/arch/i386/boot.asm"
    "kernel/kernel.c"
    "kernel/vga.h"
    "kernel/vga.c"
    "linker.ld"
    "boot/grub.cfg"
)

ALL_OK=1

for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        echo "  ✓ $file"
    else
        echo "  ✗ $file - ОТСУТСТВУЕТ"
        ALL_OK=0
    fi
done

echo ""

if [ $ALL_OK -eq 0 ]; then
    echo "❌ Не все файлы созданы!"
    exit 1
fi

echo "✓ Все исходные файлы на месте"
echo ""

# Попытка сборки
echo "🔨 Попытка сборки проекта..."
echo ""

make clean > /dev/null 2>&1
make > /tmp/build.log 2>&1

if [ $? -eq 0 ]; then
    echo "✓ Сборка успешна!"
else
    echo "❌ Ошибка сборки!"
    echo ""
    echo "Лог ошибок:"
    cat /tmp/build.log
    exit 1
fi

echo ""

# Проверка созданных файлов
echo "📦 Проверка результатов сборки..."
echo ""

if [ -f "build/kernel.bin" ]; then
    SIZE=$(stat -f%z "build/kernel.bin" 2>/dev/null || stat -c%s "build/kernel.bin" 2>/dev/null)
    echo "  ✓ kernel.bin создан (размер: $SIZE байт)"
else
    echo "  ✗ kernel.bin не создан"
    ALL_OK=0
fi

if [ -f "build/myos.iso" ]; then
    SIZE=$(stat -f%z "build/myos.iso" 2>/dev/null || stat -c%s "build/myos.iso" 2>/dev/null)
    echo "  ✓ myos.iso создан (размер: $SIZE байт)"
else
    echo "  ✗ myos.iso не создан"
    ALL_OK=0
fi

echo ""

if [ $ALL_OK -eq 0 ]; then
    echo "❌ Не все файлы собраны!"
    exit 1
fi

# Проверка Multiboot header
echo "🔍 Проверка Multiboot header..."
echo ""

MAGIC=$(hexdump -n 4 -e '4/1 "%02x"' build/kernel.bin 2>/dev/null)

if [ "$MAGIC" = "02b0ad1b" ]; then
    echo "  ✓ Multiboot magic найден: 0x1BADB002"
else
    echo "  ✗ Multiboot magic не найден или неверный: 0x$MAGIC"
    echo "    Ожидается: 0x1BADB002"
    ALL_OK=0
fi

echo ""

# Проверка символов
echo "🔍 Проверка символов..."
echo ""

if command -v nm &> /dev/null; then
    if nm build/kernel.bin | grep -q "_start"; then
        echo "  ✓ Символ _start найден"
    else
        echo "  ✗ Символ _start не найден"
        ALL_OK=0
    fi

    if nm build/kernel.bin | grep -q "kernel_main"; then
        echo "  ✓ Символ kernel_main найден"
    else
        echo "  ✗ Символ kernel_main не найден"
        ALL_OK=0
    fi
else
    echo "  ⚠ nm не установлен, пропускаем проверку символов"
fi

echo ""

# Итоговый результат
echo "════════════════════════════════════════"

if [ $ALL_OK -eq 1 ]; then
    echo "✓ ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ!"
    echo ""
    echo "ЭТАП 2 завершён успешно."
    echo ""
    echo "Для запуска в QEMU выполни:"
    echo "  make run"
    echo ""
    echo "Ожидаемый результат:"
    echo "  - Цветной текст на экране"
    echo "  - Приветствие MyOS"
    echo "  - Информация о системе"
    echo "  - Демонстрация цветов VGA"
    echo ""
else
    echo "✗ ОБНАРУЖЕНЫ ПРОБЛЕМЫ"
    echo ""
    echo "Исправь ошибки выше и запусти проверку снова."
    exit 1
fi

echo "════════════════════════════════════════"

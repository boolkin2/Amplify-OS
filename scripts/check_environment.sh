#!/bin/bash

# Скрипт проверки окружения для MyOS

echo "=================================="
echo "Проверка окружения MyOS"
echo "=================================="
echo ""

# Флаг успешности всех проверок
ALL_OK=1

# Функция проверки команды
check_command() {
    local cmd=$1
    local name=$2
    
    if command -v $cmd &> /dev/null; then
        echo "✓ $name: установлен"
        $cmd --version 2>&1 | head -n1 | sed 's/^/  /'
        return 0
    else
        echo "✗ $name: НЕ УСТАНОВЛЕН"
        ALL_OK=0
        return 1
    fi
}

# Проверка основных инструментов
check_command "nasm" "NASM (ассемблер)"
check_command "gcc" "GCC (компилятор C)"
check_command "ld" "LD (линкер)"
check_command "make" "Make (система сборки)"
check_command "qemu-system-i386" "QEMU (эмулятор)"
check_command "grub-mkrescue" "GRUB (загрузчик)"
check_command "xorriso" "xorriso (создание ISO)"

echo ""
echo "=================================="
echo "Проверка поддержки 32-bit"
echo "=================================="

# Создаём тестовый файл
cat > /tmp/test_32bit.c << 'EOF'
int main() { return 0; }
EOF

if gcc -m32 /tmp/test_32bit.c -o /tmp/test_32bit 2>/dev/null; then
    echo "✓ Поддержка 32-bit компиляции: работает"
    rm -f /tmp/test_32bit.c /tmp/test_32bit
else
    echo "✗ Поддержка 32-bit компиляции: НЕ РАБОТАЕТ"
    echo "  Установи: gcc-multilib g++-multilib (Ubuntu/Debian)"
    echo "  или: lib32-gcc-libs (Arch Linux)"
    ALL_OK=0
    rm -f /tmp/test_32bit.c
fi

echo ""
echo "=================================="
echo "Проверка структуры проекта"
echo "=================================="

# Проверка наличия ключевых директорий
DIRS=("boot" "kernel" "build" "iso")
for dir in "${DIRS[@]}"; do
    if [ -d "$dir" ]; then
        echo "✓ Директория $dir: существует"
    else
        echo "✗ Директория $dir: отсутствует"
        ALL_OK=0
    fi
done

# Проверка наличия ключевых файлов
FILES=("Makefile" "linker.ld" "boot/grub.cfg" "README.md")
for file in "${FILES[@]}"; do
    if [ -f "$file" ]; then
        echo "✓ Файл $file: существует"
    else
        echo "✗ Файл $file: отсутствует"
        ALL_OK=0
    fi
done

echo ""
echo "=================================="

if [ $ALL_OK -eq 1 ]; then
    echo "✓ ВСЕ ПРОВЕРКИ ПРОЙДЕНЫ УСПЕШНО!"
    echo ""
    echo "Окружение готово к разработке."
    echo "Следующий шаг: переходи к ЭТАПУ 2 (Загрузчик)"
else
    echo "✗ ОБНАРУЖЕНЫ ПРОБЛЕМЫ"
    echo ""
    echo "Исправь ошибки выше и запусти проверку снова."
    exit 1
fi

echo "=================================="

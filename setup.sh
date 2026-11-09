#!/bin/bash

# Скрипт установки окружения для разработки ОС
# Поддерживает Ubuntu/Debian и Arch Linux

set -e

echo "=================================="
echo "Установка окружения для MyOS"
echo "=================================="

# Определяем дистрибутив
if [ -f /etc/os-release ]; then
    . /etc/os-release
    DISTRO=$ID
else
    echo "Не удалось определить дистрибутив"
    exit 1
fi

echo "Обнаружен дистрибутив: $DISTRO"

# Установка пакетов в зависимости от дистрибутива
case $DISTRO in
    ubuntu|debian|linuxmint)
        echo "Установка пакетов для Ubuntu/Debian..."
        sudo apt-get update
        sudo apt-get install -y \
            build-essential \
            nasm \
            qemu-system-x86 \
            grub-pc-bin \
            grub-common \
            xorriso \
            mtools \
            gcc \
            g++ \
            make \
            binutils \
            gdb \
            git
        ;;
    
    arch|manjaro)
        echo "Установка пакетов для Arch Linux..."
        sudo pacman -Sy --needed --noconfirm \
            base-devel \
            nasm \
            qemu-full \
            grub \
            libisoburn \
            mtools \
            gcc \
            make \
            binutils \
            gdb \
            git
        ;;
    
    fedora|rhel|centos)
        echo "Установка пакетов для Fedora/RHEL..."
        sudo dnf install -y \
            gcc \
            gcc-c++ \
            make \
            nasm \
            qemu-system-x86 \
            grub2-tools \
            xorriso \
            mtools \
            binutils \
            gdb \
            git
        ;;
    
    *)
        echo "Неподдерживаемый дистрибутив: $DISTRO"
        echo "Установи вручную: nasm, qemu-system-x86, grub, xorriso, mtools, gcc, make"
        exit 1
        ;;
esac

echo ""
echo "=================================="
echo "Проверка установленных инструментов"
echo "=================================="

# Проверка версий
echo -n "NASM: "
nasm -v || echo "ОШИБКА: NASM не установлен"

echo -n "QEMU: "
qemu-system-i386 --version | head -n1 || echo "ОШИБКА: QEMU не установлен"

echo -n "GCC: "
gcc --version | head -n1 || echo "ОШИБКА: GCC не установлен"

echo -n "LD: "
ld --version | head -n1 || echo "ОШИБКА: LD не установлен"

echo -n "GRUB: "
grub-mkrescue --version || echo "ОШИБКА: GRUB не установлен"

echo -n "xorriso: "
xorriso --version 2>&1 | head -n1 || echo "ОШИБКА: xorriso не установлен"

echo ""
echo "=================================="
echo "Создание структуры проекта"
echo "=================================="

# Создаём структуру директорий
mkdir -p boot
mkdir -p kernel/arch/i386
mkdir -p kernel/drivers
mkdir -p kernel/fs
mkdir -p kernel/memory
mkdir -p kernel/process
mkdir -p gui
mkdir -p libc
mkdir -p userspace
mkdir -p build
mkdir -p iso/boot/grub
mkdir -p scripts

echo "Структура проекта создана"

echo ""
echo "=================================="
echo "Создание тестового файла"
echo "=================================="

# Создаём простой тестовый файл для проверки компилятора
cat > test_compiler.c << 'EOF'
#include <stdint.h>

void test_function() {
    volatile uint32_t x = 42;
    (void)x;
}
EOF

echo "Компиляция тестового файла..."
gcc -m32 -ffreestanding -c test_compiler.c -o test_compiler.o 2>&1

if [ $? -eq 0 ]; then
    echo "✓ Компилятор работает корректно"
    rm -f test_compiler.c test_compiler.o
else
    echo "✗ Ошибка компиляции"
    echo ""
    echo "Возможно, нужно установить поддержку 32-bit:"
    if [ "$DISTRO" = "ubuntu" ] || [ "$DISTRO" = "debian" ]; then
        echo "  sudo apt-get install gcc-multilib g++-multilib"
    elif [ "$DISTRO" = "arch" ]; then
        echo "  sudo pacman -S lib32-gcc-libs"
    fi
    exit 1
fi

echo ""
echo "=================================="
echo "✓ УСТАНОВКА ЗАВЕРШЕНА УСПЕШНО"
echo "=================================="
echo ""
echo "Все инструменты установлены и готовы к работе!"
echo ""
echo "Следующий шаг: запусти 'make' для сборки проекта"
echo ""

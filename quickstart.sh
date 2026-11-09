#!/bin/bash

# Быстрый старт MyOS
# Этот скрипт выполняет полную настройку окружения

set -e

echo ""
echo "╔════════════════════════════════════════╗"
echo "║     MyOS - Быстрый старт проекта       ║"
echo "╚════════════════════════════════════════╝"
echo ""

# Проверяем, запущен ли скрипт из корня проекта
if [ ! -f "setup.sh" ]; then
    echo "❌ Ошибка: запусти этот скрипт из корня проекта MyOS"
    exit 1
fi

# Шаг 1: Установка зависимостей
echo "📦 Шаг 1/4: Установка зависимостей..."
echo ""
chmod +x setup.sh
./setup.sh

if [ $? -ne 0 ]; then
    echo ""
    echo "❌ Ошибка установки зависимостей"
    exit 1
fi

echo ""
echo "✓ Зависимости установлены"
echo ""

# Шаг 2: Проверка окружения
echo "🔍 Шаг 2/4: Проверка окружения..."
echo ""
chmod +x scripts/check_environment.sh
./scripts/check_environment.sh

if [ $? -ne 0 ]; then
    echo ""
    echo "❌ Проверка окружения не пройдена"
    exit 1
fi

echo ""
echo "✓ Окружение проверено"
echo ""

# Шаг 3: Создание недостающих директорий
echo "📁 Шаг 3/4: Создание структуры проекта..."
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

echo "✓ Структура проекта создана"
echo ""

# Шаг 4: Информация о следующих шагах
echo "📋 Шаг 4/4: Готово к разработке!"
echo ""
echo "╔════════════════════════════════════════╗"
echo "║  ✓ ЭТАП 1 ЗАВЕРШЁН УСПЕШНО             ║"
echo "╚════════════════════════════════════════╝"
echo ""
echo "Структура проекта:"
echo "  ✓ boot/         - Загрузчик"
echo "  ✓ kernel/       - Ядро ОС"
echo "  ✓ gui/          - Графический интерфейс"
echo "  ✓ libc/         - Стандартная библиотека"
echo "  ✓ userspace/    - Пользовательские программы"
echo "  ✓ build/        - Скомпилированные файлы"
echo "  ✓ scripts/      - Скрипты сборки"
echo ""
echo "Доступные команды:"
echo "  make info       - Информация о проекте"
echo "  make            - Собрать проект"
echo "  make run        - Собрать и запустить в QEMU"
echo "  make clean      - Очистить проект"
echo ""
echo "Следующий этап:"
echo "  → ЭТАП 2: Загрузчик (Bootloader)"
echo ""
echo "Готов продолжить? (да/нет)"

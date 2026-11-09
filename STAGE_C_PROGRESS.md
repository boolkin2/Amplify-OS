# Этап C — реализация начата

Дата: 2026-09-14

## Реализовано в этой итерации

### Beacon

- Новый модуль `beacon.c/.h` поверх WM.
- Полноэкранный backdrop и Pearl-панель, анимированное появление 260 ms.
- Группы Threads / Facets & Strata / Do it now.
- Реальная модель элементов: unread, visible, selected.
- Клавиатура: стрелки/Tab, Enter/Space отметить прочитанным, D/Delete скрыть, C очистить, Esc закрыть.
- Мышь: выбор элемента, Clear и закрытие кликом вне панели.
- Открытие по `Super+N` и из status strip Apex.

### Constellation

- Новый модуль `constellation.c/.h` поверх WM.
- Снимок списка видимых окон через `wm_iterate`.
- Live thumbnails читаются напрямую из compositor/window buffers.
- Выбор стрелками/Tab, Enter/Space поднимает окно через `wm_focus`, Esc закрывает.
- Mouse selection и анимация появления 220 ms.
- Открытие по `Super+Tab`.

### Keyboard / WM

- Расширен PS/2 parser: Left/Right/Up/Down и Super modifier.
- Добавлены системные команды Super+N, Super+Tab, Super+Space и Super/Home.
- Beacon и Constellation встроены над окнами и обычным overlay, но под курсором.
- Motion activity обоих overlay удерживает интерактивную частоту кадров.
- Новые модули включены в `Makefile.x86_64`.

## Проверено

- Изменённые модули компилируются freestanding GCC-флагами с `-Wall -Wextra -Werror`.
- Полная ISO/QEMU golden-screen проверка пока невозможна в sandbox: отсутствуют NASM, QEMU и `grub-mkrescue`.

## Остаток полного этапа C

1. Довести Ignite до точной композиции референса и проверить boot progress states.
2. Перестроить Identity в split-layout, добавить user selector и power actions.
3. Финально откалибровать Apex по PNG после первого доступного QEMU screenshot.
4. Визуально откалибровать Beacon и Constellation на 1600x1000, 1920x1080 и 1280x800.
5. Проверить все клики, keyboard paths и 50-кратное открытие/закрытие overlay.

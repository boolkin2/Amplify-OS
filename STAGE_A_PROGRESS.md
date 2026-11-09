# Этап A — код завершён

Дата: 2026-09-14

## Реализовано

- Логическое полотно 1600×1000 с aspect-preserving fixed-point scaling, safe offsets и обратным hit testing.
- Проверены расчёты для 1600×1000, 1920×1080 и 1280×800.
- Runtime Pearl theme: light/dark, accent tokens и reduced-motion flag.
- Общие компоненты: app shell, rail, status strip, cards, buttons, metrics, ring.
- Apex dashboard заменил классический desktop: greeting, focus, agenda, clock hub, radial actions и system pulse.
- Левая rail 64 px и верхняя status strip.
- Основной режим больше не использует нижнюю taskbar; legacy start menu оставлен через кнопку `+`.
- Hover, keyboard focus, Tab, Shift+Tab, 1–9, Enter, Space и Escape.
- Нижние rail actions: Identity modal, light/dark theme, legacy launcher.
- Status strip открывает системный drawer.
- Прозрачный текстовый renderer устранил чёрные прямоугольники.
- Масштабируемый bitmap renderer используется для крупных заголовков до внедрения TTF atlas.
- Удалены ошибочные малиновые alpha-артефакты.
- Ложные переходы в неподходящие legacy-приложения отключены.

## Проверка

Все изменённые C-модули компилируются freestanding-флагами и `-Werror`. Unit tests layout/hit testing проходят. Make dependency graph валиден.

## Внешняя проверка

Golden screenshot и загрузочный тест требуют NASM/GRUB/QEMU. Установка пакетов в текущем sandbox заблокирована отключённым DNS и отсутствием root package cache. Это ограничение среды, а не незавершённый участок C-кода.

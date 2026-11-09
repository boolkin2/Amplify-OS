# Этап B — код завершён

Дата: 2026-09-14

## Реализовано

### Оконная модель

- `WINDOW_CHROME_CLASSIC`, `WINDOW_CHROME_BORDERLESS`, `WINDOW_CHROME_WORKSPACE`.
- `window_set_chrome`, `window_title_height`, `window_resize`, `window_enter_workspace`.
- Resize с overflow guard и полной синхронизацией content target.
- Hit testing и координаты событий без зависимости от обязательной title bar.
- Отдельные fallback/compositor paths для borderless и workspace.
- Workspace surfaces не получают legacy title bar, cyan border, shadow и красную системную кнопку.

### Motion

- Общий `ui_motion` controller.
- Linear, ease-out cubic и ease-in-out.
- Duration, delay и stagger-ready transition objects.
- Reduced-motion отключает переходы глобально.
- Window open/close: 220/180 ms.
- Pulse использует slide-in; overlay drawer использует slide/fade.

### Overlay layer

- Общий слой `ui_overlay` над окнами и меню, под курсором.
- Drawer, modal и toast.
- Mouse capture, Escape dismiss и keyboard blocking для modal/drawer.
- Overlay motion учитывается WM, поэтому анимация не ограничивается idle FPS.

### Pulse workspace

- Полноэкранная рабочая область справа от rail и ниже status strip.
- Собственная sidebar, header, output/editor card и три metric cards.
- Prompt закреплён внутри основной карточки.
- Сохранены `shell_exec_line` и framebuffer console hooks.
- Back и Escape запускают compositor close transition.
- Адаптивная геометрия по framebuffer.
- После входной анимации continuous redraw отключается.

### Keyboard/focus

- Keyboard driver кодирует Shift+Tab как `KBD_KEY_BACKTAB`.
- Экспортированы состояния Shift/Ctrl.
- Rail поддерживает прямую навигацию, forward/backward focus и activation.

## Проверка

С `-Werror` компилируются graphics, keyboard, layout, theme, components, motion, overlay, desktop, window, WM, compositor, apps и kernel main. Layout/hit tests проходят. Make graph содержит новые модули.

## Внешняя проверка

Полная ISO-сборка и QEMU screenshot требуют отсутствующих NASM/GRUB/QEMU. Команды установки были выполнены, но репозитории недоступны из-за отключённого DNS. После появления toolchain первым действием выполнить `make ARCH=x86_64`, boot и golden screenshot comparison.

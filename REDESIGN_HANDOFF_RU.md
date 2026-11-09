# AMPLIFY-OS Pearl — план полного редизайна по 17 референсам

> **Статус 2026-09-14:** Код Этапов A-E завершён. Этап E добавляет Signal, Arc, Resonance, Core, Facet/Palette, Mesh и Capacity с реальными RTC/audio/network/VFS данными, действиями и FAT32/RAM fallback. Подробности: `STAGE_A_PROGRESS.md` ... `STAGE_E_PROGRESS.md`. Следующий этап — F / общая golden-screen приёмка и полировка всех 17 состояний. ISO/QEMU golden-screen validation остаётся обязательной внешней проверкой, поскольку текущая среда не содержит NASM/GRUB/QEMU.

## Цель

Довести реальную x86_64 AMPLIFY-OS до максимально близкого визуального и функционального соответствия 17 изображениям из `ui_reference/`. Не делать HTML-макет вместо ОС: весь интерфейс должен отрисовываться существующим framebuffer/compositor/WM и работать в QEMU/на железе.

## Строгое правило для следующего ИИ

Не объявлять экран готовым без: (1) сборки, (2) загрузки QEMU, (3) скриншота 1600×1000, (4) визуального сравнения с соответствующим PNG, (5) проверки кликов и клавиатуры. Нельзя удалять рабочие подсистемы ядра ради красивого мокапа.

## Что уже есть в исходнике

- x86_64 ядро, framebuffer, mouse/keyboard, WM, compositor и анимации открытия/закрытия.
- Светлая тема Pearl в `kernel/arch/x86_64/ui_theme.h`.
- Растровые иконки и логотип: `ui_icons.c/.h`, генератор `tools/gen_ui_icons.py`.
- Splash/login/desktop/start menu.
- Приложения: About, System Info, Clock, Matrix, Calculator, Notes, Terminal, Files, Paint, Embers, Browser, Task Manager, 3D Cube, Amply Center, Sandbox Manager.
- Сетевой стек, HTTP/HTTPS, FAT32/VFS, RTC, audio, processes/cgroups.

## Главные расхождения

1. Текущий desktop остаётся классическим рабочим столом с ярлыками и нижней панелью. Референс Apex — это dashboard с левой вертикальной навигацией, верхней status-панелью, центральными часами и карточками.
2. Окна имеют универсальный старый каркас; референсы используют почти полноэкранные app-shell экраны без отдельной тяжёлой title bar.
3. Типографика 8×16 слишком грубая. Для сходства нужен TTF rasterizer/cache или заранее сгенерированные размеры 12/14/16/20/28/44.
4. Нет семи ключевых экранов/приложений: Signal, Arc, Resonance, Core/Palette, Mesh, Capacity, Beacon, а также полноценного Constellation task switcher.
5. Terminal визуально не соответствует Pulse и местами использует тёмный текст темы на тёмном фоне.
6. Имеющиеся анимации покрывают только окна. Нужны переходы страниц, появление карточек, ring/equalizer/progress/count-up, drawer и task switcher.

## Карта 17 референсов

| № | Файл | Реализация | Состояние |
|---|---|---|---|
| 01 | `AmplifyOS_01_ignite.png` | Ignite: splash/boot | переработать `splash.c` |
| 02 | `AmplifyOS_02_apex.png` | Apex: домашний dashboard | заменить desktop layout |
| 03 | `AmplifyOS_03_identity.png` | Identity: login/lock | переработать `login.c` |
| 04 | `AmplifyOS_04_strata.png` | Strata: файловый менеджер | переработать Files |
| 05 | `AmplifyOS_05_pulse.png` | Pulse: консоль/runner | переписать Terminal UI |
| 06 | `AmplifyOS_06_facet.png` | Facet: обои и темы | новое приложение |
| 07 | `AmplifyOS_07_thread.png` | Thread: заметки | переработать Notes |
| 08 | `AmplifyOS_08_signal.png` | Signal: уведомления/инбокс | новое приложение |
| 09 | `AmplifyOS_09_arc.png` | Arc: календарь/таймлайн | новое приложение |
| 10 | `AmplifyOS_10_resonance.png` | Resonance: аудиоплеер | новое приложение поверх audio |
| 11 | `AmplifyOS_11_core.png` | Core: настройки | новое приложение |
| 12 | `AmplifyOS_12_mesh.png` | Mesh: сеть | новое приложение поверх netif/DHCP/DNS |
| 13 | `AmplifyOS_13_capacity.png` | Capacity: диски/хранилище | новое приложение поверх VFS/FAT32 |
| 14 | `AmplifyOS_14_palette.png` | Palette: внешний вид | новое приложение/вкладка Core |
| 15 | `AmplifyOS_15_beacon.png` | Beacon: центр уведомлений | новый правый drawer/modal |
| 16 | `AmplifyOS_16_constellation.png` | Constellation: task switcher | новый overlay в WM |
| 17 | `AmplifyOS_17_amplifier.png` | Amplifier: системный монитор | переработать Task Manager/System Info |

## Архитектура UI, которую нужно сделать первой

### 1. Масштабирование

Референсы имеют 1600×1000. Ввести логическое полотно 1600×1000 и масштаб:

```c
sx = framebuffer_width / 1600.0;
sy = framebuffer_height / 1000.0;
scale = min(sx, sy);
offset_x = (framebuffer_width - 1600 * scale) / 2;
offset_y = (framebuffer_height - 1000 * scale) / 2;
```

Добавить `ui_layout.c/.h`: `ux()`, `uy()`, `uw()`, `uh()`, safe area, hit-test в логических координатах. Никаких разрозненных magic numbers в приложениях.

### 2. Design tokens

Расширить `ui_theme.h`:

- canvas `#F7EEF4`, surface `#FFFDFE`, soft `#F1E5ED`;
- ink `#241F27`, secondary `#756C77`, border `#E7D9E2`;
- gradient orange `#F0A33A` → coral `#E8785D` → berry `#B54478`;
- dark canvas `#151019`, surface `#211923`, border `#352A36`, text `#F8F3F7`;
- radii 8/12/18/26; spacing 4/8/12/16/24/32/48/64;
- card shadows only where visible in references.

Сделать runtime theme struct (light/dark/accent/wallpaper), а не только compile-time macros. Palette/Facet должны менять тему без перезагрузки.

### 3. Общие компоненты

Добавить `ui_components.c/.h`:

- `ui_app_shell()` — левая rail + top status;
- `ui_card()`, `ui_section_label()`, `ui_metric()`;
- primary/secondary/icon buttons с hover/pressed/disabled;
- tabs, segmented control, chip, list row;
- circular progress/ring chart;
- modal/drawer/toast;
- scrollbar, text field, context menu;
- skeleton/empty/error states.

Все 17 экранов должны собираться из этих компонентов.

### 4. Типографика

Приоритетный вариант: простой TTF rasterizer + glyph cache. Если это слишком рискованно для ядра — сгенерировать офлайн bitmap atlas из `initrd_content/default.ttf` для размеров 12, 14, 16, 20, 28, 44 и подключить kerning/UTF-8→CP1251 fallback. Заголовки в референсах нельзя рисовать одним 8×16 шрифтом.

### 5. Иконки

Перегенерировать единый 24/32/48 набор в outline-стиле р��ференса. Убрать буквенные заглушки. Добавить пиктограммы всех 17 модулей и состояний rail. Логотип использовать из текущих assets, не подменять псевдологотипом.

## ��лан реализации по этапам

### Этап A — базовая оболочка

1. `ui_layout` + logical coordinates.
2. Runtime theme + общий app shell.
3. Новая левая rail 64 px: logo, Home, Strata, Pulse, Facet, Thread, Signal, Arc, Core; снизу lock/settings/add.
4. Верхняя status strip справа: user, network, battery/VM, time.
5. Убрать нижн��ю taskbar из основного режима; оставить как compatibility option.
6. Добавить hover/pressed/focus и клавиатурную навигацию.

**Готово, когда:** Apex визуально совпадает по композиции, rail и status bar кликабельны, 1600×1000 и 1920×1080 работают.

### Этап B — окна и анимации

1. В `window.c/compositor.c` добавить borderless app-shell окна и fullscreen workspace mode.
2. Анимации 180–240 ms, ease-out cubic: app enter/exit, rail selection, cards stagger 20–40 ms.
3. Drawer Beacon: 260 ms slide + backdrop fade.
4. Constellation: scale 0.92→1, rotation/translation только у карточек, не у текста.
5. `prefers-reduced-motion` аналог в Settings.
6. Dirty rectangles для animated widgets; не форсировать full redraw без необходимости.

### Этап C — системные экраны

- **Ignite:** тёмный экран, большой watermark A, точный логотип, тонкая progress line и подписи boot status.
- **Identity:** светлые обои, крупное время слева снизу, login card справа, password field, user selector, shutdown/restart.
- **Apex:** приветствие, focus card, agenda, central clock ring, 8 radial quick actions, нижние status cards.
- **Beacon:** overlay поверх Apex/любого app, список grouped notifications, clear/read/actions.
- **Constellation:** получить видимые окна через `wm_iterate`, отрисовать live thumbnails из compositor buffers, keyboard arrows/Enter/Esc.

### Этап D — существующие приложения

- **Strata/Files:** группы Mine/Shared/Cold archive, grid rows, detail inspector, search, create folder, rename, delete confirmation, preview.
- **Pulse/Terminal:** см. отдельное ТЗ ниже.
- **Thread/Notes:** vertical note cards, pinned/recent, editor, autosave в FAT32, search.
- **Amplifier:** объединить Task Manager + System Info; CPU/RAM/storage triangles, uptime/build card, process/resource bars.

### Этап E — новые приложения

1. `app_signal.c`: модель notification/message item, tabs Needs you/Later/Settled, actions read/dismiss/snooze.
2. `app_arc.c`: day timeline, events, focus ring, add/edit/delete event, RTC date.
3. `app_resonance.c`: WAV/PCM library from VFS, play/pause/seek/volume/queue, animated spectrum from PCM samples.
4. `app_core.c`: Appearance/Mesh/Strata/Search/Resonance/Pulse/Arc/Prisms settings tiles.
5. `app_facet.c`: wallpaper/theme grid, preview inspector, active theme persistence.
6. `app_mesh.c`: interface card, link speed, IP/gateway/DNS/DHCP, renew/test, topology nodes.
7. `app_capacity.c`: total/free/used, file-type categories, biggest files, cleanup action.

Добавить каждый `.c` в `C_SRC` Makefile.x86_64, прототип в `apps.h`, launcher в `kernel64_main.c`, icon mapping в `ui_icons.c`.

## Отдельное ТЗ: Pulse/консоль

Референс `05_pulse` — тёмный app-shell:

- слева rail; внутри приложения — narrow task/project sidebar;
- сверху крупный заголовок “Say the outcome.” и вторичная строка;
- основной card редактора/вывода с моноширинным шрифтом;
- command prompt закреплён снизу card, не отдельная нижняя полоса окна;
- справа gradient primary button;
- снизу три metric cards: completed, elapsed, failures;
- фон `#151019`, surfaces `#211923`, border `#352A36`, основной текст `#F8F3F7`, muted `#A99FAA`;
- prompt/selection/accent — orange→berry gradient;
- курсор мигает 530 ms; появление строк 60–100 ms; spinner для долгих команд; плавный scroll.

Функциональность, которую нельзя потерять: `shell_exec_line`, console hooks, history, VFS/network commands. Добавить history Up/Down, cursor Left/Right, Home/End, Ctrl+L, Ctrl+C, tab completion, selection/copy buffer, scroll wheel, exit code и timestamp. Исправить все места, где `UI_FG_PRIMARY` (тёмный для light theme) рисуется на dark background.

## Данные и persistence

Создать `/system/ui/config.ini`, `/home/<user>/notes/`, `/home/<user>/calendar.db`, `/system/notifications.log`, `/system/audio/library.idx`. При недоступном writable FAT32 работать в RAM и явно показывать статус “Not saved”.

## Интеграция ввода

- `Super`: Apex/Home.
- `Super+Space`: Pulse.
- `Super+Tab`: Constellation.
- `Super+N`: Beacon.
- `Esc`: закрыть overlay/modal.
- Tab/Shift+Tab/Enter/Space — обязательны для focusable controls.
- Hitboxes не меньше 40×40 логических px.

## Проверка визуального соответствия

Для каждого экрана:

1. Запустить QEMU в 1600×1000.
2. Перейти в нужный app/state и сделать PNG.
3. Запустить `python3 tools/visual_compare.py reference.png actual.png out_prefix`.
4. Сначала исправлять layout (позиции/размеры), затем typography, затем цвет/детали.
5. Допуск финального layout: ключевые блоки ≤8 px; средняя цветовая ошибка ≤10%; никакого overflow/overlap.
6. Отдельно проверить 1920×1080 и 1280×800.

## Тестовая матрица

- clean build x86_64;
- boot BIOS и UEFI;
- mouse hit testing после масштабирования;
- keyboard focus/shortcuts;
- open/close 50 раз без leak/crash;
- переключение темы во время работы;
- Files/Notes persistence после reboot;
- Mesh без адаптера и с DHCP;
- Resonance без audio device;
- Capacity без FAT32;
- reduced motion;
- screenshot всех 17 состояний.

## Рекомендуемый порядок коммитов

1. `ui: add logical layout and runtime tokens`
2. `ui: add shared Pearl components and rail shell`
3. `ui: rebuild Apex Identity and Ignite`
4. `ui: add borderless workspace and transitions`
5. `app: rebuild Pulse terminal`
6. `app: rebuild Strata Thread and Amplifier`
7. `app: add Core Facet and Palette`
8. `app: add Signal and Beacon`
9. `app: add Arc and Resonance`
10. `app: add Mesh and Capacity`
11. `wm: add Constellation switcher`
12. `qa: add 17-screen golden screenshot suite`

## Copy-paste prompt для следующего ИИ

> Продолжи полный р��дизайн AMPLIFY-OS из этого архива. Сначала прочитай `CLAUDE.md`, затем `REDESIGN_HANDOFF_RU.md` и `ui_reference/reference_manifest.json`. Это реальная freestanding x86_64 ОС на C/ASM, а не web-макет. Референсы — 17 PNG 1600×1000 в `ui_reference/`; добивайся максимально близкой ко��позиции, цветов, размеров, типографики и поведения. Не уд��ляй работающие подсистемы. Дела�� этапы строго по порядку из handoff, начиная с незавершённого. После каждого экрана обязательно собери, загрузи QEMU, сделай screenshot, сравни через `tools/visual_compare.py`, исправь overflow/overlap и только затем отмечай готовым. Если не помещается весь объём, закончи один этап полностью, обнови этот handoff точным статусом и оставь следующий copy-paste prompt.

## Критерий полного завершения

Работа завершена только когда все 17 референсных состояний доступны из системы, все интерактивные элементы имеют реальные действия/данные или честные empty/error states, а golden screenshot suite и регрессионные тесты проходят. Статический рисунок интерфейса без работающих приложений завершением не считается.

---

## Этап F — типографика, единый лаунчер, движение, утечки (выполнен)

Подробности — в `STAGE_F_PROGRESS.md`. Коротко:

1. **Типографика.** Битмап 8×16 удалён. Добавлен движок `ui_font`
   (`tools/gen_ui_font.py` → `ui_font.h/.c` + `ui_font_data.c`): 8 начертаний
   (12/14/16/20/28/44 px пропорциональных + 14/16 px моно), 4-битное сглаживание,
   кернинг, синтетический bold, трекинг, ¼-пиксельное перо, CP1251 + UTF-8.
   Ширина текста теперь считается `gfx_string_width_ui()`, а не `strlen*8`.
2. **Панель задач и меню «Пуск» удалены** (`startmenu.{c,h}` удалены из дерева
   и сборки). Все пункты меню переехали в общий реестр.
3. **Core — единственный лаунчер.** `app_core.c` переписан: сетка 3×3 с пагинацией
   из `desktop_icon_count()/desktop_icon_at()`, живые метрики, клавиатура, двухшаговое
   подтверждение для Reboot/Shutdown. Реестр расширен до 24 записей,
   `DESKTOP_MAX_ICONS` поднят до 32 (было 16 при 19 регистрациях — три приложения
   молча терялись).
4. **Движение.** Каскадное появление плиток Core (200 мс, шаг 25 мс),
   подсветка рейла переезжает за 200 мс ease-out cubic,
   `ui_components_motion_active()` держит 50 FPS на время переезда,
   reduced motion уважается.
5. **Утечка памяти закрытых окон исправлена.** `window_destroy()` не вызывался
   нигде; теперь `reap_closed_windows()` в `wm.c` освобождает окно, его ARGB-буфер
   (до ~5,8 МБ) и состояние приложения после анимации закрытия; компоцитор и
   переключатель окон «забывают» указатель.

Проверка этапа: `bash scripts/test_stage_f_static.sh` (30 PASS) и
`bash scripts/test_stage_e_static.sh` (32 PASS, регрессий нет).
Визуальная приёмка в QEMU — см. раздел 8 в `STAGE_F_PROGRESS.md`.

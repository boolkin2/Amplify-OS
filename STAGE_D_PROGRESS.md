# Этап D — код завершён

Дата: 2026-09-14

## Strata

- Новый fullscreen workspace `app_strata_open` по композиции reference 04.
- Три слоя: Warm, Shared и Cold; карточки реальных VFS entries.
- Lens inspector с размером и текстовым preview через `vfs_open/read`.
- Поиск, keyboard/mouse selection и refresh.
- Создание каталога через `fat32_mkdir_p`.
- Реальное rename как безопасный copy/save/delete на FAT32.
- Двухшаговое подтверждение удаления.
- Честные empty/error/Not saved состояния.

## Pulse

- Сохранён `shell_exec_line` и framebuffer console hooks.
- Dark app-shell, project sidebar, outcome header, output/editor card и metrics.
- Добавлена история 16 команд с Up/Down.
- Редактирование cursor-aware: Left/Right, вставка в позицию, Backspace.
- Ctrl+L clear, Ctrl+C cancel, Tab completion, Esc back.
- Prompt остаётся внутри output card; сохранены сетевые/VFS/system команды.

## Thread

- Новый fullscreen workspace `app_thread_open` по reference 07.
- Continuous note/editor, pinned state, nearby threads и facets.
- Persistence в `/disk/Sanctum/notes/thread.txt`.
- Autosave после idle, Ctrl+S, save-on-exit и RAM fallback.
- Ctrl+F search с match/no-match состоянием.
- Реальный edit buffer, caret и status line.

## Amplifier

- Новый fullscreen `app_amplifier_open` по reference 17.
- Старые System Info и Task Manager launcher callbacks объединены в Amplifier.
- Live PMM/heap/task/uptime metrics.
- Triangle metrics, seven-turn load history, update card и prism-by-prism task rows.
- Task data берутся из `task_iterate`, память из `pmm_get_stats`/`heap_get_stats`.

## Интеграция

- `app_stage_d.c` включён в `Makefile.x86_64`.
- `apps.h` экспортирует Strata, Thread и Amplifier.
- Desktop/start menu routes перенаправлены на новые workspace implementations.
- Старые рабочие приложения оставлены для ABI/regression compatibility.

## Проверка

- `scripts/test_stage_d_static.sh`: 25/25 PASS.
- Все затронутые C-модули компилируются freestanding GCC с `-Wall -Wextra -Werror`.
- Make graph проходит dry-run; conflict markers отсутствуют.

## Ограничение среды

Статус означает code-complete. BIOS/UEFI QEMU boot, screenshots, click-through и golden comparison остаются обязательной внешней приёмкой: в sandbox отсутствуют NASM, QEMU и `grub-mkrescue`.

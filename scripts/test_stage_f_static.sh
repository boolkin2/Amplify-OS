#!/usr/bin/env bash
# ----------------------------------------------------------------------------
# Статический прогон этапа F (Pearl-редизайн: типографика, снос taskbar/«Пуск»,
# Core как единственный лаунчер, движение, утечки окон).
#
# Скрипт НЕ требует QEMU/NASM/GRUB: он проверяет инварианты исходников и
# компилирует затронутые модули теми же флагами, что и сборка ядра, но
# дополнительно с -Wall -Wextra -Werror.
# ----------------------------------------------------------------------------
set -euo pipefail
cd "$(dirname "$0")/.."
pass=0
check(){ if eval "$2"; then printf '[PASS] %s\n' "$1"; pass=$((pass+1)); else printf '[FAIL] %s\n' "$1"; exit 1; fi; }

K=kernel/arch/x86_64

# --- 1. Типографика -----------------------------------------------------------
check 'ui_font.c / ui_font.h на месте'        "test -s $K/ui_font.c && test -s $K/ui_font.h"
check 'атлас ui_font_data.c сгенерирован'     "test -s $K/ui_font_data.c"
check 'генератор шрифта в репозитории'        "test -s tools/gen_ui_font.py"
check 'битмап 8x16 удалён'                    "! test -e $K/font8x16.c && ! test -e $K/font8x16.h"
check 'шрифт в сборке'                        "grep -q 'ui_font.c' Makefile.x86_64 && grep -q 'ui_font_data.c' Makefile.x86_64"
check 'font8x16 больше не собирается'         "! grep -q 'font8x16' Makefile.x86_64"
check 'пропорциональная метрика текста'       "grep -q 'gfx_string_width_ui' $K/graphics.h"
check 'кернинг + синтетический bold'          "grep -q 'kern' $K/ui_font.c && grep -q 'ui_font_draw_bold' $K/ui_font.c"
check 'моноширинный путь для терминала'       "grep -q 'ui_font_draw_cell' $K/ui_font.c"

# --- 2. Снос taskbar и меню «Пуск» --------------------------------------------
check 'startmenu.{c,h} удалены'               "! test -e $K/startmenu.c && ! test -e $K/startmenu.h"
check 'нет ссылок на startmenu (x86_64)'      "! grep -rq 'startmenu' $K kernel/kernel64_main.c Makefile.x86_64"
check 'нет рисования taskbar'                 "! grep -rq 'draw_taskbar\|TASKBAR_H' $K"
check 'десктоп рисует подсказку вместо панели' "grep -q 'Core holds every prism' $K/desktop.c"

# --- 3. Единый реестр запуска (Core) ------------------------------------------
check 'реестр экспортирован'                  "grep -q 'desktop_icon_count' $K/desktop.h && grep -q 'desktop_icon_at' $K/desktop.h && grep -q 'desktop_launch_named' $K/desktop.h"
check 'вместимость реестра >= регистраций'    "test \$(grep -c 'desktop_add_icon(' kernel/kernel64_main.c) -le \$(sed -n 's/.*DESKTOP_MAX_ICONS *\\([0-9][0-9]*\\).*/\\1/p' $K/desktop.h | head -1)"
check 'Core строится из реестра'              "grep -q 'desktop_icon_count' $K/app_core.c && grep -q 'desktop_icon_at' $K/app_core.c"
check 'бывшие пункты «Пуск» в реестре'        "for n in Clock Matrix Tasks Reboot Shutdown; do grep -q \"\\\"\$n\\\"\" kernel/kernel64_main.c || exit 1; done"
check 'Core доступен с рейла десктопа'        "grep -q 'desktop_launch_named(\"Core\")' $K/desktop.c"
check 'подтверждение для Reboot/Shutdown'     "grep -q 'confirm' $K/app_core.c"

# --- 4. Движение ---------------------------------------------------------------
check 'плитки Core появляются с задержкой'    "grep -q 'ui_transition_start' $K/app_core.c"
check 'подсветка рейла едет, а не прыгает'    "grep -q 'ui_transition' $K/ui_components.c"
check 'reduced motion учитывается'            "grep -q 'ui_theme_reduced_motion' $K/ui_motion.c && grep -q 'reduced_motion' $K/compositor.c"

# --- 5. Жизненный цикл окон (утечка памяти) -----------------------------------
check 'WM реально уничтожает окна'            "grep -q 'reap_closed_windows' $K/wm.c && grep -q 'window_destroy(w)' $K/wm.c"
check 'владение состоянием приложения'        "grep -q 'owns_user_data' $K/window.h && grep -q 'owns_user_data' $K/window.c"
check 'teardown освобождает буфер и state'    "grep -q 'kfree(w->user_data)' $K/window.c"
check 'компоцитор забывает окно'              "grep -q 'compositor_forget_window' $K/compositor.c && grep -q 'compositor_forget_window' $K/compositor.h"
check 'constellation забывает окно'           "grep -q 'constellation_forget_window' $K/constellation.c && grep -q 'constellation_forget_window' $K/constellation.h"
check 'приложения помечают владение state'    "test \$(grep -c 'owns_user_data' $K/apps.c) -ge 9"

# --- 6. Гигиена ----------------------------------------------------------------
check 'нет конфликтных маркеров'              "! grep -R -nE '^(<<<<<<<|=======|>>>>>>>)' $K Makefile.x86_64"

# --- 7. Компиляция затронутых модулей -----------------------------------------
FLAGS='-m64 -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -finput-charset=UTF-8 -fexec-charset=CP1251 -Ikernel'
OUT=/tmp/amplify-stage-f-objects
mkdir -p "$OUT"
for f in ui_font ui_font_data graphics fb gui desktop ui_components ui_motion splash window wm compositor constellation app_core app_stage_e apps; do
  gcc $FLAGS -c "$K/$f.c" -o "$OUT/$f.o"
done
gcc $FLAGS -Ikernel -c kernel/kernel64_main.c -o "$OUT/kernel64_main.o"
check 'все модули этапа F компилируются'      "test -s $OUT/apps.o && test -s $OUT/kernel64_main.o"

printf 'Stage F static suite: %d PASS\n' "$pass"

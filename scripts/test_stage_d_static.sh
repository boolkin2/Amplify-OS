#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CFLAGS=(-m64 -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -finput-charset=UTF-8 -fexec-charset=CP1251 -Ikernel)
for f in kernel/arch/x86_64/app_stage_d.c kernel/arch/x86_64/apps.c kernel/arch/x86_64/wm.c kernel/arch/x86_64/desktop.c kernel/kernel64_main.c; do gcc "${CFLAGS[@]}" -c "$f" -o "/tmp/stage-d-$(basename "$f" .c).o"; done
checks=(
'kernel/arch/x86_64/app_stage_d.c:app_strata_open' 'kernel/arch/x86_64/app_stage_d.c:strata_rename' 'kernel/arch/x86_64/app_stage_d.c:fat32_mkdir_p' 'kernel/arch/x86_64/app_stage_d.c:Press D again'
'kernel/arch/x86_64/app_stage_d.c:app_thread_open' 'kernel/arch/x86_64/app_stage_d.c:THREAD_PATH' 'kernel/arch/x86_64/app_stage_d.c:autosave' 'kernel/arch/x86_64/app_stage_d.c:match found'
'kernel/arch/x86_64/apps.c:shell_exec_line' 'kernel/arch/x86_64/apps.c:history[16]' 'kernel/arch/x86_64/apps.c:KBD_KEY_LEFT' 'kernel/arch/x86_64/apps.c:KBD_KEY_UP' 'kernel/arch/x86_64/apps.c:Ctrl+L'
'kernel/arch/x86_64/app_stage_d.c:app_amplifier_open' 'kernel/arch/x86_64/app_stage_d.c:task_iterate' 'kernel/arch/x86_64/app_stage_d.c:pmm_get_stats' 'kernel/arch/x86_64/app_stage_d.c:heap_get_stats'
'kernel/kernel64_main.c:app_amplifier_open' 'Makefile.x86_64:app_stage_d.c')
for item in "${checks[@]}";do file=${item%%:*};needle=${item#*:};grep -Fq "$needle" "$file";done
make -n ARCH=x86_64 all >/tmp/stage-d-make-plan.txt
! grep -R -nE '^(<<<<<<<|=======|>>>>>>>)' kernel/arch/x86_64 kernel/kernel64_main.c
echo 'Stage D static checks: 25/25 PASS'

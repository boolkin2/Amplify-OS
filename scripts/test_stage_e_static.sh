#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
pass=0
check(){ if eval "$2"; then printf '[PASS] %s\n' "$1"; pass=$((pass+1)); else printf '[FAIL] %s\n' "$1"; exit 1; fi; }
for app in signal arc resonance core facet mesh capacity; do
 check "app_${app}.c exists" "test -s kernel/arch/x86_64/app_${app}.c"
 check "app_${app}_open exported" "grep -q 'app_${app}_open' kernel/arch/x86_64/apps.h"
 check "app_${app} in build" "grep -q 'app_${app}.c' Makefile.x86_64"
done
check 'Signal actions' "grep -q 'snooze' kernel/arch/x86_64/app_signal.c && grep -q 'dismiss' kernel/arch/x86_64/app_signal.c"
check 'Arc CRUD keys' "grep -q 'calendar.db' kernel/arch/x86_64/app_arc.c && grep -q 'Edited focus band' kernel/arch/x86_64/app_arc.c"
check 'Resonance real audio' "grep -q 'audio_play_wav' kernel/arch/x86_64/app_resonance.c && grep -q 'audio_set_volume' kernel/arch/x86_64/app_resonance.c"
# Этап F: Core больше не вызывает app_*_open напрямую — он строится из реестра
# десктопа и запускает приложения через desktop_icon_at()->on_click.
check 'Core routes settings (registry)' "grep -q 'desktop_icon_at' kernel/arch/x86_64/app_core.c && grep -q '\"Mesh\"' kernel/kernel64_main.c && grep -q '\"Facet\"' kernel/kernel64_main.c"
check 'Facet runtime theme' "grep -q 'ui_theme_set_accent' kernel/arch/x86_64/app_facet.c && grep -q 'ui_theme_set_dark' kernel/arch/x86_64/app_facet.c"
check 'Mesh live stack' "grep -q 'dhcp_start' kernel/arch/x86_64/app_mesh.c && grep -q 'dns_resolve' kernel/arch/x86_64/app_mesh.c"
check 'Capacity real VFS metrics' "grep -q 'vfs_count' kernel/arch/x86_64/app_capacity.c && grep -q 'fat32_delete_file' kernel/arch/x86_64/app_capacity.c"
check 'Launcher callbacks' "grep -q 'launch_signal' kernel/kernel64_main.c && grep -q 'launch_capacity' kernel/kernel64_main.c"
check 'Pearl icon mappings' "grep -q '\"Signal\"' kernel/arch/x86_64/ui_icons.c && grep -q '\"Capacity\"' kernel/arch/x86_64/ui_icons.c"
check 'No conflict markers' "! grep -R -nE '^(<<<<<<<|=======|>>>>>>>)' kernel/arch/x86_64 Makefile.x86_64"
FLAGS='-m64 -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -finput-charset=UTF-8 -fexec-charset=CP1251 -Ikernel'
mkdir -p /tmp/amplify-stage-e-objects
for f in app_stage_e app_signal app_arc app_resonance app_core app_facet app_mesh app_capacity; do gcc $FLAGS -c kernel/arch/x86_64/$f.c -o /tmp/amplify-stage-e-objects/$f.o; done
check 'All Stage E modules compile cleanly' "test -s /tmp/amplify-stage-e-objects/app_capacity.o"
printf 'Stage E static suite: %d PASS\n' "$pass"

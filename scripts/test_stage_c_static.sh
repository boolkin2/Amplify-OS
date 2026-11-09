#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
CFLAGS=(-m64 -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone -mcmodel=kernel -nostdlib -fno-builtin -Wall -Wextra -Werror -O2 -finput-charset=UTF-8 -fexec-charset=CP1251 -Ikernel)
files=(splash login desktop keyboard beacon constellation wm)
pass=0
for name in "${files[@]}"; do
  gcc "${CFLAGS[@]}" -c "kernel/arch/x86_64/$name.c" -o "/tmp/amplify-stage-c-$name.o"
  pass=$((pass+1))
done
checks=(
  'kernel/arch/x86_64/splash.c:IGNITING CORE'
  'kernel/arch/x86_64/login.c:power_reboot_with_curtain'
  'kernel/arch/x86_64/login.c:users_at'
  'kernel/arch/x86_64/desktop.c:login_run'
  'kernel/arch/x86_64/desktop.c:beacon_show'
  'kernel/arch/x86_64/beacon.c:BEACON_COUNT'
  'kernel/arch/x86_64/constellation.c:wm_iterate'
  'kernel/arch/x86_64/constellation.c:w->buffer'
  'kernel/arch/x86_64/wm.c:KBD_KEY_SUPER_PULSE'
  'kernel/arch/x86_64/wm.c:KBD_KEY_SUPER_TAB'
  'kernel/arch/x86_64/wm.c:KBD_KEY_SUPER_BEACON'
  'Makefile.x86_64:kernel/arch/x86_64/beacon.c'
  'Makefile.x86_64:kernel/arch/x86_64/constellation.c'
)
for item in "${checks[@]}"; do file=${item%%:*}; needle=${item#*:}; grep -Fq "$needle" "$file"; pass=$((pass+1)); done
make -n ARCH=x86_64 all >/tmp/amplify-stage-c-make-plan.txt
pass=$((pass+1))
if grep -R -nE '^(<<<<<<<|=======|>>>>>>>)' kernel/arch/x86_64 REDESIGN_HANDOFF_RU.md STAGE_C_PROGRESS.md; then exit 1; fi
pass=$((pass+1))
echo "Stage C static checks: $pass/$pass PASS"

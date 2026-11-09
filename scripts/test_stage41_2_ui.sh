#!/bin/bash
# ============================================================================
# test_stage41_2_ui.sh — подэтап 41.2-B: плавность и корректность вывода.
# ----------------------------------------------------------------------------
# Что проверяется (QEMU + OVMF, virtio-vga 1920x1080, 4 ядра, dev-диск с
# /Sanctum/perf.conf fstat=1 — ядро раз в секунду пишет `[fstat] ...`):
#   1. простой на пустом рабочем столе: 0 кадров/с, загрузка BSP < 3 %;
#   2. движение мыши (QEMU monitor mouse_move): >= 60 кадров/с, сцена не
#      перерисовывается (scene=0), на экран уходит только курсор;
#   3. курсор не оставляет следов: скриншоты до/после движения отличаются
#      только самим курсором (tools/ppmdiff.c);
#   4. мышь при открытом окне терминала: >= 60 кадров/с;
#   5. частичный вывод не оставляет мусора: после скрытия окна и после
#      закрытия панели MIND экран равен исходному рабочему столу.
# Пороги по кадрам проверяются только под KVM: под TCG один кадр сцены —
# десятки мс, цифры там ничего не значат (проверки помечаются SKIP).
# Запуск: scripts/test_stage41_2_ui.sh   (на узле Proxmox, см. CLAUDE.md §2)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
SHOT_DIR=build/screens-41_2
WORK=/tmp/amplify-stage41_2
LOG="$WORK/serial.log"
MON_PORT=4584
SECS=6

PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
for f in "$ESP" "$DEVDISK"; do [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }; done

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    CPU="-enable-kvm -cpu host"; SLOW=1; KVM=1
else
    CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}; KVM=0
    echo "  (no KVM access — TCG: frame-rate thresholds are skipped)"
fi

mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$WORK"/*.log "$WORK"/*.img "$SHOT_DIR"/*.ppm
gcc -O2 -o build/ppmdiff tools/ppmdiff.c || { echo "cannot build tools/ppmdiff.c"; exit 1; }

QEMU_PID=""
cleanup() { [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null; exec 3>&- 2>/dev/null; rm -f "$WORK"/*.img; true; }
trap cleanup EXIT

mon_connect() {
    for _ in $(seq 1 100); do
        if exec 3<>/dev/tcp/127.0.0.1/$MON_PORT 2>/dev/null; then
            sleep 0.3; read -t 0.3 -r -u 3 _ || true; read -t 0.3 -r -u 3 _ || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}
drain() { while read -t 0.02 -r -u 3 _; do :; done; }
mon()   { echo "$1" >&3; sleep 0.05; drain; }
key()   { mon "sendkey $1"; sleep "${2:-0.25}"; }
wait_log() {
    local end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$1" "$LOG" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
shot() { local p="$PWD/$SHOT_DIR/$1.ppm"; rm -f "$p"; mon "screendump $p"; sleep 1; }
# Число отличающихся пикселей между двумя скриншотами (-1 — не сравнить).
ndiff() { build/ppmdiff "$SHOT_DIR/$1.ppm" "$SHOT_DIR/$2.ppm" 2>/dev/null | sed -n 's/^diff=\(-\?[0-9]*\).*/\1/p'; }
wiggle() {
    local end=$(( $(date +%s) + $1 )) i=0 dx=9 dy=4
    while [ "$(date +%s)" -lt "$end" ]; do
        echo "mouse_move $dx $dy" >&3
        i=$((i+1))
        if [ $((i % 40)) = 0 ]; then dx=$((-dx)); dy=$((-dy)); drain; fi
        sleep 0.01
    done
    drain
}
mark() { wc -l < "$LOG"; }
# Строки [fstat] режима: после отметки, без первой (окно, начавшееся до режима)
# и без последней (окно, в которое попал конец режима).
lines_of() { tail -n +"$(( $1 + 1 ))" "$LOG" | grep -a -F "[fstat] fps" | tr -d '\r' | sed '1d;$d'; }
# Поле строки [fstat]: field "<строка>" fps | busy | scene | px  (целая часть).
field() {
    case "$2" in
        fps)   echo "$1" | sed -n 's/.*fps=\([0-9]*\)\..*/\1/p';;
        busy)  echo "$1" | sed -n 's/.*busy=\([0-9]*\)\..*/\1/p';;
        scene) echo "$1" | sed -n 's/.* scene=\([0-9]*\)\/.*/\1/p';;
        px)    echo "$1" | sed -n 's/.* px=\([0-9]*\) .*/\1/p';;
        spins) echo "$1" | sed -n 's/.* spins=\([0-9]*\).*/\1/p';;
    esac
}
# Минимум/максимум поля по строкам режима.
fmin() { local m=""; while IFS= read -r l; do v=$(field "$l" "$2"); [ -n "$v" ] && { [ -z "$m" ] || [ "$v" -lt "$m" ]; } && m=$v; done <<< "$1"; echo "${m:--1}"; }
fmax() { local m=""; while IFS= read -r l; do v=$(field "$l" "$2"); [ -n "$v" ] && { [ -z "$m" ] || [ "$v" -gt "$m" ]; } && m=$v; done <<< "$1"; echo "${m:--1}"; }
# Проверка порога, осмысленная только под KVM.
kcheck() { if [ $KVM = 1 ]; then check "$1" "$2"; else echo "  [SKIP] $1 (TCG)"; fi; }

# dev-диск + perf.conf. mtools: -D s / -D o и </dev/null (CLAUDE.md §5.13).
cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
export MTOOLS_SKIP_CHECK=1
mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
printf 'fstat=1\n' > "$WORK/perf.conf"
mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null

echo "== stage 41.2-B: frame pacing and partial present =="
qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
    -smp cpus=4,cores=4,threads=1 \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -display none -serial "file:$LOG" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
    -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
mon_connect || { echo "monitor port never opened"; exit 1; }

wait_log "[smoke 37.6c-1] RTC" $((60*SLOW)) || true
sleep 2
key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
check "desktop reached"                       'wait_log "[wm] main loop start" $((60*SLOW))'
check "frame counters enabled by perf.conf"   'wait_log "[fstat] serial report enabled" 10'
check "virtio-gpu path active"                'grep -a -q "\[drm\] active" "$LOG"'
check "virtio-gpu commands complete by MSI-X" 'grep -a -q "\[vgpu\] controlq completion by MSI-X interrupt: OK" "$LOG"'
check "software cursor without a tablet"      '! grep -a -q "\[wm\] hardware cursor" "$LOG"'
sleep 3

# --- 1. простой -------------------------------------------------------------
M=$(mark); sleep $((SECS+2)); IDLE=$(lines_of "$M")
echo "$IDLE" | tail -1 | cut -c1-200 | sed 's/^/        /'
check  "idle: counters report"                '[ -n "$IDLE" ]'
# Не «ровно 0», а «не больше 1»: смена минуты на часах — один законный кадр.
check  "idle: no frames on a bare desktop"    '[ "$(fmax "$IDLE" fps)" -ge 0 ] && [ "$(fmax "$IDLE" fps)" -le 1 ]'
kcheck "idle: BSP busy < 3%"                  '[ "$(fmax "$IDLE" busy)" -ge 0 ] && [ "$(fmax "$IDLE" busy)" -lt 3 ]'
shot 01-desktop

# --- 2-3. мышь на пустом столе ----------------------------------------------
M=$(mark); wiggle $((SECS+2)); MOUSE=$(lines_of "$M")
echo "$MOUSE" | tail -1 | cut -c1-200 | sed 's/^/        /'
kcheck "mouse: >= 60 frames/s"                '[ "$(fmin "$MOUSE" fps)" -ge 60 ]'
# fmin, а не fmax: за время режима может смениться минута на часах — это один
# законный кадр сцены с полным выводом в одном из секундных окон.
check  "mouse: scene is not redrawn"          '[ "$(fmin "$MOUSE" scene)" = 0 ]'
check  "mouse: only the cursor goes to the host (< 2000 px/frame)" \
       '[ "$(fmin "$MOUSE" px)" -ge 1 ] && [ "$(fmin "$MOUSE" px)" -lt 2000 ]'
kcheck "mouse: BSP busy < 10%"                '[ "$(fmax "$MOUSE" busy)" -lt 10 ]'
check  "no busy-wait on virtio (spins=0)"     '[ "$(fmax "$MOUSE" spins)" = 0 ]'
sleep 1; shot 02-after-mouse
mon "mouse_move 37 23"; sleep 1; shot 03-moved
D=$(ndiff 02-after-mouse 03-moved)
check  "cursor move changes only the cursor (diff=$D px)"   '[ "${D:--1}" -gt 0 ] && [ "$D" -le 500 ]'
# До/после долгого движения: курсор + (возможно) сменившаяся минута на часах.
D=$(ndiff 01-desktop 02-after-mouse)
check  "no cursor trails after ${SECS}s of motion (diff=$D px)" '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'

# --- 4. мышь при открытом окне ----------------------------------------------
key meta_l-spc 2
shot 04-terminal
D=$(ndiff 03-moved 04-terminal)
check  "terminal window is on screen (diff=$D px)"          '[ "${D:--1}" -gt 100000 ]'
M=$(mark); wiggle $((SECS+2)); WIN=$(lines_of "$M")
echo "$WIN" | tail -1 | cut -c1-200 | sed 's/^/        /'
kcheck "window + mouse: >= 60 frames/s"       '[ "$(fmin "$WIN" fps)" -ge 60 ]'
kcheck "window + mouse: BSP busy < 35%"       '[ "$(fmax "$WIN" busy)" -lt 35 ]'

# --- 5. частичный вывод не оставляет мусора ---------------------------------
sleep 1; shot 05-terminal-after
key meta_l-h 1.5; shot 06-hidden
D=$(ndiff 03-moved 06-hidden)
check  "hidden window leaves a clean desktop (diff=$D px)"  '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'
key ctrl-spc 2; shot 07-mind
D=$(ndiff 06-hidden 07-mind)
check  "MIND panel is on screen (diff=$D px)"               '[ "${D:--1}" -gt 100000 ]'
key esc 1.5; shot 08-mind-closed
D=$(ndiff 06-hidden 08-mind-closed)
check  "closed MIND panel leaves a clean desktop (diff=$D px)" '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'

# --- 6. смена режима через virtio-gpu (без VBE) ------------------------------
# Терминал уже открыт (скрыт Super+H) — Super+Space возвращает его.
type_text() { local s="$1" c; for ((i=0; i<${#s}; i++)); do c="${s:i:1}"; [ "$c" = ' ' ] && c=spc; key "$c" 0.1; done; }
key meta_l-spc 2
type_text "gpustat mode 1280x720"; key ret 2
check  "mode switch command accepted"         'grep -a -q "\[drm\] mode set 1280x720" "$LOG"'
shot 09-mode-720p
SZ=$(build/ppmdiff "$SHOT_DIR/09-mode-720p.ppm" "$SHOT_DIR/09-mode-720p.ppm" 2>/dev/null | sed -n 's/.*size=//p')
check  "scanout really is 1280x720 (got ${SZ:-?})" '[ "${SZ:-}" = "1280x720" ]'
M=$(mark); wiggle 4; AFTER=$(lines_of "$M")
check  "frames keep flowing in the new mode"  '[ "$(fmax "$AFTER" fps)" -ge 1 ]'
type_text "gpustat mode 4096x2160"; key ret 1.5
check  "mode larger than the boot mode is refused" '! grep -a -q "\[drm\] mode set 4096x2160" "$LOG"'

check  "no kernel panic"                      '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial.log"
mon "quit"; sleep 1

echo "== stage 41.2-B: $PASS passed, $FAIL failed =="
[ $FAIL = 0 ]

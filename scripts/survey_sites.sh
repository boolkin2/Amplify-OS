#!/bin/bash
# ============================================================================
# survey_sites.sh — обход настоящих сайтов браузером AMPLIFY-OS (48.2).
# ----------------------------------------------------------------------------
# Не тест «прошёл/не прошёл» (интернет и сами сайты меняются), а обзор: QEMU
# (virtio-net, slirp — узлу нужен выход в интернет) -> вход -> браузер -> по
# очереди адреса из списка. Для каждого печатается итог (`[browser] page …`
# или `[browser] error …`) и строки TLS/HTTP, по которым видно причину отказа;
# снимок экрана — build/screens-sites/NN.ppm, serial — там же.
#   scripts/survey_sites.sh [файл_со_списком]      (по умолчанию scripts/sites.txt)
# Строки списка: адрес как его набрал бы человек; `#` — комментарий.
# ============================================================================
set -u
cd "$(dirname "$0")/.."

LIST="${1:-scripts/sites.txt}"
ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
SHOT_DIR=build/screens-sites
WORK=/tmp/amplify-sites
LOG="$WORK/serial.log"
MON_PORT=4595

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
[ -f "$LIST" ] || { echo "$LIST not found"; exit 1; }
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then CPU="-enable-kvm -cpu host"; SLOW=1
else CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}; fi

# AOS_PCAP=<файл> — записать весь обмен гостя с сетью (QEMU filter-dump):
# по нему видно, чей сегмент потерялся, когда страница «встала».
PCAP_ARG=""
[ -n "${AOS_PCAP:-}" ] && PCAP_ARG="-object filter-dump,id=fd0,netdev=net0,file=$AOS_PCAP"

mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$WORK"/*.log "$WORK"/*.img "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.log "$SHOT_DIR"/*.txt

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
mon()   { echo "$1" >&3; sleep 0.05; while read -t 0.02 -r -u 3 _; do :; done; }
key()   { mon "sendkey $1"; sleep "${2:-0.25}"; }
keyname() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '.') echo dot;; ':') echo shift-semicolon;;
        '-') echo minus;; '_') echo shift-minus;; '?') echo shift-slash;; '=') echo equal;;
        '&') echo shift-7;; '%') echo shift-5;; '+') echo shift-equal;; '~') echo shift-grave_accent;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.05; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.4
    done
    return 1
}
MARK=0
mark()  { MARK=$(wc -l < "$LOG"); }
since() { tail -n +"$((MARK+1))" "$LOG" | tr -d '\r'; }
shot()  { local p="$PWD/$SHOT_DIR/$1.ppm"; rm -f "$p"; mon "screendump $p"; sleep 0.8; }

cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
export MTOOLS_SKIP_CHECK=1
mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
printf 'fstat=0\n' > "$WORK/perf.conf"
mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
mdel -i "$WORK/dev.img" ::/Sanctum/network.conf </dev/null 2>/dev/null

qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
    -smp cpus=4,cores=4,threads=1 \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -display none -serial "file:$LOG" \
    -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
    -device virtio-keyboard-pci -device virtio-tablet-pci \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0,mac=52:54:00:12:34:56 $PCAP_ARG \
    -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
    -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
    -no-reboot -no-shutdown >/dev/null 2>&1 &
QEMU_PID=$!
mon_connect || { echo "monitor port never opened"; exit 1; }

wait_log "$LOG" "[smoke 37.6c-1] RTC" $((90*SLOW)) || { echo "boot failed"; exit 1; }
sleep 2
key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
wait_log "$LOG" "[wm] main loop start" $((60*SLOW)) || { echo "login failed"; exit 1; }
sleep 2
key meta_l-spc 1.5
type_text "summon Browser"; key ret 2

N=0; OKN=0
SUM="$SHOT_DIR/summary.txt"
: > "$SUM"
while IFS= read -r url; do
    url="${url%%#*}"; url="$(echo "$url" | tr -d '\r' | sed 's/[[:space:]]*$//')"
    [ -n "$url" ] || continue
    N=$((N+1)); ID=$(printf '%02d' "$N")
    mark
    key ctrl-l 0.3
    type_text "$url"; key ret 0.3
    # Итог: строка page или error (страница «тип не поддерживается» — тоже итог).
    end=$(( $(date +%s) + 70*SLOW )); RES=""
    while [ "$(date +%s)" -lt "$end" ]; do
        RES=$(since | grep -a -E '\[browser\] (page http=|error |unsupported content)' | tail -1)
        [ -n "$RES" ] && break
        sleep 0.5
    done
    sleep 1.2; shot "$ID"
    DETAIL=$(since | grep -a -E '\[tls\]|\[net\] (redirect|https failed|fetch failed)|\[http\]|\[https\]|\[dns\] (name|servers)|\[tcp\] (connect|conn|RST from|no route)|level=|\[browser\] (links|search|meta refresh|start at|form)|\[net\] gzip' \
             | sed 's/^ *//' | tr '\n' ';' | cut -c1-900)
    case "$RES" in *"page http=200"*) OKN=$((OKN+1)); TAG="OK  ";; *"page http="*) TAG="HTTP";; "") TAG="HANG"; RES="(no result in 70 s)";; *) TAG="FAIL";; esac
    echo "$TAG $ID $url" | tee -a "$SUM"
    echo "       ${RES#*\] }" | tee -a "$SUM"
    echo "       $DETAIL" >> "$SUM"
    # Зависший запрос отменить, чтобы следующий адрес набрался.
    [ "$TAG" = HANG ] && { key esc 1.5; }
done < "$LIST"

cp "$LOG" "$SHOT_DIR/serial.log"
mon "quit"; sleep 1
echo "== sites: $OKN of $N opened with HTTP 200 =="

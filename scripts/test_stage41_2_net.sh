#!/bin/bash
# ============================================================================
# test_stage41_2_net.sh — подэтап 41.2: virtio-net вместо e1000.
# ----------------------------------------------------------------------------
# Фаза 1: QEMU с `-device virtio-net-pci` (slirp NAT). Проверяется, что карта
#   найдена, приём идёт по прерыванию MSI-X, DHCP выдал адрес, а поверх
#   virtio-net работают DNS, TCP, TLS и HTTPS (команда `https` в терминале идёт
#   на настоящий сайт — узлу нужен выход в интернет; без него проверки — SKIP).
# Фаза 2: скорость. На хосте поднимается http-сервер с файлом 100 КБ (из гостя
#   это 10.0.2.2), ядро качает его много раз подряд (perf.conf: netbench=<url>)
#   сначала через virtio-net, потом через e1000 — обе цифры печатаются. Порогов нет: скорость
#   упирается и в наш TCP-стек, сравнение — для отчёта.
# Запуск: scripts/test_stage41_2_net.sh   (узел Proxmox)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
OUT_DIR=build/screens-41_2-net
WORK=/tmp/amplify-stage41_2-net
MON_PORT=4587
HTTP_PORT=4588
BLOB_BYTES=100000          # меньше буфера HTTP-клиента ядра (128 КБ)

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
    CPU="-enable-kvm -cpu host"; SLOW=1
else
    CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}
    echo "  (no KVM access — TCG, timeouts x$SLOW)"
fi

mkdir -p "$WORK/www" "$OUT_DIR"
rm -f "$WORK"/*.log "$WORK"/*.img

QEMU_PID=""; HTTP_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null
    [ -n "$HTTP_PID" ] && kill -9 "$HTTP_PID" 2>/dev/null
    exec 3>&- 2>/dev/null; rm -f "$WORK"/*.img; rm -rf "$WORK/www"; true
}
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
        '-') echo minus;; *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.1; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
# start_qemu <log> <сетевая карта>
start_qemu() {
    local log="$1" nic="$2"
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
        -smp cpus=4,cores=4,threads=1 \
        -vga none -device virtio-vga,xres=1280,yres=720 \
        -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -netdev user,id=net0 -device "$nic",netdev=net0,mac=52:54:00:12:34:56 \
        -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
        -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
        -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
stop_qemu() {
    mon "quit"; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null
}
login() {
    wait_log "$1" "[smoke 37.6c-1] RTC" $((120*SLOW)) || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
}
# dev-диск с perf.conf; $1 — содержимое perf.conf (может быть пустым).
make_disk() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
    printf '%s\n' "$1" > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
}

# http-сервер для замера: только на loopback узла, файл из /dev/urandom.
head -c "$BLOB_BYTES" /dev/urandom > "$WORK/www/blob.bin"
( cd "$WORK/www" && exec python3 -m http.server "$HTTP_PORT" --bind 127.0.0.1 >/dev/null 2>&1 ) &
HTTP_PID=$!
URL="http://10.0.2.2:$HTTP_PORT/blob.bin"

# ===========================================================================
echo "== Phase 1: virtio-net (slirp) — DHCP, DNS, TCP, TLS, HTTPS, download =="
LOG="$WORK/serial-virtio.log"
make_disk "netbench=$URL"
start_qemu "$LOG" virtio-net-pci
check "virtio-net found"                      'wait_log "$LOG" "[vnet] virtio-net up" $((60*SLOW))'
check "receive by MSI-X interrupt"            'grep -a "\[vnet\] virtio-net up" "$LOG" | grep -q "rx irq=MSI-X"'
check "stack uses virtio-net, not e1000"      'grep -a -q "\[net\] virtio-net up" "$LOG" && ! grep -a -q "\[net\] e1000 up" "$LOG"'
check "DHCP bound over virtio-net"            'wait_log "$LOG" "[dhcp] ACK" $((30*SLOW))'
check "address from the DHCP server"          'wait_log "$LOG" "[dhcp] OFFER 10.0.2.15" 5'
login "$LOG"
check "desktop reached"                       'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
check "download finished"                     'wait_log "$LOG" "[netbench] " $((180*SLOW))'
# DNS + TCP + TLS + HTTPS к настоящему сайту: команда `https` в терминале.
key meta_l-spc 2
type_text "https https://example.com/"; key ret 0.5
if wait_log "$LOG" "[dns] resolved" $((15*SLOW)); then
    ok "DNS resolved over virtio-net"
    check "TLS handshake over virtio-net"     'wait_log "$LOG" "[tls] handshake complete" $((60*SLOW))'
else
    echo "  [SKIP] DNS/TLS/HTTPS: no internet from this host (DNS did not resolve)"
fi
V=$(grep -a -m1 "\[netbench\]" "$LOG" | tr -d '\r')
echo "        $V"
check "every download is complete ($BLOB_BYTES bytes)" 'echo "$V" | grep -q " x $BLOB_BYTES bytes in "'
check "no kernel panic"                       '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$OUT_DIR/serial-virtio.log"
stop_qemu

# ===========================================================================
echo "== Phase 2: the same download through e1000 (for comparison) =="
LOG2="$WORK/serial-e1000.log"
make_disk "netbench=$URL"
start_qemu "$LOG2" e1000
check "e1000 fallback still works"            'wait_log "$LOG2" "[net] e1000 up" $((60*SLOW))'
login "$LOG2"
wait_log "$LOG2" "[wm] main loop start" $((60*SLOW)) || true
check "DHCP bound over e1000"                 'grep -a -q "\[dhcp\] ACK" "$LOG2"'
check "download through e1000 finished"       'wait_log "$LOG2" "[netbench] " $((180*SLOW))'
E=$(grep -a -m1 "\[netbench\]" "$LOG2" | tr -d '\r')
echo "        $E"
cp "$LOG2" "$OUT_DIR/serial-e1000.log"
stop_qemu

VK=$(echo "$V" | sed -n 's/.* = \([0-9]*\) KiB\/s.*/\1/p'); EK=$(echo "$E" | sed -n 's/.* = \([0-9]*\) KiB\/s.*/\1/p')
echo "  speed: virtio-net ${VK:-?} KiB/s, e1000 ${EK:-?} KiB/s"

echo "== stage 41.2 net: $PASS passed, $FAIL failed =="
[ $FAIL = 0 ]

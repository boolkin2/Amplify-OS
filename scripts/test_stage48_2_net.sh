#!/bin/bash
# ============================================================================
# test_stage48_2_net.sh — подэтап 48.2: сеть, Mesh, браузер, анимации.
# ----------------------------------------------------------------------------
# QEMU: virtio-vga 1920x1080, virtio-net (slirp), virtio-клавиатура и планшет
# (щелчки — через QMP input-send-event, как в test_stage41_2_input.sh).
# На узле поднимается tools/test_http_server.py — из гостя это 10.0.2.2.
#
# Фаза 1 (одна загрузка, network.conf нет -> режим auto, адрес по DHCP):
#   * DHCP, продление аренды (perf.conf dhcplease=40 -> T1 через 20 с);
#   * анимации: вход на стол, Beacon, MIND, Constellation, окна — число кадров
#     из строк `[anim]` и чистый экран после закрытия (сравнение скриншотов);
#   * браузер: страница, переход по ссылке щелчком, назад/вперёд кнопками,
#     страница 600 КБ (> прежних 128 КБ) с побайтной сверкой FNV, chunked,
#     ответ без Content-Length, редирект, 404, обрезка 4.8 МБ на 4 МБ, отказ
#     соединения, DNS, отмена загрузки по Esc (и кадры во время загрузки),
#     прокрутка;
#   * Mesh: переключатель «Статика», ввод адреса в поле, отказ на неверный
#     адрес, «Применить», «Сбросить кэш», «Обновить DHCP», «Проверить DNS»;
#     после перехода на статику браузер снова открывает страницу;
#   * network.conf на диске (читается mtools после остановки QEMU).
# Фаза 2: режим static из network.conf — DHCP не запускается, сеть работает.
# Фаза 3: DHCP недоступен (линк выключен командой монитора set_link) -> через
#   3 с применяется запасной статический адрес; линк включается -> DHCP,
#   продолжавший спрашивать, получает адрес уже после загрузки.
# Запуск: scripts/test_stage48_2_net.sh   (узел Proxmox, KVM)
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
SHOT_DIR=build/screens-48_2
WORK=/tmp/amplify-stage48_2
LOG="$WORK/serial.log"
MON_PORT=4591
QMP_PORT=4592
HTTP_PORT=4593
DEAD_PORT=4594                 # здесь никто не слушает: отказ соединения
BASE="http://10.0.2.2:$HTTP_PORT"

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
command -v python3 >/dev/null || { echo "python3 not found (needed for the test http server)"; exit 1; }

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    CPU="-enable-kvm -cpu host"; SLOW=1
else
    CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}
    echo "  (no KVM access — TCG, timeouts x$SLOW, frame-rate checks skipped)"
fi

mkdir -p "$WORK/www" "$SHOT_DIR"
rm -f "$WORK"/*.log "$WORK"/*.img "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.log
gcc -O2 -o build/ppmdiff tools/ppmdiff.c || { echo "cannot build tools/ppmdiff.c"; exit 1; }

QEMU_PID=""; HTTP_PID=""
cleanup() {
    [ -n "$QEMU_PID" ] && kill -9 "$QEMU_PID" 2>/dev/null
    [ -n "$HTTP_PID" ] && kill -9 "$HTTP_PID" 2>/dev/null
    exec 3>&- 2>/dev/null; exec 4>&- 2>/dev/null
    rm -f "$WORK"/*.img; rm -rf "$WORK/www"; true
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
drain() { while read -t 0.02 -r -u 3 _; do :; done; }
mon()   { echo "$1" >&3; sleep 0.05; drain; }
key()   { mon "sendkey $1"; sleep "${2:-0.25}"; }
keyname() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '.') echo dot;; ':') echo shift-semicolon;;
        '-') echo minus;; '_') echo shift-minus;; '?') echo shift-slash;; '=') echo equal;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.07; done; }
qmp()   { echo "$1" >&4; sleep 0.1; while read -t 0.05 -r -u 4 _; do :; done; }
qmp_connect() {
    for _ in $(seq 1 50); do
        if exec 4<>/dev/tcp/127.0.0.1/$QMP_PORT 2>/dev/null; then
            sleep 0.3; while read -t 0.2 -r -u 4 _; do :; done
            qmp '{"execute":"qmp_capabilities"}'
            return 0
        fi
        sleep 0.1
    done
    return 1
}
# Планшет: координаты 0..32767 по каждой оси; у нас — пиксели экрана 1920x1080.
abs_px() {
    local ax=$(( $1 * 32767 / 1919 )) ay=$(( $2 * 32767 / 1079 ))
    qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"abs\",\"data\":{\"axis\":\"x\",\"value\":$ax}},{\"type\":\"abs\",\"data\":{\"axis\":\"y\",\"value\":$ay}}]}}"
}
btn() { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":$1,\"button\":\"left\"}}]}}"; }
click_px() { abs_px "$1" "$2"; sleep 0.3; btn true; sleep 0.15; btn false; sleep 0.2; }
wheel()    { qmp "{\"execute\":\"input-send-event\",\"arguments\":{\"events\":[{\"type\":\"btn\",\"data\":{\"down\":true,\"button\":\"$1\"}}]}}"; }
# Координаты элемента из строки раскладки ядра: `... name=X,Y ...`.
coord() { echo "$1" | tr ' ' '\n' | tr -d '\r' | sed -n "s/^$2=\([0-9]*\),\([0-9]*\)$/\1 \2/p" | head -1; }
click_at() {
    local c; c=$(coord "$1" "$2")
    [ -n "$c" ] || { echo "        (no coordinates for '$2')"; return 1; }
    click_px $c
}
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.4
    done
    return 1
}
# Отметка в журнале и ожидание строки, появившейся ПОСЛЕ отметки.
MARK=0
mark()     { MARK=$(wc -l < "$LOG"); }
since()    { tail -n +"$((MARK+1))" "$LOG" | tr -d '\r'; }
wait_new() {
    local end=$(( $(date +%s) + $2 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        since | grep -a -q -F "$1" && return 0
        sleep 0.3
    done
    return 1
}
last_new() { since | grep -a -F "$1" | tail -1; }
field()    { echo "$1" | sed -n "s/.* $2=\([^ ]*\).*/\1/p"; }
shot()  { local p="$PWD/$SHOT_DIR/$1.ppm"; rm -f "$p"; mon "screendump $p"; sleep 1; }
ndiff() { build/ppmdiff "$SHOT_DIR/$1.ppm" "$SHOT_DIR/$2.ppm" 2>/dev/null | sed -n 's/^diff=\(-\?[0-9]*\).*/\1/p'; }

# start_qemu <log> [доп. аргументы QEMU]
start_qemu() {
    local log="$1"; shift
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
        -smp cpus=4,cores=4,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        -qmp "tcp:127.0.0.1:$QMP_PORT,server,nowait" \
        -device virtio-keyboard-pci -device virtio-tablet-pci \
        -netdev user,id=net0 -device virtio-net-pci,netdev=net0,id=nic0,mac=52:54:00:12:34:56 \
        -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
        -drive file="$WORK/esp.img",format=raw,if=ide,index=1 \
        "$@" -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
stop_qemu() {
    mon "quit"; sleep 1.5
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null; exec 4>&- 2>/dev/null
}
login() {
    wait_log "$1" "[smoke 37.6c-1] RTC" $((90*SLOW)) || true
    sleep 2
    key ret 0.4; for c in a d m i n; do key $c 0.12; done; key ret 1
}
# make_disk <perf.conf> [network.conf]
make_disk() {
    cp --sparse=always "$DEVDISK" "$WORK/dev.img"
    cp "$ESP" "$WORK/esp.img"
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$WORK/dev.img" ::/Sanctum </dev/null 2>/dev/null
    printf '%b' "$1" > "$WORK/perf.conf"
    mcopy -D o -i "$WORK/dev.img" "$WORK/perf.conf" ::/Sanctum/perf.conf </dev/null
    mdel -i "$WORK/dev.img" ::/Sanctum/network.conf </dev/null 2>/dev/null
    if [ -n "${2:-}" ]; then
        printf '%b' "$2" > "$WORK/network.conf"
        mcopy -D o -i "$WORK/dev.img" "$WORK/network.conf" ::/Sanctum/network.conf </dev/null
    fi
}
fnv32() { python3 -c '
import sys
h = 2166136261
for b in open(sys.argv[1], "rb").read()[:int(sys.argv[2])]:
    h = ((h ^ b) * 16777619) & 0xFFFFFFFF
print("%08x" % h)' "$1" "$2"; }
# Частота кадров анимации из строки `[anim] <что> ... fps=N`.
anim_fps() { since | grep -a -F "[anim] $1" | tail -1 | sed -n 's/.* fps=\([0-9]*\).*/\1/p'; }
check_fps() {   # <название> <шаблон>
    local f; f=$(anim_fps "$2")
    local line; line=$(since | grep -a -F "[anim] $2" | tail -1)
    if [ -z "$line" ]; then bad "$1: animation ran (no [anim] line)"; return; fi
    ok "$1: animation ran (${line#*\] })"
    if [ "$SLOW" = 1 ]; then check "$1: >= 55 frames/s (got ${f:-?})" '[ "${f:-0}" -ge 55 ]'; fi
}

# --- тестовый http-сервер ------------------------------------------------------
python3 tools/test_http_server.py "$HTTP_PORT" "$WORK/www" >/dev/null 2>&1 &
HTTP_PID=$!
for _ in $(seq 1 50); do [ -s "$WORK/www/huge.html" ] && break; sleep 0.1; done
sleep 0.5
BIG_BYTES=$(stat -c %s "$WORK/www/big.html")
BIG_FNV=$(fnv32 "$WORK/www/big.html" "$BIG_BYTES")
HUGE_BYTES=$(stat -c %s "$WORK/www/huge.html")
HUGE_FNV=$(fnv32 "$WORK/www/huge.html" 4194304)
echo "  http server: big.html $BIG_BYTES bytes fnv=$BIG_FNV, huge.html $HUGE_BYTES bytes"

# ===========================================================================
echo "== Phase 1: DHCP, animations, browser, Mesh =="
make_disk 'fstat=1\ndhcplease=40\n'
start_qemu "$LOG"
qmp_connect || echo "  (QMP port never opened)"

check "config: no network.conf -> mode auto"   'wait_log "$LOG" "[netcfg] mode=auto (default" $((60*SLOW))'
check "DHCP bound"                             'wait_log "$LOG" "[dhcp] ACK" $((30*SLOW))'
check "netcfg applied the lease (10.0.2.15)"   'wait_log "$LOG" "[netcfg] DHCP lease applied: ip=10.0.2.15" 5'
login "$LOG"
check "desktop reached"                        'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
T_WM=$(date +%s)
mark; MARK=0
sleep 2
check_fps "desktop entry"      "desktop enter open"
sleep 1; shot 01-desktop

# --- оверлеи ----------------------------------------------------------------
mark; key meta_l-n 1.2;  check_fps "Beacon open"  "panel beacon open";  shot 02-beacon
mark; key esc 1.2;       check_fps "Beacon close" "panel beacon close"; shot 03-after-beacon
D=$(ndiff 01-desktop 03-after-beacon)
check "screen is clean after Beacon closes (diff=$D px)"   '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'
mark; key ctrl-spc 1.5;  check_fps "MIND open"    "panel mind open";    shot 04-mind
mark; key esc 1.2;       check_fps "MIND close"   "panel mind close";   shot 05-after-mind
D=$(ndiff 01-desktop 05-after-mind)
check "screen is clean after MIND closes (diff=$D px)"     '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'

# --- терминал и браузер ---------------------------------------------------------
mark; key meta_l-spc 1.5
check_fps "terminal window open" "window"
type_text "summon Browser"; mark; key ret 1.5
check "browser opened"                         'wait_new "[browser] open" 10'
check_fps "browser window open" "window Browser open"
BL=$(since | grep -a -F "[browser] layout" | tail -1)
check "browser reported its layout"            '[ -n "$BL" ]'
shot 06-browser-start

browse() { mark; click_at "$BL" addr; sleep 0.2; type_text "$1"; key ret 0.3; }
loaded() { wait_new "[browser] page http=" $((${1:-20}*SLOW)) || wait_new "[browser] error" 1; }

# Стартовая страница: адресная строка уже в фокусе.
mark; type_text "$BASE/index.html"; key ret 0.3
check "page loaded over http"                  'loaded && since | grep -a -q "\[browser\] page http=200"'
sleep 1; shot 07-browser-index
LK=$(last_new "[browser] links:")
L1=$(echo "$LK" | sed -n 's/.*links: \([0-9]*\),\([0-9]*\).*/\1 \2/p')
check "first link position reported ($L1)"     '[ -n "$L1" ]'
mark; click_px $L1
check "click on a link navigates"              'wait_new "[browser] navigate $BASE/page2.html" 10 && loaded'
sleep 0.8; shot 08-browser-page2
mark; click_at "$BL" back
check "Back button returns to the index"       'wait_new "[browser] back -> $BASE/index.html" 10 && loaded'
mark; click_at "$BL" forward
check "Forward button goes to page 2 again"    'wait_new "[browser] forward -> $BASE/page2.html" 10 && loaded'

browse "$BASE/big.html"
check "page of $((BIG_BYTES/1024)) KB loads"                      'loaded 30'
P=$(last_new "[browser] page http=")
echo "        $P"
check "body is complete: $BIG_BYTES bytes (old limit was 131072)"  '[ "$(field "$P" bytes)" = "$BIG_BYTES" ] && [ "$(field "$P" complete)" = 1 ]'
check "body is byte-exact (fnv $(field "$P" fnv) = $BIG_FNV)"      '[ "$(field "$P" fnv)" = "$BIG_FNV" ]'
sleep 1; shot 09-big-top
LB=$(since | grep -a -F "[browser] links:" | tail -1 | sed -n 's/.*links: \([0-9]*\),\([0-9]*\).*/\1 \2/p')
mark; key pgdn 0.4; key pgdn 0.4; key pgdn 0.8; shot 10-big-scrolled
D=$(ndiff 09-big-top 10-big-scrolled)
check "PgDn scrolls the page (diff=$D px)"     '[ "${D:--1}" -gt 20000 ]'
wheel wheel-down; wheel wheel-down; sleep 0.8; shot 10b-big-wheel
D=$(ndiff 10-big-scrolled 10b-big-wheel)
check "mouse wheel scrolls the page (diff=$D px)" '[ "${D:--1}" -gt 5000 ]'
# Ссылка в ПЕРВОМ абзаце страницы (до неё на странице нет ни одной стилевой
# точки — отдельный случай в раскладке строки): Home и щелчок по ней.
key home 0.6; shot 10c-big-home
D=$(ndiff 09-big-top 10c-big-home)
check "Home returns to the top (diff=$D px)"   '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'
mark; click_px $LB
check "link inside the first paragraph works"  'wait_new "[browser] navigate $BASE/page2.html" 10 && loaded'

browse "$BASE/chunked"
check "chunked response loads"                 'loaded 30'
P=$(last_new "[browser] page http=")
check "chunked body decoded byte-exact"        '[ "$(field "$P" bytes)" = "$BIG_BYTES" ] && [ "$(field "$P" fnv)" = "$BIG_FNV" ]'

browse "$BASE/noclen"
check "response without Content-Length loads"  'loaded 30'
P=$(last_new "[browser] page http=")
check "body read until the server closed (byte-exact)" '[ "$(field "$P" fnv)" = "$BIG_FNV" ] && [ "$(field "$P" complete)" = 1 ]'

browse "$BASE/redirect"
check "redirect is followed"                   'loaded && since | grep -a -q "\[net\] redirect 302 -> $BASE/page2.html"'

# Сжатые ответы: gzip, zlib и «сырой» deflate — те же байты, что big.html.
for enc in gzip zlib deflate; do
    browse "$BASE/$enc"
    check "compressed response ($enc) loads"   'loaded 30'
    P=$(last_new "[browser] page http=")
    check "$enc body unpacked byte-exact"      '[ "$(field "$P" bytes)" = "$BIG_BYTES" ] && [ "$(field "$P" fnv)" = "$BIG_FNV" ]'
done

# Cookie, поставленная ответом-перенаправлением, возвращается следующим
# запросом (без хранилища cookies здесь бесконечная петля редиректов).
browse "$BASE/cookie"
check "cookie from a redirect is stored and sent back" \
      'loaded && since | grep -a -q "\[browser\] page http=200" && since | grep -a -q "\[cookies\] 2 set by 10.0.2.2"'

browse "$BASE/hints"
check "103 Early Hints before the real response" 'loaded && since | grep -a -q "\[browser\] page http=200"'

browse "$BASE/missing"
check "HTTP 404 page is shown with its code"   'loaded && since | grep -a -q "\[browser\] page http=404"'

browse "$BASE/huge.html"
check "page of $((HUGE_BYTES/1024)) KB loads up to the limit"        'loaded 40'
P=$(last_new "[browser] page http=")
echo "        $P"
check "page truncated at 4 MB, flagged"        '[ "$(field "$P" bytes)" = 4194304 ] && [ "$(field "$P" truncated)" = 1 ]'
check "the first 4 MB are byte-exact"          '[ "$(field "$P" fnv)" = "$HUGE_FNV" ]'

browse "$BASE/image.png"
check "non-text content is refused politely"   'wait_new "[browser] unsupported content type image/png" 15'

browse "http://10.0.2.2:$DEAD_PORT/"
check "connection refused -> error page"       'wait_new "[browser] error refused" 15'
sleep 0.8; shot 11-error-refused

browse "$BASE/loop"
check "redirect loop -> error page"            'wait_new "[browser] error redirects" 20'

browse "http://no-such-host-amplify.invalid/"
check "unknown host -> DNS error page"         'wait_new "[browser] error dns" $((20*SLOW))'
sleep 0.8; shot 12-error-dns

# Отмена: сервер прислал начало и замолчал; через 3 с — Esc.
mark; click_at "$BL" addr; sleep 0.2; type_text "$BASE/stall"; key ret 0.3
sleep 3; shot 13-loading
key esc 0.5
check "Esc cancels a stalled load"             'wait_new "[browser] error cancelled" 10'
E=$(last_new "[browser] error cancelled")
FR=$(field "$E" frames)
check "interface kept drawing while loading ($FR frames in $(field "$E" ms) ms)" '[ "${FR:-0}" -ge 40 ]'

# После ошибок сеть цела: следующая страница грузится.
browse "$BASE/page2.html"
check "next page loads after the errors"       'loaded && since | grep -a -q "\[browser\] page http=200"'

# --- «настоящие сайты»: то, на чём браузер спотыкался в интернете --------------
browse "$BASE/refresh"
check "meta refresh is followed"               'wait_new "[browser] meta refresh -> $BASE/page2.html" 15 && sleep 1 && [ "$(since | grep -a -c "\[browser\] page http=200")" -ge 2 ]'

browse "$BASE/hidden"
check "page with hidden elements loads"        'loaded'
P=$(last_new "[browser] page http=")
check "hidden/aria-hidden/display:none/button/dialog are skipped, noscript and React slot are shown (links=$(field "$P" links))" \
      '[ "$(field "$P" links)" = 3 ]'

browse "$BASE/octet"
check "HTML served as application/octet-stream is shown" 'loaded && since | grep -a -q "\[browser\] page http=200 .* links=1 "'

browse "$BASE/long"
check "redirect to a 1.5 KB address is not cut" 'loaded && since | grep -a -q "\[browser\] page http=200"'

browse "$BASE/utf"
check "page with a Cyrillic link loads"        'loaded'
sleep 1
L1=$(last_new "[browser] links:" | sed -n 's/.*links: \([0-9]*\),\([0-9]*\).*/\1 \2/p')
mark; click_px $L1
check "Cyrillic path and query go out as %XX"  'loaded && since | grep -a -q "\[browser\] page http=200"'

browse "$BASE/mainjump"
check "long menu is skipped: page opens at <main>" 'loaded && wait_new "[browser] start at main content" 5'
sleep 0.8; shot 14-main-jump

browse "$BASE/form"
check "page with a search form loads"          'loaded'
sleep 1
L1=$(last_new "[browser] links:" | sed -n 's/.*links: \([0-9]*\),\([0-9]*\).*/\1 \2/p')
check "search field is shown as a link ($L1)"  '[ -n "$L1" ]'
mark; click_px $L1
check "click on the field asks for a query"    'wait_new "[browser] form field -> $BASE/find?q=" 5'
sleep 0.5; shot 15-form-query
type_text "two words"; key ret 0.3
check "typed query is sent to the form address" 'wait_new "[browser] form query -> $BASE/find?q=two+words" 5 && loaded && since | grep -a -q "\[browser\] page http=200"'

# Не адрес (есть пробел) — поисковый запрос. Сам поиск требует интернета:
# проверяем, куда браузер пошёл, и что после этого он жив.
mark; click_at "$BL" addr; sleep 0.2; type_text "amplify os"; key ret 0.3
check "text with a space becomes a search query" 'wait_new "[browser] search -> https://html.duckduckgo.com/html/?q=amplify+os" 5'
loaded 40 || true
browse "$BASE/page2.html"
check "browser is alive after the search"      'loaded && since | grep -a -q "\[browser\] page http=200"'

# HTTPS к настоящему сайту: TLS 1.2 + тот же потоковый разбор ответа, что и
# для http (узлу нужен выход в интернет; без него — SKIP).
browse "https://example.com/"
if wait_new "[dns] resolved" $((12*SLOW)); then
    check "HTTPS page loads in the browser"    'loaded 40 && since | grep -a -q "\[browser\] page http=200"'
    P=$(last_new "[browser] page http=")
    echo "        $P"
    check "HTTPS body is complete and not empty" '[ "$(field "$P" complete)" = 1 ] && [ "$(field "$P" bytes)" -gt 200 ]'
    sleep 1; shot 13b-https
else
    echo "  [SKIP] HTTPS page: no internet from this host (DNS did not resolve)"
    wait_new "[browser] error" 20 || true
fi
browse "$BASE/page2.html"
loaded || true

# --- Constellation (открыты терминал и браузер) --------------------------------
shot 14-before-const
mark; key meta_l-tab 1.2; check_fps "Constellation open"  "panel constellation open";  shot 15-constellation
mark; key esc 1.2;        check_fps "Constellation close" "panel constellation close"; shot 16-after-const
D=$(ndiff 14-before-const 16-after-const)
check "screen is clean after Constellation closes (diff=$D px)" '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'

# Аренда DHCP: потолок 40 с -> продление через 20 с после входа на стол.
while [ $(( $(date +%s) - T_WM )) -lt 30 ]; do sleep 1; done
check "DHCP lease renewed at T1"               'grep -a -q "\[dhcp\] renew: lease extended" "$LOG"'

# --- закрыть браузер, открыть Mesh ----------------------------------------------
mark; key esc 1.2
check_fps "browser window close" "window Browser close"
type_text "summon Mesh"; mark; key ret 1.5
check "Mesh opened"                            'wait_new "[mesh] open" 10'
check_fps "Mesh window open" "window Mesh open"
ML=$(since | grep -a -F "[mesh] layout" | tail -1)
check "Mesh reported its layout"               '[ -n "$ML" ]'
sleep 0.5; shot 17-mesh-dhcp

mark; click_at "$ML" static
check "click selects the Static mode"          'wait_new "[mesh] mode selected: static" 5'
mark; click_at "$ML" ip
check "click focuses the IP field"             'wait_new "[mesh] focus ip" 5'
# Поле заполнено текущим адресом (10.0.2.15): стереть и ввести неверный.
for _ in $(seq 1 16); do key backspace 0.04; done
type_text "10.0.2.999"
mark; click_at "$ML" apply
check "invalid address is rejected"            'wait_new "[mesh] apply rejected" 5'
sleep 0.5; shot 18-mesh-invalid
for _ in $(seq 1 16); do key backspace 0.04; done
type_text "10.0.2.77"
mark; click_at "$ML" apply
check "Apply accepts the static address"       'wait_new "[mesh] apply ok mode=static saved" 5'
check "static address is on the interface"     'since | grep -a -q "\[netcfg\] static applied: ip=10.0.2.77 mask=255.255.255.0 gw=10.0.2.2"'
sleep 0.5; shot 19-mesh-static
mark; click_at "$ML" flush
check "Flush cache button"                     'wait_new "[mesh] caches flushed" 5'
mark; click_at "$ML" renew
check "Renew DHCP explains itself in static mode" 'wait_new "[mesh] renew ignored: static mode" 5'
mark; click_at "$ML" testdns
if wait_new "[mesh] dns test ok" $((12*SLOW)); then ok "Test DNS button: resolver answers"
elif since | grep -a -q "\[mesh\] dns test failed"; then echo "  [SKIP] Test DNS: no DNS from this host (button works, resolver silent)"
else bad "Test DNS button did nothing"; fi
sleep 0.5; shot 20-mesh-dns

# Со статическим адресом сеть работает: браузер открывает страницу заново.
mark; key esc 1.2
check_fps "Mesh window close" "window Mesh close"
type_text "summon Browser"; mark; key ret 2
BL=$(since | grep -a -F "[browser] layout" | tail -1)
mark; key r 0.3
check "page loads from the static address"     'loaded && since | grep -a -q "\[browser\] page http=200"'

# --- анимация действительно движется ---------------------------------------
# Счётчик кадров не видит картинку. Замедляем анимации в 8 раз (`compstat
# slow 8`: 300 мс -> 2.4 с) и снимаем середину появления MIND: кадр обязан
# отличаться и от экрана до, и от готовой панели.
mark; key esc 1.5                                   # закрыть браузер -> терминал
type_text "compstat slow 8"; key ret 0.8
shot 21-slow-before
key ctrl-spc 0; sleep 0.8; shot 22-slow-mid
sleep 3; shot 23-slow-full
A=$(ndiff 21-slow-before 22-slow-mid); B=$(ndiff 22-slow-mid 23-slow-full)
check "mid-animation frame differs from the start ($A px) and from the end ($B px)" \
      '[ "${A:-0}" -gt 100000 ] && [ "${B:-0}" -gt 100000 ]'
key esc 0; sleep 0.7; shot 24-slow-closing; sleep 3
D=$(ndiff 21-slow-before 24-slow-closing)
check "closing panel is caught half-way too (diff=$D px)" '[ "${D:-0}" -gt 100000 ]'
shot 25-slow-after
D=$(ndiff 21-slow-before 25-slow-after)
check "screen is clean after the slow close (diff=$D px)" '[ "${D:--1}" -ge 0 ] && [ "$D" -le 3000 ]'
type_text "compstat slow 1"; key ret 0.5

check "no kernel panic"                        '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial-phase1.log"
stop_qemu

CONF=$(mtype -i "$WORK/dev.img" ::/Sanctum/network.conf 2>/dev/null | tr -d '\r')
check "network.conf saved: mode=static"        'echo "$CONF" | grep -q "^mode=static$"'
check "network.conf saved: ip=10.0.2.77"       'echo "$CONF" | grep -q "^ip=10.0.2.77$"'
check "network.conf saved: gateway and dns"    'echo "$CONF" | grep -q "^gateway=10.0.2.2$" && echo "$CONF" | grep -q "^dns=10.0.2.3$"'

# ===========================================================================
echo "== Phase 2: static address from network.conf =="
LOG="$WORK/serial-static.log"
make_disk "netbench=$BASE/blob.bin\n" 'mode=static\nip=10.0.2.60\nmask=255.255.255.0\ngateway=10.0.2.2\ndns=10.0.2.3\n'
start_qemu "$LOG"
check "config read: mode static"               'wait_log "$LOG" "[netcfg] mode=static (network.conf)" $((60*SLOW))'
check "static address applied at boot"         'wait_log "$LOG" "[netcfg] static applied: ip=10.0.2.60" 5'
login "$LOG"
check "desktop reached"                        'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
check "download works from the static address" 'wait_log "$LOG" "[netbench] " $((60*SLOW)) && grep -a "\[netbench\]" "$LOG" | grep -q " x 100000 bytes in "'
check "DHCP client stays off in static mode"   '! grep -a -q "\[dhcp\] DISCOVER" "$LOG"'
grep -a -m1 "\[netbench\]" "$LOG" | tr -d '\r' | sed 's/^/        /'
cp "$LOG" "$SHOT_DIR/serial-phase2.log"
stop_qemu

# ===========================================================================
echo "== Phase 3: DHCP silent -> static fallback; DHCP answers later =="
LOG="$WORK/serial-fallback.log"
make_disk 'fstat=0\n' 'mode=auto\nip=10.0.2.50\nmask=255.255.255.0\ngateway=10.0.2.2\ndns=10.0.2.3\n'
start_qemu "$LOG" -S
mon "set_link nic0 off"          # сеть «без DHCP-сервера»: кадры никуда не уходят
mon "cont"
check "config read: mode auto with a saved address" 'wait_log "$LOG" "[netcfg] mode=auto (network.conf)" $((60*SLOW))'
check "DHCP asked and got no answer"           'wait_log "$LOG" "[dhcp] DISCOVER" 10 && ! grep -a -q "\[dhcp\] OFFER" "$LOG"'
check "static fallback applied (10.0.2.50)"    'wait_log "$LOG" "[netcfg] DHCP silent, static fallback applied: ip=10.0.2.50" $((15*SLOW))'
login "$LOG"
check "desktop reached"                        'wait_log "$LOG" "[wm] main loop start" $((60*SLOW))'
N1=$(grep -a -c "\[dhcp\] DISCOVER" "$LOG")
mon "set_link nic0 on"           # DHCP-сервер «появился»
check "DHCP keeps retrying after boot and binds" 'wait_log "$LOG" "[dhcp] ACK" $((45*SLOW))'
N2=$(grep -a -c "\[dhcp\] DISCOVER" "$LOG")
check "DISCOVER was repeated ($N1 -> $N2)"     '[ "$N2" -gt "$N1" ]'
check "auto switches from the fallback to DHCP" 'wait_log "$LOG" "[netcfg] DHCP lease applied: ip=10.0.2.15" 5'
check "no kernel panic"                        '! grep -a -q "PANIC" "$LOG"'
cp "$LOG" "$SHOT_DIR/serial-phase3.log"
stop_qemu

echo "== stage 48.2 net: $PASS passed, $FAIL failed =="
[ $FAIL = 0 ]

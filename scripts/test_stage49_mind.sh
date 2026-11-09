#!/usr/bin/env bash
# ============================================================================
# test_stage49_mind.sh — AMPLIFY MIND (этап 49), end-to-end.
# ----------------------------------------------------------------------------
# Эталоны — scripts/stage49_golden.txt: их считает НЕЗАВИСИМАЯ numpy-
# реализация tools/mind_ref.py (Windows-python), а не код ядра.
#
# Фаза 0 (хост): движок ядра, собранный обычным gcc (tools/mind_host.c),
#         выдаёт те же ids, что эталон: stories260K и SmolLM2-135M (2 вопроса).
# Фаза 1 (dev-загрузка, 4 ядра, SmolLM2 на /disk/vault/mind, greedy):
#         [smoke 49] — пул AP, klog, токенизация и 32 greedy-токена
#         встроенной модели на пуле и на BSP, инструменты; вход; Ctrl+Space;
#         загрузка весов с диска; ответы модели сверяются с эталоном по ids;
#         /ls /read(запрет) /exec /new; вопрос на естественном языке ->
#         роутер -> ps -> ответ; Esc закрывает панель.
# Фаза 2 (одно ядро): smoke 49 без AP — генерация шагами на BSP.
# Фаза 3 (нет своей модели на диске): Ctrl+Space грузит встроенную
#         stories260K из initrd, продолжение сказки = эталон.
#
# Запуск из WSL:  bash scripts/test_stage49_mind.sh
# Без KVM (TCG) SmolLM2 генерирует ~1–2 ток/с — тест длинный (~15 мин).
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
MODEL=build/mind/SmolLM2-135M-Instruct-Q8_0.gguf
MODEL_SHA=5a1395716f7913741cc51d98581b9b1228d80987a9f7d3664106742eb06bba83
GOLD=scripts/stage49_golden.txt
SHOT_DIR=build/screens-49
WORK=/tmp/amplify-stage49
LOG1="$WORK/serial-mind.log"
LOG2="$WORK/serial-up.log"
LOG3="$WORK/serial-builtin.log"
MON_PORT=4579

PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
gold() { grep "^$1=" "$GOLD" | head -1 | cut -d= -f2-; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found (sudo apt install ovmf)"; exit 1; }
for f in "$ESP" "$DEVDISK" "$GOLD"; do
    [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }
done

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    CPU="-enable-kvm -cpu host"; SLOW=1
else
    CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-6}
    echo "  (no KVM access — TCG, timeouts x$SLOW)"
fi

mkdir -p "$WORK" "$SHOT_DIR"
rm -f "$WORK"/*.log "$SHOT_DIR"/*.ppm "$SHOT_DIR"/*.png

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
mon() { echo "$1" >&3; sleep 0.05; while read -t 0.1 -r -u 3 _; do :; done; }
key() { mon "sendkey $1"; sleep "${2:-0.25}"; }
# Символ -> имя клавиши QEMU (раскладка US).
keyname() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '?') echo shift-slash;; '.') echo dot;;
        ',') echo comma;; '-') echo minus;; '_') echo shift-minus;; ':') echo shift-semicolon;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.12; done; }
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 0.5
    done
    return 1
}
# Ждать, пока строк с маркером станет >= n.
wait_count() {
    local end=$(( $(date +%s) + $4 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        [ "$(grep -c -F "$2" "$1" 2>/dev/null)" -ge "$3" ] && return 0
        sleep 0.5
    done
    return 1
}
shot() {
    local p="$PWD/$SHOT_DIR/$1.ppm"
    mon "screendump $p"
    sleep 1
    [ -f "$p" ] && echo "  [shot] $SHOT_DIR/$1.ppm"
}
start_qemu() {
    local log="$1" cpus="$2"; shift 2
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine pc -m 1G \
        -smp cpus=$cpus,cores=$cpus,threads=1 \
        -vga none -device virtio-vga,xres=1920,yres=1080 \
        -display none -serial "file:$log" \
        -monitor "tcp:127.0.0.1:$MON_PORT,server,nowait" \
        "$@" -no-reboot -no-shutdown >/dev/null 2>&1 &
    QEMU_PID=$!
    mon_connect || { echo "monitor port never opened"; exit 1; }
}
stop_qemu() {
    mon "quit"; sleep 1
    kill -9 "$QEMU_PID" 2>/dev/null; wait "$QEMU_PID" 2>/dev/null
    QEMU_PID=""; exec 3>&- 2>/dev/null
}
# dev-диск: копия + (опц.) модель + mind.conf.
make_disk() {
    local img="$1" with_model="$2" conf="$3"
    cp --sparse=always "$DEVDISK" "$img"
    export MTOOLS_SKIP_CHECK=1
    mmd -D s -i "$img" ::/vault ::/vault/mind ::/Sanctum </dev/null 2>/dev/null
    [ "$with_model" = 1 ] && mcopy -D o -i "$img" "$MODEL" ::/vault/mind/ </dev/null
    printf "$conf" > "$WORK/mind.conf"
    mcopy -D o -i "$img" "$WORK/mind.conf" ::/Sanctum/mind.conf </dev/null
}
login() {
    local log="$1"
    wait_log "$log" "[smoke 37.6c-1] RTC" $((60*SLOW)) || true
    sleep 2
    key ret 0.4; type_text "admin"; key ret 1
    wait_log "$log" "[wm] main loop start" 60
}
# Задать вопрос в панели и дождаться n-го «reply done».
ask() {
    local log="$1" text="$2" n="$3" tmo="$4"
    type_text "$text"; key ret 0.4
    wait_count "$log" "[mind] reply done" "$n" "$tmo"
}
ids_of() { grep "\[mind\] ids: " "$1" | sed -n "${2}p" | sed 's/.*ids: //' | tr -d '\r'; }

HAVE_MODEL=0
if [ -f "$MODEL" ] && [ "$(sha256sum "$MODEL" | cut -d' ' -f1)" = "$MODEL_SHA" ]; then HAVE_MODEL=1; fi

# ===========================================================================
echo "== Phase 0: kernel engine on the host vs independent reference =="
gcc -O2 -pthread -Ikernel -o build/mind_host tools/mind_host.c kernel/arch/x86_64/mind_gguf.c \
    kernel/arch/x86_64/mind_math.c kernel/arch/x86_64/mind_model.c kernel/arch/x86_64/mind_tok.c \
    kernel/arch/x86_64/mind_chat.c -lm 2>"$WORK/host-cc.log"
check "host build of the kernel engine"      '[ -x build/mind_host ]'
H0=$(build/mind_host gen assets/mind/stories260K.gguf "Once upon a time" 32 | grep '^ids:' | sed 's/ids: //')
check "host: stories260K greedy = reference"  '[ "$H0" = "$(gold STORIES_IDS)" ]'
SYS="You are MIND, the assistant built into AMPLIFY-OS, a small operating system written from scratch. Answer briefly and clearly. When tool output is given, answer using only that output."
check "SmolLM2-135M-Instruct-Q8_0 present (sha256 ok)" '[ $HAVE_MODEL = 1 ]'
if [ $HAVE_MODEL = 1 ]; then
    H1=$(build/mind_host gen "$MODEL" "$(gold Q1)" 80 --chat "--system=$SYS" | grep '^ids:' | sed 's/ids: //')
    H2=$(build/mind_host gen "$MODEL" "$(gold Q2)" 80 --chat "--system=$SYS" | grep '^ids:' | sed 's/ids: //')
    check "host: SmolLM2 Q1 ids = reference"      '[ "$H1" = "$(gold Q1_IDS)" ]'
    check "host: SmolLM2 Q2 (80 tokens) = reference" '[ "$H2" = "$(gold Q2_IDS)" ]'
fi

# ===========================================================================
echo "== Phase 1: SmolLM2 in the kernel, Ctrl+Space widget (4 cores) =="
if [ $HAVE_MODEL = 1 ]; then
    make_disk "$WORK/dev.img" 1 'greedy=1\nmax_tokens=80\nctx=1024\n'
    cp "$ESP" "$WORK/esp.img"
    start_qemu "$LOG1" 4 \
        -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
        -drive file="$WORK/esp.img",format=raw,if=ide,index=1
    check "[smoke 49] self-test PASS"            'wait_log "$LOG1" "[smoke 49] AMPLIFY MIND self-test PASS" $((60*SLOW))'
    check "pool: 3 AP workers"                   'grep -q "\[smoke 49\] pool workers=3" "$LOG1"'
    check "pool: parallel_for on all 4 cores"    'grep -q "parallel_for OK slots_mask=0x000F" "$LOG1"'
    check "pool: task on the conductor core"     'grep -q "pool conductor task OK" "$LOG1"'
    check "klog: kernel log readable in memory"  'grep -q "\[smoke 49\] klog OK" "$LOG1"'
    check "builtin model: tokenizer = reference" 'grep -q "tokenize ids=$(gold STORIES_PROMPT) OK" "$LOG1"'
    check "builtin model: 32 greedy on pool = reference" 'grep -q "greedy on pool (ap=yes) OK" "$LOG1"'
    check "builtin model: 32 greedy on BSP = reference"  'grep -q "greedy on BSP OK" "$LOG1"'
    check "tools ps/ls/logs + users.txt denied + <tool> parse" 'grep -q "tools ps=OK ls=OK logs=OK deny=OK parse=OK" "$LOG1"'
    grep "\[smoke 49\]" "$LOG1" | sed 's/^/        /'

    login "$LOG1"
    sleep 3
    key ctrl-spc 0.6
    check "Ctrl+Space opens AMPLIFY MIND"        'wait_log "$LOG1" "[mind] widget open" 10'
    check "auto-discovery of /disk/vault/mind/*.gguf" 'wait_log "$LOG1" "[mind] load /disk/vault/mind/SmolLM2-135M-Instruct-Q8_0.gguf" 10'
    sleep 2; shot 01-loading
    check "SmolLM2 weights loaded from disk"     'wait_log "$LOG1" "[mind] loaded SmolLM2-135M-Instruct-Q8_0: layers=30 embd=576 vocab=49152" $((120*SLOW))'
    grep "\[mind\] loaded SmolLM2" "$LOG1" | sed 's/^/        /'
    sleep 1; shot 02-ready

    ask "$LOG1" "$(gold Q1)" 1 $((60*SLOW))
    check "Q1 answered on the AP conductor"      'grep -q "reply done: stop=1 prefill=$(gold Q1_PROMPT_LEN) gen=8 .* ap=yes" "$LOG1"'
    check "Q1 token ids = reference"             '[ "$(ids_of "$LOG1" 1)" = "$(gold Q1_IDS)" ]'
    check "Q1 text: $(gold Q1_TEXT)"             'grep -q "\[mind\] reply: $(gold Q1_TEXT)" "$LOG1"'
    sleep 1; shot 03-answer-q1

    type_text "/ls /disk/vault/mind"; key ret 1
    check "/ls lists the model directory"        'wait_log "$LOG1" "SmolLM2-135M-Instruct-Q8_0.gguf  144811360 bytes" 20'
    type_text "/read /disk/users.txt"; key ret 1
    check "/read of the password file is denied" 'wait_log "$LOG1" "[mind] data: access denied: credentials file" 20'
    type_text "/exec clock"; key ret 1
    check "/exec captures shell output"          'wait_count "$LOG1" "[mind] tool exec" 1 20 && ! grep "\[mind\] data:" "$LOG1" | tail -1 | grep -q "no output"'
    sleep 1; shot 04-tools

    type_text "/new"; key ret 0.6
    ask "$LOG1" "$(gold Q2)" 2 $((100*SLOW))
    check "Q2: 80 token ids = reference (fresh context after /new)" '[ "$(ids_of "$LOG1" 2)" = "$(gold Q2_IDS)" ]'
    sleep 1; shot 05-answer-q2

    ask "$LOG1" "what processes are running?" 3 $((120*SLOW))
    check "question routed to the ps tool"       'grep -q "\[mind\] data: uptime:" "$LOG1"'
    check "MIND answered using the tool output"  '[ "$(grep -c "\[mind\] reply done" "$LOG1")" -ge 3 ]'
    sleep 1; shot 06-routed-ps
    key esc 0.6
    check "Esc closes the panel"                 'wait_log "$LOG1" "[mind] widget close" 10'
    sleep 1; shot 07-closed
    grep -E "\[mind\] (reply done|reply:)" "$LOG1" | sed 's/^/        /'
    stop_qemu
    cp -f "$LOG1" "$SHOT_DIR/" 2>/dev/null
    rm -f "$WORK/dev.img" "$WORK/esp.img"
fi

# ===========================================================================
echo "== Phase 2: uniprocessor boot (no AP pool) =="
cp --sparse=always "$DEVDISK" "$WORK/dev.img"
cp "$ESP" "$WORK/esp.img"
start_qemu "$LOG2" 1 \
    -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
    -drive file="$WORK/esp.img",format=raw,if=ide,index=1
check "[smoke 49] PASS on a single core"     'wait_log "$LOG2" "[smoke 49] AMPLIFY MIND self-test PASS" $((60*SLOW))'
check "no pool: generation stepped on BSP"   'grep -q "pool workers=0" "$LOG2" && grep -q "greedy on pool (ap=no) OK" "$LOG2"'
stop_qemu
rm -f "$WORK/dev.img" "$WORK/esp.img"

# ===========================================================================
echo "== Phase 3: no model on disk -> builtin stories260K from initrd =="
make_disk "$WORK/dev.img" 0 'greedy=1\nmax_tokens=32\n'
cp "$ESP" "$WORK/esp.img"
start_qemu "$LOG3" 4 \
    -drive file="$WORK/dev.img",format=raw,if=ide,index=0,media=disk \
    -drive file="$WORK/esp.img",format=raw,if=ide,index=1
login "$LOG3"
sleep 2
key ctrl-spc 0.6
check "fallback to the builtin initrd model" 'wait_log "$LOG3" "[mind] loaded stories260K" 20'
ask "$LOG3" "Once upon a time" 1 $((30*SLOW))
check "story continuation ids = reference"   '[ "$(ids_of "$LOG3" 1)" = "$(gold STORIES_IDS)" ]'
check "story text starts as expected"        'grep -q "\[mind\] reply: , there was a little girl named Lily" "$LOG3"'
sleep 1; shot 08-builtin-story
# shell-команда muse в терминале Pulse (Super+Space).
key esc 0.6
key meta_l-spc 1.5
type_text "muse ps"; key ret 1
check "muse ps runs the tool"                'wait_log "$LOG3" "[mind] data: uptime:" 20'
# Вывод терминала дублируется в serial (fb_console_putc) — проверяем, что
# карточка данных действительно напечатана в окне, а не только выполнена.
check "muse ps prints the data in the terminal" 'wait_log "$LOG3" "[data] ps" 10'
type_text "muse bench 8"; key ret 1
check "muse bench reports speed"             'wait_log "$LOG3" "[mind] bench tok/s x10=" $((30*SLOW))'
sleep 1; shot 09-muse-terminal
stop_qemu
cp -f "$LOG3" "$SHOT_DIR/" 2>/dev/null

# PPM -> PNG, если есть PIL — иначе оставляем PPM.
if python3 -c "import PIL" 2>/dev/null; then
    for f in "$SHOT_DIR"/*.ppm; do python3 -c "from PIL import Image;Image.open('$f').save('${f%.ppm}.png')"; done
fi

echo
echo "== stage 49: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]

#!/usr/bin/env bash
# ============================================================================
# test_stage49_qwen3.sh — AMPLIFY MIND на Qwen3-1.7B (подэтап 49.1), end-to-end.
# ----------------------------------------------------------------------------
# Эталоны — scripts/stage49_qwen3_golden.txt: их считает НЕЗАВИСИМАЯ numpy-
# реализация tools/mind_ref.py (Windows-python, scripts/gen_qwen3_golden.sh),
# а не код ядра. Промпты сверяются с ОФИЦИАЛЬНЫМ jinja-шаблоном из GGUF,
# токенизатор эталона — с HF tokenizers (tokenizer.json модели).
#
# Фаза 0 (хост): движок ядра, собранный gcc (tools/mind_host.c):
#         промпты = официальный шаблон (5 вариантов, в т.ч. <tools> и think);
#         токенизатор qwen2 = эталон на 3021 тексте; greedy-ids ответов
#         (EN, RU) и агентный цикл (<tool_call> ls -> <tool_response> ->
#         ответ) = эталон; пакетный prefill = поштучный.
# Фаза 1 (QEMU q35, AHCI, 6 ГБ, 4 ядра): smoke 49 (+49.1: память выше 4 ГБ,
#         пакетный prefill бит-в-бит, разбор <tool_call>); вход; Ctrl+Space;
#         выбор модели по RAM; веса Qwen3 — выше 4 ГБ; кэш системного
#         промпта; ответы EN и RU (раскладка RU в панели, Ctrl+L) = эталон;
#         вопрос о процессах -> ps + точные факты шаблоном + Markdown.
# Фаза 2 (QEMU, agent=1 tools=ls): модель САМА вызывает ls в формате Qwen3,
#         ядро выполняет инструмент и модель отвечает по результату = эталон.
# Фаза 3 (установщик, q35, 4 ГБ): ISO с Qwen3 -> потоковое копирование
#         1.8 ГБ на системный раздел, сверка, sha256 на хосте; установленная
#         система выбирает Qwen3 и грузит её выше 4 ГБ.
#
# Запуск из WSL:  bash scripts/test_stage49_qwen3.sh [фазы, по умолчанию 0123]
# Без KVM (TCG) Qwen3-1.7B считает ~0.3 ток/с — тест долгий (см. CLAUDE.md §7).
# ============================================================================
set -u
cd "$(dirname "$0")/.."
export LC_ALL=C.UTF-8
PHASES=${1:-0123}

ESP=build/esp.img
ISO=build/myos-x86_64.iso
MODEL=build/mind/Qwen3-1.7B-Q8_0.gguf
MODEL_NAME=Qwen3-1.7B-Q8_0.gguf
GOLD=scripts/stage49_qwen3_golden.txt
SHOT_DIR=build/screens-49-qwen3
WORK=build/q49/test              # на D: — tmpfs WSL мал для дисков по 3-4 ГБ
LOG1="$WORK/serial-qwen3.log"
LOG2="$WORK/serial-agent.log"
LOG3="$WORK/serial-install.log"
LOG4="$WORK/serial-installed.log"
MON_PORT=4583
USER_NAME=mark
USER_PASS=test1234

PASS=0; FAIL=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }
gold() { grep "^$1=" "$GOLD" | head -1 | cut -d= -f2- | tr -d '\r'; }

OVMF=""
for f in /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd /usr/share/pve-edk2-firmware/OVMF_CODE.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found (sudo apt install ovmf)"; exit 1; }
for f in "$ESP" "$GOLD" "$MODEL"; do
    [ -f "$f" ] || { echo "$f not found"; exit 1; }
done
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    CPU="-enable-kvm -cpu host"; SLOW=1
else
    CPU="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-8}
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
keyname() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '?') echo shift-slash;; '.') echo dot;;
        ',') echo comma;; '-') echo minus;; '_') echo shift-minus;; ':') echo shift-semicolon;;
        [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";;
        *) echo "$1";;
    esac
}
type_text() { local s="$1"; for ((i=0; i<${#s}; i++)); do key "$(keyname "${s:i:1}")" 0.12; done; }
# ЙЦУКЕН: кириллица -> клавиша US (панель MIND в режиме RU, Ctrl+L).
declare -A RU=( [й]=q [ц]=w [у]=e [к]=r [е]=t [н]=y [г]=u [ш]=i [щ]=o [з]=p [х]=bracket_left
    [ъ]=bracket_right [ф]=a [ы]=s [в]=d [а]=f [п]=g [р]=h [о]=j [л]=k [д]=l [ж]=semicolon
    [э]=apostrophe [я]=z [ч]=x [с]=c [м]=v [и]=b [т]=n [ь]=m [б]=comma [ю]=dot [ё]=grave_accent )
type_ru() {
    local s="$1" c lc
    for ((i=0; i<${#s}; i++)); do
        c="${s:i:1}"; lc="${c,,}"
        if [ -n "${RU[$lc]:-}" ]; then
            if [ "$c" != "$lc" ]; then key "shift-${RU[$lc]}" 0.12; else key "${RU[$lc]}" 0.12; fi
        else key "$(keyname "$c")" 0.12; fi
    done
}
wait_log() {
    local end=$(( $(date +%s) + $3 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        grep -a -q -F "$2" "$1" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}
wait_count() {
    local end=$(( $(date +%s) + $4 ))
    while [ "$(date +%s)" -lt "$end" ]; do
        [ "$(grep -a -c -F "$2" "$1" 2>/dev/null)" -ge "$3" ] && return 0
        sleep 1
    done
    return 1
}
shot() {
    local p="$PWD/$SHOT_DIR/$1.ppm"
    mon "screendump $p"
    sleep 1
    [ -f "$p" ] && echo "  [shot] $SHOT_DIR/$1.ppm"
}
start_q35() {
    local log="$1" mem="$2"; shift 2
    qemu-system-x86_64 -bios "$OVMF" $CPU -machine q35 -m "$mem" \
        -smp cpus=4,cores=4,threads=1 \
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
login_admin() {
    local log="$1"
    wait_log "$log" "[smoke 37.6c-1] RTC" $((60*SLOW)) || true
    sleep 2
    key ret 0.4; type_text "admin"; key ret 1
    wait_log "$log" "[wm] main loop start" 60
}
ask() {
    local log="$1" text="$2" n="$3" tmo="$4"
    type_text "$text"; key ret 0.4
    wait_count "$log" "[mind] reply done" "$n" "$tmo"
}
ids_of() { grep -a "\[mind\] ids: " "$1" | sed -n "${2}p" | sed 's/.*ids: //' | tr -d '\r'; }
# Диск dev-типа с моделью (ESP отдельным диском, загрузка с ESP).
dev_drives() {
    echo "-drive file=$1,format=raw,if=none,id=d0 -device ide-hd,drive=d0,bus=ide.0" \
         "-drive file=$WORK/esp.img,format=raw,if=none,id=d1 -device ide-hd,drive=d1,bus=ide.1,bootindex=0"
}

SYS=$(gold SYS)
HAVE=0
[ "$(sha256sum "$MODEL" | cut -d' ' -f1)" = "$(gold MODEL_SHA)" ] && HAVE=1

# ===========================================================================
if [[ $PHASES == *0* ]]; then
echo "== Phase 0: kernel engine on the host vs independent reference =="
check "Qwen3-1.7B-Q8_0 present (sha256 = huggingface.co API)" '[ $HAVE = 1 ]'
gcc -O2 -pthread -Ikernel -o build/mind_host tools/mind_host.c kernel/arch/x86_64/mind_gguf.c \
    kernel/arch/x86_64/mind_math.c kernel/arch/x86_64/mind_model.c kernel/arch/x86_64/mind_tok.c \
    kernel/arch/x86_64/mind_chat.c -lm 2>"$WORK/host-cc.log"
check "host build of the kernel engine"      '[ -x build/mind_host ]'
bash scripts/check_qwen3_prompt.sh > "$WORK/prompts.txt" 2>&1
for c in user_only system tools_all think russian; do
    check "prompt '$c' = official Qwen3 chat template" 'grep -q "PROMPT $c OK" "$WORK/prompts.txt"'
done
if [ -f build/q49/texts.hex ] && [ -f build/q49/ref_ids.txt ]; then
    build/mind_host tokfile "$MODEL" build/q49/texts.hex > "$WORK/host_ids.txt"
    check "tokenizer qwen2: 3021 texts = reference (reference = HF tokenizer.json)" \
          'diff -q <(tr -d "\r" < build/q49/ref_ids.txt) "$WORK/host_ids.txt" >/dev/null'
else
    bad "tokenizer dump missing (run scripts/gen_qwen3_golden.sh tok on Windows)"
fi
H=$(build/mind_host gen "$MODEL" "$(gold Q_EN)" 24 --chat "--system=$SYS")
check "host: EN answer ids = reference ($(gold TEXT_en))" '[ "$(echo "$H" | sed -n "s/^ids: //p")" = "$(gold IDS_en)" ]'
echo "$H" | grep '^stats' | sed 's/^/        /'
H=$(build/mind_host gen "$MODEL" "$(gold Q_RU)" 32 --chat "--system=$SYS")
check "host: RU answer ids = reference"     '[ "$(echo "$H" | sed -n "s/^ids: //p")" = "$(gold IDS_ru)" ]'
printf '/disk/vault/mind:\n  Qwen3-1.7B-Q8_0.gguf  1834426016 bytes\n' > "$WORK/tool_ls.txt"
H=$(build/mind_host gen "$MODEL" "$(gold Q_TOOL)" 48 --chat "--system=$SYS" --tools=tools/mind_tools.json "--tool-response=$WORK/tool_ls.txt")
check "host: model emits <tool_call> ls (all tools) = reference" '[ "$(echo "$H" | sed -n "s/^ids: //p")" = "$(gold IDS_tool)" ]'
check "host: answer after <tool_response> = reference" '[ "$(echo "$H" | sed -n "s/^ids2: //p")" = "$(gold IDS2_tool)" ]'
echo "$H" | grep -E '^text2?:' | sed 's/^/        /'
H=$(build/mind_host gen "$MODEL" "$(gold Q_TOOL)" 48 --chat "--system=$SYS" --tools=ls "--tool-response=$WORK/tool_ls.txt")
check "host: tools=ls — call and answer = reference" \
      '[ "$(echo "$H" | sed -n "s/^ids: //p")" = "$(gold IDS_toolls)" ] && [ "$(echo "$H" | sed -n "s/^ids2: //p")" = "$(gold IDS2_toolls)" ]'
H=$(build/mind_host gen "$MODEL" "$(gold Q_THINK)" 32 --chat "--system=$SYS" --think)
check "host: think mode (<think> not closed in advance) = reference" '[ "$(echo "$H" | sed -n "s/^ids: //p")" = "$(gold IDS_think)" ]'
T1=$(MIND_TOP=1 build/mind_host gen "$MODEL" "$(gold Q_EN)" 1 --chat "--system=$SYS" | grep '^top:')
T2=$(MIND_TOP=1 build/mind_host gen "$MODEL" "$(gold Q_EN)" 1 --chat "--system=$SYS" --nobatch | grep '^top:')
check "host: batched prefill logits = token-by-token (bit-exact)" '[ -n "$T1" ] && [ "$T1" = "$T2" ]'
fi

# ===========================================================================
if [[ $PHASES == *1* ]]; then
echo "== Phase 1: Qwen3 in the kernel (q35, AHCI, 6 GB, 4 cores) =="
# Диск собирается из dev-образа; если тот повреждён (другой тест погасил QEMU
# посреди записи), mtools падает — говорим об этом прямо, а не двадцатью
# провалами «MIND не открылась».
bash scripts/make_qwen3_disk.sh "$WORK/q.img" 3G "$MODEL" 'greedy=1\nmax_tokens=32\nagent=0\nctx=1024\n' \
    || bad "test disk with the model was NOT built (dev image corrupt? rm build/disk-x86_64.img && make)"
cp "$ESP" "$WORK/esp.img"
start_q35 "$LOG1" 6G $(dev_drives "$WORK/q.img")
check "[smoke 49] self-test PASS"            'wait_log "$LOG1" "[smoke 49] AMPLIFY MIND self-test PASS" $((60*SLOW))'
check "RAM above 4 GB mapped (4096 MB)"      'grep -a -q "\[pmm\] RAM above 4 GB: 4096 MB" "$LOG1"'
check "[smoke 49.1] 64 MB block above 4 GB"  'grep -a -q "\[smoke 49.1\] highmem OK block@0x00000001" "$LOG1"'
check "[smoke 49.1] batch prefill bit-exact" 'grep -a -q "batch prefill = sequential: OK (bit-exact)" "$LOG1"'
check "[smoke 49.1] <tool_call> JSON parser" 'grep -a -q "\[smoke 49.1\] tool_call json parse OK" "$LOG1"'
grep -a -E "\[pmm\] RAM above|\[smoke 49.1\]" "$LOG1" | sed 's/^/        /'
login_admin "$LOG1"
sleep 3
key ctrl-spc 0.6
check "Ctrl+Space opens AMPLIFY MIND"        'wait_log "$LOG1" "[mind] widget open" 10'
check "model choice by RAM: Qwen3 fits"      'wait_log "$LOG1" "[mind] candidate /disk/vault/mind/$MODEL_NAME 1749 MB: fits" 20'
check "Qwen3 weights loaded (28 layers, vocab 151936)" \
      'wait_log "$LOG1" "[mind] loaded Qwen3-1.7B-Q8_0: layers=28 embd=2048 vocab=151936 ctx=1024" $((60*SLOW))'
check "weights live above 4 GB"              'grep -a "\[mind\] loaded Qwen3" "$LOG1" | grep -q "weights@0x00000001"'
grep -a "\[mind\] loaded Qwen3" "$LOG1" | sed 's/^/        /'
sleep 1; shot 01-loaded
check "system prompt KV cached once"         'wait_log "$LOG1" "[mind] system prompt cached: " $((120*SLOW))'
grep -a "system prompt cached" "$LOG1" | sed 's/^/        /'
ask "$LOG1" "$(gold Q_EN)" 1 $((120*SLOW))
check "EN answer ids = reference ($(gold TEXT_en))" '[ "$(ids_of "$LOG1" 1)" = "$(gold IDS_en)" ]'
grep -a "\[mind\] reply done" "$LOG1" | tail -1 | sed 's/^/        /'
sleep 1; shot 02-answer-en
key ctrl-l 0.4
check "Ctrl+L switches the input to RU"      'wait_log "$LOG1" "[mind] layout RU" 10'
type_ru "$(gold Q_RU)"; key ret 0.4
check "RU question typed and answered"       'wait_count "$LOG1" "[mind] reply done" 2 $((150*SLOW))'
check "RU answer ids = reference"            '[ "$(ids_of "$LOG1" 2)" = "$(gold IDS_ru)" ]'
grep -a "\[mind\] user: " "$LOG1" | tail -1 | sed 's/^/        /'
sleep 1; shot 03-answer-ru
key ctrl-l 0.4
type_text "/new"; key ret 0.6
ask "$LOG1" "$(gold Q_PS)" 3 $((200*SLOW))
check "question routed to ps"                'grep -a -q "\[mind\] data: uptime:" "$LOG1"'
check "exact facts from a template (no model)" 'grep -a "\[mind\] facts: " "$LOG1" | grep -q "\*\*Programs:\*\*"'
grep -a "\[mind\] facts: " "$LOG1" | tail -1 | sed 's/^/        /'
sleep 1; shot 04-routed-ps-markdown
type_text "/new"; key ret 0.6
type_text "/think"; key ret 0.6
check "/think switches the reasoning mode on" 'wait_log "$LOG1" "[mind] think on" 10'
ask "$LOG1" "$(gold Q_THINK)" 4 $((200*SLOW))
check "think mode: reply starts with <think>" 'grep -a "\[mind\] reply: " "$LOG1" | tail -1 | grep -q "<think>"'
check "think mode ids = reference (--think)"  '[ "$(ids_of "$LOG1" 4)" = "$(gold IDS_think)" ]'
sleep 1; shot 05-think-dimmed
grep -a -E "\[mind\] (reply done|reply:)" "$LOG1" | sed 's/^/        /'
stop_qemu
cp -f "$LOG1" "$SHOT_DIR/" 2>/dev/null
rm -f "$WORK/q.img"
fi

# ===========================================================================
if [[ $PHASES == *2* ]]; then
echo "== Phase 2: the model calls a tool itself (agent=1, tools=ls) =="
bash scripts/make_qwen3_disk.sh "$WORK/q.img" 3G "$MODEL" 'greedy=1\nmax_tokens=48\nagent=1\ntools=ls\nctx=1024\n' \
    || bad "test disk with the model was NOT built (dev image corrupt? rm build/disk-x86_64.img && make)"
cp "$ESP" "$WORK/esp.img"
start_q35 "$LOG2" 6G $(dev_drives "$WORK/q.img")
login_admin "$LOG2"
sleep 3
key ctrl-spc 0.6
check "Qwen3 loaded"                         'wait_log "$LOG2" "[mind] loaded Qwen3-1.7B-Q8_0" $((60*SLOW))'
check "system prompt with <tools> cached"    'wait_log "$LOG2" "[mind] system prompt cached: " $((300*SLOW))'
ask "$LOG2" "$(gold Q_TOOL)" 1 $((150*SLOW))
check "model called ls itself (<tool_call>)" 'wait_log "$LOG2" "[mind] model calls tool: ls /disk/vault/mind" 10'
check "tool call ids = reference"            '[ "$(ids_of "$LOG2" 1)" = "$(gold IDS_toolls)" ]'
check "ls executed by the kernel"            'grep -a -q "\[mind\] data: /disk/vault/mind:" "$LOG2"'
check "answer after <tool_response>"         'wait_count "$LOG2" "[mind] reply done" 2 $((200*SLOW))'
check "answer ids = reference ($(gold TEXT2_toolls | cut -c1-60))" '[ "$(ids_of "$LOG2" 2)" = "$(gold IDS2_toolls)" ]'
sleep 1; shot 06-agent-tool-call
grep -a -E "\[mind\] (reply done|reply:|model calls)" "$LOG2" | sed 's/^/        /'
stop_qemu
cp -f "$LOG2" "$SHOT_DIR/" 2>/dev/null
rm -f "$WORK/q.img"
fi

# ===========================================================================
if [[ $PHASES == *3* ]]; then
echo "== Phase 3: installer copies Qwen3 (q35, 4 GB), installed system loads it =="
[ -f "$ISO" ] || { echo "$ISO not found"; exit 1; }
TARGET="$WORK/target.img"
rm -f "$TARGET"; truncate -s 4G "$TARGET"
start_q35 "$LOG3" 4G \
    -drive file="$TARGET",format=raw,if=none,id=target \
    -device ide-hd,drive=target,bus=ide.0,bootindex=1 \
    -drive file="$ISO",format=raw,if=none,id=live,media=cdrom,readonly=on \
    -device ide-cd,drive=live,bus=ide.2,bootindex=0
check "installer UI started"                 'wait_log "$LOG3" "[installer] starting" $((90*SLOW))'
check "RAM above 4 GB on q35 with 4 GB"      'grep -a -q "\[pmm\] RAM above 4 GB: 2048 MB" "$LOG3"'
sleep 2
key ret 0.6
key spc 0.4
key ret 0.6
key ret 0.6
key ret 0.6
key tab;  type_text "$USER_NAME"
key tab;  type_text "$USER_PASS"
key tab;  type_text "$USER_PASS"
key ret 0.6
key ret 0.3
check "Qwen3 copied by streaming and verified" \
      'wait_log "$LOG3" "[install] mind model copied: /vault/mind/$MODEL_NAME 1834426016 bytes, verified" $((900*SLOW))'
check "installation completed"               'wait_log "$LOG3" "[install] completed successfully" $((300*SLOW))'
grep -a -E "\[install\] (mind|ERROR)" "$LOG3" | sed 's/^/        /'
sleep 1; shot 07-install-complete
stop_qemu
cp -f "$LOG3" "$SHOT_DIR/" 2>/dev/null
SFDISK=$(command -v sfdisk || echo /usr/sbin/sfdisk)
PT=$("$SFDISK" -d "$TARGET" 2>/dev/null)
SYS_START=$(echo "$PT" | grep -m1 "EBD0A0A2" | sed -E 's/.*start= *([0-9]+).*/\1/')
if [ -n "$SYS_START" ]; then
    export MTOOLS_SKIP_CHECK=1
    mcopy -o -i "$TARGET@@$((SYS_START*512))" "::/vault/mind/$MODEL_NAME" "$WORK/model.copy" 2>/dev/null
    check "host: installed model sha256 = source" '[ "$(sha256sum < "$WORK/model.copy" | cut -d" " -f1)" = "$(gold MODEL_SHA)" ]'
    rm -f "$WORK/model.copy"
    # bs=1M: по drvfs (D:) 512-байтовые записи шли бы почти час.
    SYS_SIZE=$(echo "$PT" | grep -m1 "EBD0A0A2" | sed -E 's/.*size= *([0-9]+).*/\1/')
    dd if="$TARGET" of="$WORK/sys.part" bs=1M iflag=skip_bytes,count_bytes skip=$((SYS_START*512)) \
       count=$((SYS_SIZE*512)) conv=sparse status=none 2>/dev/null
    check "host: fsck.fat of the system partition is clean" '/usr/sbin/fsck.fat -n "$WORK/sys.part" >/dev/null 2>&1'
    rm -f "$WORK/sys.part"
else
    bad "system partition not found in GPT"
fi
start_q35 "$LOG4" 4G \
    -drive file="$TARGET",format=raw,if=none,id=target \
    -device ide-hd,drive=target,bus=ide.0,bootindex=0
check "installed system booted"              'wait_log "$LOG4" "[fat32] mounted" $((90*SLOW))'
wait_log "$LOG4" "[smoke 37.6c-1] RTC" $((60*SLOW)) || true
sleep 2
key ret 0.4; type_text "$USER_PASS"; key ret 1
check "login"                                'wait_log "$LOG4" "[identity] unlocked user=$USER_NAME" 30'
wait_log "$LOG4" "[wm] main loop start" 30 || true
sleep 3
key ctrl-spc 0.6
check "MIND picks Qwen3 of the two installed models" 'wait_log "$LOG4" "[mind] load /disk/vault/mind/$MODEL_NAME" 20'
check "Qwen3 loaded on the installed system (above 4 GB)" \
      'wait_log "$LOG4" "[mind] loaded Qwen3-1.7B-Q8_0" $((90*SLOW)) && grep -a "\[mind\] loaded Qwen3" "$LOG4" | grep -q "weights@0x00000001"'
grep -a -E "\[mind\] (candidate|loaded)" "$LOG4" | sed 's/^/        /'
sleep 2; shot 08-installed-qwen3
stop_qemu
cp -f "$LOG4" "$SHOT_DIR/" 2>/dev/null
rm -f "$TARGET"
fi

# PPM -> PNG, если есть PIL — иначе оставляем PPM.
if python3 -c "import PIL" 2>/dev/null; then
    for f in "$SHOT_DIR"/*.ppm; do python3 -c "from PIL import Image;Image.open('$f').save('${f%.ppm}.png')"; done
fi

echo
echo "== stage 49.1 (Qwen3): $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]

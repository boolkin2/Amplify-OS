#!/usr/bin/env bash
# Сверка промпта Qwen3: ids, которые строит mind_chat.c (хост-сборка ядра
# build/mind_host), против ОФИЦИАЛЬНОГО jinja-шаблона из GGUF — эталоны
# PROMPT_* в scripts/stage49_qwen3_golden.txt (их пишет gen_qwen3_golden.sh
# через tools/mind_ref.py render). Печатает "PROMPT <имя> OK|MISMATCH".
set -u
cd "$(dirname "$0")/.."
M=build/mind/Qwen3-1.7B-Q8_0.gguf
G=scripts/stage49_qwen3_golden.txt
gold() { grep "^$1=" "$G" | head -1 | cut -d= -f2- | tr -d '\r'; }
SYS=$(gold SYS)
run_case() {
    local name="$1" user="$2"; shift 2
    local h r
    h=$(build/mind_host prompt "$M" "$user" "$@" | sed -n 's/^prompt: //p')
    r=$(gold "PROMPT_$name")
    if [ -n "$h" ] && [ "$h" = "$r" ]; then echo "PROMPT $name OK ($(echo "$h" | tr ',' '\n' | wc -l) tokens)"
    else echo "PROMPT $name MISMATCH"; echo "  host: ${h:0:400}"; echo "  ref:  ${r:0:400}"; fi
}
run_case user_only "$(gold Q_EN)"
run_case system "$(gold Q_PS)" "--system=$SYS"
run_case tools_all "$(gold Q_TOOL)" "--system=$SYS" "--tools=tools/mind_tools.json"
run_case think "Hello!" "--system=$SYS" --think
run_case russian "Сколько свободной памяти? Ответь «кратко» — одним предложением." "--system=$SYS"

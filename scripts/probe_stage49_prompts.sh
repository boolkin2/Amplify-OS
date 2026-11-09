#!/usr/bin/env bash
# Подбор формата «данные ОС + вопрос» для SmolLM2-135M (хост, greedy, build/mind_host).
cd "$(dirname "$0")/.."
M=build/mind/SmolLM2-135M-Instruct-Q8_0.gguf
SYS="You are MIND, the assistant built into AMPLIFY-OS, a small operating system written from scratch. Answer briefly and clearly. When tool output is given, answer using only that output."
DATA=$(printf 'uptime: 167 s\ncpu: 4 cores online\n  cpu0 (BSP): window manager, desktop, drivers\n  cpu1: MIND conductor - idle\n  cpu2: MIND compute worker - idle\n  cpu3: MIND compute worker - idle\nmemory: 698 MB free of 1017 MB; kernel heap 12/128 MB\ntasks: 0 (no user programs running)\nwindows: 0 (desktop only)\nmind: SmolLM2-135M-Instruct-Q8_0')
Q="what processes are running?"
run() { echo "== $1"; build/mind_host gen $M "$2" 70 --chat "--system=$3" | grep '^text' | cut -c1-400; }
run A "$(printf 'Tool output (ps):\n%s\n\nQuestion: %s' "$DATA" "$Q")" "$SYS"
run B "$(printf '%s\n\nUse this information about the computer:\n%s' "$Q" "$DATA")" "$SYS"
run C "$(printf 'Here is the current state of this computer:\n%s\n\nUsing only the information above, answer: %s' "$DATA" "$Q")" "$SYS"
run D "$Q" "$(printf '%s\n\nCurrent state of this computer:\n%s' "$SYS" "$DATA")"
run E "$(printf 'Read this system report and answer the question.\n\nReport:\n%s\n\nQuestion: %s\nAnswer:' "$DATA" "$Q")" "$SYS"
Q2="how much memory is free?"
run C2 "$(printf 'Here is the current state of this computer:\n%s\n\nUsing only the information above, answer: %s' "$DATA" "$Q2")" "$SYS"
run D2 "$Q2" "$(printf '%s\n\nCurrent state of this computer:\n%s' "$SYS" "$DATA")"
run E2 "$(printf 'Read this system report and answer the question.\n\nReport:\n%s\n\nQuestion: %s\nAnswer:' "$DATA" "$Q2")" "$SYS"

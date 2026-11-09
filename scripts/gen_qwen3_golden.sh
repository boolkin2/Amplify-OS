#!/usr/bin/env bash
# ============================================================================
# gen_qwen3_golden.sh — эталоны подэтапа 49.1 (Qwen3-1.7B) из НЕЗАВИСИМОЙ
# numpy-реализации tools/mind_ref.py. Запускать на Windows (Git Bash:
# Windows-python с numpy и jinja2; из WSL python.exe не запускается).
# Пишет scripts/stage49_qwen3_golden.txt, который затем читает WSL-тест
# scripts/test_stage49_qwen3.sh. Под numpy один прогон 1.7B — минуты.
#   bash scripts/gen_qwen3_golden.sh [prompts|gen|all]
# ============================================================================
set -u
cd "$(dirname "$0")/.."
export PYTHONIOENCODING=utf-8
M=build/mind/Qwen3-1.7B-Q8_0.gguf
G=scripts/stage49_qwen3_golden.txt
T=tools/mind_tools.json
SYS="You are MIND, the assistant built into AMPLIFY-OS, an operating system written from scratch. You run locally on this computer, nothing leaves it. Answer briefly and clearly, in the language of the question. Facts about this computer (processes, memory, files, logs) come only from tool results - never invent them."
WHAT=${1:-all}

# Вопросы (как их задаёт тест).
Q_EN="What is the capital of France?"
# Без точки и запятых: в раскладке RU панели MIND клавиши . и , — буквы ю/б,
# а тест набирает вопрос через sendkey.
Q_RU="Какая столица у Франции? Ответь одним предложением"
Q_TOOL="List the files in /disk/vault/mind"
Q_PS="What processes are running?"
Q_THINK="What is two plus two?"

tmp=$(mktemp)
{
    echo "# Эталоны подэтапа 49.1 (AMPLIFY MIND на Qwen3-1.7B). Получены НЕЗАВИСИМОЙ"
    echo "# numpy-реализацией tools/mind_ref.py (Windows-python) — ядро их не видит."
    echo "# Генератор: scripts/gen_qwen3_golden.sh. Модель: Qwen3-1.7B-Q8_0.gguf (Qwen/Qwen3-1.7B-GGUF)"
    echo "MODEL_SHA=061b54daade076b5d3362dac252678d17da8c68f07560be70818cace6590cb1a"
    echo "# SYS — k_system_qwen из kernel/arch/x86_64/mind.c (должен совпадать дословно)."
    echo "SYS=$SYS"
    echo "Q_EN=$Q_EN"
    echo "Q_RU=$Q_RU"
    echo "Q_TOOL=$Q_TOOL"
    echo "Q_PS=$Q_PS"
    echo "Q_THINK=$Q_THINK"
} > "$tmp"
# Эталоны, которые этот запуск не пересчитывает, переносим из старого файла.
keep() { grep -E "^($1)=" "$G" 2>/dev/null >> "$tmp" || true; }

render() {   # имя, вопрос, доп. аргументы -> PROMPT_<имя>= (официальный jinja-шаблон)
    local name="$1" q="$2"; shift 2
    local out
    out=$(python tools/mind_ref.py render "$M" "$q" "$@" | tr -d '\r')
    echo "PROMPT_${name}=$(echo "$out" | sed -n 's/^official: //p')" >> "$tmp"
    echo "$out" | grep '^match:' | sed "s/^/  $name: ref builder vs official /"
}
gen() {      # имя, вопрос, n, доп. аргументы -> IDS_/TEXT_ (+ IDS2_/TEXT2_)
    local name="$1" q="$2" n="$3"; shift 3
    local out
    out=$(python tools/mind_ref.py gen "$M" "$q" "$n" --chat "$@" | tr -d '\r')
    echo "IDS_${name}=$(echo "$out" | sed -n 's/^ids: //p')" >> "$tmp"
    echo "TEXT_${name}=$(echo "$out" | sed -n 's/^text: //p')" >> "$tmp"
    echo "GAPS_${name}=$(echo "$out" | sed -n 's/^gaps: //p')" >> "$tmp"
    if echo "$out" | grep -q '^ids2: '; then
        echo "IDS2_${name}=$(echo "$out" | sed -n 's/^ids2: //p')" >> "$tmp"
        echo "TEXT2_${name}=$(echo "$out" | sed -n 's/^text2: //p')" >> "$tmp"
    fi
    echo "  $name: $(echo "$out" | sed -n 's/^text: //p' | cut -c1-160)"
}

if [ "$WHAT" != all ] && [ "$WHAT" != prompts ]; then keep 'PROMPT_[a-z_]+'; fi
if [ "$WHAT" = all ] || [ "$WHAT" = prompts ]; then
    echo "== prompts (official jinja template from the GGUF) =="
    render user_only "$Q_EN"
    render system "$Q_PS" "--system=$SYS"
    render tools_all "$Q_TOOL" "--system=$SYS" "--tools=$T"
    render think "Hello!" "--system=$SYS" --think
    render russian "Сколько свободной памяти? Ответь «кратко» — одним предложением." "--system=$SYS"
fi
if [ "$WHAT" = all ] || [ "$WHAT" = tok ]; then
    echo "== tokenizer dump (texts + ids) =="
    python tools/mind_ref.py tokdump "$M" build/q49/texts.hex build/q49/ref_ids.txt
    python tools/mind_ref.py tokcheck "$M" build/mind/hf/tokenizer.json | tail -1 | sed 's/^/  vs HF tokenizer.json: /'
fi
if [ "$WHAT" != all ] && [ "$WHAT" != gen ]; then keep '(IDS|TEXT|GAPS|IDS2|TEXT2)_[a-z]+'; fi
if [ "$WHAT" = think ]; then
    # Только режим размышления (фаза 1 теста: /new, /think, вопрос).
    grep -v -E '^(IDS|TEXT|GAPS)_think=' "$tmp" > "$tmp.2"; mv "$tmp.2" "$tmp"
    gen think "$Q_THINK" 32 "--system=$SYS" --think
fi
if [ "$WHAT" = all ] || [ "$WHAT" = gen ]; then
    echo "== greedy generation =="
    # Ответ инструмента ls — тот же текст, что выдаёт mind_tools.c:tool_ls
    # на тестовом диске с одной моделью (фаза 2 теста в QEMU).
    printf '/disk/vault/mind:\n  Qwen3-1.7B-Q8_0.gguf  1834426016 bytes\n' > build/q49/tool_ls.txt
    gen en "$Q_EN" 24 "--system=$SYS"
    gen ru "$Q_RU" 32 "--system=$SYS"
    gen tool "$Q_TOOL" 48 "--system=$SYS" "--tools=$T" "--tool-response=build/q49/tool_ls.txt"
    gen think "$Q_THINK" 32 "--system=$SYS" --think
    gen toolls "$Q_TOOL" 48 "--system=$SYS" "--tools=$T" "--tool-names=ls" "--tool-response=build/q49/tool_ls.txt"
fi
mv "$tmp" "$G"
echo "written $G"

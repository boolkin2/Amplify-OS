#!/usr/bin/env bash
# Задать вопрос MIND на ВМ 402 и дождаться ответа (подэтап 49.1).
#   bash scripts/pve402_ask.sh "<аргументы pve402_keys.sh>" <n-й reply done> [confirm]
# Печатает строки [mind] из serial-лога узла, относящиеся к ответу.
cd "$(dirname "$0")/.."
KEYS="$1"; N="$2"
bash scripts/pve402_ssh.sh "bash /tmp/pve402_keys.sh $KEYS ret" >/dev/null
for i in $(seq 1 120); do
    c=$(bash scripts/pve402_ssh.sh "tr -d '\000' < /tmp/ai-serial.log | grep -ac 'reply done'" 2>/dev/null)
    if [ -n "${3:-}" ] && bash scripts/pve402_ssh.sh "tr -d '\000' < /tmp/ai-serial.log | grep -aq 'model asks to exec'" 2>/dev/null \
       && ! bash scripts/pve402_ssh.sh "tr -d '\000' < /tmp/ai-serial.log | grep -aq 'tool exec'" 2>/dev/null; then
        bash scripts/pve402_ssh.sh "bash /tmp/pve402_keys.sh ret" >/dev/null
    fi
    [ "${c:-0}" -ge "$N" ] && break
    sleep 5
done
bash scripts/pve402_ssh.sh "tr -d '\000' < /tmp/ai-serial.log | grep -aE 'mind\] (user|facts|reply done|reply:|model calls|model asks|tool )' | tail -12"

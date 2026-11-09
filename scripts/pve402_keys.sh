#!/usr/bin/env bash
# Отправка клавиш ВМ 402 (Proxmox, пользователь ai): аргументы — имена клавиш
# qm sendkey; "text:<строка>" — набрать строку (латиница, цифры, пробел,
# / ? . , -); "ru:<строка>" — кириллица клавишами ЙЦУКЕН (панель MIND в
# режиме RU, Ctrl+L). Кладётся в /tmp на узле:
#   scp -O scripts/pve402_keys.sh pve-node:/tmp/
export LC_ALL=C.UTF-8
declare -A RU=( [й]=q [ц]=w [у]=e [к]=r [е]=t [н]=y [г]=u [ш]=i [щ]=o [з]=p [х]=bracket_left
    [ъ]=bracket_right [ф]=a [ы]=s [в]=d [а]=f [п]=g [р]=h [о]=j [л]=k [д]=l [ж]=semicolon
    [э]=apostrophe [я]=z [ч]=x [с]=c [м]=v [и]=b [т]=n [ь]=m [б]=comma [ю]=dot [ё]=grave_accent )
send() { sudo qm sendkey 402 "$1"; sleep 0.12; }
latin() {
    case "$1" in
        ' ') echo spc;; '/') echo slash;; '?') echo shift-slash;; '.') echo dot;; ',') echo comma;;
        '-') echo minus;; [A-Z]) echo "shift-$(echo "$1" | tr 'A-Z' 'a-z')";; *) echo "$1";;
    esac
}
for a in "$@"; do
    if [ "${a#text:}" != "$a" ]; then
        s="${a#text:}"
        for ((i=0; i<${#s}; i++)); do send "$(latin "${s:i:1}")"; done
    elif [ "${a#ru:}" != "$a" ]; then
        s="${a#ru:}"
        for ((i=0; i<${#s}; i++)); do
            c="${s:i:1}"; lc="${c,,}"
            if [ -n "${RU[$lc]:-}" ]; then
                if [ "$c" != "$lc" ]; then send "shift-${RU[$lc]}"; else send "${RU[$lc]}"; fi
            else send "$(latin "$c")"; fi
        done
    else
        sudo qm sendkey 402 "$a"; sleep 0.3
    fi
done

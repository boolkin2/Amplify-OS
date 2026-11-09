#!/usr/bin/env bash
# ============================================================================
# AMPLIFY-OS — настройка TAP/bridge сети для QEMU (Этап 34+).
#
# Скрипт умеет работать в двух режимах:
#
#   1) bridge (по умолчанию)
#      Создаёт мост br0 на ХОСТЕ и подключает к нему физический интерфейс
#      (uplink). QEMU подключает свою TAP-карту к br0 через qemu-bridge-helper.
#      Гость попадает в L2-сегмент твоего LAN: получает IP по DHCP от роутера,
#      ARP/ICMP/TCP/UDP работают как у обычного ПК. Подходит для проводного
#      Ethernet. Wi-Fi так бриджить НЕЛЬЗЯ — большинство AP блокируют чужие
#      MAC через 802.11.
#
#   2) nat
#      Создаёт мост br0 БЕЗ физического uplink, выдаёт хосту IP 198.51.100.1/24
#      на br0, поднимает dnsmasq как DHCP-сервер на этом интерфейсе и
#      настраивает MASQUERADE через основной uplink. Подходит для Wi-Fi или
#      для случая, когда нет желания трогать физический интерфейс.
#
# Использование:
#   sudo bash tools/setup_tap.sh setup [bridge|nat] [uplink-iface]
#   sudo bash tools/setup_tap.sh teardown
#   sudo bash tools/setup_tap.sh status
#
# После первого setup можно запускать гостя без sudo:
#   make run-tap
# ============================================================================

set -euo pipefail

BRIDGE="${BRIDGE:-br0}"
NAT_NET="198.51.100.0/24"
NAT_HOST_IP="198.51.100.1"
NAT_DHCP_START="198.51.100.10"
NAT_DHCP_END="198.51.100.50"
QEMU_GROUP="${QEMU_GROUP:-kvm}"

GREEN=$'\e[32m'
YELLOW=$'\e[33m'
RED=$'\e[31m'
NC=$'\e[0m'

die()  { echo "${RED}error:${NC} $*" >&2; exit 1; }
log()  { echo "${GREEN}[setup-tap]${NC} $*"; }
warn() { echo "${YELLOW}[setup-tap]${NC} $*"; }

require_root() {
    [[ $EUID -eq 0 ]] || die "запусти под sudo: sudo bash $0 $*"
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "не найден: $1 (установи пакет)"
}

# Определяем дефолтный uplink (тот, через который идёт маршрут по умолчанию).
detect_uplink() {
    ip route show default | awk '/default/ {for (i=1;i<=NF;i++) if ($i=="dev") {print $(i+1); exit}}'
}

# Определяем тип NIC: wired/wireless/other.
nic_type() {
    local n="$1"
    if [[ -d "/sys/class/net/$n/wireless" ]] || [[ -e "/sys/class/net/$n/phy80211" ]]; then
        echo wireless
    elif [[ -e "/sys/class/net/$n" ]]; then
        echo wired
    else
        echo other
    fi
}

write_qemu_helper_conf() {
    install -d -m 0755 /etc/qemu
    if ! grep -qE "^allow $BRIDGE" /etc/qemu/bridge.conf 2>/dev/null; then
        echo "allow $BRIDGE" >> /etc/qemu/bridge.conf
        chmod 0644 /etc/qemu/bridge.conf
        log "разрешён bridge $BRIDGE в /etc/qemu/bridge.conf"
    fi

    # qemu-bridge-helper должен иметь CAP_NET_ADMIN, иначе он молча падает.
    local helper
    for h in /usr/lib/qemu/qemu-bridge-helper /usr/libexec/qemu-bridge-helper; do
        [[ -x "$h" ]] && helper="$h" && break
    done
    [[ -n "${helper:-}" ]] || die "qemu-bridge-helper не найден, поставь пакет qemu-system-common"

    if ! getcap "$helper" 2>/dev/null | grep -q "cap_net_admin"; then
        setcap cap_net_admin+ep "$helper"
        log "выдан CAP_NET_ADMIN: $helper"
    fi
}

cmd_setup_bridge() {
    local uplink="${1:-$(detect_uplink || true)}"
    [[ -n "$uplink" ]] || die "не удалось определить uplink, передай явно: setup bridge eth0"

    local typ
    typ=$(nic_type "$uplink")
    if [[ "$typ" == "wireless" ]]; then
        warn "$uplink — Wi-Fi. Большинство AP не пропустят чужой MAC через мост."
        warn "Используй режим nat:  sudo bash $0 setup nat"
        die "прерываю, чтобы не уронить тебе Wi-Fi"
    fi

    log "создаю мост $BRIDGE с uplink=$uplink"

    # Сохраняем IP/маршруты, чтобы перенести на мост.
    local ipcidr gw
    ipcidr=$(ip -4 -o addr show dev "$uplink" | awk '{print $4; exit}' || true)
    gw=$(ip -4 route show default dev "$uplink" | awk '/default/ {print $3; exit}' || true)

    ip link add name "$BRIDGE" type bridge 2>/dev/null || true
    ip link set "$BRIDGE" up
    # На время работы моста хост не должен «глотать» STP-фреймы — выключим STP.
    ip link set "$BRIDGE" type bridge stp_state 0 || true
    ip link set "$uplink" master "$BRIDGE"
    ip link set "$uplink" up

    if [[ -n "$ipcidr" ]]; then
        ip addr flush dev "$uplink"
        ip addr replace "$ipcidr" dev "$BRIDGE"
        if [[ -n "$gw" ]]; then
            ip route replace default via "$gw" dev "$BRIDGE" || true
        fi
        log "перенёс $ipcidr и шлюз $gw на $BRIDGE"
    fi

    write_qemu_helper_conf

    cat > /run/amplify-tap.state <<EOF
MODE=bridge
BRIDGE=$BRIDGE
UPLINK=$uplink
EOF
    log "готово. Запускай:  make run-tap"
}

cmd_setup_nat() {
    require_cmd dnsmasq
    require_cmd iptables

    local uplink="${1:-$(detect_uplink || true)}"
    [[ -n "$uplink" ]] || die "не удалось определить uplink, передай явно: setup nat wlan0"
    log "режим nat: br=$BRIDGE сеть=$NAT_NET uplink=$uplink"

    ip link add name "$BRIDGE" type bridge 2>/dev/null || true
    ip link set "$BRIDGE" up
    ip addr replace "$NAT_HOST_IP/24" dev "$BRIDGE"

    # Включаем форвардинг.
    sysctl -wq net.ipv4.ip_forward=1

    # NAT и форвардинг через uplink.
    iptables -t nat -C POSTROUTING -s "$NAT_NET" -o "$uplink" -j MASQUERADE 2>/dev/null || \
        iptables -t nat -A POSTROUTING -s "$NAT_NET" -o "$uplink" -j MASQUERADE
    iptables -C FORWARD -i "$BRIDGE" -o "$uplink" -j ACCEPT 2>/dev/null || \
        iptables -A FORWARD -i "$BRIDGE" -o "$uplink" -j ACCEPT
    iptables -C FORWARD -i "$uplink" -o "$BRIDGE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || \
        iptables -A FORWARD -i "$uplink" -o "$BRIDGE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT

    # ВХОДЯЩИЙ трафик с моста (DHCP/ICMP/DNS-к-хосту/etc).
    # Без этого правила «ping шлюза 198.51.100.1» молча теряется в INPUT
    # дефолтной политикой ufw/firewalld/iptables. Гость МОЖЕТ выйти наружу
    # через FORWARD, но НЕ может пингануть сам мост.
    iptables -C INPUT -i "$BRIDGE" -j ACCEPT 2>/dev/null || \
        iptables -I INPUT 1 -i "$BRIDGE" -j ACCEPT

    # Если активен ufw — добавим явное правило, чтобы он не сожрал нас при reload.
    if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "active"; then
        ufw allow in on "$BRIDGE" >/dev/null 2>&1 || true
        ufw route allow in on "$BRIDGE" out on "$uplink" >/dev/null 2>&1 || true
        log "ufw: разрешили входящий с $BRIDGE и форвардинг на $uplink"
    fi

    # DHCP-сервер для гостей.
    # ВАЖНО: --port=0 выключает в dnsmasq DNS-часть полностью. Нам нужен
    # ТОЛЬКО DHCP — иначе dnsmasq пытается забиндить :53 на всех адресах
    # и падает с «Address already in use», если на хосте уже крутится
    # systemd-resolved / wsl-resolver / какой-то другой DNS.
    # --listen-address и --bind-interfaces дополнительно гарантируют,
    # что DHCP-ответы пойдут только на нашем мосту.
    pkill -f "dnsmasq.*amplify-tap" 2>/dev/null || true
    rm -f /run/amplify-tap-dnsmasq.pid
    /usr/sbin/dnsmasq \
        --pid-file=/run/amplify-tap-dnsmasq.pid \
        --port=0 \
        --interface="$BRIDGE" \
        --bind-interfaces \
        --listen-address="$NAT_HOST_IP" \
        --dhcp-range="$NAT_DHCP_START,$NAT_DHCP_END,12h" \
        --dhcp-option=3,"$NAT_HOST_IP" \
        --dhcp-option=6,"$NAT_HOST_IP",8.8.8.8 \
        --log-dhcp \
        --conf-file=/dev/null
    log "dnsmasq запущен (DHCP $NAT_DHCP_START-$NAT_DHCP_END, шлюз $NAT_HOST_IP, DNS=8.8.8.8)"

    write_qemu_helper_conf

    cat > /run/amplify-tap.state <<EOF
MODE=nat
BRIDGE=$BRIDGE
UPLINK=$uplink
EOF
    log "готово. Запускай:  make run-tap"
}

cmd_setup() {
    require_root "$@"
    local mode="${1:-bridge}"
    shift || true
    # Если предыдущий setup упал на полпути — подчистим, чтобы не накладывать
    # правила и не плодить процессы dnsmasq поверх уже существующих.
    if [[ -f /run/amplify-tap.state ]] || ip link show "$BRIDGE" >/dev/null 2>&1; then
        warn "обнаружено предыдущее состояние — выполняю teardown перед setup"
        cmd_teardown_inner || true
    fi
    case "$mode" in
        bridge) cmd_setup_bridge "$@" ;;
        nat)    cmd_setup_nat    "$@" ;;
        *)      die "режим должен быть bridge или nat (получил: $mode)" ;;
    esac
}

# Внутренний teardown без require_root — для вызова из cmd_setup.
cmd_teardown_inner() {
    if [[ -f /run/amplify-tap.state ]]; then
        # shellcheck disable=SC1091
        source /run/amplify-tap.state
        if [[ "${MODE:-}" == "nat" ]]; then
            if [[ -f /run/amplify-tap-dnsmasq.pid ]]; then
                kill "$(cat /run/amplify-tap-dnsmasq.pid)" 2>/dev/null || true
                rm -f /run/amplify-tap-dnsmasq.pid
            fi
            pkill -f "dnsmasq.*amplify-tap" 2>/dev/null || true
            iptables -t nat -D POSTROUTING -s "$NAT_NET" -o "${UPLINK:-eth0}" -j MASQUERADE 2>/dev/null || true
            iptables -D FORWARD -i "$BRIDGE" -o "${UPLINK:-eth0}" -j ACCEPT 2>/dev/null || true
            iptables -D FORWARD -i "${UPLINK:-eth0}" -o "$BRIDGE" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
            iptables -D INPUT -i "$BRIDGE" -j ACCEPT 2>/dev/null || true
            if command -v ufw >/dev/null 2>&1; then
                ufw delete allow in on "$BRIDGE" >/dev/null 2>&1 || true
                ufw delete route allow in on "$BRIDGE" out on "${UPLINK:-eth0}" >/dev/null 2>&1 || true
            fi
        elif [[ "${MODE:-}" == "bridge" ]]; then
            local ipcidr gw
            ipcidr=$(ip -4 -o addr show dev "$BRIDGE" 2>/dev/null | awk '{print $4; exit}' || true)
            gw=$(ip -4 route show default 2>/dev/null | awk '/default/ {print $3; exit}' || true)
            ip link set "${UPLINK:-eth0}" nomaster 2>/dev/null || true
            if [[ -n "$ipcidr" ]] && [[ -e "/sys/class/net/${UPLINK:-eth0}" ]]; then
                ip addr flush dev "$BRIDGE" 2>/dev/null || true
                ip addr replace "$ipcidr" dev "${UPLINK:-eth0}" 2>/dev/null || true
                [[ -n "$gw" ]] && ip route replace default via "$gw" dev "${UPLINK:-eth0}" 2>/dev/null || true
            fi
        fi
    fi
    ip link set "$BRIDGE" down 2>/dev/null || true
    ip link delete "$BRIDGE" type bridge 2>/dev/null || true
    rm -f /run/amplify-tap.state
}

cmd_teardown() {
    require_root "$@"
    cmd_teardown_inner
    log "разобрано"
}

cmd_status() {
    if [[ -f /run/amplify-tap.state ]]; then
        cat /run/amplify-tap.state
        echo "---"
    else
        echo "состояние: не настроено"
    fi
    ip -br addr show "$BRIDGE" 2>/dev/null || true
    bridge link show 2>/dev/null | grep -E "master $BRIDGE" || true
}

case "${1:-}" in
    setup)    shift; cmd_setup "$@" ;;
    teardown) shift || true; cmd_teardown "$@" ;;
    status)   cmd_status ;;
    *)
        cat <<EOF
AMPLIFY-OS — TAP/bridge сеть для QEMU.

  sudo bash $0 setup bridge [iface]   — мост на физический NIC (LAN)
  sudo bash $0 setup nat    [iface]   — TAP+NAT через uplink (Wi-Fi OK)
  sudo bash $0 status                 — показать состояние
  sudo bash $0 teardown               — разобрать и вернуть как было

После setup-а: make run-tap   (без sudo)
EOF
        ;;
esac

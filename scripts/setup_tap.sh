#!/usr/bin/env bash
# ============================================================================
# setup_tap.sh — настройка TAP-интерфейса и NAT для QEMU в WSL2.
#
# Создаёт устройство tap0 (10.10.0.1/24), включает IP forwarding и
# добавляет MASQUERADE через eth0 — гость QEMU получает реальный выход
# в интернет (через двойной NAT: tap0 → eth0 WSL → Windows host).
#
# Запуск (один раз после ребута WSL):
#   sudo bash scripts/setup_tap.sh
#
# Параметры (env):
#   TAP_NAME    имя tap-устройства (по умолчанию tap0)
#   TAP_NET     сеть для гостя/хоста (10.10.0.0/24)
#   TAP_HOST_IP IP хоста в этой сети (10.10.0.1/24)
#   WAN_IF      внешний интерфейс WSL (eth0)
#   USER_NAME   владелец tap-устройства (по умолчанию SUDO_USER)
# ============================================================================
set -e

TAP_NAME=${TAP_NAME:-tap0}
TAP_NET=${TAP_NET:-10.10.0.0/24}
TAP_HOST_IP=${TAP_HOST_IP:-10.10.0.1/24}
WAN_IF=${WAN_IF:-eth0}
USER_NAME=${USER_NAME:-${SUDO_USER:-$(logname 2>/dev/null || echo root)}}

if [ "$EUID" -ne 0 ]; then
  echo "ERROR: запусти через sudo: sudo bash $0" >&2
  exit 1
fi

if ! id "$USER_NAME" >/dev/null 2>&1; then
  echo "ERROR: пользователь '$USER_NAME' не найден" >&2
  exit 1
fi

if ! ip link show "$WAN_IF" >/dev/null 2>&1; then
  echo "ERROR: внешний интерфейс '$WAN_IF' не найден" >&2
  exit 1
fi

echo "[setup_tap] tap=$TAP_NAME net=$TAP_NET host_ip=$TAP_HOST_IP wan=$WAN_IF user=$USER_NAME"

# 1) tun module (в WSL2 встроен в ядро, но modprobe безопасен)
modprobe tun 2>/dev/null || true

# 2) Создаём TAP, если ещё нет
if ! ip link show "$TAP_NAME" >/dev/null 2>&1; then
  ip tuntap add dev "$TAP_NAME" mode tap user "$USER_NAME"
  echo "[setup_tap] created $TAP_NAME (owner: $USER_NAME)"
fi
ip addr flush dev "$TAP_NAME"
ip addr add "$TAP_HOST_IP" dev "$TAP_NAME"
ip link set "$TAP_NAME" up

# 3) IP forwarding
sysctl -w net.ipv4.ip_forward=1 >/dev/null

# 4) nftables: NAT-таблица + правило MASQUERADE
#    Сначала сносим прежнюю таблицу, чтобы скрипт был идемпотентным.
nft delete table ip qemu_nat 2>/dev/null || true
nft add table ip qemu_nat
nft "add chain ip qemu_nat postrouting { type nat hook postrouting priority 100; }"
nft "add rule ip qemu_nat postrouting ip saddr $TAP_NET oif \"$WAN_IF\" masquerade"

# 5) FORWARD-разрешение (на случай дефолт-DROP в системе)
nft delete table ip qemu_filter 2>/dev/null || true
nft add table ip qemu_filter
nft "add chain ip qemu_filter forward { type filter hook forward priority 0; policy accept; }"
nft "add rule ip qemu_filter forward iif \"$TAP_NAME\" oif \"$WAN_IF\" accept"
nft "add rule ip qemu_filter forward iif \"$WAN_IF\" oif \"$TAP_NAME\" ct state established,related accept"

echo "[setup_tap] OK"
echo
ip -4 addr show "$TAP_NAME"
echo
echo "Теперь в гостевой ОС задай через 'Сетевые настройки':"
echo "  IP:    10.10.0.2"
echo "  Маска: 255.255.255.0"
echo "  Шлюз:  10.10.0.1"
echo "  DNS:   1.1.1.1"
echo "и запусти:  make run-tap"

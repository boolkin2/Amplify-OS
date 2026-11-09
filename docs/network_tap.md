# AMPLIFY-OS — TAP / bridge сеть (реальный выход в интернет)

По умолчанию `make run` использует **slirp** (user-mode networking QEMU): гостю
отдаётся фейковая сеть `10.0.2.0/24`, NAT-наружу через хост. Этого хватает для
HTTP/HTTPS из браузера, но:

- DHCP отдаёт «фейковый» IP (всегда `10.0.2.15`), а не реальный из твоего LAN;
- ICMP к произвольным хостам ограничен;
- ARP/L2-протоколы не работают;
- хост не может обращаться к гостю по IP без port-forwarding.

`make run-tap` делает по-настоящему: подключает гостевую RTL8139 к мосту `br0`
на хосте через **qemu-bridge-helper**. Гость попадает в реальный L2-сегмент,
получает IP по DHCP от роутера/dnsmasq, без хардкода.

---

## Два режима

### 1. bridge (рекомендуется для проводного Ethernet)

Хостовый `eth0` (или другой uplink) переезжает в мост `br0`. Гость живёт в той
же подсети, что и хост, IP получает от того же роутера.

```bash
sudo bash tools/setup_tap.sh setup bridge          # автодетект uplink
sudo bash tools/setup_tap.sh setup bridge eth0     # явно
```

**Не работает по Wi-Fi** — большинство точек доступа блокируют чужие MAC через
802.11. Скрипт это проверяет и отказывается бриджить wlan*-интерфейс.

### 2. nat (Wi-Fi и любой случай, когда не хочется трогать физический NIC)

Создаётся `br0` без uplink, на нём поднимается сеть `198.51.100.0/24` и
**dnsmasq** как DHCP-сервер. Через `iptables MASQUERADE` трафик гостя уходит
в интернет через основной uplink хоста.

```bash
sudo bash tools/setup_tap.sh setup nat             # автодетект uplink
sudo bash tools/setup_tap.sh setup nat wlan0       # явно
```

Гость получит IP из диапазона `198.51.100.10–50`, шлюз `198.51.100.1`,
DNS `198.51.100.1` + `8.8.8.8`.

Зависимости: `dnsmasq`, `iptables`. На Ubuntu/Debian:
```bash
sudo apt install dnsmasq iptables
```

---

## Запуск

После однократного `setup`:

```bash
make run-tap          # без sudo
```

QEMU видит `/etc/qemu/bridge.conf` с `allow br0`, запускает
`qemu-bridge-helper` (который имеет `CAP_NET_ADMIN`) и подключает свою TAP-карту
к `br0`. Гостевая ОС стартует обычным образом, DHCP-клиент в `kernel/net/dhcp.c`
получает реальный IP, ping и браузер начинают работать наружу.

Дамп трафика моста сохраняется в `build/eth0-tap.pcap` (открой Wireshark'ом).

## Проверка

```bash
make status-tap       # увидишь br0 + uplink/dnsmasq
```

Внутри гостя:

1. Залогинься (`mark`/`test1234`).
2. Открой приложение **«Сеть»**, нажми **Ping шлюз** — должны идти ответы (если
   bridge-режим — от твоего реального роутера; если nat — от хоста
   `198.51.100.1`).
3. Открой **«Браузер»**, набери `http://example.com/` или `http://1.1.1.1/`.

## Откат

```bash
sudo bash tools/setup_tap.sh teardown
```

В bridge-режиме — возвращает IP/маршрут с `br0` обратно на физический NIC.
В nat-режиме — глушит dnsmasq и убирает iptables-правила.

## Замечания

- Скрипт сохраняет состояние в `/run/amplify-tap.state` — оно живёт до
  ребута. После ребута `setup` нужно повторить (или добавить в systemd).
- Если у тебя VPN, корпоративный proxy или sandbox — bridge может не пройти,
  тогда используй nat-режим.
- Для контейнеризованных Devin-ов хост-нетворкинг ограничен; этот скрипт
  предназначен для запуска на твоей рабочей машине, а не внутри Devin VM.

## WSL2

Если ты запускаешь это из WSL2 (Ubuntu внутри Windows):

- **bridge-режим работать не будет**: WSL2 живёт в Hyper-V VM и не имеет
  прямого доступа к физическому Ethernet хоста. Используй **только nat**.
- В WSL2 на адресе `10.255.255.254:53` слушает встроенный WSL DNS-resolver
  (отсюда могла прилететь ошибка `Address already in use`). Скрипт
  запускает dnsmasq с `--port=0` — это **выключает DNS-часть** dnsmasq и
  оставляет только DHCP. Конфликт по :53 исключён.

## Известные проблемы

**`dnsmasq: failed to create listening socket for X.X.X.X: Address already in use`**

Это значит, что порт 53 (DNS) уже занят `systemd-resolved` или WSL-resolver.
Скрипт уже запускает dnsmasq с `--port=0` — обнови `tools/setup_tap.sh` до
последней версии и повтори:

```bash
sudo bash tools/setup_tap.sh teardown
sudo bash tools/setup_tap.sh setup nat
```

**Ping шлюза 198.51.100.1 — `0/4 ответов`** (nat-режим)

Хостовый firewall (ufw/iptables INPUT с дефолтной политикой DROP)
проглатывает ICMP, прилетающий на мост. Скрипт явно открывает INPUT для
интерфейса `br0` и (если активен ufw) добавляет соответствующее правило.
Если у тебя версия скрипта от первого setup'а — повтори:

```bash
sudo bash tools/setup_tap.sh teardown
sudo bash tools/setup_tap.sh setup nat
```

Проверка вручную:

```bash
sudo iptables -L INPUT -nv | grep br0       # должна быть ACCEPT-строка
sudo tcpdump -ni br0 icmp                   # запусти и пингани из гостя
sysctl net.ipv4.icmp_echo_ignore_all        # должно быть 0
```

Если в `tcpdump` видно `request` от `198.51.100.x` к `198.51.100.1`, но нет
`reply` — значит, INPUT всё-таки режется кем-то ещё (firewalld / nftables /
docker-iptables). Проверь:

```bash
sudo nft list ruleset 2>/dev/null | head
sudo iptables-save | grep -E "INPUT|FORWARD" | head
```

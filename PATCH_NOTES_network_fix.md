# Исправление сети (браузер не открывает сайты на VMware Workstation)

Первопричина: сетевой стек был зашит под QEMU SLIRP. На VMware это ломало DNS
и приводило к подстановке нерабочего статического IP.

## Изменённые файлы
1. kernel/arch/x86_64/dns.c
   - Убран захардкоженный DNS 10.0.2.3 (QEMU). Порядок выбора DNS теперь:
     выданный DHCP -> шлюз -> 8.8.8.8.
2. kernel/arch/x86_64/dhcp.c + dhcp.h
   - XID транзакции теперь случайный (rdtsc ^ MAC) вместо константы 0x11223344.
   - Добавлен dhcp_is_selecting() для безопасного повтора DISCOVER.
3. kernel/kernel64_main.c (64-битное ядро — основной браузерный путь)
   - Окно ожидания DHCP увеличено до ~5 c, DISCOVER повторяется ~раз в 1.5 c.
   - Убран фейковый fallback на 10.0.2.15 / gw 10.0.2.2.
4. kernel/kernel.c (legacy 32-бит)
   - Убран тот же фейковый fallback 10.0.2.15.

---

# Этап 48.1 — полный выход в интернет из встроенного браузера

Подробное описание — в `CLAUDE.md`, раздел §24.  Кратко по файлам:

| Файл | Изменения |
|------|-----------|
| `kernel/arch/x86_64/dns.{c,h}` | Список DNS-серверов (DHCP1 -> DHCP2 -> шлюз -> 8.8.8.8 -> 1.1.1.1 -> 8.8.4.4), 2 попытки по 1.5 с на каждый, кэш 32 имени с TTL, случайные src-порт и query ID, пропуск CNAME, IP-литералы без запроса, `dns_set_server2()`, `dns_cache_flush()` |
| `kernel/arch/x86_64/tcp.c` | RX кольцевой буфер 64 KB, реальное окно + window-update ACK, MSS 1460 в SYN, случайные ISN/ephemeral-порт, ретрансмит SYN (3x1.2 с) и данных (3x600 мс), отброс дубликатов и dup-ACK |
| `kernel/arch/x86_64/http.{c,h}` | HTTP/1.1 с нормальными заголовками, chunked (`http_dechunk`), 128 KB буфер, ранний выход по Content-Length, `http_get_ex`, `http_get_follow`, `http_find_header`, `http_resolve_url`, `http_url_is_https` |
| `kernel/arch/x86_64/https.{c,h}` | HTTP/1.1 поверх TLS 1.2, chunked, 128 KB буфер, редиректы через общие URL-утилиты + новая `net_fetch()` (http/https, автосхема, кросс-схемные редиректы до 10 шагов) |
| `kernel/arch/x86_64/dhcp.{c,h}` | Из опции 6 берём два DNS-сервера, геттер `dhcp_get_dns2()` |
| `kernel/arch/x86_64/apps.c` | Новый браузер: загрузка через `net_fetch()`, HTML->текст (без script/style), декод HTML-сущностей, UTF-8 -> CP1251, кликабельные ссылки, история + «Назад», переключатель «HTML», перенос по словам, кламп прокрутки |
| `kernel/kernel64_main.c` | После `dns_init()` ставим оба DNS: `dns_set_server()` + `dns_set_server2()` |

## Как проверить

```bash
make ARCH=x86_64
make run                      # QEMU slirp — интернет сразу
# или реальные пакеты через хост:
sudo tools/setup_tap.sh setup nat && make run-tap
```

В СИСТЕМЕ: логин `mark` / `test1234` -> Browser -> адрес -> Enter.
Проверочные адреса: `example.com`, `http://neverssl.com/`,
`https://ru.wikipedia.org/wiki/Linux`, `https://lite.duckduckgo.com/lite/?q=notion`.
Серийный лог: `[dns] ... [tcp] ... [https] ... [net] redirect ...`.

## Ограничения (после 48.1)

- Только TLS 1.2 (нет TLS 1.3), нет gzip/brotli, нет CSS/JS/картинок.
- Одно TCP-соединение за раз, без keep-alive; только IPv4; без POST/cookies.
- JS-ориентированные SPA-сайты покажут пустую страницу — нужны lite-версии.

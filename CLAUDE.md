# AMPLIFY-OS — памятка для ИИ-агента

Своя ОС с нуля (не Linux): x86_64 Long Mode, загрузка GRUB2 Multiboot2 или
UEFI (свой загрузчик). Процедуры работы (сборка, QEMU, headless-тесты,
новый модуль/экран/команда) — в скилле `.claude/skills/amplify-os/SKILL.md`.
Полная история этапов и хотфиксов: `docs/archive/CLAUDE_full_2026-09-26.md`
(читать только при разборе старых решений).

## 1. Правила работы
- **Строго поэтапно.** Закончил этап → тесты → обновил этот файл (§6, §7) →
  отчёт → вопрос «Этап N завершён. Всё работает? Переходим к N+1?».
  Без подтверждения пользователя следующий этап НЕ начинать.
- Отвечать пользователю по-русски. Комментарии в коде — по-русски,
  подробно, объясняют ПОЧЕМУ, а не только ЧТО.
- Не хардкодить факты — сверять с кодом. Утверждать «работает» только
  после прогона теста; провалы называть прямо.
- **Где работать (с 2026-10-01):** тяжёлое — только на Proxmox (ВМ 402 под
  KVM). ПК пользователя — сборка в WSL и минимальные проверки (smoke за
  1–2 мин); многочасовые TCG-прогоны на нём не запускать. Беречь токены:
  без циклов опроса и длинных ожиданий, короткие команды, вывод — через grep.
- Стиль ОС не упрощать: бренд AMPLIFY-OS (CHRT-OS), треугольный логотип,
  «мистические» имена команд и папок.

## 2. Окружение (Windows + WSL Debian)
- Проект: `D:\OS2` = `/mnt/d/OS2`. Всё собирается и запускается в WSL:
  `wsl -e bash -lc "cd /mnt/d/OS2 && make -f Makefile.x86_64 -j8 all"`.
- Git Bash портит аргументы, начинающиеся с `/` (`/usr/sbin/x` →
  `C:/Program Files/Git/...`): команду для wsl начинать с `cd ...`.
- Heredoc в инструменте Bash съедает `\\` → в C-коде появляются живые
  переводы строк. Код с `\n`, `\x..` писать через Write/Edit или скрипт-файл.
- `/tmp` в WSL — tmpfs 3.8 ГБ, очищается при остановке дистрибутива:
  логи тестов копировать в `build/`.
- В WSL нет python3; есть у Windows (`python`, с PIL, numpy, torch, jinja2,
  tokenizers). Из WSL `python.exe` НЕ запускается (алиас WindowsApps) — всё,
  что считает эталон, запускать из Git Bash (`scripts/gen_qwen3_golden.sh`). `/dev/kvm`
  есть, но он `root:kvm 660`, а пользователь WSL не в группе `kvm` → QEMU
  идёт под TCG (в разы медленнее; тесты умножают таймауты). Включить KVM может
  только пользователь: `sudo usermod -aG kvm $USER` + `wsl --shutdown`.
  Также: OVMF, xorriso, grub-pc-bin (только BIOS), mtools, dosfstools,
  openssl, libssl-dev, libcryptsetup.so.12 (без CLI cryptsetup).
- Интернет — только у Windows (в WSL нет). CDN Hugging Face недоступен;
  модели качать с ModelScope (`curl.exe --doh-url https://1.1.1.1/dns-query`)
  и сверять sha256 с API huggingface.co (оно отвечает).
- Репозиторий НЕ под git — бэкапить перед крупными удалениями.
- Тест на Proxmox: ВМ 402 «Amply-VMTest» на узле pve-node (UI pve-ui.example.local,
  user@pve). **SSH:** алиасы в `~/.ssh/config` Windows — `pve-gw` (шлюз, только
  проброс) и `pve-node` (192.0.2.10 через ProxyJump), пользователь `ai`,
  ключ в ssh-agent; звать `ssh.exe -o BatchMode=yes pve-node "..."` из
  PowerShell. sudo без пароля только на `qm status|config|start|stop|shutdown|
  set|sendkey|monitor|terminal 402`; `/mnt/pve/data/template/iso` доступен на
  запись (ISO — `scp.exe` туда). Скриншот: `printf 'screendump /tmp/ai-402.ppm\nquit\n'
  | sudo qm monitor 402` — файл получается root:root 0640, прочитать его `ai`
  пока не может (нужно доп. правило sudo, напр. `/usr/bin/cat /tmp/ai-402.ppm`).
  Serial — `sudo qm terminal 402` (видно только вывод после подключения,
  подключать ПОСЛЕ старта ВМ; в фоне: `nohup script -qfc 'sudo qm terminal
  402' /tmp/ai-serial.log &`). Скриншот читается правилом sudo `/usr/bin/cat
  /tmp/ai-402.ppm` — готово в `scripts/pve402_shot.ps1`; клавиши —
  `scripts/pve402_keys.sh` (кладётся в /tmp узла; `text:` латиница, `ru:`
  ЙЦУКЕН), вопрос MIND с ожиданием ответа — `scripts/pve402_ask.sh`.
  Грабли: `scp` только с `-O` (SFTP для ai закрыт); шлюз иногда не отвечает
  (timeout/DNS) — `scripts/pve402_ssh.sh` с повторами, но удалённые команды
  держать короткими (обрыв во время `sleep` = повтор всей команды);
  `pkill -f` через ssh находит собственную командную строку — шаблон `[q]m…`;
  `qm set --boot` действует только после stop/start ВМ, не после reboot гостя;
  смена `--ide2` в том же вызове выкидывает ide2 в конец порядка загрузки —
  `--boot` задавать ОТДЕЛЬНЫМ вызовом после смены ISO.
  Конфиг с 2026-10-01 (подэтап 41.2): OVMF, i440fx, 4 ядра (host = Xeon
  E5-2670 v2: SSSE3/AVX, НЕТ AVX2), 8 ГБ RAM, `vga: virtio`, tablet=0,
  serial0=socket, `args: -device virtio-keyboard-pci -device virtio-tablet-pci`,
  `net0: virtio=BC:24:11:00:00:01,bridge=vmbr2`, `virtio1` =
  `vm-402-disk-6.raw` 12 ГБ — ТЕСТОВАЯ система (учётная запись `tester`,
  пароль — `USER_PASS` из `scripts/test_stage57_installer.sh`, Qwen3 +
  SmolLM2, без FDE; на её ESP — ядро 41.2). С 48.2 ВМ грузит ядро с CD:
  `boot: order=ide2;virtio1;net0`.
  **С 2026-10-06 (50.5) в приводе — ПОЛНЫЙ установочный ISO**
  `amplify-qwen3-v4.iso` (2 106 312 704 байта, sha256 4cb2b1e42de8a3ccc1ddd1ed
  37dc1c72b3447c3e2a1279f6a219d37b3eaaee0f, рядом `.sha256`): ядро 50.5,
  установщик, Qwen3-1.7B и SmolLM2 в `/mind`, образы гостей TinyCore и
  Alpine в `/guests` — пользователь просил «полный ISO вместо основного, со
  всеми новшествами и ИИ». ВМ с него грузится в установщик (live): Esc на
  первом экране — живая система поверх тестового диска (учётная запись
  `tester`), либо установка заново. Заливка: `scripts/pve402_full_iso.sh
  upload <имя.iso>` (копия `build/myos-x86_64.iso` узла в хранилище, ВМ не
  трогает) и `… switch <запуск> <имя.iso>` (stop/start). Прежние образы в
  хранилище не тронуты: `amplify-qwen3.iso`, `-v2`, `-v3` (по 2 ГБ; удалить
  может только пользователь), `amplify-dev.iso` (ядро 50.1). Вернуть быструю
  заливку ядра без установщика — `scripts/pve402_deploy.sh <имя>` (dev-iso:
  ядро с CD, система с диска).
  Сеть vmbr2: адреса у узла на мосту нет, порты — ВМ 402 и `net1` ВМ 403
  «Test-RTR» (ALT Linux, ens19 = 10.0.2.2/24; аплинк ens18 = 198.51.100.72 по
  DHCP, шлюз 198.51.100.1). DHCP для 402: 10.0.2.10/24, шлюз 10.0.2.2, DNS
  8.8.8.8, аренда 600 с (сервер называет себя 10.0.2.1). **Маршрут в интернет
  у ВМ 402 есть с 2026-10-02:** на роутере была включена пересылка, но не было
  NAT — добавлено `table ip nat { chain postrouting { type nat hook
  postrouting priority srcnat; oifname "ens18" masquerade } }`, сохранено в
  `/etc/nftables/nftables.nft` (копия `.bak`), `net.ipv4.ip_forward = 1` — в
  `/etc/net/sysctl.conf`. **Веб с ВМ 402 работает с 2026-10-03** (ya.ru,
  google.com — рукопожатия TLS с проверенным сертификатом, уже не через slirp).
  До этого мешал **zapret (nfqws) на шлюзе R1** (OpenWrt,
  198.51.100.1): он перехватывает первые пакеты каждого соединения LAN на
  порты 80/443, узнаёт HTTP-запрос и TLS ClientHello и подменяет их своей
  «десинхронизацией» — и для хоста 198.51.100.72 ответы после этого не
  приходили вовсе. Приметы (стоит помнить): ping, DNS и
  `http://portquiz.net:8080/` работают; обычный HTTP на порт 443 доходит; а
  GET на порт 80 и ClientHello на 443 остаются без ACK (`[tcp] data
  retransmit` x3) — причём точно так же у `curl` на самом Test-RTR; на шлюзе
  в tcpdump видно, что сегмент пришёл и повторяется, а ACK нет. Нераспознанное
  zapret не трогает — поэтому 02.10 утром Википедия открылась «случайно»:
  ClientHello тогда уходил разрезанным. Решение (03.10, сделал пользователь):
  zapret на R1 оставлен, а 198.51.100.72 исключён из его обработки — Zapret
  Manager, `zms` → `m` (Системное меню) → `i` (Меню исключений IP из Zapret);
  пользователь подтвердил, что после этого всё работает. Сайты «опять не
  открываются» на ВМ 402 — первым делом проверить, что исключение на месте
  (`curl --head -m 8 portquiz.net` с Test-RTR должен дать 200).
  Узел ходит через vlan10 (192.0.2.x) того же R1 — там, судя по тому, что
  сайты открывались, zapret не действовал.
  Консоль 403 — только noVNC в веб-интерфейсе (Chrome
  пользователя, расширение Claude): Shift и Ctrl через неё НЕ проходят («_»
  выходит как «-», «{» как «[»), длинные строки теряют символы — команды
  короткие, спецсимволы через `` `printf '\173'` `` и `eval`. Менять что-либо
  на 403 — только с явного «да» пользователя.
  Диск ПОЛЬЗОВАТЕЛЯ (сборка 49.1, учётная запись `mark`, пароль ИИ не
  известен) отсоединён и НЕ изменён: `unused5: data:402/vm-402-disk-4.qcow2`;
  вернуть: `qm set 402 --sata0 data:402/vm-402-disk-4.qcow2`, затем
  `--delete virtio1` (диск станет unusedN) и `--boot 'order=sata0;ide2;net0'`,
  stop/start. `unused6` (`vm-402-disk-5.raw`) — первая тестовая установка на
  SATA. НИКОГДА не делать `--delete unusedN`: это уничтожает том.
  Заливка нового ядра без переустановки: `make dev-iso` → в хранилище как
  `amplify-dev.iso` → `--ide2 …amplify-dev.iso`, отдельным вызовом
  `--boot 'order=ide2;virtio1;net0'`, stop/start (ядро с CD, система с диска).
  Замер на ВМ: `/tmp/pve402_bench.sh` (копия `scripts/pve402_bench.sh`).
  Узел (2026-10-01: 40 потоков, 125 ГБ RAM, QEMU 11.0.3, gcc 14.2 — как в WSL):
  root у `ai` НЕТ; в группе kvm — есть (с 2026-10-01), свой QEMU на узле идёт
  под KVM. Сборка на узле работает без root: nasm,
  xorriso (+libisoburn/libisofs/libburn/libjte) и gnu-efi распакованы в
  `~/local` (`apt download` + `dpkg -x`), окружение — `. ~/local/env.sh`
  (PATH, LD_LIBRARY_PATH, MAKEFLAGS с EFI_INC/EFI_LIB, `$AOS`). Рабочий
  каталог — `/tmp/ai-work/OS2` (tmpfs 63 ГБ, пропадает при перезагрузке узла;
  постоянный каталог на `/mnt/pve/data` может создать только root). OVMF —
  `/usr/share/pve-edk2-firmware/OVMF_CODE.fd`. Модели на узле берутся из ISO:
  `xorriso -osirrox on -indev <iso> -extract /mind build/mind`.
  Скрипты (Git Bash): `scripts/pve_sync.sh` (исходники: tar | ssh; для /tmp —
  `PVE_WORK=/tmp/ai-work`), `scripts/pve_run.sh <файл.sh>` (выполнить скрипт на
  узле через stdin — кавычки и `$var` целы; при обрыве шлюза скрипт
  ПОВТОРЯЕТСЯ с начала → опасные шаги закрывать файлом-маркером),
  **образы гостей phantom** — `build/guests/` (в дереве не лежат и `pve_sync`
  их не возит): `CorePure64-17.1.iso` (TinyCore, tinycorelinux.net/17.x/
  x86_64/release, 26 294 272 байта, sha256 5720c5938b33874db1f5b37b296192fa
  33d8366bef7965920591bd4d04826b10; ядро 6.18.35, конфиг — рядом). На узел:
  `scp.exe -O build/guests/CorePure64-17.1.iso pve-node:/tmp/ai-work/OS2/
  build/guests/` — после перезагрузки узла (tmpfs) копировать заново, без
  образа тесты 50.2/50.3 пишут SKIP. Второй образ —
  `alpine-virt-3.24.2-x86_64.iso` (dl-cdn.alpinelinux.org/alpine/
  latest-stable/releases/x86_64, 69 206 016 байт, sha256 3ab424762af704b2c2a9
  e57df1dc37f982af260071504d977f2fb96822e7130b; ядро 6.18.52-0-virt, syslinux
  6.04) — так же копировать на узел, без него тест 50.4 пишет SKIP.
  Пользователь разрешил качать образы гостей (05.10).
  **Инструменты WebAssembly на узле (этап 51, разрешено 07.10):**
  `scripts/pve_wasm_tools.sh` (через `pve_run.sh`; повторять можно) —
  `apt download` + `dpkg -x` в `~/local` (317 МБ, переживает перезагрузку):
  clang 19.1.7 (`~/local/usr/lib/llvm-19/bin/clang --target=wasm32-wasi
  --sysroot=$HOME/local/usr`), wasm-ld, wasi-libc, wabt 1.0.36 (`wat2wasm`,
  `wast2json`, `wasm-objdump` — в PATH после `. ~/local/env.sh`). Набор
  тестов — `~/local/share/wasm-spec/*.wast` (WebAssembly/spec, тег `wg-2.0`,
  test/core: 90 файлов + 58 simd, которые нам не нужны). На ПК ничего из
  этого нет: в WSL нет интернета, в Windows — только Node.js.
  `scripts/pve_regression.sh` (на узле, через nohup; итог —
  `build/reg/summary.txt`), `scripts/pve_test.sh <имя> <тест> [clean]` (sync +
  сборка + один тест на узле в фоне; `<тест>` = `pve_regression` — вся
  регрессия; `--log <имя>` — показать лог), `scripts/pve_wait.sh <имя>` (ждать
  конца НА УЗЛЕ одним ssh — запускать в фоне, с ПК не опрашивать),
  `scripts/pve402_deploy.sh <имя>` (dev-iso → ВМ 402, stop/start, serial в
  `/tmp/ai-serial.log`). PowerShell → ssh.exe съедает `$var`, двойные
  кавычки и скобки в удалённой команде — сложное только через `pve_run.sh`.

## 3. Сборка и запуск
```
make ARCH=x86_64                 # kernel + initrd + ISO + esp.img + disk
make ARCH=x86_64 run             # UEFI, dev-ESP без live.flag → сразу в систему
make ARCH=x86_64 iso             # build/myos-x86_64.iso (BIOS+UEFI+USB-гибрид)
make ARCH=x86_64 run-installer   # ISO + пустой build/target-disk.qcow2
make ARCH=x86_64 run-installed   # загрузка установленного диска
make ARCH=x86_64 installer-disk-reset
make ARCH=x86_64 mind-model      # build/mind/*.gguf -> dev-диск /vault/mind
```
После изменения структуры/#define в заголовке — `rm -rf build/x86_64/kernel`
(зависимости по mtime не отслеживаются, остаются устаревшие .o).

## 4. Архитектура (где что лежит)
Всё ядро — `kernel/arch/x86_64/`, точка входа `kernel/kernel64_main.c`.
- **Загрузка:** `boot.asm` — `_start` (GRUB, 32→64) и `_efi_start` (UEFI).
  `uefi/main.c` — наш BOOTX64.EFI: читает kernel.bin/initrd.img с ESP, строит
  синтетический Multiboot2-info (basic_meminfo ОБЯЗАН быть первым тэгом —
  его патчат по смещению info+8), ExitBootServices. `\live.flag` на ESP →
  cmdline `live installer`.
- **Порядок kernel_main64:** serial/fb → GDT/IDT/PIC/PIT → paging/pmm/heap/vm
  → vfs/initrd → installer_init_from_mb2 → ata_init → fat32_mount → fde_scan
  → users_init → audio/cpu/acpi/apic/pci/net/smp/syscall → keyboard/mouse →
  doublebuf/drm/compositor → **экран разблокировки FDE** → ns/cgroup/seccomp
  → amply + smoke-блоки (… 48, 49 MIND, 50 VT-x) → crypto_init → splash → installer (live) →
  firstboot → login (единый вход после FDE) → wm/desktop → wm_main_loop.
  Всё после `wm_main_loop()` — мёртвый код.
- **Диски:** `ata.c` (8 слотов: 0..3 legacy IDE PIO, 4..7 порты SATA AHCI из
  `ahci.c` — DMA через bounce 64 КБ; ATAPI READ(10) для CD на обоих) →
  `iso9660.c` (чтение установочного носителя, Rock Ridge) → `disk_io.c`
  (единые номера: 0..7 ATA/AHCI, 8..15 blkcrypt, 16 RAM-диск) → `blkcrypt.c`
  (dm-crypt-аналог, aes-xts-plain64) / `luks.c` (LUKS1 как у cryptsetup) →
  `partition.c` (GPT: ESP / Basic Data / Linux LUKS) → `fat32.c`
  (том = dev+base_lba; `fat32_bind`, `fat32_mount_volume`, save/restore) +
  `fat32_mkfs.c`. `fde.c` — политика системного тома.
- **Установщик:** `installer.c` (UI, 6 экранов по макету) +
  `installer_engine.c` (GPT → mkfs → LUKS → копирование payload из initrd →
  users.txt/system.conf → модели MIND с носителя `/mind/*.gguf` → `/vault/mind`
  ПОТОКОВО (`fat32_stream_*`: серии кластеров, FAT посекторно, куски 4 МБ,
  сверка 64-бит суммой повторным чтением); модель, которой не хватит RAM
  машины, пропускается). Payload (kernel, BOOTX64.EFI) кладётся в initrd
  Makefile'ом (`INITRD_FILES`); модели — на ISO (`MIND_ISO_MODELS` = всё из
  `build/mind`, кроме stories260K, жёсткими ссылками), ISO с Qwen3 ~2.0 ГБ.
- **Память:** `pmm.c` — bitmap для нижних 4 ГБ (таблицы, DMA, всё мелкое) +
  с 49.1 экстенты RAM ВЫШЕ 4 ГБ для больших блоков (`pmm_alloc_big`: веса и
  KV MIND); `paging_map_high_identity` дописывает PDPT[4..] общего PML4[0]
  2-МБ страницами. `pmm_total_ram/pmm_free_ram` — вся RAM. UEFI-загрузчик
  кладёт инфоблок MB2 строго ниже 4 ГБ (`uefi_low_alloc`).
- **Графика/UI (Pearl):** `graphics.c`, `doublebuf.c`, `compositor.c`, `wm.c`.
  Логическая сетка 1600×1000 (`ui_layout`: ui_ux/uy/uw/uh, aspect-preserving,
  на 16:9 есть отступ по X). Шрифты `ui_font` — ФИКСИРОВАННЫЕ пиксельные
  размеры (12/14/16/20/28/44, MONO_14/16), сеткой не масштабируются.
  Тема `ui_theme` (canvas/surface/ink/muted/accent_*; тёмная и светлая).
  Иконки/логотип — `ui_icons` (`ui_logo_big()`).
- **Кадр WM (41.2):** `wm_main_loop` рисует два вида кадров. `render_scene` —
  слой рабочего стола из кэша (`g_desk`, перерисовка только при смене ключа
  `desktop_state_key`: минута, слот rail, тема) + окна + оверлеи; на экран
  уходит только область окон этого и прошлого кадра (`compositor_frame_bbox`,
  `compositor_present_rect`), полный вывод — при оверлеях/MIND/анимациях.
  `render_cursor` — движение мыши без изменения сцены: программный курсор с
  сохранением фона (`cursor_overlay_*`) либо аппаратный (`g_hw_cursor`, очередь
  курсора virtio-gpu; включается только с virtio-планшетом). Поводы для кадра
  сцены: клавиша, кнопка/перетаскивание, анимации, `wm_invalidate()`, при
  видимых окнах — раз в 100 мс (приложения рисуют в `on_draw` и о своих
  изменениях не сообщают). Пустой стол в простое не рисуется. Чужой вывод на
  экран (экран блокировки) WM видит по `doublebuf_present_count`. Смена
  режима — `drm_set_mode` (`gpustat mode WxH`, не больше режима прошивки).
- **virtio (41.2):** `virtio.c` — общий слой: modern PCI, split virtqueue,
  `virtio_pci_probe_nth` (несколько устройств одного id), MSI-X
  (`virtio_pci_msix_setup/route`; векторы 80..95, `irq_alloc_vector` в idt.c),
  `virtq_init_irq`; `virtq_wait_used` при прерывании спит в hlt (cli → проверка
  → `sti;hlt`), иначе опрос. INTx нет (нужен разбор ACPI _PRT). Драйверы:
  `virtio_gpu.c` (controlq по MSI-X с самопроверкой, UPDATE/MOVE_CURSOR),
  `virtio_input.c` (клавиатура: evdev → скан-коды набора 1 →
  `keyboard_inject_scancode`; планшет → `mouse_inject_abs`), `virtio_blk.c`
  (занимает слот `ata.c`, `ATA_KIND_VIRTIO`: установщик/GPT/FAT32/FDE без
  правок; буфер вызывающего без копии, до 1 МиБ за запрос), `virtio_net.c`
  (тот же `net_interface_t`; приём по MSI-X + страховка таймером 10 мс).
  Старые пути (VESA, PS/2, IDE/AHCI, e1000/rtl8139) — запасные, выбираются сами.
- **Время (41.2):** таймер LAPIC 1000 Гц (`lapic_timer_start`, калибровка по
  PIT, только BSP); `timer_get_uptime_ms` — 1 мс; `pit_ticks()` по-прежнему
  100 Гц (каждый 10-й тик LAPIC), PIT замаскирован. WM спит в hlt до
  настоящего прерывания (`irq_event_count`) или границы 10 мс.
- **Замеры (41.2):** `framestat.c` — фазы кадра по TSC; `compstat`,
  `compstat watch`; `/disk/Sanctum/perf.conf`: `fstat=1` (строка `[fstat]` в
  serial раз в секунду), `hwcursor=0`, `diskbench=1`, `netbench=<url>`.
- **Сеть (48.2):** кадры разбираются ПРЯМО в прерывании карты (virtio-net —
  MSI-X, e1000 — таймер 10 мс), оттуда же уходят ACK. Правила стека (`netif.h`):
  общее с обработчиком — под `net_irq_save/restore` (сборка сегмента в `tcp.c`
  `emit`, кэш ARP `arp_lookup`, буферы `ipv4_send`/`udp_send_from`); ждать
  (ответа ARP) можно только при разрешённых прерываниях — в обработчике пакет
  отбрасывается, поэтому `tcp_connect` заранее зовёт `ipv4_route_ready`;
  все ожидания — `net_idle()` (сон + «насос» кадров `net_set_idle_hook`, не
  чаще 33 мс), отмена — `net_cancel()`, причина отказа — `net_set_error`
  (`net_error_t`: no-address/dns/unreachable/refused/tls/timeout/…).
  `netcfg.c` — единственный, кто назначает адрес: `/disk/Sanctum/network.conf`
  (`mode=auto|dhcp|static`, ip/mask/gateway/dns/dns2); auto = DHCP, через 3 с
  молчания — «запасная статика», при ответе DHCP — обратно. `netcfg_boot`
  (после `net_init`), `netcfg_late_load` (после FDE), `netcfg_tick` (цикл WM).
  `dhcp.c` — автомат (DISCOVER 1.5→30 с без конца, REQUEST×3, T1/T2, NAK,
  истечение), события `dhcp_take_events`; ответ сервера применяет главный
  поток. `tcp.c`: один сокет, RST на «ничьи» сегменты, FIN только по порядку,
  `tcp_close` ждёт ≤250 мс, с непрочитанным — `tcp_abort`. `http.c`
  `http_read_response` — ПОТОКОВЫЙ разбор (заголовки 32 КБ, тело сразу в буфер
  вызывающего, chunked автоматом, 1xx пропускаются), общий для TCP и TLS;
  адрес до 2 КБ (`HTTP_URL_MAX`), путь с пробелами/кириллицей уходит как %XX;
  ожидание данных 30 с. `https.c` `net_fetch_ex` (`net_fetch_t`: статус,
  итоговый URL, Content-Type, truncated, complete, cert_trusted, error):
  cookies (`cookies.c`: память, 48 штук, Domain/Path/Secure) в запрос и из
  ответов, включая редиректы; `Accept-Encoding: gzip` и распаковка
  (`inflate.c`: gzip/zlib/deflate; при сбое — повтор с identity). `icmp.c`:
  «узел недоступен»/«TTL истёк» → быстрый отказ `tcp_connect`. `dns.c`: предел
  7 с на имя, «последний удачный сервер» первым, `dns_last_failure`.
  perf.conf: `dhcplease=N`.
- **TLS (47 → 48.2):** `tls.c` — только TLS 1.2, 8 наборов: ECDHE-ECDSA/RSA с
  AES-128-GCM, ChaCha20-Poly1305, AES-256-GCM-SHA384 и RSA с AES-128/256-GCM;
  группы x25519, P-256, P-384; extended_master_secret, renegotiation_info;
  хеш рукопожатия считается сразу на SHA-256 и SHA-384 (какой нужен, скажет
  ServerHello); запись уходит ОДНИМ вызовом `tcp_send_all` (заголовок + тело),
  запись с ClientHello помечена версией 0x0301. Цепочка: `crypto_x509.c` —
  RSA, ECDSA P-256 и P-384 (`crypto_p384.c`: Монтгомери 6×64, проверка
  подписи + ECDH не за постоянное время), обход снизу вверх с вопросом к
  хранилищу на каждом шаге; `crypto_ca_blob.c` — ВЕСЬ набор Mozilla (150
  корней, генерирует `scripts/gen_ca_blob.sh` вручную в WSL, файл лежит в
  дереве). Причины отказа: `tls` и `tls-silent` (сервер не ответил на
  ClientHello ни байтом). Ожидание: 10 с в рукопожатии, 30 с на данных.
- **Mesh и браузер (48.2):** `ui_kit.c` — кнопки/поля/переключатель в
  координатах ОКНА (`ui_components` рисует в сетке экрана — для содержимого
  окна не годится); один `pk_rect_t` и для отрисовки, и для клика.
  `app_mesh.c` — настройки сети (режим, 5 полей, 4 кнопки). `app_browser.c` —
  workspace-браузер: HTML → блоки (h1–h3, p, li, pre, hr, цитаты) + стилевые
  точки (жирный, код, ссылка) → строки (`doc_layout`, один раз на страницу),
  область страницы — под-мишень `gfx_target_t` внутри буфера окна (обрезка
  сама), история 32, страницы ошибок, буферы ~4 МБ выделяются раз. Для
  настоящих сайтов: не-адрес в строке = поиск (`BR_SEARCH_URL`, DuckDuckGo
  html; ссылки `…/l/?uddg=` раскрываются), поле `<form>` = псевдоссылка
  `amplify-form:` (запрос набирается в адресной строке), `<meta refresh>`,
  пропуск скрытого (hidden/aria-hidden/display:none/`<button>`/`<dialog>`;
  кроме `<div hidden id="S:n">` — так React шлёт само содержимое),
  `<noscript>` показывается, старт с `<main>`/`<article>` (`main_pos`; у
  MediaWiki точнее — `<div id="mw-content-text">`, иначе Википедия начинается
  со списка трёхсот языков), между ссылками вплотную ставится пробел,
  HTML узнаётся по содержимому, `%D0…` в строке показывается буквами, предел
  HTML 4 МБ. Оба
  печатают экранные координаты элементов (`[mesh] layout`, `[browser] layout`,
  `[browser] links:`) — по ним кликает тест. Долгая операция приложения:
  `net_set_idle_hook(wm_pump)` — кадры идут, ввод (кроме Esc) отбрасывается.
  Клавиши `KBD_KEY_PGUP/PGDN/HOME/END/DELETE`, колесо — `window.on_scroll`
  (только virtio-input). Shell: `weave` (сеть), `summon <prism>` (открыть окно).
- **Анимации (48.2):** `ui_fx.c` — SSE2: `fx_dim` (затемнение; `gfx_fill_rect`
  альфу НЕ смешивает), `fx_blit`, `fx_blit_scaled`; «снимок» панели в общий
  запасной буфер (`fx_capture_*`, владелец один); `fx_panel_t` —
  появление/исчезновение оверлея (`fx_panel_show/hide/frame/moving`,
  `fx_layer_frame` — слой на весь экран): MIND, Beacon, Constellation, шторка/
  toast. Окна (`compositor.c`): время в мс, workspace-окно поднимается и
  проявляется ИЗ СВОЕГО БУФЕРА (`on_draw` один раз), обычное — «хлопок»
  ease-out-back. `wm.c`: кадры движения — раз в 16 мс (`WM_FRAME_MS`), на
  время анимации окна «заморожены» (`compositor_set_frozen`: без `on_draw`),
  окно под непрозрачным workspace-окном не рисуется вовсе, вход на стол —
  снимок экрана входа уезжает вверх и тает (`enter_*`). Статистика —
  строки `[anim] …` в serial при `fstat=1`. `reduced_motion` — всё мгновенно.
- **AMPLIFY MIND (49, 49.1):** `smp_pool.c` — AP: общий IDT, сон `sti;hlt`,
  IPI 0xF0, `smp_pool_parallel_for`, долгая задача на AP1-«дирижёре».
  «ggml-lite»: `mind_gguf.c` (GGUF v2/v3), `mind_math.c` (matvec и ПАКЕТНЫЙ
  matmul F32/F16/BF16/Q8_0/Q4_0; SSSE3, под TCG авто-SSE2), `mind_model.c`
  (llama и qwen3: QK-norm, RoPE NORM/NEOX, head_dim из key_length, GQA,
  SwiGLU; пакетный prefill по 32 токена, побитово = поштучному; KV f32 в
  `pmm_alloc_big`, ctx сам уменьшается при нехватке RAM), `mind_tok.c` (SPM +
  GPT2-BPE, pre gpt2/smollm/qwen2, спецтокены USER_DEFINED в тексте всегда,
  CONTROL — только по id; \p{L}/\p{N} — таблицы `mind_unicode.h` из
  `tools/gen_unicode_tables.py`), `mind_chat.c` (общий с хостом сборщик
  промпта: ChatML / Qwen3 с пустым `<think>`, блок `<tools>` = json.dumps
  `tools/mind_tools.json`, разбор `<tool_call>{json}`). `mind.c` — выбор
  САМОЙ БОЛЬШОЙ модели, влезающей в RAM, загрузка, кэш системного промпта
  Qwen3 (`g_sys_n`, считается раз после загрузки), сэмплер, кольцо AP→BSP,
  агентный цикл (≤3 вызова; exec — после Enter), роутер (Qwen3 с agent — только
  ps + точные факты `mind_facts` как начало ответа; иначе ps/ls/read/logs),
  `/think` на ходу. `mind_tools.c` — ps/ls/read/logs/exec (users.txt не
  отдаётся; строки `[mind]` из logs убраны); `mind_widget.c` — Ctrl+Space,
  Markdown, приглушённый `<think>`, Ctrl+L EN/RU (ЙЦУКЕН). `klog` — `serial.c`.
  Веса — `/disk/vault/mind/*.gguf` или `/cdrom/mind` (live), конфиг —
  `/disk/Sanctum/mind.conf` (model/ctx/temp/top_p/top_k/repeat/max_tokens/
  greedy/agent/think/tools=ps,ls,…/simd/bench_on_boot; 0/пусто — по модели).
  Эталон — `tools/mind_ref.py` (numpy, ПОБИТОВО повторяет арифметику ядра),
  хост-сборка движка — `tools/mind_host.c` (pthreads).
- **phantom / VT-x (50.0, 50.1):** три слоя, как у KVM. `vmx.c` (аналог
  kvm-intel): разведка `vmx_probe` (CPUID.1:ECX[5], IA32_FEATURE_CONTROL, MSR
  IA32_VMX_* в порядке зависимостей — MSR, которого нет, = #GP; таблица битов
  `g_defs` одна на печать, вердикт `vmx_ready()` и сверку в тесте),
  `vmx_probe_this_cpu` на каждом AP, VMXON/VMXOFF на ВСЕХ ядрах, пока есть хоть
  одна ВМ (`vmx_enable_all/disable_all`, счётчик; CR4.VMXE ставится тут, в C).
  `hv.c` (аналог kvm): ВМ = непрерывный кусок памяти хоста (`pmm_alloc_big`,
  выровнен на 2 МБ, за ним сторожевая страница) + EPT (2 МБ или 4 КБ) + один
  vCPU; VMCS строится на ядре гостя; гость стартует в РЕАЛЬНОМ режиме
  (unrestricted guest) и сам идёт в защищённый и длинный. CR0.PE/PG/NE/CD/NW —
  под маской: смена режима = выход, EFER.LMA и бит «IA-32e» входа ведём сами;
  CR4: VMXE/SMXE/OSXSAVE/PCIDE гостю запрещены (#GP). Сам эмулирует CPUID
  (прячет VMX, XSAVE/AVX, x2APIC, MONITOR; подпись «PHANTOMAMPLY»), EFER/FS/GS/
  PAT/SYSENTER (остальные MSR = #GP, номер запоминается), CR8. Прерывания
  хоста = выход без подтверждения, после выхода `sti`. FPU: fxsave/fxrstor
  хост/гость вокруг каждого входа. `vmx_entry.asm` — VMLAUNCH/VMRESUME и точка
  возврата (HOST_RIP). Наружу — контракт вида KVM_RUN: `hv_exit_t` (IO, HLT,
  VMCALL, EPT, SHUTDOWN, FAIL) отдаётся обработчику модели устройств, который
  исполняется НА AP (только вычисления над готовой памятью, §5.11; печатать
  нельзя — состояние для разбора снимает `hv_diag`). `phantom.c` — модель
  устройств; в 50.1 это стенд: 5 отладочных портов и встроенный гость
  `phantom_guest.asm` (кусок .rodata, копируется в память гостя по 0x1000).
  **vCPU — «жилец» AP** (`smp_pool.h`): старший AP, квант 2 мс (таймер
  вытеснения VMX); во время кванта ядро не участвует в parallel-for (маска
  `g_guest_mask`, гонка закрыта парой «объяви о себе — посмотри на другого» +
  IPI); в «горячем» режиме пула (MIND генерирует) после кванта ядро 8 мс
  служит пулу — гостю ~20 % ядра. `smp_pool_call` — вызов функции на заданном
  ядре; `smp_pool_kick` — выбить ядро из гостя/сна. AP теперь на общем GDT
  ядра со своим TSS (`gdt64_init_ap`; TSS AP — селекторы 0x40+). Shell:
  `phantom` | `caps` | `selftest` | `bench` | `spin` | `status` | `stop` |
  `images` | `run` | `type` | `screen`; окно — `summon Phantom`.
  **50.2 — прерывания и устройства.** Модель устройств с прерываниями —
  `hv_model_t`: `on_exit` + три вопроса на AP перед каждым входом
  (`irq_pending(now)` — «подвинь таймеры и скажи, ждёт ли прерывание»,
  `irq_ack` — вектор, как цикл INTA, `deadline` — TSC ближайшего события).
  Доставляет hv (`irq_service`): нельзя сейчас (IF=0, после STI) — выход «окно
  прерываний»; событие, не дошедшее из-за выхода посреди доставки, кладётся
  заново (IDT-vectoring). HLT при IF=1 = сон vCPU (`HV_VCPU_HALTED`): ядро
  свободно для пула, будит модель (`hv_vcpu_kick`) или таймер BSP
  (`hv_tick_1ms` из `timer_ms_tick`: дедлайн в `g_wake_tsc`). Квант режется
  таймером вытеснения и по дедлайну модели. Старт гостя — `hv_start_t`:
  реальный режим или 32-битный защищённый (протокол загрузки Linux). Гостю
  мы представляемся как KVM (CPUID 0x40000000 «KVMKVMKVM», только часы:
  kvmclock через MSR 0x4b564d00/01 — структура заполняется раз, дальше гость
  считает время сам по RDTSC). MSR SYSCALL/KERNEL_GS_BASE — настоящие
  регистры ядра гостя (на AP ими больше никто не пользуется; сохранение при
  загрузке vCPU, возврат при снятии). CPUID: локального APIC, MTRR, MCE гостю
  нет; листы, про которые не решено явно, обнуляются. Частота TSC — по каналу
  2 таймера 8254 за 20 мс (`tsc_calibrate_pit`). `phantom_pc.c` — «плата»:
  пара 8259 (фронт, без AEOI), 8254 (счёт вычисляется из TSC; канал 0 — IRQ0
  в режимах 2/3 периодически, иначе один раз; канал 2 + порт 0x61),
  16550A на 0x3F8/IRQ4 (передача мгновенная, петля, кольца вывода 64 КБ
  AP->BSP и ввода 256 байт BSP->AP), CMOS, порт 0x92, сброс (0x64/0xCF9);
  остальные порты пусты, PCI нет. `phantom_linux.c` — СТЕНД: bzImage + initrd
  с `/disk/vault/phantom` (чтение `fat32_read_file_at`, не VFS) по 32-битному
  протоколу. `phantom_tick()` в цикле WM выносит консоль гостя в serial
  строками `[guest] …` (не больше 192 символов за проход). Команды стенда (в
  справке их нет): `phantom linux [init]`, `phantom type <текст>`, а
  `status`/`stop` работают и для него.
  **50.3 — BIOS и загрузка с ISO.** `phantom run <путь.iso>`: файл с тома
  FAT целиком в память хоста (`pmm_alloc_big`; службы BIOS идут на AP и диск
  читать не могут), `bios_boot_cd` делает «POST» с BSP — ПЗУ по F000:0,
  таблица векторов, область данных BIOS (0x400) и EBDA (0x9FC00), чистый
  экран, разбор El Torito (сектор 17 -> каталог -> запись по умолчанию, только
  «без эмуляции»), образ загрузчика на 0000:7C00 — и гость стартует в
  реальном режиме с DL=E0h. BIOS «паравиртуальный»: `phantom_bios_rom.asm` —
  256 заглушек «VMCALL; IRET» (вектор n -> F000:n*4) + настоящий код только
  для INT 08h (счётчик 0x46C, INT 1Ch, EOI) и INT 16h (STI, VMCALL, при CF —
  HLT и снова); `phantom_bios.c` — службы на C: по адресу VMCALL узнаётся
  вектор, результат — в регистрах и в FLAGS, лежащих в стеке гостя
  (`hv_guest_get/set` — счётчик команд, стек, флаги, базы сегментов из VMCS).
  INT 10h — экран 80x25 = память гостя B8000 (режим один; VBE нет) + поток в
  консоль (`stream`: подряд идущие символы строки, смена строки = перевод);
  INT 13h — один привод E0h: 41h/42h/48h/4B01h, секторы по 2048 из образа;
  INT 15h — E820 (4 записи), E801, 88h, A20; INT 16h — из кольца ввода
  консоли (того же, что читает UART: `phantom type` работает и для
  загрузчика, и для Linux); INT 1Ah — тики и часы; 11h/12h; 18h/19h = конец
  гостя; исключение в реальном режиме = конец гостя с адресом. VMCALL не из
  ПЗУ (гипервызов KVM от Linux) -> RAX=-1000. Службы, которых нет, копятся в
  `bios_stats.unknown` («вектор:AH»). `phantom screen` — текстовый экран
  гостя в serial (`[screen] NN|…`). Вывод BIOS и вывод порта идут в одну
  консоль, между ними ставится перевод строки (`con_src`).
  **50.4 — диск гостя.** `pc_attach_disk` (перед стартом): у гостя появляется
  шина PCI — «механизм 1» (0xCF8/0xCFC), два устройства: мост 8086:1237 и
  диск virtio-blk 1af4:1001 «старого вида» (legacy: регистры в портах BAR0 =
  0xC000, 64 порта; прерывание INTA -> IRQ 11 через ведомый 8259; без MSI-X и
  списка возможностей — драйвер сам уходит на legacy). Одна очередь на 128,
  в памяти гостя (описатели, кольцо «доступно», со следующей страницы —
  «использовано»); запись в порт «проверь очередь» исполняется тут же на AP:
  чтение = копирование из образа ISO в памяти хоста; диск только для чтения
  (VIRTIO_BLK_F_RO), запись = ошибка. Тот же образ BIOS отдаёт загрузчику
  через INT 13h. `phantom run <iso> [строка]` — строка «уже набрана» к
  приглашению загрузчика (кладётся в кольцо ввода до старта vCPU); память
  гостя 512 МБ, если у хоста есть запас, иначе 256. Консоль: раз гость
  заговорил в порт (`uart_tx_seen`), поток с экрана BIOS в неё не идёт
  (загрузчик с директивой SERIAL пишет каждый символ и туда, и туда); в
  журнал не попадают управляющие последовательности терминала (ESC [ …).
  Видеоконтроллер: порты 3D4/3D5 хранят регистры, `pc_vga_origin` — начало
  видимого экрана в окне B8000 (32 КБ): Linux прокручивает экран сдвигом
  начала, а не копированием. В области данных BIOS есть COM1 (0x400 = 3F8h).
  **50.5 — окно гостя, клавиатура, образы.** `app_phantom.c` — приложение
  Phantom (реестр рабочего стола, `summon Phantom`): «библиотека» (карточка
  машины, список образов, строка для загрузчика, «Запустить») и «консоль»
  (экран гостя, состояние, «Остановить», «Ctrl+Alt+Del»); вид выбирается по
  `phantom_info()`. Окно одно; его закрытие гостя не трогает. Экран: раз за
  проход цикла WM (`app_phantom_tick`) — снимок 2000 ячеек B8000
  (`phantom_screen_cells`: с учётом начала экрана; курсор — из регистров
  видеоконтроллера, BIOS теперь тоже пишет его туда), сравнение с прошлым
  снимком, перерисовка только изменившихся знакомест в СВОЙ буфер, в кадре
  окна — `fx_blit`; `wm_invalidate()` — только при изменениях. Шрифт —
  `phantom_font_data.c`: DejaVu Sans Mono в CP437, знакоместа 8..24 px
  шириной и вдвое выше (генератор `tools/gen_phantom_font.py <ttf>`,
  Windows-python; TTF — из WSL, `/usr/share/fonts/truetype/dejavu/`, копия в
  `build/fonts`); берётся самое крупное, при котором 80x25 входит в окно (на
  1920x1080 — 16x32, экран 1280x800). Рамки и блоки B0h..DFh рисуются
  линиями (`cell_graph`). Клавиатура: `keyboard_set_raw_sink` (keyboard.c) —
  перехватчик сырых скан-кодов набора 1 в обработчике прерывания (PS/2 и
  virtio-input идут через него одинаково); пока клавиатура «у гостя»
  (щелчок по экрану, Enter, сам запуск), коды уходят `phantom_key` ->
  `pc_kbd_scancode` -> кольцо BSP->AP. Вернуть — правый Ctrl, щелчок мимо
  экрана, потеря фокуса, оверлей; Super и сочетания с ним гостю не
  отдаются; зажатые в момент возврата клавиши «отпускаются» за гостя.
  У гостя — 8042 в `phantom_pc.c` (порты 0x60/0x64, IRQ1): командный байт
  0x65 (перевод в набор 1 включён — коды хоста идут как есть), ответ
  контроллера занимает выходной буфер без прерывания, ответы клавиатуры и
  скан-коды — по одному на фронт IRQ1; мыши нет («петля» D3h возвращает байт
  без признака AUX, A9h отвечает ошибкой линии — Linux не ждёт таймаутов).
  Пока гость в реальном режиме: IRQ1 -> INT 09h -> служба BIOS (`int09`:
  флаги 0x417, очередь 0x41E на 15 клавиш) -> INT 16h (сначала очередь,
  потом кольцо консоли — `phantom type`). Запуск в два приёма: `phantom_run`
  проверяет (подпись CD001 — сразу) и выделяет память, образ читается
  порциями в `phantom_tick` (`lx_load_step`: до 8 мс за проход, порция
  32 КБ..4 МБ подстраивается под диск; цикл WM на это время не спит —
  `phantom_loading`), затем `lx_boot`. Источник образа — файл FAT либо
  `/cdrom/guests/*.iso` установочного носителя (`iso_read`). Список для окна
  — `phantom_images` (`/disk/vault/phantom` + `/guests` носителя; команда
  `phantom images`). `make iso` кладёт на носитель всё из `build/guests`;
  установщик копирует образы в `/vault/phantom` (`copy_guest_images`: только
  если гостей на машине можно запускать; сбой установку не валит).
- **rune / WebAssembly (51.0–51.5):** СВОЙ движок (не перенос чужого), объём
  — WebAssembly 2.0 без SIMD. Файлы движка общие для ядра и для сборки на
  хосте и о ядре не знают (память — `wasm_port_alloc/free`, линейная память
  программы — `wasm_port_mem_alloc/free`; печати нет).
  `wasm_bin.c` — разбор двоичного модуля в `wasm_module_t`: строки и тела
  функций не копируются (буфер с модулем обязан жить дольше модуля), память
  модуля — «арена», освобождается разом; ни одному числу из файла не верим
  (счётчик сверяется с остатком байт ДО выделения памяти, LEB128 — строго по
  ширине, имена — проверка UTF-8, порядок и длины секций). `wasm_valid.c` —
  проверка тел функций по алгоритму из приложения к спецификации (стек типов
  + стек блоков, «полиморфный» стек в недостижимом коде); считает `max_stack`
  и с 51.1 ПОПУТНО ПИШЕТ ВНУТРЕННИЙ КОД функции (`wasm_int.h`: слова по 64
  бита — команда | A | B): числа раскодированы, блоков нет — остаются переходы
  с готовым адресом и счётом «сколько верхних значений перенести / сколько под
  ними сбросить» (переход вперёд встаёт в цепочку через поле B и получает адрес
  на `end`); `IR_LOOP` в начале цикла — место, где списывается «топливо»;
  конец тела — `IR_RETURN`, на него же ведут переходы «из функции».
  `wasm_rt.c` — хранилище `wasm_store_t`: владеет экземплярами, памятью,
  таблицами, глобальными и стеком интерпретатора, освобождается целиком
  (модули обязаны жить дольше хранилища). Экземпляр — это указатели на объекты
  хранилища: импортированная память или таблица — та же самая, не копия. Типы
  функций приводятся к номеру в хранилище (`wasm_canon_type`) — сверка типа у
  `call_indirect` и у импорта = сравнение двух чисел. `wasm_instantiate`:
  импорты через `wasm_resolve_fn` (сверка вида, типа, пределов), свои объекты,
  сегменты элементов и данных (границы — в 64 битах), функция start. Функции
  хоста — `wasm_host_func` (аргументы и результаты в одном массиве ячеек).
  `wasm_interp.c` — один цикл `switch` по словам. Вызов функции модуля — НЕ
  вложенный вызов C: кадр кладётся в `s->frames`, стек значений — 64-битные
  ячейки без типов в `s->stack` (аргументы на вершине стека вызывающего
  становятся первыми локальными вызванной, без копирования), поэтому глубокая
  рекурсия программы не тратит стек ядра, а «стек исчерпан» — наша проверка
  (16 384 кадра, 262 144 ячейки). На ходу проверяется только зависящее от
  данных: границы памяти и таблиц, деление, преобразования в целое, глубина;
  типы и число значений доказаны проверкой. Значения: i32/f32 — младшие 32
  бита, ссылка — 0 либо указатель на `wasm_funcinst_t` (число хоста v для
  externref хранится как v+1). min/max, округления и преобразования в целое
  написаны руками (у процессора иная семантика), остальная арифметика — SSE2.
  Пределы исполнения: «топливо» (единица = 256 витков циклов и вызовов) и
  флаг остановки, оба читаются раз в 256 витков (`wasm_store_limit`); память
  экземпляра по умолчанию до 1024 страниц (64 МБ), растёт удвоением с
  копированием. Причины остановки — `WASM_TRAP_*` (`wasm_trap_name` = текст
  официальных тестов). `wasm_spec.c` — прогон официального набора из
  файла-пакета (`tools/wasm_pack_spec.py` из вывода `wast2json`; формат — в
  шапке файла), с 51.1 — с исполнением: на каждый файл .wast своё хранилище,
  модуль `spectest`, имена `$M` и `register`, сравнение по типу (NaN — по
  классу), модули файла живут до его конца (функция модуля, чей запуск
  прервался, могла остаться в чужой таблице); и `wasm_describe`. Виды отказа
  при загрузке: malformed / invalid / limit / unsupported (SIMD, потоки).
  `rune.c` — связка с ядром: порт памяти (куча; линейная память — страницы
  `pmm_alloc_big`, обнуляются), чтение файла целиком (сначала FAT, потом VFS),
  smoke 51 и 51.1, предел времени — `rune_tick_1ms` из таймера ядра ставит
  флаг остановки, когда выходит срок. Shell: `rune` (алиас `wasm`) | `info
  <файл>` | `call [-t мс] <файл> <функция> [аргументы]` (исполняется НА BSP,
  кадры стоят; предел по умолчанию 10 с; целые — десятичные/0x, дробные вида
  1.5; f32/f64 в ответе печатаются битами) | `selftest` | `spec <пакет>
  [verbose]`. Хост: `make wasm-host` (`build/wasm_host spec|info|call|fuzz|
  fuzz-exec`; `WASM_FUEL` — предел для `call`), `make wasm-host-asan`, `make
  wasm-apps` (нужен clang узла: `userspace/wasm/*.c` под wasm32-wasi и
  `userspace/wasm/pure/pure.c` без окружения -> `build/wasm/apps/*.wasm` в
  трёх вариантах сборки + `.host` — тот же исходник под gcc, его вывод —
  эталон). Пакет тестов — `bash scripts/wasm_spec_pack.sh` ->
  `build/wasm/spec.pack` (все модули набора — `build/wasm/spec-json/*.wasm`).
  **51.2 — WASI preview1.** `wasm_wasi.c` (общий для ядра и хоста, как весь
  движок) — модуль импортов `wasi_snapshot_preview1`, все 45 функций wasi-libc
  (+ proc_raise): типы сверены с clang узла буква в букву, иначе модуль не
  свяжется. Здесь вся ПОЛИТИКА: таблица описателей (0–2 — потоки, с 3 —
  заранее открытые каталоги, всего 48), ГРАНИЦА каталога (`resolve`: путь
  собирается по компонентам поверх пути открытого каталога, «..» выше его
  корня — ENOTCAPABLE, абсолютный — EPERM; хозяин получает уже готовый путь),
  сверка каждого указателя и длины с размером памяти программы (`mem_at` ->
  EFAULT), коды ошибок POSIX (ENOENT против ENOTDIR — по первому существующему
  предку). Хозяин (`wasi_host_t`) умеет только простое: вывод, stat, «прочитать
  файл целиком», «записать файл целиком», mkdir, remove, список каталога, часы,
  случайные байты, сон. Поэтому ОТКРЫТЫЙ ФАЙЛ — КОПИЯ В ПАМЯТИ
  (`wasm_port_mem_alloc`; хвост до ёмкости всегда нулевой — так работают дыры и
  ftruncate): на диск уходит при закрытии, fsync и в `wasi_run` после конца
  программы (в том числе после ловушки); созданный файл появляется на диске
  сразу пустым; rename без помощи хозяина = копия + удаление (каталог —
  ENOTSUP). Не реализовано и отвечает ENOTSUP: ссылки, времена файлов, права
  (rights показываются, но не ведутся), сокеты. stdout/stderr отдаются как
  «символьное устройство без перемещения» — по этому libc включает построчный
  вывод. `wasi_run` — запуск «команды»: экземпляр, `_start`, итог
  (`WASI_RUN_EXIT` с кодом / `TRAP` / `NOSTART`); proc_exit = ловушка
  `WASM_TRAP_EXIT`. В ядре (`rune.c`): файлы — том FAT (`fat32_stat` — новая;
  `fat32_vfs_autoregister(false)` на время записи: файлы программы в плоский
  реестр VFS на 256 записей не идут), имена и вывод UTF-8 <-> CP1251
  (`mind_to_cp1251/utf8`; символ, разрезанный между двумя fd_write, склеивается),
  управляющие символы в окно не пускаются, настенное время = RTC при запуске +
  таймер, сон — `hlt` до срока или до флага остановки, случайные байты —
  `crypto_rand_bytes`. Shell: `rune run [-t мс] [-d каталог] [-r] <файл>
  [аргументы]` — каталог (по умолчанию `/disk/nexus`, только на томе FAT)
  программа видит как «/»; `-r` — только чтение; строки `[rune] run …`, вывод,
  `[rune] exit N | мс, fuel, calls, memory` или `[rune] stopped: причина`.
  Окружение программы: `HOME=/`, `PWD=/`, `OS=AMPLIFY-OS`; argv[0] — имя файла.
  Хост: `build/wasm_host run [-d каталог] [-r] [-e ИМЯ=значение] <файл> …`
  (`WASI_NO_RENAME=1` — как в ядре; `WASM_STATS=1`), фаззинг с настоящим
  окружением — `WASM_FUZZ_DIR=<каталог> WASM_FUZZ_FUEL=120 … fuzz-exec`.
  Проверочные программы — `userspace/wasm/{hello,calc,files,env,jail}.c`.
  **51.3 — программа на своём ядре.** Интерпретатор умеет ВСТАТЬ НА ПАУЗУ и
  продолжить (`wasm.h`): положение — пять значений в хранилище (`r_fn`, `r_fp`,
  `r_sp`, `r_ip`, `r_depth` + `r_pend` — вызов, на пороге которого встали).
  `wasm_call` возвращает либо итог, либо паузу: `WASM_PAUSED_YIELD` (вышел
  отрезок — `wasm_store_slice`, счёт в тех же единицах, что «топливо») или
  `WASM_PAUSED_SERVICE` (хранилище помечено `remote`, а следующая команда
  «привилегированная»: вызов функции хоста, рост памяти или таблицы С
  ВЫДЕЛЕНИЕМ — рост в пределах уже выделенного идёт на месте). Такую команду
  исполняет `wasm_service` — тот же цикл, ровно одна команда, — затем
  `wasm_resume`. Функция хоста может ответить `WASM_HOST_RETRY` («пока нечем
  ответить»): вызов остаётся несделанным, `wasm_service` повторяют позже.
  WASI с `cfg.nonblocking`: пустой ввод (`in` -> `WASI_IN_WAIT`) и сон
  (poll_oneoff; срок — `wasi_wake_ns`) не ждут, а отвечают RETRY; запуск в два
  приёма — `wasi_prepare` (экземпляр, `_start`) и `wasi_finish` (итог, запись
  файлов). В ядре (`rune.c`, «программа на своём ядре»): программа — «жилец»
  AP, как vCPU phantom (`smp_pool_set_resident`): `rune_resident` на AP считает
  отрезок ~2 мс между `smp_pool_guest_enter/leave` (пул MIND это ядро не
  ждёт; в горячем режиме программе остаётся ~20 % ядра), на паузе «нужна
  услуга» пишет состояние `JOB_WAIT`, будит BSP (`smp_pool_wake_bsp` — IPI тем
  же вектором 0xF0) и возвращает ядро пулу. BSP — `rune_tick()` в цикле WM:
  `wasm_service` (функция окружения целиком, на BSP можно и диск, и окно),
  состояние `JOB_RUN`, `smp_pool_kick`. Услуги идут пачкой: до 2 мс за проход,
  после каждой 0.1 мс ожидания следующей просьбы. Хранилище в каждый момент у
  одного ядра (RUN — у AP, WAIT — у BSP); прежний жилец узнаёт о конце
  программы по номеру запуска (`gen`). Ждущая ввода и спящая программа ядро
  не занимает. Цикл WM перед сном: `cli`, `rune_wants_service()`, `sti; hlt`.
  Ядро для программы — `job_pick_slot`: со 2-го по порядку, старшее оставляем
  гостю phantom, первое — дирижёру MIND, пока есть выбор; на двух ядрах — слот
  1; AP нет — исполнение на BSP, как в 51.2 (предел времени обязателен).
  Пределы: память (`-m МБ` -> `max_memory_pages` хранилища; программе —
  отказ memory.grow), время СЧЁТА (`-t мс`; по TSC, проверяет таймер ядра раз
  в мс), файлы (16 МБ на файл, 64 МБ на все). Доступ (`dir_check`): каталог
  только на томе FAT; корень тома (там users.txt) и `/disk/Sanctum` не даются
  (без учёта регистра); `-n` — без каталога вовсе; `-r` — только чтение.
  Терминал (`apps.c`): вывод fb_console привязан к окну только на время
  команды, поэтому программа пишет через `term_job_out` (в окно-владелец и в
  serial); владельца помнит сама программа (`rune_opts_t.owner`,
  `rune_owner()`), и пока она жива, строки из этого терминала идут ей на ввод
  (`rune_input`), Ctrl+C — `rune_stop`, Ctrl+D — конец ввода. Shell: `rune run
  [-b] [-i] [-t мс] [-m МБ] [-d каталог] [-r] [-n] <файл> [аргументы]` (`-b` —
  в фоне: терминал свободен, вывод только в serial, ввод закрыт, если нет
  `-i`), `rune status | stop | type <текст> | eof`. Одновременно — одна
  программа. Хост: `spec <пакет> --stepped` (пауза на каждом витке, услуги
  через `wasm_service`), `WASM_STEPPED=1` для `run` и `fuzz-exec`.
  **51.4 — окно Rune.** `app_rune.c` — приложение Rune (реестр рабочего
  стола, `summon Rune`; значок — `ui_icon_rune.c`, его строит
  `tools/gen_rune_icon.py` из плитки готовых значков и нарисованной руны):
  «библиотека» (карточка исполнителя, список программ, настройки запуска —
  аргументы, каталог, доступ «Запись / Чтение / Без файлов», предел памяти,
  «Запустить») и «консоль» (вывод программы с прокруткой, строка ввода,
  «Остановить», «Конец ввода», в подвале — ядро, время счёта и всего,
  память; после конца — итог, «К библиотеке», «Запустить снова»). Вид
  выбирается по `rune_query()`: программа есть или её итог ещё не закрыт —
  консоль. ЗАКРЫТИЕ ОКНА ПРОГРАММУ НЕ ТРОГАЕТ: всё, что должно пережить окно,
  лежит в статической «сессии» `g_ses` (журнал, чья программа, итог,
  настройки запуска); владелец программы для движка — адрес сессии, приёмник
  вывода — `ses_out`; конец программы замечает `app_rune_tick` и без окна.
  Журнал — буфер 256 КБ (CP1251) + таблица начал ЭКРАННЫХ строк (длинная
  строка режется по ширине консоли; ведётся по мере прихода текста), оба — из
  кучи при первом открытии (не в .bss, §5.50); переполнился — отбрасывается
  первая половина целыми строками. Строки `[rune] …` самого движка в журнал
  окна не идут (итог окно пишет само, по-русски), в serial идёт всё. Эхо
  ввода — как в терминале: набранное встаёт за приглашением программы.
  Программа терминала или фоновая (`rune run`, `-b`) в окне видна как
  «чужая»: состояние и «Остановить», без вывода и ввода. Для окна в `rune.c`:
  `rune_query` (состояние и итог последней программы числами), `rune_last_error`
  (причина отказа запуска), `rune_programs` (библиотека: файлы `*.wasm` из
  initrd — встроенные — и из `/disk/vault/runes`; каталог заводится сам).
  Описание программы — пользовательская секция модуля `rune.about` (в
  исходнике — ассемблерная вставка `.section .custom_section.rune.about`;
  атрибут section у clang секцию не создаёт), читается обходом заголовков
  секций без загрузки модуля. Встроенные программы — `userspace/wasm/{oracle,
  abacus,sieve,scribe}.c`; готовые модули ЛЕЖАТ В ДЕРЕВЕ
  (`userspace/wasm/dist/*.wasm`, `-O2 --strip-all`, по 31–41 КБ) и идут в
  initrd (`RUNE_DIST_FILES`): на ПК clang в wasm нет. Обновить —
  `bash scripts/pve_wasm_dist.sh` (узел: `make wasm-dist`, сверка с gcc,
  копия обратно на ПК). Shell: `rune list`, `rune screen` (видимые строки
  консоли окна — в serial, `[rune-scr] NNNN|…`), `rune log` (весь журнал,
  `[rune-log] NNNN|…`; `#` вместо `|` — строка самого окна); в окне то же —
  Ctrl+P и Ctrl+O.
  **51.5 — JIT.** `wasm_jit.c` (общий для ядра и хоста, как весь движок):
  при создании экземпляра (`wasm_instantiate`, ДО раскладки сегментов) каждая
  функция модуля переводится из внутреннего кода в машинный код x86-64 —
  «шаблон» на команду. ГЛАВНОЕ РЕШЕНИЕ: машинный код живёт в тех же данных,
  что интерпретатор, — тот же стек значений (`s->stack`), те же кадры
  (`s->frames`, в кадре добавлено поле `native` — адрес возврата в машинном
  коде), то же «положение» (функция, fp, sp, место во внутреннем коде). Родным
  стеком процессора он не пользуется вовсе (вызов = кадр + `jmp`, возврат =
  `jmp` по адресу из кадра): рекурсия программы не тратит стек ядра, а пауза
  и продолжение 51.3 работают без переделки. Регистры: rbx = fp, r13 = sp,
  r12/rbp = память программы и её размер, r14 = контекст (`wasm_jit_ctx_t`),
  r15 = счётчик витков; рабочие — rax, rcx, rdx, rsi, rdi, xmm0. Вершина
  стека «ленивая» (сдвиг `d` помнит эмиттер, r13 двигается на границах) и
  «виртуальная» (`vs[]`: верхние значения числятся «в ячейке / в локальной /
  константа / в rax / во флагах» и раскладываются по ячейкам — `mat_all` — у
  меток, переходов, вызовов и перед шаблонами, которые про это не знают);
  сравнение, за которым сразу идёт условный переход, сливается с ним.
  Чего машинный код НЕ умеет сам (таблицы, bulk-memory, рост памяти,
  округления, min/max, преобразования с ловушками и с насыщением, popcnt,
  u64 -> дробное, copysign, функции хоста) — отдаёт «водителю»
  `wasm_jit_run`: тот просит интерпретатор исполнить РОВНО ОДНУ команду
  (`s->step_once`, итог `WASM_STEPPED`) и возвращается в машинный код.
  Выход — «запись» `jit_exit_t` (причина, функция, место, адрес продолжения):
  DONE, TRAP, TICK (счётчик витков: часы, флаг остановки, «топливо», конец
  отрезка — та же логика, что `TICK()` интерпретатора, поэтому счёт «топлива»
  совпадает), TICK_CALL, CALLOUT (функция хоста), FALLBACK (команда для
  интерпретатора), RETFRAME (возврат в кадр без адреса). Вернуться в машинный
  код можно только в «точках входа» — таблица функции `jit_map` (начало,
  после каждого цикла, вызова и «чужой» команды), двоичный поиск по месту во
  внутреннем коде. `wasm_service` (одна привилегированная команда на главном
  ядре) по-прежнему исполняет интерпретатор. Не хватило памяти под код —
  хранилище целиком остаётся на интерпретаторе (`s->jit_off`). Память под
  код — `wasm_port_exec_alloc` (в ядре — куча: она исполняемая; на хосте —
  mmap RWX). Выключатель — `wasm_jit_default(bool)` (на хранилища, созданные
  после): `rune jit [on|off]` в ядре, `WASM_JIT=0` на хосте; чем исполняется
  программа — в конце строки `[rune] run … , jit | interpreter`, в `rune`
  (`jit=1`) и в подвале окна Rune. smoke 51.5 при загрузке.
- **Прочее:** `shell.c` (команды), `apps.c` + `app_*.c` (GUI; Phantom — окно
  гипервизора, Rune — окно программ WebAssembly), `users.c`
  (`/disk/users.txt`, `name:fnv1a64hex`), `crypto_*.c` (SHA-2, HMAC, PBKDF2,
  AES/AES-NI, GCM, XTS, ChaCha, RSA, X25519/Ed25519, X.509), `tls.c` (TLS 1.2),
  карты e1000/rtl8139 (запасные к virtio-net), `amply.c` (пакеты .amf),
  `ns/cgroup/seccomp/box.c` (песочницы), `gles.c` (софт-3D), `smp.c`.
- **Папки ОС:** `/disk/Sanctum` (конфиг), `/disk/nexus`, `/disk/vault`
  (в нём `mind` — модели, `phantom` — образы гостей, `runes` — программы),
  `/disk/forge`, `/disk/fonts`. Конфиг установленной системы:
  `/disk/Sanctum/system.conf`, тема — `/disk/Sanctum/system/ui-config.ini`.
- **Команды shell:** guide, shine, voice, scan, read, engrave, matrix, embers,
  pulse, https, disk, launch, elfinfo, gpustat, compstat, Amply, box, seal,
  muse (MIND), weave (сеть), summon (окно приложения), phantom (VT-x, алиас
  vm), rune (WebAssembly, алиас wasm; окно — `summon Rune`), shade, clock,
  cycle,
  freeze (+ алиасы help/ls/cat/echo/reboot…).

## 5. Грабли (каждая уже стоила часов)
1. **Кодировка:** GCC транскодирует строки UTF-8 → CP1251. В литералах
   запрещены символы вне CP1251 (`→`, `×`, box-drawing) — сборка падает.
   `·` = `"\xB7"`, `•` = `"\x95"`.
2. **FPU/SSE** включены в ядре (GCC пишет xmm где угодно). Любую новую
   настройку CR0/CR4/MSR повторять во ВСЕХ точках входа: `boot.asm:_start`,
   `boot.asm:_efi_start`, `smp_trampoline.asm:tramp_pm32` (без стека: у AP
   ESP=0). FXSAVE требует 16-байтового выравнивания.
3. **Стек ядра 32 КБ** (`boot.asm: resb 32768`; у AP — тоже 32 КБ, `smp.c`):
   локальные массивы > ~4 КБ → тихий triple fault. Большие буферы — `static`
   или kmalloc. Проверка: сборка с `CFLAGS_EXTRA=-fstack-usage`, файлы `.su`
   (самые толстые кадры — `amply.c: db_persist/db_load` по 16 КБ — не чинены).
4. **Makefile:** prerequisite-списки раскрываются при чтении правила —
   переменная, объявленная ниже по файлу, в них пустая.
5. **Бюджет кадра:** CPU-рендер ≤ ~5 мс/кадр. Иначе PS/2 (1-байтовый буфер
   8042) теряет пакеты мыши. `mouse_poll` — только в CLI-секции;
   `compositor_update_animations()` — строго раз за кадр.
6. **ATA:** таймауты по часам (`timer_get_uptime_ms`), FLUSH — в конце
   логической операции (`disk_flush_dev`), не после каждой записи.
7. **FAT32:** кластеры выделять только в пределах тома (FAT длиннее тома);
   файлы чужого тома не регистрировать в VFS (`g_vfs_attached`).
8. **Экран входа:** R/X (перезагрузка/выключение) — только на пустом поле
   имени, иначе пароль с этими буквами невводим.
9. **QEMU:** `-no-reboot` превращает triple fault в остановку всей VM;
   `-d cpu_reset -D file` показывает CR0/CR4 перед сбросом.
10. **libgcc нет:** `__builtin_popcount` без `-mpopcnt` зовёт `__popcountdi2`
   → undefined reference. Такие builtins — своей функцией. Проверка:
   `nm build/kernel-x86_64.bin | grep ' U '` должно быть пусто.
11. **Код на AP** (`smp_pool_parallel_for`, задача дирижёра) — только чистые
   вычисления над заранее выделенной памятью: kmalloc, VFS/FAT32/ATA, экран,
   shell однопоточные. Всё выделять на BSP до запуска.
12. **TCG и SIMD:** под эмулятором QEMU `pmaddubsw/psignb` (SSSE3) в ~5 раз
   медленнее SSE2 (`pmaddwd`); на железе/KVM наоборот. `mind_math` узнаёт TCG
   по CPUID 0x40000000 = "TCGTCGTCGTCG".
13. **mtools в скриптах:** `mmd`/`mcopy` на существующее имя молча ждут ответа
   в stdin → всегда `-D s`/`-D o` и `</dev/null`. `kill` обёртки `timeout`
   не убивает QEMU — убивать PID самого qemu.
14. **Эталон MIND и float:** квантование активаций Q8_0 превращает 1 ulp в
   ±1 целого q; к 30-му слою «эквивалентные» реализации расходятся на ~0.15
   логита, и greedy-ids совпадают случайно. Эталон повторяет порядок операций
   ядра (последовательные суммы, SSE-скалярное, m_expf) — любое изменение
   арифметики ядра повторить в `tools/mind_ref.py` и пересчитать эталоны.
15. **Промпт = шаблон модели:** текст между управляющими токенами кодировать
   ОДНИМ куском (`mind_prompt_*`): «\n\n» как отдельный кусок и «\n\n…» в
   составе строки дают разные ids. Проверка — `scripts/check_qwen3_prompt.sh`.
16. **Куча ядра — фиксированные 16..144 МБ** (`KHEAP_START/END`), карта
   памяти ей не указ, а где лежат initrd и инфоблок MB2 — решает загрузчик.
   GRUB в EFI-режиме кладёт модуль на 0x01600000, внутрь кучи: `doublebuf`
   затирал initrd (Page Fault в `initrd_node_read`, CR2 чуть выше 4 ГБ, в
   регистрах 0xFF000000 — цвет пикселя). Теперь `pmm_relocate_boot_data`
   (после `pmm_init`, ДО `heap_init`) выносит модули и инфоблок из кучи
   (`[pmm] moved MB2 module…`), а наш BOOTX64.EFI сам обходит этот диапазон
   (`uefi_alloc_pages_low`). Как GRUB-EFI вообще оказался в ISO — п. 17.
17. **grub-mkrescue строит ветки по установленным модулям.** В WSL есть только
   `i386-pc`, на узле — ещё и `x86_64-efi`: там он добавлял СВОЙ EFI-загрузчик
   первой EFI-записью El Torito, и под UEFI стартовал GRUB, а не наш
   BOOTX64.EFI. Makefile зовёт `grub-mkrescue -d $(GRUB_PC_DIR)` (только BIOS);
   проверка: `xorriso -indev <iso> -report_el_torito plain` — ровно одна
   запись BIOS и одна UEFI (`/efiboot.img`). Сборка на другой машине = другой
   ISO: после переноса сборки гонять тест установщика.
18. **Heredoc в Bash съедает обратные слэши и в python-скриптах** (`\n` в
   строке C превращается в живой перевод строки, `\\\n` ломает сам скрипт).
   Патч-скрипты — только файлом через Write, запуск `python <файл>`. Скрипт с
   несколькими `assert count == 1` НЕ идемпотентен: упал на середине —
   остаток применять отдельным скриптом, а не перезапуском.
19. **KVM ≠ «просто быстрее».** IDE PIO под KVM МЕДЛЕННЕЕ, чем под TCG
   (каждое обращение к порту — выход в процесс QEMU): копия 145 МБ с проверкой
   не укладывалась в 300 с. Тесты, которые гасили QEMU по первой строке лога, а
   проверяли более поздние, под TCG «работали» из-за медленной загрузки.
   Пороги и ожидания проверять под обоими ускорителями.
20. **Курсор и скриншоты.** Аппаратного курсора нет ни в кадре, ни в
   `screendump` QEMU — тесты с планшетом проверяют счётчик `hwmoves` и
   неизменность кадра. Частичный вывод и курсор с сохранением фона оставляют
   «следы», которых не видно в serial: ловит только сравнение картинок
   (`tools/ppmdiff.c`). Смена минуты на часах — законный кадр сцены: проверки
   «нет кадров» должны его допускать.
21. **MSI-X до `sti`.** virtio-blk поднимается в `ata_init` — до LAPIC и до
   разрешения прерываний: пока IF=0, `virtq_wait_used` ждёт опросом; дошло ли
   прерывание, проверяют `*_irq_selftest` (иначе каждое ожидание кончалось бы
   по тику таймера). Адрес MSI — `lapic_get_id()`, не значение из ACPI.
   Вектор очереди пишется ДО `queue_enable`; размер очереди брать у устройства
   (уменьшенный мы ему не сообщаем — кольца разойдутся).
22. **tar из Git Bash теряет бит исполнения** (`build/luks_interop` → тест 48
   молча пропускает сверку с libcryptsetup): после переноса `chmod +x`.
23. **Сеть живёт в прерывании.** Любой статический буфер или таблица стека,
   которую трогает и главный поток, и обработчик (ACK на принятый сегмент
   уходит из прерывания посреди сборки запроса), — под `net_irq_save`. Ждать
   в обработчике нельзя вообще (`sti; hlt` внутри него — вложенный разбор
   кадров). Адрес интерфейсу — только из главного потока (`netcfg.c`).
24. **DHCP в настоящей сети ≠ slirp.** dnsmasq/ISC шлют OFFER/ACK не
   широковещательно, а на выдаваемый адрес по MAC: нужен флаг BROADCAST в
   запросе и приём UDP «не нам», пока своего адреса нет (`ipv4_rx`). В slirp
   этого не видно — он отвечает широковещательно.
25. **`gfx_fill_rect` не смешивает альфу** — пишет цвет как есть. «Затемнение»
   `GFX_ARGB(96,…)` было сплошной заливкой. Полупрозрачное — `fx_dim`,
   снимки — `fx_blit` (`ui_fx.c`).
26. **Конец перехода между проверкой и кадром.** Цикл WM спрашивает «идёт ли
   анимация» в начале прохода, а кадр рисуется на миллисекунды позже — переход
   может истечь в этот промежуток. Итог анимации подводит ОДНО место
   (`fx_panel_moving`/`open_done`), и после конца обязателен чистовой кадр.
27. **Анимация и чужая работа.** Всё, что блокирует BSP при открытии панели
   (MIND искал модель на диске 200 мс), превращает 20 кадров в 6: тяжёлое —
   после чистового кадра (`mind_widget_tick`). Окна под оверлеем во время
   перехода не перерисовывать (`compositor_set_frozen`).
28. **«Сайт не открылся» — сначала спросить узел.** `openssl s_client -tls1_2
   -cipher … -curves …` и `curl` с нашими заголовками (`User-Agent`,
   `Accept-Encoding`) с узла показывают, чья это беда. Проходит у них, а у
   нас нет — ошибка НАША, даже если выглядит как блокировка: bbc.com «молчал»
   (`tls-silent`), пока запись TLS уходила двумя сегментами; lite.cnn.com и
   ria.ru слали alert 40, пока в supported_groups не было secp384r1 (серверы
   на OpenSSL сверяют кривые цепочки сертификатов со списком клиента, хотя
   обмен потом идёт на x25519). Не проходит и у openssl (youtube.com по TLS
   1.2 — тишина) — фильтр на пути; обход блокировок не делаем. 403/401/439 —
   защита от ботов (Cloudflare, Qrator), не чинится.
29. **Защита от ботов смотрит на заголовки.** С `Accept-Encoding: identity`
   rbc.ru отдаёт пустое тело `application/octet-stream`, DuckDuckGo — код 202;
   с `gzip` оба отвечают нормально. Обрезанный адрес (путь был 512 байт) —
   это 400 на длинных перенаправлениях (ya.ru, mail.ru).
30. **Маршрутизатор без NAT выглядит как «интернета нет».** Шлюз пингуется и
   отвечает RST, наружу — тишина: пересылка включена, а `masquerade` нет, и
   ответы не находят дорогу назад. Смотреть `nft list ruleset` на шлюзе.
31. **Запись TLS — одним сегментом; фильтры на пути смотрят на первый
   сегмент.** `tcp_send` ждёт ACK каждого сегмента; заголовок записи (5 байт)
   отдельным вызовом = ClientHello, разрезанный после пятого байта. На аплинке
   узла (vlan10) такие соединения «замерзали»: bbc.com молчал, x.com вставал
   посреди рукопожатия, у wttr.in не доходили последние 1580 байт (10 с),
   debian.org грузился 8.6 с вместо 1 с — а curl получал всё за 0.4 с; в
   трафике гость—slirp потерь нет (вставало за slirp). После склейки
   `tools/pcap_tcp.py` на 66 сайтах находит паузы у youtube.com (молчит и для
   openssl) и — в одном прогоне из четырёх — снова у wttr.in (не пришли
   последние 3264 байта за 30 с; всё, что slirp отдал, гость подтвердил):
   значит, «хвост» wttr.in зависает где-то на пути узла, а не из-за разрезания. На аплинке ВМ 402 (198.51.100.x) всё наоборот: целый
   ClientHello и HTTP-запрос оставались без ответа, разрезанный проходил —
   это был zapret на шлюзе R1 (§2). Вывод:
   «на одной сети работает» ничего не говорит о другой; сеть проверять
   отдельно от стека — `http://portquiz.net:8080/` (порт вне фильтров) и
   приёмник сырых байт на узле показывают, целы ли наши пакеты.
32. **«Скрытое» бывает содержимым.** React/Next.js при потоковой отдаче кладёт
   страницу в `<div hidden id="S:0">` и переставляет скриптом; пропускать всё
   с `hidden` — получить пустую avito.ru. А `<noscript>` — наоборот, для нас.
33. **Время загрузки — тоже интерфейс тестов.** Хранилище из 150 корней
   проверялось целиком при каждом старте, дважды (самотест crypto и smoke
   47.3): экран входа появлялся через 13.8 с вместо 11.3 с (smoke 47.3 —
   1.66 с вместо 0.11 с). Тесты 45/41/42 запускают ОС прямо на dev-образе
   `build/disk-x86_64.img` и гасят QEMU через `kill -9` по таймеру — kill
   попал в момент, когда гость создавал файлы тем, FAT осталась
   несогласованной (`fsck.fat`: «Contains a free cluster»), и
   test_stage49_qwen3, читающий образ целиком (`mcopy -s`), падал на 20
   проверок («Fat problem while decoding», «Streamcache allocation problem»)
   — хотя MIND ни при чём. Теперь подпись проверяется у выборки корней
   (по одному на вид ключа / каждый 16-й), а `pve_regression.sh` после
   45/41/42 пересоздаёт dev-образ. Провал «MIND не открылась» — сначала
   `fsck.fat -n build/disk-x86_64.img`.
34. **VT-x: что где можно.** VMCS читается и пишется только на том ядре, где
   она текущая (VMPTRLD): BSP не может «заглянуть» в гостя — нужное снимает
   AP в структуру. После VM exit процессор ставит пределы GDT/IDT в 0xFFFF —
   возвращать `lgdt/lidt`. Поля хоста в VMCS — значения ЭТОГО ядра (TR, базы
   GDT/TSS); селектор TR хоста не может быть 0. HOST_RSP пишется лишь при
   смене стека, состояние блокировки прерываний читается только на HLT: под
   вложенным KVM узла (аппаратной теневой VMCS нет) каждый VMREAD/VMWRITE —
   выход в гипервизор узла, один выход гостя туда-обратно стоит ~17 мкс.
   Освобождать страницы VMCS/EPT — только после VMCLEAR и сброса кэша
   трансляций (`vmx_flush_guest_tlb`): новая ВМ может получить те же страницы.
35. **Гость верит CPUID, а не нам.** Лист, отданный «как есть», стоил падения
   Linux: KVM узла показывает в 0x80000008 EBX защиты Spectre «в стиле AMD»
   (IBPB/IBRS), гость на первой смене задачи написал в MSR 0x49 (без таблицы
   исключений) — `general protection fault` в ядре гостя, смерть его init.
   Правило: гость видит только листы и биты, про которые решено явно;
   незнакомый MSR — #GP, номера копятся в `hv_stats.unknown_list`. И второе:
   под вложенным KVM гость НЕ может сам измерить частоту TSC по 8254 (нужно
   >= 1000 чтений порта за 10 мс, а выход стоит 17 мкс) — отсюда kvmclock.
36. **Один тест — один экземпляр.** У тестов общий рабочий каталог в /tmp и
   общий порт монитора: второй запуск, начатый, пока первый досиживает
   таймауты, стирает его файлы и подключается к ЕГО QEMU. Перед повтором —
   `pgrep -f '[t]est_stageNN'` на узле (в тесте 50.2 стоит защита). Большие
   файлы из подкаталогов FAT в реестре VFS не числятся (`vfs_open` — «не
   найден»): читать `fat32_read_file_at`, как модели MIND.
37. **Замер в тесте не ставить после проверки с таймаутом.** «Загрузка гостя
   за минуту вместо двух секунд» (50.3) оказалась артефактом: проверка перед
   замером не нашла строку (склеилась с соседней) и прождала свои 60 с, а
   гость давно работал и простаивал. Сначала снять момент события
   (`wait_re_n`), потом проверять. И прежде чем искать причину в коде —
   повторить прогон: семь повторов показали 1.3–1.4 с.
38. **Имя объектного файла — по имени исходника без расширения.**
   `phantom_bios.c` и `phantom_bios.asm` дают один `phantom_bios.o`: второй
   затирает первый, и линковщик жалуется на «undefined reference» к функциям
   из файла, который точно в списке. У пар .c/.asm имена обязаны различаться
   (`phantom_bios_rom.asm`). В NASM глобальная метка посреди кода обрывает
   область локальных (`.again` после неё уже «не определена»).
39. **Запись слова в порт — это два байтовых порта.** Linux пишет в
   видеоконтроллер `outw` на 3D4h: младший байт — номер регистра, старший —
   значение для 3D5h. Обработчик, смотревший только на байт, тихо терял все
   значения: начало экрана оставалось нулём, и дамп показывал застывший кадр
   при живом госте. У парных портов «номер/значение» проверять размер
   обращения. И «журнал ядра гостя» — не доказательство: после старта init
   дистрибутив может его заглушить (Alpine: `quiet`) — проверять по тому, что
   видно изнутри гостя (/sys, /proc).
40. **IPI из обработчика прерывания.** Отправка IPI — две записи в LAPIC
   («кому», потом «что»). IPI шлёт и главный поток (пул MIND будит ядра), и
   обработчики на том же ядре (таймер будит спящего гостя, с 50.5 — клавиша).
   Обработчик, вклинившийся между двумя записями, оставляет в «кому» своего
   адресата, и сигнал главного потока уходит не тому ядру — тихо, раз в
   миллион. `lapic_send_ipi` теперь под `cli` (флаги возвращаются как были).
   Правило шире: всё, что делается «в две записи» и из потока, и из
   прерывания, — под запретом прерываний.
41. **Тест не должен подставлять умолчание, если не нашёл своё.** mcopy кладёт
   `core.iso` как имя 8.3 без длинного, и FAT отдаёт его ЗАГЛАВНЫМИ
   (`CORE.ISO`); тест искал строку списка по имени в нижнем регистре, не
   нашёл и «на всякий случай» щёлкнул по первой. Под именем Alpine запустился
   TinyCore, и девять проверок упали цепочкой, выглядя как сбой гостя.
   Имена файлов сравнивать без учёта регистра; не нашёл элемент — провал
   здесь же, отдельной проверкой.
42. **Спецификацию проверяет только официальный набор тестов.** Проверка
   типов WebAssembly написана по алгоритму из приложения к спецификации и с
   первого раза прошла 3321 случай из 3322; последний — `br_table` в
   недостижимом коде: «снять значения и вернуть» значит вернуть СНЯТОЕ («любой
   тип»), а не типы метки, иначе вторая метка с другим типом даёт ложный
   отказ. Свои тесты такого не ловят: их пишет тот же, кто понял текст
   неверно. Любую правку `wasm_valid.c` / `wasm_bin.c` — через `build/wasm_host
   spec build/wasm/spec.pack` (секунды), потом тот же пакет в ядре.
43. **Терминал сам пишет в serial.** Всё, что команда печатает в окно
   терминала (`fb_console_puts`), дублируется в serial. Функция, которая шлёт
   строку и в serial, и в переданный приёмник-терминал, кладёт её в журнал
   ДВАЖДЫ (тест со счётчиком строк ошибётся). Либо разные тексты в serial и в
   окно (как у phantom), либо при заданном приёмнике писать только в него
   (`rune.c: emit`).
44. **Тест, прошедший с первого раза, сам нуждается в проверке.** Весь набор
   исполнения WebAssembly (24 094 проверки) сошёлся с первого прогона — это
   либо удача, либо проверка ничего не проверяет. Ответ даёт только порча:
   тест 51.1 собирает три намеренно сломанных интерпретатора (сложение,
   граница памяти, сверка типа вызова) и требует, чтобы набор их провалил. И
   второе, найденное тут же: предел в тесте — по потребности, а не «с
   запасом». «Топлива» на вызов было 2^20 единиц; у сломанного сложения циклы
   тестов перестали кончаться, каждый сгорал до предела, и «провал» занял бы
   десятки минут (на узле упёрлось в таймаут). Набору хватает 256 единиц —
   стоит 4096.
45. **После heredoc-патча команда идёт дальше, даже если патч упал.**
   `python - <<EOF … EOF` + следующая строка: провалившийся assert (тот же
   §5.18 — heredoc съел `\\`) не остановил запуск регрессии на
   неисправленном файле. Патч — файлом, следующую команду цеплять через `&&`.
   И heredoc с кавычками (`<<'EOF'`) в этом инструменте тоже снимает один
   уровень слэшей: `"…\\n"` в тексте патча стало живым переводом строки в C
   (51.2, `env.c`). Никаких heredoc для текста со слэшами — только Write.
46. **Компилятор вправе выбросить саму проверку.** Программа «проси память,
   пока дают» (`env oom`) досчитала до 4096 МБ, не попросив ни байта: clang
   убрал и `malloc`, и `memset` в память, которую никто не читает. Проверочный
   код обязан ИСПОЛЬЗОВАТЬ то, что получает (блоки связаны в список и потом
   читаются). Признак — счётчики окружения (`WASM_STATS=1`: 5 обращений, 2
   страницы памяти у программы, «занявшей» гигабайты).
47. **Время программы съедает диск.** `files.wasm` (шесть десятков записей
   файла) в ядре шла 9.8 с при пределе 10 с — на IDE-диске теста под KVM (PIO,
   §8), на virtio-blk — 1.3 с. Предел времени `rune run` считает всё вместе:
   счёт, диск, сон. Тесты с записью на диск — на virtio-blk; а в serial вывод
   программы перемежается строками драйвера (`[fat32] mkdir …`) — сравнивать
   вывод, отфильтровав их.
48. **Вывод «после команды» мимо окна терминала.** `fb_console_puts` попадает
   в окно терминала только пока исполняется `shell_exec_line` (хуки ставятся
   на время команды); позже он рисует прямо в кадровый буфер — поверх окон.
   Всё, что печатает не команда, а кто-то после неё (программа на другом
   ядре), идёт своим путём (`term_job_out`). И кто владеет программой, должна
   помнить программа, а не терминал: с переменной в терминале отказ запуска
   («уже работает другая») сделал бы этот терминал «консолью» чужой программы,
   и набранные строки уходили бы ей.
49. **Проход цикла WM стоит миллисекунды.** Услуга программе на каждый проход
   (опрос портов, иногда кадр) — это ~2 мс на вызов: hello с шестью вызовами
   шла 29 мс. Услуги пачкой (до 2 мс за проход, с коротким ожиданием следующей
   просьбы) — 3 мс. Всё, что «другое ядро просит у BSP», мерить в вызовах в
   секунду, а не «работает».
50. **Образ ядра обязан кончаться ниже 8 МБ.** Ядро грузится с 1 МБ, его
   .bss — 5 МБ; с адреса 0x800000 лежит память прошивки OVMF, и UEFI-загрузчик
   не может разместить сегмент, который на неё заходит («ELF:
   AllocatePages(AllocateAddress) failed at paddr …», дальше — ничего, даже
   `[boot]`). Статический журнал окна Rune на 320 КБ сдвинул конец образа с
   0x7D9000 на 0x82BEC0 — и система перестала грузиться вовсе, а тест
   полчаса щёлкал по пустому экрану. Запаса сейчас 148 КБ: большие буферы —
   из кучи (`kmalloc`) или `pmm_alloc_big`, не статическими массивами.
   С 38.1.0: «память прошивки» — это ACPI NVS (карта — §6 «38.1»), запас
   3 МБ, а переход границы роняет СБОРКУ (`KERNEL_END_LIMIT` в Makefile).
   Проверка: `readelf -lW build/kernel-x86_64.bin | grep LOAD` — у последнего
   сегмента адрес + размер в памяти < 0x800000. Сборка и `nm` этого не видят.
51. **Super+Space открывает НОВЫЙ терминал каждый раз** (окно = 7.6 МБ кучи
   на 1080p; закрывается по Esc). Тест, открывавший терминал перед каждой
   проверкой, за десять минут исчерпал кучу ядра: `[heap] OOM: kmalloc(…)`,
   терминал перестал открываться, и набранные команды ушли в поле окна под
   ним — провалы выглядели как сбой приложения. В тестах терминал после
   команды закрывать; `[heap] OOM` в serial — первое, что искать при
   «странных» провалах. И второе оттуда же: тест, не дошедший до рабочего
   стола, обязан остановиться сам, а не гнать остальные проверки.
52. **Официальный набор проверяет язык, а не твой способ его исполнять.** JIT
   прошёл все 24 094 проверки с первого прогона; семь намеренных порч эмиттера
   набор поймал, а восьмую — нет: «значение локальной, взятое на стек до
   записи в неё, не спасается» давало 0 провалов. Набор не знает, что у нас
   `local.get` ничего не копирует, и случая под это в нём нет. На КАЖДУЮ
   оптимизацию, которой нет в спецификации (отложенное значение, слитое
   сравнение, кэш в регистре), нужен свой мутант и, если набор его не ловит,
   свой тест (тест 51.5: `old.wat` через wat2wasm). И соседнее: когда меняется
   то, чем исполняется код по умолчанию, старые проверки «тест умеет падать»
   молча перестают что-либо проверять — мутанты интерпретатора в тестах 51.1
   и 51.3 теперь запускаются с `WASM_JIT=0`.
53. **Младший байт rsi/rdi без REX — это dh/bh.** Запись байта `mov [m], sil`
   без приставки REX процессор читает как `mov [m], dh`. В шаблонах, где
   нужен байтовый регистр, значение держать в rax/rcx/rdx/rbx (или всегда
   ставить REX). Ловится только исполнением (i32.store8), не сборкой.

## 6. Статус этапов фазы 3
Готово: 37 Long Mode · 38 UEFI · 40 libc/POSIX · 41 virtio-gpu · 42 compositor
· 44 софт-OpenGL ES2 + FPU/SSE · 45 Amply + FAT32 LFN · 46 песочницы ·
47 crypto + TLS 1.2 (+47.7 ECDHE) · 48 FDE (LUKS1) · 48.1 браузер в интернет ·
49 AMPLIFY MIND (свой GGUF-инференс llama, SmolLM2-135M, пул AP, Ctrl+Space,
инструменты ОС) · **49.1 MIND на Qwen3-1.7B** (qwen3, токенизатор qwen2,
`<tool_call>`, RAM выше 4 ГБ, пакетный prefill, кэш system, потоковая
установка модели, Markdown/EN-RU в панели) · 57 установщик + live ISO
(частично: нет языка/раскладки, forge, self-hosted libc).
Долги 49.1 закрыты 2026-10-01: ISO пересобран на узле, полный тест Qwen3 и
регрессия 49/57/57-ahci/48/45 пройдены без провалов (цифры — §7).
**41.2 virtio-драйверы + плавный UI** (НЕ этап 50) — готово 2026-10-01,
регрессия на узле пройдена без провалов (§7), ждёт подтверждения
пользователя. Сделано: A счётчики кадра и замеры; B кэш
слоя рабочего стола, кадр «только курсор», частичный вывод; C общий слой
virtio с MSI-X + таймер LAPIC 1000 Гц; D virtio-gpu по прерыванию,
аппаратный курсор, смена режима; E virtio-input; F virtio-blk; G virtio-net.
Попутно исправлены давние ошибки: перенос initrd из кучи ядра (§5.16),
единая сборка ISO (§5.17), DHCP (xid генерировался ПОСЛЕ сборки пакета —
адрес не приходил никогда), гонка `tcp_recv` с приёмом в прерывании,
лишние чтения порта на сектор в IDE PIO (под KVM — выходы в QEMU).
Цифры (KVM, 1920x1080, 4 ядра; «до» -> «после»), ВМ 402:
  простой на столе   9 кадров/с, 15 % BSP  ->  0 кадров/с, 0.7 % (2 % ядра хоста)
  мышь на столе      ~49 кадров/с, кадр 17 мс, 85 %  ->  ~80 (столько событий
                     шлёт тест), кадр 0.03 мс, 2 %
  мышь + терминал    ~40 кадров/с, 87 %  ->  ~88, 10 %
  терминал в простое 9 кадров/с, 19 мс, 18 %  ->  10 кадров/с, 7.7 мс, 8.7 %
  перетаскивание окна (узел): 68-70 кадров/с, кадр 2.5 мс
  диск, чтение 1 МиБ-кусками (узел): IDE 3, AHCI 369, virtio-blk 1224 МиБ/с;
  Qwen3 1.8 ГБ с virtio-blk на ВМ 402 — 5.7 с (с AHCI было 15 с)
  сеть, 64 x 100 КБ по HTTP (узел, slirp): e1000 169 КиБ/с, virtio-net 27 МиБ/с
Не сделано из исходного списка 41.2: SSE2-блиты и кэш глифов (цель достигнута
без них), INTx, virtio-scsi (отложен по согласованию), слежение за размером
окна хоста.
**48.2 «Сеть, браузер, анимации»** (НЕ этап 50) — готово 2026-10-01, тест
95/95 и регрессия на узле без провалов (§7), проверено на ВМ 402; ждёт
подтверждения пользователя. Сделано:
  сеть — `netcfg` + network.conf (auto/dhcp/static, запасная статика),
  DHCP-автомат (повторы, продление аренды, работа с настоящими серверами),
  TCP без гонок с прерыванием, RST/FIN по правилам, потоковый HTTP/HTTPS до
  2 МБ, причины отказа, DNS с пределом времени;
  Mesh — кликабельные настройки сети; браузер — workspace Pearl с типографикой,
  историей, страницами ошибок, загрузкой без заморозки интерфейса;
  анимации — окна, MIND, Beacon, Constellation, шторка, вход на стол.
Найдено и исправлено попутно: DHCP не принимал нешироковещательный ответ
(поэтому «на vmbr2 DHCP не отвечает» — отвечал всегда); общий буфер сегмента
TCP портился подтверждением из прерывания; повтор FIN сдвигал rcv_nxt дважды;
`sti; hlt` внутри обработчика (ожидание ARP); TLS ждал 10 с после FIN сервера;
браузер ждал клавиш PgUp/PgDn, которых клавиатура не выдавала; «затемнение»
оверлеев было сплошной заливкой; MIND при открытии блокировал BSP на 200 мс.
Цифры (узел, KVM, 1920x1080, 4 ядра): анимации 57–63 кадров/с (окна 21 кадр
за 340 мс, оверлеи 18–20 за 300–330 мс; на ВМ 402 — 56–63, замедленные в 10
раз — ровно 61 кадр/с три секунды подряд); страница 636 КБ — 33 мс,
побайтно (FNV) = файлу на сервере; 64 x 100 КБ по HTTP — 28 МиБ/с (как до
48.2, теперь с вежливым закрытием); кадры во время загрузки — ~30/с.
ВМ 402: DHCP 10.0.2.10 от Test-RTR, продление аренды в T1 — работает;
статика/авто через Mesh — работает.
**48.2, продолжение «настоящие сайты» (2026-10-02)** — задача пользователя:
«из браузера я должен грузиться на сайты, а не на те, что ты создал».
  ВМ 402: на Test-RTR не было NAT (§5.30) — добавлен; затем сайты не
  открывались из-за zapret на шлюзе R1 (§2) — с 03.10 этот хост из zapret
  исключён. Итоговое ядро на ВМ 402, настоящая сеть: 8 из 9 адресов с HTTP
  200 (ru.wikipedia.org/wiki/Linux 622 КБ за 1.1 с, lite.cnn.com, поиск
  «linux kernel», habr.com, github.com/torvalds/linux, lenta.ru, mail.ru —
  через meta refresh, sberbank.ru — «HTTPS ?»); bbc.com там не открывается —
  заблокирован в регионе (DNS отдаёт адрес провайдера, 443 молчит, порт 80
  отвечает 400), пользователь подтвердил, что так и должно быть.
  Последняя правка (03.10, после этих проверок): старт Википедии с тела
  статьи и пробел между ссылками вплотную (`app_browser.c`). Сборка с ней
  залита на ВМ 402 (`amplify-dev.iso`), на ПК собирается без ошибок; сетевой
  прогон на узле (`build/reg/w14.log`, `build/reg/net-suite-test.log`,
  `build/screens-sites/summary.txt`) был запущен, но его итог ИИ НЕ смотрел —
  пользователь сказал, что посмотрит сам. Полная регрессия на этой последней
  правке не повторялась (она шла на сборке до неё, §7).
  Сеть: gzip/zlib/deflate (`inflate.c`), cookies (`cookies.c`), ICMP
  unreachable, адреса до 2 КБ, заголовки 32 КБ, 1xx, ожидание данных 30 с.
  TLS: +ChaCha20-Poly1305, +AES-256-GCM-SHA384, x25519, P-384 (подписи и
  ECDH), EMS, запись и полёт одним сегментом, ClientHello 0x0301, весь набор
  корней Mozilla (150 вместо 7) — «HTTPS» вместо «HTTPS ?» почти везде.
  Браузер: поиск из адресной строки, поле поиска на странице, meta refresh,
  скрытые элементы, noscript, старт с основного содержимого, HTML по
  содержимому, 4 МБ.
  Обзор 66 адресов (`scripts/sites.txt`, узел, slirp): было 29 из 35, стало
  58–59 из 66 с HTTP 200. Не открываются: youtube.com (по TLS 1.2 сервер
  замолкает после ClientKeyExchange и у openssl — фильтр на пути; по TLS 1.3
  открылся бы, но его у нас нет), защита от ботов — stackoverflow, wiki.osdev,
  reuters, tass, ozon, amazon, временами avito (403/401/439/202). Открываются,
  но без JavaScript пусты: reddit, gazeta.ru, dzen.ru (сам; через yandex.ru —
  96 блоков), old.reddit. sberbank.ru — сертификат от корня Минцифры (нет в
  наборе Mozilla): открывается с «HTTPS ?».
Пропущен: **43** Neural Icon Forge — пользователь сказал (08.10) его не
учитывать: значки в системе уже есть; новый значок приложения ИИ рисует сам в
стиле остальных (образец — `tools/gen_rune_icon.py`).
**50 Hypervisor VT-x «phantom»** — в работе с 2026-10-05 (добро дано).
Решения пользователя: гость грузится через BIOS (вариант B: реальный режим +
прошивка, штатный загрузчик с ISO), а не прямой загрузкой ядра; сначала всё
через serial, окно — последним; долги из архива (netif/loopback для net-ns,
сеть гостя, VirGL/3D) в этап НЕ входят; CR4.VMXE ставится в C на нужном ядре
по CPUID, а не в трёх точках входа (отступление от §5.2: VMXE без VMX = #GP,
то есть смерть загрузки под TCG); команда `phantom`, алиас `vm`.
Ещё решено пользователем (05.10): vCPU привязан к одному AP и делит его с
пулом; BIOS — свой «паравиртуальный» (вектора INT — заглушки, службы на C в
хосте), не SeaBIOS; в 50.2 Linux грузится напрямую — только как тестовый
стенд устройств, пользователю этот режим не показывается.
Подэтапы (каждый — тест, отчёт, вопрос): **50.0** разведка VMX — готово
2026-10-05, тест 25/25, проверено и на ВМ 402 (VMX виден, 4/4, READY) ·
**50.1** VMXON на всех ядрах, GDT/TSS на AP, VMCS, EPT, vCPU-«жилец», гость
«реальный -> защищённый -> длинный» — готово 2026-10-05, тест 54/54 (§7),
пользователь дал добро идти дальше · **50.2** PIC, PIT, UART, CMOS, доставка прерываний, сон по
HLT, часы kvmclock; ядро Linux 6.18 (TinyCore) до оболочки и до конца своего
init — готово 2026-10-05, тест 31/31, пользователь дал добро идти дальше ·
**50.3** свой BIOS (таблица векторов, область данных, INT 08h/10h/13h El
Torito/15h/16h/1Ah) и загрузка неизменённого ISO его же isolinux, `phantom
run <iso>` — готово 2026-10-05, тест 33/33, пользователь дал добро идти дальше
· **50.4** шина PCI и диск virtio-blk с тем же ISO; Alpine 3.24.2 с
неизменённого образа до `login:` (на порту и на текстовом экране) — готово
2026-10-06, тест 29/29, пользователь дал добро идти дальше · **50.5**
приложение Phantom (окно гостя в стиле Pearl: библиотека образов и консоль),
клавиатура гостя (8042 + INT 09h BIOS), чтение образа без заморозки
интерфейса, образы гостей на установочном ISO и в установщике — готово
2026-10-06, тест 74/74 (§7), пользователь дал добро идти дальше (07.10).
Решения пользователя по
50.5 (06.10): читать ISO не в основном цикле; положить образы гостей на
стенд; гость — в ОТДЕЛЬНОМ ПРИЛОЖЕНИИ «по дизайну ОС»; на стенд — полный ISO
вместо основного, со всеми новшествами и ИИ, ИИ проверить.
«Как KVM» взято: слои, VMX на всех ядрах, контракт вида KVM_RUN, «пинок»
vCPU через IPI. Не взято: vCPU как поток планировщика — планировщик у нас
кооперативный и только на BSP (SMP-планировщик — отдельный этап).
Цифры 50.1 (узел, вложенный KVM, 4 ядра): выход гостя туда-обратно (OUT) —
17–18 мкс (~57 тыс. выходов/с); гость «три режима» — 18 выходов, 0.7 мс;
создать-запустить-снять ВМ 4 МБ — 2.2 мс; снять зациклившегося гостя — 20–30
мкс; 24 кванта за 50 мс; при горячем пуле гость получает 9 квантов за 80 мс
(~20 % ядра), пул работает на всех 4 ядрах; с гостем в фоне WM даёт те же
~10 кадров/с при открытом терминале, BSP занят на 13 %; smoke 50.1 — 18 мс
загрузки.
Цифры 50.2 (узел, вложенный KVM): ядро Linux до оболочки — см. §7; время
гостя против хоста: 11.36 с за 11.37 с (0.1 %); простой в оболочке — гость
спит, 0 % ядра; после полного init TinyCore — 4 %; загрузка до оболочки —
~52 тыс. выходов, из них 48 тыс. — порты (консоль: два выхода на символ).
Цифры 50.3 (узел, вложенный KVM): TinyCore CorePure64-17.1 с ISO: от Enter
в приглашении `boot:` до оболочки — 1.3–1.4 с (восемь прогонов подряд);
isolinux 4.05 делает за загрузку ~10 тыс. вызовов BIOS и 403 чтения диска
(12 652 сектора = 24.7 МБ); чтение ISO 26 МБ с IDE-диска теста + создание ВМ
— ~13 с (BSP занят); загрузка по умолчанию (просто Enter) доходит до
`tc@box:~$` на текстовом экране гостя; в простое гость занимает 0 % ядра.
Цифры 50.4 (узел, вложенный KVM): Alpine от старта BIOS до `login:` на
порту — 3.3–3.6 с (загрузчик ждёт 1 с, ядро + initramfs + пакеты с диска —
остальное); диск к моменту входа отдал ~190 прерываний; простой — 0 % ядра;
чтение ISO 66 МБ с IDE-диска теста — ~31 с (BSP занят, кадров нет).
Цифры 50.5 (узел, вложенный KVM, 1920x1080): экран гостя 1280x800
(знакоместо 16x32); образ 26 МБ с IDE-диска теста читается 10.3 с, 66 МБ —
27.4 с, с CD (ATAPI) 26 МБ — 4.4 с; кадров при чтении — не меньше 8 в
секунду (раньше — ни одного); Alpine: вход root с клавиатуры окна на tty1;
клавиатура гостя — IRQ1 через 8259 (254 прерывания за сеанс TinyCore); MIND
(встроенная модель) отвечает при работающем госте, ids = эталон; простой
гостя — 0 % ядра.
Этап 50 пройден по всем подэтапам.
**51 WebAssembly** — начат 2026-10-07 (добро дано; по просьбе пользователя
архивная памятка перечитана целиком). Строка плана из архива: «портирование
wasm3 (или свой интерпретатор), WASI поверх aos_*, песочница». Аудит: в
дереве про wasm нет ничего; из инструментов на ПК есть только Node.js
(Windows; его движок годится эталоном для сверки), компилятора в wasm нет ни
в WSL, ни в Windows, ни на узле — готовые модули взять неоткуда, пока
пользователь не разрешит скачать инструменты. Старые обещания «на этап 51»
из архива (zstd для Amply, полный VFS/bind-mount, fork/exec) в этап НЕ
входят, пока пользователь не скажет иначе.
Решения пользователя (07.10): движок — СВОЙ интерпретатор (не wasm3); качать
разрешено clang/lld/wasi-libc/wabt из Debian на узел и официальный набор
тестов с GitHub; приложение — консоль программы в окне Pearl (как Phantom;
своих окон у wasm-программ нет); после интерпретатора — подэтап с JIT в
x86-64. Объём языка: WebAssembly 2.0 без SIMD (multi-value, sign-ext,
nontrapping-fptoint, bulk-memory, reference-types) — то, что выдаёт clang 19.
Подэтапы: **51.0** формат и проверка модуля · **51.1** интерпретатор ·
**51.2** WASI preview1 · **51.3** запуск на отдельном ядре, лимиты, песочница
· **51.4** приложение · **51.5** JIT.
**51.0** — готово 2026-10-07, тест 30/30 (§7), пользователь дал добро идти
дальше (07.10). Сделано в 51.0:
разбор двоичного модуля и проверка тел функций (весь объём 2.0 без SIMD),
команды `rune info|selftest|spec`, smoke 51, хост-сборка движка, инструменты
на узле, упаковщик официальных тестов, проверочные программы на C.
Цифры 51.0: официальный набор (wg-2.0, 90 файлов): верных модулей принято
1126 из 1126, неверных отклонено 1477 из 1477, нераспознаваемых двоичных —
719 из 719 (ещё 581 случай — про текстовый формат, не наш слой; 24 115
проверок исполнения ждут 51.1); в ядре тот же пакет даёт те же цифры за
13 мс; 6 модулей от clang 19 (hello и calc: -O0, -O2, -O2 со всеми
расширениями) приняты, счётчики секций совпали с wasm-objdump; 40 000
испорченных модулей под AddressSanitizer — ни одного чтения за границей;
smoke 51 — 9 мс загрузки; ядро выросло на ~56 КБ.
**51.1** (интерпретатор) — готово 2026-10-07, тест 45/45 (§7), пользователь
дал добро идти дальше (07.10) и разрешил НЕ гонять всю регрессию после
каждого подэтапа — только тесты, которые правка могла задеть (в отчёте
называть, что не запускалось). Сделано: внутренний код функций (его пишет проверка),
хранилище и экземпляры со связыванием импортов, интерпретатор, функции хоста,
пределы («топливо», флаг остановки, предел времени от таймера ядра),
исполнение официального набора в ядре и на хосте, `rune call`, smoke 51.1,
проверочный модуль без окружения (`pure.c`), фаззинг с исполнением.
Цифры 51.1: официальный набор с исполнением — возвраты 21 453 из 21 453,
ловушки 2354 из 2354, исчерпание стека 15 из 15, несвязываемые модули 83 из
83, прерванный запуск 34 из 34, вызовы без проверки 155 из 155, пропущено 0;
на хосте (узел) весь набор — 149 мс, ВНУТРИ ЯДРА — 164 мс, итоговые строки
совпадают; три сломанных интерпретатора набор ловит (484, 76 и 12 провалов);
`pure.c` — 18 функций x 3 сборки clang (-O0, -O2, -O2 со всеми расширениями)
= результаты gcc-сборки хоста, дробные по битам, и на хосте, и в ядре;
фаззинг с исполнением под ASan+UBSan — 1 032 600 испорченных модулей из 3442
образцов, 92 211 приняты проверкой, 88 957 запущены, 122 122 вызова (24 157
ловушек) — ни одного срабатывания. Скорость (узел, вложенный KVM): fib(32),
7 млн вызовов — 426 мс в ядре, 300 мс на хосте; вечный цикл — ~58 млн витков
в секунду (~330 млн слов внутреннего кода); родной код быстрее в десятки раз
(все 18 функций: 5 мс против ~120 мс) — это цена интерпретатора, JIT — 51.5.
Предел времени 400 мс сработал на 400-й мс. smoke 51.1 — 19 мс загрузки (5 из
них — намеренное ожидание таймера). Движок в ядре — ~107 КБ кода.
**51.2** (WASI preview1) — готово 2026-10-07, тест 52/52 (§7), пользователь
дал добро идти дальше (07.10). Сделано: слой WASI (45 функций, описатели, граница каталога,
файлы-копии в памяти), порт на том FAT и окно терминала, `rune run`, smoke
51.2, `wasm_host run`, проверочные программы files/env/jail, фаззинг с
настоящим окружением. Выбор без вопроса пользователю (можно пересмотреть в
51.3, где песочница): программа по умолчанию видит `/disk/nexus` (не весь
диск: в корне лежит `users.txt`), другой каталог — `-d`.
Цифры 51.2 (узел, вложенный KVM): hello, calc, files (93 строки работы с
файлами и каталогами), env (аргументы, часы, сон, случайные байты, код
возврата) — вывод каждой из трёх сборок clang (-O2, -O0, -O2 со всеми
расширениями) побайтно равен выводу того же исходника под gcc, и на хосте, и
В ЯДРЕ; jail — 14 попыток выйти из каталога из 14 отбиты, в каталоге «только
чтение» 6 изменений из 6 отклонены, снаружи ничего не появилось (проверено и
на хосте, и на томе FAT после ядра); фаззинг под ASan+UBSan — 12 000
испорченных программ с настоящим окружением, 7361 запущена (2446 дошли до
своего exit, 3642 остановлены по «топливу»), ни срабатывания, ни файла вне
каталога. В ядре: calc — 397 мс (23 обращения к окружению), files — 1.3 с на
virtio-blk (497 обращений; на IDE-диске под KVM — 9.8 с), программа,
просящая память без конца, получает отказ на 63-м мегабайте и доживает до
конца, вечный цикл снят на 500-й мс при пределе 500, сон 60 мс = 61 мс,
русский текст программы доходит до окна. smoke 51.2 — 5 мс загрузки. fsck
тома после всех программ: цепочки и каталоги целы (замечание одно — счётчик
свободных кластеров в FSINFO: драйвер FAT его не ведёт, так было и раньше).
**51.3** (своё ядро, пределы, правила доступа) — готово 2026-10-07, тест
59/59 (§7); пользователь попросил промпт для перехода к следующему подэтапу
в новом чате (07.10) — то есть дал добро идти к 51.4. Сделано: пауза и продолжение в
интерпретаторе, «услуги» привилегированных команд, WASI без блокировок,
программа-«жилец» AP с обслуживанием из цикла WM, пределы времени счёта и
памяти, правила доступа к каталогам, терминал как консоль программы (ввод с
клавиатуры, Ctrl+C, Ctrl+D), фон (`-b`), `rune status/stop/type/eof`, smoke
51.3. Решено без вопроса пользователю (можно пересмотреть): предела времени
по умолчанию нет, когда у программы своё ядро (останавливает Ctrl+C / `rune
stop`); корень тома и `/disk/Sanctum` программе не даются никогда;
одновременно — одна программа; файл настроек пределов не делался — только
ключи команды.
Цифры 51.3 (узел, вложенный KVM, 4 ядра): официальный набор в пошаговом
режиме (пауза на каждом витке, каждая услуга — через `wasm_service`) — те же
итоги, 161 мс; два испорченных интерпретатора обычный прогон проходят, а
пошаговый проваливают (155 и 107 провалов); files в пошаговом режиме — 470
754 паузы и 497 услуг, вывод как у gcc. В ядре: программа, считающая без
конца, получает 99.9 % своего ядра (6672 мс счёта за 6680 мс), главное ядро
при этом занято на 14 %, кадров — не меньше 9 в секунду, терминал отвечает,
`phantom selftest` рядом проходит 9 из 9; остановка — сразу; предел счёта
300 мс срабатывает на 300-й мс счёта; с `-m 8` программе отказывают на 7-м
мегабайте; ждущая ввода программа за 4.7 с не считала ни миллисекунды;
сон 1500 мс — 1513 мс по часам и 0 мс счёта. Цена услуги: hello (6
обращений) — 3 мс, calc (23) — 452 мс при 353 мс счёта, files (497) — 1.7 с
при 1.1 с счёта (до обслуживания пачкой: 29 мс, 526 мс, 2.1 с). smoke 51.3 —
6 мс загрузки.
**51.4** (приложение Rune) — готово 2026-10-08, тест 96/96 (§7), пользователь
дал добро идти к 51.5 (08.10). Решения пользователя (07–08.10): имя — Rune;
значок ИИ рисует сам; программы — встроенные в initrd + каталог
`/disk/vault/runes`; в окне — аргументы, каталог и доступ, предел памяти
(предела времени в окне нет); одновременно — одна программа. Сделано: окно
(библиотека и консоль с прокруткой и вводом), журнал, переживающий окно,
`rune_query/rune_last_error/rune_programs`, описание программы в секции
модуля, четыре встроенные программы и их сборка в дерево, значок, команды
`rune list | screen | log`. Решено без вопроса пользователю (можно
пересмотреть): программа из терминала в окне показывается без вывода; итог
программы остаётся на экране, пока его не закроют («К библиотеке»); строки
`[rune] …` движка в консоль окна не идут.
Цифры 51.4 (узел, вложенный KVM, 4 ядра, 1920x1080): консоль 165x40
знакомест; встроенные программы — 31, 32, 41 и 31 КБ (вчетвером 136 КБ в
initrd); вывод в окне каждой из них и проверочной files (93 строки) побайтно
равен выводу того же исходника под gcc (у диалогов — с эхом набранного);
строка в 183 знака перенесена на две экранные без потерь; решето до 30 млн —
5.1 с счёта, 29 МБ памяти (459 страниц), с пределом 8 МБ программа получает
отказ и сообщает о нём сама; пока программа считает без конца (10.1 с счёта
за 10.1 с), кадров не меньше 9 в секунду, главное ядро занято не больше чем
на 37 %; кадр окна — в среднем 12 мс (худшая секунда — 27 мс).
**51.5** (JIT) — готово 2026-10-08, тест 55/55 (§7), пользователь принял
(08.10). Сделано: `wasm_jit.c` (эмиттер x86-64 из внутреннего кода,
«ленивая» и «виртуальная» вершина стека, слияние сравнения с переходом),
водитель с выходами к интерпретатору на одну команду, режим «один шаг» в
интерпретаторе, точки входа для продолжения после паузы, выключатель
(`rune jit`, `WASM_JIT`), smoke 51.5, показ режима в заголовке запуска и в
окне Rune. Решено без вопроса пользователю (можно пересмотреть): JIT включён
по умолчанию; редкие команды остаются за интерпретатором; оптимизаций сверх
шаблонов (распределение регистров, встраивание) нет.
Цифры 51.5 (узел, вложенный KVM): официальный набор машинным кодом — те же
итоги, что у интерпретатора (возвраты 21 453, ловушки 2354, …, провалов 0), в
обычном и в пошаговом режиме, на хосте и ВНУТРИ ЯДРА (148 мс против 202 мс у
интерпретатора; набор — короткие вызовы, выигрыш мал); 8 испорченных
эмиттеров из 8 пойманы (7 — набором, 1 — своим модулем); фаззинг под
ASan+UBSan — 1 033 200 испорченных модулей, 89 910 запущены машинным кодом,
ни срабатывания. Скорость, JIT / интерпретатор / gcc: fib(32) — 67 / 667 мс
(в ядре 68 / 637); решето до 30 млн — 1.07 / 7.2 / 0.34 с (в ядре счёт 1.9 /
7.5 с); calc — 49 / 560 / 9 мс (в ядре счёт 55 / 557 мс). То есть в 7–11 раз
быстрее интерпретатора и в 3–7 раз медленнее родного кода. Машинного кода на
программу WASI — 116–167 КБ (вчетверо больше самого модуля). smoke 51.5 — 45 мс загрузки (из них 36 — намеренный
прогон того же цикла интерпретатором для сверки). Образ ядра вырос на 33 КБ:
до границы 8 МБ (§5.50) осталось 110 КБ.
Этап 51 пройден по всем подэтапам.
**38.1 «Ядро в верхней половине адресов»** — решение пользователя (08.10):
до этапа 52 убрать границу 8 МБ (§5.50) полностью — вариант D из обсуждения:
ядро слинковано на верхние виртуальные адреса, физически грузится туда, где
прошивка даёт место, таблицы страниц строит загрузчик/вход. Сначала
пользователь выбрал вариант C (перемещения при тождественном отображении),
затем заменил его на D. Начат 2026-10-08. Копия исходников до правок —
`build/backup_pre_38_1/` (kernel, uefi, линкер-скрипт, Makefile; НЕ трогать).
Решения пользователя (08.10): путь (б) — physmap и `phys_to_virt`/
`virt_to_phys` во всех подсистемах, тождественное отображение в конце снять;
ветку GRUB-EFI объявить неподдерживаемой (образ > 7 МБ по LMA 1 МБ ляжет на
ACPI NVS; наш ISO её и не использует, §5.17) и убрать из теста 57; NX/W^X —
в этом же этапе; на ВМ 402 — только в самом конце; «полная регрессия» для
38.1 = `pve_regression.sh` + тесты вне её, которые правка задевает
(`scripts/pve_extra38.sh`, одним запуском — `scripts/pve_full38.sh`).
Раскладка (цель): PML4[0] пусто; PML4[1] — программы; PML4[256]
0xFFFF800000000000 — physmap всей RAM; PML4[384] 0xFFFFC00000000000 — окно
MMIO; PML4[511] 0xFFFFFFFF80000000 — образ ядра; куча — кусок от pmm ниже
4 ГБ через physmap (не «16..144 МБ»). Таблицы строит входная заглушка
`boot.asm` (обе точки входа), загрузчик кладёт образ одним куском ниже 4 ГБ
с выравниванием 2 МБ; таблица перемещений не нужна. Откат — флаг сборки
`HIGHER_HALF=0` до приёмки 38.1.4.
Подэтапы: **38.1.0** большие массивы из .bss + проверка конца образа ·
**38.1.1** ядро наверху, тождественное отображение остаётся (стена 8 МБ
снята здесь) · **38.1.2** `phys_to_virt`/`virt_to_phys`/`mmio_map` во всех
подсистемах при базе physmap = 0 · **38.1.3** настоящий physmap, куча от pmm,
окно MMIO, сборка `NOIDENT=1` (PML4[0] снят — ловит пропущенное) ·
**38.1.4** тождественное отображение снято по умолчанию · **38.1.5** NX/W^X,
исполняемая область JIT.
Карта памяти OVMF (снята 08.10 на узле: pc 1 и 8 ГБ, q35 4 ГБ; одинакова):
0x100000–0x800000 свободно; **0x800000–0x808000, 0x80B000–0x80C000,
0x810000–0x900000 — ACPI NVS** (память прошивки навсегда: это и есть «стена»);
0x900000–0x1600000 — данные служб загрузки (после ExitBootServices свободны;
начало кучи попадает сюда); дальше до верха RAM ниже 4 ГБ свободно одним
куском. Снять заново: `bash scripts/memmap_dump.sh [RAM] [pc|q35]` (кладёт на
копию ESP `\memmap.flag` — загрузчик печатает карту до своих выделений).
Объём по коду (поиск 08.10, только собираемое): адрес -> указатель — 78 мест
в 31 файле, указатель -> адрес — ~60 в 26, вызовов `pmm_alloc_*` — ~38 в 17;
поиск по приведениям не видит кучу, MMIO ниже 4 ГБ (LAPIC, IOAPIC, e1000,
кадровый буфер) и буферы DMA из kmalloc/статические.
**38.1.0** — готово 2026-10-08, полная регрессия без провалов (§7), ждёт
подтверждения пользователя. Из .bss вынесены: таблица счётчиков pmm (1 МБ;
берётся в `pmm_init` у самого pmm — `early_take`, адрес в serial: `[pmm]
refcount table @`), буфер `diskbench` (1 МБ), буфер заголовков установщика
(288 КБ), материал слота LUKS (250 КБ), три контекста TLS (351 КБ) — всё из
кучи при первой надобности. Конец образа 0x7E4FA0 -> 0x508BC0, запас до стены
110 КБ -> 3037 КБ. Makefile роняет сборку, если конец образа >=
`KERNEL_END_LIMIT` (0x800000; 0 — не проверять). Не тронуты (остались в
.bss): `virtio_net` 394 КБ, `netif` 128 КБ, `mind_widget` 2x64 КБ, `amply`
100 КБ и мелочь — до 38.1.1 они не мешают.
Дальше: 38.1 · 52 embersd-журнал + crash-dump ·
53 Unicode/локали/IME · 54 Bluetooth/WiFi · 55 Doom/Quake · 56 OTA + A/B ·
58 Release 1.0.

## 7. Тесты (headless QEMU, парсят serial + скриншоты)
Прогон 2026-10-01 на сборке 48.2 — узел Proxmox, KVM, сборка там же; весь
набор — `scripts/pve_regression.sh` (итог `build/reg/summary.txt`), логи на
ПК — `build/reg-pve/48_2-final/`. Всё без провалов: 45 — 25/25 · 41 — 9 OK ·
42 — 18/18 · 49_qwen3 — 51/51 (7 мин под KVM, под TCG было 44) · 49_mind —
35/35 · 57 (IDE) — 27/27 · 57_ahci — 19/19 · 48 (IDE) — 25/25 · 57 на
virtio-blk — 27/27 · 48 на virtio-blk — 25/25 · 41_2_ui — 26/26 ·
41_2_input — 21/21 · 41_2_net — 14/14 (с DNS/TLS/HTTPS к настоящему сайту) ·
48_2_net — 95/95.
**Прогон 2026-10-03 («настоящие сайты», итоговая сборка), узел, KVM:** 45 —
25/25 · 41 — 9 OK · 42 — 18/18 · 57 (IDE) — 27/27 · 57_ahci — 19/19 · 48 (IDE)
— 25/25 · 49_qwen3 — 51/51 (7 мин) · 49_mind — 35/35 · 41_2_ui — 26/26 ·
41_2_input — 21/21 · 41_2_net — 14/14 · 57 и 48 на virtio-blk — 27/27 и
25/25 · 48_2_net — 116/117. Единственный провал — проверка частоты кадров:
в обеих регрессиях «Constellation close: >= 55 frames/s (got 54)» (анимация
200 мс: 11 кадров вместо 12, целое деление 54.7 -> 54); отдельным запуском
тот же тест трижды дал 117/117 (12 кадров, 58 кадров/с), в четвёртый раз
(параллельно на узле шла сборка dev-iso) — 116/117 на «desktop entry: 54».
Проверки >= 55 стоят на самой границе: один пропущенный кадр = провал;
причина пропуска кадра не найдена. Обзор сайтов на той же сборке: 58 из 66 с
HTTP 200. После регрессии была ещё одна правка браузера (§6) — её прогон
`w14` не разобран.
**Прогон 2026-10-06 (сборка 50.4), узел, KVM:** 45 — 25/25 · 41 — 9 OK ·
42 — 18/18 · 57 (IDE) — 27/27 · 57_ahci — 19/19 · 48 (IDE) — 25/25 ·
49_qwen3 — 51/51 · 49_mind — 35/35 · 41_2_ui — 26/26 · 41_2_input — 21/21 ·
41_2_net — 14/14 · 48_2_net — 117/117 · 50_0_probe — 25/25 · 50_1_vmx —
54/54 · 50_2_linux — 31/31 · 50_4_alpine — 29/29 · 57 и 48 на virtio-blk —
27/27 и 25/25 · 50_3_bios — 32/33 в самой регрессии и 33/33 отдельным
запуском после правки ТЕСТА: он ждал в карте E820 гостя верх памяти 256 МБ, а
с 50.4 `phantom run` даёт гостю 512 МБ (ядро не менялось). На сборках 50.0–
50.3 (05.10) регрессия проходила без провалов. Проверки частоты кадров
48_2_net прошли все пять раз; причина прежних провалов на границе так и не
найдена. Под KVM узла smoke 50.1 (настоящий гость на AP) исполняется при
КАЖДОЙ загрузке каждого теста.
**Прогон 2026-10-06 (сборка 50.5), узел, KVM — без провалов:** 45 — 25/25 ·
41 — 9 OK · 42 — 18/18 · 57 (IDE) — 28/28 · 57_ahci — 19/19 · 48 (IDE) —
25/25 · 49_qwen3 — 51/51 · 49_mind — 35/35 · 41_2_ui — 26/26 · 41_2_input —
21/21 · 41_2_net — 14/14 · 48_2_net — 117/117 · 50_0_probe — 25/25 ·
50_1_vmx — 54/54 · 50_2_linux — 31/31 · 50_3_bios — 33/33 · 50_4_alpine —
29/29 · 50_5_app — 74/74 · 57 на virtio-blk — 29/29 · 48 на virtio-blk —
25/25. У теста 57 на одну-две проверки больше прежнего (образы гостей).
ISO этой сборки (`build/myos-x86_64.iso`, sha256 4cb2b1e4…aaee0f — тот же
файл, что проходил 49_qwen3 и 57) выгружен на стенд как
`amplify-qwen3-v4.iso` (§2). На самой ВМ 402 после перезапуска в serial:
«[smoke 49] AMPLIFY MIND self-test PASS», «[smoke 50] verdict: READY», 4/4
ядра, «[smoke 50.1] … PASS» (гость на ядре стенда, 19 выходов, 9.9 мс), на
экране — установщик (build 20261006). ДАЛЬШЕ ЭКРАНА УСТАНОВЩИКА НА СТЕНДЕ
НЕ ПРОВЕРЯЛОСЬ: вход в систему — это пароль, ИИ его не набирает; окно
Phantom и Qwen3 на ВМ 402 глазами смотрит пользователь. Qwen3 одновременно
с работающим гостем не запускался нигде (пользователь от этой проверки
отказался, 06.10). После этой регрессии в дереве одна правка — строка
подсказки в карточке «VT-x недоступен» окна Phantom (не влезала в карточку);
в ISO v4 её нет, на стенде эта карточка не показывается (VT-x там есть).
**Прогон 2026-10-07 (сборка 51.0), узел, KVM — без провалов:** 45 — 25/25 ·
41 — 9 OK · 42 — 18/18 · 57 (IDE) — 28/28 · 57_ahci — 19/19 · 48 (IDE) —
25/25 · 49_qwen3 — 51/51 · 49_mind — 35/35 · 41_2_ui — 26/26 · 41_2_input —
21/21 · 41_2_net — 14/14 · 48_2_net — 117/117 · 50_0 — 25/25 · 50_1 — 54/54
· 50_2 — 31/31 · 50_3 — 33/33 · 50_4 — 29/29 · 50_5 — 74/74 · 51_0_format —
30/30 · 57 на virtio-blk — 29/29 · 48 на virtio-blk — 25/25. На стенд (ВМ
402) сборка 51.0 не заливалась: там по-прежнему ISO v4 (50.5).
**Прогон 2026-10-07 (сборка 51.1), узел, KVM — без провалов:** 45 — 25/25 ·
41 — 9 OK · 42 — 18/18 · 57 (IDE) — 28/28 · 57_ahci — 19/19 · 48 (IDE) —
25/25 · 49_qwen3 — 51/51 · 49_mind — 35/35 · 41_2_ui — 26/26 · 41_2_input —
21/21 · 41_2_net — 14/14 · 48_2_net — 117/117 · 50_0 — 25/25 · 50_1 — 54/54
· 50_2 — 31/31 · 50_3 — 33/33 · 50_4 — 29/29 · 50_5 — 74/74 · 51_0_format —
30/30 · 51_1_interp — 45/45 · 57 на virtio-blk — 29/29 · 48 на virtio-blk —
25/25. Первый отдельный запуск теста 51.1 дал 44/45: проверка «тест умеет
падать» не дождалась сломанного интерпретатора (§5.44) — исправлен предел в
исполнителе набора, не движок. На стенд (ВМ 402) сборка 51.1 не заливалась.
**Прогон 2026-10-07 (сборка 51.2), узел, KVM — ВЫБОРОЧНЫЙ (решение
пользователя, §6): только тесты, которые правка могла задеть.** 51_2_wasi —
52/52 · 51_1_interp — 45/45 · 51_0_format — 30/30 · 45 (том FAT, Amply) —
25/25. Первый запуск 51_1 дал 44/45: проверка «тест умеет падать» собирает
движок своим списком файлов, а в движке появился `wasm_wasi.c` — исправлен
тест (список), не движок. Первый запуск 51_2 — 49/52: фаззинг (мутантам дано
слишком много «топлива», шаг не кончался — прерван вручную; §5.44 ещё раз) и
сравнение вывода files (между строк программы в serial шли строки драйвера
FAT) — исправлен тест. НЕ ЗАПУСКАЛИСЬ на этой сборке: 41, 42, 48, 49 (оба),
41_2 (три), 48_2, 50_0…50_5, 57 (все), 48 и 57 на virtio-blk. Из них правка
51.2 касается тех, что пишут на том FAT (57, 48): в `fat32.c` добавлены две
функции и одно условие в `create_entry` (без вызова выключателя поведение
прежнее) — на этой сборке они не проверены. На стенд (ВМ 402) сборка 51.2 не
заливалась.
**Прогон 2026-10-07 (сборка 51.3), узел, KVM — ВЫБОРОЧНЫЙ, без провалов:**
51_3_core — 59/59 (дважды: до и после обслуживания пачкой) · 51_2_wasi —
52/52 · 51_1_interp — 45/45 · 51_0_format — 30/30 и те, кто делит с
программой пул ядер, цикл WM и терминал: 41_2_ui — 26/26 · 41_2_input —
21/21 · 49_mind — 35/35 · 50_1_vmx — 54/54 · 50_5_app — 74/74. НЕ
ЗАПУСКАЛИСЬ на этой сборке: 45, 41, 42, 48, 57 (все), 49_qwen3, 41_2_net,
48_2_net, 50_0, 50_2, 50_3, 50_4. Что из 51.3 их касается: правка цикла WM
(вызов `rune_tick` и проверка перед сном) и обработка клавиш терминала — их
проходит каждый тест, а проверены они на восьми перечисленных; правка
`fat32.c` из 51.2 на тестах 57 и 48 по-прежнему не проверена. На стенд (ВМ
402) сборка 51.3 не заливалась.
**Прогон 2026-10-08 (сборка 51.4), узел, KVM — ВЫБОРОЧНЫЙ, без провалов:**
51_4_app — 96/96 · 51_3_core — 59/59 · 51_2_wasi — 52/52 · 51_1_interp —
45/45 · 51_0_format — 30/30 · 50_5_app — 74/74 · 41_2_ui — 26/26 ·
41_2_input — 21/21 · 57 на virtio-blk — 29/29 · 48 на virtio-blk — 25/25
(последние два — из-за выросшего initrd; ими же впервые проверена правка
`fat32.c` из 51.2 — на virtio-blk, на IDE и AHCI она по-прежнему не
прогонялась). Тест 51.4 прошёл с третьего прогона: первый — ядро не
загрузилось (§5.50), второй — 62/95, тест исчерпал кучу терминалами (§5.51);
оба раза правился и код (журнал — в кучу), и тест. НЕ ЗАПУСКАЛИСЬ на этой
сборке: 45, 41, 42, 48 и 57 на IDE, 57_ahci, 49_qwen3, 49_mind, 41_2_net,
48_2_net, 50_0…50_4. Что из 51.4 их касается: образ ядра вырос (до конца
0x7DBEE0), initrd — на 136 КБ, в цикле WM появился вызов `app_rune_tick`
(без окна — одна проверка), в реестре — приложение и значок; ядро с этим
грузилось в десяти перечисленных тестах. MIND рядом с программой и гость
phantom рядом с окном Rune не проверялись. На стенд (ВМ 402) сборка 51.4 не
заливалась.
**Прогон 2026-10-08 (сборка 51.5), узел, KVM — ВЫБОРОЧНЫЙ, без провалов:**
51_5_jit — 55/55 · 51_4_app — 96/96 · 51_3_core — 59/59 · 51_2_wasi — 52/52
· 51_1_interp — 45/45 · 51_0_format — 30/30 · 50_5_app — 74/74 · 41_2_ui —
26/26 · 57 на virtio-blk — 29/29. С 51.5 все они исполняют программы
МАШИННЫМ КОДОМ (JIT включён по умолчанию); интерпретатор проверяют тест 51.5
(`WASM_JIT=0`, `rune jit off`) и мутанты тестов 51.1 и 51.3. Первый прогон
теста 51.5 дал 53/54: мутант «старое значение локальной» набор не ловил
(§5.52) — добавлен свой модуль, эмиттер не менялся. НЕ ЗАПУСКАЛИСЬ на этой
сборке: 45, 41, 42, 48 (все), 57 на IDE и AHCI, 49_qwen3, 49_mind,
41_2_input, 41_2_net, 48_2_net, 50_0…50_4. Что из 51.5 их касается: образ
ядра вырос на 33 КБ (конец 0x7E4FA0), при загрузке добавился smoke 51.5
(45 мс); ядро с этим грузилось в девяти перечисленных тестах. После прогона
в дереве одна правка — строка статистики в `tools/wasm_host.c` (`WASM_STATS`).
MIND и гость phantom рядом с программой в машинном коде не проверялись. На
стенд (ВМ 402) сборка 51.5 не заливалась.
**Прогон 2026-10-08 (сборка 38.1.0), узел, KVM — ПОЛНЫЙ (`pve_full38.sh`),
без провалов:** 45 — 25/25 · 41 — 9 OK · 42 — 18/18 · 57 (IDE) — 28/28 ·
57_ahci — 19/19 · 48 (IDE) — 25/25 · 49_qwen3 — 51/51 · 49_mind — 35/35 ·
41_2_ui — 26/26 · 41_2_input — 21/21 · 41_2_net — 14/14 · 48_2_net —
117/117 · 50_0 — 25/25 · 50_1 — 54/54 · 50_2 — 31/31 · 50_3 — 33/33 · 50_4 —
29/29 · 50_5 — 74/74 · 51_0 — 30/30 · 51_1 — 45/45 · 51_2 — 52/52 · 51_3 —
59/59 · 57 и 48 на virtio-blk — 29/29 и 25/25; вне регрессии: 40_libc —
31/31 · 46_sandbox — 41/41 · 47_crypto — 68/68 · 47_send_recv — 6/6 · 51_4 —
96/96 · 51_5 — 55/55 · `test_uefi_boot` и `test_e1000_qemu` отработали (они
вердикта не выносят — только печатают строки serial). На сборке ДО правок
(тот же день) 40_libc давал 30/31, а 47_crypto — 66/68: устарели ожидания
самих тестов (ISO с этапа 57 открывает установщик, а не firstboot; самотест
crypto давно 28/28, строка про корни сменила вид в 48.2) — тесты поправлены,
ядро ни при чём. НЕ запускались: 47_live/modern/ecdsa (тот же путь, что
send_recv), 47_internet и e1000_https (нужен интернет), test_mouse_*,
c5_splash, test_stage_*_static (ядро не проверяют). На стенд (ВМ 402) не
заливалось.
Тесты установщика и FDE с диском на IDE идут под TCG (PIO под KVM пишет
~0.2 МиБ/с; `AOS_IDE_KVM=1` — принудительно KVM), с `AOS_DISK_BUS=virtio` —
под KVM на virtio-blk. Регрессия шла на сборке до последней правки
`app_browser.c` (экран «Загружаю страницу» вместо прежней страницы ошибки);
после неё повторён только 48_2_net — 95/95. Описания тестов ниже — что они
проверяют.
- `scripts/test_stage49_qwen3.sh [0123]` — 51/51. Фаза 0 (хост):
  промпт = официальный jinja-шаблон GGUF (5 вариантов), токенизатор = эталон
  (3021 текст; эталон = HF tokenizer.json), ids EN/RU/`<tool_call>`/ответа
  после `<tool_response>` = эталон. Фаза 1 (q35, 6 ГБ): smoke 49.1, веса выше
  4 ГБ, кэш system, ответы EN/RU(ЙЦУКЕН)/think = эталон, ps + факты. Фаза 2:
  модель сама зовёт ls (= эталон). Фаза 3: установщик копирует Qwen3 (q35,
  4 ГБ), sha256 на хосте, установленная система грузит Qwen3. Эталоны —
  `scripts/stage49_qwen3_golden.txt`, генератор (Windows, ~25 мин) —
  `scripts/gen_qwen3_golden.sh`; нужны `build/mind/Qwen3-1.7B-Q8_0.gguf`,
  `build/mind/hf/tokenizer.json`, `build/q49/texts.hex` (+`ref_ids.txt`). Под TCG на ПК ~1.5 ч.
  Замер: `scripts/bench_stage49_qwen3.sh <cpus> <mem> "bench_on_boot=4"`.
  Скорость Qwen3-1.7B: TCG 4 ядра — prefill ~0.4, генерация ~0.3 ток/с;
  KVM (Proxmox, E5-2670 v2, 4 ядра) — загрузка 1.8 ГБ за 15 с, prefill
  ~11 ток/с (system 489 токенов — 42 с, раз после загрузки), генерация ~5.5 ток/с.
- `scripts/test_stage49_mind.sh` — 35/35. MIND: хост-движок = эталон, smoke 49,
  SmolLM2 в ядре через Ctrl+Space (ids ответов = `scripts/stage49_golden.txt`),
  /ls /read /exec /new, роутер→ps, одно ядро, запасная stories260K, `muse` в
  терминале. Нужна `build/mind/SmolLM2-135M-Instruct-Q8_0.gguf` (sha256 в
  golden). Под TCG ~15 мин. Замер скорости: `scripts/bench_stage49_mind.sh
  <cpus> "bench_on_boot=16;simd=auto"`. TCG, 4 ядра: ~4.4 ток/с, 1 ядро: 2.0.
- `scripts/test_stage48_fde.sh` — 25/25 (узел; `build/luks_interop` везти с ПК и
  `chmod +x` — без него interop молча пропускается, выходит 23/23): smoke 48, установка с шифрованием,
  libcryptsetup-interop, загрузка с разблокировкой. (Этап 49: проверка «нет
  открытого текста» прогоняет раздел через `tr '\0' '\n'` — иначе `grep` по
  гигабайтам нулей без переводов строк ловил OOM в WSL.)
- `scripts/test_stage57_installer.sh` — 27/27 (узел; и через наш BOOTX64.EFI, и
  через GRUB-EFI с переносом initrd из кучи): установка, GPT/fsck на хосте,
  загрузка установленного, BIOS-ветка ISO.
- `scripts/test_stage57_ahci.sh` — 19/19 (узел): q35 (AHCI-диск + AHCI-CD, OVMF) —
  установщик видит диск, модель с ISO копируется и сверяется (sha256 на
  хосте), установленная система на q35 грузит SmolLM2 из /disk/vault/mind.
- `scripts/test_stage45_amply.sh` — 25/25 (узел: там есть python3; в WSL его
  нет, и `amf python round-trip` там падает — 24/25).
- 41.2 (узел, KVM):
  `test_stage41_2_ui.sh` — 26/26 (простой без кадров, мышь >= 60 кадров/с без
  перерисовки сцены, нет следов курсора и мусора после скрытия окна/MIND,
  MSI-X, смена режима 1280x720); `test_stage41_2_input.sh` — 21/21
  (virtio-клавиатура и планшет на MSI-X, точное попадание курсора, аппаратный
  курсор, перетаскивание окна >= 60 кадров/с, запасной путь PS/2);
  `test_stage41_2_net.sh` — 14/14 (virtio-net, DHCP, скачивание, HTTPS к
  настоящему сайту через команду `https` в терминале, сравнение с e1000;
  без интернета на узле DNS/TLS — SKIP); `AOS_DISK_BUS=virtio
  test_stage57_installer.sh` — 27/27 и `…test_stage48_fde.sh` — 25/25
  (установка, FDE и загрузка с virtio-blk). Замеры: `bench_stage41_2_frames.sh`, `bench_stage41_2_disk.sh`,
  на ВМ 402 — `pve402_bench.sh`. Сравнение скриншотов — `tools/ppmdiff.c`.
- 50.0 (узел, KVM): `test_stage50_0_probe.sh` — 25/25, ~1 мин. Фаза 1
  (`-cpu host`, нужен вложенный KVM; без него — SKIP, не PASS): ядро видит VMX
  на 4 ядрах из 4, FEATURE_CONTROL, ревизия VMCS, EPT и unrestricted guest,
  вердикт READY, 42 бита возможностей совпадают со списком самого QEMU по QMP
  (`query-cpu-model-expansion`), `phantom` / `vm caps` в терминале. Фаза 2
  (`-cpu host,-vmx`; под TCG — `-cpu max`): ни одного чтения MSR VMX, вердикт
  ABSENT, загрузка до рабочего стола. Что отдаёт ядру KVM узла: EPT (4 уровня,
  2 МБ и 1 ГБ, только-исполнение), unrestricted guest, VPID, таймер
  вытеснения (шаг TSC>>5), битовые карты MSR и I/O, загрузка/сохранение EFER,
  INVEPT/INVVPID, FEATURE_CONTROL=0x100005 (заперт, VMXON разрешён), ревизия
  VMCS 0x11e57ed0; нет EPT A/D, PML, VMFUNC, XSAVES, TSC scaling, INVPCID.
  Блок `[smoke 50]` — 33 мс загрузки с VMX, 4 мс без. TCG VMX не эмулирует:
  на ПК (WSL без KVM) ничего из этапа 50, кроме ветки «VMX нет», не запустить.
- 50.1 (узел, KVM): `test_stage50_1_vmx.sh` — 54/54, ~2 мин, четыре загрузки.
  Фаза 1 (4 ядра): smoke 50.1 (VMX на 4/4 ядрах и обратно; гость докладывает
  «RPLIFV»: реальный, защищённый, длинный режим, IN, сохранность xmm, VMCALL;
  сумма его памяти = посчитанной хостом; блок < 100 мс загрузки), `phantom
  selftest` — 9 сценариев: cores, modes, modes-4k-ept (EPT только 4-КБ
  страницами), isolation (запись за край RAM гостя = выход EPT с верным
  адресом, сторожевая страница хоста цела), triple-fault, preempt
  (зациклившийся гость: кванты, выходы таймера вытеснения, снятие, счётчик
  после снятия стоит), exit-cost (20000 OUT), cycles (100 раз создать-
  запустить-снять: свободная RAM и куча ядра не изменились), pool-share
  (parallel-for верен при считающем госте; в горячем режиме ядро гостя есть
  среди исполнителей, а гость получает 2..24 кванта за 80 мс); затем фоновый
  гость `phantom spin`: счётчик растёт, `[fstat]` показывает кадры (>= 5/с) и
  занятость BSP < 60 %, `phantom stop` < 50 мс. Фаза 2 (2 ядра, единственный
  AP = дирижёр): те же 9 сценариев. Фаза 3 (1 ядро) и фаза 4 (`-cpu
  host,-vmx`): отказ с объяснением, загрузка до рабочего стола. Без KVM фазы
  1–3 — SKIP.
- 50.2 (узел, KVM): `test_stage50_2_linux.sh` — 31/31, ~3 мин, две загрузки.
  Нужен `build/guests/CorePure64-17.1.iso` (§2; нет — SKIP): тест достаёт из
  него vmlinuz64 и corepure64.gz и кладёт на копию dev-диска в
  /vault/phantom. Фаза 1 (`phantom linux`): баннер ядра 6.18.35 в serial,
  «kvm-clock: Using msrs», «tsc: Detected … MHz», «Switched to clocksource
  kvm-clock», «ttyS0 … is a 16550A», «Run /bin/sh as init process»; команды
  через `phantom type`: `uname -r`, `echo`, `mount -t proc`, два чтения
  /proc/uptime против часов хоста (расхождение < 10 %; было 0.1 %),
  /proc/interrupts (IRQ0 timer и IRQ4 ttyS0 через XT-PIC с ненулевыми
  счётчиками); простой: `run < 20 %` и сон по HLT; кадры WM >= 5/с; нет
  oops/паники в ядре гостя; `reboot -f` = «guest asked for a reset»; после
  гостя `phantom selftest` 9/9. Фаза 2 (`phantom linux init`): штатный init
  TinyCore отрабатывает (до «root login on 'tty1'»), гость жив, без паники,
  `run < 30 %`. Замеры: ядро от старта до оболочки — 1.9 с; чтение 26 МБ с
  IDE-диска теста и создание ВМ — ~8 с (BSP на это время занят).
- 50.3 (узел, KVM): `test_stage50_3_bios.sh` — 33/33, ~4.5 мин, две
  загрузки; восемь прогонов подряд без провалов. ISO (§2; нет — SKIP) кладётся
  на копию dev-диска как /vault/phantom/core.iso БЕЗ изменений. Фаза 1:
  `phantom run` -> «bios boot: … El Torito boot image at sector 37»; баннер
  ISOLINUX и приглашение `boot:` (текст экрана BIOS в serial); `phantom type
  corepure64 console=ttyS0 loglevel=7 printk.time=1 rdinit=/bin/sh`
  (`AOS_GUEST_ARGS` — свои параметры) -> «Loading /boot/vmlinuz64», баннер
  ядра, «Run /bin/sh»; от Enter до оболочки < 15 с (обычно 1.3–1.4 с; разрывы
  в журнале гостя — по его меткам времени и по строкам [fstat] хоста); ядро
  получило набранную командную строку и initrd с ISO, карту E820, «Console:
  colour VGA+ 80x25», kvm-clock; команды в оболочке, ход времени против
  хоста, простой < 20 % ядра, BIOS читал диск, нет oops, `reboot -f` ловится.
  Фаза 2: в приглашении только Enter (`phantom type`) — загрузка по
  умолчанию; через 30 с `phantom screen` показывает на текстовом экране
  гостя `tc@box`; гость жив и в простое; после — `phantom selftest` 9/9.
- 50.4 (узел, KVM): `test_stage50_4_alpine.sh` — 29/29, ~5 мин, две загрузки.
  ISO Alpine (§2; нет — SKIP) кладётся на копию dev-диска как
  /vault/phantom/alpine.iso без изменений. Фаза 1: `phantom run <iso> virt
  console=ttyS0,115200 loglevel=7` -> ISOLINUX 6, ядро 6.18.52 с набранной
  строкой, «pci 0000:00:01.0: [1af4:1001]», «virtio_pci: leaving for legacy
  driver», `login:` на порту (момент снимается до проверок); вход `root`,
  `cat /etc/alpine-release` = 3.24.2, `uname -r`, `/sys/block/vda/size` =
  135168 и `ro` = 1, `mount`: «/dev/vda on /media/vda type iso9660», в
  /proc/interrupts у virtio ненулевой счётчик; простой < 20 %, диск без
  ошибок; `poweroff` -> «guest halted itself with interrupts off». Фаза 2:
  без строки (загрузка по умолчанию), через 60 с `phantom screen` показывает
  `login:` на текстовом экране гостя (tty1); гость жив и простаивает; после —
  `phantom selftest` 9/9.
- 50.5 (узел, KVM): `test_stage50_5_app.sh` — 74/74, ~10 мин, три загрузки.
  Всё — как человек: щелчки по координатам из `[phantom-ui] layout …` (QMP,
  планшет), клавиши — `sendkey` (virtio-клавиатура хоста -> перехват ->
  8042 гостя). Что набрано в госте, тест узнаёт от самого гостя: команда в
  его оболочке на tty1 пишет метку в /dev/ttyS0, она приходит строкой
  `[guest] …`. Фаза 1 (оба ISO на диске в /vault/phantom): `summon Phantom`,
  список из двух образов; TinyCore — выбор щелчком, «Запустить», кадры при
  чтении образа >= 5/с, клавиатура сразу у гостя, шрифт 16x32; в приглашении
  `boot:` строка набирается клавишами (INT 09h/16h) и доходит до «Command
  line» ядра; ядро находит «i8042 KBD port» и «AT Translated Set 2
  keyboard»; `echo K5-$((6*7)) >/dev/ttyS0` (Shift) даёт K5-42, стрелка
  вверх повторяет команду, в /proc/interrupts у i8042 ненулевой счётчик;
  правый Ctrl возвращает клавиатуру, щелчок по экрану отдаёт; кадр окна
  меняется от набранного; Esc закрывает окно — гость жив (`phantom status`),
  повторный `summon` продолжает сеанс; кнопка Ctrl+Alt+Del -> «guest asked
  for a reset» -> окно «Завершён» -> «Закрыть гостя». Alpine: строка `virt`
  из поля, вход root на tty1 с клавиатуры окна, 3.24.2, IRQ1 и линия
  virtio; затем MIND (Ctrl+Space, встроенная stories260K, greedy): ответ на
  «Once upon a time», ids = эталон этапа 49; гость после MIND отвечает;
  Super+Space при клавиатуре у гостя открывает терминал; простой < 30 %;
  `phantom stop` из терминала — окно возвращается в библиотеку; selftest
  9/9. Фаза 2 (`-vmx`): окно открывается, «Запустить» отказывает с причиной.
  Фаза 3 (на диске образов нет, в приводе — `build/myos-x86_64.iso`, загрузка
  с диска): в списке оба образа с пометкой «medium», TinyCore читается с CD
  и грузится до `tc@box` на текстовом экране. Скриншоты —
  `build/screens-50_5/*.ppm`.
- 51.0 (узел): `test_stage51_0_format.sh` — 30/30, ~3 мин, одна загрузка.
  Фаза 0 (хост, тот же движок под gcc): `make wasm-host` без предупреждений;
  `wasm_spec_pack.sh` (90 файлов .wast -> пакет); `wasm_host spec` — верные
  модули приняты все, неверные и нераспознаваемые отклонены все, провалов 0;
  `make wasm-apps` — 6 модулей clang приняты, счётчики типов/функций/
  импортов/экспортов/данных/глобальных равны `wasm-objdump -h`, у hello видны
  импорты WASI и `_start`, calc-feat действительно содержит memory.copy/fill;
  `wasm_host_asan fuzz` — 40 000 испорченных модулей без срабатываний
  санитайзера. Фаза 1 (QEMU): строки `[smoke 51]` (два модуля, 5 видов порчи
  с верной причиной, 200 циклов без утечки, < 100 мс); в терминале `rune`,
  `wasm selftest`, `rune info` для hello.wasm с диска (счётчики = хосту),
  для обрезанного файла (malformed), для calc с расширениями, для
  несуществующего; `rune spec /disk/nexus/spec.pack` — строка итога
  СОВПАДАЕТ с хостом, куча ядра не изменилась. Без инструментов узла
  (wast2json, clang) соответствующие проверки — SKIP, не PASS. С 51.1 `rune
  spec` ещё и исполняет набор (ВМ теста — 2 ГБ), `selftest` — 10 проверок.
- 51.1 (узел): `test_stage51_1_interp.sh` — 45/45, ~4.5 мин, одна загрузка.
  Фаза 0 (хост): `wasm_host spec` — пять строк по видам проверок исполнения
  (всё сошлось, пропущено 0); «тест умеет падать» — три испорченные sed'ом
  копии `wasm_interp.c` обязаны дать провалы; `make wasm-apps` и `pure.host`
  (эталон: 18 строк «функция(аргументы) = значение»), каждая функция из
  pure.wasm, pure-O0.wasm, pure-feat.wasm через `wasm_host call` = эталону;
  ловушки в настоящем коде (unreachable, запись по дикому адресу, деление на
  ноль, INT_MIN / -1), вечный цикл = «out of fuel»; pure-feat действительно
  содержит memory.copy/fill и trunc_sat; hello.wasm (WASI) не запускается с
  именем недостающего импорта; набор под ASan+UBSan = те же цифры;
  `fuzz-exec` по всем модулям набора и программам. Фаза 1 (QEMU, 2 ГБ):
  `[smoke 51.1]` (6 проверок, < 100 мс), `rune` (exec=1), `rune selftest`
  (10 из 10), `rune spec /disk/spec.pack` — ОБЕ итоговые строки (`total:` и
  `exec:`) совпадают с хостом, куча не изменилась; `rune call /disk/p.wasm …`
  — 18 функций = эталону, по 4 из двух других сборок; ловушки; `rune call -t
  400 … forever 1` — «stopped (time limit 400 ms)», время остановки 390..480
  мс, после неё система отвечает; неверные аргументы, нет функции, нет файла,
  WASI-программа; fib(32) с временем. Тест ждёт НОВУЮ строку ответа
  (`cmd_expect` считает вхождения до и после): разные сборки дают одинаковый
  текст.
- 51.2 (узел): `test_stage51_2_wasi.sh` — 52/52, ~6 мин, одна загрузка
  (диски — virtio-blk, §5.47). Фаза 0 (хост): `make wasm-apps`; `same` —
  программа под gcc в пустом каталоге против модуля через `wasm_host run -d
  <пустой каталог>` (вывод и код возврата; отличие одно — строка `arg0=`):
  hello, calc, files, env (args/clock/random/exit 7) x 3 сборки; что files
  оставила на диске (один `work/kept.txt`); то же с `WASI_NO_RENAME=1`;
  переменная окружения, ввод (5 байт из канала), abort (код 101, «stopped:
  unreachable»), вечный цикл под `WASM_FUEL`, `env oom`, модуль без `_start`;
  jail (14 из 14, рядом с каталогом ничего не изменилось) и `jail ro` с `-r`
  (6 из 6, seed.txt цел); calc/files/jail под ASan+UBSan; `fuzz-exec` с
  `WASM_FUZZ_DIR` (каталог на глубине a/b/c — выход даже на уровень виден):
  нет срабатываний, тысячи запущены, вне каталога 0 файлов. Фаза 1 (QEMU, 2
  ГБ): `[smoke 51.2]` (< 100 мс); `rune` (wasi=1), `rune selftest` (12 из 12);
  `rune run /disk/nexus/…` — hello (код 7, `arg0=hello.wasm`), calc, files,
  `env args|clock` — вывод между строкой `[rune] run <путь> …` и строкой итога
  (`prog_out`, без строк `[fat32] …`) равен эталону gcc; random, getenv OS,
  exit 3, stdin (0 байт), abort, oom, `-t 500 … spin`, utf8 (в журнале —
  байты CP1251); jail с `-d /disk/nexus/cell` и `-r -d /disk/nexus/ro`; модуль
  без `_start`, каталога нет, файла нет; повторный selftest. После выхода QEMU
  — том глазами хоста (mtools): `nexus/work/kept.txt` один и с верным текстом,
  `nexus/outside.txt` цел и рядом ничего не появилось, `nexus/ro` не тронут,
  `fsck.fat -n` без признаков повреждения цепочек и каталогов. С 51.3
  программы идут на своём ядре: проверки ждут строку ИТОГА (`run_has`), а не
  текст вывода — пока программа жива, терминал — её консоль.
- 51.3 (узел): `test_stage51_3_core.sh` — 59/59, ~8 мин, три загрузки (4, 2 и
  1 ядро; диски virtio-blk; `fstat=1`). Фаза 0 (хост): `wasm_host spec
  --stepped` — итоги как у обычного прогона, под ASan+UBSan тоже; «тест умеет
  падать» — два испорченных сохранения положения (обычный прогон 0 провалов,
  пошаговый — не 0); hello/calc/files x 3 сборки с `WASM_STEPPED=1` — вывод
  как у gcc, в статистике тысячи пауз и сотни услуг; `fuzz-exec` в пошаговом
  режиме под санитайзерами. Фаза 1 (4 ядра): `[smoke 51.3]` (ядро >= 1, услуги
  через главное, < 100 мс); `rune selftest` — 14; hello/calc/files — вывод как
  у gcc, в заголовке `core N`, N >= 1; `rune run -b … spin`: через 4 с `rune
  status` — «computing», счёт >= 2500 мс, по `[fstat]` кадров >= 5/с и BSP <
  60 %; вторая программа — отказ; `phantom selftest` рядом — 9 из 9 (без VT-x
  — SKIP); `rune stop`; `-t 300` (счёт 290..400 мс); `-m 8` + `env oom` (3..8
  МБ); диалог `env echo` из терминала: две строки, Ctrl+D, затем Ctrl+C на
  `spin`; `-b -i … echo`: «waiting for input», счёт < 100 мс, `rune type`,
  `rune eof`; `env sleep 1500`: по часам 1500..2500 мс, счёт < 100 мс; `-d
  /disk`, `-d /disk/Sanctum`, `-d /disk/sanctum/system` — отказ; `-n` + files —
  «cannot create work», код 1. Фаза 2 (2 ядра): calc на ядре 1, spin + status
  + stop. Фаза 3 (1 ядро): smoke говорит «нет второго ядра», hello на главном
  ядре («main core, limit»), `-t 400 … spin` — стоп по пределу, selftest — 13.
- 51.4 (узел, KVM): `test_stage51_4_app.sh` — 96/96, ~13 мин, две загрузки
  (4 ядра и 1; диски virtio-blk; 1920x1080). Всё — как человек: щелчки по
  координатам из `[rune-ui] layout …` (QMP, планшет), клавиши — `sendkey`.
  Что окно ПОКАЗЫВАЕТ, тест узнаёт от окна: Ctrl+O / Ctrl+P печатают в serial
  весь журнал консоли / видимые строки (`[rune-log]`, `[rune-scr]`; CP1251 ->
  iconv). Фаза 0 (хост): модули в `userspace/wasm/dist` побайтно = свежей
  сборке (`make wasm-dist RUNE_DIST_OUT=…`), в каждом секция `rune.about`,
  все четыре в initrd, каждая = gcc на хост-движке; ожидаемый текст диалога
  в окне строится из эталона gcc (после каждого приглашения — введённая
  строка). Фаза 1: `rune list` (4 встроенные с описаниями + 3 с диска,
  не-wasm файл не виден), `summon Rune`; abacus — щелчок, «Запустить», три
  выражения, выход, журнал = эталону с эхом (и НЕ равен чужому эталону —
  сравнение умеет падать); «Запустить снова» и строка в 183 знака — перенос;
  oracle — аргумент из поля, игра до победы, затем «Конец ввода»; scribe —
  каталог из поля, запись, список, доступ «Чтение» (отказ записи) и «Без
  файлов»; `/disk/Sanctum` — отказ с причиной; sieve — по умолчанию, предел
  99 (отказ), предел 8 (программе не дают памяти), без предела 30 млн;
  испорченный модуль — отказ; files — 93 строки, окно у конца, PgUp, Home,
  колесо, End (номера строк и сами строки); env spin — `rune status`, кадры
  >= 5/с, занятость BSP < 60 %, вторая программа — отказ, Esc закрывает
  окно, программа жива, `summon` возвращает к ней, «Остановить»; программа,
  кончившаяся при закрытом окне, — итог и весь текст после открытия;
  программа терминала (`rune run -b`) — окно показывает и останавливает,
  затем библиотека; selftest 14; после QEMU — `notes.txt` на томе, каталог
  программ не тронут, fsck. Фаза 2 (1 ядро): программа на главном ядре,
  вывод в окне = gcc. Без рабочего стола тест останавливается сам (§5.51).
  Скриншоты — `build/screens-51_4/*.ppm`.
- 51.5 (узел, KVM): `test_stage51_5_jit.sh` — 55/55, ~11 мин, две загрузки
  (4 ядра и 1; диски virtio-blk). Фаза 0 (хост): набор с JIT = строкам
  интерпретатора (`WASM_JIT=0`), то же `--stepped`, то же под ASan+UBSan;
  свой модуль `old.wat` (wat2wasm): локальная, прочитанная до записи в неё,
  и `local.tee` — то, чего в наборе нет (§5.52); «тест умеет падать» — восемь
  sed-мутантов `wasm_jit.c` (сложение, граница памяти, тип косвенного вызова,
  сравнение, переставленные операнды, старое значение локальной, перенос при
  переходе, NaN в сравнении): с JIT — провалы или падение, с `WASM_JIT=0` тот
  же файл проходит; hello/calc/files/sieve x 3 сборки = gcc, они же отрезками
  (`WASM_STEPPED=1`); pure.c — 54 результата = gcc; ловушки и «топливо»;
  скорость — JIT не меньше чем втрое быстрее (fib, решето, calc); фаззинг с
  исполнением под санитайзером, обычный и отрезками. Фаза 1 (4 ядра):
  `[smoke 51.5]` (< 100 мс) и прежние smoke машинным кодом; `rune` (jit=1),
  selftest 16; `rune spec` в ядре с JIT и после `rune jit off` — строки хоста,
  куча не изменилась; hello/calc/files на своём ядре = gcc, заголовок «, jit»;
  решето 30 млн; spin + status + stop; `-t 300`; `-m 8`; диалог и Ctrl+D; сон
  1500 мс без счёта; затем всё то же интерпретатором («, interpreter») и
  сравнение времени (>= 3 раз); selftest ещё раз; нет `[heap] OOM`. Фаза 2
  (1 ядро): hello на главном ядре («main core, limit …, jit»), `-t 400`,
  selftest 15. Мутант переписал строку шаблона — поправить sed в тесте, иначе
  `pattern-not-found`.
- Тест 57 с 50.5: +проверка образов гостей — под KVM с вложенной
  виртуализацией установщик копирует их в /vault/phantom (и первый сверяется
  побайтно с хоста), под TCG — пропускает с «no usable VT-x».
- Обзор настоящих сайтов (не тест «прошёл/нет» — сайты меняются):
  `bash scripts/pve_test.sh <имя> pve_net_suite` = тест 48.2 +
  `survey_sites.sh` (QEMU со slirp, вход, браузер, адреса из
  `scripts/sites.txt`; итог `build/screens-sites/summary.txt`, скриншоты,
  serial) + запись трафика и `tools/pcap_tcp.py` (паузы >1.5 с в потоках TCP
  → `tcp-gaps.txt`). Свой список: `AOS_PCAP=/tmp/x.pcap bash
  scripts/survey_sites.sh <файл>`.
- 48.2 (узел, KVM): `test_stage48_2_net.sh` — 117/117, ~5 мин (с 2026-10-02:
  +gzip/zlib/deflate побайтно, cookie из редиректа, 103 Early Hints, meta
  refresh, скрытые элементы/noscript/React-слот, HTML под octet-stream,
  редирект на адрес 1.5 КБ, кириллица в ссылке как %XX, старт с `<main>`,
  поле поиска формы, поиск из адресной строки; обрезка теперь на 4 МБ).
  Нужны python3
  (`tools/test_http_server.py`: страницы, chunked, без Content-Length,
  редиректы, 404, 3 МБ, «зависший» ответ) и QMP. Фаза 1: DHCP и продление
  аренды (`dhcplease=40`); анимации — строки `[anim] … fps=N` >= 55 и чистый
  экран после закрытия; браузер — щелчок по ссылке, назад/вперёд, страница
  636 КБ и chunked побайтно (FNV), обрезка на 2 МБ, отказ/DNS/петля
  редиректов, отмена по Esc, кадры во время загрузки, PgDn/колесо/Home,
  HTTPS к настоящему сайту (без интернета на узле — SKIP);
  Mesh — щелчки по координатам из `[mesh] layout`, отказ на неверный адрес,
  «Применить», network.conf на диске (mtools); замедленная анимация
  (`compstat slow 8`) — кадр середины отличается и от начала, и от конца.
  Фаза 2: `mode=static` из файла, DHCP выключен, загрузка работает. Фаза 3:
  линк выключен (`set_link nic0 off`) → запасная статика → линк включён →
  DHCP отвечает после загрузки, система переходит на его адрес.
- Также `test_stage40/41/42/46/47_*.sh`. Smoke-блоки печатаются в serial как
  `[smoke NN]` до splash — по ним тесты и судят.

## 8. Ограничения (честно говорить пользователю)
- Диски: virtio-blk (41.2), SATA AHCI (DMA, опрос, 1 слот команд), IDE/ATA
  (PIO: под KVM чтение ~3.7 МиБ/с, запись ~0.2 МиБ/с — QEMU делает отдельную
  операцию на каждый сектор, READ/WRITE MULTIPLE и DMA не реализованы; только
  запасной путь); нет virtio-scsi, NVMe и
  USB mass storage. Том FAT монтируется первый найденный по порядку слотов
  (IDE → AHCI → virtio): две установленные системы на разных дисках путаются.
- Ввод: virtio-input (клавиатура, планшет, мышь) или PS/2; USB HID нет.
  Системных раскладок нет (этап 53).
- virtio (41.2): только modern-транспорт и MSI-X (INTx нет — устройство без
  MSI-X работает опросом); virtio-gpu — только 2D (virgl/3D нет); аппаратный
  курсор включается лишь с virtio-планшетом и виден, только если клиент
  консоли умеет рисовать курсор сам (noVNC умеет; на `screendump` его нет);
  смена режима — вручную (`gpustat mode WxH`), не больше режима прошивки,
  без слежения за размером окна хоста; открытые окна размер не меняют.
  virtio-net — без контрольных сумм/TSO на стороне хоста, одна пара очередей.
- Плавность: счётчик кадров гостя — это не то, что видно в консоли Proxmox
  (её частоту задаёт VNC-сервер QEMU). Окна приложений перерисовываются
  целиком на каждый кадр сцены (`on_draw`), при открытом окне остаётся
  обновление 10 раз/с (~8 % одного ядра на 1080p); SSE2-блиты и кэш глифов
  не делались — замеры показали, что цель достигается без них.
- Сеть (48.2): один TCP-сокет на всю систему — соединения строго по очереди,
  загрузка страницы синхронная (интерфейс при этом рисуется, но ввод, кроме
  Esc, отбрасывается; DHCP-автомат на это время стоит); приёмный буфер 64 КБ
  без масштабирования окна; сегменты не по порядку не копятся (просим
  повтор); IPv4 без фрагментации, IPv6 нет; TLS только 1.2 (TLS 1.3 и HTTP/2
  нет; сжатие — только gzip/deflate, без brotli); cookies живут в памяти до
  перезагрузки (48 штук, без срока годности); состояние
  линка карты не читается (выдернутый кабель = «DHCP молчит»); одна сетевая
  карта; DHCP: без RELEASE/DECLINE и проверки занятости адреса (ARP-probe).
  Статический адрес чужого узла примется без возражений.
- Браузер (48.2): только текст и ссылки — нет картинок, CSS, JavaScript;
  из форм — только одно текстовое поле на форму (запрос набирается в адресной
  строке и уходит GET-запросом, даже если форма хотела POST; скрытые поля не
  передаются), входа на сайты нет; таблицы идут текстом через « | », курсива
  нет; кодировки UTF-8 и Windows-1251; HTML до 4 МБ и 512 КБ текста (дальше
  обрезается с пометкой), до 2048 ссылок на странице. Не откроются: сайты
  «только TLS 1.3», сайты за защитой от ботов (Cloudflare/Qrator: 403, капча),
  сайты, которые без JS пусты, и то, что фильтруется на пути (youtube.com).
  Обход блокировок не делается. Поиск — DuckDuckGo (html-версия), запрос —
  только латиницей: адресная строка принимает ASCII, курсор в конце строки
  (системные раскладки — этап 53). Сертификат проверяется по набору Mozilla,
  но непроверенный не блокируется (значок «HTTPS ?»; так у сайтов на корне
  Минцифры). «Скрытое» определяется по атрибутам, а не по CSS — на части
  сайтов видны служебные подписи меню.
- Анимации (48.2): окна под оверлеем на время перехода не обновляются;
  обычные (не workspace) окна при «хлопке» масштабируются ближайшим соседом;
  на 16:9 workspace-окно закрывает rail (так было и до 48.2). Колесо мыши —
  только virtio-input и только в браузере.
- phantom (50.5): гость грузится со своего ISO через наш BIOS и работает в
  окне приложения Phantom. Экран — ТОЛЬКО ТЕКСТ 80x25 (нет VBE и графики:
  графические меню загрузчиков и X не покажутся; мигание символов не
  рисуется, курсор всегда подчёркиванием). Мыши у гостя нет. Клавиатура:
  набор 1 с включённым «переводом» контроллера (гость, выключивший перевод и
  ждущий набор 2, получит не те коды — не встречалось); правый Ctrl и Super
  гостю не достаются (первый возвращает клавиатуру системе, второй — клавиша
  оболочки); пока клавиатура у гостя, Ctrl+Space (MIND) уходит ему — сначала
  правый Ctrl; автоповтор — тот, что даёт клавиатура хоста; буфера обмена
  нет; индикаторы Caps/Num не отражаются; в загрузчике (BIOS) раскладка
  американская. Проверены ДВА образа: TinyCore CorePure64-17.1 (isolinux
  4.05) и Alpine virt 3.24.2 (syslinux 6.04); другие дистрибутивы, GRUB,
  Windows, BSD не пробовались. Диск гостя: один, только чтение, содержимое —
  сам ISO (установить гостя некуда, постоянного диска нет), virtio-blk
  только «старого вида». Шина PCI — два устройства, без PCI BIOS. BIOS:
  только El Torito «без эмуляции», нет APM/ACPI (`poweroff` кончается
  остановкой процессора — окно пишет «гость выключился»). Перезагрузка
  гостя (и Ctrl+Alt+Del) = его конец: окно показывает последний экран,
  запускать заново — кнопкой. ISO читается в RAM целиком (образ + 512 или
  256 МБ гостя + запас); чтение идёт фоном с индикатором, но со скоростью
  диска: 66 МБ с IDE — ~27 с, кадров при этом ~8 в секунду. Отказ, который
  выясняется после чтения (образ не загрузочный), виден в окне и в serial,
  а в терминале после `phantom run` — нет. В списке образов до 12 файлов,
  видно 4–6 без прокрутки; файл с именем 8.3 показывается заглавными. Одна
  работающая ВМ, один vCPU, нужен второй процессор; у гостя 8259, 8254, один
  порт, часы, клавиатура — нет сети, ACPI, локального APIC, XSAVE/AVX, звука.
  Строковый ввод-вывод и MMIO не эмулируются. Частоту TSC гость узнаёт
  только через kvmclock. На двух ядрах гость сидит на ядре-дирижёре: пока
  MIND генерирует, гость стоит. С MIND рядом проверена только встроенная
  модель (ответ за миллисекунды); долгая генерация Qwen3 при работающем
  госте не замерялась (расчёт: гостю ~20 % ядра). Только Intel VT-x. Всё
  проверено лишь под вложенным KVM узла; на настоящем железе не запускалось.
- rune / WebAssembly (51.4): программа WASI (обычный C под wasm32-wasi:
  printf, fopen, opendir, clock_gettime) запускается из окна Rune или
  командой `rune run` и исполняется ИНТЕРПРЕТАТОРОМ на отдельном ядре;
  интерфейс при этом живой. ОДНА ПРОГРАММА одновременно — и в окне, и в
  терминале вместе. Окно Rune: своих окон и графики у программ нет — только
  текстовая консоль; вывод и ошибки одним цветом, ESC-последовательностей
  нет; журнал консоли — 256 КБ / 16 тысяч экранных строк, дальше начало
  отбрасывается; при запуске следующей программы журнал прежней стирается;
  выделить и скопировать текст нельзя; ввод — строками, только латиница и
  знаки ASCII (раскладок нет до этапа 53), курсор в конце строки, истории
  ввода нет; аргументы делятся по пробелам (кавычек нет), до 16 штук и 160
  знаков; предел времени счёта в окне не задаётся (только `rune run -t`), а
  на машине с одним ядром окно на время программы замирает (до 10 с), и ввод
  ей недоступен; программа, запущенная из терминала, в окне видна без вывода
  и ввода; список — до 24 программ, описание — только у тех, в ком есть
  секция `rune.about`; кадр окна на 1080p — около 12 мс (окно перерисовывается
  целиком, как и остальные), при открытой консоли главное ядро занято на
  десяток процентов; после перестройки журнала под другую ширину экрана
  служебные строки теряют свой цвет. Консоль программы терминала —
  терминал, из которого её запустили; пока она работает, этот терминал команд
  не принимает (строки идут ей; Ctrl+C — остановить, Ctrl+D — конец ввода), а
  подсказка терминала остаётся прежней («amplify >»). Закрыли терминал —
  программа осталась без консоли (вывод только в serial), остановить —
  `rune stop` из другого терминала. В фоне (`-b`) вывод виден только в serial.
  Ввод — строками (без посимвольного режима и без эха, отличного от
  терминального); буфер ввода 1 КБ, лишнее отбрасывается. На машине с ОДНИМ
  ядром программа идёт на главном, как в 51.2: интерфейс стоит, предел
  времени обязателен (10 с по умолчанию), ввода нет. На двух ядрах программа
  делит единственное вычислительное ядро с MIND и гостем phantom: с гостем
  одновременно не запустится («нет свободного ядра»); когда MIND генерирует,
  программе остаётся ~20 % ядра (не замерялось). Каждое обращение программы к
  окружению (вывод, файл, часы) исполняет главное ядро из цикла окон — около
  0.5–1 мс на обращение вместе с самой работой: программа с тысячами мелких
  обращений идёт заметно медленнее, чем считает (files: 1.1 с счёта, 1.7 с по
  часам). Предел времени (`-t`) — на СЧЁТ, по умолчанию его нет; времени «по
  часам» предела нет вовсе. Пределы задаются только ключами команды (файла
  настроек нет). Программа видит ОДИН каталог тома FAT (по умолчанию
  `/disk/nexus`, иначе `-d`; корень тома и `/disk/Sanctum` не даются, `-n` —
  без каталога); `/cdrom` и файлы initrd ей недоступны. Открытый файл — копия
  в памяти: до 16 МБ на файл и 64 МБ на все открытые вместе, на диск пишется
  при закрытии, fsync и завершении (выдернутое питание = потеря
  несохранённого); один файл, открытый дважды, — две независимые копии;
  открытых описателей — до 45. Нет: времён файлов (mtime = 0), прав и
  владельцев, ссылок, переименования каталогов (файл переименовывается копией
  и удалением — не атомарно), сокетов и сети, ожидания готовности описателей
  (poll — только сон; сон с шагом ~1 мс). Имена файлов — символы CP1251 без
  `? * " < > | :`, регистр букв не различается (FAT), имя до ~190 знаков, путь
  до 255. Настенное время — RTC (считается UTC). Вывод и ошибки идут в одно
  окно; управляющие символы и ESC-последовательности (цвета) заменяются на
  «?», символ вне CP1251 — тоже. Права WASI (rights) программе показываются,
  но не ведутся. На IDE-диске под KVM запись файлов медленная. Скорость
  (51.5): JIT — простой шаблонный, без распределения регистров и без
  встраивания: в 7–11 раз быстрее интерпретатора, но в 3–7 раз медленнее
  родного кода (вызовы и дробная арифметика — дальше всего: значения живут в
  памяти, дробные — без регистров); команды, которых он не умеет (таблицы,
  bulk-memory, округления, min/max, преобразования дробного в целое, popcnt,
  copysign), исполняет интерпретатор с выходом из машинного кода на каждую —
  программа, состоящая из них (плотная работа с memory.copy малыми кусками,
  floor/trunc в цикле), ускорится мало; код переводится весь и сразу при
  запуске (ленивой и многоуровневой компиляции нет), занимает кучу ядра и не
  сохраняется между запусками; память под него исполняемая и записываемая
  одновременно (W^X нет); только x86-64, SSE2. Проверено тем же набором, что
  интерпретатор, но на настоящем железе не запускалось. Память
  программы — до 64 МБ (`-m` — меньше). Объём языка — WebAssembly 2.0 без
  SIMD; модуль с v128, потоками/атомарными операциями, исключениями,
  хвостовыми вызовами, GC, 64-битной или второй памятью получает отказ при
  загрузке; текстовый формат (.wat) не читается. Ловушка называет причину,
  но не место. Проверено на программах clang 19 + wasi-libc из Debian (C);
  Rust, Go, C++ с исключениями, WASI preview2 не пробовались. Готовых программ
  в ОС четыре (oracle, abacus, sieve, scribe — учебные, в initrd); собрать
  свою можно только вне ОС (clang + wasi-libc) и положить в `/vault/runes` —
  средства принести файл в систему, кроме установочного носителя и
  браузера без сохранения, нет. Всё проверено под вложенным KVM узла на 4, 2 и 1 ядре; на настоящем
  железе не запускалось.
- Установленная система грузится только через UEFI; Secure Boot выключить.
- ESP (ядро, initrd) не шифруется; ключ тома в RAM, пока система работает.
- MIND: архитектуры `llama` и `qwen3`, типы F32/F16/BF16/Q8_0/Q4_0 (нет
  K-квантов, qwen2/gemma, AVX2 — на Proxmox-CPU его и нет). Qwen3-1.7B-Q8_0
  нужна машина с ~3 ГБ свободной RAM (веса 1.71 ГиБ одним блоком + KV f32
  448 МиБ при ctx 2048; KV f16 не сделан); на 1–2 ГБ MIND сама берёт
  SmolLM2-135M. Генерация упирается в память (~5.5 ток/с под KVM на 4 ядрах
  E5-2670 v2), под TCG Qwen3 практически непригодна (~0.3 ток/с). Контекст
  2048: после 4–6 вопросов с данными диалог начинается заново после
  системного промпта. Русский ввод — только в панели MIND (Ctrl+L), знаки
  «.» и «,» в режиме RU — буквы ю/б (системные раскладки — этап 53). Веса
  читаются в RAM целиком; на установленную систему модели ставит установщик
  с ISO, в live-сессии MIND грузит их прямо с носителя (`/cdrom/mind/...`).

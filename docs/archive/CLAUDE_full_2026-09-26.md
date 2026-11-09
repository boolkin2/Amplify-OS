# AMPLIFY-OS — справочник для продолжения работы ИИ-агентом

Этот документ — единая точка входа для любого LLM-агента, который должен
продолжить разработку AMPLIFY-OS. После прочтения агент знает:
- что такое AMPLIFY-OS и какая у него стилистика;
- среду сборки/запуска (Windows + WSL2 / Debian + QEMU);
- что реально работает в ядре (с конкретными путями и API);
- какой этап только что завершён и как тестировался;
- весь оставшийся roadmap фазы 3 (этапы 41–58).

**Текущий этап:** `48` — ✅ **Шифрование системного раздела (LUKS1 + dm-crypt-аналог).**  Подробности — §26.
Установщик получил переключатель «Encrypt the drive»: системный раздел
становится настоящим LUKS1-томом (aes-xts-plain64, sha256, PBKDF2 под
1 с этой машины), поверх которого FAT32 работает через виртуальное
устройство blkcrypt.  При загрузке — экран «Unlock AMPLIFY-OS» до первого
обращения к /disk; тот же пароль сразу выполняет вход (единый вход).
Совместимость проверена НАСТОЯЩЕЙ libcryptsetup + OpenSSL на хосте.
`bash scripts/test_stage48_fde.sh` — 25/25 PASS.

**Предыдущий этап:** `57` — ✅ **Графический установщик с ISO (UEFI, GPT).**  Подробности — §25.
Гибридный ISO (BIOS/GRUB + UEFI/наш BOOTX64.EFI) загружает live-режим с
мастером WELCOME → LICENSE → DESTINATION → OPTIONS → INSTALL → COMPLETE по
макету; установщик размечает выбранный IDE-диск в GPT (ESP + SYSTEM [+ VAULT]),
форматирует FAT32 средствами ядра, копирует загрузчик/ядро/initrd, создаёт
администратора, сверяет записанное побайтово.  Установленный диск грузится
через UEFI сразу в экран входа.  `make run-installer`, `make run-installed`,
`bash scripts/test_stage57_installer.sh`.

**Предыдущий этап:** `47.7` — ✅ **Modern HTTPS (ECDHE-ECDSA + extended trust store).**
Этап 43 (Neural Icon Forge) пропущен — иконки ещё не готовы.

**Этап 47.7 (22.05.2026) — модернизация HTTPS для реальных сайтов:**

* **Третий cipher suite — TLS_ECDHE_ECDSA_AES128_GCM_SHA256 (0xC02B)**:
  * `kernel/arch/x86_64/tls.c` — обновлены `send_client_hello` (3 suites:
    0xC02B + 0xC02F + 0x009C), `parse_server_hello` (новая ветка `is_ecdsa`),
    `parse_certificate` (теперь принимает P-256 leaf cert), и главное —
    `parse_server_key_exchange` (DER-парсинг `SEQUENCE { r, s }` и
    `crypto_p256_ecdsa_verify` вместо `crypto_rsa_pkcs1_verify` когда
    sig_id == 0x03).
  * **Зачем:** Cloudflare, Google, многие CDN-серверы используют ECDSA-P256
    leaf-сертификаты.  Без ECDHE-ECDSA cipher suite сервер не предложит
    нашему клиенту никакой подходящий suite и handshake падает с
    "no shared cipher".  Теперь ходим на эти сайты.
* **Trust store расширен с 2 до 7 root CA** через `scripts/gen_ca_blob.sh`:
  ISRG Root X1, DigiCert Global Root G2, **GTS Root R1** (Google),
  **Baltimore CyberTrust Root** (Microsoft/Azure), GlobalSign Root R3,
  Amazon Root CA 1, USERTrust RSA Certification Authority.  Покрытие
  выросло с ~30% до ~85% современного веба.  Total DER blob ~8 KB.
  ECDSA root'ы (GTS R4, GlobalSign E46) сознательно НЕ включены —
  большинство P-384, мы умеем только P-256.
* **Parse-error в chain validation → best-effort** вместо fatal'а:
  раньше любая ошибка парса intermediate cert'а ломала handshake.  Теперь
  жёстко fail'имся только на `X509_CHAIN_ERR_SIG_INVALID` и
  `X509_CHAIN_ERR_EXPIRED` (доказанные ошибки безопасности).  Caller
  через `require_trust=true` в `https_get` может включить strict mode.
* **Новые автотесты:**
  * `scripts/test_stage47_ecdsa.sh` — **3/3 PASS**:  поднимает openssl
    s_server с ECDSA-P256 cert + cipher ECDHE-ECDSA-AES128-GCM-SHA256,
    AMPLIFY-OS делает live handshake.  Подтверждает реальную работу
    нашего ECDSA-on-SKE кода.
  * `scripts/test_stage47_send_recv.sh` — **6/6 PASS**:  end-to-end
    HTTPS GET к `openssl s_server -www` — handshake + `tls_send` GET +
    `tls_recv` response + HTTP parse → проверяем status=200,
    body_len>0.  Доказательство что TLS application data layer работает.
  * `scripts/test_stage47_internet.sh` — smoke с РЕАЛЬНЫМИ сайтами
    (example.com + cloudflare.com) через QEMU `-netdev user` (slirp NAT
    с DNS 10.0.2.3).  В моём WSL-окружении за корпоративным MITM-прокси
    получаем `code=302` (redirect от прокси), что подтверждает работу
    всего стека: DNS → TCP → TLS(ECDHE-RSA с прокси-сертом) → HTTP.
* **Дополнительный flag `AMPLIFY_TLS_INTERNET`** в `kernel64_main.c`
  активирует `[smoke 47.7]` блок: автоматический `https_get` на
  example.com + cloudflare.com в boot path (для CI-теста).
* **Все регрессии:** `test_stage47_crypto.sh` (68/68), `test_stage47_live.sh`
  (3/3), `test_stage47_modern.sh` (7/7) — проходят без правок.

**Подэтап 47.6 (21.05.2026) — Modern HTTPS первая итерация:**
SHA-1, P-256 (ECDH + ECDSA), strict chain validation, ECDHE-RSA cipher suite,
ALPN.  Подробности в §23.

**Этап 47 — Crypto + TLS (краткая выжимка, полный разбор в §23 ниже):**

* **Криптопримитивы** (`crypto_{cpu,sha,hmac,aes,chacha,rand,self_test}.c`,
  ~1800 строк): SHA-256/384/512 (FIPS 180-4), HMAC + HKDF + TLS 1.2 PRF
  (RFC 2104 / RFC 5869 / RFC 5246 §5), AES-128/256 software (FIPS-197 reference) +
  AES-NI fast path (inline asm `aesenc`/`aesenclast`/`aeskeygenassist`),
  AES-CBC + AES-GCM (software GF(2^128) GHASH bit-by-bit), ChaCha20-Poly1305
  AEAD (RFC 8439), Fortuna-style PRNG с reseed от RDRAND/RDSEED/RDTSC.
  CPUID-detect: AES-NI / PCLMULQDQ / RDRAND / RDSEED / SHA-NI.
  Self-test: 15 KAT-векторов (NIST FIPS-197 / RFC 4231 / RFC 8439).
* **Public-key crypto** (`crypto_{bn,rsa,curve}.c`, ~880 строк):
  generic 4096-bit bignum (128 × 32-bit limbs) с modexp (bit-by-bit reduction),
  RSA-PKCS#1 v1.5 verify (SHA-256/384/512), X25519 ECDH (RFC 7748 Montgomery
  ladder), Ed25519 verify (RFC 8032, ref10-style twisted Edwards). 4 KAT.
* **ASN.1 + X.509 + trust store** (`crypto_{asn1,x509,ca,ca_blob}.c` + `gen_ca_blob.sh`,
  ~600 строк + 2 KB DER): DER parser (tag/length/value, INTEGER/OID/UTCTime),
  X.509 v3 parser (TBS, SPKI, SAN, BasicConstraints), chain validation,
  trust store с 2 Mozilla root CA (ISRG Root X1 RSA-4096 + DigiCert Global
  Root G2 RSA-2048) — генерируется скриптом из `/etc/ssl/certs/` во время
  сборки (как mbedTLS/BearSSL). 4 KAT.
* **TLS 1.2 client** (`tls.{h,c}`, ~740 строк): полный handshake state machine
  ClientHello → ServerHello → Certificate → ServerHelloDone → ClientKeyExchange +
  CCS + Finished → server CCS + Finished.  Один cipher suite:
  `TLS_RSA_WITH_AES_128_GCM_SHA256` (0x009C).  Record layer (cleartext +
  GCM-encrypted, AAD = seq+type+ver+len, nonce = implicit_iv+seq).  RSA-encrypt
  premaster через PKCS#1 v1.5 type 2.  KDF: master_secret + 40-byte key_block
  через TLS 1.2 PRF-SHA256.  SNI extension. Finished verify_data через
  PRF + running SHA-256 от handshake messages. `close_notify` alert.
* **HTTPS GET** (`https.{h,c}`, ~150 строк): обёртка над DNS + TCP + TLS.
  `https_get(url, body, status, require_trust)`.  Browser app в `apps.c`
  понимает `https://` префикс и автоматически идёт через TLS.
* **TCP listen/accept** (расширение `tcp.c` + `sock.c`): single-connection
  server-side.  Состояния `TCP_STATE_LISTEN` и `TCP_STATE_SYN_RCVD` добавлены,
  `tcp_listen(port)` / `tcp_accept(peer_ip, peer_port, timeout_ms)`.
  `sock_sys_listen`/`sock_sys_accept` больше не возвращают `ENOSYS`.
* **Shell-команда `https <url>`**: HTTPS GET + статус + первые 256 байт body
  (для интерактивных тестов).  Альтернативное имя `https-test` (для скриптов).
* **Smoke в boot path** (`kernel64_main.c`): `[smoke 47.1]`..`[smoke 47.5]`
  блоки в serial — feature detection, self-test PASS, GCM round-trip,
  X25519 ECDH alice/bob match, X.509 self-signed verify, `tcp_listen` OK.
  И **`[smoke 47.5 live]`** — реальный TLS handshake к 10.0.2.2:4443
  (если на host'е запущен `openssl s_server`).
* **Автотесты:**
  - `scripts/test_stage47_crypto.sh` — **58/58 PASS** (исходники + сборка +
    grep символов + headless QEMU smoke с парсингом serial + регрессия 46).
  - `scripts/test_stage47_live.sh` — **3/3 PASS** end-to-end live test:
    стартует `openssl s_server -tls1_2 -cipher AES128-GCM-SHA256` с
    self-signed RSA-2048 cert на host'е WSL, запускает QEMU с `-netdev user`
    (host доступен как 10.0.2.2), kernel выполняет handshake к 10.0.2.2:4443.
    Подтверждено: real ServerHello + Certificate + RSA premaster encrypt +
    KDF + GCM Finished — всё работает с реальной OpenSSL-стороной.

**Сборка & boot подтверждены:** `make ARCH=x86_64` собирает kernel ~430 KB
(+~120 KB от crypto+TLS относительно 46), 13 новых .o.  Boot до login screen
≈ 6.5 сек на TCG, +0.3 сек когда crypto_init крутит KAT.

**Что закрыто в этапах 47.6 / 47.7 (раньше было отложено):**

- ✅ **ECDHE-RSA P-256** (47.6) и **ECDHE-ECDSA P-256** (47.7) — оба cipher
  suite поддерживаются.  Cloudflare/Google теперь handshake'ятся.
- ✅ **SHA-1** (47.6) — для legacy CA (Baltimore CyberTrust Root).
- ✅ **Полная strict chain validation** (47.6) — time-check через RTC,
  KeyUsage, BasicConstraints+pathLen, ExtKeyUsage, multi-intermediate
  walking.

**Сознательно НЕ реализовано в этапе 47 (ещё отложено):**

- **TLS 1.3.**  Принципиально другой record layer (одна handshake-fly,
  всё после ServerHello encrypted), другой KDF (только HKDF), encrypted
  extensions, 0-RTT, post-handshake messages.  ~1000 дополнительных строк.
  Большинство современных сайтов всё ещё поддерживают TLS 1.2 backwards
  compat, так что не критично.
- **ECDSA-P384** для root CA с P-384 ключами (GTS R4, GlobalSign E46).
  Эти CA имеют parallel RSA roots (GTS R1, GlobalSign R3) — мы их и
  используем.  Для полной независимости — отдельный труд ~300 строк.
- **CRL / OCSP revocation.**  Целая отдельная инфраструктура (HTTP fetch
  CRL, кеш, валидация).  Современные TLS делают через OCSP stapling —
  отложим до момента, когда найдём реальный use-case.
- **Session resumption, ALPN, client cert auth** — TLS extensions.
- **PCLMULQDQ-accelerated GHASH.**  Bit-by-bit умножение в GF(2^128)
  работает, но медленно (~50× slower aппаратного).  `pclmulqdq` через
  inline asm дал бы significant speedup для bulk-encryption (HTTPS streams).
- **AVX2 / SHA-NI batch crypto.** Аналогично — micro-opt.
- **Forge / Amply over HTTPS** (этап 47 в roadmap'е упоминал HTTPS-репо для
  Amply).  Технически готово — `https_get` доступен, но пакетный менеджер
  пока не использует.  Lightweight extension для следующего этапа.

**Этап 46 — sandbox-runtime (краткая выжимка, полный разбор в §23 ниже):**

* **Пять видов namespace'ов** (`kernel/arch/x86_64/ns.{c,h}`): PID, UTS,
  mount, net, user. Каждый с refcount + CoW через `ns_unshare(flags)`.
  `task_t` получил поле `ns_set_t* ns` — указатель на «набор пяти»
  namespace-объектов; на root по умолчанию, в boxe — отдельная копия
  тех namespace'ов, для которых `CLONE_NEW*` бит выставлен.
* **Cgroups v2** (`kernel/arch/x86_64/cgroup.{c,h}`): иерархические
  лимиты CPU-time-ns, RAM bytes, IO bytes, PIDS count. Учёт через
  `cgroup_charge_*` с поднятием по parent-chain — отказ в любом
  родителе отклоняет операцию. Подключено: `task_yield` чарджит
  CPU-ns в cgroup активной задачи; `task_create_*`/`task_exit`
  поддерживают pids-counter; `cgroup_tick` сбрасывает period budget.
* **Seccomp-фильтры** (`kernel/arch/x86_64/seccomp.{c,h}`): whitelist
  до 128 syscall-номеров, действие по умолчанию ALLOW/ERRNO/KILL,
  STRICT-режим (только read/write/exit/sigreturn). Готовые профили:
  `default`, `strict`, `net`, `compute`. Проверка вшита в
  `syscall_dispatch` ДО switch'а через `seccomp_gate(args)` —
  блокированный syscall возвращает -EPERM или вызывает `task_exit(137)`.
* **Box-runtime** (`kernel/arch/x86_64/box.{c,h}`): `box_run(spec, log)`
  загружает ELF из VFS, создаёт vm_space, копирует namespace'ы,
  создаёт cgroup с лимитами, прикрепляет seccomp-фильтр, регистрирует
  задачу. `box_list()` отдаёт live-метрики (mem_used, cpu_total_ns,
  seccomp_blocked) для GUI и `box list` shell.
* **Десять новых syscall'ов** в диапазоне 0x150..0x15A:
  UNSHARE/SETHOSTNAME/GETHOSTNAME/SECCOMP_LOAD/SECCOMP_ALLOW/CGROUP_*
  (CREATE/LIMIT/ATTACH/STAT)/NS_INFO/BOX_RUN. ABI совместимо с уже
  существующим syscall.h (диапазон не пересекается с 0x140..0x148 GPU
  и с 0x130..0x13B sockets). См. таблицу в `syscall.h:89-119`.
* **VFS / sock хуки**: `vfs_open_flags` дёргает `ns_mount_path_allowed`
  до lookup'а — задача в isolated mnt-ns получает -EACCES для путей
  вне allow-list. `sock_sys_socket` дёргает `ns_net_allowed` — задача
  в isolated net-ns не может создавать internet-сокеты.
* **Shell-команда `box`** (`kernel/arch/x86_64/shell.c`): подкоманды
  `run`/`run-strict`/`run-net`/`run-compute`/`list`/`ns`/`cg`/`test`.
  Цветной вывод, alias к глобальному mnemonic-реестру AMPLIFY-OS.
* **GUI-приложение Sandbox Manager** (`kernel/arch/x86_64/app_box.c`):
  окно 620×420 с тремя табами (Boxes/Namespaces/Cgroups), кнопками
  [Test][Refresh][Hello box][Strict box], строкой логирования и
  hotkeys (1/2/3 — табы, T/R/H/S — действия). Иконка `B` на десктопе
  + пункт "Sandbox Manager" в стартменю.
* **Smoke-блок в boot path** (`kernel/kernel64_main.c:536-626`): пять
  property-проверок (cgroup memory limit, seccomp strict, ns_unshare CoW,
  ns_summaries count, mnt-ns gate). Виден в serial как `[smoke 46]`
  блок ДО splash, чтобы headless-тест мог его читать.
* **Автотест `scripts/test_stage46_sandbox.sh`** — **41/41 PASS**:
  присутствие исходников + сборка + grep-symbols в kernel.bin + boot
  через QEMU+OVMF + property-проверки в исходниках + регрессия 45.

**Сборка & boot подтверждены:** `make ARCH=x86_64` собирает kernel
~308 KB (было 292 KB), 9 новых .o (ns/cgroup/seccomp/box/app_box +
extends в task/syscall/vfs/sock/shell/kernel64_main). Boot до login
screen ≈ 5.2 сек. Smoke-блок в serial:

```
[ns] root namespaces created (PID/UTS/MNT/NET/USER)
[cgroup] root cgroup created
[seccomp] subsystem initialized
[smoke 46] cgroup mem r1=0 r2=-1 oom_count=1 (OK)
[smoke 46] seccomp strict write=0 open=-2 blocked=1 (OK)
[smoke 46] ns_unshare(NEWUTS|NEWPID) diff_uts_pid=yes same_net=yes (OK)
[smoke 46] ns_summaries count=5 (OK)
[smoke 46] mnt-ns allow=yes deny=yes (OK)
[smoke 46] sandbox initialized, cgroup_count=1
```

**Сознательно НЕ реализовано в этапе 46 (отложено на 47/50/51):**

- **Полноценный bind-mount.** Mount-namespace сейчас работает как
  «список разрешённых path-prefix'ов»; одна и та же inode под двумя
  путями (как Linux bind-mount) требует переписи vfs_node_t —
  отложено на этап 51 (Unicode + локали + полный VFS).
- **PID-namespace c REAL re-mapping.** Сейчас «видимый PID» хранится
  отдельным полем `task_t.ns_pid_visible`, но `task_iterate` в WM/
  TaskMgr продолжает использовать глобальный tid. `ns_can_see_task`
  есть, но в TaskMgr пока не вшит — это требует refactor'а apps.c
  taskmgr_draw, отложено.
- **Loopback внутри изолированного net-ns.** Сейчас isolated net-ns
  даёт -EACCES на любой socket(); реальный loopback (127.0.0.1
  через kernel-side virtual netif) — этап 50 (VT-x), там же
  родится full virtio-net.
- **Реальный POSIX clone() с CLONE_NEW* флагами.** `AOS_SYS_CLONE`
  (0x120) сейчас по-прежнему создаёт thread (этап 40 group D); мы
  предоставили отдельный `AOS_SYS_UNSHARE` для namespace-операций
  + `AOS_SYS_BOX_RUN` как высокоуровневый «всё-в-одном». Слияние
  с CLONE_NEW* сделаем после реализации полноценного fork(2) на
  этапе 51 (когда libc дойдёт до fork-exec модели).
- **seccomp-bpf BPF-программ.** Текущая реализация — простой
  whitelist + `def_action`. Полноценный BPF-fineprint (с фильтрами
  по аргументам syscall'а) отложен до этапа 53 (WebAssembly), где
  у нас появится JIT-инфраструктура для безопасного выполнения
  user-supplied filter-программ.
- **OOM-killer / cgroup notifications.** Превышение mem_max сейчас
  возвращает -1 из cgroup_charge_mem; для полноценного «прибить
  процесс с самым большим RSS» нужна интеграция с reaper-кодом
  task_exit + memcg_oom event'ы. Отложено на этап 52 (системный
  журнал/embersd, там же будет crash-dump).

**Этап 45** — ✅ **Amply пакетный менеджер + полная FAT32 (subdirs + LFN).**
Подробности этапа 45 — в §20 (новый раздел), краткая выжимка:

**HOTFIX 20.05.2026 — FullHD 1920×1080 + 3D куб ускорен (15 FPS → плавно).**
Подробности — в §22. Краткая выжимка:

* **Разрешение по умолчанию 1920×1080** — поднято с 1024×768. Multiboot2
  framebuffer-тэг в `boot.asm` и UEFI GOP-target в `uefi/main.c` обновлены.
  Все слои (fb_init, doublebuf, compositor, WM, splash, login, desktop,
  apps) уже читали размер динамически из `fb_state()` — никаких других
  правок не понадобилось. Memory: 1920×1080×4 = 8.1 MB кадра в kernel
  heap, всё ещё в пределах 128 MB heap'а. QEMU-args обновлены:
  `-vga none -device virtio-vga,xres=1920,yres=1080` для UEFI/BIOS
  путей; `-global VGA.vgamem_mb=16` для legacy `-vga std` целей.
* **3D-куб 15 FPS → ровно (60+ FPS на TCG)** — три отдельные правки:
  1. **`window_t.wants_continuous_redraw`** (новое поле). WM в idle уходил
     в 10 FPS из-за HOTFIX 19.05.2026 (perf), а куб сам не генерирует
     mouse/kbd events — отсюда потолок 10 FPS с проскоками 30 FPS на
     случайных событиях. Теперь окно может попросить непрерывный
     рендер через флаг; `wm_main_loop` сканирует все видимые окна и
     форсит interactive-режим если кто-то его запросил. `app_glcube`
     ставит `win->wants_continuous_redraw = true`.
  2. **Инкрементальный rasterizer в `gles.c`**. Был ~60 ops/pixel
     (edge_fn × 3 = 24 ops + барицентрики + Z + interp_inv_w + N varying
     numerator = ~36 ops) + per-pixel `1.0/x` для perspective recovery.
     Стал ~15 ops/pixel: edge functions, Z, interp_inv_w и numerator'ы
     варингов теперь линейны по (px, py) — инициализируем в углу
     bounding box и инкрементируем `+= step_x` на пиксель / `+= step_y`
     на строку. Early-Z поднят перед `1/iw` и varying recovery —
     occlude'нутые пиксели не тратят div+N mul.
  3. **GL_NEAREST для куба + `*0.2f` вместо `/5.0f`** в `app_glcube`.
     Биленейный сэмплинг = 4 текселя × 4 канала × 4 op = 64 op/pixel,
     визуально незаметен на 64×64 шахматной текстуре под Lambert+fog.
     Уход на NEAREST даёт ~30 op/pixel в FS.

**HOTFIX 16.05.2026 — регрессия SMP boot после этапа 44 (FPU/SSE).**
Симптом: после стадии 44/45 любой `make ARCH=x86_64 run` зависал
сразу после строки `[smp] starting APs, total CPUs=4`, причём вся
QEMU-машина останавливалась (включая BSP). Корневая причина: на этапе 44
ядро стало собираться БЕЗ `-mno-sse`, GCC начал свободно использовать
xmm-регистры в любом C-коде, включая `ap_main64`. Но `smp_trampoline.asm`
(не менялся с мая 11) поднимает CR4 только с PAE — без OSFXSR /
OSXMMEXCPT и без CR0.MP|NE / FNINIT. Первая же SSE-инструкция в
`ap_main64` давала AP'у #UD → #DF → triple fault. QEMU c `-no-reboot`
интерпретирует triple-fault как fatal на всю VM и останавливает BSP
тоже. Фикс: добавлены те же CR0/CR4/FNINIT/LDMXCSR-шаги в `tramp_pm32`
секцию `smp_trampoline.asm`, что и в `boot.asm` (см. §21 для подробностей).
После фикса (на TCG): `[smp] cores up: 4/4`, smoke 45 проходит за ~6.9 сек,
25/25 тестов PASS.

**HOTFIX 19.05.2026 (perf) — лаги под KVM-on-VMware.**
Симптом: после перехода на KVM+host-cpu система визуально лагает —
курсор дрожит, окна перерисовываются скачками. Корневая причина —
WM форсил 50 FPS даже в idle, и каждый кадр включал в себя десятки
блитов с per-pixel bounds-check на больших окнах + 1024×768 store
по 4-байта в LFB. Под nested-virt КАЖДАЯ store-операция в VRAM
дороже на 1-2 порядка, и CPU ушёл в 100% постоянного рендеринга.

Три точечных фикса в hot path:

1. `kernel/arch/x86_64/wm.c:wm_main_loop` — idle-режим 10 FPS вместо
   50 FPS. На любое пользовательское событие (мышь/клавиатура/
   анимация) — рендер мгновенно. Часы в taskbar обновляются раз в
   секунду, 10 FPS более чем хватает. Экономия CPU при idle ≈ 5×.

2. `kernel/arch/x86_64/graphics.c:gfx_fill_rect` — qword-stride
   (2 пикселя за store вместо 1). Для фона рабочего стола
   1024×768 это 786k store → 393k store. Прямой 2× speedup на самой
   горячей функции графики (плюс GCC вектoризирует загрузку лучше
   на 64-bit регистрах).

3. `kernel/arch/x86_64/compositor.c:blit_with_opacity` — pre-clip
   рамки до inner loop (не 60 000 проверок per-pixel на 300×200
   окно) + fast-path для opacity=255 (~95% случаев). Ускорение
   per-window blit ≈ 3-5× под KVM.

Verified: `make run` (KVM+host+UEFI+4cores) проходит до login,
admin/admin → authenticated, wm_main_loop запускается, мышь и
клавиатура отзывчивы.

**HOTFIX 19.05.2026 — регрессия SMP boot **под KVM** (LDMXCSR без стека) +
оптимизация PCI scan.** После §21-фикса SMP работал под TCG, но под
`-enable-kvm -cpu host` triple-fault возвращался (особенно на nested-virt:
KVM-внутри-VMware). Диагностика трампа через прямой `out 0x3F8, al`
(буквы A..N после каждого шага) показала: AP падает между шагом 'G'
(CR4 PAE+OSFXSR set) и 'H' (LDMXCSR). Причина: §21-код делал
`push dword 0x1F80; ldmxcsr [esp]; add esp, 4` — но у AP после SIPI
**ESP = 0** (мы его явно не устанавливаем). Первый `push` уводит ESP в
0xFFFFFFFC. На VM с 1 ГБ RAM этот адрес ЛЕЖИТ ВЫШЕ физической памяти.
Под TCG запись по такому адресу молча проходит (TCG относится к
out-of-RAM-write терпимо). Под KVM = #PF → #DF → triple fault.
**Фикс:** грузим MXCSR прямо из памяти трампа без стека:
```asm
ldmxcsr [tramp_mxcsr_init - trampoline_start + TRAMPOLINE_PHYS]
; ... в data-секции:
tramp_mxcsr_init: dd 0x1F80
```
**Дополнительно — PCI scan кеширование** (`kernel/arch/x86_64/pci.c`):
До фикса `pci_find_device` / `pci_find_by_class` делали full-scan
256 × 32 × 8 = 65536 BDF на КАЖДЫЙ вызов. Под nested KVM каждый
port-IO = VMExit (~50-200 µs), 6 full-scan'ов × 130k port-IO ≈ 80 секунд
только на PCI-init. Теперь при первом `pci_scan_dump()` сохраняем все
найденные устройства в `g_pci_cache[PCI_CACHE_MAX=64]`, `find_*()` ищут
в нём. Плюс при пустой функции 0 пропускаем все 8 функций (Intel spec).
После фикса под KVM+host+4 cores+UEFI:
- `[smp] cores up: 4/4` без триплфолта
- В System Info видна реальная модель ЦП хоста (`Intel(R) Xeon(R) Gold
  6148 CPU @ 2.40GHz` при `-cpu host` под KVM)
- Полный набор инструкций реального ЦП (включая AVX-512F на Xeon Gold)
- Hypervisor: `KVMKVMKVM`

- **FAT32 драйвер переписан**: добавлены подкаталоги (любая глубина),
  Long File Names (UCS-2/UTF-16LE по 13 символов на dir-entry, 0x40-marker,
  rotate-1 checksum от 8.3 alias), авто-расширение цепочки кластеров на
  write, `fat32_mkdir`/`fat32_mkdir_p`/`fat32_iter_dir`. VFS-регистрация
  при mount теперь рекурсивно ходит по всем подкаталогам и регистрирует
  каждый файл с полным путём типа `/disk/Sanctum/Amply/Welcome/welcome.txt`.
  Обратная совместимость со старым 8.3 API сохранена.
- **Свой формат пакета `.amf`** (AMPLIFY File): 48-байтовый header
  ("AMF1" magic + ver + manifest_len + file_count + payload_len +
  compression + 16-байтовая signature) → manifest INI ([package]/[depends])
  → file_table (120 байт на файл) → payload. Подпись AMPLIFY-Sig-v1 — две
  FNV-1a 64-bit (с публичным seed и секретным AMPLY_KEY_DEFAULT). Запас по
  размеру signature[] позволит в этапе 47 заменить на Ed25519 без
  поломки формата.
- **Утилита host-сборки `tools/mkamf.c`**: рекурсивно собирает .amf из
  директории `amply_pkgs/<name>/{manifest.ini,files/<rel>}`. Файлы из
  `files/Sanctum/...` попадают в .amf как `/disk/Sanctum/...`.
- **Kernel-side `kernel/arch/x86_64/amply.{c,h}`**:
  * парсер manifest+file-table+payload, проверка подписи;
  * персистентная БД `/disk/Sanctum/Amply/db.txt`;
  * индекс репо из `amply_index.txt` в initrd (3 пакета встроены:
    `welcome`, `cyber-theme`, `notes-pack`);
  * рекурсивный install с обнаружением циклов зависимостей через DFS-стек;
  * транзакционный install: при ошибке распаковки откатываются все ранее
    распакованные файлы;
  * remove с проверкой обратных зависимостей;
  * upgrade-all (по сравнению версий в индексе), search, info, list,
    verify_installed, repo_url getter/setter.
- **Shell-команда `Amply`** (плюс алиас `amply`): `Amply install`/`remove`/
  `upgrade`/`search`/`info`/`list`/`verify`/`repo`/`repo-show`. Цветной вывод
  лога (зелёный для `[ok]`, красный для `[error]`, жёлтый для `[hint]/[skip]`).
- **GUI-приложение `Amply Center`** (в `apps.c`): окно 620×420, две колонки
  (каталог репо и/или установленные через таб-переключатель), журнал
  операций справа, toolbar [Установить][Удалить][Проверить][Обновить][Очистить лог].
  Hotkeys: Enter — установить выбранный, D — удалить, V — проверить,
  U — upgrade-all, L — clear log. Иконка 'A' на рабочем столе + пункт
  "Amply Center" в startmenu.
- **Smoke-тест в kernel boot path** (до splash): автоматический
  install→verify→remove над `cyber-theme` (с подтягиванием `welcome` как
  зависимость). Виден в serial-логе как `[smoke 45]` блок.
- **Автотест `scripts/test_stage45_amply.sh`** — **25/25 PASS**: сборка
  всех артефактов, AMF1 magic, install с резолвом зависимостей и
  распаковкой в подкаталоги (`/disk/Sanctum/themes/cyber/theme.cfg`),
  verify, remove, db_count tracking, round-trip Python-парсер с проверкой
  обоих FNV-подписей.

**Сборка и boot подтверждены:** `make ARCH=x86_64` собирает kernel
~292 KB, 3 .amf пакета (1.2-1.8 KB каждый, общий payload 3.2 KB), initrd
~250 KB. Boot до login screen 5160 мс. Smoke-блок в serial показывает:
welcome (2 файла) + cyber-theme (2 файла) распакованы в подкаталоги
`/disk/Sanctum/Amply/Welcome/` и `/disk/Sanctum/themes/cyber/`,
двойная FNV-подпись валидна, БД персистится в `/disk/Sanctum/Amply/db.txt`.

**Сознательно НЕ реализовано в этапе 45 (отложено на 47/51/57):**

- **Сжатие zstd / brotli / gzip** — поле `compression` в .amf header'е есть,
  но всегда 0 (NONE). Для базы пакетов в initrd 250 KB это OK; для реального
  репозитория zstd-стрим даст 3-5× сжатия. Подключим в этапе 51 вместе с
  WebAssembly-runtime (там же будет работать compression API).
- **Ed25519/RSA-подпись** — текущая двойная FNV-1a защищает от случайной
  модификации и от пакетов из чужих репозиториев (без знания AMPLY_KEY_DEFAULT
  второй FNV не подделать). Криптостойкая подпись — этап 47 (TLS+crypto).
- **HTTPS-скачивание удалённых .amf** — `amply_set_repo_url`/`repo_url`
  готовы, есть `http.c` (HTTP/1.0), но для репозитория нужен HTTPS — этап 47.
  Сейчас install работает с пакетами в initrd (`amf=amply_*.amf`).
- **post-install скрипты в manifest** — формат `[post-install]` зарезервирован,
  но shell.c-интерпретатор пока его не выполняет (требует sandbox этапа 46).
- **self-hosted сборка `Amply build`** — отложена до этапа 57 вместе со
  всем self-hosted toolchain'ом. До этого .amf создаются host-утилитой
  `tools/mkamf` через make.
- **fork/exec/pipe/chdir/shm_open closing-out** — в roadmap фазы 3 они
  значились "закрываются в этапе 45". На текущем этапе 45 я этим не
  занимался — сосредоточился на пакетном менеджере как самостоятельном
  завершённом блоке. Перевешиваем эти долги на этап 46 (sandboxes), где
  fork/exec и chdir и так будут переделываться под namespaces.

**Этап 44** — ✅ 3D-стек (OpenGL ES 2.0 software). См. §19.

**Этап 42** — ✅ Compositing с альфа-каналом, тенями, скруглениями. См. §14, §15.

- **Активирован FPU + SSE/SSE2 в ядре.** До этапа 44 ядро AMPLIFY-OS
  собиралось с `-mno-mmx -mno-sse -mno-sse2` — GCC не мог использовать
  xmm-регистры. В этапе 44:
  - `kernel/arch/x86_64/boot.asm` — оба пути (GRUB `_start` и UEFI
    `_efi_start`) теперь полностью инициализируют FPU: CR0.MP|NE=1,
    CR0.EM|TS=0, CR4.OSFXSR|OSXMMEXCPT=1, FNINIT + LDMXCSR(0x1F80).
  - `Makefile.x86_64` — флаги `-mno-mmx/-mno-sse/-mno-sse2` выпилены,
    GCC свободно использует xmm в любом kernel-файле (включая 3D-стек).
  - `kernel/arch/x86_64/task.{h,c}` — `task_t.fpu_state` (512 байт,
    16-byte aligned, отдельная kmalloc_aligned-аллокация). `task_yield`
    делает FXSAVE/FXRSTOR. `task_exit` освобождает блок.
  - `kernel/arch/x86_64/isr.asm` — `isr_common` оборачивает C-диспатчер
    в FXSAVE/FXRSTOR на 512-байтовом scratch'е на стеке. Это
    защищает userspace/kernel FPU-state от corruption любым IRQ.
  - `kernel/arch/x86_64/syscall_entry.asm` — то же самое для SYSCALL
    пути (заодно исправлен alignment-баг — до SSE-эпохи никто не
    замечал, что `call syscall_dispatch` вызывался с RSP в 8 mod 16).
- **Программный 3D-стек в `kernel/arch/x86_64/gles.{c,h}` + `gles_math.c`:**
  - Полная математика на `float` через SSE (SQRTSS, обычные mulss/addss):
    sinf, cosf, tanf (range-reduction + полиномиальная аппроксимация),
    sqrtf (одна SSE-инструкция), fabsf (bit-trick), fmodf, mat4 (identity,
    translate, scale, rotate_xyz, perspective, lookat, mul, mul_vec4),
    vec3 (normalize, dot, cross, sub).
  - **EGL-подобный API:** `aos_egl_create/destroy/make_current/swap_buffers/
    resize`. Контекст владеет Z-буфером (float[w*h]).
  - **GLES2-подобный state machine:** `gl_viewport`, `gl_clear_color/depth/
    color/clear`, `gl_enable/disable` (DEPTH_TEST, CULL_FACE, BLEND),
    `gl_front_face`, `gl_cull_face`, `gl_blend_func`, `gl_use_program`,
    `gl_set_uniforms`, `gl_bind_texture`, `gl_vertex_attrib_pointer`,
    `gl_enable_vertex_attrib`, `gl_draw_arrays`, `gl_draw_elements`.
  - **Шейдеры — указатели на C-функции** (как SwiftShader-Lite/ANGLE для
    embedded), сигнатура: `vs(attribs, uniforms, out gl_Position, out
    varyings)` и `fs(varyings, uniforms, sampler0, out gl_FragColor)`.
    Это даёт приложениям полную свободу per-vertex/per-fragment кода
    без необходимости иметь рантайм-компилятор GLSL (десятки тысяч
    строк, заведомо нецелесообразно для kernel-mode).
  - **Растеризатор** (Pineda 1988 edge-functions):
    transform VS → perspective divide → viewport-transform → backface
    culling по знаку 2D-area → bounding-box scan-line → барицентрики
    → Z-test → perspective-correct interp varying'ов через 1/w →
    fragment shader → α-blend (src-over-dst). Stat'ы: triangles_total/
    culled/drawn, pixels_shaded, draw_calls.
  - **Текстуры:** GL_NEAREST / GL_LINEAR (билинейная фильтрация по 4
    texel'ам), GL_REPEAT / GL_CLAMP_TO_EDGE wrapping.
- **Демо-приложение `app_glcube` (`kernel/arch/x86_64/app_glcube.c`):**
  Окно 360×280, текстурированный куб (6 граней × 2 треугольника + Z-buffer +
  backface culling). Каждый кадр обновляются углы поворота вокруг X и Y,
  пересчитывается MVP-матрица (model = rotate_y * rotate_x, view = lookat,
  proj = perspective 60° FOV). Vertex shader умножает позицию на MVP и
  трансформирует нормаль через model-матрицу. Fragment shader делает
  Lambert (ambient 0.25 + diffuse 0.75 × max(dot(N,L), 0)) + сэмплинг
  процедурной шахматной текстуры 64×64 (оранжевый+золотой в стилистике
  AMPLIFY-OS) + лёгкий fog по глубине. Внизу окна — HUD с FPS/треугольниками/
  пикселями. Регистрация: иконка "3D куб" на рабочем столе (`launch_glcube`)
  + пункт "3D куб (GLES)" в startmenu.

**Сборка & boot подтверждены:** `make ARCH=x86_64` собирает kernel
288 KB / ISO 9.6 MB без ошибок (только warning'и на indentation —
несущественно). QEMU headless boot проходит все подсистемы вплоть до
login screen (5160 мс uptime). В CPU features detection строкой `FPU SSE
SSE2` подтверждается, что наш CR4.OSFXSR работает.

**Сознательно НЕ реализовано в этапе 44 (отложено на 50/51):**

- **VirGL/3D mode для virtio-gpu.** Текущий driver работает в 2D-mode
  (см. virtio_gpu.c). Полноценная аппаратная 3D-композиция требует
  VirGL renderer на хосте и virtio-gpu-pci с `-device virtio-gpu-pci,
  virgl=on`. Это даст:
  - Per-window GPU resources с аппаратной Z-order композицией.
  - Реальную тройную буферизацию через GPU compose-resource.
  - Gaussian-blur тени как fragment shader.
  - Glassmorphism-blur фона.
  - Partial-flush через `drm_present_primary_rect()`.
  Все эти долги из этапов 41/42 формально переносятся на этап 50
  (Hypervisor VT-x) — там придётся либо включить VirGL, либо иметь
  свой 3D-стек для гостевых ОС.

- **Полноценный GLSL-компилятор.** Шейдеры остаются C-функциями.
  Парсер GLSL → AST → IR → SSA → SSE-codegen — это 30K+ строк
  Mesa/SwiftShader. Если когда-то будем портировать реальный браузер
  / Doom 3 — придётся добавить (например, через wasm-портирование
  glslang или SwiftShader).

- **SIMD-batch фрагментный шейдер.** Сейчас FS вызывается per-pixel.
  Real-world растеризаторы (llvmpipe) обрабатывают quads (2×2 пикселя)
  одновременно через SSE/AVX. Это дало бы 4× speedup. Отложено.

- **Tile-based rendering.** Стандартный приём embedded GPU (PowerVR,
  Adreno): экран делится на тайлы, каждый рендерится в L1 cache.
  Полезно для больших viewport'ов. Отложено.

- **Геометрический клипинг.** Сейчас при пересечении треугольника с near
  плоскостью мы целиком его reject'им (грубо). Полноценный
  Sutherland-Hodgman clip-triangulation — отложено. Для куба в
  центре сцены это работает; для near-flythrough'ов сломается.

**Этап 42** — ✅ Compositing с альфа-каналом, тенями, скруглениями
(с HOTFIX 15.05.2026 — см. ниже). Модуль
`kernel/arch/x86_64/compositor.{c,h}` реализует:
- **Direct-to-doublebuf рендер.** Изначально в этапе 42 был задуман
  третий compose-буфер (тройная буферизация), но на QEMU TCG он давал
  3 МБ memcpy/кадр и тормозил всё ядро так, что PS/2 8042 терял пакеты
  мыши (см. блок «HOTFIX» ниже и §18). Сейчас compositor рисует прямо
  в doublebuf, `compositor_target()` возвращает `doublebuf_target()`.
  Compose-буфер не аллоцируется. **Полноценная тройная буферизация
  (для tearing-free при анимациях) отложена на этап 44** — там она
  станет почти бесплатной через GPU compose-resource в VirGL.
- **Per-pixel alpha compositing:** окна рисуются с учётом window_t.opacity +
  per-pixel alpha. `blit_with_opacity()` модулирует alpha каждого пикселя на
  opacity окна.
- **Тени (быстрый путь).** По умолчанию `compositor_draw_shadow()` уходит
  в `gfx_drop_shadow` — несколько концентрических прямоугольников с
  убывающей альфой, ~200 µs/окно. Полноценный 3-pass box blur
  (приближение Gaussian, scratch-буфер 400×400) сохранён, но скрыт за
  `#ifdef COMPOSITOR_HEAVY_SHADOW` — это был узкий место на QEMU TCG
  (~30–50 мс/окно). **Реальный Gaussian-blur запланирован как GPU-шейдер
  в этапе 44** (см. §15).
- **Скруглённые углы с антиалиасингом:** `compositor_fill_round_rect_aa()`
  использует 4×4 supersampling для subpixel coverage на углах.
  `compositor_stroke_round_rect_aa()` рисует 1px скруглённую рамку с AA.
- **Анимации открытия/закрытия:** ease-out cubic, scale 0.85->1.0 + fade
  при открытии (180 мс), scale 1.0->0.92 + fade при закрытии (120 мс).
  `wm_add_window()` автоматически запускает open-анимацию,
  close-button — close-анимацию (окно скрывается после завершения).
  Прогресс продвигает только `compositor_update_animations()` — он
  вызывается строго один раз за кадр из `render_frame()`. Для
  read-only проверки «есть ли активная анимация» из main loop —
  `compositor_peek_animation_active()`.
- **Dirty-rect tracking (инфраструктура):** `compositor_mark_dirty()`
  собирает до 32 rect'ов за кадр, merge в bounding box. Сейчас используется
  только для статистики (full flush всегда). Реальный partial-flush через
  `drm_present_primary_rect()` поедет в этапе 44 вместе с GPU-композицией.
- **Shell-команда `compstat`:** показывает frames_total/partial/full,
  dirty_rects, anims_started/completed.
- **Graceful fallback:** если `compositor_init()` не удался — WM продолжает
  работать через старый doublebuf-путь (`window_render` без эффектов).
  Автотест `scripts/test_stage42_compositor.sh` — **16/16 PASS**.

**HOTFIX 15.05.2026 — регрессия PS/2 мыши после этапа 42.**
После реализации compositor курсор замирал после первого движения, хотя
часы в taskbar продолжали идти. Корневая причина — рендер кадра вырос
до ~150 мс на QEMU TCG (3 МБ compose→doublebuf memcpy + 6-проходный box
blur + 4×4 AA), а PS/2 8042 имеет всего 1 байт буфера: за один кадр
мышь успевала прислать ~10 пакетов, доезжал ровно один, дальше
синхронизация `pkt_idx` ломалась. Фикс:
1. `compositor.c` — compose-буфер выпилен, рисуем прямо в doublebuf;
   тени по умолчанию через `gfx_drop_shadow` (box-blur под флагом
   `COMPOSITOR_HEAVY_SHADOW`). Добавлен `compositor_peek_animation_active()`.
2. `wm.c` — main loop использует peek вместо update (раньше анимации
   шли в 2× скорости из-за двойного `compositor_update_animations()`);
   частота рендера снижена со 100 FPS до 50 FPS (`pit_ticks() - last >= 2`).
3. `mouse.c` — `mouse_poll()` обёрнут в `cli`/`sti`, чтобы не гоняться
   с `mouse_irq()` за 8042 input register.

После фикса кадр стал ~10–15 мс, мышь работает плавно. См. подробный
разбор в §14 (пункты 19–22) и §18.

Следующий этап — **43 (Neural Icon Forge)**.

**Сознательно НЕ реализовано в этапе 42 (отложено на 43/44):**

- **Растровые иконки вместо glyph-символов.** Текущие иконки рабочего стола
  по-прежнему рисуются через 8×16 bitmap-шрифт (glyph). Переход на PNG-иконки
  через stb_image запланирован на этап 43 (Neural Icon Forge).

- **Per-window GPU resource'ы.** Каждое окно по-прежнему рисуется в CPU-буфер,
  а GPU получает уже скомпонованный кадр. Аппаратная композиция нескольких
  resource'ов на стороне host'а невозможна в virtio-gpu 2D mode.
  **Откладывается на этап 44** (VirGL/3D).

- **Полноценная тройная буферизация (tearing-free).** Compose-буфер был
  убран HOTFIX'ом, потому что CPU-копия 3 МБ/кадр слишком дорога. На
  этапе 44 (VirGL) tearing-protection вернётся в виде GPU
  compose-resource'а — host-side компоновщика, который меняет scanout
  атомарно на vblank без CPU-копии.

- **Gaussian-blur тени.** Код 3-pass box blur (приближение Gaussian)
  оставлен в `compositor.c` под `#ifdef COMPOSITOR_HEAVY_SHADOW`, но в
  релизе по умолчанию выключен — на CPU слишком дорог. **На этапе 44
  переедет в GPU-шейдер** (fragment shader через VirGL/OpenGL ES).
  Тогда blur станет ~0.1 мс/окно вместо 30–50 мс, и эффект включится
  обратно.

- **Blur для glassmorphism-эффектов.** Blur фона за полупрозрачным окном
  требует чтения уже отрисованного фона — дорого на CPU. Появится в
  этапе 44 вместе с GPU-шейдерами теней.

- **Real partial-flush через dirty rects.** Сейчас `mark_dirty()` собирает
  rect'ы только для статистики (`compstat`). Реальный частичный present
  (`drm_present_primary_rect()`) надёжно работает только когда есть
  отдельный compose-resource на GPU — то есть в этапе 44.

- **Анимация сворачивания/разворачивания.** Инфраструктура готова
  (`ANIM_MINIMIZE` в enum), но UI для minimize-кнопки отложен.

**Предыдущие завершённые этапы:**
- `41` — virtio-gpu + DRM. Modern virtio 1.0 PCI transport, 2D resource,
  TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. Fallback VESA. 46/46 тестов.
- `37` — Long Mode основного ядра (x86_64 с нуля).
- `38` — UEFI handoff. Один и тот же `build/kernel-x86_64.bin` грузится
  двумя путями: GRUB2 Multiboot2 (через `_start`, 32->64 трамплин) и
  UEFI/OVMF (через `_efi_start`, native 64-bit вход). Loader на gnu-efi
  (`uefi/main.c`) читает ELF c ESP, строит синтетический `mb2_info_t`
  из EFI-данных (mmap, GOP framebuffer, ACPI RSDP, initrd как module-тэг),
  вызывает `ExitBootServices()` и прыгает в ядро. `kernel_main64`
  не отличает эти два пути -- приходит одинаковая Multiboot2-структура.
- `40` — libc finalize: полный POSIX-слой для userspace (stat/dup/fcntl,
  sigaction, sockets, pthread+futex+TLS, libc.so + dynlink). 31/31 тестов.

**ВАЖНО для следующих агентов — нумерация:** в этой редакции документа
все этапы фазы 3 приведены к **нумерации из оригинального ТЗ пользователя**
(37=Long Mode, 38=UEFI, 39=зарезервирован/слит с 40, 40=libc, 41=virtio-gpu,
…, 58=Release 1.0). В ранней редакции CLAUDE.md роадмап начинался с
39=musl libc — это УСТАРЕЛО. Смотри актуальную таблицу в §15.

**Архитектура:** x86_64 Long Mode, ядро с нуля (не Linux), загрузка через
GRUB2 Multiboot2 **ИЛИ** UEFI/OVMF.
**Хост:** Windows + WSL2 Ubuntu (либо Debian). Все команды `make`, `qemu`,
скрипты — из WSL.

---

## 0. Идентичность проекта (НЕ ТРОГАТЬ — это ценное)

- **Имя ОС:** **AMPLIFY-OS**. В старых файлах встречается «MyOS v2.0» — это
  устаревшее. Брендирование AMPLIFY-OS + треугольный логотип
  (см. `kernel/arch/x86_64/splash.c`, `desktop.c`, `power.c`).
- **Уникальные имена системных папок** (вместо /etc, /var, /usr):
  - `/disk/Sanctum` — конфигурация и БД пользователей
  - `/disk/nexus` — программы и системные модули
  - `/disk/vault` — данные пользователей
  - `/disk/forge` — будущий пакетный менеджер
  - `/disk/fonts` — TTF-шрифты
  - `/usb` — точка монтирования USB
- **Мистические имена shell-команд** (стилистику расширять, не упрощать):
  `guide`=help, `shine`=clear, `voice`=echo, `pulse`=ping, `embers`=log,
  `matrix`=ps, `scan`=ls, `read`=cat, `engrave`=write, `delve`=cd,
  `carve`=mkdir, `erase`=rm, `cycle`=reboot, `freeze`=halt, `launch`=run,
  `lsmod`, `elfinfo`.
- **UI на русском.** Исходники в UTF-8, GCC при сборке транскодирует в
  CP1251 через `-finput-charset=UTF-8 -fexec-charset=CP1251`. Bitmap-шрифт
  `font8x16.c` — это полные 256 глифов CP1251 (см. п. 4).
- **Тон комментариев** в коде: подробные, на русском, объясняют ПОЧЕМУ, а
  не только ЧТО.

---

## 1. Сборка и запуск

```bash
# ПОЛНАЯ СБОРКА (ISO + 64 МБ FAT32 disk + esp.img):
make ARCH=x86_64

# Если ругается на права (старые .o от sudo):
sudo rm -rf build/x86_64 build/iso-x86_64 build/disk-x86_64.img
make ARCH=x86_64

# === GRUB-путь (BIOS-style, через ISO) ===
make ARCH=x86_64 run
# или:
qemu-system-x86_64 -cdrom build/myos-x86_64.iso -boot d \
  -drive file=build/disk-x86_64.img,format=raw,if=ide,index=0 \
  -m 256M -vga std \
  -audiodev none,id=audio0 -device AC97,audiodev=audio0

# === UEFI-путь (через OVMF, этап 38) ===
make ARCH=x86_64 run-uefi
# Зависит от: build/uefi/BOOTX64.EFI + build/kernel-x86_64.bin + initrd → build/esp.img
# QEMU: -bios OVMF.fd, esp на IDE slave (index=1), disk на IDE master (index=0)

# Headless-тест со скриншотами (если в WSL есть python3 + PIL)
python3 scripts/test_c5_splash.py

# Headless-тест без python3 — bash + Windows-Python для PIL
bash scripts/test_c5_splash_bash.sh
```

**Зависимости WSL / Debian:** `nasm`, `gcc`, `ld` (binutils), `make`,
`grub-mkrescue` (пакет `grub-common` + `grub-pc-bin`), `xorriso`,
**`dosfstools`** (mkfs.vfat для ESP), **`mtools`** (mmd/mcopy для записи
в FAT-образ без mount), `qemu-system-x86_64` (`qemu-system-x86`),
**`gnu-efi`** (заголовки `efi.h` + `crt0-efi-x86_64.o`),
**`ovmf`** (UEFI firmware для QEMU).
Опционально: `python3`, `python3-pil` для автотестов со скриншотами.

Одной командой на Debian/Ubuntu:
```bash
sudo apt install -y nasm gcc binutils make \
    grub-common grub-pc-bin xorriso \
    dosfstools mtools \
    qemu-system-x86 \
    gnu-efi ovmf \
    python3 python3-pil
```

**32-битный legacy путь** (для отладки): `make ARCH=i386 run`. Сосуществует
со x86_64 — отдельные деревья `kernel/arch/i386/` и `kernel/arch/x86_64/`,
`Makefile` диспетчирует по `ARCH=`, `Makefile.x86_64` — изолированный
набор флагов и целей.

---

## 2. Последовательность загрузки

### 2a. Две точки входа (этап 38) — `kernel/arch/x86_64/boot.asm`

| Путь | Entry point | ABI | Что делает |
|------|-------------|-----|-----------|
| **GRUB2 Multiboot2** | `_start` | 32-bit, EAX/EBX | CPUID Long Mode → строит PML4/PDPT/PD (4 GB identity) → PAE → EFER.LME → CR0.PG → lgdt → far jmp в 64-bit `long_mode_start` → `kernel_main64(magic, info)` |
| **UEFI (OVMF)** | `_efi_start` | native 64-bit, RDI=info | `cld` → заполняет те же `boot_pml4`/`boot_pdpt`/`boot_pd` (но 64-битным кодом, RIP-relative `lea`) → CR3 → lgdt → far retf на CS=0x08 → `kernel_main64(MB2_BOOT_MAGIC, info)` |

Развилка: ELF `e_entry` = `_efi_start` (UEFI читает оттуда), а GRUB перенаправляется
на `_start` через **Multiboot2 entry-address тэг (тип 3)** в `.multiboot` секции.
Сам `kernel_main64` НЕ знает каким путём пришёл — приходит одинаковый
`mb2_info_t` (для UEFI его синтезирует `uefi/main.c`).

### 2b. `kernel_main64` (одинаково для обоих путей)

1. `serial_init()` → `fb_init()` (VESA 1024×768×32)
2. `gdt64_init()` → `idt64_init()` → `pic_remap()` → `timer_init()` → `pit_init(100)`
3. `paging_init()` → `pmm_init()` → `heap_init()` → `vm_init()`
4. `vfs_init()` → `initrd_init_from_mb2()`
5. `ata_init()` → `fat32_mount()`
6. `users_init()` (читает `/disk/users.txt` если есть)
7. `audio_init()` → `swap_init()` → `cpu_detect()` → `acpi_init()` → `lapic_init()` → `ioapic_init()`
8. `pci_scan_dump()` → `net_init()` → ожидание DHCP-bound
9. `smp_init()` → `syscall_init()`
10. `keyboard_init()` → `mouse_init()`
11. `doublebuf_init()` → **`splash_show()`** → **`firstboot_run_if_needed()`** → **`login_run()`**
12. `wm_init()` → `desktop_init()` → `startmenu_init()` → регистрация иконок и пунктов меню → `wm_main_loop()` (не возвращается)

> Smoke-тесты (ATA read, FAT32 write, ICMP, DNS, TCP, AC97, pipe, signal,
> unix_socket, SHM, swap) описаны после `wm_main_loop()` в исходнике и
> остаются недостижимыми. Если нужны — выносить в отдельную ELF-программу
> или в pre-WM фазу.

### 2c. UEFI loader (`uefi/main.c`, этап 38)

Шаги efi_main:
1. `read_esp_file(L"\\kernel.bin")` → буфер в EfiLoaderData через SimpleFileSystem
2. `load_kernel_elf()`: парсит ELF64-headers, для каждого PT_LOAD —
   `AllocatePages(AllocateAddress, p_paddr)` + копирует данные + обнуляет
   BSS-хвост
3. `read_esp_file(L"\\initrd.img")` → `AllocatePages(AllocateMaxAddress=0xFFFFFFFF)`
   (MB2 module-тэг требует 32-бит адреса)
4. `query_gop()` — выбирает RGB-режим ближе к 1024×768
5. `find_rsdp()` — обходит `g_st->ConfigurationTable[]` (ACPI 2.0 GUID → ACPI 1.0 GUID)
6. `build_mb2_info_partial()` — пишет в 16 KB буфер тэги: basic_meminfo,
   framebuffer (с color_info R=16 G=8 B=0 для BGRA-памяти), acpi_new/old, module
7. Цикл из 5 попыток: GetMemoryMap → write mmap-тэг → ExitBootServices
8. **После EBS** инициализирует COM1 (`com1_init`, divisor=3 → 38400 baud),
   выводит финальный лог через прямые outb 0x3F8
9. SysV-вызов `entry(info_ptr)` → `_efi_start` забирает указатель в RDI

---

## 3. Память и задачи

- **PMM:** битмап-аллокатор, `pmm_alloc_page()` / `pmm_free_page()`.
- **Heap:** `kmalloc()` / `kfree()`, first-fit.
- **Paging:** 4-уровневая (PML4 → PDPT → PD → PT), идентичное отображение
  нижних 16 МБ + framebuffer.
- **VM spaces:** per-user `vm_space_t` со своим PML4. API:
  `vm_create_user()`, `vm_switch()`.
- **Задачи:** кооперативный round-robin (`task_create_user()`, `task_start()`,
  `task_yield()`, `task_exit()`). У каждой 16 КБ kernel-стека и сохранённый
  RSP. Первый запуск — через `task_first_run()` trampoline.
- **Syscalls:** `syscall_init()` ставит `LSTAR` → `syscall_entry`.
  Пользовательский режим стартует через `enter_user_mode(entry, rsp)`.
- **SysV ABI x86_64:** аргументы syscall в `RDI, RSI, RDX, R10, R8, R9`.

---

## 4. Графический стек

Всё рисование идёт через `gfx_target_t` (pixels + pitch). `gfx_screen()`
возвращает VESA framebuffer; `doublebuf_target()` — off-screen buffer;
`doublebuf_swap()` копирует его во VRAM.

**Примитивы (`kernel/arch/x86_64/graphics.c`):**
- `gfx_clear`, `gfx_fill_rect`, `gfx_stroke_rect`, `gfx_fill_rect_gradient`
- `gfx_fill_round_rect` (circle-test rounding)
- `gfx_drop_shadow` (приближение через box-fill)
- `gfx_draw_char`, `gfx_draw_string` — 8×16 bitmap-шрифт
- `gfx_blend` — alpha-blend src-over-dst

**Шрифт (`kernel/arch/x86_64/font8x16.{c,h}`):**
- Полный 256-символьный CP1251 (4096 байт): ASCII 0x20–0x7F + Ё (0xA8) +
  ё (0xB8) + А–Я (0xC0–0xDF) + а–я (0xE0–0xFF) + типографика 0x80–0x9F.
- `aos64_font_glyph(c)` — всегда валидный 16-байтный глиф (0..255).
- **Важно для модификаций:** Makefile.x86_64 транскодирует исходники
  UTF-8 → CP1251. Это значит, что в `const char* s = "Привет"` каждый
  русский символ — РОВНО 1 байт в `.rodata`, и `font_data[c*16]` сразу
  даёт правильный глиф. Без этого флага кириллица превращается в
  невидимые UTF-8 двухбайтовые последовательности (был баг до 37.6c-6).
- **Запрещённые в строковых литералах символы** (нет в CP1251 — билд
  упадёт): `→` U+2192, `×` U+00D7, box-drawings U+25xx. Заменять на
  `->`, `x`, `--`.

**Тема (`kernel/arch/x86_64/ui_theme.h`):** единая палитра «cosmic forge»:
- BG: `UI_BG_DEEP` 0xFF0A1028 → `UI_BG_DARK` 0xFF101830 → `UI_BG_MID` 0xFF182340
- Акценты: `UI_ACCENT_ORANGE` 0xFFF5A037 → `UI_ACCENT_MAGENTA` 0xFFC84190 → `UI_ACCENT_GOLD` 0xFFFFD200
- Текст: `UI_FG_PRIMARY` / `UI_FG_SECONDARY` / `UI_FG_DIM` / `UI_FG_OK` / `UI_FG_ERROR`
- Радиусы: `UI_RADIUS_S` 4, `UI_RADIUS_M` 8, `UI_RADIUS_L` 14
- Хелперы: `ui_lerp()`, `ui_lighten()`, `ui_darken()`

---

## 5. Файловая система FAT32

`kernel/arch/x86_64/fat32.{c,h}` — root read-only, short name 8.3,
один кэш-сектор FAT.

```c
bool    fat32_is_mounted(void);
int     fat32_mount(void);                       // регистрирует /disk/* в VFS
int64_t fat32_read_file (const char*, void*, uint64_t);
int64_t fat32_write_file(const char*, const void*, uint64_t);   // только в существующий файл
int64_t fat32_create_file(const char*, const void*, uint64_t);  // создать новый
int     fat32_delete_file(const char*);
int64_t fat32_save_file (const char*, const void*, uint64_t);   // upsert (create-or-overwrite)
```

Disk image: 64 МБ, кластер 512 байт (128k кластеров), создаётся через
`scripts/mkdisk-x86_64.sh` (`mkfs.fat -F 32 -s 1` + `mcopy`).

---

## 6. Firstboot / Login / Splash

Все три экрана — **графические** (рисуют в `doublebuf_target()`),
блокируются на ввод с клавиатуры.

- **`splash_show()`** (`splash.c`): 32-кадровая анимация (~1.3 с).
  Градиентный фон + supersampled треугольный логотип + прогресс-бар +
  `speaker_play_boot_melody()` (C-E-G-C6). Запускается сразу после
  `doublebuf_init()`.

- **`firstboot_run_if_needed()`** (`firstboot.c`): запускается только
  если `users_count() == 0`. 4 шага:
  1. **Welcome** — логотип, описание, кнопка «Начать (Enter)».
  2. **Policy** (лицензионное соглашение) — 4 пункта политики
     безопасности, кнопка «Принимаю (Enter)».
  3. **Create Admin** — поля `Имя пользователя`, `Пароль`, `Повторите
     пароль`. Tab переключает, Enter подтверждает. Валидация: имя
     `a-zA-Z0-9_-`, пароль ≥4, пароли совпадают. Вызывает `users_add()`
     + `users_save()`.
  4. **Success** — анимированный progress bar, автотаймаут ~2 с.

- **`login_run()`** (`login.c`): градиентный фон + логотип + поля имя/
  пароль. Tab переключает фокус, Enter подтверждает. Вызывает
  `users_verify(name, pass)`. После 5 неудачных попыток — пауза 2 с.
  Возвращает `login_session_t{ .name, .is_guest, .login_ms }`.

Весь текст этих экранов — на русском (см. правки этапа 37.6c-6).

---

## 7. Пользователи / безопасность

`kernel/arch/x86_64/users.{c,h}`:
- `users_init()` — читает `fat32_read_file("/disk/users.txt")`, парсит
  `name:hash\n`.
- `users_add(name, pass)` — FNV-1a 64-bit хеш пароля, добавляет в память.
- `users_verify(name, pass)` — сравнивает хеш.
- `users_save()` — пишет обратно через `fat32_save_file()`.
- Лимиты: 8 пользователей, имя ≤31 символ, пароль ≤63 символа.

---

## 8. Текстовый shell (text-mode fallback)

`kernel/arch/x86_64/shell.c` — 20 команд с уникальным AMPLIFY-OS-неймингом:
`guide`, `shine`, `voice`, `pulse`, `embers`, `matrix`, `scan`, `read`,
`engrave`, `launch`, `elfinfo`, `disk`, `shade`, `cycle`, `freeze`, `clock`
(+ алиасы `help`, `clear`, `echo`, `ls`, `cat`, `ping`, `run`, `reboot`,
`halt`, `date`, `exit`).

Заметки:
- `engrave <file> <text>` пишет через `fat32_write_file()` (только в
  существующие файлы).
- `launch <elf>` (`run` — алиас) загружает ELF64 из VFS через
  `elf64_load` + `task_create_user` и вызывает `task_yield` для первого
  прыжка в user-mode. См. `cmd_launch` в `shell.c`. Примеры:
  `launch /hello_libc.elf`, `launch /stage40_hello.elf`.
- `elfinfo <path>` — распечатывает ELF64-заголовок без запуска.
- `pulse <ip>` отправляет ICMP echo.
- Зарезервированные `delve`, `carve`, `erase`, `lsmod` — в списке guide
  стоят, но реализация появится в следующих этапах (требует chdir /
  mkdir-VFS / module loader).

---

## 9. GUI-приложения (`kernel/arch/x86_64/apps.c`)

12 приложений, зарегистрированных в desktop и start menu. Каждое
открывается через `app_*_open()` → `window_t*`.

| Приложение | Глиф | Акцент | Статус 37.6c-5/6 |
|------------|------|--------|-------------------|
| About      | `i`  | blue   | OK, переведено |
| Files      | `F`  | blue   | open (preview), delete, refresh |
| Notes      | `N`  | brown  | Ctrl+S → `/disk/NOTES.TXT`, load on open |
| Calc       | `+`  | cyan   | редизайн темы, большие кнопки |
| Term       | `>`  | teal   | полный 18-командный shell через `shell_exec_line` |
| Paint      | `P`  | red    | 12-цветная палитра, 3 кисти, save → `/disk/PAINT.PPM` |
| Embers     | `*`  | red    | uptime + CPU bars demo |
| Browser    | `W`  | blue   | HTTP GET, кнопка «Идти» |
| Info       | `s`  | green  | ЦП / ОЗУ / время работы / этап (рус.) |
| Clock      | `C`  | cyan   | RTC live |
| Matrix     | `M`  | green  | memory stats |
| TaskMgr    | `T`  | red    | реальный список через `task_iterate` + `wm_iterate` |

Оконная система: `window_t` имеет `.x, .y, .w, .h, .title`, колбэки
`on_draw`, `on_click`, `on_key`, `.user_data` для состояния. WM
композитит все окна + панель задач + иконки рабочего стола + курсор в
doublebuf.

---

## 10. Сетевой стек

- `rtl8139.c` — драйвер PCI NIC, IRQ 11 через IOAPIC
- `netif.c` — один интерфейс, ARP-таблица, IP/MAC/gw
- `dhcp.c` — клиент: DISCOVER → OFFER → REQUEST → ACK
- `dns.c` — резолвер с кэшем и списком серверов [48.1]: DHCP1 -> DHCP2 ->
  gateway -> 8.8.8.8 -> 1.1.1.1 -> 8.8.4.4, по 2 попытки на сервер по 1.5 с,
  кэш 32 записи с TTL, случайный src-порт/ID, пропуск CNAME,
  IPv4-литералы без запроса.  API: `dns_resolve`, `dns_set_server`,
  `dns_set_server2`, `dns_cache_flush`
- `tcp.c` — стейт-машина + [48.1] ring-буфер RX 64 KB с реальным окном,
  MSS 1460 в SYN, случайные ISN/порт, ретрансмит SYN и данных,
  отброс дубликатов и dup-ACK. API: `tcp_connect`, `tcp_send`, `tcp_recv`,
  `tcp_close`, `tcp_listen`, `tcp_accept`
- `http.c` — HTTP/1.1 клиент [48.1]: `http_get`, `http_get_ex` (с заголовками),
  `http_get_follow`, chunked-декодер `http_dechunk`, разбор заголовков
  `http_find_header`, разрешение относительных URL `http_resolve_url`
- `https.c` — HTTPS поверх `tls.c` + [48.1] `net_fetch()` — единая загрузка
  http/https с кросс-схемными редиректами и автоподбором схемы
- `icmp.c` — echo request/reply

---

## 11. Аудио

- `speaker.c` — PC speaker через PIT канал 2. `speaker_beep(freq, ms)`,
  `speaker_play_boot_melody()` (C-E-G-C6).
- `ac97.c` — Intel AC97, играет 48 kHz стерео int16 через `ac97_play()`.
- `audio.c` — высокоуровневый враппер `audio_play()` — принимает любой
  sample rate, ресемпл в 48 kHz nearest-neighbor.

---

## 12. История этапов (что уже реализовано)

**Фаза 1 (этапы 1–7):** базовое i386 ядро — boot, GDT/IDT, PMM, VFS,
ATA, FAT32 RO. Описано в устаревших `STAGE*_COMPLETE.txt` и `docs_STAGE*.md`.

**Фаза 2 (этапы 8–32):** i386 фичи — heap, paging, VM, tasks, IPC, ELF,
network, audio, USB, dynlink, security, wm/desktop/apps, TTF, PNG/JPEG,
AC97 WAV-плеер. **Старое i386-дерево `kernel/arch/i386/` ещё работает**
через `make ARCH=i386`.

**Фаза 2.5 (этапы 33B/33C):** UEFI PoC (`uefi/main.c`) и PoC 64-битного
hello-world (`kernel64/kmain.c`) — обе демки **поглощены этапами 37 и 38**
(старая UEFI-info-демка переписана в полноценный loader; 64-bit hello из
`kernel64/` устарел, его роль выполняет основное `kernel/arch/x86_64/`).

**Фаза 3 — этап 37:** ✅ ЗАВЕРШЁН. Перенос ВСЕГО функционального ядра
в Long Mode:
- `kernel/arch/x86_64/` — параллельное дерево (boot.asm, gdt.c, idt.c,
  isr.asm, irq.asm, syscall.c, user_mode.asm, smp_trampoline.asm и т.д.).
- GDT64 + 4-уровневые таблицы страниц, CR4.PAE → EFER.LME → CR0.PG.
- Multiboot2 вместо Multiboot1.
- `Makefile.x86_64` — изолированные флаги, `make ARCH=x86_64`
  становится дефолтом.
- Подэтапы 37.1–37.6:
  - **37.1** — скелет: boot, gdt, idt, fb, font, doublebuf, графика.
  - **37.2** — paging, pmm, heap, vm, task, syscall, user mode.
  - **37.3** — драйверы: ata, fat32, pci, rtl8139, ac97.
  - **37.4** — userspace ELF (hello, counter), initrd.
  - **37.5** — IPC (pipe, signal, unix_socket, shm), swap.
  - **37.6** — UI: wm, desktop, startmenu, apps, splash, firstboot, login.
  - **37.6c-5** — все 12 приложений финализированы, единая тема, FAT32
    create/delete.
  - **37.6c-6** — **ЛОКАЛИЗАЦИЯ:** полный CP1251 шрифт (256 глифов),
    `-fexec-charset=CP1251` в Makefile, перевод splash/firstboot/login/
    about/desktop/startmenu/apps на русский. Тест:
    `scripts/test_c5_splash_bash.sh` подтверждает читаемость всех
    русских строк на 9 скриншотах.

**Фаза 3 — этап 38:** ✅ ЗАВЕРШЁН. UEFI handoff в основной путь.
- Полностью переписан `uefi/main.c` (~700 строк): SimpleFileSystem reader,
  ELF64-parser, GOP query, RSDP locator (через `g_st->ConfigurationTable`),
  построитель синтетического `mb2_info_t` (basic_meminfo + framebuffer +
  acpi + module + mmap + end), цикл GetMemoryMap → ExitBootServices.
- Добавлена 64-битная точка входа `_efi_start` в `kernel/arch/x86_64/boot.asm`
  (заполняет boot_pml4/pdpt/pd RIP-relative-lea'ми, грузит свой gdt64,
  far-retf, jumps в kernel_main64).
- В Multiboot2 header добавлен **entry-address тэг (тип 3)** в `.multiboot`
  секции, который указывает GRUB на `_start` (старый 32-bit вход).
  `linker_x86_64.ld` поменял `ENTRY(_start)` → `ENTRY(_efi_start)` —
  UEFI читает ELF e_entry. Так один и тот же `kernel-x86_64.bin`
  поддерживает оба пути загрузки.
- `Makefile.x86_64` обзавёлся целями `bootefi`, `esp`, `run-uefi`.
  `build/esp.img` — FAT32 64 MB с `/EFI/BOOT/BOOTX64.EFI`, `/kernel.bin`,
  `/initrd.img`. QEMU: `-bios OVMF.fd`, ESP на IDE-slave (index=1),
  disk-x86_64.img на IDE-master (index=0) — наш ATA-драйвер цепляет
  только master, OVMF перечисляет оба.
- **Критичные баги, которые были и исправлены** (стоит запомнить):
  1. **`com1_init()` пере-EBS убивал OVMF UART-state** — TerminalDxe мирорил
     ConOut в Serial используя свою конфигурацию 16550; наша
     перезапись DLAB/divisor десинхронизировала драйвер, выливалось в
     поток U+BFAF на serial. Фикс: COM1-инициализация ТОЛЬКО после
     ExitBootServices.
  2. **`objcopy -j` без `-j .rodata`** — в современных gnu-efi
     `elf_x86_64_efi.lds` не мерджит .rodata в .data (как старые),
     поэтому все L"..." CHAR16-строки исчезали из BOOTX64.EFI →
     OutputString читал мусор. Фикс: добавлен `-j .rodata` в
     `uefi/Makefile`.
  3. **DF=1 при входе в `_efi_start`** — UEFI не гарантирует DF=0,
     `rep stosq` мог пойти в обратную сторону. Фикс: `cld` в начале
     `_efi_start`.

**Фаза 3 — этап 40:** ✅ ЗАВЕРШЁН. libc finalize — полный POSIX-слой для userspace.

> Примечание по нумерации: в ранней редакции roadmap был отдельный stage 39
> «musl libc». После сверки с ТЗ пользователя принята нумерация, где
> 37=Long Mode, 38=UEFI, **39=зарезервирован и слит с 40**, 40=libc,
> 41=virtio-gpu … (полный список в §15). Номер 39 больше нигде не
> используется, агентам не следует «приступать к этапу 39» — идти сразу
> на 41.

Что было закрыто:

- **Группа A:** новые syscalls 0x110..0x115 (stat, fstat, mkdir, dup,
  dup2, fcntl), расширение `AOS_SYS_OPEN` (0x103) поддержкой O_CREAT/
  O_TRUNC/O_APPEND/O_EXCL через `fat32_create_file`/`fat32_save_file`,
  переработка `vfs.c` с fd_kind enum (UNUSED/FILE/SOCKET/PIPE),
  refcount на fd-entry, `ungetc` (1-символьный pushback в `FILE_impl`).
- **Группа B:** syscalls 0x116..0x118 — sigaction/kill/sigreturn.
  Per-task signal state (`sig_handlers[32]`, `sig_pending`, `sig_mask`,
  `sig_restorer`) в `task_t`. Новый модуль `kernel/arch/x86_64/sig.c`
  (~250 строк) строит signal-frame в user-stack и перехватывает
  return context в `syscall_entry.asm`. Trampoline
  `libc-x86_64/src/signal/sigreturn.S` выполняет handler и возвращается
  через AOS_SYS_SIGRETURN. Page fault из CPL=3 → SIGSEGV.
- **Группа C:** sockets 0x130..0x13B. Новый модуль
  `kernel/arch/x86_64/sock.c` — пул из 16 сокетов, TCP client поверх
  существующего `tcp.c` (один активный сокет), UDP с кольцевыми
  буферами. listen/accept возвращают ENOSYS (отложено P2).
- **Группа D:** pthread через 0x120..0x124 (clone, futex_wait/wake,
  set_tid, join). `task_create_thread()` делит `vm_space` с родителем
  через `vm_retain`; TLS через FS.base MSR (0xC0000100); сохранение/
  восстановление FS.base в `task_yield`. Новый
  `kernel/arch/x86_64/futex.c` — хеш-таблица 64 bucket'ов, spin-yield
  блокировка (на кооперативном шедулере). Libc
  `libc-x86_64/src/pthread/pthread.c` — `pthread_create` через
  AOS_SYS_CLONE с TLS/stack аллокацией, fast-path mutex через CAS,
  slow-path через futex, `pthread_self` через FS:[0].
- **Группа E:** libc.so + dynlink x86_64. Параллельная PIC-сборка
  libc из тех же источников через LIBC_PIC_CFLAGS (-fPIC). 
  `build/sysroot/usr/lib/libc.so` — ELF64 shared object, 75 KB,
  с PT_DYNAMIC (SONAME, HASH, GNU_HASH, RELA, JMPREL, 47 RELATIVE).
  Новый `kernel/arch/x86_64/dynlink.c` — парсер PT_DYNAMIC, symbol
  lookup, relocations R_X86_64_RELATIVE/GLOB_DAT/JUMP_SLOT/64.
  Loadable через явный `dynlink_load_so()`; runtime-hookup для
  PT_INTERP в `elf.c:elf64_load` отложен (рискованное изменение,
  требует отдельной итерации).
- **Группа F:** 6 мигрированных userspace-программ (префикс
  `stage40_`): hello, elftest (обе функциональны), forktest, exectest,
  cwdtest, shmtest (последние 4 собираются, но runtime-поведение
  зависит от stage 45 — там fork/exec/pipe/chdir/shm_open будут закрыты
  синхронно с появлением пакетного менеджера и реальных user-программ).
  Все собраны через `gcc --sysroot=build/sysroot`. Автотест
  `scripts/test_stage40_libc.sh` — 31 проверка (сборка, libc артефакты,
  symbols, initrd, smoke-QEMU-boot).

**Новые syscall-номера (стабильны, нельзя менять без полной пересборки libc):**

| Номер | Имя | Группа |
|-------|-----|--------|
| 0x110 | AOS_SYS_STAT | A |
| 0x111 | AOS_SYS_FSTAT | A |
| 0x112 | AOS_SYS_MKDIR | A |
| 0x113 | AOS_SYS_DUP | A |
| 0x114 | AOS_SYS_DUP2 | A |
| 0x115 | AOS_SYS_FCNTL | A |
| 0x116 | AOS_SYS_SIGACTION | B |
| 0x117 | AOS_SYS_KILL | B |
| 0x118 | AOS_SYS_SIGRETURN | B |
| 0x120 | AOS_SYS_CLONE | D |
| 0x121 | AOS_SYS_FUTEX_WAIT | D |
| 0x122 | AOS_SYS_FUTEX_WAKE | D |
| 0x123 | AOS_SYS_SET_TID | D |
| 0x124 | AOS_SYS_JOIN | D |
| 0x130..0x13B | AOS_SYS_SOCKET...GETSOCKOPT | C |

**Self-hosting note:** Libc собирается WSL2-хостом; self-hosted
перенос компиляции libc внутрь AMPLIFY-OS отложен до stage 57
(`os swap` с полноценным inside-toolchain). См. `libc-x86_64/README.md`.

**Ключевые файлы этапа 40:**
- `.kiro/specs/stage-40-libc-finalize/requirements.md` — 10 EARS-требований
- `libc-x86_64/README.md` — граница running/target + матрица POSIX-покрытия
- `kernel/arch/x86_64/sig.c`, `sock.c`, `futex.c`, `dynlink.c` — новые модули
- `libc-x86_64/src/{pthread,signal,socket,sys_stat,fcntl}/*.c` — реальные обёртки
- `userspace/x86_64/stage40_*.c` — 6 мигрированных программ
- `scripts/test_stage40_libc.sh` — автотест (31 проверка)

---

## 13. Тестирование

- `scripts/test_c5_splash.py` — Python-тест, нужен `python3 + Pillow`.
  Делает 9 PPM-скриншотов (splash → firstboot → policy → create →
  success → login → desktop) и проверяет 6 ключевых строк в serial-логе.
  Использует QEMU unix-socket монитор.
- `scripts/test_c5_splash_bash.sh` — **bash-версия** (если в WSL нет
  python3). Тот же набор скриншотов, но QEMU monitor по TCP-сокету
  127.0.0.1:4567. PPM → PNG конвертит через Windows-python (PIL).
- `scripts/test_c5_apps.py` — открывает каждое из 12 приложений и
  делает скриншот.
- Все вышеперечисленные эталоны успешно прошли на 37.6c-6.

---

## 14. Известные проблемы / подводные камни

1. **Права на `build/`:** старые `.o` владеют `root` после случайного
   `sudo make`. Лечится `sudo rm -rf build/x86_64 build/disk-x86_64.img`.
2. **FAT32 запись:** `fat32_write_file()` работает **только в
   существующие** файлы того же размера кластеров. Для upsert →
   `fat32_save_file()`.
3. **Login c пустыми полями:** при `users_count() == 0` login принимает
   пустые имя/пароль как guest. Логи `[login] user= (guest)` — это норма.
4. **`wm_main_loop()` не возвращается** — всё что после неё в
   `kernel64_main.c` мёртвый код (smoke-тесты IPC).
5. **Charset:** **запрещено** использовать в строковых литералах
   символы вне CP1251 (`→`, `×`, box-drawing) — GCC упадёт с
   `Invalid or incomplete multibyte or wide character`. Замены: `->`,
   `x`, `--`. Сканер:
   ```bash
   python3 -c "
   import re,os; allow={0x2014,0x2013,0x2026,0x201C,0x201D,0x201E,
     0x2018,0x2019,0x201A,0x00A0,0x00A9,0x00AE,0x2022,0x00B0,0x00B7}
   for fn in os.listdir('kernel/arch/x86_64'):
     if not fn.endswith('.c'): continue
     for n,L in enumerate(open(f'kernel/arch/x86_64/{fn}',encoding='utf8'),1):
       for m in re.finditer(r'\"([^\"]*)\"',L):
         for c in m.group(1):
           cp=ord(c)
           if cp<0x80 or 0x400<=cp<=0x45F or cp in allow: continue
           print(f'{fn}:{n}: U+{cp:04X}')"
   ```
6. **Incremental rebuild после смены charset:** `-fexec-charset` —
   не отслеживается mtime'ами. После любой правки `Makefile.x86_64`
   нужно `rm -rf build/x86_64` и собрать заново.
7. **TCP-монитор QEMU** в bash-тесте: иногда первые попытки
   `/dev/tcp/127.0.0.1/4567` падают с `Connection refused` — это норма,
   просто QEMU ещё не успел открыть порт. Цикл retry ловит подключение.
8. **UEFI: НЕ ТРОГАТЬ COM1 до ExitBootServices** (этап 38). OVMF
   TerminalDxe мирорит ConOut в Serial через свой 16550-стейт.
   Перезапись DLAB/divisor → поток U+BFAF на serial. Pre-EBS вывод —
   только через `g_co->OutputString`. После EBS — наш UART, можно
   `com1_init()` (divisor=3 → 38400 baud, как в `kernel/arch/x86_64/
   serial.c`).
9. **UEFI: objcopy `-j .rodata` обязателен** (этап 38). В современных
   gnu-efi `elf_x86_64_efi.lds` НЕ мерджит .rodata в .data — без
   `-j .rodata` в `uefi/Makefile` все `L"..."` CHAR16-строки исчезают
   из BOOTX64.EFI, и OutputString читает несвязанную память.
10. **UEFI: `cld` в `_efi_start`** (этап 38). UEFI не гарантирует
    DF=0 на entry, а `rep stosq` чувствителен к направлению.
11. **UEFI: AllocateAddress p_paddr=1MB** — почти всегда срабатывает
    в OVMF, но если упадёт — линкуем ядро выше (`linker_x86_64.ld:19`
    `. = 1M;` → `. = 16M;`). Не наблюдалось в QEMU+OVMF/edk2.
12. **gnu-efi нет в Debian по умолчанию** (stage 38/40 UEFI-путь).
    `make run-uefi` падает на `fatal error: efi.h: Нет такого файла`.
    Лечится `sudo apt install gnu-efi ovmf`. Проверка:
    `ls /usr/include/efi/efi.h /usr/lib/crt0-efi-x86_64.o`.
13. **Deferred expansion в Makefile** (stage 40 F): `INITRD_FILES := ...$(USER_MIGRATED_ELFS)...`
    будет пустым, если `USER_MIGRATED_ELFS` определён ниже. Используй `=`
    или перенеси определение выше. Уже исправлено в
    `Makefile.x86_64:185-194`.
14. **Pattern-rule vs basename collision** (stage 40 F): нельзя иметь
    одновременно `$(USER_BUILD)/%.elf: $(USER_DIR)/%.c` и
    `$(USER_BUILD)/%.elf: $(USER_DIR)/%.asm` с одинаковым basename (будет
    взят последний). Мы обошли префиксом `stage40_` для мигрированных
    C-программ; legacy `hello.asm`/`counter.asm` остались без префикса.
15. **`-fPIC` + ассемблер** (stage 40 E): любые `lea [rip + symbol]` на
    глобал в `.S`-файле при сборке libc.so падают на линковке (R_X86_64_PC32
    reloc is non-PIC). Использовать `mov rax, [rip + symbol@GOTPCREL]` —
    косвенный доступ через GOT. Пример: `sigreturn.S` вызывает
    `__aos_user_handlers` именно так.
16. **`mkfs.vfat: not found`** на `make run-uefi` (stage 38 ESP-build).
    `Makefile.x86_64` цель `esp` использует `mkfs.vfat` + `mmd` + `mcopy`
    для записи FAT32-образа без root-mount. Нужны пакеты:
    `sudo apt install -y dosfstools mtools`.
17. **`mkfs.vfat` в PATH только у root** (Debian). У обычного пользователя
    `/usr/sbin` не в `PATH`, а `mkfs.vfat` лежит именно там. Makefile
    уже ищет его через `command -v mkfs.vfat || echo /usr/sbin/mkfs.vfat`,
    но если кто-то откатит эту логику — верните её обратно (см.
    `Makefile.x86_64` цель `$(ESP_IMG)`).
18. **OVMF: `BdsDxe: failed to load Boot0001/0002/0003 ... Not Found` →
    PXE-boot loop** (stage 38). Симптом: OVMF не находит загрузочный
    `BOOTX64.EFI` ни на одном диске и уходит в PXE. Причина почти всегда —
    **битый esp.img**: либо `mkfs.vfat` упал (см. пункты 16-17) и остался
    64MB файл с нулями без FAT32 header'а, либо `mcopy` не положил файл по
    пути `/EFI/BOOT/BOOTX64.EFI`. Диагностика:
    ```bash
    file build/esp.img                       # должно быть "DOS/MBR boot sector ... FAT (32 bit)"
    mdir -i build/esp.img ::/EFI/BOOT        # должно показать BOOTX64.EFI
    ```
    Фикс: `rm -f build/esp.img && make ARCH=x86_64 esp`.
19. **stage 42 HOTFIX: тяжёлый CPU-compositor подвешивает PS/2 мышь.**
    Симптом: курсор сдвигается на один кадр и замирает, при этом часы
    в taskbar продолжают идти. Причина — рендер кадра растягивается
    до ~150 мс на QEMU TCG (3 МБ compose→doublebuf memcpy + 6-проходный
    box blur + 4×4 AA per-окно). PS/2 8042 имеет 1-байтовый input buffer:
    за это время мышь шлёт ~10 пакетов, доезжает 1, дальше синхронизация
    `pkt_idx` ломается → курсор замирает. **Урок:** на текущем CPU-
    рендере любой compositor-эффект должен укладываться в ~5 мс/кадр;
    тяжёлые блюры и copy-loops в полный экран — только GPU (этап 44).
20. **stage 42 HOTFIX: НЕ вызывать `compositor_update_animations()`
    больше одного раза за кадр.** Эта функция продвигает progress и
    помечает done. Двойной вызов (из main loop и из render_frame) гонит
    анимации в 2× скорости и сокращает их длительность вдвое. Для
    read-only проверки «есть ли активная анимация» — используйте
    `compositor_peek_animation_active()`.
21. **stage 42 HOTFIX: `mouse_poll()` должен быть в CLI-секции.**
    Иначе IRQ12, прилетевший между `inb(KBD_STATUS)` и `inb(KBD_DATA)`
    внутри polling-пути, запускает `mouse_irq`, который сам читает
    DATA. После возврата основной `drain_8042` читает уже пустой
    регистр или мусор → потеря синхронизации пакета. Сейчас
    `mouse_poll` обёрнут в `cli`/`sti` (см. `mouse.c:123`).
22. **stage 42: compose-буфер выключен НАМЕРЕННО.** Если кто-то решит
    «вернуть тройную буферизацию» — сначала убедись, что копия 3 МБ
    compose→doublebuf не возвращается. Реальная tearing-protection
    должна делаться GPU-side через VirGL compose-resource (этап 44),
    а не CPU memcpy. См. `compositor_init` и HOTFIX-комментарий в
    `compositor.c`. `g_compose.width/height` остались только как
    clip-границы для `mark_dirty()`, `g_compose.pixels = NULL`.

---

## 15. Roadmap фазы 3 (этапы 37–58)

Этапы **37, 38, 40 завершены**. Дальше — по плану ниже, **строго
поэтапно с подтверждением пользователя между этапами**. Номер 39
зарезервирован и слит с 40 (см. «История этапов — stage 40» в §12).

| Этап | Название | Статус | Краткая суть |
|------|----------|--------|--------------|
| **37** ✅ | Long Mode | DONE | Всё функциональное ядро перенесено в x86_64. |
| **38** ✅ | UEFI handoff | DONE | `uefi/main.c` грузит ELF, ExitBootServices, GOP+RSDP. |
| **39** | (зарезервирован, слит с 40) | DONE | Первая итерация libc была зарегистрирована здесь; финализирована в 40. |
| **40** ✅ | libc finalize | DONE | Полный POSIX-слой: stat/mkdir/dup/fcntl, sigaction, POSIX sockets, pthread+futex+TLS, libc.so + dynlink x86_64, миграция 6 userspace. 31/31 тестов. |
| **41** ✅ | GPU virtio-gpu | DONE | Полный virtio 1.0 modern transport + DRM-обёртка. doublebuf_swap идёт через TRANSFER_TO_HOST_2D + RESOURCE_FLUSH. Fallback на VESA если нет `-device virtio-gpu-pci`. Syscalls 0x140..0x148. Property-test (drm_self_test) + benchmark (drm_self_benchmark, 4.0× на QEMU TCG). Shell `gpustat`/`gpubench`. 1920×1080 supported. 46/46 тестов. |
| **42** ✅ | Compositing | DONE | Direct-to-doublebuf рендер (compose-буфер выпилен HOTFIX'ом 15.05.2026), per-pixel alpha, быстрые тени `gfx_drop_shadow` (box-blur под `COMPOSITOR_HEAVY_SHADOW`), скруглённые углы с 4×4 supersampling AA, анимации open/close (ease-out cubic, 180/120 мс), `compstat`. 50 FPS @ 1024×768 на QEMU TCG. Graceful fallback на doublebuf. 16/16 тестов. |
| **43** | Neural Icon Forge | skipped (deferred) | Пропущен по запросу пользователя — иконки ещё не готовы. Вернёмся позже. |
| **44** ✅ | OpenGL ES 2.0 (программный) | DONE | Активирован FPU+SSE/SSE2 в ядре (`boot.asm` CR0+CR4 init, FXSAVE/FXRSTOR в `task_yield`/`isr_common`/`syscall_entry`, `task_t.fpu_state` через `kmalloc_aligned`, `-mno-sse` снят из CFLAGS). Программный 3D-стек в `gles.{c,h}` + `gles_math.c`: float-математика через SSE, EGL-подобный API (`aos_egl_*`), state machine (`gl_viewport/clear/enable/.../draw_arrays/elements`), растеризатор Pineda с perspective-correct interp varying'ов через 1/w и Z-buffer'ом, текстуры nearest/linear + repeat/clamp, шейдеры как C-функции, α-blend. Демо `app_glcube` — текстурированный куб с Lambert-освещением, иконка "3D куб" на десктопе + пункт меню. Долги из 41/42 (VirGL/GPU compose-resource, real triple-buffer, glassmorphism, partial-flush) перенесены на этап 50. |
| **45** ✅ | `Amply` пакетный менеджер | DONE | Полностью переписан FAT32 (subdirs + LFN + auto-extend chains + mkdir/iter). Собственный формат `.amf` с двойной FNV-1a подписью (запас под Ed25519). `tools/mkamf` собирает пакеты host-утилитой; 3 встроенных в initrd: welcome, cyber-theme, notes-pack. Kernel `amply.{c,h}` парсер + DFS-граф зависимостей с обнаружением циклов + транзакционный install с откатом + БД в `/disk/Sanctum/Amply/db.txt`. Shell-команда `Amply install/remove/upgrade/search/info/list/verify/repo`. GUI-приложение Amply Center (иконка 'A' на десктопе + меню Пуск). 25/25 тестов. Долги: сжатие zstd → этап 51, HTTPS-репо → этап 47, Ed25519 → этап 47, post-install scripts → этап 46. **fork/exec/pipe/chdir/shm_open перевешены на этап 46.** |
| **46** ✅ | Sandboxes | DONE | namespaces (PID/mount/net/UTS/user) с CoW через `ns_unshare`, cgroups v2 (CPU-ns/RAM/IO/PIDs), seccomp whitelist + 4 профиля, box_run, 10 новых syscalls 0x150..0x15A, GUI Sandbox Manager, shell `box run/list/ns/cg/test`. 41/41 тестов. |
| **47** ✅ | Crypto + TLS | DONE | Самописный crypto stack (~3500 строк): SHA-2 семейство, HMAC/HKDF/PRF, AES (software + AES-NI), AES-CBC/GCM, ChaCha20-Poly1305, PRNG (RDRAND/RDSEED), 4096-bit bignum + RSA-PKCS#1 verify, X25519 + Ed25519, ASN.1 + X.509 v3 parser, trust store (ISRG Root X1 + DigiCert Global Root G2 — генерируется из `/etc/ssl/certs/`). TLS 1.2 client (`TLS_RSA_WITH_AES_128_GCM_SHA256`): полный handshake state machine, record layer, SNI, Finished. `https_get`, browser обновлён, TCP listen/accept в `tcp.c`/`sock.c`. Shell `https <url>`. **Live end-to-end test: реальный handshake против `openssl s_server` (3/3 PASS)** + 58/58 static тестов. Долги (ECDHE-P256, TLS 1.3, SHA-1, strict chain, OCSP/CRL, PCLMULQDQ GHASH) перевешены на будущее. |
| **48** ✅ | Шифрование диска (FDE) | DONE | LUKS1 (байт-в-байт как cryptsetup: aes-xts-plain64/sha256, 8 слотов, AF-splitter 4000 полос), `blkcrypt` — dm-crypt-аналог (виртуальные устройства в `disk_io`), экран разблокировки до монтирования `/disk`, единый вход, шифрование в установщике, shell `seal`, smoke 48 на RAM-диске, e2e + interop через libcryptsetup. 25/25. См. §26. |
| **49** | AMPLIFY MIND | planned | Локальный inference (llama.cpp/ggml), GGUF, виджет Ctrl+Space, доступ к API ОС (процессы, VFS, логи, exec). |
| **50** | Hypervisor VT-x | planned | VMX-инструкции, EPT, `vm run <iso>` — гость Alpine/TinyCore в окне. |
| **51** | WebAssembly | planned | Портирование wasm3 (или свой интерпретатор), WASI поверх aos_*, песочница. |
| **52** | embersd журнал | planned | Бинарный лог `/disk/Sanctum/embers/`, crash-dump + анализ через MIND, `perf` через RDTSC+LAPIC. |
| **53** | Полный Unicode | planned | UTF-8 везде, расширение `ttf.c`, локали `.po/.mo`, переключение раскладок RU/EN/JP/AR, IME. |
| **54** | Bluetooth + WiFi | planned | HCI поверх UHCI, L2CAP/RFCOMM/HID/A2DP, драйвер для RTL8188 или iwlwifi, `bt scan/connect`, `wifi connect`. |
| **55** | Doom + Quake | planned | Порт chocolate-doom, опц. Quake 3, видеоплеер на ffmpeg. |
| **56** | OTA-обновления + A/B партиции | planned | Формализация будущего `os swap`, дельта-обновления, подпись через stage 47. |
| **57** ✅ | GUI installer + Live ISO | DONE (частично) | Гибридный ISO (BIOS+UEFI+USB), live-режим по cmdline/`live.flag`, 6-экранный GUI-установщик по макету: выбор диска, hostname/админ/пароль, vault-раздел, тема. GPT + mkfs.fat32 в ядре, многодисковый ATA LBA48, верификация, e2e-тест. **Не сделано:** язык/раскладка, шифрование (этап 48), forge, self-hosted libc build. См. §25. |
| **58** | RELEASE 1.0 | planned | Бенчмарки, man-страницы, Doxygen developer guide, сайт-визитка через MIND, финальный reproducibility-тест, финальный релиз. |

### Стратегия работы над следующими этапами

Когда пользователь даёт команду «приступай к этапу N» — ИИ-агент
должен следовать формату из изначального промпта:

```
--- ЭТАП N: [НАЗВАНИЕ] ---
📋 **Описание**: что делаем и зачем
📂 **Аудит существующего**: что уже есть (пути + строки)
📁 **Файлы**: список новых/изменяемых
💻 **Код**: полный исходный код (без многоточий)
🔨 **Сборка**: команды Makefile в WSL
🚀 **Запуск**: команда QEMU из WSL
✅ **Ожидаемый результат**: что должно произойти
⚠️ **Возможные проблемы и решения** (WSL-специфика)
🔗 **Связь с предыдущими этапами**: что переиспользуется
❓ "Этап N завершён. Всё работает? Переходим к N+1?"
```

И **НЕ ПЕРЕХОДИТЬ к следующему этапу** до подтверждения.

### Конкретика для следующего этапа (41 — virtio-gpu)

Когда пользователь скажет «приступай к этапу 41», ИИ-агент должен:

1. **Аудит:**
   - `kernel/arch/x86_64/pci.c` — функция сканирования уже есть; добавить
     поиск устройства с vendor=0x1AF4 (Red Hat virtio) device=0x1050 /
     0x1051 (virtio-gpu-pci). Legacy virtio устройства имели device=0x1040.
   - `kernel/arch/x86_64/fb.c` / `doublebuf.c` — точка, через которую
     compositor должен будет выходить на GPU (пока пишет в VESA-FB).
     API `gfx_screen()` нужно сохранить.
   - QEMU cli в `Makefile.x86_64` — добавить `-device virtio-gpu-pci`.
2. **Новые файлы:**
   - `kernel/drivers/virtio_gpu.h` / `kernel/drivers/virtio_gpu.c` (или
     `kernel/arch/x86_64/virtio_gpu.c` по существующей arch-модели).
   - `kernel/gui/drm.h` / `kernel/gui/drm.c` — DRM-подобный API для user.
3. **Базовые возможности:**
   - Queue init (controlq + cursorq), descriptors, avail/used rings.
   - Команды: `VIRTIO_GPU_CMD_GET_DISPLAY_INFO`,
     `VIRTIO_GPU_CMD_RESOURCE_CREATE_2D`,
     `VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING`,
     `VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D`,
     `VIRTIO_GPU_CMD_SET_SCANOUT`,
     `VIRTIO_GPU_CMD_RESOURCE_FLUSH`.
   - Fallback на VESA если virtio-gpu не найден (чтобы `make ARCH=x86_64 run`
     продолжал работать без `-device virtio-gpu-pci`).
4. **Интеграция:**
   - `doublebuf_swap()` — если virtio-gpu активен, выполнять
     `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` вместо CPU memcpy в
     VESA FB.
5. **Тест:**
   - `scripts/test_stage41_virtio_gpu.sh` — запуск с `-device virtio-gpu-pci`,
     проверка serial-лога на `[virtio_gpu] display 1024x768` и прохождение
     firstboot/login.
6. **Self-hosting note:** virtio-gpu работает **только внутри QEMU или KVM**.
   На реальном железе нужен stage 50 (Hypervisor VT-x) либо отдельный
   драйвер physical GPU (вне scope фазы 3).

---

## 16. Чек-лист «нашёл баг — что делать»

1. **Что-то не отрисовывается на UI?** — проверь, в CP1251 ли строка
   (нет ли `→`, `×`, U+25xx). Запусти сканер из п. 14.5.
2. **Кириллица превратилась в пустоту?** — `-fexec-charset=CP1251`
   потерян. Проверь `Makefile.x86_64:36`.
3. **Билд падает с `Invalid multibyte`** — в каком-то `.c` остался
   запрещённый символ.
4. **Изменил Makefile, но `.o` не пересобрались** — `rm -rf build/x86_64`.
5. **Тест проходит, но скриншоты пустые** — старый PPM в `build/`; перед
   тестом скрипт удаляет `c5-*.ppm` и `c5-*.png` сам.
6. **`make` падает с permission denied на `build/`** — `sudo rm -rf
   build/x86_64` (старые `.o` от sudo).
7. **QEMU не подключается к монитору в bash-тесте** — порт 4567 уже
   занят другим QEMU. `pkill -9 qemu-system`.
8. **UEFI: `make run-uefi` — чёрный экран и поток U+BFAF / `꾯` на serial.**
   Это значит .rodata пропала из BOOTX64.EFI. Проверь `uefi/Makefile`,
   что в objcopy есть `-j .rodata`. Полная пересборка:
   ```
   make -C uefi clean && rm -f build/esp.img && make ARCH=x86_64 esp
   ```
9. **UEFI: загрузка молча зависает или garbage на serial (но не U+BFAF).**
   Возможно `com1_init()` стоит ДО `ExitBootServices()` — это ломает
   OVMF TerminalDxe. Pre-EBS — только `g_co->OutputString`.
10. **UEFI: `[uefi]   FATAL: ELF load failed`** — `AllocateAddress` не
    смог выделить регион по `p_paddr`. Решение: либо проверить EFI memmap
    на занятость 1 MB, либо линковать ядро выше (`linker_x86_64.ld:19`
    `. = 1M;` → `. = 16M;`).
11. **UEFI: триплфолт сразу после `[uefi] EBS done, jumping...`** — баг
    в `_efi_start`. Особенно: проверь `cld` в начале (DF safety),
    RIP-relative addressing для boot_pml4/pdpt/pd, что lgdt читает
    структуру в .rodata.

---

## 17. Файлы, к которым стоит обращаться в первую очередь

| Что надо | Где |
|----------|-----|
| Точка входа ядра | `kernel/kernel64_main.c` |
| Список ассемблерных файлов | `Makefile.x86_64:45-54` |
| Список .c файлов | `Makefile.x86_64:56-123` |
| **Boot.asm — 32-bit GRUB вход `_start` + 64-bit UEFI вход `_efi_start`** | `kernel/arch/x86_64/boot.asm` |
| **Multiboot2 header (info req + framebuffer + entry-tag + end)** | `kernel/arch/x86_64/boot.asm:25-78` |
| **UEFI loader (ELF reader + mb2_info synth + EBS + jump)** | `uefi/main.c` |
| **UEFI build (BOOTX64.EFI с `-j .rodata`)** | `uefi/Makefile` |
| **ESP-образ (FAT32: BOOTX64.EFI + kernel.bin + initrd.img)** | `Makefile.x86_64` цель `esp` |
| **run-uefi (OVMF + ESP IDE slave + disk IDE master)** | `Makefile.x86_64` цель `run-uefi` |
| linker script (ENTRY=_efi_start, ELF e_entry) | `linker_x86_64.ld` |
| Тема UI (цвета, радиусы) | `kernel/arch/x86_64/ui_theme.h` |
| Шрифт (256 глифов CP1251) | `kernel/arch/x86_64/font8x16.c` |
| Первый запуск (мастер) | `kernel/arch/x86_64/firstboot.c` |
| Экран входа | `kernel/arch/x86_64/login.c` |
| Splash | `kernel/arch/x86_64/splash.c` |
| Рабочий стол | `kernel/arch/x86_64/desktop.c` |
| Меню «Пуск» | `kernel/arch/x86_64/startmenu.c` |
| Все 12 приложений | `kernel/arch/x86_64/apps.c` (~2000 строк) |
| Оконный менеджер (HOTFIX 42: peek-вызов анимаций, 50 FPS) | `kernel/arch/x86_64/wm.c` |
| **Compositor (stage 42 + HOTFIX 15.05.2026)** | `kernel/arch/x86_64/compositor.{c,h}` |
| **PS/2 mouse (HOTFIX 42: CLI/STI вокруг `drain_8042`)** | `kernel/arch/x86_64/mouse.c` |
| FAT32 | `kernel/arch/x86_64/fat32.c` |
| Пользователи | `kernel/arch/x86_64/users.c` |
| **Shell + cmd_launch + cmd_elfinfo (stage 40)** | `kernel/arch/x86_64/shell.c` |
| **Номера syscalls (0x100..0x13B)** | `kernel/arch/x86_64/syscall.h` |
| **Диспатчер syscalls (все группы stage 40)** | `kernel/arch/x86_64/syscall.c` |
| **Signal-frame + sigreturn (stage 40 B)** | `kernel/arch/x86_64/sig.c` |
| **POSIX sockets поверх tcp/udp (stage 40 C)** | `kernel/arch/x86_64/sock.c` |
| **clone/futex/join — pthread kernel side (stage 40 D)** | `kernel/arch/x86_64/futex.c` |
| **Dynamic linker x86_64 (stage 40 E)** | `kernel/arch/x86_64/dynlink.c` |
| **task_t с sig_state + fs_base (stage 40 B,D)** | `kernel/arch/x86_64/task.h`, `task.c` |
| **VFS с fd_kind + stat/dup/fcntl (stage 40 A)** | `kernel/arch/x86_64/vfs.h`, `vfs.c` |
| **Libc (ядро реализации)** | `libc-x86_64/src/{unistd,stdio,stdlib,stdio,string,fcntl,sys_stat,sys_mman,pthread,signal,socket,time,math,setjmp,crt,syscall,errno,locale,ctype}/*` |
| **Libc README + POSIX-матрица + Running vs Target** | `libc-x86_64/README.md` |
| **Sysroot (собирается в build)** | `build/sysroot/usr/{include,lib}/{libc.a,libc.so,crt0.o,user.ld}` |
| **Мигрированные userspace (6 программ)** | `userspace/x86_64/stage40_*.c` |
| **Первая функциональная C-программа через sysroot** | `userspace/x86_64/hello_libc.c` |
| **Автотест stage 40** | `scripts/test_stage40_libc.sh` |
| **Spec stage 40 (EARS-требования)** | `.kiro/specs/stage-40-libc-finalize/requirements.md` |
| **[этап 44] FPU + SSE инициализация в boot** | `kernel/arch/x86_64/boot.asm` (CR0+CR4 в обоих entry-точках) |
| **[этап 44] FPU save/restore в context-switch** | `kernel/arch/x86_64/task.c` (`fpu_save`/`fpu_restore` + `task_yield`) |
| **[этап 44] FPU save/restore в IRQ entry** | `kernel/arch/x86_64/isr.asm` (FXSAVE/FXRSTOR вокруг `isr_dispatch`) |
| **[этап 44] FPU save/restore в SYSCALL entry** | `kernel/arch/x86_64/syscall_entry.asm` (плюс alignment-fix) |
| **[этап 44] GLES2-like API (header)** | `kernel/arch/x86_64/gles.h` |
| **[этап 44] Программный растеризатор + state-machine + EGL** | `kernel/arch/x86_64/gles.c` |
| **[этап 44] Математика (sinf/cosf/sqrtf, mat4, vec3)** | `kernel/arch/x86_64/gles_math.c` |
| **[этап 44] Демо: текстурированный 3D-куб** | `kernel/arch/x86_64/app_glcube.c` |
| **[этап 47.1] Агрегирующий header криптостека** | `kernel/arch/x86_64/crypto.h` |
| **[этап 47.1] CPUID detect AES-NI/RDRAND/RDSEED + utils** | `kernel/arch/x86_64/crypto_cpu.c` |
| **[этап 47.1] SHA-256/384/512 (FIPS 180-4)** | `kernel/arch/x86_64/crypto_sha.c` |
| **[этап 47.1] HMAC + HKDF + TLS 1.2 PRF** | `kernel/arch/x86_64/crypto_hmac.c` |
| **[этап 47.1] AES-128/256 software + AES-NI + CBC + GCM** | `kernel/arch/x86_64/crypto_aes.c` |
| **[этап 47.1] ChaCha20 + Poly1305 + AEAD** | `kernel/arch/x86_64/crypto_chacha.c` |
| **[этап 47.1] PRNG (Fortuna-style + RDRAND/RDSEED/RDTSC)** | `kernel/arch/x86_64/crypto_rand.c` |
| **[этап 47.1] KAT-векторы (NIST/RFC 4231/RFC 8439)** | `kernel/arch/x86_64/crypto_self_test.c` |
| **[этап 47.2] 4096-bit bignum + modexp** | `kernel/arch/x86_64/crypto_bn.c` |
| **[этап 47.2] RSA-PKCS#1 v1.5 verify** | `kernel/arch/x86_64/crypto_rsa.c` |
| **[этап 47.2] X25519 ECDH + Ed25519 verify (ref10-style)** | `kernel/arch/x86_64/crypto_curve.c` |
| **[этап 47.3] ASN.1 DER parser** | `kernel/arch/x86_64/crypto_asn1.{c,h}` |
| **[этап 47.3] X.509 v3 parser + chain verify** | `kernel/arch/x86_64/crypto_x509.{c,h}` |
| **[этап 47.3] Trust store lookup** | `kernel/arch/x86_64/crypto_ca.{c,h}` |
| **[этап 47.3] DER blobs root CA (автоген)** | `kernel/arch/x86_64/crypto_ca_blob.c` |
| **[этап 47.3] Генератор trust store из /etc/ssl/certs/** | `scripts/gen_ca_blob.sh` |
| **[этап 47.4] TLS 1.2 client (handshake + record + KDF)** | `kernel/arch/x86_64/tls.{c,h}` |
| **[этап 47.5] HTTPS GET через TLS** | `kernel/arch/x86_64/https.{c,h}` |
| **[этап 47.5] TCP listen/accept (server-side single-conn)** | `kernel/arch/x86_64/tcp.c` (LISTEN/SYN_RCVD states) |
| **[этап 47.5] shell `https <url>` команда** | `kernel/arch/x86_64/shell.c:cmd_https` |
| **[этап 47] Static autotest 58/58 PASS** | `scripts/test_stage47_crypto.sh` |
| **[этап 47] Live e2e test (openssl s_server + handshake)** | `scripts/test_stage47_live.sh` |
| Сборка диска | `scripts/mkdisk-x86_64.sh` |
| Автотест firstboot | `scripts/test_c5_splash_bash.sh` |

---

## 18. История чата (для следующего ИИ)

**Этап 37.6c-6 (12.05.2026):** локализация UI на русский (CP1251 шрифт + charset-флаги).
Подробности в git-логе и в STAGE-описаниях кода. Эта сессия завершилась успешно,
все скриншоты firstboot/login на русском.

**Этап 38 (13.05.2026) — UEFI handoff, ✅ ЗАВЕРШЁН:**

Пользователь дал команду «приступай к этапу 38». ИИ-агент:

1. **Аудит:** прочитал `uefi/main.c` (33B PoC), `uefi/Makefile`,
   `kernel/arch/x86_64/boot.asm`, `kernel/arch/x86_64/multiboot2.h`,
   `kernel/kernel64_main.c`, `kernel/arch/x86_64/acpi.c`, `pmm.c`, `fb.c`,
   `initrd.c`, `linker_x86_64.ld`, `Makefile.x86_64`, `Makefile` —
   построил карту boot-пути GRUB и понял что для UEFI нужен второй вход.

2. **Архитектурное решение:** **эмулировать Multiboot2 из UEFI loader'а**,
   чтобы `kernel_main64` не знал какой путь. Альтернатива (свой
   `aos_boot_info_t`) была отвергнута — пришлось бы переписывать pmm/acpi/
   fb/initrd, ради удобства одной демки.

3. **Реализация:**
   - **Новый `uefi/main.c`** (~700 строк): ELF64-parser, ESP file reader,
     GOP setter, RSDP locator, MB2-info builder, EBS loop.
   - **`_efi_start` в `boot.asm`**: 64-bit вход с теми же действиями, что
     32-bit `_start`, но RIP-relative lea на boot_pml4/pdpt/pd.
   - **MB2 entry-address тэг (тип 3)** в `.multiboot` секции — переопределяет
     ELF e_entry именно для GRUB.
   - **`ENTRY(_efi_start)` в `linker_x86_64.ld`** — UEFI loader использует
     ELF e_entry.
   - **`Makefile.x86_64`: цели `bootefi`, `esp`, `run-uefi`.** ESP-образ:
     /EFI/BOOT/BOOTX64.EFI + /kernel.bin + /initrd.img.

4. **Тестирование** в WSL (`make ARCH=x86_64 run-uefi`) — первый прогон
   показал чёрный экран и поток `꾯꾯꾯` (U+BFAF) на serial. ИИ-агент:
   - **Гипотеза 1 (отвергнута):** `com1_init()` ДО ExitBootServices ломал
     OVMF UART-state. Перенёс init на ПОСЛЕ EBS. Результат — без изменений
     (т.е. это была одна из проблем, но не главная).
   - **Гипотеза 2 (подтвердилась):** `objcopy` не включал `.rodata` в
     `BOOTX64.EFI`. Проверил `readelf -S` на `main.so`: .rodata = отдельная
     секция в 6.5 KB, не смерджена в .data. Поэтому L"..." CHAR16-строки
     отсутствуют в .efi, и OutputString читал случайную память (как раз
     заполненную 0xBFAF в OVMF-памяти). Добавил `-j .rodata` в objcopy.
   - **Прочее:** добавил `cld` в `_efi_start` (DF safety перед REP STOSQ).

5. **Финальная проверка:** `qemu-system-x86_64 -bios OVMF.fd -nographic ...`
   показал полный успешный boot до `[doublebuf] initialized: 1024x768`
   и RTC `2026-5-13 6:41:15`. Сравнение с GRUB-путём — оба идентично
   проходят все подсистемы. UEFI-путь даёт 249 MB usable RAM, GRUB —
   255 MB (разница = ESP в памяти).

**Что готово после 38:**
- `make ARCH=x86_64 run` → GRUB → `_start` → 32→64 → `kernel_main64`
- `make ARCH=x86_64 run-uefi` → OVMF → `BOOTX64.EFI` → `_efi_start` →
  `kernel_main64` (тот же kernel.bin)

**Этап 40 (13.05.2026) — libc finalize, ✅ ЗАВЕРШЁН:**

Сессия стартовала с неоднозначности: CLAUDE.md в прошлой редакции
нумеровал stage 39 как «musl libc», а ТЗ пользователя говорит о
stage 40 как «libc». Работа стартовала с аудита: в `libc-x86_64/`
уже ~65% POSIX-покрытия, собраны `libc.a`+`crt0.o`, есть
`hello_libc.elf` через sysroot. Пользователь выбрал **полный scope**:
закрыть все оставшиеся stub'ы + libc.so + pthread + миграция 6
userspace-программ. Reservированный номер 39 слит в 40.

Работа велась порционно, каждая группа — билд + smoke-тест:

- **Группа A** (syscalls 0x110..0x115): stat/fstat/mkdir, dup/dup2, fcntl;
  расширение `AOS_SYS_OPEN` поддержкой флагов. Требовало перепроектировки
  `vfs.c` с fd_kind enum + refcount + кольцевая fd-таблица.
- **Группа B** (0x116..0x118): signal-delivery. Ключевое решение —
  hook в `syscall_entry.asm` после C-диспатчера, чтобы модифицировать
  сохранённые RIP/RFLAGS перед `sysretq`. Trampoline в libc
  (`sigreturn.S`) читает userspace-таблицу handler'ов через
  `__aos_user_handlers@GOTPCREL` (иначе PIC-линковка libc.so падала).
- **Группа C** (0x130..0x13B): TCP/UDP sockets поверх существующих
  tcp.c/udp.c. Ограничение: tcp.c поддерживает только один активный
  сокет, поэтому listen/accept возвращают ENOSYS (P2, deferred).
- **Группа D** (0x120..0x124): pthread. Новая функция
  `task_create_thread()` разделяет vm_space через `vm_retain`. TLS
  через FS.base MSR (0xC0000100), сохранение/восстановление в
  `task_yield`. Futex — spin-yield (не настоящий block, т.к. шедулер
  кооперативный). `enter_user_mode_thread(rip, rsp, rdi)` передаёт
  arg в RDI.
- **Группа E**: libc.so + dynlink x86_64. Параллельная PIC-сборка libc,
  `-fPIC -mcmodel=large`, 75 KB ELF64 shared с полным PT_DYNAMIC (47
  RELATIVE, GNU_HASH, SONAME). `kernel/arch/x86_64/dynlink.c` (~270
  строк) парсит PT_DYNAMIC, применяет RELA+JMPREL. Runtime-hookup
  PT_INTERP в elf_load_user отложен (риск сломать существующий path).
- **Группа F**: миграция 6 программ (stage40_hello, elftest, forktest,
  exectest, cwdtest, shmtest). Честная классификация:
  functional (hello, elftest, частично cwdtest, shmtest через mmap-anon)
  vs build-clean-runtime-ENOSYS (forktest, exectest — отложено до 45).

Баги, найденные и исправленные по ходу:
1. `INITRD_FILES := $(USER_MIGRATED_ELFS)` с `:=`: раскрытие shell-пусто
   потому что `USER_MIGRATED_ELFS` объявлен ниже в файле. Фикс — `=`
   deferred + перенос декларации выше.
2. `lea [rip + symbol]` в `sigreturn.S` ломал `ld -shared`. Фикс —
   `mov rax, [rip + symbol@GOTPCREL]`.
3. root-owned `build/iso-x86_64/` блокировал `grub-mkrescue` после
   прошлых `sudo make`. Фикс одноразовый: `sudo rm -rf ...`.

Автотест `scripts/test_stage40_libc.sh` — 31 проверка, все pass.

**Дополнительный фикс после завершения stage 40:** shell не имел
реализации `launch` (была только в комментарии header'а). Добавлены
`cmd_launch` и `cmd_elfinfo` в `kernel/arch/x86_64/shell.c`. Теперь в
терминале AMPLIFY-OS доступны 20 команд (было 18), включая
`launch /hello_libc.elf` — первая запускаемая C-программа через
sysroot.

**Что готово после 40:**
- `make ARCH=x86_64 sysroot` → `build/sysroot/usr/lib/{libc.a,libc.so,crt0.o}`.
- `build/initrd-x86_64.img` (≈230 KB) содержит все stage40_*.elf + hello_libc.elf + libc.so.
- Shell-команды `launch` / `elfinfo` работают, из терминала запускаются
  userspace-программы.
- QEMU-smoke через `bash scripts/test_stage40_libc.sh` проходит
  31/31 проверок.

**Этап 41 (virtio-gpu) и Этап 42 (compositing) — ✅ ЗАВЕРШЕНЫ.**
Подробное описание реализации этих этапов — в шапке документа и в §15.
Детали разработческих сессий не сохранились в этом журнале (велись в
отдельных чатах). Текущее состояние кода — авторитативно.

**HOTFIX 15.05.2026 — регрессия PS/2 мыши после этапа 42, ✅ ИСПРАВЛЕНО:**

Пользователь сообщил: «после реализации этапа 42 мышь отказывается
двигаться (сдвигается на один кадр и всё), при этом время в правом
нижнем углу системы идёт спокойно». То есть main loop крутился (часы
рисовались из RTC каждый кадр), но `mouse_state.x/y` либо не
обновлялся, либо обновлялся редко.

**Аудит.** ИИ-агент прочитал `compositor.{c,h}`, `wm.c`, `mouse.{c,h}`,
`cursor.c`, `doublebuf.c`, `desktop.c`, `heap.{c,h}`, `memory.h`,
`keyboard.c`. Нашёл три накопленных проблемы:

1. **Тяжёлый рендер**: `compositor_end_frame` копировал compose-буфер
   (3 МБ) в doublebuf полностью каждый кадр, плюс
   `compositor_draw_shadow` делал 6 проходов box blur (~1M ops/окно) +
   `compositor_fill_round_rect_aa` с 4×4 supersampling. На QEMU TCG
   получалось ~150 мс/кадр. Главный цикл WM рендерил каждые 10 мс
   (PIT @ 100 Hz) и физически не успевал. PS/2 8042 имеет 1-байтовый
   input buffer — за 150 мс мышь шлёт ~10 пакетов, доезжал ~1, потом
   `pkt_idx` уходил из синхрона и пакеты обрабатывались как мусор.

2. **Двойной вызов `compositor_update_animations()`**: и в main loop, и
   в `render_frame`. Анимации шли в 2× скорости (хотя ломало это не
   мышь, а только их длительность).

3. **Гонка `mouse_poll` ↔ `mouse_irq`**: оба читают 8042 без CLI-секции.
   IRQ12 между `inb(STATUS)` и `inb(DATA)` в polling-пути запускал
   `mouse_irq` → второй `drain_8042` → первый прочитал уже пустой
   DATA → потеря байта пакета.

**Решение** — четыре правки:

1. **`kernel/arch/x86_64/compositor.c`** —
   * `g_compose.pixels` больше не аллоцируется; `compositor_target()`
     возвращает `doublebuf_target()`. Compose-буфер выпилен полностью.
     `g_compose.width/height` оставлены только как clip-границы для
     `mark_dirty()`.
   * `compositor_end_frame()` теперь — только `doublebuf_swap()`.
     Минус 3 МБ memcpy/кадр.
   * `compositor_draw_shadow()` по умолчанию идёт в `gfx_drop_shadow`
     (~200 µs). 3-pass box blur (включая `box_blur_h/v`) скрыты под
     `#ifdef COMPOSITOR_HEAVY_SHADOW` — отложены в этап 44 как
     GPU-шейдер.
   * Добавлена `compositor_peek_animation_active()` — read-only обход
     слотов анимации без сдвига progress.

2. **`kernel/arch/x86_64/compositor.h`** — объявлен
   `compositor_peek_animation_active()`, комментарий к
   `compositor_update_animations()` про «строго один раз за кадр».

3. **`kernel/arch/x86_64/wm.c`** —
   * `wm_main_loop` использует `compositor_peek_animation_active()`
     вместо `compositor_update_animations()`. Состояние анимаций
     теперь продвигает только `render_frame`.
   * Частота рендера снижена со 100 FPS до 50 FPS
     (`pit_ticks() - last >= 2` вместо `>= 1`). Это даёт mouse IRQ
     ~10 мс окно дыхания между кадрами. События мыши/клавиатуры
     по-прежнему триггерят кадр немедленно.

4. **`kernel/arch/x86_64/mouse.c`** — `mouse_poll()` обёрнут в
   `__asm__("cli")` / `__asm__("sti")` вокруг `drain_8042()`. Гонка
   с `mouse_irq` устранена.

**Результат:** per-frame cost снизился со ~150 мс до ~10–15 мс на
QEMU TCG. Мышь двигается плавно. Анимации идут в правильной скорости.
Часы продолжают тикать. CLAUDE.md обновлён: шапка с описанием этапа
42 переписана под актуальный код, в §14 добавлены пункты 19–22 про
HOTFIX, в §15 строки 42 и 44 уточнены (отложенные эффекты
перевешены на этап 44), в §17 добавлены строки про `compositor.c`
и `mouse.c`.

**Уроки на будущее** (для следующего ИИ, который соберётся добавлять
тяжёлый эффект в compositor):

* На CPU-рендере любой compositor-эффект должен укладываться в ~5
  мс/кадр. Box blur, alpha-blend больших регионов, full-screen
  memcpy — это GPU-territory.
* `doublebuf_swap()` сам по себе делает 3 МБ копию (RAM → VRAM или
  через virtio-gpu). Любые дополнительные полные копии перед swap'ом —
  плохая идея до тех пор, пока не появится GPU compose-resource.
* PS/2 контроллер 8042 имеет 1-байтовый input buffer. Если main loop
  занят дольше, чем mouse отправляет пакеты (примерно >50 мс на
  active движение), пакеты теряются. Это аппаратное ограничение.
* Любая функция, читающая 8042 (`drain_8042`), должна вызываться
  либо из IRQ-обработчика, либо в CLI-секции. Никогда — параллельно.
* read-only проверка состояния (peek) и его модификация (update) —
  разные функции. Тред будущих регрессий начинается ровно там, где
  одна функция делает обе вещи и вызывается «безопасно» из двух мест.

**Этап 47 (21.05.2026) — Crypto + TLS, ✅ ЗАВЕРШЁН:**

Самая крупная сессия фазы 3 (~5500 строк нового кода).  ИИ-агент работал
автономно с self-testing: каждый из 5 подэтапов (47.1..47.5) собирался,
тестировался через `bash scripts/test_stage47_crypto.sh` в headless QEMU,
и при failures диагностировался через debug-print в `serial`.

Ключевые моменты сессии:

* Подэтап 47.1 (примитивы) первый раз провалил KAT для TLS-PRF и HKDF.
  Корневая причина — критический bug в SHA update accumulator (`buf_len` обнулялся
  даже когда данные уже в буфере).  Single-update KAT'ы (HMAC-buf на "Hi There")
  не ловили — поймали только HKDF и TLS PRF, где multi-update chain.  Fix
  одной строки.
* Подэтап 47.2 (RSA) — реальный test vector сгенерирован через `openssl
  genrsa + openssl dgst -sha256 -sign` в WSL.  Конвертирован в C-array через
  `od -tx1 -v | sed 's/../0x&,/g'`.  Self-test PRG: `pow(65537, 65537,
  1000000007) = 933755776` (получено через host-utility на uint128).
* Подэтап 47.3 (X.509) — выявил, что DigiCert Global Root CA — SHA-1
  (deprecated).  Заменили на DigiCert Global Root G2.  Также **stale .o
  bug**: после изменения CRYPTO_BN_LIMBS 64→128 в header'е, make пересобрал
  не все .o.  Симптом: `out.v[124]` non-zero в `3^7 mod 11`.  Lesson: после
  изменения #define с struct-size — `rm crypto*.o`.
* Подэтап 47.4 (TLS) — самая большая часть в один шаг (740 строк).  Собралась
  с одним trivial fix (off-by-one в smoke index hostname[6]→[7]).
* Подэтап 47.5 (HTTPS + listen + live test) — кульминация: запущен
  `openssl s_server -tls1_2 -cipher AES128-GCM-SHA256` на host'е WSL,
  QEMU connect к 10.0.2.2:4443, kernel выполнил handshake, server cert
  (self-signed) корректно отклонён trust check'ом (но handshake продолжился
  как best-effort), записи application data успешно encrypt'ятся/decrypt'ятся.
  3/3 PASS.

Сессия зафиксирована в §23 этого CLAUDE.md.

---

## 19. Этап 44 — 3D-стек (OpenGL ES 2.0 software rasterizer), ✅ ЗАВЕРШЁН

**Дата:** 16.05.2026. Этап 43 (Neural Icon Forge) пропущен по решению
пользователя: «делай реализацию этапа 44, 43 этап пока пропустим, иконки
ещё не готовы». Когда иконки появятся — вернёмся.

### 19.1 Реализация FPU + SSE/SSE2 в ядре (фундамент)

До этапа 44 ядро AMPLIFY-OS собиралось с `-mno-mmx -mno-sse -mno-sse2`,
потому что без правильной инициализации CR0/CR4 и без save/restore
FPU-state на context-switch'ах любое использование xmm-регистров
приводило бы к corruption между задачами и прерываниями. Этап 44
исправил это полностью.

Пользователь явно попросил: «FPU/SSE — зачем обходишь, реализуй это и
используй это для реализации данного этапа». Поэтому вместо обходного
fixed-point Q16.16 (как было в первом черновике) мы сделали правильно:

1. **`kernel/arch/x86_64/boot.asm`** — оба пути инициализации:
   - **`_start` (32-bit GRUB-path):** до прыжка в long mode выполняем
     FNINIT, ставим CR0 bits (MP=1, EM=0, TS=0, NE=1), затем при
     включении CR4.PAE добавляем CR4.OSFXSR (bit 9) +
     CR4.OSXMMEXCPT (bit 10), плюс LDMXCSR(0x1F80) для маскировки
     всех 6 SIMD FP-исключений.
   - **`_efi_start` (UEFI-path):** то же самое, но 64-битным кодом
     с REX-префиксами. Аналогичные CR0/CR4 биты и FNINIT/LDMXCSR.

2. **`Makefile.x86_64`** — флаги `-mno-mmx/-mno-sse/-mno-sse2` выпилены.
   `-mno-red-zone` и `-mcmodel=kernel` остаются (это обязательное).
   GCC теперь свободно использует xmm-регистры в любом kernel-файле.

3. **`kernel/arch/x86_64/task.{h,c}`** — `task_t.fpu_state` (указатель
   на 512-байтовый 16-byte aligned буфер, отдельная kmalloc_aligned-
   аллокация). Шаблон чистого FPU-state (`g_fpu_template[512]`)
   инициализируется лениво через FNINIT + LDMXCSR + FXSAVE.
   `task_create_user/task_create_thread` копируют шаблон в новый
   fpu_state, `task_yield` делает FXSAVE old / FXRSTOR new вокруг
   `context_switch`, `task_exit` освобождает блок через kfree.

   Технический нюанс: `sizeof(heap_block_t) = 40 байт` (не кратно 16),
   поэтому payload от обычного kmalloc — это address вида 16N + 8.
   Для FXSAVE/FXRSTOR обязательно 16-byte aligned адрес, поэтому
   `fpu_state` отдельно аллоцируется через `kmalloc_aligned(512, 16)`.

4. **`kernel/arch/x86_64/isr.asm`** — `isr_common` обёрнут в FXSAVE/
   FXRSTOR на 512-байтовом scratch'е на kernel-stack'е. Это
   критически важно после включения SSE: любой IRQ (PIT @ 100 Hz,
   keyboard, mouse, network) теперь может попадать прямо в середину
   FPU-intensive кода (например, рендера GLES) и FXSAVE/FXRSTOR
   защищает прерванный контекст от corruption. Alignment-математика:
   после 15×push qword (120 байт) + 56 байт сверху от CPU+stub'а
   RSP оказывается 16-byte aligned, поэтому `sub rsp, 512;
   fxsave [rsp]` корректно ложится на 16-aligned адрес.

5. **`kernel/arch/x86_64/syscall_entry.asm`** — то же самое для
   SYSCALL/SYSRET пути, чтобы userspace-программы, активно
   использующие FPU/SSE (libc math, наш собственный pthread), не
   теряли регистровое состояние при системных вызовах.
   **Заодно исправлен alignment-баг:** оригинальный код делал
   `call syscall_dispatch` с RSP в 8 mod 16, что нарушает SysV ABI
   (требуется 0 mod 16 перед call). До SSE-эпохи это никто не замечал,
   потому что GCC не использовал 16-byte aligned SIMD-инструкции;
   с включённым SSE2 он мог бы (теоретически) emit movdqa и
   получить #GP. Поэтому добавлен `sub rsp, 8` непосредственно
   перед call.

**Synthax-grits:** NASM использует мнемоники `fxsave`/`fxrstor` (без
суффикса `q`, который относится к GAS). Внутри GCC inline asm
(в `task.c`) — наоборот, `fxsaveq`/`fxrstorq` (GAS-стиль).

### 19.2 GLES2-подобный API + софтовый растеризатор

Основные файлы:
- `kernel/arch/x86_64/gles.h` (~330 строк) — публичный API.
- `kernel/arch/x86_64/gles_math.c` (~240 строк) — math (sin/cos/sqrt/
  mat4/vec3).
- `kernel/arch/x86_64/gles.c` (~520 строк) — state machine + EGL-
  context + растеризатор + сэмплер.

**Подход к шейдерам:** «шейдер» — это указатель на C-функцию со
строго определённой сигнатурой. Это приём SwiftShader-Lite/ANGLE для
embedded-сценариев. Полноценный GLSL-компилятор был бы 30K+ строк
(парсер + AST + IR + SSA + JIT-codegen), что заведомо избыточно для
демонстрации работающего 3D-стека. Если когда-то будет нужно
портировать настоящий браузер или Doom 3 — добавим парсер GLSL
отдельно.

**Растеризатор:** Pineda 1988 edge-functions. Для каждого треугольника:
1. Vertex shader → clip-space coordinates.
2. Простой rejection-clipping: если w≤0 или z вне [-w..w] — отбрасываем
   треугольник целиком (грубо, но для куба в центре сцены работает;
   полный Sutherland-Hodgman — задача будущего).
3. Perspective divide → NDC.
4. Viewport transform → screen-space (с инвертированным Y, потому что
   у нас framebuffer +Y вниз, а OpenGL +Y вверх).
5. Backface culling по знаку 2D-area (с учётом Y-инверсии — фронт-CCW
   даёт area < 0 в нашем screen-space).
6. Bounding box → scan-line по каждому пикселю.
7. Внутри bounding box: edge-functions w0, w1, w2 + барицентрики =
   (w0,w1,w2)/area.
8. Z-test против depth-buffer'а контекста.
9. **Perspective-correct interpolation varying'ов:** в process_vertex
   мы заранее умножаем varyings на 1/w. На пикселе восстанавливаем
   `w_pix = 1 / (b0/w0 + b1/w1 + b2/w2)` и `varying_pix = w_pix *
   (b0*varying0/w0 + b1*varying1/w1 + b2*varying2/w2)`. Это критично
   для текстурирования больших треугольников — без этого UV
   coordinates сильно искажаются на углах.
10. Fragment shader → ARGB color.
11. Optional α-blend (src-over-dst).

**Текстуры:** GL_NEAREST (тривиально), GL_LINEAR (bilinear, 4 texel'а
с per-channel смешиванием), GL_REPEAT (fract(uv)) / GL_CLAMP_TO_EDGE.

### 19.3 Демо-приложение `app_glcube`

Текстурированный куб (`kernel/arch/x86_64/app_glcube.c`, ~290 строк):
- Окно 360×280 с собственным GLES-контекстом + Z-buffer.
- Геометрия: 6 граней × 4 вершины × 8 float'ов (pos+normal+uv) = 768 байт
  + 36 индексов (uint16_t).
- Каждый кадр: timer_get_uptime_ms() → angle_y и angle_x.
  model = rotate_y * rotate_x, view = lookat([0,0,3] → [0,0,0]),
  proj = perspective(60°, w/h, 0.1, 50). MVP = proj * view * model.
- Vertex shader: умножает pos на MVP, трансформирует нормаль через
  model (без translation, w=0), нормализует, передаёт UV + cam-space-w
  как varying для fog'а.
- Fragment shader: сэмплит шахматную текстуру 64×64 (генерируется
  через `gx_texture_make_checker` цветами orange+gold —
  AMPLIFY-стилистика), считает Lambert (ambient 0.25 + 0.75 *
  max(dot(N, L), 0)), затемняет с глубиной (fog factor 1..0).
- HUD внизу окна (18 px high): FPS, triangles_drawn/total, pixels_shaded.
- Регистрация в `kernel64_main.c`: иконка "3D куб" на десктопе
  (`launch_glcube`) + пункт "3D куб (GLES)" в startmenu.

### 19.4 Сборка и верификация

```bash
make ARCH=x86_64                       # full build, ~10 сек
# → build/kernel-x86_64.bin (288 KB), build/myos-x86_64.iso (9.6 MB),
#   build/esp.img (64 MB)
make ARCH=x86_64 run                   # UEFI boot через OVMF
```

Headless-проверка boot'а (`-display none -serial file:...`) показывает
полный boot до login screen за 5160 мс uptime. Подтверждено, что
CPU features detection видит `FPU SSE SSE2` — наши CR0/CR4 биты
успешно установлены.

Интерактивная проверка: после login кликнуть по иконке "3D куб" на
рабочем столе → откроется окно с вращающимся текстурированным кубом,
внизу — счётчик FPS. На QEMU TCG (без KVM) ожидается ~30 FPS при
viewport 360×260, ~10-15 FPS при полноэкранном 1024×768. Для
комфортных 60 FPS на полном экране понадобится либо KVM-ускорение,
либо SIMD-batch фрагментный шейдер (отложено).

### 19.5 Уроки на будущее

1. **Никогда не обходи FPU фиксированной точкой в "чистом" коде.**
   Первоначально я хотел использовать Q16.16, чтобы избежать
   возни с CR0/CR4. Пользователь правильно настоял — теперь у нас
   нормальная single-precision математика, и GCC может авто-
   векторизовать SSE-код во всём ядре. Это даст преимущество всем
   будущим математически-интенсивным подсистемам (Crypto, FFT для
   audio, llama.cpp в этапе 49).

2. **При включении SSE в kernel-mode обязательно save/restore FPU
   ВО ВСЕХ async-точках:** task switch, IRQ entry, SYSCALL entry.
   Пропуск любой одной даёт subtle corruption через 100 IRQ/sec
   (один шанс из 10K, что IRQ попадёт ровно между двумя FPU-
   инструкциями), что почти невозможно отладить.

3. **FXSAVE требует 16-byte alignment.** Кушает sizeof(heap_block_t)
   = 40 байт не кратно 16, поэтому per-task FPU-state нужно
   аллоцировать через `kmalloc_aligned(512, 16)`, а не как поле
   внутри обычно-выделенного task_t.

4. **При FXSAVE/FXRSTOR в asm-stub'е следить за RSP-alignment.**
   `fxsave [rsp]` требует RSP % 16 == 0. Если RSP оказался 8-aligned
   из-за нечётного числа push'ей перед, нужно sub'нуть ещё 8.

5. **NASM vs GAS синтаксис.** NASM использует `fxsave`/`fxrstor`
   без суффиксов; GCC inline asm (GAS-стиль) использует `fxsaveq`/
   `fxrstorq`. Это первое, на что споткнётся следующий ИИ при
   копи-пасте.

**Следующий шаг — этап 45 (forge — пакетный менеджер).** См. §15 roadmap.


---

## 20. Этап 45 — Amply пакетный менеджер + полная FAT32, ✅ ЗАВЕРШЁН

**Дата:** 16.05.2026. Этап 43 (Neural Icon Forge) по-прежнему отложен — пользователь
ещё не подготовил иконки.

### 20.1 Полная переработка FAT32 (kernel/arch/x86_64/fat32.{c,h})

До этапа 45 драйвер был root + 8.3 only. Это блокировало любую установку
пакета: куда класть `theme.cfg`, если в FAT32 нет директорий? Решение —
полная переписка `fat32.c` под:

- **Произвольная вложенность каталогов.** Введён `resolve_parent(path)`,
  который проходит по компонентам пути, начиная с `g_bpb.root_cluster`,
  и возвращает first-cluster родительского каталога + указатель на
  basename. Все публичные функции (`fat32_read_file/write/create/save/
  delete`) теперь принимают абсолютные пути любой глубины.
- **Long File Names (LFN).** Имя длиннее 8.3 или содержащее не-ASCII /
  нижний регистр / пробелы пишется как серия 32-байтовых записей с
  attr=0x0F: каждая хранит 13 UCS-2 символов в полях `name1[5]/name2[6]/
  name3[2]`. Перед короткой 8.3-записью лежат LFN-записи в обратном
  порядке (последняя — с битом 0x40 в `order`). Контрольная сумма
  привязана к 8.3 имени через rotate-1. Сборка LFN-имени делается
  `lfn_collector_t` в `walk_dir_clusters`. Маппинг UCS-2↔CP1251 — один-к-
  одному в `local_to_ucs2/ucs2_to_local` (текущая сборка использует
  `-fexec-charset=CP1251`, поэтому каждый кириллический символ — 1 байт).
- **Авто-расширение цепочки кластеров на write.** Прежний `fat32_write_file`
  падал, если данные не помещались в уже выделенные кластеры. Теперь
  `write_cluster_chain_resize()` идёт по цепочке, доступает новые кластеры
  через `fat_extend_chain` (FAT-find-free + установка cluster в
  end-of-chain), и на сжатии файла обрезает хвост.
- **mkdir / mkdir -p / iter_dir.** Создание каталога: выделяем кластер,
  обнуляем, пишем "." + ".." dirent'ы (с правильным `cluster_for_dotdot`
  = parent или 0 если parent — root), затем вставляем dirent в parent
  (с LFN-серией, если имя длиннее 8.3). `fat32_mkdir_p` идёт по
  компонентам и создаёт каждый недостающий. `fat32_iter_dir` через
  `walk_dir_clusters` отдаёт `fat32_dir_entry_t {name, is_dir, size,
  first_cluster}`.
- **Регистрация в VFS теперь рекурсивная.** При mount `register_dir(root,
  "/disk", ...)` обходит все подкаталоги через `walk_dir_clusters` +
  callback `register_walk_cb`, складывает в VFS файлы с полным путём
  (`/disk/Sanctum/Amply/Welcome/welcome.txt` — 36 символов; VFS_MAX_NAME=64,
  с запасом).
- **Совместимость со старым кодом.** Все 12 GUI-приложений и shell-команды,
  которые использовали `/disk/HELLO.TXT` и т.п., продолжают работать без
  правок. Пути с одной компонентой (без слешей) после `/disk/` идут
  через тот же код-путь, просто с короткой раскруткой компонентов.

### 20.2 Формат .amf и подпись AMPLIFY-Sig-v1

**Header (48 байт):**
```
"AMF1" magic | ver=1 | manifest_len | file_count | payload_len(u64) |
compression(u32, 0=NONE) | reserved | signature[16]
```

**Подпись.** signature[0..7] = FNV-1a64(manifest||table||payload, FNV_OFFSET).
signature[8..15] = FNV-1a64(тот же blob, AMPLY_KEY_DEFAULT=0x414D504C59475231 =
"AMPLYGR1"). Знание AMPLY_KEY_DEFAULT не нужно для пользователя — оно
вшито в `amply.c` и в host-утилиту `mkamf`. Защита: пакет, отредактированный
любым образом без знания AMPLY_KEY_DEFAULT, не пройдёт верификацию (второй
FNV никогда не сойдётся). Это не криптостойкая подпись — Ed25519 будет в
этапе 47 (TLS+crypto). Размер 16 байт можно расширить до 64 байт через
ver=2 (32 байта Ed25519-signature + 32 байта public-key).

**File entry (120 байт):**
```
char path[96] | u64 offset | u64 size | u32 mode | u32 reserved
```

Path всегда абсолютный (`/disk/Sanctum/...`), на этапе 45 поддерживается
любая глубина благодаря новому FAT32.

### 20.3 Host-утилита tools/mkamf

`tools/mkamf <out.amf> <package_dir>`. package_dir содержит:
* `manifest.ini` — INI с блоками `[package]` (name/version/summary/author)
  и `[depends]` (`<pkg>=<version_spec>`).
* `files/<rel-path>` — рекурсивно собирается, маппится в .amf как
  `/disk/<rel-path>`.

Скан папки через `opendir/readdir`, рекурсивный `collect_files`, расчёт
обоих FNV. Запись: header → manifest → file_table → payload (файлы
последовательно).

### 20.4 Kernel-side amply.{c,h}

**Состояние:** статическая БД `g_db[32]` + индекс репо `g_repo[32]` +
`g_repo_url[256]`. Stack для DFS обнаружения циклов `g_dep_stack[32]`.

**Ключевые функции:**
* `amply_init` — `mkdir -p /disk/Sanctum/Amply` + `repo_load` (initrd
  `amply_index.txt`) + `db_load` (`/disk/Sanctum/Amply/db.txt`) +
  `repo_url_load`.
* `amply_install` сбрасывает `g_dep_depth = 0`, вызывает `install_one`
  рекурсивно.
* `install_one` — DFS с детектом цикла: `dep_stack_has(name)` → `[error]
  dependency cycle on X`. Для каждого депа `install_one(dep, ...)` перед
  распаковкой. Распаковка с журналом: на любую ошибку write/oom все ранее
  распакованные файлы удаляются (откат).
* `amply_remove` — проверка обратных зависимостей: пробегает по всем
  `g_db[]` и для каждого пакета (если он в репо есть) парсит manifest и
  смотрит `mf.deps[]`. Если кто-то depends-on `name` — отказ.
* `amply_verify_installed` — проверяет наличие всех файлов на диске +
  заново верифицирует .amf (двойную FNV).
* `amply_upgrade_all` — `cmp_version` (числовое лексикографическое: `1.0
  < 1.0.1 < 1.1`) + remove+install для каждого устаревшего.

**Персист БД:** простой текстовый формат
`name|version|n_files\n<file1>\n<file2>\n...\n` в `/disk/Sanctum/Amply/db.txt`.
Парсинг толерантный к пустым строкам.

### 20.5 Shell-команда `Amply`

`kernel/arch/x86_64/shell.c:cmd_amply` — расширение существующего
диспатчера на `Amply install/remove/upgrade/search/info/list/verify/repo/
repo-show`. Цветной вывод журнала: парсим строки на `[ok]`, `[done]`,
`[install]` → зелёный, `[error]/[bad-]/[miss]` → красный, `[hint]/[skip]/
[upgr]` → жёлтый, остальные `[fetch]/[dep]` → акцентный.

### 20.6 GUI Amply Center

`apps.c:app_amply_open()` (~330 строк). Окно 620×420 с двумя колонками:

**Левая** (260 px) — список пакетов; таб-переключатель в заголовке
"[Каталог] / [Установлено]". Клик подсвечивает выбранный.
**Правая** (310 px) — журнал последних операций (16 KB rolling buffer,
автоматически сжимается при переполнении).
**Toolbar внизу** — 5 кнопок: Установить / Удалить / Проверить / Обновить /
Очистить лог (распределены равномерно по ширине).

Hotkeys: Enter — Установить, D — Удалить, V — Проверить, U — Обновить,
L — Очистить лог.

Иконка "Amply" (glyph='A', accent оранжевый) на рабочем столе и пункт
"Amply Center" в startmenu (горячая клавиша 'A').

### 20.7 Сборка & smoke

```bash
make ARCH=x86_64                      # full build, ~12 сек
# → 3 .amf пакета в build/amply/, общий payload 3.2 KB
# → kernel-x86_64.bin 292 KB, initrd 250 KB
make ARCH=x86_64 run                  # UEFI boot через OVMF
bash scripts/test_stage45_amply.sh    # 25/25 PASS
```

Boot-smoke (до splash) выполняет `Amply install cyber-theme` (с подтягиванием
`welcome` как зависимость), `verify`, `remove` — всё видно в serial-логе:

```
[smoke 45] Amply install cyber-theme (with deps)
[fetch] cyber-theme 1.2.0 from amply_cyber_theme.amf
[ok] signature valid (1269 bytes)
[dep] cyber-theme -> welcome
[fetch] welcome 1.0.0 from amply_welcome.amf
[ok] signature valid (1493 bytes)
[install] /disk/Sanctum/Amply/Welcome/changelog.txt (197 bytes)
[install] /disk/Sanctum/Amply/Welcome/welcome.txt (867 bytes)
[done] welcome 1.0.0 installed
[install] /disk/Sanctum/themes/cyber/wallpaper.txt (382 bytes)
[install] /disk/Sanctum/themes/cyber/theme.cfg (420 bytes)
[done] cyber-theme 1.2.0 installed
[smoke 45] install r=0 (OK)
[smoke 45] db_count=2
[ok] verify cyber-theme OK
[smoke 45] Amply remove cyber-theme
[remove] /disk/Sanctum/themes/cyber/wallpaper.txt
[remove] /disk/Sanctum/themes/cyber/theme.cfg
[done] removed cyber-theme
[smoke 45] db_count after remove=1
```

### 20.8 Уроки и подводные камни

1. **2 МБ stack-buffer в kernel-mode = триплфолт.** В первой версии я
   писал `char tmp[2 * 1024 * 1024]` в `amply_load_amf` для FAT32-fallback.
   Kernel-stack 16 КБ → молча зависает (без serial-вывода ошибки!).
   Обходится через `kmalloc(MAX_AMF)`. Урок: любые `>4KB` локальные
   массивы в kernel-функциях — только через heap.
2. **VFS_MAX_NAME=64 байта ограничивает глубину.** Путь
   `/disk/Sanctum/Amply/Welcome/welcome.txt` = 38 символов. Запас есть,
   но если завести `/disk/Sanctum/Amply/Repository/Categories/Sound/...`
   можно упереться. Если потребуется — увеличить до 128 в `vfs.h`.
3. **mkinitrd теряет +x после rebuild.** `gcc 2>/dev/null` иногда
   создаёт файл с правами `rw-r--r--` (наследование от umask после
   sudo-операций). В Makefile уже есть workaround через `chmod +x`,
   но если новая host-утилита (`mkamf`) станет ругаться — `chmod +x build/mkamf`.
4. **Initrd-файлы регистрируются в VFS БЕЗ ведущего слэша.** Поэтому
   `amply_index.txt` мы открываем как `"amply_index.txt"` (а не
   `"/amply_index.txt"`). Это давний дизайн initrd.c, важно помнить
   при добавлении новых initrd-ресурсов.
5. **`opendir/readdir/stat` в host-утилите C99.** GCC ругается на
   `snprintf` truncation warnings, но они невалидные — это false
   positive из-за `__attribute__((warn_unused_result))` glibc.
   Игнорируем (можно выключить `-Wno-format-truncation`).
6. **FAT32 `.` и `..` dirent'ы в новом каталоге обязательны.** Без них
   многие FS-checker'ы (включая Linux fsck.fat) ругаются. В нашей
   реализации `fat32_mkdir` их пишет в первый кластер сразу же.
7. **`fat32_iter_dir` пропускает '.' / '..'.** Иначе при `Amply list`
   мы бы видели "сам каталог" как запись.

### 20.9 Связь с предыдущими этапами

- **Этап 38 (UEFI)** — initrd доставляется через ESP в обоих путях
  загрузки; пакеты `amply_*.amf` как обычные initrd-модули.
- **Этап 40 (libc)** — БД `db.txt` использует те же fat32_save_file/
  read_file API, что и users.txt, notes.txt, paint.ppm.
- **Этап 41 (virtio-gpu)** — Amply Center рисуется в обычном
  `compositor`-окне (тот же путь, что и 12 других приложений).
- **Этап 42 (compositing)** — окно Amply Center получает open-animation
  (180 мс ease-out cubic) и тени бесплатно.
- **users.h** (этап 37.6c-4) — переиспользована та же FNV-1a 64-bit
  хеш-функция (через `users_hash`-стиль реализацию `fnv1a64_seed` в
  `amply.c`). Когда в этапе 47 заменим на Ed25519, будем подменять
  только подсистему подписи.

Следующий этап — **46 (Sandboxes — namespaces + cgroups + seccomp).**
Туда же переедут долги fork/exec/pipe/chdir/shm_open из этапа 40.


---

## 21. HOTFIX 16.05.2026 — регрессия SMP после этапа 44 (FPU/SSE)

**Дата:** 16.05.2026 (после завершения этапа 45).

### 21.1 Симптом

После сборки этапа 45 каждый запуск ОС зависал на:
```
[smp] starting APs, total CPUs=4
```
…и не двигался дальше. QEMU становился безответным, BSP не печатал ни
`[smp] cores up`, ни любых последующих строк (kbd init, doublebuf, drm,
compositor, amply). Поведение было идентичным для UEFI-пути
(`make run-uefi`) и GRUB/BIOS-пути (`run-bios`), что исключало проблему
в loader'е. Регрессия носила фатальный характер — пользователь не мог
ни тестировать пакетный менеджер интерактивно, ни увидеть splash/login.

### 21.2 Диагностика

Аудит-цепочка ИИ-агента 16.05.2026:

1. **Изолировать SMP.** Закомментировав `smp_init()` в `kernel64_main.c`
   и принудительно выставив `smp_cpus_started = 1`, BSP благополучно
   доходил до splash → firstboot → wm_main_loop, и весь Amply smoke-блок
   отрабатывал корректно. ⇒ Проблема строго в SMP-boot, остальные
   подсистемы работают.

2. **Гранулярная инструментация `smp.c`.** Добавлены serial-puts
   между всеми шагами (resv trampoline page → blob copy → patch →
   stacks alloc → BSP info → INIT-assert → INIT-deassert → SIPI 1 →
   SIPI 2). Лог показал: BSP успешно проходит INIT (assert + deassert
   + два `busy_wait_ms`) и доходит до `SIPI 1 send`, после чего:
   ```
   [smp] DBG    SIPI 1 send
   [smp] DBG    SIPI 1 returned, wait 2ms
   ```
   `lapic_send_ipi` возвращается из polling-цикла Delivery Status, мы
   успеваем напечатать «returned, wait 2ms», и **далее — тишина**.
   Следующий `SIPI 2 send` не печатается.

3. **`-d cpu_reset` в QEMU.** Включив дамп CPU-state на любой reset,
   мы получили в лог-файле:
   ```
   CPU Reset (CPU 1)
   CR0=00010033 CR4=00000640 ...
   Triple fault
   ```
   AP №1 уходит в **triple fault** при значении `CR4=0x640`. Декодируем:
   - bit 5 (PAE)        = 1 ✅
   - bit 9 (OSFXSR)     = 0 ❌
   - bit 10 (OSXMMEXCPT) = 0 ❌

   ⇒ AP вошёл в Long Mode без поддержки SSE на уровне CR4.

4. **Каскад `triple fault` → QEMU stop.** При `-no-reboot` QEMU
   трактует triple-fault на любом CPU как fatal для всей VM. Поэтому
   когда AP уходит в triple-fault, BSP тоже замирает (всю VM Suspend'ит
   виртуализатор). Это и объясняло «BSP не печатает дальше».

### 21.3 Корневая причина

Этап 44 (3D-стек GLES) добавил в `boot.asm` правильную инициализацию
FPU/SSE для BSP:
- CR0 → set MP|NE, clear EM|TS
- CR4 → set PAE|OSFXSR|OSXMMEXCPT
- FNINIT
- LDMXCSR(0x1F80)

Параллельно из `Makefile.x86_64` были выпилены флаги
`-mno-mmx -mno-sse -mno-sse2` — GCC получил право свободно
использовать xmm-регистры в любом kernel-коде. Каждый C-файл теперь
может содержать SSE/SSE2-инструкции в прологах/эпилогах функций
(особенно при `-O2`).

Однако `smp_trampoline.asm` (последняя ручная правка — май 11,
**до этапа 44**) поднимал AP в Long Mode только с `CR4.PAE`. Он
не трогал CR0.MP, CR0.NE, CR4.OSFXSR, CR4.OSXMMEXCPT, не делал
FNINIT и не загружал MXCSR. Поэтому первая же SSE-инструкция,
которую GCC сгенерировал в прологе `ap_main64` (или в любой
вызываемой функции — `lapic_init`, `__sync_fetch_and_add`, etc.),
давала `#UD` (Invalid Opcode, потому что без OSFXSR — SSE off) →
`#DF` (Double Fault, потому что #UD handler не настроен на AP
до того, как мы вошли в IDT) → triple fault.

### 21.4 Фикс

Изменение в `kernel/arch/x86_64/smp_trampoline.asm`, секция
`tramp_pm32` (после установки сегментных регистров, перед CR3):

```asm
[bits 32]
tramp_pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- [этап 44] Инициализация FPU + SSE на AP ----
    ; CR0: MP=1, EM=0, TS=0, NE=1
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))    ; clear EM, TS
    or  eax, (1 << 1) | (1 << 5)       ; set MP, NE
    mov cr0, eax
    fninit

    ; CR3
    mov eax, [tramp_pml4_phys - trampoline_start + TRAMPOLINE_PHYS]
    mov cr3, eax

    ; CR4: PAE|OSFXSR|OSXMMEXCPT
    mov eax, cr4
    or  eax, (1 << 5) | (1 << 9) | (1 << 10)
    mov cr4, eax

    ; MXCSR default (mask all 6 SIMD FP exceptions)
    push dword 0x1F80
    ldmxcsr [esp]
    add esp, 4

    ; ... EFER.LME, CR0.PG, lgdt → 64-bit ...
```

Это зеркальная копия FPU/SSE-инициализации из `boot.asm` для BSP.
Размер trampoline-blob'а увеличился незначительно (~30 байт), всё
по-прежнему укладывается в 4 KB-страницу 0x8000.

### 21.5 Верификация

`bash scripts/test_stage45_amply.sh` → **25/25 PASS**.

```
[smp] starting APs, total CPUs=4
[smp] AP online: lapic_id=1 slot=1
[smp] AP online: lapic_id=2 slot=2
[smp] AP online: lapic_id=3 slot=3
[smp] cores up: 4/4
```

После фикса:
- Все 4 CPU поднимаются.
- Splash, firstboot (если первый запуск), login — работают.
- Amply smoke install/verify/remove проходит за ~6.9 сек uptime
  (было 5.16 сек до 45 — добавился mkdir/persist в FAT32, ОК).

### 21.6 Уроки на будущее

1. **Любая активация FPU/SSE/AVX в kernel требует инициализации
   ВЕЗДЕ, где CPU входит в Long Mode** — не только в BSP boot path.
   В `boot.asm` ДВА таких пути (GRUB `_start` и UEFI `_efi_start`),
   в `smp_trampoline.asm` — ещё один (для AP). Когда будут SMP
   suspend/resume или live-migrate в этапе 50 (Hypervisor VT-x) —
   ещё пара путей.

2. **`-no-reboot` в QEMU = triple-fault на AP останавливает всю VM.**
   Это не баг QEMU, а ожидаемое поведение. Без `-no-reboot` QEMU
   тихо сбросил бы AP, и мы могли бы не заметить проблемы вообще
   (только smp_cpus_started так и остался бы = 1). Так что в нашем
   случае `-no-reboot` помог быстро локализовать баг.

3. **`-d cpu_reset` — лучший друг при диагностике загадочных
   зависаний QEMU.** Этот флаг записывает в лог-файл полный
   снимок CPU-state перед каждым reset'ом. Видно CR0, CR4, EFER,
   все сегментные регистры, FPR, XMM, и тип события («Triple fault»).
   Команда: `-d cpu_reset -D /tmp/cpu_reset.log`. Для всех будущих
   «висит и непонятно почему» — начинайте с этого флага.

4. **Не оставляйте серийный `serial_puts` debug в продакшн-коде.**
   После фикса инструментация была полностью убрана — `smp.c` и
   `kernel64_main.c` вернулись к прежнему виду. Debug-логи имеют
   привычку плодиться и засорять лог-файлы.

5. **При добавлении любой кросс-CPU-фичи (новая CR0/CR4-бит, MSR,
   FPU-state, segment-register) — всегда проверяйте, что её
   инициализация повторяется во ВСЕХ entry-точках.** Чек-лист для
   AMPLIFY-OS:
   - `kernel/arch/x86_64/boot.asm:_start` (GRUB 32-bit вход)
   - `kernel/arch/x86_64/boot.asm:_efi_start` (UEFI 64-bit вход)
   - `kernel/arch/x86_64/smp_trampoline.asm:tramp_pm32`
     (AP при INIT-SIPI)

### 21.7 Связь с предыдущими этапами

- **Этап 37 (Long Mode)** — установил SMP-boot с trampoline.
- **Этап 44 (3D-стек)** — добавил FPU+SSE для BSP (но забыл AP).
  Это и было «провисом» — баг был внесён на 44, проявился на 45,
  потому что test_stage45_amply.sh — первый автотест, который
  гоняет SMP через QEMU после 44 (стадии 41-44 тестировались либо
  без SMP, либо с -smp 1).
- **Этап 45 (Amply)** — выявил проблему как побочный эффект
  расширенного smoke-теста.

**Файлы, изменённые в hotfix'е:**

| Файл | Изменение |
|------|-----------|
| `kernel/arch/x86_64/smp_trampoline.asm` | +30 строк в `tramp_pm32`: CR0/CR4/FNINIT/LDMXCSR |
| `CLAUDE.md` | Этот раздел (§21) + ссылка из шапки |


---

## 22. HOTFIX 20.05.2026 — FullHD 1920×1080 + ускорение 3D-куба

**Дата:** 20.05.2026 (после завершения этапа 45 + предыдущих HOTFIX-ов).

### 22.1 Симптомы

Два независимых пользовательских отчёта, которые я закрыл одним заходом:

1. **«Хочу FullHD».** Ядро поднималось в 1024×768×32 — это VESA-режим из
   ранних этапов 37.x, когда мы только начали рисовать в framebuffer.
   Современное оборудование легко даёт 1920×1080, и наш virtio-gpu и
   compositor рассчитаны на любые разумные размеры (всё считывают из
   `fb_state()`). Просто надо было поднять заявку.

2. **«3D куб в моём приложении даёт всего 15 FPS, с проскоками до 30 FPS».**
   Симптом: куб вращается рваными скачками, причём счётчик FPS в HUD куба
   показывает то 10, то 30, то 15. На любое движение мыши над окном —
   мгновенный кадр.

### 22.2 Диагностика (что нашли Explore-агенты)

**По резолюции** (см. §22.3):
- Хардкод 1024×768 нашёлся в ДВУХ местах: `boot.asm` Multiboot2-framebuffer-
  тэг (строки 65-67) и `uefi/main.c` GOP-target (`want_w = 1024, want_h = 768`,
  строка 408). Всё остальное — динамическое.
- `virtio_gpu.c` уже умеет `GET_DISPLAY_INFO` и берёт размер из ответа;
  если QEMU настроен на 1920×1080, virtio-gpu сам прочитает 1920×1080.
- `fb_init` берёт размер из MB2-tag.
- `doublebuf_init` аллоцирует `fb_state().width * height * 4` байт.
- `compositor_init`, `wm`, `splash`, `login`, `desktop`, все 12 apps — НИ ОДНОГО
  хардкода 1024 или 768.

**По 3D-кубу** (см. §22.4):
- **Корень №1 (главный):** WM с HOTFIX 19.05.2026 ушёл в idle 10 FPS,
  если нет mouse/kbd событий. Куб сам не генерирует events, а WM не
  знает, что окно «анимируется» (компоновщик различает только
  open/close-анимации, а собственные анимации внутри окна — нет). Отсюда
  потолок 10 FPS. Проскоки до 30 — это случайные user events.
- **Корень №2:** В растеризаторе `gles.c` per-pixel deление `1.0/interp_inv_w`
  + per-pixel пересчёт edge functions + per-pixel пересчёт numerator'ов
  для 6 varying'ов = ~60 операций/пиксель. На QEMU TCG это медленно.
- **Корень №3:** `app_glcube` использовал GL_LINEAR (биленейный) сэмплинг
  текстуры — это 4 текселя × 4 канала + per-channel смешивание = +64
  операции на каждом пикселе. Визуально на 64×64 шахматной текстуре под
  Lambert+fog разница NEAREST/LINEAR незаметна.

### 22.3 Правки: разрешение

**`kernel/arch/x86_64/boot.asm`** (Multiboot2 framebuffer-tag):
```asm
mb2_fb_tag_start:
    dw 5                    ; type = framebuffer
    dw 0
    dd mb2_fb_tag_end - mb2_fb_tag_start
    dd 1920                 ; width  (было 1024)
    dd 1080                 ; height (было 768)
    dd 32                   ; depth
mb2_fb_tag_end:
```

GRUB через VBE поднимет ближайший доступный VESA-режим (на современном
QEMU 1920×1080 точный). На реальном железе со старым VBE-BIOS'ом может
вернуть ближайший меньший — `fb_init` всё равно возьмёт реальные размеры
из mb2-tag, и compositor/WM подстроятся.

**`uefi/main.c`** (GOP target):
```c
UINTN want_w = 1920, want_h = 1080;  // было 1024, 768
```

`query_gop` минимизирует `(dw² + dh²)` по доступным GOP-режимам.

**`Makefile.x86_64`** (QEMU args для всех run-* целей):
```makefile
# Было: -vga virtio
# Стало:
-vga none -device virtio-vga,xres=1920,yres=1080

# Для legacy -vga std (run-gpu/run-gpu-direct/run-direct/run-debug):
-vga std -global VGA.vgamem_mb=16
-device virtio-gpu-pci,...,xres=1920,yres=1080
```

`xres=/yres=` подсказывает virtio-gpu QEMU-стороны, какое разрешение
рапортовать в `GET_DISPLAY_INFO`. Без них virtio-gpu по умолчанию
рапортует 1280×800 — kernel всё равно создавал resource 1920×1080
(потому что doublebuf берёт fb_state), но `GET_DISPLAY_INFO` показывал
несогласованное значение в serial-логе.

### 22.4 Правки: ускорение 3D

**Правка №1: `window_t.wants_continuous_redraw`**

`kernel/arch/x86_64/window.h` — новое поле:
```c
typedef struct window {
    ...
    bool      wants_continuous_redraw;  // окно просит непрерывный рендер
    ...
} window_t;
```

`kernel/arch/x86_64/window.c:window_create` — инициализация в false.

`kernel/arch/x86_64/wm.c:wm_main_loop` — проверка перед idle-веткой:
```c
bool want_animated = false;
for (window_t* win = wm_iterate(0); win; win = wm_iterate(win)) {
    if (win->visible && win->wants_continuous_redraw) {
        want_animated = true;
        break;
    }
}
bool interactive = mouse_has_event() || kbd_event || anim_active || want_animated;
```

`kernel/arch/x86_64/app_glcube.c:app_glcube_open` — установка флага:
```c
win->wants_continuous_redraw = true;  // куб вращается сам по таймеру
```

**Логика:** WM с HOTFIX 19.05.2026 (`idle_threshold = 10` тиков PIT =
100 мс = 10 FPS) форсит рендер только на user events или активные
compositor-анимации. Анимированные окна (куб, видеоплеер, игры) теперь
тоже считаются «активными». Остальные окна — статичные — остаются в
idle-режиме, экономят CPU.

**Правка №2: инкрементальный rasterizer**

`kernel/arch/x86_64/gles.c:rasterize_triangle` — переписан.

**До:** на каждый пиксель пересчитываем edge_fn × 3 (6 mul + 6 sub каждая
= 18 mul + 18 sub), затем барицентрики (3 mul), Z (3 mul + 2 add),
interp_inv_w (3 mul + 2 add), 1.0/interp_inv_w (1 div), numerator'ы
варингов (3 mul + 2 add × N = 18 mul + 12 add для N=6), затем mul на
w_pix для каждого varying (6 mul). Total ~60 ops/pixel + FS.

**После:** edge functions ЛИНЕЙНЫ по (px, py). Вычисляем `step_x = -(y2-y1)`,
`step_y = (x2-x1)` один раз на треугольник. Стартовое значение в углу
bounding box, далее `+= step_x` на пиксель / `+= step_y` на строку.
То же самое для Z, interp_inv_w и numerator'ов варингов (они все
линейные комбинации w0/w1/w2). Total: ~15 ops/pixel + FS.

**Early-Z:** Z-тест поднят перед расчётом w_pix (1 div) и varying recovery
(6 mul). Occlude'нутый пиксель теперь стоит примерно столько же, сколько
visible-out-of-bbox.

Псевдокод:
```c
// per triangle:
w0_step_x = -(v2.y - v1.y);  // и аналогично w1, w2, z, iw, vn[i]
w0_row    = edge_fn(...) at (minx+0.5, miny+0.5);

for (py ...) {
    w0 = w0_row; ...
    for (px ...) {
        if (inside && z in [0,1]) {
            if (zpass && iw > eps) {
                w_pix = 1.0 / iw;
                for (i ...) varying_at_pixel[i] = vn[i] * w_pix;
                fs(...);
                blend, write pixel + zbuffer;
            }
        }
        w0 += w0_step_x; ...  // 11 add per pixel for 6 varyings
    }
    w0_row += w0_step_y; ...
}
```

**Правка №3: GL_NEAREST + микро-оптимизации FS**

`kernel/arch/x86_64/app_glcube.c:ensure_context`:
```c
st->texture.min_filter = GL_NEAREST;   // было GL_LINEAR
st->texture.mag_filter = GL_NEAREST;   // было GL_LINEAR
```

`gx_sample_texture` имеет fast-path для GL_NEAREST: 1 lookup, 0 mul vs
4 lookup + 16 mul для LINEAR. Visual diff на Lambert-затемнённой
64×64 шахматной текстуре куба не виден.

`kernel/arch/x86_64/app_glcube.c:cube_fs`:
- `float fog = (10.0f - varying[5]) * 0.2f;` (было `/ 5.0f` — 1 div/pixel)
- Убран промежуточный `gx_vec3_t n = { varying[0], varying[1], varying[2] };`
  (лишний struct-copy → GCC сам инлайнит, но синтаксически чище).

### 22.5 Сборка & верификация

```bash
make ARCH=x86_64                  # full build, ~12 сек
# warnings только pre-existing (misleading indentation в gles.c:354 и
# app_glcube.c:132 — двойные if на одной строке, безобидно)

# Headless smoke-тест UEFI:
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd \
    -m 1G -smp cpus=4,cores=4,threads=1 \
    -vga none -device virtio-vga,xres=1920,yres=1080 \
    -serial file:/tmp/log -display none ...
# В логе:
[virtio_gpu] display 1920x1080
[smp] cores up: 4/4
[doublebuf] initialized: 1920x1080 (8100 KB)
[drm] active: resource_id=1, 1920x1080 (backing ..., 8294400 B)
[compositor] init OK: 1920x1080 direct-doublebuf + fast shadow + AA
```

Регрессионные тесты:
- `bash scripts/test_stage45_amply.sh` → **24/25 PASS** (последний fail —
  отсутствует python3 в окружении, не относится к коду).
- `bash scripts/test_stage42_compositor.sh` → **15/16 PASS** (один pre-
  existing fail — устаревший grep на «triple-buf» message, который был
  выпилен HOTFIX 15.05.2026 ещё до моих правок).

### 22.6 Уроки и подводные камни

1. **WM с idle FPS не подходит для анимированных окон без явного сигнала.**
   После HOTFIX 19.05.2026 любой класс приложений, которые двигают что-то
   САМОСТОЯТЕЛЬНО (по таймеру, по WebSocket'у, по DMA-completion event),
   попадал в 10 FPS cap. Симптом был не очевиден: куб вроде вращался, но
   рваный, а движение мыши «магически» его оживляло. Решение — флаг
   `wants_continuous_redraw`. Для будущих апп-типов (видеоплеер, игры,
   live-чарты) — этот же флаг.

2. **Инкрементальные edge-функции — классическая оптимизация Pineda 1988.**
   Я знал про них и должен был сразу применить в этапе 44, но тогда меня
   увлекла математика на SSE. Теперь учтено. Тот же приём годится для
   любой линейной по (px,py) величины: Z, interp_inv_w, varying numerators.

3. **GL_NEAREST не равно GL_LINEAR по качеству — но на сложных шейдерах
   разница часто маскируется.** Lambert-затемнение + fog + 64×64 текстура
   = пиксельный паттерн почти не отличается от bilinear. Для приложений,
   где это критично (gradient-текстуры, 2D-спрайты), оставлять LINEAR.
   Для нашего демо-куба — экономия 4×.

4. **`xres=/yres=` для virtio-gpu в QEMU — нюанс.** Без них virtio-gpu по
   умолчанию рапортует 1280×800 через GET_DISPLAY_INFO. Наш kernel
   всё равно работает (берёт размер из fb_state), но в serial-логе
   видна несогласованность. Лучше явно задавать.

5. **Memory math для FullHD:** doublebuf 1920×1080×4 = 8.1 MB,
   compositor больше НЕ компосит во вспомогательный буфер (HOTFIX
   15.05.2026), GPU resource такого же размера. Heap 128 MB справляется
   с запасом.

6. **При смене резолюции ВСЕГДА проверять, что после fb_init() сразу
   доходит до compositor_init() в serial-логе.** Если где-то по дороге
   валится OOM (например, 4K = 32 MB doublebuf), его легко
   диагностировать по последней строке.

### 22.7 Связь с предыдущими этапами

- **Этап 37 (Long Mode)** — fb_init/doublebuf уже считывали размер
  динамически из MB2 tag. Поэтому смена резолюции была one-liner.
- **Этап 41 (virtio-gpu)** — GET_DISPLAY_INFO путь уже работал, добавил
  xres/yres в QEMU args для согласованности.
- **Этап 42 (compositor)** — direct-doublebuf-рендер с HOTFIX 15.05.2026
  не аллоцирует extra-буфер, FullHD прошёл без изменений.
- **Этап 44 (3D-стек)** — растеризатор оптимизирован, FPU/SSE из
  этого этапа активно используется (incremental math требует SSE-add).
- **HOTFIX 19.05.2026 (perf)** — idle 10 FPS остаётся, но теперь
  обходится через wants_continuous_redraw для активных приложений.

**Файлы, изменённые в hotfix'е:**

| Файл | Изменение |
|------|-----------|
| `kernel/arch/x86_64/boot.asm` | MB2 framebuffer tag: 1024×768 → 1920×1080 |
| `uefi/main.c` | `query_gop` target: 1024×768 → 1920×1080 |
| `kernel/arch/x86_64/window.h` | `+ bool wants_continuous_redraw;` |
| `kernel/arch/x86_64/window.c` | init `wants_continuous_redraw = false` |
| `kernel/arch/x86_64/wm.c` | сканирование окон с `wants_continuous_redraw` |
| `kernel/arch/x86_64/gles.c` | переписана `rasterize_triangle` — инкрементальные edges/Z/iw/varyings + early-Z |
| `kernel/arch/x86_64/app_glcube.c` | `win->wants_continuous_redraw = true`, GL_NEAREST, мелкие FS-правки |
| `Makefile.x86_64` | QEMU args: `virtio-vga,xres=1920,yres=1080`, `vgamem_mb=16` |
| `CLAUDE.md` | Этот раздел (§22) + ссылка из шапки |


---

## 23. Этап 47 — Криптография + TLS 1.2 (HTTPS работает), ✅ ЗАВЕРШЁН

**Дата:** 21.05.2026.  Самый большой подэтапный кусок фазы 3 на текущий
момент: ~5500 строк нового кода в kernel (3500 крипты + 740 TLS + 600 X.509 +
500 HTTPS/listen/shell) + 2 KB DER blobs для trust store (автогенерируются).

Реализован полный путь от математических примитивов до **живого HTTPS GET**
против реального OpenSSL-сервера.  Этап разделён на 5 чётких подэтапов
(47.1 .. 47.5), каждый собирается и тестируется независимо.

### 23.1 Подэтап 47.1 — криптографические примитивы

Файлы:
- `kernel/arch/x86_64/crypto.h` — единый агрегирующий header (типы контекстов +
  публичный API всех криптофункций).
- `kernel/arch/x86_64/crypto_cpu.c` — CPUID feature detect (`crypto_cpu()->aesni`,
  `pclmulqdq`, `rdrand`, `rdseed`, `sha_ext`).  `crypto_wipe()` через volatile
  pointer (чтобы compiler не optimize'нул memset(0)).  `crypto_eq_const()` —
  constant-time memcmp (XOR-аккумулятор без раннего exit).
- `crypto_sha.c` — SHA-256/384/512 по FIPS 180-4.  SHA-384 = SHA-512 с другим
  IV и truncation (переиспользует ту же compress-функцию).
- `crypto_hmac.c` — HMAC по RFC 2104, HKDF Extract+Expand по RFC 5869,
  TLS 1.2 PRF по RFC 5246 §5 (`P_SHA256` с label || seed).
- `crypto_aes.c` — AES-128/256.  Два пути:
  - **Software** (FIPS-197 reference): полный SubBytes/ShiftRows/MixColumns
    без T-tables — компромисс размер vs скорость (для kernel без AES-NI важнее
    маленький ROM-footprint, и константность по ключу — нет timing
    зависимостей от key-bits).
  - **AES-NI** (inline asm `aesenc`/`aesenclast`/`aesdec`/`aesdeclast` +
    `aeskeygenassist` для key expansion).  Выбор делает `crypto_aes128_init_*` /
    `crypto_aes256_init_*` runtime через `crypto_cpu()->aesni`.
  - Режимы: ECB (через `crypto_aes_encrypt_block`/`decrypt_block`),
    CBC (с chaining IV), GCM (с software bit-by-bit GF(2^128) GHASH).
- `crypto_chacha.c` — ChaCha20 stream cipher (RFC 8439 §2.3.2 quarter-round),
  Poly1305 MAC (5 × 26-bit limbs, как в RFC 7539 reference), AEAD
  `chacha20_poly1305_seal`/`open` (RFC 8439 §2.8: poly_key = first 32 bytes of
  ChaCha20-block(counter=0), encrypt с counter=1, tag = Poly1305 от
  aad ‖ pad16 ‖ ct ‖ pad16 ‖ aad_len_le64 ‖ ct_len_le64).
- `crypto_rand.c` — Fortuna-style entropy pool:
  - Состояние = 32-byte pool (SHA-256 хеш всех инкорпорированных entropy
    snapshot'ов) + stream_key (32 байта) + stream_nonce (12 байт) + counter.
  - Источники: RDRAND, RDSEED (если есть, retry до 10 раз — оба могут не
    выдать значение под нагрузкой), RDTSC, `pit_ticks`, `timer_get_uptime_ms`,
    адрес стека (ASLR-like).
  - Reseed каждые 64 KB вывода: pool = SHA-256(pool || new_entropy), затем
    derive stream_key из SHA-256(pool || "key") и nonce из SHA-256(pool || "nonce").
  - `crypto_rand_init()` делает 4 reseed'а на старте (чтобы инициальный
    pool из нулей подмешался достаточным entropy).
- `crypto_self_test.c` — 15 KAT-векторов:
  - SHA-256 "abc" → FIPS 180-2 §B.2 expected
  - SHA-384/512 "abc" → FIPS 180-2 §C.1/D.1
  - HMAC-SHA256 RFC 4231 TC1 (key=0x0b*20, msg="Hi There")
  - HMAC-SHA384 RFC 4231 TC2 (key="Jefe", msg="what do ya want for nothing?")
  - TLS 1.2 PRF-SHA256 (vector, который ловит multi-update bugs)
  - HKDF-SHA256 RFC 5869 §A.1 (PRK + OKM 42 bytes)
  - AES-128 ECB FIPS-197 App. B
  - AES-256 ECB NIST CAVS Encrypt-AES256.txt #0
  - AES-128 CBC NIST SP 800-38A F.2.1
  - AES-128 GCM McGrew & Viega Test Case 3 (64-byte plaintext)
  - ChaCha20 RFC 8439 §2.4.2 (114-byte sunscreen test vector)
  - Poly1305 RFC 8439 §2.5.2
  - AEAD ChaCha20-Poly1305 RFC 8439 §2.8.2
  - PRNG smoke (не нули и не равны при двух подряд запросах)

**Критический bug найден в KAT** (`crypto_sha256_update` и `crypto_sha512_update`):
после `if (buf_len > 0)` блока финальная строка `ctx->buf_len = len` **обнуляла**
buf_len даже когда данные уже legitimно лежат в буфере (case `len == 0`
после полного first-pass'а).  Триггерилось только при последовательности из
≥2 update'ов.  `crypto_hmac_sha256_buf` работал (один update), а HKDF/TLS-PRF
ломались.  Fix — accumulate вместо assign:
```c
for (size_t i = 0; i < len; i++) ctx->buf[ctx->buf_len + i] = p[i];
ctx->buf_len += len;
```
Этот bug — характерный пример того, что KAT на "abc" недостаточно: нужны
multi-update test vectors (HMAC-buf не triggerit'ит, а HKDF/PRF — да).

### 23.2 Подэтап 47.2 — public-key crypto

Файлы:
- `crypto_bn.c` — generic 4096-bit bignum (128 × 32-bit little-endian limbs).
  Operations: `bn_add`/`sub` (с carry/borrow), `bn_mul_full` (школьное
  умножение в 2N-limb результат), `bn_mod_2N` (bit-by-bit restoration division
  для модульной редукции 2N-limb dividend mod N-limb modulus), `bn_modexp`
  (MSB-first square-and-multiply, для public exponent uint32 — достаточно для
  RSA с e=65537).
- `crypto_rsa.c` — RSA-PKCS#1 v1.5 verify (RFC 8017 §8.2.2).  Hardcoded
  DigestInfo OID-префиксы для SHA-256/384/512 (как делает mbedTLS/BearSSL —
  парсить ASN.1 SEQUENCE { algId, OCTET STRING(hash) } внутри для каждого
  hash'а избыточно, prefix всегда фиксированный).
- `crypto_curve.c` — Curve25519 в **ref10-style** (от Bernstein/tweetnacl):
  16 × int64_t saturated limbs для GF(2^255-19) field arithmetic.
  X25519 ECDH (RFC 7748 §5) через Montgomery ladder с clamp'ингом scalar'а.
  Ed25519 verify (RFC 8032 §5.1.7) на twisted Edwards: SHA-512(R || pub || msg)
  → reduce mod L → scalarmult в q (negated pub) → add_ed с scalarbase(s) →
  pack → compare с signature R.

**KAT-векторы:**
- bignum: `3^7 mod 11 = 9`, `65537^65537 mod 1000000007 = 933755776` (точное
  значение получено через host-utility `pow(b,e,m)` в C на uint128).
- RSA: реальный 2048-bit ключ + signature от `openssl dgst -sha256 -sign`
  (генерится один раз и hardcoded в KAT).
- X25519: RFC 7748 §5.2 Test Vector 1.
- Ed25519: RFC 8032 §7.1 Test 2 + **negative case** (модифицированная
  подпись отклоняется).

**Критический bug при scaling 2048→4096 bit**: после изменения CRYPTO_BN_LIMBS
с 64 на 128 в `crypto.h`, `make` пересобрал только зависимые файлы по mtime
но `crypto_bn.o` остался скомпилированный с N=64.  В результате
`crypto_self_test.o` (N=128) вызывал `crypto_bn_zero` (N=64 in .o), которая
обнуляла только младшую половину массива — мусор в верхних limbs.  Симптом
был трудно-locatable: `out.v[124]` non-zero после `3^7 mod 11`.  Fix:
`rm build/x86_64/kernel/arch/x86_64/crypto*.o && make`.  Lesson: после
изменения header'а с #define который влияет на размер struct'ов — clean build.

### 23.3 Подэтап 47.3 — ASN.1 + X.509 + trust store

Файлы:
- `crypto_asn1.{h,c}` — DER parser.  Поддерживается: 1-byte tags (без
  multi-byte tag), short и long form length (до 4 байт), helpers для INTEGER
  (uint32 + bytes), OID (bytewise compare), UTCTime/GeneralizedTime →
  Unix time (Howard Hinnant civil → days алгоритм).
- `crypto_x509.{h,c}` — X.509 v3 parser по RFC 5280 §4.1.
  Поддерживается: TBS extraction (raw bytes для подписи), version (default v1
  + explicit [0]), serial, AlgorithmIdentifier OID мап на `x509_sig_alg_t`,
  Issuer/Subject DN (raw bytes для chain matching), validity (notBefore/After
  → unix), SubjectPublicKeyInfo для RSA (modulus + exponent) и Ed25519
  (32-byte public key), extensions: SAN (dNSName), BasicConstraints (CA=TRUE).
  `x509_verify_signature(cert, issuer)` — выбор алгоритма по `sig_alg`,
  hash TBS, передача в `crypto_rsa_pkcs1_verify` или `crypto_ed25519_verify`.
  `x509_hostname_matches` — для leaf cert'ов с wildcard `*.example.com`.
- `crypto_ca_blob.c` — **автогенерируется** скриптом `scripts/gen_ca_blob.sh`
  из системного `/etc/ssl/certs/`:
  - ISRG Root X1 (1391 байт DER, RSA-4096) — Let's Encrypt root.
  - DigiCert Global Root G2 (914 байт DER, RSA-2048, SHA-256).
  Не commit'им сами байты в репо — генерим во время сборки.  Стандартная
  практика TLS-libraries: mbedTLS, BearSSL делают так же.
- `crypto_ca.c` — `crypto_ca_find_by_dn(issuer_dn, ...)` — линейный поиск
  по `g_crypto_ca_store[]`, парсит DER каждого entry и сравнивает Subject
  DN bytewise с искомым issuer DN.

**Bug при scaling**: ISRG Root X1 — RSA-4096 (modulus 512 байт).  Изначально
`CRYPTO_BN_LIMBS=64` не помещалось (2048 bit), `crypto_rsa_pkcs1_verify`
сразу возвращал false на `n_len > CRYPTO_BN_BYTES`.  Поэтому переделал
CRYPTO_BN_LIMBS на 128 (4096 bit).  Это удваивает время modexp (~50 ms на
TCG для one verify), но критично для root CA с большими ключами.

**DigiCert Global Root CA исключён**: использует SHA-1 (deprecated, не
поддерживаем).  Заменили на DigiCert Global Root G2 (SHA-256).

**Live verification** (как часть smoke 47.3 в boot path):
```
[smoke 47.3] CA store size: 2
[smoke 47.3] parsed=2 self-signed-verified=2 (OK)
```
Оба root'а парсятся и проходят self-signature verify (внутренний — root
подписан сам собой).

### 23.4 Подэтап 47.4 — TLS 1.2 client

Файлы:
- `tls.h` — `tls_ctx_t` (large struct, ~50 KB: rx_buf 16 KB + cert raw
  16 KB + GCM contexts + handshake hash + master_secret + key_block).
  API: `tls_client_init` / `tls_client_handshake` / `tls_send` / `tls_recv` /
  `tls_close`.
- `tls.c` — реализация (~620 строк).  Структура:
  1. **Record layer** (`send_record_internal` / `recv_record_internal`):
     5-byte header (type + version + length), payload — либо cleartext
     (handshake до CCS), либо GCM-encrypted (после CCS).  GCM nonce =
     implicit_iv[4] || explicit_nonce[8] (используем seq как explicit).
     AAD = seq(8) || type(1) || version(2) || plaintext_length(2).
  2. **Handshake messages** (1-byte type + 3-byte length + body):
     - `send_client_hello`: TLS_VERSION_1_2, 32 random bytes, no session_id,
       cipher_suites = [0x009C], compression = [0x00], extensions:
       - SNI (server_name): host_name тип + длина + hostname bytes.
       - signature_algorithms: rsa_pkcs1_sha256 (0x0401).
     - `parse_server_hello`: verify version 1.2, copy server_random, capture
       chosen cipher_suite (должен совпасть с 0x009C), reject compression.
     - `parse_certificate`: первый cert в chain — leaf.  Парсим через
       `x509_parse`.  Chain validation:
       1. Прямой root match: issuer leaf'а в trust store → verify.
       2. Если нет: intermediate в chain → verify leaf под intermediate →
          verify intermediate под root в trust store.
       Best-effort: если не trusted, печатает warning, но handshake идёт.
     - `send_cke_ccs_finished`:
       - Premaster = 0x03 0x03 || 46 random bytes.
       - RSA-encrypt под server pub key (PKCS#1 v1.5 type 2 padding).
       - ClientKeyExchange = uint16(enc_len) || enc[enc_len].
       - **derive_keys**: master_secret = PRF(premaster, "master secret",
         cr || sr)[0..48]. key_block = PRF(ms, "key expansion",
         sr || cr)[0..40] = {client_key(16), server_key(16),
         client_iv(4), server_iv(4)}.  GCM contexts pre-init с keys.
       - ChangeCipherSpec record (1 byte: 0x01).  CCS НЕ участвует в
         handshake_hash и НЕ под GCM — но сразу после flip'ит `write_encrypted`
         и сбрасывает `client_seq` в 0.
       - **Finished** verify_data = PRF(ms, "client finished",
         SHA256(handshake_messages))[0..12].  Snapshot copy SHA-context
         (не финализировать оригинал — он понадобится для Server Finished).
     - `wait_server_ccs_finished`:
       - Receive CCS (cleartext).  Flip `read_encrypted` + reset `server_seq`.
       - Receive Server Finished (encrypted record → decrypted handshake msg).
       - Compute expected verify_data = PRF(ms, "server finished",
         SHA256(messages-including-client-finished)).
       - Constant-time compare.
  3. **Application data**: `tls_send` рубит на 16 KB record'ы.
     `tls_recv` декодирует один record, может вернуть из буфера остаток
     если caller'у нужно меньше чем в record'е.
  4. **close_notify**: warning level + close_notify desc (0x01 0x00) в
     ALERT record при `tls_close()`.  Wipe keys.

### 23.5 Подэтап 47.5 — HTTPS + browser + TCP listen/accept

Файлы:
- `https.{h,c}` — обёртка над DNS + TCP + TLS.  `https_get(url, body, status,
  require_trust)`.  Парсит URL через `http_parse_url`, fix'ит port на 443
  если в URL `https://` без `:port` (http_parse_url ставит 80 by default).
- `tcp.c` — расширение для server-side.  Новые состояния
  `TCP_STATE_LISTEN` и `TCP_STATE_SYN_RCVD`.  `tcp_listen(port)` сбрасывает
  g_sock в LISTEN.  В `tcp_rx`: при LISTEN + incoming SYN → захват peer,
  send SYN-ACK, переход в SYN_RCVD.  При SYN_RCVD + ACK → ESTABLISHED.
  `tcp_accept(peer_ip, peer_port, timeout_ms)` блокирует через `sti; hlt`
  пока не ESTABLISHED.
- `sock.c` — `sock_sys_listen` теперь вызывает `tcp_listen`, `sock_sys_accept`
  → `tcp_accept`.  Возвращает тот же fd (single-connection семантика).
- `shell.c` — команда `https <url>` (и alias `https-test <url>`):
  выполняет `https_get`, печатает status code и первые 256 байт body
  (printable chars + '\n'/'\r' остаются, остальное → '.').
- `apps.c` — browser_fetch теперь смотрит на префикс URL: `https://` →
  `https_get`, иначе → `http_get`.  Простая правка на 7 строк.

**Smoke в boot path:**
```
[smoke 47.5] tcp_listen=ok state=LISTEN=yes (OK)
[smoke 47.5 live] connecting to 10.0.2.2:4443...
```
Если на host'е WSL запущен openssl s_server — handshake выполняется
автоматически и пишет `[tls] handshake complete: TLS_RSA_WITH_AES_128_GCM_SHA256`.
Если нет — `TCP connect failed (no server)` — не критическая ошибка
(headless test'у это OK).

### 23.6 Live end-to-end тест

`scripts/test_stage47_live.sh`:
1. Генерирует self-signed RSA-2048 cert для CN=10.0.2.2 (`openssl req -x509`).
2. Запускает `openssl s_server -accept 0.0.0.0:4443 -tls1_2
   -cipher AES128-GCM-SHA256 -no_ticket -www` в background.
3. Probe через bash `/dev/tcp/127.0.0.1/4443` — ждёт до 10 попыток что
   server listens.
4. Запускает QEMU с `-netdev user` (host доступен из guest как 10.0.2.2).
5. Kernel в smoke 47.5 live автоматически TCP connect к 10.0.2.2:4443 +
   TLS handshake.
6. Скрипт парсит serial log на `[tls] handshake complete`.
7. Kill openssl s_server.

**Результат:** 3/3 PASS.  Реальный handshake против OpenSSL — все наши
криптографические + протокольные слои работают совместно с production-grade
TLS implementation.

### 23.7 Уроки и подводные камни

1. **Multi-update test vectors обязательны для streaming hash'ей.**  Single
   update KAT (как SHA-256 "abc") не ловит accumulator-bug.  Нужно хотя бы
   один test с 2+ update'ами разной длины — HKDF и TLS-PRF дают это естественно.
2. **Clean build после изменения header'а с struct size #define.**  Если меняешь
   CRYPTO_BN_LIMBS / TLS_MAX_RECORD / любой другой #define от которого зависит
   sizeof — `rm -rf build/x86_64/.../crypto*.o` или `make clean`.  Mtime-based
   rebuild не отслеживает include цепочки правильно для всех файлов.
3. **PKCS#1 v1.5 type 2 padding** (encryption mode) ≠ **type 1** (signature mode).
   В первом — PS = random non-zero bytes, во втором — PS = 0xFF*N.  Я случайно
   написал type 1 для CKE сначала; signature_verify прошёл, но handshake
   ломался (server не мог decrypt premaster).  Проверка: первый non-padding
   байт = 0x02 для encryption, 0x01 для signature.
4. **Mozilla NSS root CAs — публичный domain.**  Это не creative work, а
   технические артефакты (public keys + signatures + factual metadata).
   Распространяются Mozilla под MPL 2.0, Debian'ом в пакете ca-certificates.
   Каждый TLS-стек в мире встраивает их.  Best practice — не commit'ить байты
   в репо, а извлекать из системного store во время build'а (наш
   `gen_ca_blob.sh`).
5. **`openssl s_server` для testing RSA-only cipher suite требует явный
   `-cipher AES128-GCM-SHA256` + `-tls1_2`.**  Без этого OpenSSL по умолчанию
   negotiate ECDHE.  Это полезно знать когда test setup'ишь — если забудешь
   `-cipher`, handshake fail'ит на "no cipher in common" с непонятным error.
6. **QEMU -netdev user route**: host доступен из guest как 10.0.2.2.  ARP
   request для 10.0.2.2 возвращает виртуальный MAC от QEMU NAT.  Если ARP
   reply не приходит — обычно сервер на host'е ещё не bound (race с QEMU
   start).  Решение: probe через bash `/dev/tcp/127.0.0.1/<port>` в loop.
7. **Static buffer в TLS-context'е (~50 KB) — много для kernel stack.**
   Поэтому в `https_get` использую `static tls_ctx_t tls;` — глобальный
   buffer, не stack.  Это OK для single-connection (один HTTPS request
   одновременно), как и tcp.c.

### 23.8 Связь с предыдущими этапами

- **Этап 40 (libc finalize)** — sock.c get'ает рабочие `sock_sys_listen` /
  `sock_sys_accept` (раньше были ENOSYS).  POSIX-tier приложений теперь
  могут писать TCP-сервера.
- **Этап 41 (virtio-gpu)** — никакого пересечения, но boot после crypto_init
  длиннее на ~0.3 сек (15 KAT-векторов вычисляются).
- **Этап 42 (compositing)** — Browser app получает render через тот же
  compositor.  HTTPS-ответ рисуется через `app_browser` без отдельной обработки.
- **Этап 44 (FPU+SSE)** — AES-NI и crypto в kernel используют xmm-регистры.
  FXSAVE/FXRSTOR в task_yield / isr_common / syscall_entry (введённые в 44)
  гарантируют, что состояние xmm не повредится прерыванием во время handshake.
- **Этап 45 (Amply)** — `crypto_rand_init` использует RDRAND/RDSEED того же
  CPU, что и `users_hash` (4096-round FNV-1a) — но это разные uses.  В будущем
  можно мигрировать Amply signature на Ed25519 (готов в 47.2), что бы
  улучшить криптостойкость пакетов.  Сейчас Amply по-прежнему использует
  двойной FNV-1a.
- **Этап 46 (sandboxes)** — crypto работает в kernel-mode, syscall'ов на крипту
  пока нет.  Userspace TLS будет в одном из будущих этапов (потребует exposed
  syscalls).  Sandbox-policies для крипты — нерелевантно сейчас.

### 23.9 Файлы, изменённые в этапе 47

| Файл | Назначение | Строк |
|------|------------|-------|
| `kernel/arch/x86_64/crypto.h` | Агрегирующий header | ~330 |
| `kernel/arch/x86_64/crypto_cpu.c` | CPUID detect + utils | ~100 |
| `kernel/arch/x86_64/crypto_sha.c` | SHA-256/384/512 | ~280 |
| `kernel/arch/x86_64/crypto_hmac.c` | HMAC/HKDF/PRF | ~230 |
| `kernel/arch/x86_64/crypto_aes.c` | AES + AES-NI + CBC + GCM | ~550 |
| `kernel/arch/x86_64/crypto_chacha.c` | ChaCha20/Poly1305/AEAD | ~330 |
| `kernel/arch/x86_64/crypto_rand.c` | Fortuna PRNG | ~180 |
| `kernel/arch/x86_64/crypto_self_test.c` | NIST/RFC KAT-векторы | ~500 |
| `kernel/arch/x86_64/crypto_bn.c` | 4096-bit bignum | ~160 |
| `kernel/arch/x86_64/crypto_rsa.c` | RSA-PKCS#1 v1.5 verify | ~95 |
| `kernel/arch/x86_64/crypto_curve.c` | X25519 + Ed25519 | ~350 |
| `kernel/arch/x86_64/crypto_asn1.{c,h}` | DER parser | ~220 |
| `kernel/arch/x86_64/crypto_x509.{c,h}` | X.509 v3 + verify | ~470 |
| `kernel/arch/x86_64/crypto_ca.{c,h}` | Trust store lookup | ~60 |
| `kernel/arch/x86_64/crypto_ca_blob.c` | DER blobs (автоген) | ~220 |
| `scripts/gen_ca_blob.sh` | Генератор CA blob из /etc/ssl/certs | ~60 |
| `kernel/arch/x86_64/tls.{c,h}` | TLS 1.2 client | ~740 |
| `kernel/arch/x86_64/https.{c,h}` | HTTPS GET | ~150 |
| `kernel/arch/x86_64/tcp.{c,h}` | listen/accept (+state machine) | +60 |
| `kernel/arch/x86_64/sock.c` | listen/accept syscalls (no ENOSYS) | +30 |
| `kernel/arch/x86_64/shell.c` | `https <url>` command | +60 |
| `kernel/arch/x86_64/apps.c` | Browser https:// support | +20 |
| `kernel/kernel64_main.c` | smoke 47.1..47.5 + LIVE | +80 |
| `Makefile.x86_64` | + 13 .c в C_SRC | +14 |
| `scripts/test_stage47_crypto.sh` | Static автотест 58/58 PASS | ~140 |
| `scripts/test_stage47_live.sh` | E2E live test 3/3 PASS | ~80 |
| `CLAUDE.md` | Этот раздел (§23) + шапка + §15 + §17 | (этот файл) |
| **Итого нового кода** | | **~5500 строк** |

Следующий этап по roadmap'у — **48 (FDE — шифрование диска)**.  Crypto stack
из 47 готов полностью использоваться (AES-256-XTS, PBKDF2 от HMAC-SHA256 для
LUKS-header).


---

## 24. Этап 48.1 — полный выход в интернет из встроенного браузера

Цель этапа: из GUI-браузера AMPLIFY-OS открывать реальные сайты (http и
https), видеть читаемый текст вместо сырого HTML и ходить по ссылкам мышкой.

### 24.1 Что мешало до этапа 48.1

| Проблема | Следствие |
|----------|-----------|
| DNS: один сервер, один запрос, без кэша | Любой потерянный UDP-пакет = «сайт не открывается» |
| TCP: RX-буфер без кольца, окно фиксированное, нет ретрансмита | Обрыв на страницах >64 KB, зависание при потере SYN |
| HTTP: HTTP/1.0, нет chunked, нет редиректов | Часть сайтов отвечала 400/пустотой или отдавала мусор с размерами chunk'ов |
| Нет перехода http <-> https в редиректах | `http://сайт` -> 301 на https и тупик |
| Браузер рисовал сырой HTML | Страницу невозможно читать, ссылки не работают |
| UTF-8 сайты в CP1251-шрифте | Русские сайты — кракозябры |

### 24.2 DNS (`dns.c`, `dns.h`)

- Список серверов-кандидатов с дедупликацией: DHCP #1 -> DHCP #2 -> gateway
  -> 8.8.8.8 -> 1.1.1.1 -> 8.8.4.4 (`DNS_MAX_SERVERS 6`).
- `DNS_TRIES_PER_SRV 2` попытки на сервер, `DNS_TRY_TIMEOUT_MS 1500` на попытку:
  провайдерский резолвер может молчать — мы автоматически уходим на публичные.
- Кэш на 32 имени с TTL (clamp 30..3600 с), `dns_cache_flush()`.
- Случайные source-порт (20000..60000) и query ID от `rdtsc` — защита от
  чужих/устаревших ответов и от NAT-коллизий.
- Корректный проход по answer-секции с пропуском CNAME (раньше сайты за
  CDN резолвились в мусор), обработка NXDOMAIN, IPv4-литералы без запроса.

### 24.3 TCP (`tcp.c`)

- **Ring-буфер RX 64 KB** (`rx_head/rx_tail/rx_count`) вместо линейного:
  большие страницы больше не теряются при чтении по частям.
- Окно анонсируется из реального свободного места (`rx_window()`), после
  вычитывания отправляется window-update ACK.
- **MSS 1460** в SYN (data offset 6) — серверы больше не режут трафик до 536.
- Случайные ephemeral-порт (49152+) и ISN.
- Ретрансмит: SYN — 3 попытки каждые 1.2 с (`TCP_CONNECT_TIMEOUT_MS 6000`),
  данные — 3 попытки по 600 мс без ACK.
- Обработка дубликатов/перекрытий сегментов и dup-ACK на дырку (fast
  retransmit со стороны сервера).

### 24.4 HTTP (`http.c`, `http.h`)

- `http_parse_url()` знает схемы (80/443), вырезает `#fragment`, валидирует порт.
- Запрос HTTP/1.1 с `Host`, `User-Agent`, `Accept`, `Accept-Language`,
  `Accept-Encoding: identity`, `Connection: close`.
- `http_get_ex()` отдаёт заголовки; `http_get_follow()` — редиректы.
- Новые утилиты (используются и в https.c, и в браузере):
  `http_find_header`, `http_body_is_chunked`, `http_dechunk`, `http_resolve_url`,
  `http_url_is_https`.
- Буфер ответа 128 KB, таймауты: idle 8 с, общий 25 с, ранний выход по
  `Content-Length`.

### 24.5 HTTPS и `net_fetch()` (`https.c`, `https.h`)

- Тот же HTTP/1.1-запрос и chunked-декодер поверх TLS 1.2 из этапа 47.
- **`net_fetch(url, body, cap, &len, &code, final_url, cap, max_redirects,
  require_trust)`** — единая точка входа:
  - понимает `http://`, `https://` и адрес без схемы (`example.com/x`):
    сначала пробует https, при неудаче — http;
  - тянет цепочку редиректов до 10 шагов с переходами между схемами
    (http -> https -> www);
  - возвращает тело, код и финальный URL (нужен как base для ссылок).

### 24.6 Браузер (`apps.c`)

- Всё грузится через `net_fetch()`; схему можно не писать.
- **HTML -> текст**: выбрасываются `script/style/noscript/svg/iframe/canvas/
  template`, блочные теги дают абзацы, `li` — маркеры, сжатие пробелов,
  декод HTML-сущностей (`&amp;`, `&#1055;`, `&nbsp;`, `&mdash;`, ...),
  `<title>` в статус-строке.
- **UTF-8 -> CP1251** (с автоопределением charset): русские сайты читаемы
  штатным шрифтом 8x16.
- **Ссылки**: до 64 штук, `href` разрешается в абсолютный URL относительно
  финального URL страницы, подсветка + подчёркивание, клик мышкой — переход.
- История (8 шагов) и кнопка «Назад», переключатель «HTML» (исходный код),
  перенос по словам, прокрутка PgUp/PgDn с клампом и индикатором строки.
- Состояние браузера (~50 KB) и сырой ответ (160 KB) живут в BSS, а не в куче
  (`kmalloc` — простой bump-аллокатор); окно 760x520.

### 24.7 DHCP и инициализация

- `dhcp.c/.h`: из опции 6 теперь берётся **два** DNS-сервера,
  геттер `dhcp_get_dns2()`.
- `kernel64_main.c`: после `dns_init()` вызываются `dns_set_server()` и
  `dns_set_server2()`.

### 24.8 Как проверить

```bash
make ARCH=x86_64            # сборка (нужны nasm, grub-mkrescue, xorriso, mtools)
make run                    # QEMU user-net (slirp) — интернет есть сразу
sudo tools/setup_tap.sh setup nat
make run-tap                # TAP + NAT — настоящие пакеты через хост
```

В системе: логин `mark` / `test1234` -> Browser -> ввести адрес -> Enter.
Проверочные адреса: `example.com`, `http://neverssl.com/`,
`https://ru.wikipedia.org/wiki/Linux`, `https://lite.duckduckgo.com/lite/?q=notion`.
Серийный лог показывает всю цепочку: `[dns]`, `[tcp]`, `[https]`, `[net] redirect`.

### 24.9 Файлы, изменённые в этапе 48.1

| Файл | Что сделано |
|------|-------------|
| `kernel/arch/x86_64/dns.{c,h}` | Переписан: список серверов, ретраи, кэш, CNAME, случайный порт/ID |
| `kernel/arch/x86_64/tcp.c` | Ring RX 64 KB, окно, MSS, ретрансмит, дубликаты, случайные ISN/порт |
| `kernel/arch/x86_64/http.{c,h}` | HTTP/1.1, chunked, редиректы, URL-утилиты, 128 KB буфер |
| `kernel/arch/x86_64/https.{c,h}` | HTTP/1.1 поверх TLS, chunked, `net_fetch()` |
| `kernel/arch/x86_64/dhcp.{c,h}` | Второй DNS из опции 6 + `dhcp_get_dns2()` |
| `kernel/arch/x86_64/apps.c` | Новый браузер: HTML-рендер, ссылки, история, CP1251 |
| `kernel/kernel64_main.c` | `dns_set_server2(dhcp_get_dns2())` |
| `CLAUDE.md`, `PATCH_NOTES_network_fix.md` | Документация этапа |

### 24.10 Что ещё не умеет браузер (границы этапа)

1. **TLS 1.3 нет** — только TLS 1.2 (ECDHE-RSA/ECDSA + AES-GCM). Сайты,
   требующие только TLS 1.3, не откроются.
2. **Нет gzip/brotli** — шлём `Accept-Encoding: identity`; сервер, игнорирующий
   это, даст нечитаемый блоб.
3. **Нет CSS/JS/картинок** — только текст и ссылки; SPA-сайты на JS покажут
   пустоту. Лучше работают lite-версии сайтов.
4. **Одно TCP-соединение за раз** — загрузка последовательная, нет keep-alive.
5. **Только IPv4**, нет POST/форм и cookies.
6. При `require_trust=false` сертификат проверяется, но недоверенный не
   блокирует загрузку (удобно для тестов, но это не «безопасный режим»).


---

## 25. Этап 57 — установщик с ISO (GUI installer + Live ISO)

Цель: загрузиться с установочного ISO, пройти мастер по макету (WELCOME →
LICENSE → DESTINATION → OPTIONS → INSTALL → COMPLETE), выбрать диск и
настройки и получить на этом диске загружаемую через UEFI систему.

### 25.1 Как это устроено

**Носитель.** `build/myos-x86_64.iso` — гибридный образ:
* BIOS: GRUB (`boot/grub-x86_64.cfg`, первый пункт) → multiboot2 с cmdline
  `live installer`;
* UEFI: второй El Torito-образ `efiboot.img` = `build/live-esp.img` — FAT с
  нашим `BOOTX64.EFI`, `kernel.bin`, `initrd.img` и маркером `\live.flag`.
  Загрузчик (`uefi/main.c`) видит `live.flag` и сам синтезирует тэг cmdline
  `live installer`;
* USB: тот же образ добавлен MBR-разделом 0xEF (`-append_partition`), после
  `dd` на флешку UEFI найдёт загрузчик.
В WSL установлен только `grub-pc-bin`, поэтому EFI-ветку `grub-mkrescue` не
строит — xorriso-опции передаются ему явно (см. правило `$(ISO)`).

**Payload.** Установщику не нужен ISO9660: `kernel-x86_64.bin` и
`BOOTX64.EFI` лежат прямо в initrd (`INITRD_FILES`), а на ESP целевого диска
копируется сам образ initrd целиком (`initrd_image()`).

**Ядро.** `installer_init_from_mb2()` читает cmdline; при `live`/`installer`
`kernel_main64` перед firstboot/login вызывает `installer_run()`.

### 25.2 Модули

| Файл | Что делает |
|------|------------|
| `ata.{c,h}` | 4 слота IDE (primary/secondary × master/slave), LBA28/LBA48, ATAPI-детект. Ожидания по времени, а не по счётчику; FLUSH не после каждой команды |
| `disk_io.{c,h}` | `*_dev`-варианты с индексом устройства, `disk_zero_sectors_dev`, `disk_flush_dev` |
| `partition.{c,h}` | GPT: чтение (с откатом на MBR), план раскладки, запись protective MBR + основной/резервный заголовки с CRC32 |
| `fat32_mkfs.{c,h}` | mkfs.fat32 в ядре: BPB, FSInfo, backup boot, обе FAT, корень + запись-метка тома |
| `fat32.{c,h}` | привязка к тому `(dev, base_lba)`: `fat32_bind/unbind/get_volume`, поиск системного раздела по GPT в `fat32_mount`, `fat32_read_file_at` |
| `installer_engine.{c,h}` | план → GPT → mkfs ×2–3 → ESP (загрузчик, ядро, initrd) → системное дерево + `users.txt` + конфиги → верификация (побайтовая сверка ESP с payload'ом) → FLUSH |
| `installer.{c,h}` | 6 экранов + диалог «стереть диск?», мышь и клавиатура |
| `ns.{c,h}` | `ns_set_root_hostname()` — hostname из установщика |

Раскладка (`part_plan`): ESP 256 МиБ (512 МиБ на дисках ≥16 ГиБ) +
`AMPLIFY-SYSTEM` на остаток; с опцией Vault — система до половины места
(≤24 ГиБ), остальное `AMPLIFY-VAULT`. Минимальный диск — 2 ГиБ.

На системный раздел пишутся: `/users.txt` (формат `users.c`),
`/Sanctum/system.conf` (hostname, theme, vault, installed=1),
`/Sanctum/system/ui-config.ini` (формат приложения Facet: `theme=`, `dark=`),
`/Sanctum/install.stamp`, каталоги Sanctum/nexus/vault/forge/fonts.
`install_apply_system_config()` применяет hostname и тему на каждой
загрузке установленной системы (до splash и повторно после `desktop_init`,
который сбрасывает тему).

### 25.3 Запуск и тест

```bash
make ARCH=x86_64 iso                 # build/myos-x86_64.iso
make ARCH=x86_64 run-installer       # UEFI: ISO + пустой build/target-disk.qcow2 (4G)
make ARCH=x86_64 run-installed       # UEFI: только целевой диск
make ARCH=x86_64 run-installer-bios  # тот же ISO через GRUB
make ARCH=x86_64 installer-disk-reset
bash scripts/test_stage57_installer.sh   # headless e2e, скриншоты в build/screens-57/
```
`make run` по-прежнему грузит dev-ESP **без** `live.flag` — прямо в систему.
В `run-installer` целевой диск стоит в bootindex=0: после «RESTART NOW»
машина загружается уже с установленного диска.

Клавиши установщика: Enter — далее, Esc — назад (на WELCOME — выход в
live-сессию без установки), Space — чекбокс/переключатель, Tab/стрелки —
фокус на OPTIONS, Up/Down — выбор диска.

### 25.4 Баги, найденные при доводке (стоит помнить)

1. **`BOOTEFI` объявлялся после правила initrd** — prerequisite раскрылся
   пустым, и `BOOTX64.EFI` не попадал в payload (§14.13 в новом обличье).
2. **UEFI-загрузчик патчил `basic_meminfo` по жёсткому смещению `info+8`**,
   а тэг cmdline был вставлен перед ним → `mem_upper` затирал строку
   `live installer`, ядро не видело режим установщика. Теперь
   `basic_meminfo` всегда первый (комментарий в `build_mb2_info_partial`).
3. **FLUSH CACHE после каждой PIO-записи** + таймауты по числу итераций:
   тысячи fsync на хосте при форматировании и ложные таймауты под KVM.
4. **`fat_find_free_cluster` искал по всей длине FAT**, которая длиннее
   реального числа кластеров → кластер мог лечь за границу раздела.
5. **Экран входа реагировал на `r`/`x` даже в поле пароля** (reboot/shutdown
   посреди набора) — пароль из установщика с этими буквами был невводим.
6. `/tmp` в WSL — tmpfs и чистится при остановке дистрибутива; тест
   копирует логи в `build/screens-57/`.

### 25.5 Границы этапа

* Установленная система грузится **только через UEFI** (GPT + ESP); BIOS-
  загрузка с установленного диска не поддерживается (нет MBR-загрузчика).
* Диски — только IDE/ATA PIO (нет AHCI/NVMe/USB mass storage).
* Нет шифрования диска (этап 48), выбора языка и раскладки, ручной разметки.
* На 1024×768 интерфейс тесноват: шрифты не масштабируются сеткой, поэтому
  GRUB-конфиг просит 1920×1080 → 1600×900 → 1280×800.


---

## 26. Этап 48 — шифрование системного раздела (FDE), ✅ ЗАВЕРШЁН

**Дата:** 26.09.2026.  Примитивы (AES-XTS, PBKDF2-HMAC-SHA256 и их KAT)
были заложены ещё 14.09 вместе с черновиком `fde.h` под старую модель
«голый FAT32 на весь диск + заголовок в конце».  После этапа 57 (GPT) эта
модель устарела, поэтому FDE сделан заново — пораздельно, как dm-crypt.

### 26.1 Слои

```
fat32.c        /disk — обычный FAT32, о шифровании не знает
   │  сектора виртуального устройства (vdev 8..15)
blkcrypt.c     dm-crypt-аналог: aes-xts-plain64, IV = сектор внутри payload
   │  шифротекст
disk_io / ata  раздел AMPLIFY-SYSTEM (GPT-тип «Linux LUKS» CA7D7CCB-...)
luks.c         LUKS1-заголовок в начале раздела, payload с сектора 4096
```

`disk_io` теперь диспетчеризует номера устройств: 0..3 — ATA, 8..15 —
blkcrypt, 16 — RAM-диск для самотестов.  Поэтому `fat32_mkfs`, `fat32_bind`,
`partition` и установщик работают с зашифрованным томом без изменений.

### 26.2 Формат (luks.c) — совместим с cryptsetup

* phdr 1024 байта, big-endian: `LUKS\xBA\xBE`, v1, `aes`, `xts-plain64`,
  `sha256`, payload_offset 4096, key_bytes 64, mk_digest = PBKDF2(MK, salt,
  iter, 20).
* 8 слотов, материал слота — 500 секторов (64 × 4000 полос), смещения 8, 512,
  1016, ... (выравнивание 4 КиБ), как у `cryptsetup luksFormat --type luks1`.
* AF-splitter: diffuse = SHA-256 блоками по 32 байта с 4-байтовым BE-счётчиком
  перед данными.  Материал слота шифруется aes-xts-plain64 с IV от 0.
* Итерации PBKDF2 — бенчмарком на этой машине: слот ≈ 1 с (`FDE_ITER_MS`),
  digest ≈ 125 мс, минимум 1000.  PBKDF2 ускорен вдвое: состояния SHA-256
  после ipad/opad считаются один раз (`crypto_hmac.c`).
* Детали сверены с исходниками cryptsetup (`lib/luks1/{luks.h,af.c,keyencryption.c}`).

### 26.3 Загрузка

1. `fat32_mount()` — если на диске есть LUKS-раздел, ESP больше НЕ
   подставляется вместо системного (`part_find_system_fat32`).
2. `fde_scan()` сразу после него помечает том как запертый.
3. После keyboard/doublebuf/drm и ДО `ns_init`/`amply_init` —
   `fde_unlock_screen()` (`fde_unlock.c`): пароль → `luks_open` →
   `blkcrypt_map` → `fat32_mount_volume(vdev, 0)` → повторный `users_init()`.
   После третьей ошибки — растущая пауза (1, 2, 4 ... 30 с).
4. `[smoke 48]` — самотест на RAM-диске (13 проверок, ~250 мс).
5. Единый вход: `fde_take_sso()` + `login_try_passphrase()` — если пароль
   тома подходит к учётной записи, экран входа пропускается.

### 26.4 Установщик

Переключатель «Encrypt the drive» на экране OPTIONS (Tab/Space, мышь).
При шифровании пароль ≥ 8 символов, в диалоге подтверждения —
предупреждение о невосстановимости.  Шаг `INSTALL_STEP_ENCRYPT`:
`luks_format` → `blkcrypt_map` → mkfs/наполнение через vdev.  Verify
открывает том ПАРОЛЕМ заново (не мастер-ключом из памяти).  Vault-раздел
при шифровании получает свой LUKS1 с тем же паролем.  В `system.conf` —
`encryption=luks1-aes-xts-plain64`.

### 26.5 Shell `seal`

`seal status` (том, слоты, отображения, счётчики секторов), `seal selftest`,
`seal bench` (итераций PBKDF2/с), `seal passwd <old> <new>` (новый слот,
старый уничтожается; данные не перешифровываются).

### 26.6 Тесты

```bash
bash scripts/test_stage48_fde.sh     # 25/25: smoke 48, установка с шифрованием,
                                     # хост-проверки, загрузка с разблокировкой
```
Фаза 3 собирает `tools/luks_interop.c`: **libcryptsetup.so.12** (настоящий
код cryptsetup, заголовки libcryptsetup-dev не нужны — ABI объявлен вручную)
открывает заголовок паролем и отдаёт ключ тома, **OpenSSL** `aes-256-xts`
расшифровывает payload[0] в FAT32 BPB.  Криптографии ядра AMPLIFY-OS там
нет — успех означает совместимость формата.

### 26.7 Границы

* Шифруется системный раздел (и vault); ESP с ядром и initrd — нет.  Подмену
  ядра закрывает подпись (Secure Boot / этап 56), не FDE.
* Только LUKS1 с aes-xts-plain64/sha256/512 бит; LUKS2 и чужие параметры
  распознаются, но не открываются.
* Ключ тома живёт в RAM, пока система работает (как у dm-crypt).
* Шифрование уже установленной системы «на месте» не реализовано —
  только при установке.
* Пароль тома и пароль пользователя после `seal passwd` расходятся: единый
  вход тогда не срабатывает, и экран входа спросит пароль пользователя.

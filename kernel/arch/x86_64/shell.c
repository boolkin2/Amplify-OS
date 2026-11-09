// ============================================================================
// shell.c — interactive AMPLIFY-OS shell для x86_64.
// ----------------------------------------------------------------------------
// Сохраняет фирменную «мистическо-механическую» стилистику AMPLIFY-OS.
// На подэтапе 37.6c-1 поддержано 18 встроенных команд:
//
//   guide     — список команд
//   shine     — clear screen
//   voice T   — echo
//   scan      — список файлов VFS (initrd + FAT32)
//   read P    — печать файла
//   engrave P — перезапись файла на FAT32 (с параметрами)
//   carve  D  — mkdir на FAT32 (зарезервировано — 37.6c-2)
//   erase  P  — удалить файл (зарезервировано — 37.6c-2)
//   delve  D  — cwd (зарезервировано)
//   matrix    — стат. RAM/heap/процессов
//   embers    — последние записи лога (через serial)
//   pulse H   — ICMP echo
//   launch P  — запустить ELF из VFS
//   disk      — инфо ATA + FAT32
//   shade  N  — beep частоты N Hz
//   cycle     — reboot с шторкой
//   freeze    — shutdown
//   clock     — текущее время CMOS/RTC
//
// Будущие подэтапы добавят: lsmod, elfinfo, sound (audio_play_wav), forge.
// ============================================================================

#include "shell.h"
#include "keyboard.h"
#include "fb.h"
#include "serial.h"
#include "vfs.h"
#include "fat32.h"
#include "heap.h"
#include "pmm.h"
#include "io.h"
#include "rtc.h"
#include "speaker.h"
#include "power.h"
#include "timer.h"
#include "ata.h"
#include "icmp.h"
#include "netif.h"
#include "dhcp.h"
#include "dns.h"
#include "tcp.h"
#include "elf.h"
#include "vm.h"
#include "task.h"
#include "drm.h"
#include "virtio_gpu.h"
#include "compositor.h"
#include "framestat.h"
#include "amply.h"
// [этап 46] sandbox-runtime
#include "ns.h"
#include "cgroup.h"
#include "seccomp.h"
#include "box.h"
#include "mind.h"
#include "vmx.h"
#include "phantom.h"
#include "rune.h"
#include "apps.h"
// [этап 47.5] HTTPS
#include "https.h"
#include "netcfg.h"
#include "ui_motion.h"
#include "desktop.h"
// Этап 48: шифрование системного тома.
#include "fde.h"
#include "luks.h"

#include <stdint.h>
#include <stdbool.h>

#define LINE_MAX 256

// ----------------------------------------------------------------------------
// Tiny string helpers.
// ----------------------------------------------------------------------------
static int sh_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static int sh_strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static int s_strncmp_(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (!a[i]) return 0;
    }
    return 0;
}

static int sh_atoi(const char* s) {
    int v = 0;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

// Парсинг IP-адреса вида "192.168.1.1" → 0xC0A80101 (network byte order).
// Возвращает 0 при ошибке.
static uint32_t sh_parse_ip(const char* s) {
    uint32_t parts[4] = {0,0,0,0};
    int  pi = 0;
    int  digits_seen = 0;
    while (*s) {
        if (*s >= '0' && *s <= '9') {
            parts[pi] = parts[pi] * 10 + (uint32_t)(*s - '0');
            digits_seen++;
            if (parts[pi] > 255) return 0;
        } else if (*s == '.') {
            if (digits_seen == 0) return 0;
            pi++;
            if (pi >= 4) return 0;
            digits_seen = 0;
        } else {
            return 0;
        }
        s++;
    }
    if (pi != 3) return 0;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

// argv-токенизатор. Один пробел/таб — разделитель. argc <= max_args.
static int sh_tokenize(char* line, char* argv[], int max_args) {
    int argc = 0;
    int i = 0;
    while (line[i] && argc < max_args) {
        while (line[i] == ' ' || line[i] == '\t') i++;
        if (!line[i]) break;
        argv[argc++] = &line[i];
        while (line[i] && line[i] != ' ' && line[i] != '\t') i++;
        if (line[i]) line[i++] = 0;
    }
    return argc;
}

// ----------------------------------------------------------------------------
// Цвета для UI shell.
// ----------------------------------------------------------------------------
#define BG          0xFF101820
#define FG_TEXT     0xFFFFFFFF
#define FG_DIM      0xFF8A95B0
#define FG_OK       0xFF80FF80
#define FG_WARN     0xFFFFD200
#define FG_ERR      0xFFFF6060
#define FG_ACCENT   0xFF60D0F0

static void say(uint32_t fg, const char* s) {
    fb_console_set_color(fg, BG);
    fb_console_puts(s);
    fb_console_set_color(FG_TEXT, BG);
}

// ----------------------------------------------------------------------------
// Команды.
// ----------------------------------------------------------------------------

static void cmd_guide(void) {
    say(FG_WARN, "AMPLIFY-OS guide:\n");
    fb_console_puts("  guide          — this help\n");
    fb_console_puts("  shine          — clear screen\n");
    fb_console_puts("  voice <text>   — echo text\n");
    fb_console_puts("  scan           — list VFS files\n");
    fb_console_puts("  read <path>    — print file contents\n");
    fb_console_puts("  engrave <path> <text...>  — write text to FAT32 file\n");
    fb_console_puts("  matrix         — system info (memory, uptime)\n");
    fb_console_puts("  embers         — kernel log tail (in-memory klog)\n");
    fb_console_puts("  pulse <ip>     — ICMP echo (ping)\n");
    fb_console_puts("  disk           — ATA + FAT32 info\n");
    fb_console_puts("  launch <path>  — run ELF64 program from VFS (e.g. /hello_libc.elf)\n");
    fb_console_puts("  elfinfo <path> — show ELF header without running\n");
    fb_console_puts("  gpustat        — virtio-gpu/DRM state and frame counters (stage 41)\n");
    fb_console_puts("  gpubench [N]   — GPU vs CPU swap benchmark (default N=32 frames)\n");
    fb_console_puts("  weave          — network: show; weave auto|dhcp|static <ip> <mask> [gw] [dns] (48.2)\n");
    fb_console_puts("  summon <prism> — open an app window: summon Mesh, summon Browser (48.2)\n");
    fb_console_puts("  rune           — WebAssembly: list | run <file.wasm> [args] | status | stop | info <file> | selftest (alias wasm; window: summon Rune)\n");
    fb_console_puts("  phantom        — VT-x hypervisor: images | run <iso> | type <text> | screen | status | stop | selftest (alias vm; window: summon Phantom)\n");
    fb_console_puts("  compstat       — compositor stats: frames, dirty-rects, anims (stage 42)\n");
    fb_console_puts("  compstat watch — frame time counters to serial, on/off (stage 41.2)\n");
    fb_console_puts("  compstat slow N — slow all animations down N times, 1 = normal (48.2)\n");
    fb_console_puts("  shade <hz>     — PC speaker tone, 250 ms\n");
    fb_console_puts("  clock          — current RTC time\n");
    fb_console_puts("  Amply ...      — package manager (try `Amply` for sub-help)\n");
    fb_console_puts("  box ...        — sandbox runtime (etap 46; try `box` for sub-help)\n");
    fb_console_puts("  seal ...       — disk encryption: status | selftest | bench | passwd\n");
    fb_console_puts("  muse ...       — AMPLIFY MIND local AI: load | ask | bench | ps | logs (Ctrl+Space in GUI)\n");
    fb_console_puts("  cycle          — reboot AMPLIFY-OS\n");
    fb_console_puts("  freeze         — shutdown AMPLIFY-OS\n");
}

static void cmd_shine(void) {
    fb_console_reset();
    fb_console_set_color(FG_TEXT, BG);
}

static void cmd_voice(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        fb_console_puts(argv[i]);
        if (i + 1 < argc) fb_console_putc(' ');
    }
    fb_console_putc('\n');
}

static void cmd_scan(void) {
    int n = vfs_count();
    fb_console_puts("VFS files: ");
    fb_console_print_dec(n);
    fb_console_puts("\n");
    for (int i = 0; i < n; i++) {
        vfs_node_t* nd = vfs_at(i);
        fb_console_puts("  ");
        say(FG_OK, nd->name);
        fb_console_puts(" — ");
        fb_console_print_dec(nd->size);
        fb_console_puts(" bytes\n");
    }
}

static void cmd_read(int argc, char* argv[]) {
    if (argc < 2) {
        say(FG_ERR, "usage: read <path>\n");
        return;
    }
    const char* path = argv[1];

    int fd = vfs_open(path);
    if (fd >= 0) {
        char buf[256];
        int total = 0;
        for (;;) {
            int64_t got = vfs_read(fd, buf, sizeof(buf));
            if (got <= 0) break;
            for (int64_t i = 0; i < got; i++) fb_console_putc(buf[i]);
            total += (int)got;
            if (total > 8192) break;
        }
        vfs_close(fd);
        fb_console_puts("\n");
        return;
    }

    // Fallback на FAT32 (если путь /disk/...).
    char tmp[2048];
    int64_t got = fat32_read_file(path, tmp, sizeof(tmp));
    if (got >= 0) {
        for (int64_t i = 0; i < got; i++) fb_console_putc(tmp[i]);
        fb_console_puts("\n");
    } else {
        say(FG_ERR, "read: file not found\n");
    }
}

// engrave <path> word1 word2 ... → перезаписывает файл (через fat32_write_file)
// Поддерживает только пути /disk/...
static void cmd_engrave(int argc, char* argv[]) {
    if (argc < 3) {
        say(FG_ERR, "usage: engrave <path> <text...>\n");
        return;
    }
    const char* path = argv[1];
    char buf[1024];
    int  off = 0;
    for (int i = 2; i < argc && off < (int)sizeof(buf) - 2; i++) {
        const char* w = argv[i];
        while (*w && off < (int)sizeof(buf) - 1) buf[off++] = *w++;
        if (i + 1 < argc && off < (int)sizeof(buf) - 1) buf[off++] = ' ';
    }
    buf[off++] = '\n';

    int64_t r = fat32_write_file(path, buf, (uint64_t)off);
    if (r < 0) {
        say(FG_ERR, "engrave: write failed (file must already exist on /disk/)\n");
    } else {
        say(FG_OK, "engrave: wrote ");
        fb_console_print_dec((uint64_t)r);
        fb_console_puts(" bytes\n");
    }
}

static void cmd_matrix(void) {
    pmm_stats_t  ps = pmm_get_stats();
    heap_stats_t hs = heap_get_stats();
    uint64_t up_ms = timer_get_uptime_ms();

    say(FG_WARN, "Matrix:\n");
    fb_console_puts("  PMM free pages: ");
    fb_console_print_dec(ps.free_pages);
    fb_console_puts("  used: ");
    fb_console_print_dec(ps.used_pages);
    fb_console_puts("\n");

    fb_console_puts("  Heap used: ");
    fb_console_print_dec(hs.used_size);
    fb_console_puts("/");
    fb_console_print_dec(hs.total_size);
    fb_console_puts(" bytes (");
    fb_console_print_dec(hs.used_blocks);
    fb_console_puts(" blocks)\n");

    fb_console_puts("  Uptime: ");
    fb_console_print_dec(up_ms / 1000);
    fb_console_puts(".");
    fb_console_print_dec((up_ms % 1000) / 100);
    fb_console_puts(" s\n");
}

// embers — последние записи системного лога. Минимальная реализация на 37.6c-1:
// мы храним «ringbuffer заметок» в самом shell. Полноценный embersd с
// /disk/Sanctum/embers/ — этап 51.
// [этап 49] Теперь это настоящий хвост журнала ядра: serial.c держит
// последние 64 КБ вывода в памяти (klog). Бинарный embersd — этап 52.
static void cmd_embers(void) {
    static char tail[2048];
    uint64_t n = klog_read_tail(tail, sizeof(tail));
    const char* p = tail;
    if (n == sizeof(tail) - 1) { while (*p && *p != '\n') p++; if (*p) p++; }
    say(FG_DIM, "embers (kernel log tail):\n");
    fb_console_puts(p);
    fb_console_puts("\n");
}

// ----------------------------------------------------------------------------
// [этап 49] muse — AMPLIFY MIND из командной строки.
// ----------------------------------------------------------------------------
static void muse_print_status(void) {
    char st[200];
    mind_status(st, sizeof(st));
    say(FG_ACCENT, "MIND: ");
    fb_console_puts(st);
    fb_console_puts("\n");
}

// Печатает реплики, появившиеся после номера base (mind_msg_total() ДО
// mind_submit), и дописывает растущую реплику MIND по мере генерации
// (Esc — стоп). Номер, а не индекс: /ps и /ls выполняются синхронно внутри
// mind_submit, и их карточка уже лежит в журнале к началу печати; а журнал
// хранит последние 40 реплик, так что индексы при переполнении сдвигаются.
static void muse_stream(uint32_t base) {
    uint32_t cur = 0;
    bool have = false;
    int printed = 0;
    for (;;) {
        mind_poll();
        char c = keyboard_get_char();
        if (c == 27) mind_cancel();
        uint32_t total = mind_msg_total();
        uint32_t oldest = total - (uint32_t)mind_msg_count();
        uint32_t from = have ? cur : base;
        if (from < oldest) from = oldest;
        for (uint32_t s = from; s < total; s++) {
            int i = (int)(s - oldest);
            if (mind_msg_role(i) == MIND_ROLE_USER) continue;
            if (!have || s != cur) {
                if (have) fb_console_puts("\n");
                cur = s;
                have = true;
                printed = 0;
                if (mind_msg_role(i) == MIND_ROLE_TOOL) say(FG_WARN, "[data] ");
                else if (mind_msg_role(i) == MIND_ROLE_MIND) say(FG_ACCENT, "MIND> ");
            }
            const char* t = mind_msg_text(i);
            int len = sh_strlen(t);
            if (len > printed) { fb_console_puts(t + printed); printed = len; }
        }
        mind_state_t st = mind_state();
        if (st != MIND_ST_BUSY && st != MIND_ST_LOADING) break;
        __asm__ __volatile__("sti; hlt");
    }
    fb_console_puts("\n");
    if (mind_state() == MIND_ST_CONFIRM) {
        say(FG_WARN, "MIND wants to run: ");
        fb_console_puts(mind_pending_exec());
        fb_console_puts("  [y/N] ");
        char c = keyboard_wait_char();
        fb_console_putc(c);
        fb_console_puts("\n");
        uint32_t next = mind_msg_total();
        mind_confirm(c == 'y' || c == 'Y');
        muse_stream(next);
    }
}

static void cmd_muse(int argc, char* argv[]) {
    const char* sub = argc > 1 ? argv[1] : "";
    if (!*sub || sh_strcmp(sub, "status") == 0) {
        muse_print_status();
        fb_console_puts("  muse load [path] | ask <text> | bench [n] | ps | ls [path] | read <path> | logs | new | unload | selftest\n");
        return;
    }
    if (sh_strcmp(sub, "load") == 0) {
        say(FG_DIM, "loading model...\n");
        if (mind_load_sync(argc > 2 ? argv[2] : 0) != 0) { say(FG_ERR, mind_error()); fb_console_puts("\n"); return; }
        muse_print_status();
        return;
    }
    if (sh_strcmp(sub, "unload") == 0) { mind_unload(); say(FG_OK, "model unloaded\n"); return; }
    if (sh_strcmp(sub, "new") == 0) { mind_new_chat(); say(FG_OK, "new conversation\n"); return; }
    if (sh_strcmp(sub, "selftest") == 0) {
        bool ok = mind_self_test();
        say(ok ? FG_OK : FG_ERR, ok ? "MIND self-test PASS\n" : "MIND self-test FAIL (see serial)\n");
        return;
    }
    if (sh_strcmp(sub, "bench") == 0) {
        if (mind_state() != MIND_ST_READY && mind_load_sync(0) != 0) { say(FG_ERR, mind_error()); fb_console_puts("\n"); return; }
        int n = argc > 2 ? sh_atoi(argv[2]) : 64;
        int g = 0, p = 0;
        int got = mind_bench(n, &g, &p);
        fb_console_puts("bench: ");
        fb_console_print_dec((uint64_t)(got > 0 ? got : 0));
        fb_console_puts(" tokens, generation ");
        fb_console_print_dec((uint64_t)(g / 10));
        fb_console_puts(".");
        fb_console_print_dec((uint64_t)(g % 10));
        fb_console_puts(" tok/s, prefill ");
        fb_console_print_dec((uint64_t)(p / 10));
        fb_console_puts(" tok/s\n");
        serial_puts("[mind] bench tok/s x10=");
        serial_print_dec((uint64_t)g);
        serial_puts("\n");
        return;
    }
    // Остальное — текст для MIND: `muse ask ...`, `muse ps`, `muse ls /disk`...
    static char line[512];
    line[0] = 0;
    int start = 1;
    if (sh_strcmp(sub, "ask") == 0) start = 2;
    else {
        line[0] = '/';
        line[1] = 0;
    }
    int n = sh_strlen(line);
    for (int i = start; i < argc; i++) {
        for (const char* s = argv[i]; *s && n < 500; s++) line[n++] = *s;
        if (i + 1 < argc && n < 500) line[n++] = ' ';
    }
    line[n] = 0;
    uint32_t base = mind_msg_total();
    if (mind_submit(line) != 0 && mind_state() != MIND_ST_READY) {
        say(FG_ERR, "MIND is busy or has no model: ");
        fb_console_puts(mind_error());
        fb_console_puts("\n");
        return;
    }
    muse_stream(base);
}

static void cmd_pulse(int argc, char* argv[]) {
    if (argc < 2) {
        say(FG_ERR, "usage: pulse <ip>\n");
        return;
    }
    uint32_t ip = sh_parse_ip(argv[1]);
    if (!ip) {
        // Может быть hostname?
        ip = dns_resolve(argv[1]);
        if (!ip) {
            say(FG_ERR, "pulse: bad ip / dns failed\n");
            return;
        }
    }
    fb_console_puts("pulse -> ");
    netif_dump_ip4(ip);
    fb_console_puts(" ...\n");

    const char payload[] = "amplify-os-pulse";
    icmp_send_echo(ip, 1, 1, payload, sizeof(payload) - 1);
    timer_sleep(300);
    say(FG_OK, "  pulse sent (replies via serial trace)\n");
}

// [этап 47.5] cmd_https: выполняет HTTPS GET и печатает status + первые 256
// байт body.  Использование: https <url>.  Например: https https://example.com.
static char g_https_body[8192];
static void cmd_https(int argc, char* argv[]) {
    if (argc < 2) { say(FG_ERR, "usage: https <url>\n"); return; }
    size_t body_len = 0;
    int    status   = 0;
    fb_console_puts("https GET ");
    fb_console_puts(argv[1]);
    fb_console_puts(" ...\n");
    // require_trust = false: позволим untrusted certs для тестов.  Strict-mode
    // включится при `https-strict ...` (опционально, не реализуем сейчас).
    bool ok = https_get(argv[1], g_https_body, sizeof(g_https_body),
                        &body_len, &status, false);
    if (!ok) { say(FG_ERR, "https: request failed\n"); return; }
    char buf[64];
    int n = 0;
    const char* p = "  status="; while (*p) buf[n++] = *p++;
    if (status >= 100) { buf[n++] = '0' + (status/100)%10; }
    if (status >= 10)  { buf[n++] = '0' + (status/10)%10;  }
    buf[n++] = '0' + status%10;
    buf[n++] = ' '; buf[n++] = 'l'; buf[n++] = 'e'; buf[n++] = 'n'; buf[n++] = '=';
    // simple uint print
    char num[16]; int nn = 0; size_t tmp = body_len;
    if (tmp == 0) num[nn++] = '0';
    else { while (tmp) { num[nn++] = '0' + (tmp % 10); tmp /= 10; } }
    while (nn > 0) buf[n++] = num[--nn];
    buf[n++] = '\n'; buf[n] = 0;
    say(FG_OK, buf);
    // Печатаем первые 256 байт body.
    char preview[260];
    size_t take = body_len < 256 ? body_len : 256;
    for (size_t i = 0; i < take; i++) {
        char c = g_https_body[i];
        preview[i] = (c >= 32 && c < 127) || c == '\n' || c == '\r' ? c : '.';
    }
    preview[take] = 0;
    fb_console_puts(preview);
    if (body_len > 256) fb_console_puts("\n...[truncated]\n");
}

// ----------------------------------------------------------------------------
// [48.2] `weave` — сеть из терминала: показать состояние или сменить режим.
//   weave                              — режим, источник адреса, адреса
//   weave auto | dhcp                  — включить DHCP (auto — с запасной статикой)
//   weave static <ip> <mask> [gw] [dns] — статический адрес
// Настройка применяется и записывается в /disk/Sanctum/network.conf — то же
// самое делает экран Mesh.
// ----------------------------------------------------------------------------
static void weave_ip(const char* label, uint32_t ip) {
    char b[16];
    netcfg_format_ip(ip, b);
    fb_console_puts(label);
    fb_console_puts(ip ? b : "-");
    fb_console_puts("\n");
}

static void cmd_weave(int argc, char* argv[]) {
    net_interface_t* n = netif_get_default();
    if (argc < 2) {
        const netcfg_t* c = netcfg_get();
        fb_console_puts("  iface   : "); fb_console_puts(n ? n->name : "none");
        fb_console_puts(" ("); fb_console_puts(netif_driver_name()); fb_console_puts(")\n");
        fb_console_puts("  mode    : "); fb_console_puts(netcfg_mode_name(c->mode));
        fb_console_puts("   source: "); fb_console_puts(netcfg_source_name());
        fb_console_puts("   dhcp: ");   fb_console_puts(dhcp_state_name()); fb_console_puts("\n");
        weave_ip("  address : ", n ? n->ip4_addr : 0);
        weave_ip("  mask    : ", n ? n->ip4_netmask : 0);
        weave_ip("  gateway : ", n ? n->ip4_gateway : 0);
        weave_ip("  dns     : ", dns_get_server());
        weave_ip("  static  : ", c->ip);
        return;
    }
    netcfg_t c = *netcfg_get();
    if (sh_strcmp(argv[1], "auto") == 0)      c.mode = NETCFG_AUTO;
    else if (sh_strcmp(argv[1], "dhcp") == 0) c.mode = NETCFG_DHCP;
    else if (sh_strcmp(argv[1], "static") == 0) {
        if (argc < 4 || !netcfg_parse_ip(argv[2], &c.ip) || !netcfg_parse_ip(argv[3], &c.mask)) {
            say(FG_ERR, "usage: weave static <ip> <mask> [gateway] [dns]\n");
            return;
        }
        c.mode = NETCFG_STATIC;
        c.gateway = 0; c.dns = 0; c.dns2 = 0;
        if (argc >= 5 && !netcfg_parse_ip(argv[4], &c.gateway)) { say(FG_ERR, "weave: bad gateway\n"); return; }
        if (argc >= 6 && !netcfg_parse_ip(argv[5], &c.dns))     { say(FG_ERR, "weave: bad dns\n"); return; }
    } else { say(FG_ERR, "usage: weave [auto|dhcp|static <ip> <mask> [gw] [dns]]\n"); return; }

    const char* why = "";
    if (!netcfg_apply(&c, &why)) { say(FG_ERR, "weave: "); say(FG_ERR, why); say(FG_ERR, "\n"); return; }
    say(FG_OK, netcfg_save() ? "  applied and saved to network.conf\n" : "  applied (not saved: no disk)\n");
}

// [48.2] `summon <prism>` — открыть окно приложения по имени из реестра
// рабочего стола (тот же список, что показывает Core).
static void cmd_summon(int argc, char* argv[]) {
    if (argc < 2) { say(FG_ERR, "usage: summon <prism>   (Mesh, Browser, Files, Notes, ...)\n"); return; }
    if (!desktop_launch_named(argv[1])) { say(FG_ERR, "summon: no such prism\n"); return; }
    say(FG_OK, "  summoned\n");
}

// [этап 50] `phantom` — гипервизор VT-x (алиас `vm`).
//   phantom            — сводка: есть ли VT-x, годится ли, ключевые возможности
//   phantom caps       — сырые MSR IA32_VMX_* и все распознанные биты
//   phantom selftest   — [50.1] все сценарии со встроенным проверочным гостем
//   phantom bench      — [50.1] цена одного выхода из гостя
//   phantom spin       — [50.1] фоновый гость, считающий в бесконечном цикле
//   phantom status     — [50.1] что он насчитал; phantom stop — снять его
//   phantom run <iso> [строка] — [50.3] гость с ISO через наш BIOS; [50.4] строка
//                        для загрузчика и диск с тем же ISO
//   phantom type <txt> — набрать строку на консоли гостя; phantom screen — его экран
//   phantom images     — [50.5] образы гостей на диске и на установочном носителе
// Окно гостя — приложение Phantom (`summon Phantom`).
// Каждый вызов пишет в serial строку `[phantom] status …` — по ней судит тест
// (текст окна терминала в serial не попадает).
static void cmd_phantom(int argc, char* argv[]) {
    if (argc >= 2 && sh_strcmp(argv[1], "caps") == 0) {
        vmx_describe(fb_console_puts, "  ", true);
        vmx_log_status();
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "run") == 0) {
        // [50.3] Гость с ISO через наш BIOS. Отказ (нет VT-x, нет файла, не
        // ISO) phantom_run объясняет сам, в окне и в serial.
        if (argc < 3) { say(FG_ERR, "usage: phantom run <path to .iso on /disk> [line for the boot loader]\n"); return; }
        // [50.4] Остальные слова — строка, «набранная» в приглашении
        // загрузчика (метка и параметры ядра).
        static char line[200];
        int n = 0;
        for (int i = 3; i < argc; i++) {
            if (i > 3 && n < (int)sizeof(line) - 1) line[n++] = ' ';
            for (const char* p = argv[i]; *p && n < (int)sizeof(line) - 1; p++) line[n++] = *p;
        }
        line[n] = 0;
        phantom_run(fb_console_puts, argv[2], line);
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "screen") == 0) { phantom_screen(fb_console_puts); return; }
    if (argc >= 2 && sh_strcmp(argv[1], "images") == 0) {
        // [50.5] Тот же список, что показывает окно Phantom. Путь из него
        // годится для `phantom run`.
        static phantom_image_t img[12];
        int n = phantom_images(img, 12);
        serial_puts("[phantom] images: "); serial_print_dec((uint64_t)n); serial_puts("\n");
        if (!n) say(FG_WARN, "  no guest images: put a bootable *.iso into /disk/vault/phantom\n");
        for (int i = 0; i < n; i++) {
            fb_console_puts("  "); fb_console_puts(img[i].path);
            fb_console_puts(img[i].on_medium ? "   (installation medium)\n" : "\n");
            serial_puts("[phantom] image "); serial_puts(img[i].path);
            serial_puts(" "); serial_print_dec(img[i].bytes); serial_puts(" bytes\n");
        }
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "selftest") == 0) {
        // Сценарии идут подряд на BSP (~1 с): на это время кадры стоят.
        int fails = phantom_selftest(fb_console_puts);
        if (fails) say(FG_ERR, "  some scenarios FAILED (details in the kernel log)\n");
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "bench") == 0)  { phantom_bench(fb_console_puts); return; }
    if (argc >= 2 && sh_strcmp(argv[1], "spin") == 0)   { phantom_spin_start(fb_console_puts); return; }
    if (argc >= 2 && sh_strcmp(argv[1], "status") == 0) { phantom_spin_status(fb_console_puts); return; }
    if (argc >= 2 && sh_strcmp(argv[1], "stop") == 0)   { phantom_spin_stop(fb_console_puts); return; }
    // [50.2] Стенд: ядро Linux напрямую. В справке его нет намеренно —
    // штатный запуск гостя будет через BIOS (`phantom run <iso>`).
    if (argc >= 2 && sh_strcmp(argv[1], "linux") == 0) {
        phantom_linux_start(fb_console_puts, argc >= 3 && sh_strcmp(argv[2], "init") == 0);
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "type") == 0) {
        // Слова команды снова склеиваются через пробел: оболочка их разрезала.
        static char text[200];
        int n = 0;
        for (int i = 2; i < argc; i++) {
            if (i > 2 && n < (int)sizeof(text) - 1) text[n++] = ' ';
            for (const char* p = argv[i]; *p && n < (int)sizeof(text) - 1; p++) text[n++] = *p;
        }
        text[n] = 0;
        phantom_type(fb_console_puts, text);
        return;
    }
    if (argc >= 2) { say(FG_ERR, "usage: phantom [caps | selftest | bench | spin | images | run <iso> | type <text> | screen | status | stop]\n"); return; }
    vmx_describe(fb_console_puts, "  ", false);
    if (vmx_ready()) say(FG_OK,   "  VT-x is ready for guests\n");
    else             say(FG_WARN, "  VT-x cannot be used on this machine\n");
    vmx_log_status();
}

// [этап 51] `rune` — WebAssembly (алиас `wasm`). «Руна» — модуль .wasm.
//   rune                 — что умеет движок
//   rune info <файл>     — разобрать и проверить модуль, показать, что в нём
//   rune selftest        — встроенные модули самопроверки
//   rune spec <пакет> [verbose] — официальный набор тестов (spec.pack)
// Всё, что печатается, идёт и в serial (строки `[rune] …`, `[wasm-spec] …`) —
// по ним судит тест.
static void cmd_rune(int argc, char* argv[]) {
    if (argc >= 3 && sh_strcmp(argv[1], "info") == 0) { (void)rune_info(fb_console_puts, argv[2]); return; }
    if (argc >= 3 && sh_strcmp(argv[1], "spec") == 0) {
        // Тысячи модулей подряд на BSP: на это время кадры стоят (секунды).
        (void)rune_spec(fb_console_puts, argv[2], argc >= 4 && sh_strcmp(argv[3], "verbose") == 0);
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "selftest") == 0) { (void)rune_selftest(fb_console_puts); return; }
    if (argc >= 3 && sh_strcmp(argv[1], "run") == 0) {
        // rune run [-b] [-i] [-t мс] [-m МБ] [-d каталог] [-r] [-n] <файл> [аргументы]
        // Программа идёт на отдельном ядре; команда возвращается сразу, вывод
        // приходит в терминал по мере работы. -b — «в фоне»: терминал свободен,
        // вывод только в журнал.
        rune_opts_t o;
        for (unsigned i = 0; i < sizeof(o); i++) ((uint8_t*)&o)[i] = 0;
        int at = 2;
        bool bg = false, stdin_open = false;
        while (at < argc && argv[at][0] == '-') {
            const char* f = argv[at];
            bool val = at + 1 < argc;
            if ((sh_strcmp(f, "-t") == 0 || sh_strcmp(f, "-m") == 0) && val) {
                uint32_t n = 0;
                for (const char* c = argv[at + 1]; *c >= '0' && *c <= '9'; c++) n = n * 10 + (uint32_t)(*c - '0');
                if (f[1] == 't') o.cpu_ms = n; else o.memory_mb = n;
                at += 2;
            } else if (sh_strcmp(f, "-d") == 0 && val) { o.dir = argv[at + 1]; at += 2; }
            else if (sh_strcmp(f, "-r") == 0) { o.read_only = true; at++; }
            else if (sh_strcmp(f, "-n") == 0) { o.no_files = true; at++; }
            else if (sh_strcmp(f, "-b") == 0) { bg = true; at++; }
            else if (sh_strcmp(f, "-i") == 0) { stdin_open = true; at++; }
            else break;
        }
        if (at < argc) {
            o.path = argv[at];
            o.nargs = argc - at - 1;
            o.args = &argv[at + 1];
            // Не в фоне программа принадлежит этому терминалу: пишет в него и
            // получает от него клавиатуру. В фоне ввод открывает -i (его даёт
            // `rune type`), вывод идёт только в журнал.
            o.owner = bg ? 0 : app_term_job_owner();
            rune_sink_t sink = bg ? 0 : app_term_job_sink();
            o.interactive = bg ? stdin_open : o.owner != 0;
            if (rune_start(&o, sink) && bg) say(FG_WARN, "  started in the background: rune status | rune type <text> | rune stop\n");
            return;
        }
    }
    if (argc >= 2 && sh_strcmp(argv[1], "status") == 0) { rune_status(fb_console_puts); return; }
    // [51.5] rune jit [on|off] — чем исполнять программы, запущенные после этого.
    if (argc >= 2 && sh_strcmp(argv[1], "jit") == 0) {
        if (argc >= 3) rune_jit_set(sh_strcmp(argv[2], "off") != 0);
        serial_puts(rune_jit_on() ? "[rune] jit on\n" : "[rune] jit off\n");
        say(FG_WARN, rune_jit_on() ? "  new programs run as machine code (JIT)\n" : "  new programs run in the interpreter\n");
        return;
    }
    // [51.4] Окно Rune: что в его консоли сейчас (screen) и весь журнал (log) —
    // в serial; list — библиотека программ, тот же список, что в окне.
    if (argc >= 2 && (sh_strcmp(argv[1], "screen") == 0 || sh_strcmp(argv[1], "log") == 0)) {
        app_rune_dump(argv[1][0] == 'l');
        say(FG_WARN, "  the Rune window console is dumped to the kernel log\n");
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "list") == 0) {
        static rune_prog_t pr[24];
        int n = rune_programs(pr, 24);
        serial_puts("[rune] programs: "); serial_print_dec((uint64_t)n); serial_puts("\n");
        if (!n) say(FG_WARN, "  no programs: put a *.wasm file into " RUNE_PROG_DIR "\n");
        for (int i = 0; i < n; i++) {
            fb_console_puts("  "); fb_console_puts(pr[i].path);
            if (pr[i].about[0]) { fb_console_puts("  - "); fb_console_puts(pr[i].about); }
            fb_console_puts("\n");
            serial_puts("[rune] program "); serial_puts(pr[i].path);
            serial_puts(pr[i].builtin ? " built-in " : " disk "); serial_print_dec(pr[i].bytes);
            serial_puts(" | "); serial_puts(pr[i].about); serial_puts("\n");
        }
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "stop") == 0) {
        if (rune_active()) { rune_stop(); say(FG_WARN, "  stopping the program\n"); }
        else say(FG_WARN, "  no program is running\n");
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "eof") == 0) { rune_input_eof(); return; }
    if (argc >= 2 && sh_strcmp(argv[1], "type") == 0) {
        // Строка на ввод программе: слова через пробел и перевод строки в конце.
        for (int i = 2; i < argc; i++) {
            int n = 0;
            while (argv[i][n]) n++;
            if (i > 2) rune_input(" ", 1);
            rune_input(argv[i], n);
        }
        rune_input("\n", 1);
        return;
    }
    if (argc >= 4 && sh_strcmp(argv[1], "call") == 0) {
        // rune call [-t мс] <файл> <функция> [аргументы]. Исполняется на BSP:
        // кадры стоят, пока функция не вернётся или не выйдет время.
        int at = 2;
        uint32_t limit = 0;
        if (sh_strcmp(argv[2], "-t") == 0 && argc >= 6) {
            for (const char* c = argv[3]; *c >= '0' && *c <= '9'; c++) limit = limit * 10 + (uint32_t)(*c - '0');
            at = 4;
        }
        (void)rune_call(fb_console_puts, argv[at], argv[at + 1], argc - at - 2, &argv[at + 2], limit);
        return;
    }
    if (argc >= 2) { say(FG_ERR, "usage: rune [run [-b] [-t ms] [-m MB] [-d dir] [-r] [-n] <file.wasm> [args] | list | jit [on|off] | status | stop | type <text> | eof | info <file> | call <file> <func> [args] | selftest | spec <pack>]\n"); return; }
    say(FG_OK,   "  rune: WebAssembly 2.0 engine of AMPLIFY-OS (no SIMD, threads, exceptions)\n");
    say(FG_WARN, "  rune run <file.wasm> [args] - run a WASI program; it sees " RUNE_DEFAULT_DIR " as /\n");
    say(FG_WARN, "  the program runs on its own core; Ctrl+C stops it, Ctrl+D ends its input\n");
    serial_puts(rune_jit_on() ? "[rune] status engine=1 stage=51.3 exec=1 wasi=1 core=1 jit=1\n"
                              : "[rune] status engine=1 stage=51.3 exec=1 wasi=1 core=1 jit=0\n");
}

// ----------------------------------------------------------------------------
// Этап 40: `launch <path>` — запуск ELF64-программы из VFS.
// 1. Открываем файл через VFS, читаем в kernel-heap.
// 2. Создаём user vm_space.
// 3. elf64_load — маппит PT_LOAD сегменты в vm.
// 4. Аллоцируем 2-страничный user-stack.
// 5. task_create_user регистрирует задачу в round-robin.
// 6. task_yield передаёт ей управление; при exit() мы вернёмся сюда.
// ----------------------------------------------------------------------------
static void cmd_launch(int argc, char* argv[]) {
    if (argc < 2) { say(FG_ERR, "usage: launch <path>\n"); return; }
    const char* path = argv[1];

    int fd = vfs_open(path);
    if (fd < 0) {
        say(FG_ERR, "launch: "); say(FG_ERR, path);
        say(FG_ERR, ": not found\n");
        return;
    }
    int64_t sz = vfs_size_fd(fd);
    if (sz <= 0 || sz > 16 * 1024 * 1024) {
        say(FG_ERR, "launch: bad size\n"); vfs_close(fd); return;
    }
    uint8_t* blob = (uint8_t*)kmalloc((size_t)sz);
    if (!blob) { say(FG_ERR, "launch: out of memory\n"); vfs_close(fd); return; }
    int64_t got = vfs_read(fd, blob, (uint64_t)sz);
    vfs_close(fd);
    if (got != sz) { say(FG_ERR, "launch: short read\n"); kfree(blob); return; }

    vm_space_t* vm = vm_create_user();
    if (!vm) { say(FG_ERR, "launch: vm_create_user failed\n"); kfree(blob); return; }

    uint64_t entry = elf64_load(blob, (uint64_t)sz, vm);
    if (!entry) {
        say(FG_ERR, "launch: elf64_load failed (not a valid ELF64)\n");
        kfree(blob); return;
    }

    // User-stack: 2 страницы выше .text/.bss.
    uint64_t stack_va_base = USER_BASE + 0x100000;
    for (int i = 0; i < 2; i++) {
        uint64_t pa = pmm_alloc_page();
        if (!pa) { say(FG_ERR, "launch: OOM user stack\n"); kfree(blob); return; }
        if (!vm_map_user_page(vm, stack_va_base + i * 0x1000, pa, true)) {
            say(FG_ERR, "launch: stack map failed\n"); kfree(blob); return;
        }
    }
    uint64_t user_rsp = stack_va_base + 2 * 0x1000 - 16;

    task_t* t = task_create_user(vm, entry, user_rsp);
    if (!t) { say(FG_ERR, "launch: task_create_user failed\n"); kfree(blob); return; }

    // Сообщение со stack-формированием tid в ASCII (без printf).
    say(FG_WARN, "launch: spawned tid=");
    char nbuf[16]; int n = 0; int v = t->tid;
    if (v == 0) nbuf[n++] = '0';
    else { char tmp[16]; int tn = 0;
        while (v > 0) { tmp[tn++] = '0' + (v % 10); v /= 10; }
        while (tn > 0) nbuf[n++] = tmp[--tn];
    }
    nbuf[n] = 0;
    say(FG_WARN, nbuf); say(FG_WARN, " "); say(FG_WARN, path); say(FG_WARN, "\n");

    // Блоб больше не нужен — elf64_load уже скопировал PT_LOAD в user-pages.
    kfree(blob);

    // Передаём управление новой задаче. Она вернёт нам CPU через exit() или yield().
    task_yield();
}

// ----------------------------------------------------------------------------
// Этап 40: `elfinfo <path>` — печать ELF64-заголовка (без запуска).
// ----------------------------------------------------------------------------
static void cmd_elfinfo(int argc, char* argv[]) {
    if (argc < 2) { say(FG_ERR, "usage: elfinfo <path>\n"); return; }
    int fd = vfs_open(argv[1]);
    if (fd < 0) { say(FG_ERR, "elfinfo: not found\n"); return; }
    uint8_t hdr[64];
    int64_t got = vfs_read(fd, hdr, sizeof(hdr));
    vfs_close(fd);
    if (got < 64) { say(FG_ERR, "elfinfo: too short\n"); return; }
    if (hdr[0] != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') {
        say(FG_ERR, "elfinfo: not an ELF file\n"); return;
    }
    say(FG_WARN, "ELF magic OK.  ");
    fb_console_puts("class=");
    fb_console_puts(hdr[4] == 2 ? "ELF64" : "ELF32");
    fb_console_puts(" data=");
    fb_console_puts(hdr[5] == 1 ? "LSB" : "MSB");
    fb_console_puts(" type=");
    uint16_t et = *(uint16_t*)&hdr[16];
    fb_console_puts(et == 2 ? "EXEC" : et == 3 ? "DYN" : "?");
    fb_console_puts(" machine=");
    uint16_t em = *(uint16_t*)&hdr[18];
    fb_console_puts(em == 62 ? "x86_64" : "?");
    fb_console_puts("\n");
}

static void cmd_disk(void) {
    say(FG_WARN, "disk:\n");
    if (!ata_present()) {
        fb_console_puts("  ATA: no disk attached\n");
        return;
    }
    const ata_info_t* di = ata_info();
    fb_console_puts("  ATA model: ");
    fb_console_puts(di->model);
    fb_console_puts("\n  size: ");
    fb_console_print_dec(di->size_mb);
    fb_console_puts(" MB\n");
    fb_console_puts("  FAT32 mount: ");
    fb_console_puts(fat32_is_mounted() ? "/disk\n" : "(not mounted)\n");
}

static void cmd_shade(int argc, char* argv[]) {
    if (argc < 2) {
        say(FG_ERR, "usage: shade <hz>\n");
        return;
    }
    int hz = sh_atoi(argv[1]);
    if (hz < 50 || hz > 8000) {
        say(FG_ERR, "shade: hz must be 50..8000\n");
        return;
    }
    fb_console_puts("shade ");
    fb_console_print_dec((uint64_t)hz);
    fb_console_puts(" Hz x 250 ms ...\n");
    speaker_beep((uint32_t)hz, 250);
}

// ----------------------------------------------------------------------------
// Этап 41: `gpustat` — печать состояния DRM/virtio-gpu подсистемы.
// `gpubench` — синхронный self-benchmark (16 GPU vs 16 CPU frames).
// ----------------------------------------------------------------------------
static void cmd_gpustat(int argc, char* argv[]) {
    // [41.2] `gpustat mode 1280x720` — сменить разрешение через virtio-gpu.
    if (argc >= 3 && sh_strcmp(argv[1], "mode") == 0) {
        uint32_t w = 0, h = 0; const char* p = argv[2];
        while (*p >= '0' && *p <= '9') w = w * 10 + (uint32_t)(*p++ - '0');
        if (*p == 'x' || *p == 'X') p++;
        while (*p >= '0' && *p <= '9') h = h * 10 + (uint32_t)(*p++ - '0');
        if (drm_set_mode(w, h)) say(FG_OK, "display mode changed\n");
        else say(FG_ERR, "gpustat mode: need virtio-gpu and WxH not larger than the boot mode\n");
        return;
    }
    drm_info_t info = drm_get_info();
    say(FG_WARN, "GPU:\n");
    fb_console_puts("  active: ");
    fb_console_puts(info.active ? "yes (virtio-gpu)" : "no (VESA fallback)");
    fb_console_puts("\n");
    if (info.active) {
        fb_console_puts("  resource_id: ");
        fb_console_print_dec(info.resource_id);
        fb_console_puts("\n  scanout: ");
        fb_console_print_dec(info.width);
        fb_console_puts("x");
        fb_console_print_dec(info.height);
        fb_console_puts("\n  frames presented: ");
        fb_console_print_dec(info.frames_presented);
        fb_console_puts(" (failed=");
        fb_console_print_dec(info.frames_failed);
        fb_console_puts(")\n  cmd/resp: ");
        fb_console_print_dec(info.cmd_total);
        fb_console_puts("/");
        fb_console_print_dec(info.resp_total);
        fb_console_puts("\n  inflight: ");
        fb_console_print_dec(virtio_gpu_inflight());
        fb_console_puts("\n  total present time: ");
        fb_console_print_dec(info.ms_in_present);
        fb_console_puts(" ms\n");
    }
}

static void cmd_gpubench(int argc, char* argv[]) {
    int frames = 32;
    if (argc >= 2) {
        int v = sh_atoi(argv[1]);
        if (v >= 4 && v <= 256) frames = v;
    }
    if (!drm_active()) {
        say(FG_ERR, "gpubench: GPU is not active (VESA fallback). Run with -device virtio-gpu-pci.\n");
        return;
    }
    fb_console_puts("Running ");
    fb_console_print_dec((uint64_t)frames);
    fb_console_puts(" GPU vs CPU frames... (results in serial log)\n");
    bool gpu_win = drm_self_benchmark((uint32_t)frames);
    if (gpu_win) say(FG_OK,   "  GPU >= 2x faster than CPU memcpy.\n");
    else         say(FG_WARN, "  GPU was NOT 2x faster than CPU on this run.\n");
}

// ----------------------------------------------------------------------------
// Этап 42: `compstat` -- статистика композитора (тройная буферизация,
// анимации, dirty-rect'ы).
// ----------------------------------------------------------------------------
static void cmd_compstat(int argc, char* argv[]) {
    extern bool compositor_active(void);
    extern compositor_stats_t compositor_get_stats(void);
    // [41.2] `compstat watch` — переключить ежесекундный отчёт счётчиков
    // кадра в serial (по умолчанию выключен: строки попадали бы в klog).
    // [48.2] `compstat slow N` — замедлить анимации в N раз (1 — как обычно).
    if (argc >= 3 && sh_strcmp(argv[1], "slow") == 0) {
        uint32_t n = 0;
        for (const char* p = argv[2]; *p >= '0' && *p <= '9'; p++) n = n * 10 + (uint32_t)(*p - '0');
        ui_motion_set_slow(n);
        say(FG_OK, ui_motion_slow() > 1 ? "animations: slow motion\n" : "animations: normal speed\n");
        return;
    }
    if (argc >= 2 && sh_strcmp(argv[1], "watch") == 0) {
        fs_set_serial(!fs_serial());
        say(FG_OK, fs_serial() ? "frame stats -> serial: on\n"
                               : "frame stats -> serial: off\n");
        return;
    }
    say(FG_WARN, "Compositor:\n");
    fb_console_puts("  active: ");
    fb_console_puts(compositor_active() ? "yes (triple-buf + AA + shadows)" : "no (fallback)");
    fb_console_puts("\n");
    if (compositor_active()) {
        compositor_stats_t st = compositor_get_stats();
        fb_console_puts("  frames total:   ");
        fb_console_print_dec(st.frames_total);
        fb_console_puts("\n  frames partial: ");
        fb_console_print_dec(st.frames_partial);
        fb_console_puts("\n  frames full:    ");
        fb_console_print_dec(st.frames_full);
        fb_console_puts("\n  dirty rects:    ");
        fb_console_print_dec(st.dirty_rects_total);
        fb_console_puts("\n  anims started:  ");
        fb_console_print_dec(st.anims_started);
        fb_console_puts("\n  anims done:     ");
        fb_console_print_dec(st.anims_completed);
        fb_console_puts("\n");
    }

    // [41.2] Последнее секундное окно счётчиков времени кадра.
    fs_report_t r;
    if (!fs_last_report(&r)) {
        fb_console_puts("  frame stats: no data yet\n");
        return;
    }
    say(FG_WARN, "Frame (last second):\n");
    fb_console_puts("  fps: ");
    fb_console_print_dec(r.fps_x10 / 10);
    fb_console_putc('.');
    fb_console_print_dec(r.fps_x10 % 10);
    fb_console_puts("   BSP busy: ");
    fb_console_print_dec(r.busy_permille / 10);
    fb_console_putc('.');
    fb_console_print_dec(r.busy_permille % 10);
    fb_console_puts("%   wakeups: ");
    fb_console_print_dec(r.wakeups);
    fb_console_puts("\n  px/frame: ");
    fb_console_print_dec(r.px_per_frame);
    fb_console_puts("   virtio spins/frame: ");
    fb_console_print_dec(r.spins_per_frame);
    fb_console_puts("\n  phase, us avg/max:\n");
    for (int i = 0; i < FS_PHASES; i++) {
        fb_console_puts("    ");
        fb_console_puts(fs_phase_name(i));
        fb_console_puts(": ");
        fb_console_print_dec(r.avg_us[i]);
        fb_console_putc('/');
        fb_console_print_dec(r.max_us[i]);
        fb_console_puts("\n");
    }
}

static void cmd_clock(void) {
    rtc_time_t t;
    rtc_read(&t);
    say(FG_ACCENT, "clock: ");
    // YYYY-MM-DD hh:mm:ss
    fb_console_print_dec(t.year);
    fb_console_putc('-');
    if (t.month < 10) fb_console_putc('0');
    fb_console_print_dec(t.month);
    fb_console_putc('-');
    if (t.day < 10) fb_console_putc('0');
    fb_console_print_dec(t.day);
    fb_console_putc(' ');
    if (t.hour < 10) fb_console_putc('0');
    fb_console_print_dec(t.hour);
    fb_console_putc(':');
    if (t.minute < 10) fb_console_putc('0');
    fb_console_print_dec(t.minute);
    fb_console_putc(':');
    if (t.second < 10) fb_console_putc('0');
    fb_console_print_dec(t.second);
    fb_console_puts("\n");
}

static void cmd_cycle(void) {
    say(FG_WARN, "Cycling system...\n");
    timer_sleep(120);
    power_reboot_with_curtain();
}

static void cmd_freeze(void) {
    say(FG_WARN, "Freezing system...\n");
    timer_sleep(120);
    power_shutdown_with_curtain();
}

// ----------------------------------------------------------------------------
// Этап 45: `Amply ...` — встроенный пакетный менеджер.
// ----------------------------------------------------------------------------
static void amply_print_log(const char* log) {
    int i = 0;
    while (log[i]) {
        // Подсветка: строки [error]/[fail] — красные, [ok]/[done] — зелёные,
        // [skip]/[hint] — жёлтые.  Иначе — обычный.
        int eol = i; while (log[eol] && log[eol] != '\n') eol++;
        int line_len = eol - i;
        uint32_t fg = FG_TEXT;
        if (line_len > 7 && log[i] == '[') {
            const char* tag = log + i + 1;
            if (s_strncmp_(tag, "error", 5) == 0 || s_strncmp_(tag, "bad-", 4) == 0 ||
                s_strncmp_(tag, "miss",  4) == 0)                fg = FG_ERR;
            else if (s_strncmp_(tag, "ok",   2) == 0 ||
                     s_strncmp_(tag, "done", 4) == 0 ||
                     s_strncmp_(tag, "inst", 4) == 0)             fg = FG_OK;
            else if (s_strncmp_(tag, "hint", 4) == 0 ||
                     s_strncmp_(tag, "skip", 4) == 0 ||
                     s_strncmp_(tag, "upgr", 4) == 0)             fg = FG_WARN;
            else                                                  fg = FG_ACCENT;
        }
        fb_console_set_color(fg, BG);
        for (int k = 0; k < line_len; k++) fb_console_putc(log[i + k]);
        fb_console_putc('\n');
        fb_console_set_color(FG_TEXT, BG);
        i = eol;
        if (log[i] == '\n') i++;
    }
}

static void cmd_amply_help(void) {
    say(FG_WARN, "Amply — пакетный менеджер AMPLIFY-OS (этап 45):\n");
    fb_console_puts("  Amply install <pkg>   установить пакет (с зависимостями)\n");
    fb_console_puts("  Amply remove  <pkg>   удалить пакет\n");
    fb_console_puts("  Amply upgrade         обновить все пакеты\n");
    fb_console_puts("  Amply search  <term>  искать в репозитории\n");
    fb_console_puts("  Amply info    <pkg>   показать manifest пакета\n");
    fb_console_puts("  Amply list            список установленных\n");
    fb_console_puts("  Amply verify  <pkg>   проверить целостность\n");
    fb_console_puts("  Amply repo    <url>   задать URL репо\n");
    fb_console_puts("  Amply repo-show       показать текущий URL\n");
}

static void cmd_amply(int argc, char* argv[]) {
    if (argc < 2) { cmd_amply_help(); return; }
    const char* sub = argv[1];

    static char log_buf[4096];

    if (sh_strcmp(sub, "install") == 0) {
        if (argc < 3) { say(FG_ERR, "usage: Amply install <pkg>\n"); return; }
        log_buf[0] = 0;
        int r = amply_install(argv[2], log_buf, sizeof(log_buf));
        amply_print_log(log_buf);
        if (r != 0) say(FG_ERR, "Amply install failed.\n");
    }
    else if (sh_strcmp(sub, "remove") == 0) {
        if (argc < 3) { say(FG_ERR, "usage: Amply remove <pkg>\n"); return; }
        log_buf[0] = 0;
        int r = amply_remove(argv[2], log_buf, sizeof(log_buf));
        amply_print_log(log_buf);
        if (r != 0) say(FG_ERR, "Amply remove failed.\n");
    }
    else if (sh_strcmp(sub, "upgrade") == 0) {
        log_buf[0] = 0;
        amply_upgrade_all(log_buf, sizeof(log_buf));
        amply_print_log(log_buf);
    }
    else if (sh_strcmp(sub, "search") == 0) {
        if (argc < 3) { say(FG_ERR, "usage: Amply search <term>\n"); return; }
        const amply_index_entry_t* res[16];
        int n = amply_search(argv[2], res, 16);
        if (n == 0) { say(FG_DIM, "(no matches)\n"); return; }
        for (int i = 0; i < n; i++) {
            say(FG_OK, res[i]->name);
            fb_console_puts(" ");
            say(FG_WARN, res[i]->version);
            fb_console_puts("  ");
            fb_console_puts(res[i]->summary);
            fb_console_puts("\n");
        }
    }
    else if (sh_strcmp(sub, "info") == 0) {
        if (argc < 3) { say(FG_ERR, "usage: Amply info <pkg>\n"); return; }
        const amply_index_entry_t* idx = amply_repo_find(argv[2]);
        if (!idx) { say(FG_ERR, "Amply: package not in repo\n"); return; }
        const amply_installed_t* inst = amply_db_find(argv[2]);
        say(FG_WARN, "name:    "); fb_console_puts(idx->name);    fb_console_puts("\n");
        say(FG_WARN, "version: "); fb_console_puts(idx->version); fb_console_puts("\n");
        say(FG_WARN, "summary: "); fb_console_puts(idx->summary); fb_console_puts("\n");
        say(FG_WARN, "amf:     "); fb_console_puts(idx->amf_path);fb_console_puts("\n");
        say(FG_WARN, "status:  ");
        if (inst) {
            say(FG_OK, "installed ");
            fb_console_puts(inst->version);
            fb_console_puts(" (");
            fb_console_print_dec(inst->n_files);
            fb_console_puts(" files)\n");
        } else {
            say(FG_DIM, "not installed\n");
        }
    }
    else if (sh_strcmp(sub, "list") == 0) {
        int n = amply_db_count();
        if (n == 0) { say(FG_DIM, "(no packages installed)\n"); return; }
        for (int i = 0; i < n; i++) {
            const amply_installed_t* r = amply_db_at(i);
            say(FG_OK, r->name);
            fb_console_puts(" ");
            say(FG_WARN, r->version);
            fb_console_puts("  ");
            fb_console_print_dec((uint64_t)r->n_files);
            fb_console_puts(" files\n");
        }
    }
    else if (sh_strcmp(sub, "verify") == 0) {
        if (argc < 3) { say(FG_ERR, "usage: Amply verify <pkg>\n"); return; }
        log_buf[0] = 0;
        amply_verify_installed(argv[2], log_buf, sizeof(log_buf));
        amply_print_log(log_buf);
    }
    else if (sh_strcmp(sub, "repo") == 0) {
        if (argc < 3) { say(FG_ERR, "usage: Amply repo <url>\n"); return; }
        if (amply_set_repo_url(argv[2]) < 0) {
            say(FG_ERR, "Amply: failed to persist repo URL\n");
        } else {
            say(FG_OK, "Amply repo URL set: ");
            fb_console_puts(argv[2]);
            fb_console_puts("\n");
        }
    }
    else if (sh_strcmp(sub, "repo-show") == 0) {
        const char* u = amply_repo_url();
        if (u && u[0]) { say(FG_ACCENT, "repo: "); fb_console_puts(u); fb_console_puts("\n"); }
        else            say(FG_DIM, "repo: (offline / initrd)\n");
    }
    else {
        cmd_amply_help();
    }
}

// ----------------------------------------------------------------------------
// Этап 46: `box ...` — sandbox-runtime для изолированного запуска ELF.
//   box run <path> [name]      — запуск с default-профилем (PID/UTS/NET/MNT)
//   box run-strict <path>      — STRICT seccomp (только read/write/exit)
//   box list                   — активные box-задачи
//   box ns                     — список namespace'ов
//   box cg                     — список cgroup'ов и их usage
// ----------------------------------------------------------------------------
static void cmd_box_help(void) {
    say(FG_WARN, "box — sandbox runtime AMPLIFY-OS (этап 46):\n");
    fb_console_puts("  box run <path> [name]     запустить ELF в полной изоляции\n");
    fb_console_puts("  box run-strict <path>     STRICT seccomp (read/write/exit/sigreturn)\n");
    fb_console_puts("  box run-net <path>        как run, но с разрешённой сетью\n");
    fb_console_puts("  box run-compute <path>    compute-only (без I/O и сети)\n");
    fb_console_puts("  box list                  список активных box-задач\n");
    fb_console_puts("  box ns                    список namespace-объектов\n");
    fb_console_puts("  box cg                    список cgroup'ов и метрик\n");
    fb_console_puts("  box test                  встроенный self-test sandbox\n");
}

// Печать лога box_run с подсветкой по тегам [box]/[ok]/[error].
static void box_print_log(const char* log) {
    if (!log) return;
    int i = 0;
    while (log[i]) {
        int eol = i;
        while (log[eol] && log[eol] != '\n') eol++;
        int line_len = eol - i;
        uint32_t fg = FG_TEXT;
        if (line_len > 4 && log[i] == '[') {
            const char* tag = log + i + 1;
            if (s_strncmp_(tag, "ok",    2) == 0) fg = FG_OK;
            else if (s_strncmp_(tag, "error",5) == 0) fg = FG_ERR;
            else if (s_strncmp_(tag, "box",  3) == 0) fg = FG_ACCENT;
        }
        fb_console_set_color(fg, BG);
        for (int k = 0; k < line_len; k++) fb_console_putc(log[i + k]);
        fb_console_putc('\n');
        fb_console_set_color(FG_TEXT, BG);
        i = eol;
        if (log[i] == '\n') i++;
    }
}

static void cmd_box_run_with_profile(int argc, char* argv[], const char* profile) {
    if (argc < 3) {
        say(FG_ERR, "usage: box <run|run-strict|run-net|run-compute> <path> [name]\n");
        return;
    }
    box_spec_t spec;
    box_spec_default(&spec, argv[2], argc >= 4 ? argv[3] : "box");
    // Прописываем выбранный профиль.
    {
        int j = 0;
        while (profile[j] && j < 15) { spec.seccomp_profile[j] = profile[j]; j++; }
        spec.seccomp_profile[j] = 0;
    }
    // Профиль "net" — открываем сетевую видимость, убираем NEWNET.
    if (sh_strcmp(profile, "net") == 0) {
        spec.clone_flags &= ~AOS_CLONE_NEWNET;
    }
    static char log_buf[2048];
    log_buf[0] = 0;
    int tid = box_run(&spec, log_buf, sizeof(log_buf));
    box_print_log(log_buf);
    if (tid < 0) {
        say(FG_ERR, "box: запуск не удался\n");
    } else {
        say(FG_OK, "box: спавн tid=");
        fb_console_print_dec((uint64_t)tid);
        fb_console_puts(", уступаем CPU...\n");
        // Передаём CPU новой задаче. Когда она завершится — вернёмся в shell.
        task_yield();
    }
}

static void cmd_box_list(void) {
    box_listing_t list[8];
    int n = box_list(list, 8);
    if (n == 0) { say(FG_DIM, "(no active sandboxes)\n"); return; }
    say(FG_WARN, "Active sandboxes:\n");
    for (int i = 0; i < n; i++) {
        fb_console_puts("  tid=");
        fb_console_print_dec((uint64_t)list[i].tid);
        fb_console_puts(" vpid=");
        fb_console_print_dec((uint64_t)list[i].visible_pid);
        fb_console_puts(" cg=");
        fb_console_print_dec((uint64_t)list[i].cgroup_id);
        fb_console_puts(" mem=");
        fb_console_print_dec(list[i].mem_used);
        fb_console_puts(" b cpu=");
        fb_console_print_dec(list[i].cpu_total_ns / 1000000ULL);
        fb_console_puts(" ms blocked=");
        fb_console_print_dec((uint64_t)list[i].seccomp_blocked);
        fb_console_puts(" ");
        say(FG_ACCENT, list[i].name);
        fb_console_puts(" ");
        fb_console_puts(list[i].path);
        fb_console_puts("\n");
    }
}

static void cmd_box_ns(void) {
    ns_summary_t s[16];
    int n = ns_get_summaries(s, 16);
    say(FG_WARN, "Namespaces (");
    fb_console_print_dec((uint64_t)n);
    fb_console_puts("):\n");
    static const char* kinds[] = {"PID", "UTS", "MNT", "NET", "USER"};
    for (int i = 0; i < n; i++) {
        fb_console_puts("  ns_id=");
        fb_console_print_dec((uint64_t)s[i].ns_id);
        fb_console_puts(" kind=");
        fb_console_puts(s[i].kind >= 0 && s[i].kind < 5 ? kinds[s[i].kind] : "?");
        fb_console_puts(" ref=");
        fb_console_print_dec((uint64_t)s[i].refcount);
        fb_console_puts(" extra=");
        fb_console_print_dec((uint64_t)s[i].extra);
        fb_console_puts("\n");
    }
}

static void cmd_box_cg(void) {
    int n = cgroup_count();
    say(FG_WARN, "Cgroups (");
    fb_console_print_dec((uint64_t)n);
    fb_console_puts("):\n");
    for (int i = 0; i < n; i++) {
        cgroup_t* c = cgroup_at(i);
        if (!c) continue;
        fb_console_puts("  cg id=");
        fb_console_print_dec((uint64_t)c->id);
        fb_console_puts(" '");
        say(FG_ACCENT, c->name);
        fb_console_puts("' pids=");
        fb_console_print_dec((uint64_t)c->pids_current);
        fb_console_puts("/");
        fb_console_print_dec((uint64_t)c->pids_max);
        fb_console_puts(" mem=");
        fb_console_print_dec(c->mem_current);
        fb_console_puts("/");
        fb_console_print_dec(c->mem_max_bytes);
        fb_console_puts(" cpu=");
        fb_console_print_dec(c->cpu_total_ns / 1000000ULL);
        fb_console_puts(" ms throttles=");
        fb_console_print_dec(c->cpu_throttles + c->mem_oom + c->io_throttles + c->pids_denied);
        fb_console_puts("\n");
    }
}

// box test — встроенный self-test для песочницы (без отдельного ELF):
//   1. Создать cgroup, поставить лимиты, проверить что они применяются.
//   2. Создать seccomp профиль strict, проверить что check блокирует.
//   3. Создать unshare-namespace и проверить, что hostname отделён.
static void cmd_box_test(void) {
    say(FG_WARN, "box test — self-test sandbox-runtime:\n");

    // Test 1 — cgroup.
    cgroup_t* c = cgroup_create(cgroup_root(), "boxtest");
    if (!c) { say(FG_ERR, "  FAIL  cgroup_create\n"); return; }
    cgroup_set_limit_mem(c, 1024);
    int r1 = cgroup_charge_mem(c, 512);
    int r2 = cgroup_charge_mem(c, 1000);    // должен fail (1512 > 1024)
    fb_console_puts("  cgroup mem 512+1000 over 1024 -> r1="); fb_console_print_dec((uint64_t)(uint32_t)r1);
    fb_console_puts(" r2="); fb_console_print_dec((uint64_t)(uint32_t)r2);
    fb_console_puts(" oom_count=");
    fb_console_print_dec(c->mem_oom);
    fb_console_puts("\n");
    if (r1 == 0 && r2 == -1 && c->mem_oom == 1) say(FG_OK, "  PASS  cgroup memory limit\n");
    else                                        say(FG_ERR, "  FAIL  cgroup memory limit\n");
    cgroup_charge_mem(c, -512);
    cgroup_destroy(c);

    // Test 2 — seccomp.
    seccomp_filter_t* f = seccomp_profile("strict");
    if (!f) { say(FG_ERR, "  FAIL  seccomp_profile\n"); return; }
    int sr1 = seccomp_check(f, 0x101);  // WRITE — allowed in strict
    int sr2 = seccomp_check(f, 0x103);  // OPEN  — blocked in strict (KILL)
    fb_console_puts("  seccomp strict: write->"); fb_console_print_dec((uint64_t)(uint32_t)sr1);
    fb_console_puts(" open->"); fb_console_print_dec((uint64_t)(uint32_t)sr2);
    fb_console_puts(" blocked=");
    fb_console_print_dec(f->total_blocked);
    fb_console_puts("\n");
    if (sr1 == 0 && sr2 == -2 && f->total_blocked == 1) say(FG_OK, "  PASS  seccomp strict\n");
    else                                                 say(FG_ERR, "  FAIL  seccomp strict\n");
    seccomp_release(f);

    // Test 3 — namespace unshare.
    ns_set_t* ns_orig = ns_root();
    ns_set_t* ns_new  = ns_unshare(ns_orig, AOS_CLONE_NEWUTS | AOS_CLONE_NEWPID);
    if (!ns_new) { say(FG_ERR, "  FAIL  ns_unshare\n"); return; }
    bool diff_uts = (ns_new->uts != ns_orig->uts);
    bool diff_pid = (ns_new->pid != ns_orig->pid);
    bool same_net = (ns_new->net == ns_orig->net);
    fb_console_puts("  unshare(NEWUTS|NEWPID): uts_diff="); fb_console_puts(diff_uts ? "y" : "n");
    fb_console_puts(" pid_diff="); fb_console_puts(diff_pid ? "y" : "n");
    fb_console_puts(" net_same="); fb_console_puts(same_net ? "y" : "n");
    fb_console_puts("\n");
    if (diff_uts && diff_pid && same_net) say(FG_OK, "  PASS  ns_unshare CoW semantics\n");
    else                                  say(FG_ERR, "  FAIL  ns_unshare CoW semantics\n");
    ns_release(ns_new);

    say(FG_OK, "box test: complete\n");
}

static void cmd_box(int argc, char* argv[]) {
    if (argc < 2) { cmd_box_help(); return; }
    const char* sub = argv[1];
    if      (sh_strcmp(sub, "run")         == 0) cmd_box_run_with_profile(argc, argv, "default");
    else if (sh_strcmp(sub, "run-strict")  == 0) cmd_box_run_with_profile(argc, argv, "strict");
    else if (sh_strcmp(sub, "run-net")     == 0) cmd_box_run_with_profile(argc, argv, "net");
    else if (sh_strcmp(sub, "run-compute") == 0) cmd_box_run_with_profile(argc, argv, "compute");
    else if (sh_strcmp(sub, "list")        == 0) cmd_box_list();
    else if (sh_strcmp(sub, "ns")          == 0) cmd_box_ns();
    else if (sh_strcmp(sub, "cg")          == 0) cmd_box_cg();
    else if (sh_strcmp(sub, "test")        == 0) cmd_box_test();
    else                                          cmd_box_help();
}

// ----------------------------------------------------------------------------
// [этап 48] seal — шифрование системного тома (LUKS1 + blkcrypt).
//   seal / seal status        — том, слоты ключей, активные отображения
//   seal selftest             — LUKS/XTS self-test на RAM-диске
//   seal bench                — скорость PBKDF2 и итерации для 1 с
//   seal passwd <old> <new>   — сменить пароль тома (новый слот, старый стереть)
// ----------------------------------------------------------------------------
static void cmd_seal(int argc, char* argv[]) {
    const char* sub = argc >= 2 ? argv[1] : "status";

    if (sh_strcmp(sub, "status") == 0) {
        static char buf[1536];
        fde_describe(buf, (int)sizeof(buf));
        fb_console_puts(buf);
    } else if (sh_strcmp(sub, "selftest") == 0) {
        say(FG_ACCENT, "seal: LUKS1 + aes-xts-plain64 self-test on a RAM disk...\n");
        bool ok = fde_self_test();
        say(ok ? FG_OK : FG_ERR, ok ? "seal selftest: PASS\n"
                                    : "seal selftest: FAIL (details in serial log)\n");
    } else if (sh_strcmp(sub, "bench") == 0) {
        uint32_t rate = luks_benchmark(LUKS_KEY_BYTES);
        fb_console_puts("PBKDF2-HMAC-SHA256 (64-byte key): ");
        fb_console_print_dec(rate);
        fb_console_puts(" iterations/s\nA new key slot will use ");
        fb_console_print_dec((uint64_t)rate * FDE_ITER_MS / 1000);
        fb_console_puts(" iterations (");
        fb_console_print_dec(FDE_ITER_MS);
        fb_console_puts(" ms on this machine)\n");
    } else if (sh_strcmp(sub, "passwd") == 0) {
        if (argc < 4) { say(FG_WARN, "usage: seal passwd <old> <new>\n"); return; }
        if (!fde_unlocked()) { say(FG_ERR, "seal: the system volume is not encrypted\n"); return; }
        say(FG_ACCENT, "seal: re-keying the volume (about two seconds)...\n");
        int r = fde_change_passphrase(argv[2], argv[3]);
        if (r == LUKS_OK)            say(FG_OK,  "seal: passphrase changed, the old key slot is destroyed\n");
        else if (r == LUKS_BAD_PASS) say(FG_ERR, "seal: wrong passphrase (or the new one is shorter than 4)\n");
        else                         say(FG_ERR, "seal: could not write the key slot\n");
    } else {
        say(FG_WARN, "seal status | selftest | bench | passwd <old> <new>\n");
    }
}

// ----------------------------------------------------------------------------
// Главный диспетчер. Публичный API — используется и shell_run, и GUI-термом.
// ----------------------------------------------------------------------------
void shell_exec_line(char* line) {
    char* argv[16];
    int argc = sh_tokenize(line, argv, 16);
    if (argc == 0) return;

    if      (sh_strcmp(argv[0], "guide")   == 0) cmd_guide();
    else if (sh_strcmp(argv[0], "help")    == 0) cmd_guide();
    else if (sh_strcmp(argv[0], "shine")   == 0) cmd_shine();
    else if (sh_strcmp(argv[0], "clear")   == 0) cmd_shine();
    else if (sh_strcmp(argv[0], "voice")   == 0) cmd_voice(argc, argv);
    else if (sh_strcmp(argv[0], "echo")    == 0) cmd_voice(argc, argv);
    else if (sh_strcmp(argv[0], "scan")    == 0) cmd_scan();
    else if (sh_strcmp(argv[0], "ls")      == 0) cmd_scan();
    else if (sh_strcmp(argv[0], "read")    == 0) cmd_read(argc, argv);
    else if (sh_strcmp(argv[0], "cat")     == 0) cmd_read(argc, argv);
    else if (sh_strcmp(argv[0], "engrave") == 0) cmd_engrave(argc, argv);
    else if (sh_strcmp(argv[0], "matrix")  == 0) cmd_matrix();
    else if (sh_strcmp(argv[0], "embers")  == 0) cmd_embers();
    else if (sh_strcmp(argv[0], "muse")    == 0) cmd_muse(argc, argv);
    else if (sh_strcmp(argv[0], "pulse")   == 0) cmd_pulse(argc, argv);
    else if (sh_strcmp(argv[0], "ping")    == 0) cmd_pulse(argc, argv);
    else if (sh_strcmp(argv[0], "https")   == 0) cmd_https(argc, argv);
    else if (sh_strcmp(argv[0], "https-test") == 0) cmd_https(argc, argv);
    else if (sh_strcmp(argv[0], "disk")    == 0) cmd_disk();
    else if (sh_strcmp(argv[0], "launch")  == 0) cmd_launch(argc, argv);
    else if (sh_strcmp(argv[0], "run")     == 0) cmd_launch(argc, argv);
    else if (sh_strcmp(argv[0], "elfinfo") == 0) cmd_elfinfo(argc, argv);
    else if (sh_strcmp(argv[0], "gpustat") == 0) cmd_gpustat(argc, argv);
    else if (sh_strcmp(argv[0], "gpubench")== 0) cmd_gpubench(argc, argv);
    else if (sh_strcmp(argv[0], "compstat")== 0) cmd_compstat(argc, argv);
    else if (sh_strcmp(argv[0], "weave")   == 0) cmd_weave(argc, argv);
    else if (sh_strcmp(argv[0], "summon")  == 0) cmd_summon(argc, argv);
    else if (sh_strcmp(argv[0], "phantom") == 0) cmd_phantom(argc, argv);
    else if (sh_strcmp(argv[0], "vm")      == 0) cmd_phantom(argc, argv);
    else if (sh_strcmp(argv[0], "rune")    == 0) cmd_rune(argc, argv);
    else if (sh_strcmp(argv[0], "wasm")    == 0) cmd_rune(argc, argv);
    else if (sh_strcmp(argv[0], "Amply")   == 0) cmd_amply(argc, argv);
    else if (sh_strcmp(argv[0], "amply")   == 0) cmd_amply(argc, argv);
    else if (sh_strcmp(argv[0], "box")     == 0) cmd_box(argc, argv);
    else if (sh_strcmp(argv[0], "seal")    == 0) cmd_seal(argc, argv);
    else if (sh_strcmp(argv[0], "shade")   == 0) cmd_shade(argc, argv);
    else if (sh_strcmp(argv[0], "clock")   == 0) cmd_clock();
    else if (sh_strcmp(argv[0], "date")    == 0) cmd_clock();
    else if (sh_strcmp(argv[0], "cycle")   == 0) cmd_cycle();
    else if (sh_strcmp(argv[0], "reboot")  == 0) cmd_cycle();
    else if (sh_strcmp(argv[0], "freeze")  == 0) cmd_freeze();
    else if (sh_strcmp(argv[0], "halt")    == 0) cmd_freeze();
    else if (sh_strcmp(argv[0], "exit")    == 0) cmd_freeze();
    else {
        say(FG_ERR, argv[0]);
        say(FG_ERR, ": unknown command (try `guide`)\n");
    }
}

static void shell_prompt(void) {
    fb_console_set_color(FG_ACCENT, BG);
    fb_console_puts("amplify");
    fb_console_set_color(FG_WARN, BG);
    fb_console_puts(" > ");
    fb_console_set_color(FG_TEXT, BG);
}

void shell_run(void) {
    fb_console_reset();

    fb_console_set_color(FG_ACCENT, BG);
    fb_console_puts("\n");
    fb_console_puts("        /\\        \n");
    fb_console_puts("       /  \\       \n");
    fb_console_puts("      / /\\ \\      AMPLIFY-OS x86_64 — interactive shell\n");
    fb_console_puts("     / /  \\ \\     18 commands available. Type `guide`.\n");
    fb_console_puts("    / /____\\ \\    \n");
    fb_console_puts("   /__________\\   \n");
    fb_console_puts("\n");
    fb_console_set_color(FG_TEXT, BG);

    char line[LINE_MAX];
    int  pos = 0;

    shell_prompt();
    for (;;) {
        char c = keyboard_wait_char();

        if (c == '\n' || c == '\r') {
            fb_console_putc('\n');
            line[pos] = 0;
            shell_exec_line(line);
            pos = 0;
            shell_prompt();
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                fb_console_putc('\b');
                fb_console_putc(' ');
                fb_console_putc('\b');
            }
        } else if (c >= 32 && c < 127 && pos < LINE_MAX - 1) {
            line[pos++] = c;
            fb_console_putc(c);
        }
    }
}

// sh_strlen держим в файле на будущее (этап 37.6c-2: pipe-cmd, history).
__attribute__((used)) static int sh_keepalive(const char* s) { return sh_strlen(s); }

// ============================================================================
// pipecmds.c — Реализация команд-канальников MyOS shell (Этап 29)
// ============================================================================

#include "pipecmds.h"
#include "sh_io.h"

#include "../vga.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../ipc/signal.h"
#include "../ipc/unix_socket.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// Мини-хелперы
// ============================================================================

static int  pc_strlen(const char* s) {
    int n = 0; while (s && s[n]) n++; return n;
}
static int  pc_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static bool pc_contains(const char* haystack, const char* needle) {
    if (!needle || !needle[0]) return true;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (haystack[i + j] && needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return true;
    }
    return false;
}

// Проверка: пришёл ли SIG_INT/SIG_KILL текущей команде. Используется в циклах
// длинных команд, чтобы они умели прерываться по hush/smite.
static bool pc_should_stop(void) {
    sh_ctx_t* ctx = sh_ctx_get();
    if (!ctx || ctx->pid <= 0) return false;
    if (signal_pending(ctx->pid, SIG_KILL)) return true;
    if (signal_pending(ctx->pid, SIG_INT))  return true;
    return false;
}

// ============================================================================
// spill <путь> — «вылить» содержимое файла в stdout (≈ cat)
// ============================================================================

void cmd_spill(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Ispol'zovanie: spill <put'>");
        return;
    }

    int fd = vfs_open_file(args[1], VFS_O_RDONLY);
    if (fd < 0) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("Fajl ne najden.");
        return;
    }

    char buf[129];
    int  bytes;
    sh_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    while ((bytes = vfs_read_file(fd, buf, 128)) > 0) {
        if (pc_should_stop()) break;
        for (int i = 0; i < bytes; i++) sh_putc(buf[i]);
    }
    vfs_close_file(fd);
}

// ============================================================================
// sift <подстрока> — оставить только строки, содержащие подстроку (≈ grep)
// ============================================================================

void cmd_sift(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Ispol'zovanie: sift <podstroka>  (rabotaet s pipe)");
        return;
    }
    sh_ctx_t* ctx = sh_ctx_get();
    if (!ctx->in_pipe) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("sift trebuet stdin (ispol'zujte cherez pipe '|').");
        return;
    }

    const char* needle = args[1];
    char        line[512];
    while (sh_next_line(line, sizeof(line))) {
        if (pc_should_stop()) break;
        if (pc_contains(line, needle)) {
            sh_puts(line);
            sh_putc('\n');
        }
    }
}

// ============================================================================
// holler <текст...> — однократный «крик» в stdout (≈ echo)
// ============================================================================

void cmd_holler(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    sh_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (int i = 1; i < argc; i++) {
        if (i > 1) sh_putc(' ');
        sh_puts(args[i]);
    }
    sh_putc('\n');
}

// ============================================================================
// tally — подсчитать строки stdin (≈ wc -l)
// ============================================================================

void cmd_tally(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    (void)argc; (void)args;
    sh_ctx_t* ctx = sh_ctx_get();
    if (!ctx->in_pipe) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("tally trebuet stdin (ispol'zujte cherez pipe '|').");
        return;
    }

    uint32_t lines = 0, bytes = 0;
    char     line[512];
    while (sh_next_line(line, sizeof(line))) {
        if (pc_should_stop()) break;
        lines++;
        bytes += (uint32_t)pc_strlen(line) + 1;
    }

    sh_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    sh_puts("Strok: ");
    sh_print_dec(lines);
    sh_puts("  Bajt: ");
    sh_print_dec(bytes);
    sh_putc('\n');
}

// ============================================================================
// crown <N> — взять первые N строк stdin (≈ head -n N)
// ============================================================================

void cmd_crown(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Ispol'zovanie: crown <N>");
        return;
    }
    sh_ctx_t* ctx = sh_ctx_get();
    if (!ctx->in_pipe) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("crown trebuet stdin (ispol'zujte cherez pipe '|').");
        return;
    }

    // Парсим N
    int n = 0;
    for (int i = 0; args[1][i]; i++) {
        if (args[1][i] < '0' || args[1][i] > '9') { n = 0; break; }
        n = n * 10 + (args[1][i] - '0');
    }
    if (n <= 0) n = 10;

    char line[512];
    int  printed = 0;
    while (printed < n && sh_next_line(line, sizeof(line))) {
        if (pc_should_stop()) break;
        sh_puts(line);
        sh_putc('\n');
        printed++;
    }
}

// ============================================================================
// sole — оставить только уникальные подряд идущие строки (≈ uniq)
// ============================================================================

void cmd_sole(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    (void)argc; (void)args;
    sh_ctx_t* ctx = sh_ctx_get();
    if (!ctx->in_pipe) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("sole trebuet stdin (ispol'zujte cherez pipe '|').");
        return;
    }

    char prev[512]; prev[0] = '\0';
    char curr[512];
    bool first = true;
    while (sh_next_line(curr, sizeof(curr))) {
        if (pc_should_stop()) break;
        if (first || pc_strcmp(prev, curr) != 0) {
            sh_puts(curr);
            sh_putc('\n');
            // copy curr → prev
            int i = 0;
            while (curr[i] && i < (int)sizeof(prev) - 1) { prev[i] = curr[i]; i++; }
            prev[i] = '\0';
            first = false;
        }
    }
}

// ============================================================================
// plumes — список зарегистрированных задач (≈ ps)
// ============================================================================

void cmd_plumes(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    (void)argc; (void)args;

    signal_task_t snap[SIGNAL_MAX_TASKS];
    int n = signal_list(snap, SIGNAL_MAX_TASKS);

    sh_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    sh_println("=== Plumes — zaregistrirovannye zadachi ===");

    if (n == 0) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("(net aktivnyh zadach)");
        return;
    }

    sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    sh_println("  PID  IMYa                     SIGNALY");
    for (int i = 0; i < n; i++) {
        sh_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        sh_puts("  ");
        sh_print_dec((uint32_t)snap[i].pid);
        sh_puts("    ");
        sh_puts(snap[i].name);
        // Выравнивание имени
        int nlen = pc_strlen(snap[i].name);
        for (int j = nlen; j < 24; j++) sh_putc(' ');

        if (snap[i].signals == 0) {
            sh_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
            sh_puts("-");
        } else {
            sh_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
            if (snap[i].signals & SIG_MASK_INT)  sh_puts("INT ");
            if (snap[i].signals & SIG_MASK_KILL) sh_puts("KILL ");
        }
        sh_putc('\n');
    }
}

// ============================================================================
// hush <pid> — мягко попросить остановиться (SIG_INT)
// ============================================================================

static int pc_parse_int(const char* s) {
    int v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

void cmd_hush(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Ispol'zovanie: hush <pid>");
        return;
    }
    int pid = pc_parse_int(args[1]);
    if (pid <= 0) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("Nekorrektnyj pid.");
        return;
    }
    if (signal_send(pid, SIG_INT)) {
        sh_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        sh_puts("Hush -> pid ");
        sh_print_dec((uint32_t)pid);
        sh_putc('\n');
    } else {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("Net takoj zadachi.");
    }
}

// ============================================================================
// smite <pid> — безусловно убить (SIG_KILL)
// ============================================================================

void cmd_smite(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Ispol'zovanie: smite <pid>");
        return;
    }
    int pid = pc_parse_int(args[1]);
    if (pid <= 0) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("Nekorrektnyj pid.");
        return;
    }
    if (signal_send(pid, SIG_KILL)) {
        sh_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        sh_puts("Smite -> pid ");
        sh_print_dec((uint32_t)pid);
        sh_putc('\n');
    } else {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("Net takoj zadachi.");
    }
}

// ============================================================================
// bind <name> — демо: создать LISTENER unix-domain socket
// ============================================================================

void cmd_bind(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Ispol'zovanie: bind <imya-soketa>");
        return;
    }
    int fd = usock_bind(args[1]);
    if (fd < 0) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("Ne udalos' sozdat' listener (zanyato ili pereploneno).");
        return;
    }
    usock_listen(fd);
    sh_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    sh_puts("Listener sozdan: '");
    sh_puts(args[1]);
    sh_puts("' (fd=");
    sh_print_dec((uint32_t)fd);
    sh_println(")");
}

// ============================================================================
// speak <name> <текст...> — демо: connect к слушателю и послать сообщение
// ============================================================================

void cmd_speak(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 3) {
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Ispol'zovanie: speak <imya> <tekst...>");
        return;
    }

    int cli = usock_connect(args[1]);
    if (cli < 0) {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_println("Ne udalos' podklyuchit'sya — listener ne najden.");
        return;
    }

    // Сервер немедленно accept (наша модель — без preempt'a)
    // Слушатель найдём по имени через bind-таблицу: упростим — ищем сервер
    // по тому же имени. usock_accept ждёт fd слушателя. Нам он недоступен из
    // этой функции, поэтому делаем «инлайн»-демо: клиент сам пишет/читает,
    // и тут же сам делает recv, чтобы продемонстрировать pipe. В реальной
    // программе сервер был бы отдельным таском.
    for (int i = 2; i < argc; i++) {
        if (i > 2) usock_send(cli, " ", 1);
        usock_send(cli, args[i], (size_t)pc_strlen(args[i]));
    }
    usock_send(cli, "\n", 1);

    sh_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    sh_puts("Otpravleno cherez '");
    sh_puts(args[1]);
    sh_puts("', fd=");
    sh_print_dec((uint32_t)cli);
    sh_putc('\n');

    // Не закрываем — пусть сервер заберёт через accept+recv.
}

// ============================================================================
// Диспетчер
// ============================================================================

bool pipecmds_is_known(const char* name) {
    return  pc_strcmp(name, "spill") == 0 ||
            pc_strcmp(name, "sift")  == 0 ||
            pc_strcmp(name, "holler")== 0 ||
            pc_strcmp(name, "tally") == 0 ||
            pc_strcmp(name, "crown") == 0 ||
            pc_strcmp(name, "sole")  == 0 ||
            pc_strcmp(name, "plumes")== 0 ||
            pc_strcmp(name, "hush")  == 0 ||
            pc_strcmp(name, "smite") == 0 ||
            pc_strcmp(name, "bind")  == 0 ||
            pc_strcmp(name, "speak") == 0;
}

bool pipecmds_dispatch(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc <= 0) return false;
    const char* name = args[0];
    if (pc_strcmp(name, "spill")  == 0) { cmd_spill(argc, args);  return true; }
    if (pc_strcmp(name, "sift")   == 0) { cmd_sift(argc, args);   return true; }
    if (pc_strcmp(name, "holler") == 0) { cmd_holler(argc, args); return true; }
    if (pc_strcmp(name, "tally")  == 0) { cmd_tally(argc, args);  return true; }
    if (pc_strcmp(name, "crown")  == 0) { cmd_crown(argc, args);  return true; }
    if (pc_strcmp(name, "sole")   == 0) { cmd_sole(argc, args);   return true; }
    if (pc_strcmp(name, "plumes") == 0) { cmd_plumes(argc, args); return true; }
    if (pc_strcmp(name, "hush")   == 0) { cmd_hush(argc, args);   return true; }
    if (pc_strcmp(name, "smite")  == 0) { cmd_smite(argc, args);  return true; }
    if (pc_strcmp(name, "bind")   == 0) { cmd_bind(argc, args);   return true; }
    if (pc_strcmp(name, "speak")  == 0) { cmd_speak(argc, args);  return true; }
    return false;
}

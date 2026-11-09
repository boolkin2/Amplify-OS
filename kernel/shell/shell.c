// ============================================================================
// shell.c - Командная оболочка MyOS
// ============================================================================

#include "shell.h"
#include "../vga.h"
#include "../io.h"
#include "../drivers/keyboard.h"
#include "../drivers/timer.h"
#include "../memory/pmm.h"
#include "../memory/heap.h"
#include "../memory/paging.h"
#include "../memory/memory.h"
#include "../fs/vfs.h"
#include "../fs/initrd.h"
#include "../drivers/ata.h"
#include "../loader/elf.h"
#include "../loader/dynlink.h"
#include "../task/task.h"

// --- Этап 30: SMP & cpu stats ---
#include "../arch/i386/cpu_stats.h"
#include "../arch/i386/acpi.h"
#include "../arch/i386/lapic.h"
#include "../arch/i386/ioapic.h"
#include "../arch/i386/smp.h"

// --- Этап 29: IPC ---
#include "sh_io.h"
#include "pipecmds.h"
#include "../ipc/pipe.h"
#include "../ipc/signal.h"
#include "../ipc/unix_socket.h"

// ============================================================================
// Вспомогательные функции для работы со строками
// ============================================================================

static int shell_strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int shell_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

static void shell_strcpy(char* dst, const char* src) {
    while (*src) { *dst++ = *src++; }
    *dst = '\0';
}


// ============================================================================
// История команд
// ============================================================================

static char history[SHELL_HISTORY_SIZE][SHELL_MAX_INPUT];
static int history_count = 0;
static int history_pos = 0;

static void history_add(const char* cmd) {
    if (shell_strlen(cmd) == 0) return;
    if (history_count > 0 && shell_strcmp(history[(history_count - 1) % SHELL_HISTORY_SIZE], cmd) == 0) return;
    shell_strcpy(history[history_count % SHELL_HISTORY_SIZE], cmd);
    history_count++;
    history_pos = history_count;
}

// ============================================================================
// Парсинг командной строки
// ============================================================================

static int parse_args(char* input, char args[][SHELL_MAX_ARG_LEN]) {
    int argc = 0;
    int i = 0;
    while (input[i] && argc < SHELL_MAX_ARGS) {
        while (input[i] == ' ') i++;
        if (!input[i]) break;
        int j = 0;
        while (input[i] && input[i] != ' ' && j < SHELL_MAX_ARG_LEN - 1) {
            args[argc][j++] = input[i++];
        }
        args[argc][j] = '\0';
        argc++;
    }
    return argc;
}

// ============================================================================
// Чтение строки ввода с поддержкой истории
// ============================================================================

static int shell_readline(char* buffer, int max_len) {
    int pos = 0;
    buffer[0] = '\0';
    history_pos = history_count;

    while (1) {
        char c = keyboard_getchar();

        if (c == '\n') {
            vga_putchar('\n');
            buffer[pos] = '\0';
            return pos;
        } else if (c == '\b') {
            if (pos > 0) {
                pos--;
                buffer[pos] = '\0';
                vga_putchar('\b');
            }
        } else if (c >= 32 && c < 127) {
            if (pos < max_len - 1) {
                buffer[pos++] = c;
                buffer[pos] = '\0';
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                vga_putchar(c);
            }
        }
    }
}

// ============================================================================
// Вывод приглашения командной строки
// ============================================================================

static void shell_prompt(void) {
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("myos");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts(":");
    vga_set_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
    vga_puts("/");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_puts("$ ");
}

// ============================================================================
// КОМАНДА: guide — список команд и справка
// ============================================================================

static void cmd_guide(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("=== AMPLIFY-OS Shell - Spravka po komandam ===");
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  guide          ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Pokazat' etu spravku");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  shine          ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Ochistit' ekran");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  voice <tekst>  ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Vyvesti tekst na ekran");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  pulse          ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Vremya raboty sistemy");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  embers         ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Monitor zagruzki yader CPU (htop-analog)");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  matrix         ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Informatsiya o pamyati");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  scan           ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Spisok fajlov (InitRD)");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  read <fajl>    ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Vyvesti soderzhimoe fajla");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  engrave <f> <t>");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Zapisat' tekst t v fajl f");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  delve <dir>    ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Pokazat' soderzhimoe kataloga");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  carve <dir>    ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Sozdat' katalog");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  erase <fajl>   ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Udalit' fajl");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  disk           ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Info o diske");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  cycle          ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Perezagruzka sistemy");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  freeze         ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Vyklyuchenie (QEMU ACPI)");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  shade <tsvet>  ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Smena tsveta teksta");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  launch <elf>   ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Zapustit' ELF-programmu");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  lsmod          ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Spisok zagr. bibliotek");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  elfinfo <fajl> ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Info o ELF fajle");

    vga_println("");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_println("--- Etap 29: pipe komandy (rabotayut s '|') ---");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  spill <fajl>   ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- 'Vylit'' soderzhimoe fajla v stdout");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  sift <text>    ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Otfil'trovat' stroki stdin po tekstu");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  holler <text>  ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Vyvesti tekst (analog echo)");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  tally          ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Podschet strok i bajt iz stdin");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  crown <N>      ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Pervye N strok stdin");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  sole           ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Tol'ko unikal'nye podryad stroki");

    vga_println("");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_println("--- Etap 29: signaly i zadachi ---");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  plumes         ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Spisok zaregistrirovannyh zadach");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  hush <pid>     ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Otpravit' SIG_INT (myagkaya pros'ba)");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  smite <pid>    ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Otpravit' SIG_KILL (bezuslovno)");

    vga_println("");
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_println("--- Etap 29: Unix Domain Sockets (demo) ---");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  bind <name>    ");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("- Sozdat' listener-soket s imenem");

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_puts("  speak <name> <text>");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println(" - Connect i otpravka teksta");

    vga_println("");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("Primery konvejera:");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("  spill /disk/log.txt | sift error");
    vga_println("  spill /disk/log.txt | crown 5");
    vga_println("  holler hello | tally");

    vga_println("");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("Tsveta dlya 'shade': black, blue, green, cyan,");
    vga_println("  red, magenta, brown, lgrey, dgrey, lblue,");
    vga_println("  lgreen, lcyan, lred, lmagenta, yellow, white");
}

// ============================================================================
// КОМАНДА: shine — очистка экрана
// ============================================================================

static void cmd_shine(void) {
    vga_clear();
}

// ============================================================================
// КОМАНДА: voice <текст> — вывод текста
// ============================================================================

static void cmd_voice(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_println("Ispol'zovanie: voice <tekst>");
        return;
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (int i = 1; i < argc; i++) {
        if (i > 1) vga_puts(" ");
        vga_puts(args[i]);
    }
    vga_println("");
}

// ============================================================================
// КОМАНДА: pulse — время работы системы
// ============================================================================

static void cmd_pulse(void) {
    uint32_t uptime_sec = timer_get_uptime_sec();
    uint32_t hours = uptime_sec / 3600;
    uint32_t minutes = (uptime_sec % 3600) / 60;
    uint32_t seconds = uptime_sec % 60;
    uint64_t ticks = timer_get_ticks();

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("=== Pulse - Vremya raboty sistemy ===");
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Uptime:     ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(hours);
    vga_puts("h ");
    vga_print_dec(minutes);
    vga_puts("m ");
    vga_print_dec(seconds);
    vga_println("s");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Tiki:       ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec((uint32_t)ticks);
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Chastota:   ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(timer_get_frequency());
    vga_println(" Hz");
}

// ============================================================================
// КОМАНДА: embers — живой монитор активности ядер CPU (htop-аналог).
// ----------------------------------------------------------------------------
// Каждое ядро у нас называется «уголёк» (ember). Команда снимает дельту
// busy/idle ticks за окно ~250 мс и показывает её в виде ASCII-бара.
// Обновляется до тех пор, пока пользователь не нажмёт 'q' или Esc.
// ============================================================================

static void embers_draw_bar(uint32_t pct) {
    if (pct > 100) pct = 100;
    int filled = (int)(pct / 5);   // 20 колонок шкалы
    vga_puts("[");

    // Цвет «угля» — от тёмно-красного к ярко-жёлтому в зависимости от %.
    uint8_t color;
    if      (pct >= 75) color = VGA_COLOR_LIGHT_RED;
    else if (pct >= 50) color = VGA_COLOR_LIGHT_BROWN;   // янтарный
    else if (pct >= 25) color = VGA_COLOR_BROWN;
    else                color = VGA_COLOR_DARK_GREY;

    vga_set_color(color, VGA_COLOR_BLACK);
    for (int i = 0; i < filled; i++) vga_puts("#");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    for (int i = filled; i < 20; i++) vga_puts(".");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("]");
}

static void cmd_embers(void) {
    // Снимаем стартовый снимок per-CPU счётчиков.
    uint32_t prev_busy[MAX_CPUS], prev_idle[MAX_CPUS];
    for (int i = 0; i < MAX_CPUS; i++) {
        prev_busy[i] = cpu_stats[i].busy_ticks;
        prev_idle[i] = cpu_stats[i].idle_ticks;
    }

    int frame = 0;
    while (1) {
        // Окно сэмплирования.
        timer_sleep(250);

        vga_clear();
        vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
        vga_println("============================================");
        vga_println("        EMBERS — monitor yader CPU         ");
        vga_println("============================================");

        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("Yader v sisteme: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_print_dec(num_cpus);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("   Probuzhdeno: ");
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_print_dec(smp_cpus_started);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("   IOAPIC: ");
        if (ioapic_present()) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_puts("yes");
        } else {
            vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
            vga_puts("no");
        }
        vga_println("");

        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_puts("Frame: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_print_dec(frame);
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_println("    [q] vyhod");
        vga_println("");

        for (int i = 0; i < MAX_CPUS; i++) {
            if (!cpu_stats[i].online) continue;

            uint32_t busy_now = cpu_stats[i].busy_ticks;
            uint32_t idle_now = cpu_stats[i].idle_ticks;
            uint32_t db = busy_now - prev_busy[i];
            uint32_t di = idle_now - prev_idle[i];
            prev_busy[i] = busy_now;
            prev_idle[i] = idle_now;

            uint32_t total = db + di;
            uint32_t pct = total ? (db * 100u) / total : 0;

            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            vga_puts("Yadro ");
            vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
            vga_print_dec(i);
            vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
            vga_puts(" (LAPIC ");
            vga_print_dec(cpu_stats[i].lapic_id);
            vga_puts(") ");
            if (i == 0) {
                vga_set_color(VGA_COLOR_LIGHT_MAGENTA, VGA_COLOR_BLACK);
                vga_puts("[BSP] ");
            } else {
                vga_set_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
                vga_puts("[AP]  ");
            }
            embers_draw_bar(pct);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts(" ");
            if (pct < 10) vga_puts("  ");
            else if (pct < 100) vga_puts(" ");
            vga_print_dec(pct);
            vga_println("%");
        }

        vga_println("");
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_println("---");
        vga_println("Cherta — tsvet pokazyvaet zhar yadra:");
        vga_println("  serye - dremlet, koritsa - tlevaet,");
        vga_println("  yantar' - gorit, krasnyj - rabotaet polno.");

        // Опрос клавиатуры — выходим по 'q' или Esc (0x1B).
        if (keyboard_has_key()) {
            char c = keyboard_getchar_nonblock();
            if (c == 'q' || c == 'Q' || c == 0x1B || c == '\n') break;
        }
        frame++;
        if (frame > 240) break;   // ~60 секунд — авто-выход
    }

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("");
}

// ============================================================================
// КОМАНДА: matrix — информация о памяти
// ============================================================================

static void cmd_matrix(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("=== Matrix - Informatsiya o pamyati ===");
    vga_println("");

    // Физическая память (PMM)
    pmm_info_t pmm = pmm_get_info();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_println("  [ Fizicheskaya pamyat' (PMM) ]");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Vsego:        ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(pmm.total_memory / MB);
    vga_println(" MB");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Svobodno:     ");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print_dec(pmm.free_memory / MB);
    vga_println(" MB");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Zanyato:      ");
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_print_dec(pmm.used_memory / MB);
    vga_println(" MB");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Stranits:     ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(pmm.total_pages);
    vga_puts(" (sv: ");
    vga_print_dec(pmm.free_pages);
    vga_puts(", zan: ");
    vga_print_dec(pmm.used_pages);
    vga_println(")");
    vga_println("");

    // Виртуальная память (Paging)
    paging_info_t pg = paging_get_info();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_println("  [ Virtual'naya pamyat' (Paging) ]");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Paging:       ");
    vga_set_color(pg.enabled ? VGA_COLOR_LIGHT_GREEN : VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_println(pg.enabled ? "Vklyuchen" : "Otklyuchen");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Otobrazh:     ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(pg.pages_mapped);
    vga_puts(" stranits (");
    vga_print_dec(pg.pages_mapped * 4);
    vga_println(" KB)");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Page Faults:  ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(pg.page_faults);
    vga_println("");
    vga_println("");

    // Куча (Heap)
    heap_info_t hi = heap_get_info();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_println("  [ Kucha (Heap) ]");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Razmer:       ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(hi.total_size / KB);
    vga_println(" KB");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Svobodno:     ");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_print_dec(hi.free_size / KB);
    vga_println(" KB");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Zanyato:      ");
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_print_dec(hi.used_size / KB);
    vga_println(" KB");
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Blokov:       ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(hi.block_count);
    vga_puts(" (sv: ");
    vga_print_dec(hi.free_blocks);
    vga_puts(", zan: ");
    vga_print_dec(hi.used_blocks);
    vga_println(")");
}

// ============================================================================
// КОМАНДА: scan — список файлов
// ============================================================================

static void cmd_scan(void) {
    vfs_node_t* root = vfs_get_root();
    if (!root) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Fajlovaya sistema ne zagruzhena!");
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("=== Scan - Spisok fajlov ===");
    vga_println("");

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  Imya                          Razmer");
    vga_println("");
    vga_puts("  ------------------------------------");
    vga_println("");

    uint32_t idx = 0;
    uint32_t total_size = 0;
    vfs_dirent_t* dirent;

    while ((dirent = vfs_readdir(root, idx)) != NULL) {
        vfs_node_t* fnode = vfs_finddir(root, dirent->name);

        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_puts("  ");
        vga_puts(dirent->name);

        int nlen = shell_strlen(dirent->name);
        for (int j = nlen; j < 30; j++) vga_puts(" ");

        if (fnode) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            vga_print_dec(fnode->length);
            vga_puts(" B");
            total_size += fnode->length;
        }
        vga_println("");
        idx++;
    }

    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_puts("  ------------------------------------");
    vga_println("");
    vga_puts("  Itogo: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_print_dec(idx);
    vga_puts(" fajl(ov), ");
    vga_print_dec(total_size);
    vga_println(" bajt");
}

// ============================================================================
// КОМАНДА: read <файл> — чтение файла
// ============================================================================

static void cmd_read(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_println("Ispol'zovanie: read <put'>");
        return;
    }

    int fd = vfs_open_file(args[1], VFS_O_RDONLY);
    if (fd < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Fajl ne najden.");
        return;
    }
    
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("--- Soderzhimoe ---");
    vga_println("");

    char buf[129];
    int bytes;
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    while ((bytes = vfs_read_file(fd, buf, 128)) > 0) {
        buf[bytes] = '\0';
        vga_puts(buf);
    }
    vga_println("");
    vfs_close_file(fd);
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("--- konets fajla ---");
}

static void cmd_delve(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    const char* path = (argc > 1) ? args[1] : "/disk";
    vfs_node_t* dir = vfs_resolve_path(path);
    if (!dir) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Katalog ne najden.");
        return;
    }
    if (!(dir->flags & VFS_DIRECTORY)) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Eto ne katalog.");
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("--- Soderzhimoe "); vga_puts(path); vga_println(" ---");
    
    int idx = 0;
    vfs_dirent_t* d;
    while ((d = vfs_readdir(dir, idx)) != NULL) {
        vfs_node_t* child = vfs_finddir(dir, d->name);
        if (child) {
            if (child->flags & VFS_DIRECTORY) vga_set_color(VGA_COLOR_LIGHT_BLUE, VGA_COLOR_BLACK);
            else vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            vga_puts("  "); vga_puts(d->name);
            int len = shell_strlen(d->name);
            for (int i = len; i < 20; i++) vga_puts(" ");
            if (!(child->flags & VFS_DIRECTORY)) {
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                vga_print_dec(child->length); vga_puts(" B");
            } else {
                vga_puts("<DIR>");
            }
            vga_println("");
        }
        idx++;
    }
}

static void cmd_engrave(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 3) {
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_println("Ispol'zovanie: engrave <put'> <tekst...>");
        return;
    }
    
    int fd = vfs_open_file(args[1], VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Oshibka sozdaniya/otkrytiya fajla.");
        return;
    }
    
    for (int i = 2; i < argc; i++) {
        vfs_write_file(fd, args[i], shell_strlen(args[i]));
        if (i < argc - 1) vfs_write_file(fd, " ", 1);
    }
    vfs_write_file(fd, "\n", 1);
    vfs_close_file(fd);
    
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    vga_println("Fajl uspeshno zapisan.");
}

static void cmd_carve(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        vga_println("Ispol'zovanie: carve <put'>");
        return;
    }
    int rc = vfs_mkdir(args[1]);
    if (rc == VFS_OK) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_println("Katalog sozdan.");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Oshibka sozdaniya kataloga.");
    }
}

static void cmd_erase(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        vga_println("Ispol'zovanie: erase <put'>");
        return;
    }
    int rc = vfs_unlink(args[1]);
    if (rc == VFS_OK) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_println("Udaleno.");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Oshibka udaleniya.");
    }
}

static void cmd_disk(void) {
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("=== Disk Info ===");
    
    if (ata_is_present(0)) {
        const ata_device_t* dev = ata_get_device(0);
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("  Drive 0: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        vga_print_dec(dev->size_mb); vga_println(" MB");
    } else {
        vga_println("  Drive 0 ne najden.");
    }
}

// ============================================================================
// КОМАНДА: cycle — перезагрузка
// ============================================================================

static void cmd_cycle(void) {
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_println("Perezagruzka sistemy...");

    // Perezagruzka cherez kontroller klaviatury 8042
    // Komanda 0xFE na port 0x64 vyzyvaet apparatnyj sbros
    uint8_t good = 0x02;
    while (good & 0x02) {
        good = inb(0x64);
    }
    outb(0x64, 0xFE);

    // Esli ne srabotalo - beskonechnyj cikl
    while (1) {
        __asm__ volatile("hlt");
    }
}

// ============================================================================
// KOMANDA: freeze - vyklyuchenie cherez QEMU ACPI
// ============================================================================

static void cmd_freeze(void) {
    vga_set_color(VGA_COLOR_LIGHT_BROWN, VGA_COLOR_BLACK);
    vga_println("Vyklyuchenie sistemy...");

    // QEMU shutdown via 0x604 (ACPI)
    outw(0x604, 0x2000);
    // Fallback for older QEMU (port 0xB004)
    outw(0xB004, 0x2000);

    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_println("Vyklyuchenie ne udalos'. Ispol'zujte Ctrl+C.");
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

// ============================================================================
// KOMANDA: shade <tsvet> - smena tsveta teksta
// ============================================================================

static vga_color_t current_text_color = VGA_COLOR_WHITE;

static void cmd_shade(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_println("Ispol'zovanie: shade <tsvet>");
        vga_println("Tsveta: black, blue, green, cyan, red, magenta,");
        vga_println("  brown, lgrey, dgrey, lblue, lgreen, lcyan,");
        vga_println("  lred, lmagenta, yellow, white");
        return;
    }

    struct { const char* name; vga_color_t color; } colors[] = {
        {"black",    VGA_COLOR_BLACK},
        {"blue",     VGA_COLOR_BLUE},
        {"green",    VGA_COLOR_GREEN},
        {"cyan",     VGA_COLOR_CYAN},
        {"red",      VGA_COLOR_RED},
        {"magenta",  VGA_COLOR_MAGENTA},
        {"brown",    VGA_COLOR_BROWN},
        {"lgrey",    VGA_COLOR_LIGHT_GREY},
        {"dgrey",    VGA_COLOR_DARK_GREY},
        {"lblue",    VGA_COLOR_LIGHT_BLUE},
        {"lgreen",   VGA_COLOR_LIGHT_GREEN},
        {"lcyan",    VGA_COLOR_LIGHT_CYAN},
        {"lred",     VGA_COLOR_LIGHT_RED},
        {"lmagenta", VGA_COLOR_LIGHT_MAGENTA},
        {"yellow",   VGA_COLOR_LIGHT_BROWN},
        {"white",    VGA_COLOR_WHITE},
    };

    int num_colors = sizeof(colors) / sizeof(colors[0]);
    for (int i = 0; i < num_colors; i++) {
        if (shell_strcmp(args[1], colors[i].name) == 0) {
            current_text_color = colors[i].color;
            vga_set_color(current_text_color, VGA_COLOR_BLACK);
            vga_puts("Tsvet izmenen na: ");
            vga_println(colors[i].name);
            return;
        }
    }

    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    vga_puts("Neizvestnyj tsvet: ");
    vga_println(args[1]);
    vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    vga_println("Vvedite 'shade' bez argumentov dlya spiska tsvetov.");
}

// ============================================================================
// KOMANDA: launch <put'> - zapusk ELF-programmy
// ============================================================================

static void cmd_launch(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_println("Ispol'zovanie: launch <put' k ELF fajlu>");
        vga_println("Primer: launch /disk/nexus/bin/hello.elf");
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_puts("Zapusk ELF: ");
    vga_println(args[1]);

    task_t* task = task_exec_elf(args[1]);
    if (task) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        vga_puts("Programma zapuschena, PID=");
        vga_print_dec(task->id);
        vga_println("");
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Oshibka zapuska programmy!");
    }
}

// ============================================================================
// KOMANDA: lsmod - spisok zagruzhennyh bibliotek
// ============================================================================

static void cmd_lsmod(void) {
    dynlink_print_info();
}

// ============================================================================
// KOMANDA: elfinfo <put'> - informatsiya o ELF fajle
// ============================================================================

static void cmd_elfinfo(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc < 2) {
        vga_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        vga_println("Ispol'zovanie: elfinfo <put' k ELF fajlu>");
        return;
    }

    int fd = vfs_open_file(args[1], VFS_O_RDONLY);
    if (fd < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        vga_println("Fajl ne najden.");
        return;
    }

    int file_size = vfs_get_filesize(fd);
    if (file_size <= 0) {
        vfs_close_file(fd);
        vga_println("Pustoj fajl.");
        return;
    }

    uint8_t* buf = (uint8_t*)kmalloc(file_size);
    if (!buf) {
        vfs_close_file(fd);
        vga_println("Nedostatochno pamyati.");
        return;
    }

    vfs_read_file(fd, buf, file_size);
    vfs_close_file(fd);

    elf_print_info(buf);

    kfree(buf);
}

// ============================================================================
// Vypolnenie odnoj komandy (bez pipe). argc==0 - net komandy.
// ============================================================================

static bool dispatch_single(int argc, char args[][SHELL_MAX_ARG_LEN]) {
    if (argc == 0) return true;

    if (pipecmds_dispatch(argc, args)) return true;

    if (shell_strcmp(args[0], "guide") == 0) {
        cmd_guide();
    } else if (shell_strcmp(args[0], "shine") == 0) {
        cmd_shine();
    } else if (shell_strcmp(args[0], "voice") == 0) {
        cmd_voice(argc, args);
    } else if (shell_strcmp(args[0], "pulse") == 0) {
        cmd_pulse();
    } else if (shell_strcmp(args[0], "embers") == 0) {
        cmd_embers();
    } else if (shell_strcmp(args[0], "matrix") == 0) {
        cmd_matrix();
    } else if (shell_strcmp(args[0], "scan") == 0) {
        cmd_scan();
    } else if (shell_strcmp(args[0], "read") == 0) {
        cmd_read(argc, args);
    } else if (shell_strcmp(args[0], "engrave") == 0) {
        cmd_engrave(argc, args);
    } else if (shell_strcmp(args[0], "delve") == 0) {
        cmd_delve(argc, args);
    } else if (shell_strcmp(args[0], "carve") == 0) {
        cmd_carve(argc, args);
    } else if (shell_strcmp(args[0], "erase") == 0) {
        cmd_erase(argc, args);
    } else if (shell_strcmp(args[0], "disk") == 0) {
        cmd_disk();
    } else if (shell_strcmp(args[0], "cycle") == 0) {
        cmd_cycle();
    } else if (shell_strcmp(args[0], "freeze") == 0) {
        cmd_freeze();
    } else if (shell_strcmp(args[0], "shade") == 0) {
        cmd_shade(argc, args);
    } else if (shell_strcmp(args[0], "launch") == 0) {
        cmd_launch(argc, args);
    } else if (shell_strcmp(args[0], "lsmod") == 0) {
        cmd_lsmod();
    } else if (shell_strcmp(args[0], "elfinfo") == 0) {
        cmd_elfinfo(argc, args);
    } else {
        sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        sh_puts("Neizvestnaya komanda: ");
        sh_println(args[0]);
        sh_set_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
        sh_println("Vvedite 'guide' dlya spravki.");
    }
    return true;
}

// ============================================================================
// Parser pipe: razrezaet input po simvolu '|' na otdel'nye segmenty.
// ============================================================================

#define SHELL_MAX_PIPE_SEGMENTS 4

static int split_pipe(char* input, char* segments[SHELL_MAX_PIPE_SEGMENTS]) {
    int  n = 0;
    int  i = 0;
    bool in_seg = false;

    while (input[i] == ' ') i++;
    segments[n++] = &input[i];
    in_seg = true;

    while (input[i]) {
        if (input[i] == '|' && n < SHELL_MAX_PIPE_SEGMENTS) {
            input[i] = '\0';
            int back = i - 1;
            while (back >= 0 && input[back] == ' ') { input[back] = '\0'; back--; }
            i++;
            while (input[i] == ' ') i++;
            segments[n++] = &input[i];
            in_seg = true;
            continue;
        }
        i++;
    }
    (void)in_seg;
    return n;
}

// ============================================================================
// shell_execute_command - tochka vhoda shell. Podderzhivaet '|' (pipe).
// ============================================================================

static void shell_ipc_init_once_fwd(void);

void shell_execute_command(char* input) {
    shell_ipc_init_once_fwd();

    if (shell_strlen(input) == 0) return;

    history_add(input);

    char work[SHELL_MAX_INPUT];
    int  ilen = shell_strlen(input);
    if (ilen >= SHELL_MAX_INPUT) ilen = SHELL_MAX_INPUT - 1;
    for (int i = 0; i < ilen; i++) work[i] = input[i];
    work[ilen] = '\0';

    char* segs[SHELL_MAX_PIPE_SEGMENTS] = {0};
    int   nseg = split_pipe(work, segs);

    if (nseg == 1) {
        char args[SHELL_MAX_ARGS][SHELL_MAX_ARG_LEN];
        int  argc = parse_args(segs[0], args);
        if (argc == 0) return;

        int pid = signal_register_task(args[0]);
        sh_ctx_t ctx = { NULL, NULL, pid };
        sh_ctx_set(&ctx);

        dispatch_single(argc, args);

        sh_ctx_set(NULL);
        if (pid > 0) signal_unregister(pid);
        return;
    }

    pipe_t* prev_pipe = NULL;

    for (int s = 0; s < nseg; s++) {
        char args[SHELL_MAX_ARGS][SHELL_MAX_ARG_LEN];
        int  argc = parse_args(segs[s], args);
        if (argc == 0) {
            sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            sh_println("Pustaya sektsiya v pipe.");
            if (prev_pipe) { pipe_destroy(prev_pipe); prev_pipe = NULL; }
            return;
        }

        bool is_last = (s == nseg - 1);

        pipe_t* next_pipe = NULL;
        if (!is_last) {
            next_pipe = pipe_create(PIPE_DEFAULT_CAPACITY);
            if (!next_pipe) {
                sh_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                sh_println("Ne hvataet pamyati dlya pipe.");
                if (prev_pipe) pipe_destroy(prev_pipe);
                return;
            }
        }

        int pid = signal_register_task(args[0]);
        sh_ctx_t ctx;
        ctx.in_pipe  = prev_pipe;
        ctx.out_pipe = next_pipe;
        ctx.pid      = pid;
        sh_ctx_set(&ctx);

        dispatch_single(argc, args);

        if (next_pipe) pipe_close_write(next_pipe);

        sh_ctx_set(NULL);
        if (pid > 0) signal_unregister(pid);

        if (prev_pipe) {
            pipe_destroy(prev_pipe);
            prev_pipe = NULL;
        }
        prev_pipe = next_pipe;
    }

    if (prev_pipe) pipe_destroy(prev_pipe);
}

// ============================================================================
// Glavnyj cikl obolochki
// ============================================================================

static bool g_ipc_inited = false;
static void shell_ipc_init_once(void) {
    if (g_ipc_inited) return;
    signal_init();
    usock_init();
    g_ipc_inited = true;
}
static void shell_ipc_init_once_fwd(void) { shell_ipc_init_once(); }

void shell_run(void) {
    shell_ipc_init_once();

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    vga_println("Dobro pozhalovat' v AMPLIFY-OS Shell!");
    vga_println("Vvedite 'guide' dlya spiska komand.");
    vga_println("Etap 29: pipe '|', signaly, sokety dostupny.");
    vga_println("");

    char input[SHELL_MAX_INPUT];

    while (1) {
        shell_prompt();
        shell_readline(input, SHELL_MAX_INPUT);
        shell_execute_command(input);
    }
}

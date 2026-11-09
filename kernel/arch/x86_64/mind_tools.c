// ============================================================================
// mind_tools.c — доступ AMPLIFY MIND к API ОС (этап 49).
// ----------------------------------------------------------------------------
// Инструменты — обычные функции ядра, которые собирают текстовый отчёт:
//   ps          — задачи планировщика, окна WM, ядра CPU, память, uptime;
//   ls  <path>  — каталог /disk/... (FAT32) или корень initrd (VFS);
//   read <path> — начало файла (до ~4 КБ), бинарные файлы не показываем;
//   logs        — хвост журнала ядра (klog: всё, что ушло в serial);
//   exec <cmd>  — команда shell, её вывод перехватывается через хуки
//                 fb_console (как это делает терминал Pulse).
// Всё вызывается только на BSP: VFS/FAT32/shell однопоточные.
//
// Безопасность: exec, запрошенный МОДЕЛЬЮ, требует подтверждения человеком
// (mind_tool_needs_confirm; виджет спрашивает Enter/Esc). Файл паролей
// users.txt не отдаётся никому — даже если «попросит» текст в журнале.
// Рекурсивный вызов `muse` из exec запрещён.
// ============================================================================
#include "mind.h"
#include "mind_util.h"
#include "smp.h"
#include "smp_pool.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "task.h"
#include "wm.h"
#include "window.h"
#include "fat32.h"
#include "vfs.h"
#include "initrd.h"
#include "serial.h"
#include "fb.h"
#include "shell.h"
#include "iso9660.h"

// ----------------------------------------------------------------------------
// Перекодировки CP1251 <-> UTF-8.
// ----------------------------------------------------------------------------
static const uint16_t k_cp1251_hi[64] = {   // 0x80..0xBF
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021,
    0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
    0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0xFFFD, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
    0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7,
    0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
    0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7,
    0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
};

// Длина корректной UTF-8-последовательности в p (0 — не UTF-8).
static int utf8_len(const uint8_t* p, int n, uint32_t* cp) {
    uint8_t c = p[0];
    int len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || len > n) return 0;
    for (int i = 1; i < len; i++) if ((p[i] & 0xC0) != 0x80) return 0;
    uint32_t v = c & (0x7F >> len);
    for (int i = 1; i < len; i++) v = (v << 6) | (p[i] & 0x3F);
    if ((len == 2 && v < 0x80) || (len == 3 && v < 0x800)) return 0;   // overlong
    *cp = v;
    return len;
}

static int put_utf8(uint32_t cp, char* o, int room) {
    if (cp < 0x80) { if (room < 1) return 0; o[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        if (room < 2) return 0;
        o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2;
    }
    if (room < 3) return 0;
    o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F)); return 3;
}

int mind_to_utf8(const char* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o + 1 < cap;) {
        uint8_t c = (uint8_t)in[i];
        if (c < 0x80) { out[o++] = (char)c; i++; continue; }
        uint32_t cp;
        int l = utf8_len((const uint8_t*)in + i, n - i, &cp);
        if (l) {                                 // уже UTF-8 — копируем как есть
            if (o + l >= cap) break;
            for (int k = 0; k < l; k++) out[o++] = in[i + k];
            i += l;
            continue;
        }
        cp = c >= 0xC0 ? 0x0410u + (c - 0xC0) : k_cp1251_hi[c - 0x80];
        int w = put_utf8(cp, out + o, cap - 1 - o);
        if (!w) break;
        o += w;
        i++;
    }
    out[o] = 0;
    return o;
}

static int uni_to_cp1251(uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    if (cp >= 0x0410 && cp <= 0x044F) return (int)(cp - 0x0410 + 0xC0);
    for (int k = 0; k < 64; k++) if (k_cp1251_hi[k] == cp) return 0x80 + k;
    return -1;
}

int mind_to_cp1251(const char* in, int n, char* out, int cap) {
    int o = 0;
    for (int i = 0; i < n && o + 1 < cap;) {
        uint8_t c = (uint8_t)in[i];
        if (c < 0x80) { out[o++] = (char)c; i++; continue; }
        uint32_t cp;
        int l = utf8_len((const uint8_t*)in + i, n - i, &cp);
        if (!l) { out[o++] = (char)c; i++; continue; }   // уже CP1251
        int m = uni_to_cp1251(cp);
        out[o++] = m >= 0 ? (char)m : '?';
        i += l;
    }
    out[o] = 0;
    return o;
}

// ----------------------------------------------------------------------------
// Буфер отчёта (родная кодировка), в конце -> UTF-8.
// ----------------------------------------------------------------------------
#define TOOL_RAW 6144
static char g_raw[TOOL_RAW];

static void raw_reset(void) { g_raw[0] = 0; }
static void raw_s(const char* s) { m_append(g_raw, TOOL_RAW, s); }
static void raw_d(int64_t v) { m_append_dec(g_raw, TOOL_RAW, v); }

static int raw_finish(char* out, int cap) {
    return mind_to_utf8(g_raw, (int)m_strlen(g_raw), out, cap);
}

// ----------------------------------------------------------------------------
// ps
// ----------------------------------------------------------------------------
static void tool_ps(void) {
    uint64_t up = timer_get_uptime_ms();
    raw_s("uptime: "); raw_d((int64_t)(up / 1000)); raw_s(" s\n");
    raw_s("cpu: "); raw_d(smp_cpus_started); raw_s(" cores online\n");
    // Роли ядер: в AMPLIFY-OS графическая сессия — это главный цикл ядра на
    // BSP (не задача планировщика), а AP с этапа 49 — пул вычислений MIND.
    bool busy = mind_state() == MIND_ST_BUSY;
    for (int c = 0; c < (int)smp_cpus_started && c < 16; c++) {
        raw_s("  cpu"); raw_d(c);
        if (c == 0) raw_s(" (BSP): window manager, desktop, drivers\n");
        else if (c == 1) raw_s(busy ? ": MIND conductor - generating\n" : ": MIND conductor - idle\n");
        else raw_s(busy ? ": MIND compute worker - busy\n" : ": MIND compute worker - idle\n");
    }

    // [49.1] Вся RAM, включая память выше 4 ГБ (там живут веса MIND).
    heap_stats_t hs = heap_get_stats();
    raw_s("memory: "); raw_d((int64_t)(pmm_free_ram() / (1024 * 1024))); raw_s(" MB free of ");
    raw_d((int64_t)(pmm_total_ram() / (1024 * 1024))); raw_s(" MB; kernel heap ");
    raw_d((int64_t)(hs.used_size / (1024 * 1024))); raw_s("/");
    raw_d((int64_t)(hs.total_size / (1024 * 1024))); raw_s(" MB\n");

    raw_s("tasks: "); raw_d(task_count());
    raw_s(task_count() ? "\n" : " (no user programs running)\n");
    int shown = 0;
    for (task_t* t = task_iterate(0); t && shown < 24; t = task_iterate(t), shown++) {
        raw_s("  tid "); raw_d(t->tid); raw_s(" "); raw_s(task_state_name(t->state));
        raw_s(t->vm ? " (user)\n" : " (kernel)\n");
    }
    raw_s("windows: "); raw_d(wm_count());
    raw_s(wm_count() ? "\n" : " (desktop only)\n");
    shown = 0;
    for (window_t* w = wm_iterate(0); w && shown < 24; w = wm_iterate(w), shown++) {
        raw_s("  \""); raw_s(w->title); raw_s("\"");
        raw_s(w->visible ? (w->focused ? " focused\n" : " visible\n") : " hidden\n");
    }
    raw_s("mind: "); raw_s(mind_model_label()); raw_s("\n");
}

// ----------------------------------------------------------------------------
// ls
// ----------------------------------------------------------------------------
typedef struct { int n; } ls_ctx_t;

static int ls_cb(const fat32_dir_entry_t* e, void* u) {
    ls_ctx_t* c = (ls_ctx_t*)u;
    if (e->name[0] == '.' && (e->name[1] == 0 || (e->name[1] == '.' && e->name[2] == 0))) return 0;
    if (c->n >= 80) { if (c->n == 80) raw_s("  ...\n"); c->n++; return 0; }
    raw_s("  "); raw_s(e->name);
    if (e->is_dir) raw_s("/");
    else { raw_s("  "); raw_d(e->size); raw_s(" bytes"); }
    raw_s("\n");
    c->n++;
    return 0;
}

static int iso_ls_cb(const char* name, const iso_entry_t* e, void* u) {
    ls_ctx_t* c = (ls_ctx_t*)u;
    if (c->n >= 80) return 1;
    raw_s("  "); raw_s(name);
    if (e->is_dir) raw_s("/");
    else { raw_s("  "); raw_d(e->size); raw_s(" bytes"); }
    raw_s("\n");
    c->n++;
    return 0;
}

static void tool_ls(const char* path) {
    if (!path || !*path) path = "/disk";
    if (m_starts(path, "/disk")) {
        if (!fat32_is_mounted()) { raw_s("error: /disk is not mounted\n"); return; }
        ls_ctx_t c = { 0 };
        raw_s(path); raw_s(":\n");
        if (fat32_iter_dir(path, ls_cb, &c) < 0) { raw_s("error: no such directory\n"); return; }
        if (!c.n) raw_s("  (empty)\n");
        return;
    }
    if (m_starts(path, MIND_CD_PREFIX) || m_streq(path, "/cdrom")) {
        if (!iso_mount_boot()) { raw_s("error: no installation medium\n"); return; }
        const char* sub = path + 6;                       // "/cdrom/x" -> "/x"
        if (!*sub) sub = "/";
        ls_ctx_t c = { 0 };
        raw_s(path); raw_s(" (installation medium):\n");
        if (iso_list(sub, iso_ls_cb, &c) < 0) { raw_s("error: no such directory\n"); return; }
        if (!c.n) raw_s("  (empty)\n");
        return;
    }
    // Корень: файлы initrd (VFS-узлы без префикса /disk/) + сам /disk.
    raw_s("/ (initrd):\n");
    int n = vfs_count();
    for (int i = 0; i < n && i < 200; i++) {
        vfs_node_t* v = vfs_at(i);
        if (!v || m_starts(v->name, "/disk")) continue;
        raw_s("  "); raw_s(v->name); raw_s("  "); raw_d((int64_t)v->size); raw_s(" bytes\n");
    }
    raw_s("  disk/\n");
}

// ----------------------------------------------------------------------------
// read
// ----------------------------------------------------------------------------
static bool path_denied(const char* p) {
    // Хэши паролей — не для глаз модели и не для экрана. FAT32 не различает
    // регистр, поэтому и сравнение без учёта регистра (Users.TXT — тот же файл).
    static const char needle[] = "users.txt";
    for (const char* s = p; *s; s++) {
        int k = 0;
        while (needle[k] && s[k] && ((s[k] >= 'A' && s[k] <= 'Z') ? s[k] + 32 : s[k]) == needle[k]) k++;
        if (!needle[k]) return true;
    }
    return false;
}

static void tool_read(const char* path) {
    static uint8_t buf[4096];
    if (!path || !*path) { raw_s("usage: read <path>\n"); return; }
    if (path_denied(path)) { raw_s("access denied: credentials file\n"); return; }
    int64_t got = -1;
    uint64_t total = 0;
    if (m_starts(path, "/disk")) {
        got = fat32_read_file_at(path, 0, buf, sizeof(buf) - 1, &total);
    } else {
        const char* nm = path[0] == '/' ? path + 1 : path;
        const uint8_t* p = initrd_get_file(nm, &total);
        if (p) {
            got = (int64_t)(total < sizeof(buf) - 1 ? total : sizeof(buf) - 1);
            m_memcpy(buf, p, (uint64_t)got);
        }
    }
    if (got < 0) { raw_s("error: file not found\n"); return; }
    int bad = 0;
    for (int64_t i = 0; i < got; i++) {
        uint8_t c = buf[i];
        if (c < 0x20 && c != '\n' && c != '\t' && c != '\r') bad++;
    }
    raw_s(path); raw_s(" ("); raw_d((int64_t)total); raw_s(" bytes)\n");
    if (got > 0 && bad * 10 > got) { raw_s("binary file - not shown\n"); return; }
    buf[got] = 0;
    for (int64_t i = 0; i < got; i++) if (buf[i] == '\r') buf[i] = ' ';
    raw_s((const char*)buf);
    if ((uint64_t)got < total) raw_s("\n... (truncated)\n");
}

// ----------------------------------------------------------------------------
// logs
// ----------------------------------------------------------------------------
// Сколько байт журнала отдаём: UTF-8 от этого объёма должен влезть в буфер
// вывода mind.c (TOOL_OUT = 4 КБ), а raw_finish сохраняет НАЧАЛО текста —
// лишнее срезало бы как раз самые свежие строки.
#define LOGS_MAX 2800

static void tool_logs(void) {
    // 8 КБ хвоста: после отбрасывания строк самого MIND должно остаться что читать.
    static char tail[8192];
    static char keep[8192];
    uint64_t n = klog_read_tail(tail, sizeof(tail));
    const char* p = tail;
    if (n == sizeof(tail) - 1) {          // обрезано — начинаем с целой строки
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    // [49.1] Строки самого MIND ([mind] user/reply/data...) — не журнал
    // системы: это вопросы пользователя и ответы модели. Модели они только
    // мешают (она пересказывала свой же диалог), а в карточке — шум.
    // Оставляем из них загрузку модели и ошибки.
    int kn = 0;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\n') e++;
        bool skip = m_starts(p, "[mind] ") && !m_starts(p, "[mind] loaded") && !m_starts(p, "[mind] error");
        for (const char* c = p; !skip && c < e && kn < (int)sizeof(keep) - 2; c++) keep[kn++] = *c;
        if (!skip && kn < (int)sizeof(keep) - 1) keep[kn++] = '\n';
        p = *e ? e + 1 : e;
    }
    keep[kn] = 0;
    // Только самые свежие строки: последние LOGS_MAX байт с начала строки.
    const char* q = keep;
    if (kn > LOGS_MAX) {
        q = keep + kn - LOGS_MAX;
        while (*q && *q != '\n') q++;
        if (*q) q++;
    }
    raw_s("kernel log (last lines):\n");
    raw_s(q);
}

// ----------------------------------------------------------------------------
// exec
// ----------------------------------------------------------------------------
static int g_cap_n;
static void cap_putc(char c) {
    if (g_cap_n + 1 < TOOL_RAW) { g_raw[g_cap_n++] = c; g_raw[g_cap_n] = 0; }
}
static void cap_reset(void) { g_cap_n = 0; g_raw[0] = 0; }

static void tool_exec(const char* cmd) {
    static char line[256];
    if (!cmd || !*cmd) { raw_s("usage: exec <command>\n"); return; }
    if (m_starts(cmd, "muse")) { raw_s("error: MIND cannot call itself\n"); return; }
    line[0] = 0;
    m_append(line, sizeof(line), cmd);
    g_cap_n = 0;
    g_raw[0] = 0;
    fb_console_set_hooks(cap_putc, cap_reset);
    shell_exec_line(line);
    fb_console_set_hooks(0, 0);
    if (!g_cap_n) raw_s("(no output)\n");
}

// ----------------------------------------------------------------------------
// [49.1] Точные ответы без модели (шаблон). Qwen3-1.7B пересказывает данные
// уже неплохо, но числа и списки «из головы» модели всё равно не доверяем:
// для вопросов о процессах, памяти, ядрах, окнах и времени работы MIND
// собирает ответ сам, а модель лишь дополняет его (текст ниже становится
// началом ответа ассистента, см. mind.c:ask_qwen_with_data).
// Строки — в кодировке ядра (CP1251, GCC транскодирует литералы), на выходе
// — UTF-8.
// ----------------------------------------------------------------------------
void mind_lower_cp1251(char* s, int n) {
    for (int i = 0; i < n && s[i]; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c >= 'A' && c <= 'Z') s[i] = (char)(c + 32);
        else if (c >= 0xC0 && c <= 0xDF) s[i] = (char)(c + 32);     // А..Я -> а..я
        else if (c == 0xA8) s[i] = (char)0xB8;                     // Ё -> ё
    }
}

bool mind_has_stem(const char* s, const char* stem) {
    uint64_t n = m_strlen(stem);
    for (; *s; s++) {
        uint64_t k = 0;
        while (k < n && s[k] == stem[k]) k++;
        if (k == n) return true;
    }
    return false;
}

// «процесс», но не «процессор».
static bool has_process_word(const char* s) {
    for (; *s; s++) {
        if (m_starts(s, "процесс") && !m_starts(s, "процессор")) return true;
    }
    return false;
}

static bool has_en(const char* s, const char* w) {
    // Слово латиницей с начала слова (s уже в нижнем регистре).
    uint64_t n = m_strlen(w);
    for (const char* p = s; *p; p++) {
        if (p > s && ((p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= '0' && p[-1] <= '9'))) continue;
        if (m_starts(p, w)) return true;
        (void)n;
    }
    return false;
}

// 1 ядро, 2-4 ядра, 5+ ядер (и 11-14 — «ядер»).
static const char* cores_word(int n) {
    int d = n % 10, h = n % 100;
    if (d == 1 && h != 11) return " ядро";
    if (d >= 2 && d <= 4 && (h < 12 || h > 14)) return " ядра";
    return " ядер";
}

int mind_facts(const char* q, char* out, int cap) {
    static char low[1024];
    int ln = mind_to_cp1251(q, (int)m_strlen(q), low, sizeof(low));
    mind_lower_cp1251(low, ln);
    bool ru = false;
    for (int i = 0; i < ln; i++) if ((uint8_t)low[i] >= 0xC0) { ru = true; break; }
    bool mem = has_en(low, "memory") || has_en(low, "ram") || mind_has_stem(low, "памят") ||
               mind_has_stem(low, "оператив");
    bool cpu = has_en(low, "cpu") || has_en(low, "core") || has_en(low, "processor") ||
               mind_has_stem(low, "ядр") || mind_has_stem(low, "ядер") || mind_has_stem(low, "процессор");
    bool up = has_en(low, "uptime") || has_en(low, "how long") || mind_has_stem(low, "время работы") ||
              mind_has_stem(low, "сколько работает") || mind_has_stem(low, "аптайм");
    bool win = has_en(low, "window") || mind_has_stem(low, "окн") || mind_has_stem(low, "окон");
    bool task = has_en(low, "process") || has_en(low, "task") || has_en(low, "program") ||
                has_en(low, "running") || has_process_word(low) || mind_has_stem(low, "задач") ||
                mind_has_stem(low, "программ") || mind_has_stem(low, "запущен");
    if (has_en(low, "state") || has_en(low, "status") || mind_has_stem(low, "состояни")) mem = cpu = up = win = task = true;
    if (!(mem || cpu || up || win || task)) { out[0] = 0; return 0; }
    if (task) win = true;              // «что запущено» — это и окна

    raw_reset();
    if (up) {
        uint64_t s = timer_get_uptime_ms() / 1000;
        raw_s(ru ? "- **Время работы:** " : "- **Uptime:** ");
        if (s >= 3600) { raw_d((int64_t)(s / 3600)); raw_s(ru ? " ч " : " h "); }
        if (s >= 60) { raw_d((int64_t)(s / 60 % 60)); raw_s(ru ? " мин " : " min "); }
        raw_d((int64_t)(s % 60)); raw_s(ru ? " с\n" : " s\n");
    }
    if (cpu) {
        int n = (int)smp_cpus_started;
        raw_s(ru ? "- **Процессор:** " : "- **CPU:** ");
        raw_d(n);
        raw_s(ru ? cores_word(n) : (n == 1 ? " core" : " cores"));
        if (n == 1) raw_s(ru ? ", на нём работает всё\n" : ", it runs everything\n");
        else {
            raw_s(ru ? ": cpu0 - оконный менеджер и драйверы, cpu1" : ": cpu0 runs the window manager and drivers, cpu1");
            if (n > 2) { raw_s("-"); raw_d(n - 1); }
            raw_s(ru ? " - вычислительный пул MIND\n" : (n > 2 ? " are the MIND compute pool\n" : " is the MIND compute pool\n"));
        }
    }
    if (mem) {
        raw_s(ru ? "- **Память:** свободно " : "- **Memory:** ");
        raw_d((int64_t)(pmm_free_ram() / (1024 * 1024)));
        raw_s(ru ? " МБ из " : " MB free of ");
        raw_d((int64_t)(pmm_total_ram() / (1024 * 1024)));
        raw_s(ru ? " МБ\n" : " MB\n");
    }
    if (task) {
        int user = 0, kern = 0;
        for (task_t* t = task_iterate(0); t; t = task_iterate(t)) { if (t->vm) user++; else kern++; }
        raw_s(ru ? "- **Программы:** " : "- **Programs:** ");
        if (!user && !kern) {
            raw_s(ru ? "пользовательских программ нет - рабочий стол и приложения работают внутри ядра\n"
                     : "none - the desktop and its apps run inside the kernel\n");
        } else {
            raw_d(user + kern);
            raw_s(ru ? " задач (пользовательских: " : " tasks (user: ");
            raw_d(user);
            raw_s(ru ? ", ядра: " : ", kernel: ");
            raw_d(kern);
            raw_s(")\n");
        }
    }
    if (win) {
        int nw = wm_count();
        raw_s(ru ? "- **Окна:** " : "- **Windows:** ");
        if (!nw) raw_s(ru ? "нет, только рабочий стол\n" : "none, only the desktop\n");
        else {
            raw_d(nw);
            raw_s(ru ? ": " : " open: ");
            int shown = 0;
            for (window_t* w = wm_iterate(0); w && shown < 8; w = wm_iterate(w), shown++) {
                if (shown) raw_s(", ");
                raw_s(w->title);
                if (w->focused) raw_s(ru ? " (активно)" : " (focused)");
                else if (!w->visible) raw_s(ru ? " (скрыто)" : " (hidden)");
            }
            raw_s("\n");
        }
    }
    raw_s("\n");
    return raw_finish(out, cap);
}

// ----------------------------------------------------------------------------
bool mind_tool_known(const char* name) {
    return m_streq(name, "ps") || m_streq(name, "ls") || m_streq(name, "read") ||
           m_streq(name, "logs") || m_streq(name, "exec");
}

bool mind_tool_needs_confirm(const char* name) {
    return m_streq(name, "exec");
}

int mind_tool_run(const char* name, const char* args, char* out, int cap) {
    if (!out || cap <= 0) return -1;
    raw_reset();
    while (args && *args == ' ') args++;
    if (m_streq(name, "ps")) tool_ps();
    else if (m_streq(name, "ls")) tool_ls(args);
    else if (m_streq(name, "read")) tool_read(args);
    else if (m_streq(name, "logs")) tool_logs();
    else if (m_streq(name, "exec")) tool_exec(args);
    else { out[0] = 0; return -1; }
    serial_puts("[mind] tool ");
    serial_puts(name);
    serial_puts(" -> ");
    serial_print_dec(m_strlen(g_raw));
    serial_puts(" bytes\n");
    // Начало отчёта одной строкой — по нему судят headless-тесты.
    serial_puts("[mind] data: ");
    for (int i = 0; g_raw[i] && i < 240; i++) serial_putc(g_raw[i] == '\n' ? '|' : g_raw[i]);
    serial_puts("\n");
    return raw_finish(out, cap);
}

// ============================================================================
// app_rune.c — Rune: окно программ WebAssembly AMPLIFY-OS (подэтап 51.4).
// ----------------------------------------------------------------------------
// До 51.4 программа жила только в терминале: `rune run`, консоль — тот терминал,
// из которого её запустили. Это приложение — место, куда заходят, чтобы
// запускать программы и работать с ними:
//   «Библиотека» — что умеет эта машина (своё ядро для программы или нет),
//       список программ: встроенные (initrd) и свои (/vault/runes), настройки
//       запуска (аргументы, каталог и доступ к нему, предел памяти), «Запустить»;
//   «Консоль»    — вывод программы по мере работы, с прокруткой; строка ввода;
//       «Остановить», «Конец ввода»; состояние: ядро, время счёта, память.
// Вид выбирается сам: программа есть (или только что кончилась и её итог ещё
// не закрыт) — консоль, иначе — библиотека. Своих окон у программ нет.
//
// ЗАКРЫТИЕ ОКНА ПРОГРАММУ НЕ ТРОГАЕТ. Поэтому всё, что должно пережить окно,
// лежит не в нём, а в статической «сессии» (g_ses): журнал вывода, чья
// программа, итог. Владелец программы для движка (rune_opts_t.owner) — адрес
// сессии, приёмник вывода — ses_out; окно, открытое снова, показывает тот же
// журнал с того же места.
//
// ЖУРНАЛ — один линейный буфер текста (CP1251, строки через '\n') и таблица
// начал ЭКРАННЫХ строк: длинная строка программы занимает несколько экранных.
// Таблица ведётся по мере прихода текста, поэтому прокрутка не зависит от
// длины журнала. Переполнился буфер — отбрасывается первая половина (целыми
// строками), о чём окно честно пишет.
//
// Раскладка считается одной функцией rn_layout() — ею пользуются и отрисовка,
// и обработчик щелчков. Координаты элементов печатаются в serial
// (`[rune-ui] layout …`, в пикселях экрана) — по ним щёлкает тест. Что сейчас
// на экране консоли, тест спрашивает командой `rune screen`, весь журнал —
// `rune log` (app_rune_dump).
// ============================================================================
#include "apps.h"
#include "app_stage_e.h"
#include "ui_kit.h"
#include "ui_icons.h"
#include "rune.h"
#include "wm.h"
#include "heap.h"
#include "fat32.h"
#include "smp_pool.h"
#include "keyboard.h"
#include "timer.h"
#include "serial.h"

#define RN_MAX_PROG   24
#define RN_LIST_MAX   6                 // строк списка программ на высоком экране
#define RN_LOG_BYTES  (256 * 1024)
#define RN_LOG_ROWS   16384
#define RN_COLS_MAX   200
#define RN_MAX_ARGS   16
#define RN_MEM_MAX_MB 64                // потолок памяти программы (wasm_rt.c: 1024 страницы)

enum { V_LIBRARY = 0, V_CONSOLE };
enum { ST_INFO = 0, ST_OK, ST_ERR };
enum { F_NONE = 0, F_ARGS, F_DIR, F_MEM, F_COUNT };
enum { ACC_RW = 0, ACC_RO, ACC_NONE };

// Признаки экранной строки — в старших битах её начала (самому началу хватает
// 18 бит: журнал — 256 КБ).
#define ROW_SYS   0x80000000u           // строка самого окна, не программы (итог, «^D»)
#define ROW_OFF   0x00FFFFFFu

// ---------------------------------------------------------------------------
// Сессия: то, что переживает окно.
// ---------------------------------------------------------------------------
// Журнал (текст и таблица строк, вместе 320 КБ) берётся из кучи при первом
// открытии окна и не возвращается. Статическим массивом его держать нельзя:
// .bss ядра кончается под самой отметкой 8 МБ, выше — память прошивки OVMF,
// и образ, выросший за неё, UEFI-загрузчик не может разместить (CLAUDE.md §5.50).
typedef struct {
    char*    text;                      // RN_LOG_BYTES
    int      len;
    uint32_t* row;                      // RN_LOG_ROWS
    int      rows;
    int      cols;                      // ширина консоли в знакоместах (по ней нарезаны строки)
    int      col;                       // столбец конца журнала
    bool     sys_line;                  // текущая (последняя) строка — служебная
    bool     trimmed;                   // начало журнала отброшено
    uint64_t change;                    // растёт с каждой правкой журнала: окну пора перерисоваться
    // ---- программа окна ----
    bool     mine;                      // работает программа, запущенная отсюда
    bool     ended;                     // она кончилась, итог ещё не закрыт
    uint64_t ends0;                     // счётчик концов программ на момент запуска
    char     name[64], path[160];
    int      end_kind, exit_code;
    char     end_text[64];
    uint64_t end_cpu_ms, end_wall_ms;
    uint32_t end_pages;
    // ---- настройки запуска (их удобно не набирать заново) ----
    char     args[160], dir[120], mem[6];
    int      access;
    bool     inited;
} rn_session_t;
static rn_session_t g_ses;

typedef struct {
    window_t* win;
    rune_prog_t prog[RN_MAX_PROG];
    int      n_prog, sel;
    int      focus;                     // F_*
    char     status[200];
    int      status_kind;
    char     input[200];                // строка ввода программе
    rune_state_t q;
    int      view;
    int      top;                       // первая видимая экранная строка
    bool     follow;                    // держаться у конца журнала
    int      vis;                       // сколько экранных строк помещается
    ui_font_id_t font;
    uint64_t seen_change, last_sec;
    int      last_key;                  // что уже нарисовано/напечатано: вид + состояние
    int      logged_key;
} rn_t;

typedef struct {
    // библиотека
    pk_rect_t host, tips;
    pk_rect_t list, row[RN_LIST_MAX];
    int       rows;
    pk_rect_t launch, f_args, f_dir, seg[3], f_mem, b_start, b_refresh, status;
    // консоль
    pk_rect_t b1, b2;                   // правая и левая кнопки полосы состояния
    pk_rect_t panel, text, input, b_send;
    int       foot_y;
} rn_layout_t;

static rn_t* g_rn;                      // окно одно: программа тоже одна

// ---------------------------------------------------------------------------
// Мелочи.
// ---------------------------------------------------------------------------
static void p_copy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && i < cap - 1 && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
static void p_cat(char* d, const char* s, int cap) {
    int n = 0;
    while (d[n]) n++;
    for (int i = 0; s && s[i] && n < cap - 1; i++) d[n++] = s[i];
    d[n] = 0;
}
static void p_cat_num(char* d, uint64_t v, int cap) {
    char t[24];
    se_num(t, v);
    p_cat(d, t, cap);
}
static void set_status(rn_t* s, int kind, const char* text) {
    p_copy(s->status, text, sizeof(s->status));
    s->status_kind = kind;
}
// «1.2 с» / «340 мс»: время счёта растёт от миллисекунд до часов.
static void p_cat_time(char* d, uint64_t ms, int cap) {
    if (ms < 1000) { p_cat_num(d, ms, cap); p_cat(d, " мс", cap); return; }
    if (ms < 60000) {
        p_cat_num(d, ms / 1000, cap); p_cat(d, ".", cap); p_cat_num(d, (ms % 1000) / 100, cap);
        p_cat(d, " с", cap);
        return;
    }
    uint64_t sec = ms / 1000;
    p_cat_num(d, sec / 60, cap); p_cat(d, " мин ", cap); p_cat_num(d, sec % 60, cap); p_cat(d, " с", cap);
}
static void p_cat_mem(char* d, uint32_t pages, int cap) {
    // Страница — 64 КБ. До мегабайта показываем килобайты: у маленькой
    // программы «0 МБ» выглядело бы как «памяти нет».
    if (pages < 16) { p_cat_num(d, (uint64_t)pages * 64, cap); p_cat(d, " КБ", cap); }
    else {
        p_cat_num(d, pages / 16, cap);
        if (pages < 160) { p_cat(d, ".", cap); p_cat_num(d, (pages % 16) * 10 / 16, cap); }
        p_cat(d, " МБ", cap);
    }
}

// Причины отказа и конца программы приходят из rune.c по-английски (те же
// строки идут в serial, по ним судят тесты). В окне говорим по-русски.
static const char* ru_reason(const char* e) {
    if (!e || !e[0]) return "Программа не запустилась";
    if (se_contains(e, "another program"))        return "Уже работает другая программа - сначала остановите её";
    if (se_contains(e, "no free core"))           return "Нет свободного ядра процессора (его занял гость Phantom)";
    if (se_contains(e, "directory does not exist")) return "Каталога программы нет на диске";
    if (se_contains(e, "must be on the disk"))    return "Каталог программы должен быть на диске: /disk/...";
    if (se_contains(e, "without '..'"))           return "Каталог программы задаётся без «..»";
    if (se_contains(e, "whole disk"))             return "Весь диск программе не даётся: на нём системные файлы";
    if (se_contains(e, "system settings"))        return "Каталог настроек системы программе не даётся";
    if (se_contains(e, "file not found"))         return "Файл программы не найден";
    if (se_contains(e, "file is empty"))          return "Файл программы пуст";
    if (se_contains(e, "rejected"))               return "Это не программа WebAssembly, или она повреждена";
    if (se_contains(e, "not enough"))             return "Не хватает памяти для запуска программы";
    if (se_contains(e, "arguments"))              return "Слишком много или слишком длинные аргументы";
    if (se_contains(e, "_start"))                 return "В модуле нет точки входа _start: это не программа WASI";
    if (se_contains(e, "too long to start"))      return "Модуль слишком долго запускался и был остановлен";
    return e;
}

static void ru_end(const rn_session_t* g, char* out, int cap) {
    out[0] = 0;
    if (g->end_kind == RUNE_END_EXIT) {
        p_cat(out, g->exit_code ? "Программа завершилась с кодом " : "Программа завершилась, код ", cap);
        p_cat_num(out, (uint64_t)(uint32_t)g->exit_code, cap);
        return;
    }
    const char* e = g->end_text;
    if (se_contains(e, "by user"))             p_cat(out, "Остановлена пользователем", cap);
    else if (se_contains(e, "time limit"))     p_cat(out, "Остановлена: вышло время счёта", cap);
    else if (se_contains(e, "unreachable"))    p_cat(out, "Программа аварийно остановилась (abort)", cap);
    else if (se_contains(e, "out of bounds"))  p_cat(out, "Сбой программы: обращение за пределы своей памяти", cap);
    else if (se_contains(e, "divide by zero")) p_cat(out, "Сбой программы: деление на ноль", cap);
    else if (se_contains(e, "exhausted"))      p_cat(out, "Сбой программы: исчерпан стек вызовов", cap);
    else { p_cat(out, "Сбой программы: ", cap); p_cat(out, e, cap); }
}

// ---------------------------------------------------------------------------
// Журнал.
// ---------------------------------------------------------------------------
static void log_rebuild(rn_session_t* g);

static void log_new_row(rn_session_t* g) {
    if (g->rows < RN_LOG_ROWS) g->row[g->rows++] = (uint32_t)g->len | (g->sys_line ? ROW_SYS : 0);
}

// Отбросить первую половину журнала — целыми строками программы.
static void log_trim(rn_session_t* g) {
    int cut = g->len / 2;
    while (cut < g->len && g->text[cut - 1] != '\n') cut++;
    for (int i = cut; i < g->len; i++) g->text[i - cut] = g->text[i];
    g->len -= cut;
    g->trimmed = true;
    log_rebuild(g);
}

static void log_putc(rn_session_t* g, char c) {
    if (g->len >= RN_LOG_BYTES - 1 || g->rows >= RN_LOG_ROWS - 1) log_trim(g);
    if (c == '\n') {
        g->text[g->len++] = c;
        g->col = 0;
        g->sys_line = false;
        log_new_row(g);                 // следующая строка начинается здесь (пока пустая)
        return;
    }
    if (g->col >= g->cols) { g->col = 0; log_new_row(g); }     // перенос длинной строки
    g->text[g->len++] = c;
    g->col++;
}

// Нарезать журнал на экранные строки заново (сменилась ширина или обрезали начало).
static void log_rebuild(rn_session_t* g) {
    int len = g->len;
    g->len = 0; g->rows = 0; g->col = 0; g->sys_line = false;
    log_new_row(g);
    // Признак «служебная» после перестройки не восстанавливается: он нужен
    // только для цвета, а перестройка — редкость (смена размера экрана).
    for (int i = 0; i < len; i++) {
        char c = g->text[i];
        if (c == '\n') { g->len++; g->col = 0; log_new_row(g); continue; }
        if (g->col >= g->cols) { g->col = 0; log_new_row(g); }
        g->len++;
        g->col++;
    }
    g->change++;
}

static void log_clear(rn_session_t* g) {
    g->len = 0; g->rows = 0; g->col = 0; g->sys_line = false; g->trimmed = false;
    log_new_row(g);
    g->change++;
}

static void log_puts(rn_session_t* g, const char* s) {
    for (; *s; s++) log_putc(g, *s);
    g->change++;
}

// Строка самого окна — всегда с новой строки и другим цветом.
static void log_sys(rn_session_t* g, const char* s) {
    if (g->col) log_putc(g, '\n');
    g->sys_line = true;
    if (g->rows) g->row[g->rows - 1] |= ROW_SYS;
    log_puts(g, s);
    log_putc(g, '\n');
}

// Число экранных строк с текстом: последняя, пустая («здесь начнётся
// следующая»), не считается.
static int log_rows(const rn_session_t* g) {
    int n = g->rows;
    if (n > 0 && (int)(g->row[n - 1] & ROW_OFF) >= g->len) n--;
    return n;
}

// Текст экранной строки r — в out (без перевода строки). Возвращает длину.
static int log_row_text(const rn_session_t* g, int r, char* out, int cap) {
    int a = (int)(g->row[r] & ROW_OFF);
    int b = r + 1 < g->rows ? (int)(g->row[r + 1] & ROW_OFF) : g->len;
    int n = 0;
    for (int i = a; i < b && n < cap - 1; i++) {
        if (g->text[i] == '\n') break;
        out[n++] = g->text[i];
    }
    out[n] = 0;
    return n;
}

// Приёмник вывода программы (BSP, из rune_tick). Сюда же rune.c шлёт свои
// строки `[rune] run …`, `[rune] exit …` — целиком и с начала строки. В журнал
// окна они не идут: итог окно показывает само, по-русски. В serial уходит всё,
// как у терминала (он дублирует вывод команд): по этим строкам судят тесты.
static void ses_out(const char* s) {
    rn_session_t* g = &g_ses;
    serial_puts(s);
    if (g->col == 0 && s[0] == '[' && s[1] == 'r' && s[2] == 'u' && s[3] == 'n' && s[4] == 'e' && s[5] == ']') return;
    log_puts(g, s);
}

static bool ses_init(void) {
    rn_session_t* g = &g_ses;
    if (g->inited) return true;
    if (!g->text) g->text = kmalloc(RN_LOG_BYTES);
    if (!g->row) g->row = kmalloc(RN_LOG_ROWS * sizeof(uint32_t));
    if (!g->text || !g->row) return false;
    g->inited = true;
    g->cols = 100;
    p_copy(g->dir, RUNE_DEFAULT_DIR, sizeof(g->dir));
    log_clear(g);
    return true;
}

// ---------------------------------------------------------------------------
// Раскладка.
// ---------------------------------------------------------------------------
static void rn_layout(const window_t* w, const rn_t* s, rn_layout_t* L) {
    (void)s;
    int x0 = 78, x1 = w->width - 70;
    int content = x1 - x0;
    int top = 104;

    // ---- библиотека ----
    int lw = content * 36 / 100;
    if (lw < 380) lw = 380;
    if (lw > 520) lw = 520;
    int rx = x0 + lw + 24;
    int rw = x1 - rx;
    if (rw > 900) rw = 900;
    L->host = (pk_rect_t){ x0, top, lw, 262 };
    L->tips = (pk_rect_t){ x0, top + 262 + 18, lw, 214 };
    // Сколько строк списка помещается над карточкой запуска и строкой состояния.
    int rows = (w->height - 34 - 10 - 40 - 16 - 232 - 18 - 64 - top) / 58;
    L->rows = rows > RN_LIST_MAX ? RN_LIST_MAX : rows < 2 ? 2 : rows;
    L->list = (pk_rect_t){ rx, top, rw, 54 + L->rows * 58 + 10 };
    for (int i = 0; i < RN_LIST_MAX; i++)
        L->row[i] = (pk_rect_t){ rx + 14, top + 48 + i * 58, rw - 28, 52 };
    L->launch = (pk_rect_t){ rx, L->list.y + L->list.h + 18, rw, 232 };
    int fx = rx + 28, fw = rw - 56, ly = L->launch.y;
    L->f_args = (pk_rect_t){ fx, ly + 46, fw, 38 };
    // Вторая строка: каталог | доступ (три положения) | память.
    int mem_w = 96, seg_w = 3 * 118 + 2 * 6;
    int dir_w = fw - mem_w - seg_w - 2 * 16;
    if (dir_w < 180) { dir_w = 180; seg_w = fw - mem_w - dir_w - 2 * 16; }
    L->f_dir = (pk_rect_t){ fx, ly + 118, dir_w, 38 };
    int sw = (seg_w - 2 * 6) / 3;
    for (int i = 0; i < 3; i++)
        L->seg[i] = (pk_rect_t){ fx + dir_w + 16 + i * (sw + 6), ly + 118, sw, 38 };
    L->f_mem = (pk_rect_t){ fx + fw - mem_w, ly + 118, mem_w, 38 };
    L->b_start   = (pk_rect_t){ fx, ly + 174, 190, 40 };
    L->b_refresh = (pk_rect_t){ fx + 202, ly + 174, 190, 40 };
    L->status = (pk_rect_t){ rx, L->launch.y + L->launch.h + 16, rw, 40 };

    // ---- консоль ----
    L->b1 = (pk_rect_t){ x1 - 180, 90, 180, 38 };
    L->b2 = (pk_rect_t){ x1 - 180 - 12 - 180, 90, 180, 38 };
    L->foot_y = w->height - 36;
    int in_y = L->foot_y - 14 - 38;
    L->input  = (pk_rect_t){ x0, in_y, content - 150 - 12, 38 };
    L->b_send = (pk_rect_t){ x1 - 150, in_y, 150, 38 };
    L->panel  = (pk_rect_t){ x0, 140, content, in_y - 12 - 140 };
    L->text   = (pk_rect_t){ x0 + 16, 140 + 12, content - 16 - 26, L->panel.h - 24 };
}

// Координаты элементов — в serial, в пикселях ЭКРАНА (центры).
static void log_rect(const window_t* w, const char* name, pk_rect_t r) {
    serial_puts(" "); serial_puts(name); serial_puts("=");
    serial_print_dec((uint64_t)(w->x + r.x + r.w / 2)); serial_puts(",");
    serial_print_dec((uint64_t)(w->y + r.y + r.h / 2));
}

static void log_layout(const window_t* w, const rn_t* s, const rn_layout_t* L) {
    if (s->view == V_LIBRARY) {
        serial_puts("[rune-ui] layout view=library");
        static const char* const nm[RN_LIST_MAX] = { "prog0", "prog1", "prog2", "prog3", "prog4", "prog5" };
        int first = s->sel >= L->rows ? s->sel - L->rows + 1 : 0;
        for (int i = 0; i < L->rows && first + i < s->n_prog; i++) log_rect(w, nm[i], L->row[i]);
        log_rect(w, "args", L->f_args);
        log_rect(w, "dir", L->f_dir);
        log_rect(w, "rw", L->seg[0]); log_rect(w, "ro", L->seg[1]); log_rect(w, "nofiles", L->seg[2]);
        log_rect(w, "mem", L->f_mem);
        log_rect(w, "start", L->b_start);
        log_rect(w, "refresh", L->b_refresh);
        serial_puts(" first="); serial_print_dec((uint64_t)first);
    } else {
        bool ended = g_ses.ended && !s->q.active;
        serial_puts(ended ? "[rune-ui] layout view=console ended" : "[rune-ui] layout view=console");
        log_rect(w, "panel", L->panel);
        log_rect(w, "input", L->input);
        log_rect(w, "send", L->b_send);
        log_rect(w, ended ? "back" : "stop", L->b1);
        log_rect(w, ended ? "again" : "eof", L->b2);
        serial_puts(" cols="); serial_print_dec((uint64_t)g_ses.cols);
        serial_puts(" rows="); serial_print_dec((uint64_t)s->vis);
    }
    serial_puts("\n");
}

// ---------------------------------------------------------------------------
// Действия.
// ---------------------------------------------------------------------------
static void rn_refresh(rn_t* s) {
    s->n_prog = rune_programs(s->prog, RN_MAX_PROG);
    if (s->sel >= s->n_prog) s->sel = s->n_prog ? s->n_prog - 1 : 0;
    s->logged_key = -1;                                 // строк стало другое число — раскладку заново
    serial_puts("[rune-ui] programs: "); serial_print_dec((uint64_t)s->n_prog);
    for (int i = 0; i < s->n_prog; i++) {
        serial_puts(i ? "; " : " - "); serial_puts(s->prog[i].name);
        serial_puts(" ("); serial_print_dec(s->prog[i].bytes);
        serial_puts(s->prog[i].builtin ? " bytes, built-in)" : " bytes, disk)");
    }
    serial_puts("\n");
}

static void rn_select(rn_t* s, int i) {
    if (i < 0 || i >= s->n_prog) return;
    s->sel = i;
    s->logged_key = -1;                                 // список мог прокрутиться
    serial_puts("[rune-ui] selected "); serial_puts(s->prog[i].name); serial_puts("\n");
}

static void rn_start(rn_t* s) {
    rn_session_t* g = &g_ses;
    if (!s->n_prog) { set_status(s, ST_ERR, "Запускать нечего: программ нет"); return; }
    if (rune_active()) { set_status(s, ST_ERR, ru_reason("another program")); return; }
    const rune_prog_t* pr = &s->prog[s->sel];

    // Предел памяти: пусто — по умолчанию; иначе число 1..64.
    uint32_t mb = 0;
    for (const char* c = g->mem; *c; c++) mb = mb * 10 + (uint32_t)(*c - '0');
    if (g->mem[0] && (mb < 1 || mb > RN_MEM_MAX_MB)) {
        set_status(s, ST_ERR, "Предел памяти - от 1 до 64 МБ (пусто - 64 МБ)");
        s->focus = F_MEM;
        serial_puts("[rune-ui] start refused: the memory limit must be 1..64 MB\n");
        return;
    }
    // Аргументы — через пробел. Строки режем в своей копии: rune_start их
    // копирует к себе, дальше они не нужны.
    static char abuf[sizeof(g->args)];
    static char* av[RN_MAX_ARGS];
    int ac = 0;
    p_copy(abuf, g->args, sizeof(abuf));
    for (char* c = abuf; *c;) {
        while (*c == ' ') c++;
        if (!*c) break;
        if (ac == RN_MAX_ARGS) { set_status(s, ST_ERR, "Слишком много аргументов (не больше 16)"); return; }
        av[ac++] = c;
        while (*c && *c != ' ') c++;
        if (*c) *c++ = 0;
    }

    rune_opts_t o;
    for (size_t i = 0; i < sizeof(o); i++) ((uint8_t*)&o)[i] = 0;
    o.path = pr->path;
    o.nargs = ac;
    o.args = av;
    o.dir = g->dir;
    o.read_only = g->access == ACC_RO;
    o.no_files = g->access == ACC_NONE;
    o.interactive = true;               // у окна есть строка ввода
    o.memory_mb = mb;
    o.owner = g;

    serial_puts("[rune-ui] start "); serial_puts(pr->path);
    if (g->args[0]) { serial_puts(" args: "); serial_puts(g->args); }
    serial_puts("\n");

    rune_state_t q;
    rune_query(&q);
    log_clear(g);
    p_copy(g->name, pr->name, sizeof(g->name));
    p_copy(g->path, pr->path, sizeof(g->path));
    g->ends0 = q.ends;
    g->ended = false;
    g->mine = true;
    if (!rune_start(&o, ses_out)) {
        g->mine = false;
        set_status(s, ST_ERR, ru_reason(rune_last_error()));
        serial_puts("[rune-ui] start refused: "); serial_puts(rune_last_error()); serial_puts("\n");
        return;
    }
    s->status[0] = 0;
    s->focus = F_NONE;
    s->input[0] = 0;
    s->follow = true;
}

static void rn_send(rn_t* s) {
    rn_session_t* g = &g_ses;
    if (!g->mine || !s->q.active || !s->q.input_open) return;
    int n = se_len(s->input);
    // Эхо — как в терминале: набранное встаёт в журнал сразу за приглашением
    // программы, на той же строке.
    log_puts(g, s->input);
    log_puts(g, "\n");
    rune_input(s->input, n);
    rune_input("\n", 1);
    serial_puts("[rune-ui] input: "); serial_puts(s->input); serial_puts("\n");
    s->input[0] = 0;
    s->follow = true;
}

static void rn_eof(rn_t* s) {
    if (!g_ses.mine || !s->q.active || !s->q.input_open) return;
    log_sys(&g_ses, "^D  (конец ввода)");
    rune_input_eof();
    serial_puts("[rune-ui] input closed\n");
}

static void rn_stop(rn_t* s) {
    if (!s->q.active) return;
    serial_puts("[rune-ui] stop\n");
    rune_stop();
}

static void rn_back(rn_t* s) {
    g_ses.ended = false;
    s->input[0] = 0;
    serial_puts("[rune-ui] back to the library\n");
}

static void rn_scroll_to(rn_t* s, int top) {
    int total = log_rows(&g_ses);
    int max = total > s->vis ? total - s->vis : 0;
    if (top < 0) top = 0;
    if (top >= max) { top = max; s->follow = true; } else s->follow = false;
    s->top = top;
}

// ---------------------------------------------------------------------------
// Отрисовка.
// ---------------------------------------------------------------------------
static void info_row(const gfx_target_t* t, int x, int y, int value_x, int max_w,
                     const char* label, const char* value) {
    const ui_runtime_theme_t* h = ui_theme_current();
    pk_text(t, x, y, label, h->muted, UI_FONT_14);
    pk_text_clip(t, value_x, y, max_w, value, h->ink, UI_FONT_14, true);
}

static void fmt_size(char* out, int cap, uint32_t bytes) {
    out[0] = 0;
    if (bytes < 1024 * 1024) { p_cat_num(out, (bytes + 512) / 1024, cap); p_cat(out, " КБ", cap); }
    else { p_cat_num(out, bytes >> 20, cap); p_cat(out, " МБ", cap); }
}

static void draw_library(window_t* w, const gfx_target_t* t, rn_t* s, const rn_layout_t* L) {
    const ui_runtime_theme_t* h = ui_theme_current();
    rn_session_t* g = &g_ses;
    char buf[200];
    int cores = smp_pool_workers();

    // ---- исполнитель ----
    pk_card(t, L->host);
    {
        int x = L->host.x + 28, y = L->host.y + 22;
        int vx = x + 170, mw = L->host.w - 170 - 56;
        pk_label(t, x, y, "ИСПОЛНИТЕЛЬ ПРОГРАММ");
        ui_draw_bitmap_scaled(t, x, y + 30, 56, 56, ui_icon(UI_ICON_RUNE));
        pk_text_bold(t, x + 74, y + 30, "Rune", h->ink, UI_FONT_20);
        pk_text(t, x + 74, y + 60, "Программы WebAssembly", h->muted, UI_FONT_14);
        pk_chip(t, x, y + 104, cores > 0 ? "Программа - на своём ядре" : "Одно ядро: интерфейс ждёт программу",
                cores > 0 ? 0xFF46A171 : h->accent_orange);
        info_row(t, x, y + 146, vx, mw, "Одновременно", "одна программа");
        info_row(t, x, y + 172, vx, mw, "Память программы", "до 64 МБ");
        info_row(t, x, y + 198, vx, mw, "Время счёта", cores > 0 ? "без предела" : "до 10 с");
    }

    // ---- как работать ----
    pk_card(t, L->tips);
    {
        static const char* const tip[] = {
            "Выберите программу и нажмите «Запустить».",
            "Программа видит один каталог диска как «/».",
            "Вводите строки внизу консоли, Enter - отправить.",
            "Esc закрывает окно - программа работает дальше.",
            "Свои программы: файлы *.wasm в /vault/runes.",
        };
        int x = L->tips.x + 28, y = L->tips.y + 22;
        pk_label(t, x, y, "КАК РАБОТАТЬ");
        for (int i = 0; i < 5; i++) {
            gfx_fill_round_rect(t, x, y + 36 + i * 30 + 6, 6, 6, 3, h->accent_coral);
            pk_text_clip(t, x + 18, y + 34 + i * 30, L->tips.w - 28 - 18 - 28, tip[i], h->ink, UI_FONT_14, false);
        }
    }

    // ---- программы ----
    pk_card(t, L->list);
    pk_label(t, L->list.x + 28, L->list.y + 22, "ПРОГРАММЫ");
    if (!s->n_prog) {
        pk_text(t, L->list.x + 28, L->list.y + 64, "Программ нет.", h->ink, UI_FONT_16);
        pk_text_clip(t, L->list.x + 28, L->list.y + 94, L->list.w - 56,
                     "Положите файл *.wasm (программу WASI) в папку /vault/runes системного диска",
                     h->muted, UI_FONT_14, false);
        pk_text_clip(t, L->list.x + 28, L->list.y + 118, L->list.w - 56,
                     "и нажмите «Обновить список».", h->muted, UI_FONT_14, false);
    }
    // Список длиннее окна прокручивается за выбранной строкой.
    int first = s->sel >= L->rows ? s->sel - L->rows + 1 : 0;
    for (int i = 0; i < L->rows && first + i < s->n_prog; i++) {
        const rune_prog_t* pr = &s->prog[first + i];
        pk_rect_t r = L->row[i];
        bool sel = (first + i == s->sel);
        pk_state_t st = pk_pointer_state(w, r, false);
        if (sel) {
            gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 10, h->accent_coral);
            gfx_fill_round_rect(t, r.x + 2, r.y + 2, r.w - 4, r.h - 4, 8, h->surface_soft);
        } else if (st != PK_IDLE) {
            gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 10, h->surface_soft);
        }
        gfx_fill_round_rect(t, r.x + 14, r.y + 14, 24, 24, 12, sel ? h->accent_coral : h->border);
        gfx_fill_round_rect(t, r.x + 22, r.y + 22, 8, 8, 4, h->surface);
        pk_text_clip(t, r.x + 52, r.y + 7, r.w - 52 - 16, pr->name, h->ink, UI_FONT_16, sel);
        buf[0] = 0;
        if (pr->about[0]) { p_cat(buf, pr->about, sizeof(buf)); p_cat(buf, " \xB7 ", sizeof(buf)); }
        char sz[24];
        fmt_size(sz, sizeof(sz), pr->bytes);
        p_cat(buf, sz, sizeof(buf));
        p_cat(buf, pr->builtin ? " \xB7 встроенная" : " \xB7 диск, /vault/runes", sizeof(buf));
        pk_text_clip(t, r.x + 52, r.y + 29, r.w - 52 - 16, buf, h->muted, UI_FONT_12, false);
    }
    if (s->n_prog > L->rows) {
        buf[0] = 0;
        p_cat_num(buf, (uint64_t)(s->sel + 1), sizeof(buf)); p_cat(buf, " из ", sizeof(buf));
        p_cat_num(buf, (uint64_t)s->n_prog, sizeof(buf));
        pk_text(t, L->list.x + L->list.w - 28 - ui_font_text_width(buf, UI_FONT_12), L->list.y + 22, buf, h->muted, UI_FONT_12);
    }

    // ---- запуск ----
    pk_card(t, L->launch);
    pk_label(t, L->f_args.x, L->launch.y + 22, "АРГУМЕНТЫ ПРОГРАММЫ - НЕОБЯЗАТЕЛЬНО");
    pk_field(t, L->f_args, g->args, "через пробел", s->focus == F_ARGS, false, false, UI_FONT_16);
    pk_label(t, L->f_dir.x, L->launch.y + 96, "КАТАЛОГ ПРОГРАММЫ");
    pk_label(t, L->seg[0].x, L->launch.y + 96, "ДОСТУП К НЕМУ");
    pk_label(t, L->f_mem.x, L->launch.y + 96, "ПАМЯТЬ, МБ");
    pk_field(t, L->f_dir, g->access == ACC_NONE ? "" : g->dir, g->access == ACC_NONE ? "каталога нет" : "/disk/...",
             s->focus == F_DIR, false, g->access == ACC_NONE, UI_FONT_16);
    static const char* const acc[3] = { "Запись", "Чтение", "Без файлов" };
    for (int i = 0; i < 3; i++)
        pk_segment(t, L->seg[i], acc[i], g->access == i, pk_pointer_state(w, L->seg[i], false));
    pk_field(t, L->f_mem, g->mem, "64", s->focus == F_MEM, false, false, UI_FONT_16);
    pk_button(t, L->b_start, "Запустить", true, pk_pointer_state(w, L->b_start, !s->n_prog));
    pk_button(t, L->b_refresh, "Обновить список", false, pk_pointer_state(w, L->b_refresh, false));

    if (s->status[0]) {
        uint32_t c = s->status_kind == ST_ERR ? h->accent_berry
                   : s->status_kind == ST_OK  ? 0xFF2F8A5B : h->muted;
        gfx_fill_round_rect(t, L->status.x, L->status.y, 4, L->status.h, 2, c);
        pk_text_clip(t, L->status.x + 16, L->status.y + 11, L->status.w - 16, s->status,
                     s->status_kind == ST_INFO ? h->ink : c, UI_FONT_14, s->status_kind != ST_INFO);
    }
    pk_text(t, 78, w->height - 34,
            "Вверх/вниз - программа   Tab - поля запуска   Enter - запустить   R - обновить   Esc - закрыть",
            h->muted, UI_FONT_12);
}

// Цвета консоли: тёмная панель в любой теме — как экран гостя в Phantom.
#define CON_BG    0xFF0E0C12
#define CON_INK   0xFFE6E1EA
#define CON_SYS   0xFF8E8796
#define CON_DIM   0xFF625C68

static void draw_console(window_t* w, const gfx_target_t* t, rn_t* s, const rn_layout_t* L) {
    const ui_runtime_theme_t* h = ui_theme_current();
    rn_session_t* g = &g_ses;
    const rune_state_t* q = &s->q;
    bool ours = g->mine || g->ended;
    bool ended = g->ended && !q->active;
    char buf[240];
    int x0 = 78;

    // ---- полоса состояния ----
    const char* name = g->name;
    if (!ours) {                                        // программа терминала: имя файла из её пути
        name = q->path;
        for (const char* c = q->path; *c; c++) if (*c == '/') name = c + 1;
    }
    int name_w = L->b2.x - x0 - 24;
    pk_text_clip(t, x0, 92, name_w > 520 ? 520 : name_w, name, h->ink, UI_FONT_20, true);
    int cx = x0 + ui_font_text_width(name, UI_FONT_20) + 18;
    if (cx > x0 + 540) cx = x0 + 540;
    const char* st; uint32_t dot;
    if (ended)                               { st = "Завершена";      dot = g->end_kind == RUNE_END_EXIT && !g->exit_code ? 0xFF46A171 : h->accent_berry; }
    else if (q->state == RUNE_ST_INPUT)      { st = "Ждёт ввода";     dot = h->accent_orange; }
    else if (q->state == RUNE_ST_SLEEPING)   { st = "Спит";           dot = h->accent_orange; }
    else                                     { st = "Считает";        dot = 0xFF46A171; }
    if (cx + 200 < L->b2.x) pk_chip(t, cx, 94, st, dot);

    if (ended) {
        pk_button(t, L->b2, "Запустить снова", false, pk_pointer_state(w, L->b2, false));
        pk_button(t, L->b1, "К библиотеке", true, pk_pointer_state(w, L->b1, false));
    } else {
        pk_button(t, L->b2, "Конец ввода", false, pk_pointer_state(w, L->b2, !g->mine || !q->input_open));
        pk_button(t, L->b1, "Остановить", false, pk_pointer_state(w, L->b1, false));
    }

    // ---- панель с выводом ----
    gfx_fill_round_rect(t, L->panel.x - 2, L->panel.y - 2, L->panel.w + 4, L->panel.h + 4, 16, h->border);
    gfx_fill_round_rect(t, L->panel.x, L->panel.y, L->panel.w, L->panel.h, 14, CON_BG);
    int lh = ui_font_line_height(s->font);
    if (!ours) {
        // Программу запустили из терминала: её консоль — там, сюда вывод не
        // приходит. Окно показывает состояние и умеет остановить.
        const char* a = q->owner ? "Эта программа запущена из терминала - её вывод и ввод там."
                                 : "Эта программа запущена в фоне (rune run -b) - её вывод идёт в журнал ядра.";
        const char* b = "Здесь видно её состояние и есть кнопка «Остановить».";
        int mx = L->panel.x + L->panel.w / 2, my = L->panel.y + L->panel.h / 2;
        pk_text(t, mx - ui_font_text_width(a, UI_FONT_16) / 2, my - 24, a, CON_INK, UI_FONT_16);
        pk_text(t, mx - ui_font_text_width(b, UI_FONT_14) / 2, my + 8, b, CON_SYS, UI_FONT_14);
    } else {
        int total = log_rows(g);
        if (s->follow) s->top = total > s->vis ? total - s->vis : 0;
        if (s->top > total) s->top = 0;
        for (int i = 0; i < s->vis && s->top + i < total; i++) {
            int r = s->top + i;
            if (!log_row_text(g, r, buf, sizeof(buf))) continue;
            pk_text(t, L->text.x, L->text.y + i * lh, buf, (g->row[r] & ROW_SYS) ? CON_SYS : CON_INK, s->font);
        }
        if (!total && !ended) {
            const char* a = "Программа пока ничего не вывела";
            pk_text(t, L->text.x, L->text.y, a, CON_DIM, s->font);
        }
        // Полоса прокрутки — только когда есть что прокручивать.
        if (total > s->vis) {
            int tx = L->panel.x + L->panel.w - 14, ty = L->panel.y + 14, th = L->panel.h - 28;
            int kh = th * s->vis / total;
            if (kh < 24) kh = 24;
            int ky = ty + (th - kh) * s->top / (total - s->vis);
            gfx_fill_round_rect(t, tx, ty, 6, th, 3, 0xFF2A2630);
            gfx_fill_round_rect(t, tx, ky, 6, kh, 3, s->follow ? CON_DIM : h->accent_coral);
        }
    }

    // ---- строка ввода ----
    bool can_type = g->mine && q->active && q->input_open;
    pk_field(t, L->input, s->input,
             ended ? "программа завершена"
             : !ours ? "ввод - в терминале программы"
             : !q->input_open ? "ввод закрыт" : "строка для программы",
             can_type, false, !can_type, s->font);
    pk_button(t, L->b_send, "Отправить", can_type, pk_pointer_state(w, L->b_send, !can_type));

    // ---- подвал ----
    int fy = L->foot_y;
    int right = w->width - 70;
    buf[0] = 0;
    if (ended) {
        p_cat(buf, "счёт ", sizeof(buf)); p_cat_time(buf, g->end_cpu_ms, sizeof(buf));
        p_cat(buf, " \xB7 всего ", sizeof(buf)); p_cat_time(buf, g->end_wall_ms, sizeof(buf));
        p_cat(buf, " \xB7 память ", sizeof(buf)); p_cat_mem(buf, g->end_pages, sizeof(buf));
    } else {
        if (q->core) { p_cat(buf, "ядро ", sizeof(buf)); p_cat_num(buf, (uint64_t)q->core, sizeof(buf)); }
        else p_cat(buf, "главное ядро", sizeof(buf));
        p_cat(buf, q->jit ? " \xB7 JIT" : " \xB7 интерпретатор", sizeof(buf));
        p_cat(buf, " \xB7 счёт ", sizeof(buf)); p_cat_time(buf, q->cpu_ms, sizeof(buf));
        p_cat(buf, " \xB7 всего ", sizeof(buf)); p_cat_time(buf, q->wall_ms, sizeof(buf));
        p_cat(buf, " \xB7 память ", sizeof(buf)); p_cat_mem(buf, q->memory_pages, sizeof(buf));
        p_cat(buf, " из ", sizeof(buf));
        p_cat_num(buf, q->memory_limit_mb ? q->memory_limit_mb : RN_MEM_MAX_MB, sizeof(buf)); p_cat(buf, " МБ", sizeof(buf));
    }
    int tw = ui_font_text_width(buf, UI_FONT_14);
    pk_text(t, right - tw, fy, buf, h->muted, UI_FONT_14);

    int left_w = right - tw - 32 - x0;
    if (ended) {
        char why[160];
        ru_end(g, why, sizeof(why));
        bool good = g->end_kind == RUNE_END_EXIT && !g->exit_code;
        uint32_t c = good ? 0xFF2F8A5B : h->accent_berry;
        gfx_fill_round_rect(t, x0, fy - 3, 4, 24, 2, c);
        pk_text_clip(t, x0 + 16, fy, left_w - 16, why, c, UI_FONT_14, true);
    } else if (g->trimmed && ours) {
        pk_text_clip(t, x0, fy, left_w, "Вывод длинный: его начало отброшено.   PgUp/PgDn - прокрутка   Esc - закрыть окно",
                     h->muted, UI_FONT_14, false);
    } else {
        pk_text_clip(t, x0, fy, left_w,
                     ours ? "Enter - отправить   Ctrl+D - конец ввода   Ctrl+C - остановить   PgUp/PgDn - прокрутка   Esc - закрыть окно"
                          : "Ctrl+C - остановить   Esc - закрыть окно",
                     h->muted, UI_FONT_14, false);
    }
}

static int view_key(const rn_t* s) {
    return s->view == V_LIBRARY ? 0 : (g_ses.ended && !s->q.active) ? 2 : 1;
}

static void rn_draw(window_t* w, const gfx_target_t* t) {
    rn_t* s = w->user_data;
    const ui_runtime_theme_t* h = ui_theme_current();
    rn_layout_t L;
    rn_layout(w, s, &L);
    if (s->logged_key != view_key(s)) { s->logged_key = view_key(s); log_layout(w, s, &L); }

    gfx_clear(t, h->canvas);
    se_header(t, "RUNE - WEBASSEMBLY", s->view == V_LIBRARY ? "Программы" : "Программа", false);
    if (s->view == V_LIBRARY) draw_library(w, t, s, &L);
    else draw_console(w, t, s, &L);
    s->seen_change = g_ses.change;
}

// ---------------------------------------------------------------------------
// Ввод.
// ---------------------------------------------------------------------------
static void rn_click(window_t* w, int lx, int ly) {
    rn_t* s = w->user_data;
    rn_session_t* g = &g_ses;
    rn_layout_t L;
    rn_layout(w, s, &L);
    if (s->view == V_LIBRARY) {
        int first = s->sel >= L.rows ? s->sel - L.rows + 1 : 0;
        for (int i = 0; i < L.rows && first + i < s->n_prog; i++)
            if (pk_hit(L.row[i], lx, ly)) { rn_select(s, first + i); s->focus = F_NONE; return; }
        if (pk_hit(L.f_args, lx, ly)) { s->focus = F_ARGS; serial_puts("[rune-ui] focus args\n"); return; }
        if (pk_hit(L.f_dir, lx, ly)) {
            if (g->access != ACC_NONE) { s->focus = F_DIR; serial_puts("[rune-ui] focus dir\n"); }
            return;
        }
        if (pk_hit(L.f_mem, lx, ly)) { s->focus = F_MEM; serial_puts("[rune-ui] focus mem\n"); return; }
        for (int i = 0; i < 3; i++)
            if (pk_hit(L.seg[i], lx, ly)) {
                static const char* const nm[3] = { "read-write", "read-only", "no files" };
                g->access = i;
                if (i == ACC_NONE && s->focus == F_DIR) s->focus = F_NONE;
                serial_puts("[rune-ui] access "); serial_puts(nm[i]); serial_puts("\n");
                return;
            }
        if (pk_hit(L.b_start, lx, ly)) { rn_start(s); return; }
        if (pk_hit(L.b_refresh, lx, ly)) { rn_refresh(s); set_status(s, ST_INFO, "Список программ обновлён"); return; }
        s->focus = F_NONE;
        return;
    }
    bool ended = g->ended && !s->q.active;
    if (pk_hit(L.b1, lx, ly)) { if (ended) rn_back(s); else rn_stop(s); return; }
    if (pk_hit(L.b2, lx, ly)) { if (ended) { rn_back(s); rn_start(s); } else rn_eof(s); return; }
    if (pk_hit(L.b_send, lx, ly)) { rn_send(s); return; }
}

static void rn_scroll(window_t* w, int notches) {
    rn_t* s = w->user_data;
    if (s->view != V_CONSOLE) return;
    rn_scroll_to(s, s->top - notches * 3);
    wm_invalidate();
}

static void edit(char* buf, int cap, char c, bool digits) {
    int n = se_len(buf);
    if (c == '\b') { if (n > 0) buf[n - 1] = 0; return; }
    if (digits ? (c < '0' || c > '9') : (c < 0x20 || c > 0x7E)) return;
    if (n < cap - 1) { buf[n] = c; buf[n + 1] = 0; }
}

static void rn_key(window_t* w, char c) {
    rn_t* s = w->user_data;
    rn_session_t* g = &g_ses;
    if (c == 27) { se_close(w); return; }
    if (s->view == V_CONSOLE) {
        bool ended = g->ended && !s->q.active;
        if (c == KBD_KEY_PGUP) { rn_scroll_to(s, s->top - (s->vis > 2 ? s->vis - 2 : 1)); return; }
        if (c == KBD_KEY_PGDN) { rn_scroll_to(s, s->top + (s->vis > 2 ? s->vis - 2 : 1)); return; }
        if (c == KBD_KEY_HOME) { rn_scroll_to(s, 0); return; }
        if (c == KBD_KEY_END)  { rn_scroll_to(s, 1 << 30); return; }
        if (c == KBD_KEY_UP)   { rn_scroll_to(s, s->top - 1); return; }
        if (c == KBD_KEY_DOWN) { rn_scroll_to(s, s->top + 1); return; }
        // Ctrl+P / Ctrl+O — то же, что `rune screen` / `rune log`: видимые строки
        // консоли или весь её журнал — в журнал ядра (serial), не уходя из окна.
        if (c == 16) { app_rune_dump(false); return; }
        if (c == 15) { app_rune_dump(true); return; }
        if (ended) {
            if (c == '\n') rn_back(s);
            return;
        }
        if (c == 3) { rn_stop(s); return; }             // Ctrl+C
        if (c == 4) { rn_eof(s); return; }              // Ctrl+D
        if (!g->mine || !s->q.input_open) return;
        if (c == '\n') { rn_send(s); return; }
        edit(s->input, sizeof(s->input), c, false);
        return;
    }
    if (c == '\n') { rn_start(s); return; }
    if (c == '\t') { s->focus = (s->focus + 1) % F_COUNT; if (s->focus == F_DIR && g->access == ACC_NONE) s->focus = F_MEM; return; }
    if (c == KBD_KEY_BACKTAB) { s->focus = (s->focus + F_COUNT - 1) % F_COUNT; if (s->focus == F_DIR && g->access == ACC_NONE) s->focus = F_ARGS; return; }
    if (c == KBD_KEY_UP)   { rn_select(s, s->sel - 1); return; }
    if (c == KBD_KEY_DOWN) { rn_select(s, s->sel + 1); return; }
    if (s->focus == F_ARGS) { edit(g->args, sizeof(g->args), c, false); return; }
    if (s->focus == F_DIR)  { edit(g->dir, sizeof(g->dir), c, false); return; }
    if (s->focus == F_MEM)  { edit(g->mem, 3, c, true); return; }
    if (c == 'r' || c == 'R') { rn_refresh(s); set_status(s, ST_INFO, "Список программ обновлён"); }
}

// ---------------------------------------------------------------------------
// Проход цикла WM.
// ---------------------------------------------------------------------------
// Конец программы окна замечаем и БЕЗ окна (оно могло быть закрыто): иначе
// итог потерялся бы, а следующая программа из терминала выглядела бы «нашей».
static void ses_tick(const rune_state_t* q) {
    rn_session_t* g = &g_ses;
    if (!g->mine || q->active) return;
    g->mine = false;
    if (q->ends == g->ends0) return;                    // не наша: кончилась, не начавшись
    g->ended = true;
    g->end_kind = q->end_kind;
    g->exit_code = q->exit_code;
    p_copy(g->end_text, q->end_text, sizeof(g->end_text));
    g->end_cpu_ms = q->end_cpu_ms;
    g->end_wall_ms = q->end_wall_ms;
    g->end_pages = q->end_pages;
    char why[160];
    ru_end(g, why, sizeof(why));
    log_sys(g, why);
    serial_puts("[rune-ui] program ended: ");
    serial_puts(g->end_kind == RUNE_END_EXIT ? "exit " : "stopped ");
    if (g->end_kind == RUNE_END_EXIT) serial_print_dec((uint64_t)(uint32_t)g->exit_code); else serial_puts(g->end_text);
    serial_puts("\n");
}

void app_rune_tick(void) {
    if (!g_ses.inited) return;                          // окно ни разу не открывали
    rune_state_t q;
    if (!g_rn) {
        if (g_ses.mine) { rune_query(&q); ses_tick(&q); }
        return;
    }
    rn_t* s = g_rn;
    rune_query(&s->q);
    ses_tick(&s->q);
    s->view = (s->q.active || g_ses.ended) ? V_CONSOLE : V_LIBRARY;

    bool dirty = false;
    int key = view_key(s);
    if (key != s->last_key) {
        static const char* const nm[] = { "library", "running", "ended" };
        serial_puts("[rune-ui] state "); serial_puts(nm[key]); serial_puts("\n");
        if (key == 1) s->follow = true;
        s->last_key = key;
        dirty = true;
    }
    if (!s->win->visible) return;
    if (s->view == V_CONSOLE) {
        if (g_ses.change != s->seen_change) dirty = true;
        // Счётчики в подвале — раз в секунду, а состояние («считает / ждёт
        // ввода») — сразу: оно входит в ключ ниже.
        uint64_t sec = s->q.active ? s->q.wall_ms / 1000 * 8 + (uint64_t)s->q.state : 0;
        if (sec != s->last_sec) { s->last_sec = sec; dirty = true; }
    }
    if (dirty) wm_invalidate();
}

// `rune screen` — что сейчас видно в консоли окна, `rune log` — весь журнал
// (по экранным строкам). Для теста: окно само в serial свой текст не печатает.
void app_rune_dump(bool whole) {
    rn_session_t* g = &g_ses;
    char buf[RN_COLS_MAX + 8];
    if (!g->inited) { serial_puts("[rune-ui] screen: the window was never opened\n"); return; }
    int total = log_rows(g);
    int a = 0, b = total;
    if (!whole) {
        if (!g_rn) { serial_puts("[rune-ui] screen: the window is closed\n"); return; }
        a = g_rn->follow ? (total > g_rn->vis ? total - g_rn->vis : 0) : g_rn->top;
        b = a + g_rn->vis < total ? a + g_rn->vis : total;
    }
    for (int r = a; r < b; r++) {
        (void)log_row_text(g, r, buf, sizeof(buf));
        serial_puts(whole ? "[rune-log] " : "[rune-scr] ");
        serial_print_dec((uint64_t)(r / 1000 % 10)); serial_print_dec((uint64_t)(r / 100 % 10));
        serial_print_dec((uint64_t)(r / 10 % 10)); serial_print_dec((uint64_t)(r % 10));
        serial_puts((g->row[r] & ROW_SYS) ? "#" : "|"); serial_puts(buf); serial_puts("\n");
    }
    serial_puts(whole ? "[rune-ui] log: rows=" : "[rune-ui] screen: rows=");
    serial_print_dec((uint64_t)(b - a));
    serial_puts(" top="); serial_print_dec((uint64_t)a);
    serial_puts(" total="); serial_print_dec((uint64_t)total);
    serial_puts(" cols="); serial_print_dec((uint64_t)g->cols);
    serial_puts(g->trimmed ? " trimmed\n" : "\n");
}

static void rn_destroy(window_t* w) {
    rn_t* s = w->user_data;
    if (g_rn == s) g_rn = 0;
    // Программа остаётся: её владелец и журнал — в сессии, а не в окне.
    serial_puts("[rune-ui] closed\n");
}

window_t* app_rune_open(void) {
    if (g_rn) {                                         // окно уже есть — просто показать его
        g_rn->win->visible = true;
        wm_focus(g_rn->win);
        wm_invalidate();
        return g_rn->win;
    }
    if (!ses_init()) { serial_puts("[rune-ui] no memory for the console log\n"); return 0; }
    rn_t* s = kmalloc(sizeof(*s));
    if (!s) return 0;
    for (size_t i = 0; i < sizeof(*s); i++) ((uint8_t*)s)[i] = 0;
    s->logged_key = -1;
    s->last_key = -1;
    s->follow = true;
    window_t* w = se_workspace("Rune", rn_draw, rn_click, rn_key, s, false);
    if (!w) { kfree(s); return 0; }
    s->win = w;
    w->on_destroy = rn_destroy;
    w->on_scroll = rn_scroll;

    // Шрифт консоли: крупный, если в панель входит хотя бы 100 знаков, иначе
    // мелкий. Ширина консоли в знакоместах — по окну; журнал, нарезанный под
    // другую ширину (экран сменил режим), режется заново.
    rn_layout_t L;
    rn_layout(w, s, &L);
    s->font = UI_FONT_MONO_16;
    if (L.text.w / ui_font_faces[UI_FONT_MONO_16].mono_adv < 100) s->font = UI_FONT_MONO_14;
    int adv = ui_font_faces[s->font].mono_adv ? ui_font_faces[s->font].mono_adv : 8;
    int cols = L.text.w / adv;
    if (cols > RN_COLS_MAX) cols = RN_COLS_MAX;
    if (cols < 20) cols = 20;
    s->vis = L.text.h / ui_font_line_height(s->font);
    if (s->vis < 1) s->vis = 1;
    if (cols != g_ses.cols) { g_ses.cols = cols; log_rebuild(&g_ses); }

    g_rn = s;
    rune_query(&s->q);
    s->view = (s->q.active || g_ses.ended) ? V_CONSOLE : V_LIBRARY;
    rn_refresh(s);
    serial_puts("[rune-ui] open, console ");
    serial_print_dec((uint64_t)cols); serial_puts("x"); serial_print_dec((uint64_t)s->vis);
    serial_puts(s->font == UI_FONT_MONO_16 ? ", font 16\n" : ", font 14\n");
    return w;
}

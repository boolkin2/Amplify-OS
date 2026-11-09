// ============================================================================
// app_phantom.c — Phantom: окно гипервизора AMPLIFY-OS (подэтап 50.5).
// ----------------------------------------------------------------------------
// До 50.5 гость жил только в терминале: `phantom run`, экран — дампом в serial,
// ввод — командой `phantom type`. Это приложение — то место, куда заходят,
// чтобы работать с гостевой системой:
//   «Библиотека» — что это за машина (есть ли VT-x, сколько памяти), список
//       образов *.iso с диска (/vault/phantom) и с установочного носителя
//       (/guests), строка для загрузчика, кнопка «Запустить»;
//   «Консоль»    — текстовый экран гостя 80x25, его состояние и счётчики,
//       кнопки «Остановить» и «Ctrl+Alt+Del».
// Вид выбирается сам: нет гостя — библиотека, есть — консоль. Закрытие окна
// гостя не трогает: он работает дальше, а окно, открытое снова, показывает
// его экран.
//
// ЭКРАН. Гость рисует сам — в свою видеопамять B8000 (символ + атрибут). Мы
// раз за проход цикла WM снимаем 2000 ячеек (phantom_screen_cells), сравниваем
// с прошлым снимком и перерисовываем в СВОЙ буфер только изменившиеся
// знакоместа; в кадре окна буфер просто копируется. Шрифт — phantom_font
// (DejaVu Sans Mono в кодировке гостя CP437), размер знакоместа — самый
// крупный, при котором экран помещается в окно целиком: ничего не
// растягивается. Рамки и блоки псевдографики рисуются линиями — иначе они не
// сходились бы на стыках знакомест.
//
// КЛАВИАТУРА. Гостю нужны не символы, а клавиши: нажатия и отпускания, Ctrl,
// Alt, стрелки, автоповтор. Поэтому, пока клавиатура «у гостя», окно ставит
// перехватчик сырых скан-кодов (keyboard_set_raw_sink): коды уходят прямо в
// контроллер 8042 гостя (phantom_key), мимо очереди символов ОС. Перехватчик
// исполняется в обработчике прерывания — в нём только флаги и запись в кольцо.
// Вернуть клавиатуру системе — ПРАВЫЙ CTRL (как «хост-клавиша» в VirtualBox)
// или щелчок мимо экрана. Клавиша Super гостю не отдаётся никогда: это
// клавиша оболочки Pearl.
//
// Раскладка считается одной функцией ph_layout() — ею пользуются и отрисовка,
// и обработчик щелчков. Координаты элементов печатаются в serial
// (`[phantom-ui] layout …`, в пикселях экрана) — по ним щёлкает тест.
// ============================================================================
#include "apps.h"
#include "app_stage_e.h"
#include "ui_kit.h"
#include "ui_fx.h"
#include "phantom.h"
#include "phantom_font.h"
#include "wm.h"
#include "heap.h"
#include "pmm.h"
#include "keyboard.h"
#include "timer.h"
#include "serial.h"
#include "mind_widget.h"
#include "beacon.h"
#include "constellation.h"
#include "ui_overlay.h"

#define PH_COLS       80
#define PH_ROWS       25
#define PH_CELLS      (PH_COLS * PH_ROWS)
#define PH_MAX_IMG    12
#define PH_LIST_MAX   6                 // строк списка образов на высоком экране
#define PH_BEZEL      14                // поле вокруг экрана гостя

enum { V_LIBRARY = 0, V_CONSOLE };
enum { ST_INFO = 0, ST_OK, ST_ERR };

typedef struct {
    window_t* win;
    // ---- библиотека ----
    phantom_image_t img[PH_MAX_IMG];
    int      n_img, sel;
    char     line[160];                 // строка для загрузчика гостя
    bool     line_focus;
    char     status[200];
    int      status_kind;
    // ---- экран гостя ----
    const pf_face_t* face;
    uint32_t* px;                       // 80*cell_w x 25*cell_h пикселей
    int      px_w, px_h;
    uint16_t shadow[PH_CELLS];          // что сейчас нарисовано в px
    bool     have_screen;
    int      cur_idx;                   // знакоместо, где нарисован курсор (-1 — нигде)
    // ---- состояние ----
    phantom_info_t info;
    int      view;
    int      last_state;
    uint64_t last_mb;                   // сколько МБ образа было прочитано на прошлом проходе
    uint64_t last_sec;                  // секунда работы гостя, показанная в подвале
    bool     want_capture;              // отдать клавиатуру гостю, как только он стартует
    int      logged_view;               // для какого вида раскладка уже в serial (-1 — ни для какого)
} ph_t;

typedef struct {
    // библиотека
    pk_rect_t host, tips;
    pk_rect_t list, row[PH_LIST_MAX];
    int       rows;
    pk_rect_t launch, field, b_start, b_refresh, status;
    // консоль
    pk_rect_t b_stop, b_cad;
    pk_rect_t bezel, screen;
    int       foot_y;
} ph_layout_t;

static ph_t* g_ph;                      // окно одно: гость тоже один

// Захват клавиатуры. Эти три переменные делят главный поток и обработчик
// прерывания клавиатуры (оба на BSP): главный поток меняет их с IF=0.
static volatile bool g_cap;             // скан-коды идут гостю
static volatile bool g_cap_drop;        // нажат правый Ctrl: вернуть клавиатуру системе
static uint8_t g_down[2][16];           // какие клавиши гость считает нажатыми ([1] — с префиксом E0)

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
static void p_cat_2d(char* d, uint64_t v, int cap) {
    char t[3] = { (char)('0' + (v / 10) % 10), (char)('0' + v % 10), 0 };
    p_cat(d, t, cap);
}
static void set_status(ph_t* s, int kind, const char* text) {
    p_copy(s->status, text, sizeof(s->status));
    s->status_kind = kind;
}

static inline uint64_t irq_off(void) {
    uint64_t f;
    __asm__ __volatile__("pushfq; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_back(uint64_t f) {
    if (f & 0x200) __asm__ __volatile__("sti");
}

// Причины отказа и конца гостя приходят из нижнего слоя по-английски (те же
// строки идут в serial, по ним судят тесты). В окне говорим по-русски.
static const char* ru_reason(const char* e) {
    if (!e || !e[0]) return "";
    if (se_contains(e, "no usable VT-x"))        return "На этой машине нет пригодного Intel VT-x";
    if (se_contains(e, "second CPU core"))       return "Гостю нужно второе ядро процессора";
    if (se_contains(e, "already running"))       return "Гость уже запущен";
    if (se_contains(e, "background guest"))      return "Ядро занято проверочным гостем (phantom stop)";
    if (se_contains(e, "not found"))             return "Файл образа не найден";
    if (se_contains(e, "too small"))             return "Файл слишком мал для образа диска";
    if (se_contains(e, "not enough free RAM"))   return "Не хватает свободной памяти для образа и гостя";
    if (se_contains(e, "no contiguous RAM"))     return "Нет цельного куска памяти под образ";
    if (se_contains(e, "CD001"))                 return "Это не образ ISO 9660";
    if (se_contains(e, "reading the ISO"))       return "Ошибка чтения образа с диска";
    if (se_contains(e, "not bootable"))          return "Образ не загрузочный: на нём нет записи El Torito";
    if (se_contains(e, "non-BIOS platform"))     return "Образ грузится только через UEFI, а у гостя BIOS";
    if (se_contains(e, "emulation is not"))      return "Образ грузится в режиме эмуляции дискеты - такого у гостя нет";
    if (se_contains(e, "boot line is too long")) return "Строка для загрузчика слишком длинная";
    return e;
}

static const char* ru_end(const char* e) {
    if (se_contains(e, "asked for a reset"))     return "Гость перезагрузился. Перезагрузка гостя здесь означает его конец";
    if (se_contains(e, "halted itself"))         return "Гость выключился (остановил процессор)";
    if (se_contains(e, "triple fault"))          return "Сбой гостя: тройная ошибка процессора";
    if (se_contains(e, "outside its RAM"))       return "Гость обратился к памяти за пределами своей";
    if (se_contains(e, "nothing to boot"))       return "BIOS гостя: на образе не с чего загружаться";
    if (se_contains(e, "BIOS to reboot"))        return "Гость попросил BIOS о перезагрузке (Ctrl+Alt+Del)";
    if (se_contains(e, "CPU exception"))         return "Исключение процессора в загрузчике гостя";
    if (se_contains(e, "could not continue"))    return "Гипервизор не смог продолжить работу гостя";
    return "Гость остановлен";
}

// ---------------------------------------------------------------------------
// Экран гостя: знакоместо -> пиксели своего буфера.
// ---------------------------------------------------------------------------
// Шестнадцать цветов текстового режима VGA. Чёрный чуть подкрашен — под тёмные
// поверхности Pearl; остальные — как у адаптера, чтобы цветной вывод гостя
// выглядел привычно.
static const uint32_t k_vga[16] = {
    0xFF0E0C12, 0xFF2A52BE, 0xFF2FA35A, 0xFF2AA6A6, 0xFFC0443C, 0xFFA84CA8, 0xFFB87A2A, 0xFFB9B5BE,
    0xFF625C68, 0xFF6C8CFF, 0xFF6BE08A, 0xFF6BE0E0, 0xFFFF7A6E, 0xFFE884E8, 0xFFF2D36B, 0xFFFFFFFF,
};

static inline uint32_t mix(uint32_t bg, uint32_t fg, uint32_t a /*0..255*/) {
    uint32_t rb = (((fg & 0xFF00FF) * a + (bg & 0xFF00FF) * (255 - a)) >> 8) & 0xFF00FF;
    uint32_t g  = (((fg & 0x00FF00) * a + (bg & 0x00FF00) * (255 - a)) >> 8) & 0x00FF00;
    return 0xFF000000u | rb | g;
}

// Прямоугольник внутри знакоместа (x0, y0 — его левый верхний угол в буфере).
static void cell_rect(ph_t* s, int x0, int y0, int x, int y, int w, int h, uint32_t c) {
    int cw = s->face->cell_w, ch = s->face->cell_h;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > cw) w = cw - x;
    if (y + h > ch) h = ch - y;
    for (int j = 0; j < h; j++) {
        uint32_t* p = s->px + (size_t)(y0 + y + j) * s->px_w + x0 + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

// Рамки CP437 (B3h..DAh): какие линии выходят из центра знакоместа вверх,
// вправо, вниз и влево — 0 нет, 1 одинарная, 2 двойная.
#define BX(u, r, d, l) ((uint8_t)((u) | ((r) << 2) | ((d) << 4) | ((l) << 6)))
static const uint8_t k_box[0xDB - 0xB3] = {
    BX(1,0,1,0), BX(1,0,1,1), BX(1,0,1,2), BX(2,0,2,1), BX(0,0,2,1), BX(0,0,1,2), BX(2,0,2,2), BX(2,0,2,0),
    BX(0,0,2,2), BX(2,0,0,2), BX(2,0,0,1), BX(1,0,0,2), BX(0,0,1,1),
    BX(1,1,0,0), BX(1,1,0,1), BX(0,1,1,1), BX(1,1,1,0), BX(0,1,0,1), BX(1,1,1,1), BX(1,2,1,0), BX(2,1,2,0),
    BX(2,2,0,0), BX(0,2,2,0), BX(2,2,0,2), BX(0,2,2,2), BX(2,2,2,0), BX(0,2,0,2), BX(2,2,2,2), BX(1,2,0,2),
    BX(2,1,0,1), BX(0,2,1,2), BX(0,1,2,1), BX(2,1,0,0), BX(1,2,0,0), BX(0,2,1,0), BX(0,1,2,0), BX(2,1,2,1),
    BX(1,2,1,2), BX(1,0,0,1), BX(0,1,1,0),
};

// Псевдографика: тени, рамки, блоки. Двойная линия рисуется как толстая
// (три толщины), из середины которой потом вырезается просвет: углы и
// перекрёстки при этом получаются сами.
static void cell_graph(ph_t* s, int x0, int y0, uint8_t code, uint32_t fg, uint32_t bg) {
    int cw = s->face->cell_w, ch = s->face->cell_h;
    if (code <= 0xB2) {                                 // B0..B2: штриховка 25/50/75 %
        cell_rect(s, x0, y0, 0, 0, cw, ch, mix(bg, fg, code == 0xB0 ? 64 : code == 0xB1 ? 128 : 192));
        return;
    }
    if (code >= 0xDB) {                                 // DB..DF: сплошные блоки
        if (code == 0xDB) cell_rect(s, x0, y0, 0, 0, cw, ch, fg);
        else if (code == 0xDC) cell_rect(s, x0, y0, 0, ch / 2, cw, ch - ch / 2, fg);
        else if (code == 0xDD) cell_rect(s, x0, y0, 0, 0, cw / 2, ch, fg);
        else if (code == 0xDE) cell_rect(s, x0, y0, cw / 2, 0, cw - cw / 2, ch, fg);
        else cell_rect(s, x0, y0, 0, 0, cw, ch / 2, fg);
        return;
    }
    uint8_t b = k_box[code - 0xB3];
    int u = b & 3, r = (b >> 2) & 3, d = (b >> 4) & 3, l = (b >> 6) & 3;
    int t = cw / 8 > 0 ? cw / 8 : 1;                    // толщина одинарной линии
    int cx = (cw - t) / 2, cy = (ch - t) / 2;           // левый верхний угол центральной точки
    // одинарные
    if (u == 1) cell_rect(s, x0, y0, cx, 0, t, cy + t, fg);
    if (d == 1) cell_rect(s, x0, y0, cx, cy, t, ch - cy, fg);
    if (l == 1) cell_rect(s, x0, y0, 0, cy, cx + t, t, fg);
    if (r == 1) cell_rect(s, x0, y0, cx, cy, cw - cx, t, fg);
    // двойные: толстая полоса…
    if (u == 2) cell_rect(s, x0, y0, cx - t, 0, 3 * t, cy + 2 * t, fg);
    if (d == 2) cell_rect(s, x0, y0, cx - t, cy - t, 3 * t, ch - (cy - t), fg);
    if (l == 2) cell_rect(s, x0, y0, 0, cy - t, cx + 2 * t, 3 * t, fg);
    if (r == 2) cell_rect(s, x0, y0, cx - t, cy - t, cw - (cx - t), 3 * t, fg);
    // … и просвет в её середине
    if (u == 2) cell_rect(s, x0, y0, cx, 0, t, cy + t, bg);
    if (d == 2) cell_rect(s, x0, y0, cx, cy, t, ch - cy, bg);
    if (l == 2) cell_rect(s, x0, y0, 0, cy, cx + t, t, bg);
    if (r == 2) cell_rect(s, x0, y0, cx, cy, cw - cx, t, bg);
}

static void scr_put(ph_t* s, int idx, uint16_t cell, bool cursor) {
    const pf_face_t* f = s->face;
    int cw = f->cell_w, ch = f->cell_h;
    int x0 = (idx % PH_COLS) * cw, y0 = (idx / PH_COLS) * ch;
    uint8_t code = (uint8_t)cell, attr = (uint8_t)(cell >> 8);
    // Старший бит атрибута на адаптере — мигание; мы показываем его как
    // обычный фон из восьми тёмных цветов.
    uint32_t fg = k_vga[attr & 15], bg = k_vga[(attr >> 4) & 7];

    cell_rect(s, x0, y0, 0, 0, cw, ch, bg);
    if (code >= 0xB0 && code <= 0xDF) {
        cell_graph(s, x0, y0, code, fg, bg);
    } else {
        const pf_glyph_t* g = &f->glyphs[code];
        if (g->w && g->h) {
            uint32_t lut[16];
            for (int k = 0; k < 16; k++) lut[k] = mix(bg, fg, (uint32_t)k * 17);
            const uint8_t* cov = f->blob + g->off;
            int stride = (g->w + 1) / 2;
            for (int j = 0; j < g->h && g->top + j < ch; j++) {
                uint32_t* p = s->px + (size_t)(y0 + g->top + j) * s->px_w + x0 + g->left;
                const uint8_t* row = cov + j * stride;
                for (int i = 0; i < g->w && g->left + i < cw; i++) {
                    uint8_t a = (i & 1) ? (row[i >> 1] & 15) : (row[i >> 1] >> 4);
                    if (a) p[i] = lut[a];
                }
            }
        }
    }
    if (cursor) {
        // Курсор — подчёркивание, как у адаптера: восьмая часть знакоместа.
        int h = ch / 8 > 2 ? ch / 8 : 2;
        cell_rect(s, x0, y0, 0, ch - h - 1, cw, h, (attr & 15) == ((attr >> 4) & 7) ? k_vga[7] : fg);
    }
}

// Сверить буфер с видеопамятью гостя. true — что-то перерисовано.
static bool scr_sync(ph_t* s) {
    static uint16_t cells[PH_CELLS];
    int cc = -1, cr = -1;
    if (!s->px || !phantom_screen_cells(cells, &cc, &cr)) return false;
    // Курсор мигает, только пока клавиатура у гостя; иначе он горит ровно —
    // видно, где он, но окно не перерисовывается дважды в секунду впустую.
    bool lit = !g_cap || ((timer_get_uptime_ms() / 500) & 1) == 0;
    int cur = (cc >= 0 && lit && s->info.state == PHANTOM_RUNNING) ? cr * PH_COLS + cc : -1;
    bool changed = false;
    for (int i = 0; i < PH_CELLS; i++) {
        bool was = (i == s->cur_idx), is = (i == cur);
        if (s->have_screen && cells[i] == s->shadow[i] && was == is) continue;
        scr_put(s, i, cells[i], is);
        s->shadow[i] = cells[i];
        changed = true;
    }
    s->cur_idx = cur;
    s->have_screen = true;
    return changed;
}

// ---------------------------------------------------------------------------
// Клавиатура: захват и возврат.
// ---------------------------------------------------------------------------
// Обработчик прерывания клавиатуры (IF=0). Быстро и без печати.
static bool ph_raw(uint8_t sc, bool ext) {
    if (!g_cap) return false;
    uint8_t code = (uint8_t)(sc & 0x7F);
    bool up = sc & 0x80;
    if (ext && code == 0x1D) {                          // правый Ctrl — вернуть клавиатуру
        if (!up) g_cap_drop = true;
        return true;
    }
    // Super и всё, что нажато вместе с ним (Super+Space — терминал, Super+Tab
    // — список окон), остаётся оболочке Pearl: гость в текстовом режиме этой
    // клавишей не пользуется, а человеку нужен выход из окна без мыши.
    if (ext && (code == 0x5B || code == 0x5C)) return false;
    if (keyboard_super_pressed()) return false;
    uint8_t* bit = &g_down[ext ? 1 : 0][code >> 3];
    if (up) *bit &= (uint8_t)~(1u << (code & 7)); else *bit |= (uint8_t)(1u << (code & 7));
    phantom_key(sc, ext);
    return true;
}

static void cap_set(bool on) {
    if (on == g_cap && !g_cap_drop) return;
    uint64_t f = irq_off();
    if (!on) {
        // Клавиши, которые гость считает нажатыми, отпускаем за него: иначе
        // зажатый в момент возврата Shift или Ctrl остался бы у гостя навсегда.
        for (int e = 0; e < 2; e++)
            for (int c = 1; c < 128; c++)
                if (g_down[e][c >> 3] & (1u << (c & 7))) phantom_key((uint8_t)(c | 0x80), e != 0);
    }
    for (int i = 0; i < 16; i++) g_down[0][i] = g_down[1][i] = 0;
    bool was = g_cap;
    g_cap = on;
    g_cap_drop = false;
    irq_back(f);
    if (was != on) serial_puts(on ? "[phantom-ui] keyboard: guest\n" : "[phantom-ui] keyboard: host\n");
    wm_invalidate();
}

// «Три пальца» гостю: Ctrl и Alt вниз, Del вниз-вверх, Alt и Ctrl вверх.
static void send_cad(void) {
    uint64_t f = irq_off();
    phantom_key(0x1D, false); phantom_key(0x38, false);
    phantom_key(0x53, true);  phantom_key(0xD3, true);
    phantom_key(0xB8, false); phantom_key(0x9D, false);
    irq_back(f);
    serial_puts("[phantom-ui] sent Ctrl+Alt+Del\n");
}

// ---------------------------------------------------------------------------
// Раскладка.
// ---------------------------------------------------------------------------
static void ph_layout(const window_t* w, const ph_t* s, ph_layout_t* L) {
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
    L->rows = w->height >= 860 ? PH_LIST_MAX : 4;
    L->list = (pk_rect_t){ rx, top, rw, 54 + L->rows * 58 + 10 };
    for (int i = 0; i < PH_LIST_MAX; i++)
        L->row[i] = (pk_rect_t){ rx + 14, top + 48 + i * 58, rw - 28, 52 };
    L->launch = (pk_rect_t){ rx, L->list.y + L->list.h + 18, rw, 166 };
    L->field  = (pk_rect_t){ rx + 28, L->launch.y + 50, rw - 56, 38 };
    L->b_start   = (pk_rect_t){ rx + 28, L->launch.y + 106, 190, 40 };
    L->b_refresh = (pk_rect_t){ rx + 28 + 202, L->launch.y + 106, 190, 40 };
    L->status = (pk_rect_t){ rx, L->launch.y + L->launch.h + 16, rw, 40 };

    // ---- консоль ----
    L->b_stop = (pk_rect_t){ x1 - 170, 90, 170, 38 };
    L->b_cad  = (pk_rect_t){ x1 - 170 - 12 - 170, 90, 170, 38 };
    int sw = s->px_w, sh = s->px_h;
    int bw = sw + 2 * PH_BEZEL, bh = sh + 2 * PH_BEZEL;
    L->bezel  = (pk_rect_t){ (w->width - bw) / 2, 140, bw, bh };
    L->screen = (pk_rect_t){ L->bezel.x + PH_BEZEL, L->bezel.y + PH_BEZEL, sw, sh };
    L->foot_y = L->bezel.y + bh + 12;
}

// Координаты элементов — в serial, в пикселях ЭКРАНА (центры).
static void log_rect(const window_t* w, const char* name, pk_rect_t r) {
    serial_puts(" "); serial_puts(name); serial_puts("=");
    serial_print_dec((uint64_t)(w->x + r.x + r.w / 2)); serial_puts(",");
    serial_print_dec((uint64_t)(w->y + r.y + r.h / 2));
}

static void log_layout(const window_t* w, const ph_t* s, const ph_layout_t* L) {
    if (s->view == V_LIBRARY) {
        serial_puts("[phantom-ui] layout view=library");
        static const char* const nm[PH_LIST_MAX] = { "img0", "img1", "img2", "img3", "img4", "img5" };
        for (int i = 0; i < s->n_img && i < L->rows; i++) log_rect(w, nm[i], L->row[i]);
        log_rect(w, "line", L->field);
        log_rect(w, "start", L->b_start);
        log_rect(w, "refresh", L->b_refresh);
    } else {
        serial_puts("[phantom-ui] layout view=console");
        log_rect(w, "screen", L->screen);
        log_rect(w, "stop", L->b_stop);
        log_rect(w, "cad", L->b_cad);
        serial_puts(" cell="); serial_print_dec(s->face->cell_w);
        serial_puts("x"); serial_print_dec(s->face->cell_h);
        serial_puts(" size="); serial_print_dec((uint64_t)s->px_w);
        serial_puts("x"); serial_print_dec((uint64_t)s->px_h);
    }
    serial_puts("\n");
}

// ---------------------------------------------------------------------------
// Действия.
// ---------------------------------------------------------------------------
static void ph_refresh(ph_t* s) {
    s->n_img = phantom_images(s->img, PH_MAX_IMG);
    if (s->sel >= s->n_img) s->sel = s->n_img ? s->n_img - 1 : 0;
    s->logged_view = -1;                                // строк стало другое число — раскладку заново
    serial_puts("[phantom-ui] images: "); serial_print_dec((uint64_t)s->n_img);
    for (int i = 0; i < s->n_img; i++) {
        serial_puts(i ? "; " : " - "); serial_puts(s->img[i].name);
        serial_puts(" ("); serial_print_dec(s->img[i].bytes);
        serial_puts(s->img[i].on_medium ? " bytes, medium)" : " bytes, disk)");
    }
    serial_puts("\n");
}

static void ph_start(ph_t* s) {
    if (s->info.unavailable[0]) {
        set_status(s, ST_ERR, ru_reason(s->info.unavailable));
        serial_puts("[phantom-ui] start refused: "); serial_puts(s->info.unavailable); serial_puts("\n");
        return;
    }
    if (!s->n_img) { set_status(s, ST_ERR, "Запускать нечего: образов нет"); return; }
    const phantom_image_t* im = &s->img[s->sel];
    serial_puts("[phantom-ui] start "); serial_puts(im->path);
    if (s->line[0]) { serial_puts(" line: "); serial_puts(s->line); }
    serial_puts("\n");
    if (!phantom_run(0, im->path, s->line)) {
        phantom_info(&s->info);
        set_status(s, ST_ERR, ru_reason(s->info.error));
        return;
    }
    s->status[0] = 0;
    s->line_focus = false;
    s->want_capture = true;                             // гость стартует — клавиатура сразу ему
}

static void ph_stop(ph_t* s) {
    cap_set(false);
    s->want_capture = false;
    phantom_stop_guest();
    serial_puts("[phantom-ui] guest closed\n");
}

// ---------------------------------------------------------------------------
// Отрисовка.
// ---------------------------------------------------------------------------
static void fmt_mb(char* out, int cap, uint64_t bytes) {
    out[0] = 0;
    p_cat_num(out, (bytes + (1u << 19)) >> 20, cap);
    p_cat(out, " МБ", cap);
}

static void info_row(const gfx_target_t* t, int x, int y, int value_x, int max_w,
                     const char* label, const char* value) {
    const ui_runtime_theme_t* h = ui_theme_current();
    pk_text(t, x, y, label, h->muted, UI_FONT_14);
    pk_text_clip(t, value_x, y, max_w, value, h->ink, UI_FONT_14, true);
}

// Значок приложения: машина внутри машины — два вложенных экрана.
static void draw_mark(const gfx_target_t* t, int x, int y) {
    const ui_runtime_theme_t* h = ui_theme_current();
    gfx_fill_rect_gradient(t, x, y, 56, 56, h->accent_berry, h->accent_orange);
    gfx_fill_round_rect(t, x + 10, y + 13, 36, 26, 5, 0xFFFFFFFF);
    gfx_fill_round_rect(t, x + 13, y + 16, 30, 20, 3, h->accent_berry);
    gfx_fill_round_rect(t, x + 22, y + 22, 18, 11, 2, 0xFFFFFFFF);
    gfx_fill_rect(t, x + 20, y + 42, 16, 3, 0xFFFFFFFF);
}

static void draw_library(window_t* w, const gfx_target_t* t, ph_t* s, const ph_layout_t* L) {
    const ui_runtime_theme_t* h = ui_theme_current();
    char buf[200];
    bool ok = !s->info.unavailable[0];

    // ---- эта машина ----
    pk_card(t, L->host);
    {
        int x = L->host.x + 28, y = L->host.y + 22;
        int vx = x + 170, mw = L->host.w - 170 - 56;
        pk_label(t, x, y, "ГИПЕРВИЗОР");
        draw_mark(t, x, y + 30);
        pk_text_bold(t, x + 74, y + 30, "Phantom", h->ink, UI_FONT_20);
        pk_text(t, x + 74, y + 60, "Гости на Intel VT-x", h->muted, UI_FONT_14);
        pk_chip(t, x, y + 104, ok ? "VT-x готов" : "VT-x недоступен", ok ? 0xFF46A171 : h->accent_berry);
        if (ok) {
            fmt_mb(buf, sizeof(buf), pmm_free_ram());
            info_row(t, x, y + 146, vx, mw, "Свободно памяти", buf);
            info_row(t, x, y + 172, vx, mw, "Гостю достанется",
                     pmm_free_ram() >= (900ull << 20) ? "512 МБ, одно ядро" : "256 МБ, одно ядро");
            info_row(t, x, y + 198, vx, mw, "Экран и ввод", "текст 80x25, клавиатура");
        } else {
            pk_text_clip(t, x, y + 146, L->host.w - 56, ru_reason(s->info.unavailable), h->ink, UI_FONT_14, true);
            pk_text_clip(t, x, y + 172, L->host.w - 56, "Нужен процессор Intel с VT-x, EPT и вторым ядром.",
                         h->muted, UI_FONT_14, false);
            pk_text_clip(t, x, y + 198, L->host.w - 56, "В ВМ - включить вложенную виртуализацию.",
                         h->muted, UI_FONT_14, false);
        }
    }

    // ---- как работать ----
    pk_card(t, L->tips);
    {
        static const char* const tip[] = {
            "Выберите образ и нажмите «Запустить».",
            "Щелчок по экрану гостя отдаёт ему клавиатуру.",
            "Правый Ctrl возвращает клавиатуру системе.",
            "Esc закрывает окно - гость работает дальше.",
            "Образы: файлы *.iso в /vault/phantom.",
        };
        int x = L->tips.x + 28, y = L->tips.y + 22;
        pk_label(t, x, y, "КАК РАБОТАТЬ");
        for (int i = 0; i < 5; i++) {
            gfx_fill_round_rect(t, x, y + 36 + i * 30 + 6, 6, 6, 3, h->accent_coral);
            pk_text_clip(t, x + 18, y + 34 + i * 30, L->tips.w - 28 - 18 - 28, tip[i], h->ink, UI_FONT_14, false);
        }
    }

    // ---- образы ----
    pk_card(t, L->list);
    pk_label(t, L->list.x + 28, L->list.y + 22, "ОБРАЗЫ ГОСТЕЙ");
    if (!s->n_img) {
        pk_text(t, L->list.x + 28, L->list.y + 64, "Образов нет.", h->ink, UI_FONT_16);
        pk_text_clip(t, L->list.x + 28, L->list.y + 94, L->list.w - 56,
                     "Положите загрузочный *.iso в папку /vault/phantom системного диска",
                     h->muted, UI_FONT_14, false);
        pk_text_clip(t, L->list.x + 28, L->list.y + 118, L->list.w - 56,
                     "или вставьте установочный носитель AMPLIFY-OS - образы есть на нём.",
                     h->muted, UI_FONT_14, false);
    }
    for (int i = 0; i < s->n_img && i < L->rows; i++) {
        pk_rect_t r = L->row[i];
        bool sel = (i == s->sel);
        pk_state_t st = pk_pointer_state(w, r, false);
        if (sel) {
            gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 10, h->accent_coral);
            gfx_fill_round_rect(t, r.x + 2, r.y + 2, r.w - 4, r.h - 4, 8, h->surface_soft);
        } else if (st != PK_IDLE) {
            gfx_fill_round_rect(t, r.x, r.y, r.w, r.h, 10, h->surface_soft);
        }
        gfx_fill_round_rect(t, r.x + 14, r.y + 14, 24, 24, 12, sel ? h->accent_coral : h->border);
        gfx_fill_round_rect(t, r.x + 22, r.y + 22, 8, 8, 4, h->surface);
        pk_text_clip(t, r.x + 52, r.y + 7, r.w - 52 - 16, s->img[i].name, h->ink, UI_FONT_16, sel);
        fmt_mb(buf, sizeof(buf), s->img[i].bytes);
        p_cat(buf, s->img[i].on_medium ? " \xB7 установочный носитель" : " \xB7 диск, /vault/phantom", sizeof(buf));
        pk_text_clip(t, r.x + 52, r.y + 29, r.w - 52 - 16, buf, h->muted, UI_FONT_12, false);
    }
    if (s->n_img > L->rows) {
        buf[0] = 0;
        p_cat(buf, "и ещё ", sizeof(buf)); p_cat_num(buf, (uint64_t)(s->n_img - L->rows), sizeof(buf));
        pk_text(t, L->list.x + L->list.w - 110, L->list.y + 22, buf, h->muted, UI_FONT_12);
    }

    // ---- запуск ----
    pk_card(t, L->launch);
    pk_label(t, L->launch.x + 28, L->launch.y + 22, "СТРОКА ДЛЯ ЗАГРУЗЧИКА ГОСТЯ - НЕОБЯЗАТЕЛЬНО");
    pk_field(t, L->field, s->line, "пусто - загрузка по умолчанию", s->line_focus, false, false, UI_FONT_16);
    pk_button(t, L->b_start, "Запустить", true, pk_pointer_state(w, L->b_start, !ok || !s->n_img));
    pk_button(t, L->b_refresh, "Обновить список", false, pk_pointer_state(w, L->b_refresh, false));

    if (s->status[0]) {
        uint32_t c = s->status_kind == ST_ERR ? h->accent_berry
                   : s->status_kind == ST_OK  ? 0xFF2F8A5B : h->muted;
        gfx_fill_round_rect(t, L->status.x, L->status.y, 4, L->status.h, 2, c);
        pk_text_clip(t, L->status.x + 16, L->status.y + 11, L->status.w - 16, s->status,
                     s->status_kind == ST_INFO ? h->ink : c, UI_FONT_14, s->status_kind != ST_INFO);
    }
    pk_text(t, 78, w->height - 34,
            "Вверх/вниз - образ   Tab - строка загрузчика   Enter - запустить   R - обновить   Esc - закрыть",
            h->muted, UI_FONT_12);
}

static void draw_console(window_t* w, const gfx_target_t* t, ph_t* s, const ph_layout_t* L) {
    const ui_runtime_theme_t* h = ui_theme_current();
    const phantom_info_t* in = &s->info;
    char buf[200];
    int x0 = 78;

    // ---- полоса состояния ----
    int name_w = L->b_cad.x - x0 - 24;
    pk_text_clip(t, x0, 92, name_w > 520 ? 520 : name_w, in->image, h->ink, UI_FONT_20, true);
    int cx = x0 + ui_font_text_width(in->image, UI_FONT_20) + 18;
    if (cx > x0 + 540) cx = x0 + 540;
    const char* st; uint32_t dot;
    if (in->state == PHANTOM_LOADING)      { st = "Читаю образ";   dot = h->accent_orange; }
    else if (in->state == PHANTOM_ENDED)   { st = "Завершён";      dot = h->accent_berry; }
    else if (in->sleeping)                 { st = "Работает \xB7 простаивает"; dot = 0xFF46A171; }
    else                                   { st = "Работает";      dot = 0xFF46A171; }
    if (cx + 240 < L->b_cad.x) pk_chip(t, cx, 94, st, dot);

    pk_button(t, L->b_cad, "Ctrl+Alt+Del", false, pk_pointer_state(w, L->b_cad, in->state != PHANTOM_RUNNING));
    pk_button(t, L->b_stop, in->state == PHANTOM_LOADING ? "Отменить"
                          : in->state == PHANTOM_ENDED   ? "Закрыть гостя" : "Остановить",
              in->state == PHANTOM_ENDED, pk_pointer_state(w, L->b_stop, false));

    // ---- экран ----
    // Рамка экрана: при захвате клавиатуры — акцентная, чтобы было видно,
    // куда сейчас идут клавиши.
    gfx_fill_round_rect(t, L->bezel.x - 2, L->bezel.y - 2, L->bezel.w + 4, L->bezel.h + 4, 16,
                        g_cap ? h->accent_coral : h->border);
    gfx_fill_round_rect(t, L->bezel.x, L->bezel.y, L->bezel.w, L->bezel.h, 14, k_vga[0]);
    if (s->have_screen && s->px) {
        fx_blit(t, L->screen.x, L->screen.y, s->px, s->px_w, s->px_w, s->px_h, 255, false);
    } else {
        // Гостя ещё нет: образ читается в память.
        int mx = L->screen.x + L->screen.w / 2, my = L->screen.y + L->screen.h / 2;
        const char* title = "Читаю образ в память";
        pk_text_bold(t, mx - ui_font_text_width(title, UI_FONT_20) / 2, my - 52, title, 0xFFFFFFFF, UI_FONT_20);
        int bw = L->screen.w > 560 ? 480 : L->screen.w - 80;
        uint64_t total = in->image_bytes ? in->image_bytes : 1;
        se_progress(t, mx - bw / 2, my - 4, bw, (uint8_t)(in->loaded_bytes * 255 / total), 0xFF2A2630, h->accent_coral);
        buf[0] = 0;
        p_cat_num(buf, in->loaded_bytes >> 20, sizeof(buf)); p_cat(buf, " из ", sizeof(buf));
        p_cat_num(buf, (in->image_bytes + (1u << 19)) >> 20, sizeof(buf)); p_cat(buf, " МБ", sizeof(buf));
        pk_text(t, mx - ui_font_text_width(buf, UI_FONT_14) / 2, my + 18, buf, k_vga[7], UI_FONT_14);
        const char* hint = "Гость стартует сам, как только образ будет прочитан";
        pk_text(t, mx - ui_font_text_width(hint, UI_FONT_12) / 2, my + 46, hint, k_vga[8], UI_FONT_12);
    }

    // ---- подвал ----
    int fy = L->foot_y;
    if (in->state == PHANTOM_ENDED) {
        gfx_fill_round_rect(t, x0, fy + 2, 4, 28, 2, h->accent_berry);
        pk_text_clip(t, x0 + 16, fy + 7, w->width - x0 - 70 - 16, ru_end(in->end_text), h->accent_berry, UI_FONT_14, true);
    } else if (g_cap) {
        pk_chip(t, x0, fy + 2, "Клавиатура у гостя \xB7 правый Ctrl - вернуть", h->accent_coral);
    } else if (in->state == PHANTOM_RUNNING) {
        pk_text(t, x0, fy + 7, "Щёлкните по экрану или нажмите Enter - клавиатура перейдёт к гостю.   Esc - закрыть окно",
                h->muted, UI_FONT_14);
    }
    if (in->state == PHANTOM_RUNNING || in->state == PHANTOM_ENDED) {
        uint64_t sec = in->uptime_ms / 1000;
        buf[0] = 0;
        p_cat(buf, "ядро ", sizeof(buf)); p_cat_num(buf, (uint64_t)in->core, sizeof(buf));
        p_cat(buf, " \xB7 ", sizeof(buf)); p_cat_num(buf, in->cpu_pct, sizeof(buf)); p_cat(buf, " %", sizeof(buf));
        p_cat(buf, " \xB7 ", sizeof(buf)); p_cat_num(buf, in->ram_mb, sizeof(buf)); p_cat(buf, " МБ", sizeof(buf));
        p_cat(buf, " \xB7 диск ", sizeof(buf)); p_cat_num(buf, in->disk_mb, sizeof(buf)); p_cat(buf, " МБ", sizeof(buf));
        p_cat(buf, " \xB7 ", sizeof(buf));
        if (sec >= 3600) { p_cat_num(buf, sec / 3600, sizeof(buf)); p_cat(buf, ":", sizeof(buf)); }
        p_cat_2d(buf, (sec / 60) % 60, sizeof(buf)); p_cat(buf, ":", sizeof(buf)); p_cat_2d(buf, sec % 60, sizeof(buf));
        int tw = ui_font_text_width(buf, UI_FONT_14);
        if (!g_cap || tw + 420 < w->width - x0 - 70)
            pk_text(t, w->width - 70 - tw, fy + 7, buf, h->muted, UI_FONT_14);
    }
}

static void ph_draw(window_t* w, const gfx_target_t* t) {
    ph_t* s = w->user_data;
    const ui_runtime_theme_t* h = ui_theme_current();
    ph_layout_t L;
    ph_layout(w, s, &L);
    if (s->logged_view != s->view) { s->logged_view = s->view; log_layout(w, s, &L); }

    gfx_clear(t, h->canvas);
    se_header(t, "PHANTOM - HYPERVISOR", s->view == V_LIBRARY ? "Гости" : "Гость", false);
    if (s->view == V_LIBRARY) draw_library(w, t, s, &L);
    else draw_console(w, t, s, &L);
}

// ---------------------------------------------------------------------------
// Ввод.
// ---------------------------------------------------------------------------
static void ph_click(window_t* w, int lx, int ly) {
    ph_t* s = w->user_data;
    ph_layout_t L;
    ph_layout(w, s, &L);
    if (s->view == V_LIBRARY) {
        for (int i = 0; i < s->n_img && i < L.rows; i++)
            if (pk_hit(L.row[i], lx, ly)) {
                s->sel = i;
                s->line_focus = false;
                serial_puts("[phantom-ui] selected "); serial_puts(s->img[i].name); serial_puts("\n");
                return;
            }
        if (pk_hit(L.field, lx, ly)) { s->line_focus = true; serial_puts("[phantom-ui] focus line\n"); return; }
        if (pk_hit(L.b_start, lx, ly)) { ph_start(s); return; }
        if (pk_hit(L.b_refresh, lx, ly)) { ph_refresh(s); set_status(s, ST_INFO, "Список образов обновлён"); return; }
        s->line_focus = false;
        return;
    }
    if (pk_hit(L.b_stop, lx, ly)) { ph_stop(s); return; }
    if (pk_hit(L.b_cad, lx, ly)) { if (s->info.state == PHANTOM_RUNNING) send_cad(); return; }
    if (pk_hit(L.bezel, lx, ly)) {
        if (s->info.state == PHANTOM_RUNNING) cap_set(true);
        return;
    }
    cap_set(false);                                     // щелчок мимо экрана — клавиатура системе
}

// Сюда клавиши приходят, только пока клавиатура у системы: при захвате
// скан-коды до очереди символов не доходят.
static void ph_key(window_t* w, char c) {
    ph_t* s = w->user_data;
    if (c == 27) { se_close(w); return; }
    if (s->view == V_CONSOLE) {
        if (c == '\n' && s->info.state == PHANTOM_RUNNING) cap_set(true);
        return;
    }
    if (c == '\n') { ph_start(s); return; }
    if (c == '\t' || c == KBD_KEY_BACKTAB) { s->line_focus = !s->line_focus; return; }
    if (c == KBD_KEY_UP)   { if (s->sel > 0) s->sel--; return; }
    if (c == KBD_KEY_DOWN) { if (s->sel + 1 < s->n_img) s->sel++; return; }
    if (s->line_focus) {
        int n = se_len(s->line);
        if (c == '\b') { if (n > 0) s->line[n - 1] = 0; return; }
        if (c >= 0x20 && c <= 0x7E && n < (int)sizeof(s->line) - 1) { s->line[n] = c; s->line[n + 1] = 0; }
        return;
    }
    if (c == 'r' || c == 'R') { ph_refresh(s); set_status(s, ST_INFO, "Список образов обновлён"); }
}

// ---------------------------------------------------------------------------
// Проход цикла WM.
// ---------------------------------------------------------------------------
void app_phantom_tick(void) {
    ph_t* s = g_ph;
    if (!s) return;
    window_t* w = s->win;
    phantom_info(&s->info);
    const phantom_info_t* in = &s->info;
    int view = in->state == PHANTOM_IDLE ? V_LIBRARY : V_CONSOLE;
    bool dirty = false;

    if ((int)in->state != s->last_state) {
        static const char* const nm[] = { "idle", "loading", "running", "ended" };
        serial_puts("[phantom-ui] state "); serial_puts(nm[in->state]); serial_puts("\n");
        if (in->state == PHANTOM_LOADING || in->state == PHANTOM_IDLE) { s->have_screen = false; s->cur_idx = -1; }
        // Отказ после чтения образа (не загрузочный и т.п.) приходит уже без
        // команды, которой можно ответить, — показываем его в библиотеке.
        if (in->state == PHANTOM_IDLE && in->error[0]) set_status(s, ST_ERR, ru_reason(in->error));
        if (in->state != PHANTOM_RUNNING) cap_set(false);
        if (in->state == PHANTOM_IDLE) s->want_capture = false;
        s->last_state = (int)in->state;
        dirty = true;
    }
    s->view = view;

    bool mine = w->visible && w->focused && wm_focused() == w && !mind_widget_visible() &&
                !beacon_visible() && !constellation_visible() && !ui_overlay_visible();
    if (g_cap_drop || (g_cap && !mine)) cap_set(false);
    if (s->want_capture && in->state == PHANTOM_RUNNING) {
        s->want_capture = false;
        if (mine) cap_set(true);
    }

    if (!w->visible) return;
    if (in->state == PHANTOM_LOADING) {
        uint64_t mb = in->loaded_bytes >> 20;
        if (mb != s->last_mb) { s->last_mb = mb; dirty = true; }
    } else if (in->state == PHANTOM_RUNNING || in->state == PHANTOM_ENDED) {
        if (scr_sync(s)) dirty = true;
        uint64_t sec = in->uptime_ms / 1000;            // подвал со счётчиками — раз в секунду
        if (in->state == PHANTOM_RUNNING && sec != s->last_sec) { s->last_sec = sec; dirty = true; }
    }
    if (dirty) wm_invalidate();
}

static void ph_destroy(window_t* w) {
    ph_t* s = w->user_data;
    cap_set(false);
    keyboard_set_raw_sink(0);
    if (s && s->px) kfree(s->px);
    if (g_ph == s) g_ph = 0;
    serial_puts("[phantom-ui] closed\n");
}

window_t* app_phantom_open(void) {
    if (g_ph) {                                         // окно уже есть — просто показать его
        g_ph->win->visible = true;
        wm_focus(g_ph->win);
        wm_invalidate();
        return g_ph->win;
    }
    ph_t* s = kmalloc(sizeof(*s));
    if (!s) return 0;
    for (size_t i = 0; i < sizeof(*s); i++) ((uint8_t*)s)[i] = 0;
    s->cur_idx = -1;
    s->logged_view = -1;
    s->last_state = -1;
    window_t* w = se_workspace("Phantom", ph_draw, ph_click, ph_key, s, false);
    if (!w) { kfree(s); return 0; }
    s->win = w;
    w->on_destroy = ph_destroy;

    // Самое крупное знакоместо, при котором экран 80x25 помещается в окно
    // под полосой состояния и над подвалом. Буфер экрана — на всё время окна.
    int aw = w->width - 2 * 40 - 2 * PH_BEZEL, ah = w->height - 140 - 52 - 2 * PH_BEZEL;
    int pick = 0;
    for (int i = 0; i < PHANTOM_FONT_FACES; i++)
        if (PH_COLS * phantom_font_faces[i].cell_w <= aw && PH_ROWS * phantom_font_faces[i].cell_h <= ah) pick = i;
    for (; pick >= 0 && !s->px; pick--) {
        s->face = &phantom_font_faces[pick];
        s->px_w = PH_COLS * s->face->cell_w;
        s->px_h = PH_ROWS * s->face->cell_h;
        s->px = kmalloc((size_t)s->px_w * (size_t)s->px_h * 4);
    }
    if (s->px) for (int i = 0; i < s->px_w * s->px_h; i++) s->px[i] = k_vga[0];

    g_ph = s;
    phantom_info(&s->info);
    phantom_clear_error();
    ph_refresh(s);
    keyboard_set_raw_sink(ph_raw);
    serial_puts("[phantom-ui] open, screen font ");
    serial_print_dec(s->face->cell_w); serial_puts("x"); serial_print_dec(s->face->cell_h);
    serial_puts(s->px ? "\n" : " (no memory for the screen buffer)\n");
    return w;
}

// ============================================================================
// phantom_bios.c — службы BIOS гостя phantom (этап 50.3). См. phantom_bios.h.
// ----------------------------------------------------------------------------
// Как вызов попадает сюда: гость делает INT n -> процессор гостя по таблице
// векторов прыгает на заглушку в F000:(n*4) -> там VMCALL -> выход в
// гипервизор -> платформа (phantom_pc) отдаёт его bios_vmcall. К этому
// моменту hv уже перешагнул VMCALL: счётчик команд гостя стоит на IRET
// заглушки, а в стеке гостя лежат IP, CS и FLAGS вызвавшего. Результат
// возвращается регистрами и правкой этих сохранённых FLAGS (перенос = ошибка).
//
// Всё здесь, кроме функций с пометкой BSP, исполняется на AP гостя: только
// память гостя, образ ISO в памяти хоста и регистры. Печатать нельзя.
// ============================================================================
#include "phantom_bios.h"
#include <stddef.h>

extern const uint8_t phantom_bios_start[];
extern const uint8_t phantom_bios_end[];
extern const uint8_t phantom_bios_int08[];
extern const uint8_t phantom_bios_int16[];
extern const uint8_t phantom_bios_int16_call[];

#define ROM_GPA     0xF0000u
#define TEXT_GPA    0xB8000u
#define EBDA_GPA    0x9FC00u
#define COLS        80
#define ROWS        25
#define CD_DRIVE    0xE0
#define CD_SECTOR   2048u

typedef struct { uint64_t addr, size; uint32_t type; } e820_t;

struct bios {
    hv_vm_t* vm;
    pc_t*    pc;
    const uint8_t* iso;
    uint64_t iso_bytes;
    uint8_t* ram;                   // память гостя с адреса 0
    uint64_t ram_bytes;
    uint32_t boot_lba;
    uint16_t boot_sectors;
    e820_t   map[4];
    int      map_n;
    int      s_row, s_col;          // где кончился текст, уже ушедший в поток консоли
    uint32_t int16_call;            // смещение VMCALL в заглушке INT 16h
    bios_stats_t st;
};

static struct bios g_bios;
static bool g_bios_used;

// ----------------------------------------------------------------------------
// Регистры и память гостя.
// ----------------------------------------------------------------------------
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint64_t rd64(const uint8_t* p) { return rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
static void wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t* p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }
static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

#define AL(r)  ((uint8_t)(r)->rax)
#define AH(r)  ((uint8_t)((r)->rax >> 8))
#define AX(r)  ((uint16_t)(r)->rax)
#define BL(r)  ((uint8_t)(r)->rbx)
#define BH(r)  ((uint8_t)((r)->rbx >> 8))
#define BX(r)  ((uint16_t)(r)->rbx)
#define CL(r)  ((uint8_t)(r)->rcx)
#define CH(r)  ((uint8_t)((r)->rcx >> 8))
#define CX(r)  ((uint16_t)(r)->rcx)
#define DL(r)  ((uint8_t)(r)->rdx)
#define DH(r)  ((uint8_t)((r)->rdx >> 8))
#define DX(r)  ((uint16_t)(r)->rdx)

static void set16(uint64_t* reg, uint16_t v) { *reg = (*reg & ~0xFFFFull) | v; }
static void set_lo(uint64_t* reg, uint8_t v) { *reg = (*reg & ~0xFFull) | v; }
static void set_hi(uint64_t* reg, uint8_t v) { *reg = (*reg & ~0xFF00ull) | ((uint64_t)v << 8); }

// Указатель хоста на память гостя по «сегмент:смещение» (база сегмента — из
// VMCS: в реальном режиме это селектор * 16, но гость мог оставить в регистре
// и «большой» сегмент после защищённого режима). 0 — за пределами памяти.
static uint8_t* gptr(struct bios* b, int seg_base_id, uint16_t off, uint32_t len) {
    uint64_t lin = hv_guest_get(b->vm, seg_base_id) + off;
    if (lin + len > b->ram_bytes) return 0;
    return b->ram + lin;
}

// Сохранённые FLAGS вызвавшего лежат в стеке гостя третьим словом (IP, CS, FLAGS).
static uint8_t* stacked_flags(struct bios* b) {
    return gptr(b, HV_G_SS_BASE, (uint16_t)(hv_guest_get(b->vm, HV_G_RSP) + 4), 2);
}

static void ret_cf(struct bios* b, bool cf) {
    uint8_t* f = stacked_flags(b);
    if (f) wr16(f, (uint16_t)((rd16(f) & ~1u) | (cf ? 1u : 0u)));
}

static void ret_zf(struct bios* b, bool zf) {
    uint8_t* f = stacked_flags(b);
    if (f) wr16(f, (uint16_t)((rd16(f) & ~0x40u) | (zf ? 0x40u : 0u)));
}

// Флаг переноса в ТЕКУЩИХ флагах гостя — им заглушка INT 16h узнаёт, что
// клавиши ещё нет (см. phantom_bios_rom.asm).
static void live_cf(struct bios* b, bool cf) {
    uint64_t f = hv_guest_get(b->vm, HV_G_RFLAGS);
    hv_guest_set(b->vm, HV_G_RFLAGS, (f & ~1ull) | (cf ? 1 : 0));
}

static void note_unknown(struct bios* b, int vec, uint8_t ah) {
    uint32_t key = ((uint32_t)vec << 8) | ah;
    for (int i = 0; i < b->st.unknown_n; i++) if (b->st.unknown[i] == key) return;
    if (b->st.unknown_n < 16) b->st.unknown[b->st.unknown_n++] = key;
}

// ----------------------------------------------------------------------------
// INT 10h — текстовый экран. Экран настоящий: это память гостя по адресу
// B8000 (символ + атрибут), та самая, в которую потом пишет ядро Linux. Пока у
// гостя нет окна, написанное дублируется потоком в консоль (serial).
// ----------------------------------------------------------------------------
static uint8_t* cell(struct bios* b, int row, int col) {
    return b->ram + TEXT_GPA + (uint32_t)(row * COLS + col) * 2;
}

// Экран — не поток: программа ставит символ в любое место. Поток делаем так:
// пока символы идут подряд в одной строке — пишем их; другая строка или шаг
// назад — перевод строки и отступ пробелами. Пробелы сами по себе не пишем
// (ими стирают экран), они появятся как отступ перед следующим символом.
static void stream(struct bios* b, int row, int col, uint8_t ch) {
    if (ch == ' ' || ch == 0) return;
    if (ch < 0x20 || ch > 0x7E) ch = '+';               // псевдографика рамок
    if (row != b->s_row || col < b->s_col) {
        pc_con_putc(b->pc, '\n');
        b->s_row = row;
        b->s_col = 0;
    }
    while (b->s_col < col) { pc_con_putc(b->pc, ' '); b->s_col++; }
    pc_con_putc(b->pc, (char)ch);
    b->s_col++;
    b->st.chars++;
}

static void put_cell(struct bios* b, int row, int col, uint8_t ch, int attr) {
    if (row < 0 || row >= ROWS || col < 0 || col >= COLS) return;
    uint8_t* c = cell(b, row, col);
    c[0] = ch;
    if (attr >= 0) c[1] = (uint8_t)attr;
    stream(b, row, col, ch);
}

// Сдвиг окна на n строк вверх (n > 0) или вниз (n < 0); n == 0 — очистить.
static void scroll(struct bios* b, int n, uint8_t attr, int r0, int c0, int r1, int c1) {
    if (r1 >= ROWS) r1 = ROWS - 1;
    if (c1 >= COLS) c1 = COLS - 1;
    if (r0 > r1 || c0 > c1) return;
    int h = r1 - r0 + 1;
    bool full = c0 == 0 && c1 == COLS - 1;
    if (n == 0 || n >= h || -n >= h) {
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) { uint8_t* p = cell(b, r, c); p[0] = ' '; p[1] = attr; }
        if (full) b->s_row = -1;                        // после очистки поток начинает с новой строки
        return;
    }
    if (n > 0) {
        for (int r = r0; r <= r1; r++)
            for (int c = c0; c <= c1; c++) {
                uint8_t* d = cell(b, r, c);
                if (r + n <= r1) { uint8_t* s = cell(b, r + n, c); d[0] = s[0]; d[1] = s[1]; }
                else { d[0] = ' '; d[1] = attr; }
            }
        if (full && b->s_row >= 0) b->s_row -= n;       // текст уехал вверх вместе со строками
    } else {
        n = -n;
        for (int r = r1; r >= r0; r--)
            for (int c = c0; c <= c1; c++) {
                uint8_t* d = cell(b, r, c);
                if (r - n >= r0) { uint8_t* s = cell(b, r - n, c); d[0] = s[0]; d[1] = s[1]; }
                else { d[0] = ' '; d[1] = attr; }
            }
        if (full) b->s_row = -1;
    }
}

// Вывод «как на телетайп»: символ в позицию курсора, курсор дальше, с
// переносом и прокруткой. Курсор хранится в области данных BIOS (0x450).
static void tty(struct bios* b, uint8_t ch) {
    int col = b->ram[0x450], row = b->ram[0x451];
    if (ch == 7) return;
    if (ch == 8) { if (col) col--; }
    else if (ch == '\r') col = 0;
    else if (ch == '\n') row++;
    else { put_cell(b, row, col, ch, -1); col++; }
    if (col >= COLS) { col = 0; row++; }
    if (row >= ROWS) { scroll(b, row - (ROWS - 1), 0x07, 0, 0, ROWS - 1, COLS - 1); row = ROWS - 1; }
    b->ram[0x450] = (uint8_t)col;
    b->ram[0x451] = (uint8_t)row;
}

static void int10(struct bios* b, vmx_regs_t* r) {
    uint8_t al = AL(r);
    switch (AH(r)) {
    case 0x00:                                          // установить режим: у нас один, 80x25
        if (!(al & 0x80)) scroll(b, 0, 0x07, 0, 0, ROWS - 1, COLS - 1);
        b->ram[0x449] = 3;
        b->ram[0x450] = 0; b->ram[0x451] = 0;
        break;
    case 0x01: case 0x05: case 0x0B: case 0x10:         // вид курсора, страница, палитра
        break;
    case 0x02:
        b->ram[0x450] = DL(r);
        b->ram[0x451] = DH(r);
        break;
    case 0x03:
        set16(&r->rdx, (uint16_t)(b->ram[0x450] | (b->ram[0x451] << 8)));
        set16(&r->rcx, 0x0607);
        break;
    case 0x06: scroll(b, al, BH(r), CH(r), CL(r), DH(r), DL(r)); break;
    case 0x07: scroll(b, al ? -(int)al : 0, BH(r), CH(r), CL(r), DH(r), DL(r)); break;
    case 0x08: {
        int col = b->ram[0x450], row = b->ram[0x451];
        uint8_t* c = (row < ROWS && col < COLS) ? cell(b, row, col) : 0;
        set16(&r->rax, c ? (uint16_t)(c[0] | (c[1] << 8)) : 0x0720);
        break;
    }
    case 0x09: case 0x0A: {                             // символ CX раз, курсор на месте
        int col = b->ram[0x450], row = b->ram[0x451];
        int n = CX(r);
        for (int i = 0; i < n && col + i < COLS; i++)
            put_cell(b, row, col + i, al, AH(r) == 0x09 ? BL(r) : -1);
        break;
    }
    case 0x0E: tty(b, al); break;
    case 0x0F:
        set16(&r->rax, (uint16_t)((COLS << 8) | 3));     // AH — столбцов, AL — режим
        set_hi(&r->rbx, 0);
        break;
    case 0x11:
        if (al == 0x30) { set16(&r->rcx, 16); set_lo(&r->rdx, ROWS - 1); }
        break;
    case 0x12:
        if (BL(r) == 0x10) { set16(&r->rbx, 0x0003); set16(&r->rcx, 0); }   // цветной, 256 КБ
        break;
    case 0x13: {                                        // строка ES:BP длиной CX в позицию DH:DL
        int n = CX(r);
        bool with_attr = al & 2;
        uint8_t* s = gptr(b, HV_G_ES_BASE, (uint16_t)r->rbp, (uint32_t)n * (with_attr ? 2 : 1));
        if (!s) break;
        uint8_t col0 = b->ram[0x450], row0 = b->ram[0x451];
        b->ram[0x450] = DL(r); b->ram[0x451] = DH(r);
        for (int i = 0; i < n; i++) tty(b, with_attr ? s[i * 2] : s[i]);
        if (!(al & 1)) { b->ram[0x450] = col0; b->ram[0x451] = row0; }
        break;
    }
    case 0x1A:
        if (al == 0) { set_lo(&r->rax, 0x1A); set16(&r->rbx, 0x0008); }     // адаптер VGA, цветной
        break;
    case 0x4F:
        set16(&r->rax, 0x0100);                         // VBE: такой функции нет
        break;
    default:
        note_unknown(b, 0x10, AH(r));
        break;
    }
    // [50.5] Курсор — в регистры видеоконтроллера: окно гостя берёт его оттуда
    // же, откуда потом будет брать у Linux (тот пишет регистры сам).
    if (b->ram[0x451] < ROWS && b->ram[0x450] < COLS)
        pc_vga_set_cursor(b->pc, (uint32_t)(b->ram[0x451] * COLS + b->ram[0x450]));
}

// ----------------------------------------------------------------------------
// INT 13h — единственный диск: привод CD, загрузка El Torito без эмуляции.
// ----------------------------------------------------------------------------
static void disk_ret(struct bios* b, vmx_regs_t* r, uint8_t status) {
    set_hi(&r->rax, status);
    ret_cf(b, status != 0);
}

static void int13(struct bios* b, vmx_regs_t* r) {
    uint8_t ah = AH(r);
    bool ours = DL(r) == CD_DRIVE;
    if (ah == 0x00) { disk_ret(b, r, 0); return; }      // сброс: успех для любого диска
    if (!ours) { disk_ret(b, r, 0x01); return; }        // других дисков нет

    switch (ah) {
    case 0x01: disk_ret(b, r, 0); break;
    case 0x41:                                          // есть ли расширения (чтение по LBA)
        if (BX(r) != 0x55AA) { disk_ret(b, r, 0x01); break; }
        set16(&r->rbx, 0xAA55);
        set16(&r->rcx, 0x0007);
        set_hi(&r->rax, 0x30);                          // версия 3.0
        ret_cf(b, false);
        break;
    case 0x42: {                                        // расширенное чтение, пакет в DS:SI
        uint8_t* dap = gptr(b, HV_G_DS_BASE, (uint16_t)r->rsi, 16);
        if (!dap) { disk_ret(b, r, 0x01); break; }
        uint32_t count = rd16(dap + 2);
        uint64_t dst = (uint64_t)rd16(dap + 6) * 16 + rd16(dap + 4);
        // Буфер FFFF:FFFF — «адрес лежит дальше 64-битным числом».
        if (rd16(dap + 4) == 0xFFFF && rd16(dap + 6) == 0xFFFF && dap[0] >= 0x18) dst = rd64(dap + 16);
        uint64_t lba = rd64(dap + 8), bytes = (uint64_t)count * CD_SECTOR;
        if (lba > b->iso_bytes / CD_SECTOR || lba * CD_SECTOR + bytes > b->iso_bytes ||
            dst + bytes > b->ram_bytes) {
            wr16(dap + 2, 0);                           // ничего не прочитано
            disk_ret(b, r, 0x04);                       // «сектор не найден»
            break;
        }
        const uint8_t* src = b->iso + lba * CD_SECTOR;
        uint8_t* d = b->ram + dst;
        for (uint64_t i = 0; i < bytes; i++) d[i] = src[i];
        b->st.disk_reads++;
        b->st.disk_sectors += count;
        disk_ret(b, r, 0);
        break;
    }
    case 0x48: {                                        // параметры диска, буфер в DS:SI
        uint8_t* p = gptr(b, HV_G_DS_BASE, (uint16_t)r->rsi, 0x1A);
        if (!p) { disk_ret(b, r, 0x01); break; }
        wr16(p, 0x1A);
        wr16(p + 2, 0x0004);                            // сменный носитель
        wr32(p + 4, 0xFFFFFFFFu);                       // геометрии «цилиндр-головка» у CD нет
        wr32(p + 8, 0xFFFFFFFFu);
        wr32(p + 12, 0xFFFFFFFFu);
        wr64(p + 16, b->iso_bytes / CD_SECTOR);
        wr16(p + 24, CD_SECTOR);
        disk_ret(b, r, 0);
        break;
    }
    case 0x4B: {                                        // состояние загрузки El Torito
        uint8_t* p = gptr(b, HV_G_DS_BASE, (uint16_t)r->rsi, 0x13);
        if (!p) { disk_ret(b, r, 0x01); break; }
        for (int i = 0; i < 0x13; i++) p[i] = 0;
        p[0] = 0x13;
        p[1] = 0;                                       // без эмуляции дискеты/диска
        p[2] = CD_DRIVE;
        wr32(p + 4, b->boot_lba);
        wr16(p + 0x0C, 0x07C0);
        wr16(p + 0x0E, b->boot_sectors);
        disk_ret(b, r, 0);
        break;
    }
    default:
        // Чтение по «цилиндр-головка-сектор» (02h), геометрия (08h), тип (15h)
        // — для CD без эмуляции их нет и на настоящих BIOS.
        if (ah != 0x02 && ah != 0x08 && ah != 0x15) note_unknown(b, 0x13, ah);
        disk_ret(b, r, 0x01);
        break;
    }
}

// ----------------------------------------------------------------------------
// INT 15h — карта памяти и линия A20.
// ----------------------------------------------------------------------------
static void int15(struct bios* b, vmx_regs_t* r) {
    uint8_t ah = AH(r), al = AL(r);
    uint64_t ext_kb = (b->ram_bytes - 0x100000) / 1024;
    switch (ah) {
    case 0xE8:
        if (al == 0x20 && (uint32_t)r->rdx == 0x534D4150u) {       // 'SMAP'
            uint32_t idx = (uint32_t)r->rbx;
            uint8_t* p = gptr(b, HV_G_ES_BASE, (uint16_t)r->rdi, 20);
            if (idx >= (uint32_t)b->map_n || !p) { ret_cf(b, true); break; }
            wr64(p, b->map[idx].addr);
            wr64(p + 8, b->map[idx].size);
            wr32(p + 16, b->map[idx].type);
            r->rax = 0x534D4150u;
            r->rcx = 20;
            r->rbx = (idx + 1 < (uint32_t)b->map_n) ? idx + 1 : 0;  // 0 — запись последняя
            ret_cf(b, false);
        } else if (al == 0x01) {
            // Память выше 1 МБ: до 16 МБ — килобайтами, дальше — блоками по 64 КБ.
            uint64_t low = ext_kb > 0x3C00 ? 0x3C00 : ext_kb;
            uint64_t high = b->ram_bytes > (16ull << 20) ? (b->ram_bytes - (16ull << 20)) / 65536 : 0;
            if (high > 0xFFFF) high = 0xFFFF;
            set16(&r->rax, (uint16_t)low);  set16(&r->rcx, (uint16_t)low);
            set16(&r->rbx, (uint16_t)high); set16(&r->rdx, (uint16_t)high);
            ret_cf(b, false);
        } else {
            set_hi(&r->rax, 0x86);
            ret_cf(b, true);
        }
        break;
    case 0x88:
        set16(&r->rax, (uint16_t)(ext_kb > 0xFC00 ? 0xFC00 : ext_kb));
        ret_cf(b, false);
        break;
    case 0x24:                                          // линия A20: включена всегда
        if (al == 0x02) set_lo(&r->rax, 1);
        if (al == 0x03) set16(&r->rbx, 3);
        set_hi(&r->rax, 0);
        ret_cf(b, false);
        break;
    case 0x86:                                          // «подождать CX:DX микросекунд» — не ждём
    case 0x90: case 0x91:                               // устройство занято/освободилось
    case 0xEC:                                          // «буду работать в длинном режиме»
        set_hi(&r->rax, 0);
        ret_cf(b, false);
        break;
    default:
        // C0h (таблица конфигурации), 53h (APM), E9h (SpeedStep), 4Fh — нет.
        if (ah != 0xC0 && ah != 0xC1 && ah != 0x53 && ah != 0xE9 && ah != 0x4F) note_unknown(b, 0x15, ah);
        set_hi(&r->rax, 0x86);                          // «функция не поддерживается»
        ret_cf(b, true);
        break;
    }
}

// ----------------------------------------------------------------------------
// INT 16h — клавиатура. Источник — кольцо ввода консоли гостя (то же, из
// которого позже читает последовательный порт): символы ASCII.
// ----------------------------------------------------------------------------
static uint16_t key_code(int ch) {
    static const char row1[] = "qwertyuiop", row2[] = "asdfghjkl", row3[] = "zxcvbnm";
    uint8_t scan = 0;
    int lc = (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
    if (ch == '\r' || ch == '\n') return 0x1C0D;
    if (ch == 8 || ch == 0x7F)    return 0x0E08;
    if (ch == 0x1B)               return 0x011B;
    if (ch == '\t')               return 0x0F09;
    if (ch == ' ')                scan = 0x39;
    else if (ch >= '1' && ch <= '9') scan = (uint8_t)(0x02 + ch - '1');
    else if (ch == '0')           scan = 0x0B;
    for (int i = 0; row1[i] && !scan; i++) if (row1[i] == lc) scan = (uint8_t)(0x10 + i);
    for (int i = 0; row2[i] && !scan; i++) if (row2[i] == lc) scan = (uint8_t)(0x1E + i);
    for (int i = 0; row3[i] && !scan; i++) if (row3[i] == lc) scan = (uint8_t)(0x2C + i);
    return (uint16_t)((scan << 8) | (uint8_t)ch);       // у знаков без кода AH = 0: читают AL
}

// ----------------------------------------------------------------------------
// [50.5] Настоящая клавиатура: IRQ1 -> INT 09h -> очередь клавиш BIOS.
// ----------------------------------------------------------------------------
// Пока гость в реальном режиме (загрузчик), клавиши окна приходят так же, как
// на настоящей машине: контроллер 8042 поднимает IRQ1, вектор 09h ведёт в
// нашу заглушку, а служба ниже делает работу обработчика BIOS — читает
// скан-код, ведёт состояние Shift/Ctrl/Alt/Caps в области данных (0x417) и
// кладёт слово «скан-код : символ» в очередь на 15 клавиш (0x41E..0x43D,
// указатели 0x41A и 0x41C). INT 16h берёт клавиши оттуда. Ядро Linux позже
// ставит на IRQ1 свой обработчик и читает порт 0x60 само — эта служба ему не
// нужна. Раскладка — американская, как у BIOS.
static const char k_plain[0x3A] = {
    0, 27, '1','2','3','4','5','6','7','8','9','0','-','=', 8, 9,
    'q','w','e','r','t','y','u','i','o','p','[',']', 13, 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0, '\\',
    'z','x','c','v','b','n','m',',','.','/', 0, '*', 0, ' ',
};
static const char k_shift[0x3A] = {
    0, 27, '!','@','#','$','%','^','&','*','(',')','_','+', 8, 0,
    'Q','W','E','R','T','Y','U','I','O','P','{','}', 13, 0,
    'A','S','D','F','G','H','J','K','L',':','"','~', 0, '|',
    'Z','X','C','V','B','N','M','<','>','?', 0, '*', 0, ' ',
};

// Указатели очереди гость может испортить — тогда считаем её пустой.
static bool kbuf_ptrs(struct bios* b, uint16_t* head, uint16_t* tail) {
    *head = rd16(b->ram + 0x41A);
    *tail = rd16(b->ram + 0x41C);
    bool ok = *head >= 0x1E && *head < 0x3E && *tail >= 0x1E && *tail < 0x3E && !((*head | *tail) & 1);
    if (!ok) { *head = *tail = 0x1E; wr16(b->ram + 0x41A, 0x1E); wr16(b->ram + 0x41C, 0x1E); }
    return ok;
}

static bool kbuf_push(struct bios* b, uint16_t key) {
    uint16_t head, tail;
    kbuf_ptrs(b, &head, &tail);
    uint16_t next = (uint16_t)(tail + 2 >= 0x3E ? 0x1E : tail + 2);
    if (next == head) return false;                     // очередь полна: клавиша пропадает, как в BIOS
    wr16(b->ram + 0x400 + tail, key);
    wr16(b->ram + 0x41C, next);
    return true;
}

static int kbuf_peek(struct bios* b) {
    uint16_t head, tail;
    kbuf_ptrs(b, &head, &tail);
    return head == tail ? -1 : rd16(b->ram + 0x400 + head);
}

static void kbuf_pop(struct bios* b) {
    uint16_t head, tail;
    kbuf_ptrs(b, &head, &tail);
    if (head != tail) wr16(b->ram + 0x41A, (uint16_t)(head + 2 >= 0x3E ? 0x1E : head + 2));
}

static int int09(struct bios* b) {
    int sc = pc_kbd_take(b->pc);
    if (pc_irq_in_service(b->pc, 1)) pc_irq_eoi(b->pc, 1);
    if (sc < 0) return HV_RESUME;
    uint8_t* fl = b->ram + 0x417;                       // бит 0/1 — Shift, 2 — Ctrl, 3 — Alt, 6 — Caps Lock
    if (sc == 0xE0) { b->ram[0x496] |= 0x02; return HV_RESUME; }     // префикс «серой» клавиши
    bool e0 = b->ram[0x496] & 0x02;
    b->ram[0x496] &= (uint8_t)~0x02;
    bool up = sc & 0x80;
    uint8_t code = (uint8_t)(sc & 0x7F);

    // E0 2A / E0 36 — «ненастоящий» Shift, которым клавиатура обрамляет стрелки
    // и PrtSc: к состоянию Shift он отношения не имеет.
    if (e0 && (code == 0x2A || code == 0x36)) return HV_RESUME;
    uint8_t mod = code == 0x2A ? 0x02 : code == 0x36 ? 0x01 : code == 0x1D ? 0x04 : code == 0x38 ? 0x08 : 0;
    if (mod) {
        if (up) *fl &= (uint8_t)~mod; else *fl |= mod;
        return HV_RESUME;
    }
    if (up) return HV_RESUME;
    if (code == 0x3A) { *fl ^= 0x40; return HV_RESUME; }             // Caps Lock
    bool shift = *fl & 0x03, ctrl = *fl & 0x04, alt = *fl & 0x08;
    // Ctrl+Alt+Del в BIOS — перезагрузка. У нас она означает конец гостя.
    if (ctrl && alt && code == 0x53) { b->st.end = BIOS_END_REBOOT; return HV_STOP; }

    uint16_t key;
    if (e0) {
        // Серые клавиши: стрелки, Home/End/PgUp/PgDn/Ins/Del, Enter и «/»
        // цифрового блока. В младшем байте E0 — так их отличает INT 16h/10h.
        if (code == 0x1C) key = 0xE00D;
        else if (code == 0x35) key = 0xE02F;
        else key = (uint16_t)((code << 8) | 0xE0);
    } else if (code < 0x3A) {
        char ch = shift ? k_shift[code] : k_plain[code];
        bool letter = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (letter && (*fl & 0x40)) ch ^= 0x20;                      // Caps Lock меняет регистр букв
        if (ctrl) {
            if (letter) ch = (char)((ch | 0x20) - 'a' + 1);          // Ctrl+A = 1 … Ctrl+Z = 26
            else if (ch == '[') ch = 27;
            else if (ch == 13) ch = 10;
            else if (ch != 27 && ch != 8 && ch != 9 && ch != ' ') ch = 0;
        }
        if (alt) ch = 0;                                             // Alt+клавиша: только скан-код
        if (!ch && !alt && !ctrl && code != 0x0F) return HV_RESUME;  // у кода нет символа (кроме Shift+Tab)
        key = (uint16_t)((code << 8) | (uint8_t)ch);
    } else if (code >= 0x3B && code <= 0x44) key = (uint16_t)(code << 8);        // F1..F10
    else if (code == 0x57 || code == 0x58) key = (uint16_t)((code == 0x57 ? 0x85 : 0x86) << 8);
    else if (code >= 0x47 && code <= 0x53) key = (uint16_t)(code << 8);          // цифровой блок как стрелки
    else return HV_RESUME;
    (void)kbuf_push(b, key);
    return HV_RESUME;
}

static void int16(struct bios* b, vmx_regs_t* r) {
    live_cf(b, false);
    uint8_t ah = AH(r);
    switch (ah) {
    case 0x00: case 0x10: {                             // дождаться клавиши
        // Два источника: очередь BIOS (клавиатура окна) и кольцо консоли
        // (`phantom type`, строка для загрузчика).
        int key = kbuf_peek(b);
        if (key >= 0) kbuf_pop(b);
        else {
            int ch = pc_con_getc(b->pc);
            if (ch < 0) { live_cf(b, true); return; }   // заглушка уснёт и спросит снова
            key = key_code(ch);
        }
        // Старая служба (00h) о серых клавишах не знает: им положен символ 0.
        if (ah == 0x00 && (key & 0xFF) == 0xE0 && (key >> 8)) key &= 0xFF00;
        set16(&r->rax, (uint16_t)key);
        b->st.keys++;
        break;
    }
    case 0x01: case 0x11: {                             // есть ли клавиша (ZF=1 — нет)
        int key = kbuf_peek(b);
        if (key < 0) { int ch = pc_con_peek(b->pc); if (ch >= 0) key = key_code(ch); }
        if (key >= 0) {
            if (ah == 0x01 && (key & 0xFF) == 0xE0 && (key >> 8)) key &= 0xFF00;
            set16(&r->rax, (uint16_t)key);
        }
        ret_zf(b, key < 0);
        break;
    }
    case 0x02: set_lo(&r->rax, b->ram[0x417]); break;   // состояние Shift/Ctrl/Alt/Caps
    case 0x12: set16(&r->rax, b->ram[0x417]); break;
    case 0x05:                                          // положить клавишу CX в очередь
        set_lo(&r->rax, kbuf_push(b, CX(r)) ? 0 : 1);
        break;
    default: break;                                     // скорость автоповтора и т.п.
    }
}

// ----------------------------------------------------------------------------
// INT 1Ah — счётчик тиков и календарные часы.
// ----------------------------------------------------------------------------
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void int1a(struct bios* b, vmx_regs_t* r) {
    uint64_t t = pc_unix_now(b->pc);
    switch (AH(r)) {
    case 0x00: {
        uint32_t ticks = rd32(b->ram + 0x46C);          // его ведёт INT 08h в госте
        set16(&r->rcx, (uint16_t)(ticks >> 16));
        set16(&r->rdx, (uint16_t)ticks);
        set_lo(&r->rax, 0);
        ret_cf(b, false);
        break;
    }
    case 0x02:
        set16(&r->rcx, (uint16_t)((bcd((int)(t / 3600 % 24)) << 8) | bcd((int)(t / 60 % 60))));
        set16(&r->rdx, (uint16_t)(bcd((int)(t % 60)) << 8));
        ret_cf(b, false);
        break;
    case 0x04: {
        // Дни от 1970-01-01 -> год, месяц, день.
        int64_t z = (int64_t)(t / 86400) + 719468;
        int era = (int)(z / 146097);
        int doe = (int)(z - (int64_t)era * 146097);
        int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int mp = (5 * doy + 2) / 153;
        int d = doy - (153 * mp + 2) / 5 + 1;
        int m = mp + (mp < 10 ? 3 : -9);
        int y = yoe + era * 400 + (m <= 2);
        set16(&r->rcx, (uint16_t)((bcd(y / 100) << 8) | bcd(y % 100)));
        set16(&r->rdx, (uint16_t)((bcd(m) << 8) | bcd(d)));
        ret_cf(b, false);
        break;
    }
    default:
        ret_cf(b, true);                                // в том числе B1h — PCI BIOS: шины нет
        break;
    }
}

// ----------------------------------------------------------------------------
// Разбор VMCALL.
// ----------------------------------------------------------------------------
static int bios_vmcall(hv_vm_t* vm, void* ctx) {
    struct bios* b = (struct bios*)ctx;
    vmx_regs_t* r = hv_regs(vm);

    if (hv_guest_get(vm, HV_G_CS_BASE) != ROM_GPA) {
        // VMCALL не из нашего ПЗУ — гость сам зовёт гипервизор (ядро Linux
        // считает нас KVM). Услуг у нас нет: отвечаем, как KVM, «нет такой».
        r->rax = (uint64_t)-1000;
        return HV_RESUME;
    }
    uint32_t ip = (uint32_t)hv_guest_get(vm, HV_G_RIP) & 0xFFFF;    // уже за VMCALL (3 байта)
    int vec = -1;
    if (ip == b->int16_call + 3) vec = 0x16;
    else if (ip >= 3 && ip < 0x400 + 3 && !((ip - 3) & 3)) vec = (int)((ip - 3) / 4);
    b->st.calls++;

    switch (vec) {
    case 0x09: return int09(b);                         // [50.5] IRQ1: клавиша
    case 0x10: int10(b, r); break;
    case 0x11: set16(&r->rax, 0x0222); break;           // сопроцессор, цветной экран, один COM-порт
    case 0x12: set16(&r->rax, EBDA_GPA / 1024); break;  // обычной памяти, КБ
    case 0x13: int13(b, r); break;
    case 0x14: set_hi(&r->rax, 0x80); break;            // последовательный порт BIOS: тайм-аут
    case 0x15: int15(b, r); break;
    case 0x16: int16(b, r); break;
    case 0x17: set_hi(&r->rax, 0x01); break;            // принтера нет
    case 0x18: b->st.end = BIOS_END_BOOT_FAIL; return HV_STOP;
    case 0x19: b->st.end = BIOS_END_REBOOT; return HV_STOP;
    case 0x1A: int1a(b, r); break;
    case 0x1C: case 0x05: case 0x01: case 0x03: case 0x04:
        break;                                          // крючок таймера по умолчанию, PrtSc, отладка
    default:
        // Аппаратное прерывание без обработчика гостя: просто закрываем его.
        if (vec >= 0x08 && vec <= 0x0F && pc_irq_in_service(b->pc, vec - 8)) { pc_irq_eoi(b->pc, vec - 8); break; }
        if (vec >= 0x70 && vec <= 0x77 && pc_irq_in_service(b->pc, vec - 0x70 + 8)) { pc_irq_eoi(b->pc, vec - 0x70 + 8); break; }
        // Векторы 09h..0Fh — это ещё и линии IRQ1..7. Загрузчик, повесивший на
        // них свой обработчик, может сам закрыть прерывание и передать его
        // дальше нам — тогда «в обслуживании» уже ничего нет, и это не сбой.
        // Сбоем считаем только вектор 0Dh (нарушение защиты): после него
        // IRET вернул бы на ту же команду, и гость зациклился бы.
        if (vec >= 0x09 && vec <= 0x0F && vec != 0x0D) break;
        if (vec < 0 || vec < 0x10) {
            // Исключение процессора в реальном режиме (деление на ноль,
            // неверная команда, нарушение защиты): обработчика у гостя нет,
            // продолжать нечем. Адрес команды — в его стеке.
            uint8_t* st = gptr(b, HV_G_SS_BASE, (uint16_t)hv_guest_get(vm, HV_G_RSP), 4);
            b->st.end = BIOS_END_EXCEPTION;
            b->st.end_vec = (uint8_t)(vec < 0 ? 0xFF : vec);
            b->st.end_ip = st ? rd16(st) : 0;
            b->st.end_cs = st ? rd16(st + 2) : 0;
            return HV_STOP;
        }
        note_unknown(b, vec, AH(r));
        break;
    }
    return HV_RESUME;
}

// ----------------------------------------------------------------------------
// BSP.
// ----------------------------------------------------------------------------
bios_t* bios_create(hv_vm_t* vm, pc_t* pc, const uint8_t* iso, uint64_t iso_bytes) {
    if (g_bios_used) return 0;
    struct bios* b = &g_bios;
    uint8_t* z = (uint8_t*)b;
    for (size_t i = 0; i < sizeof(*b); i++) z[i] = 0;
    b->vm = vm;
    b->pc = pc;
    b->iso = iso;
    b->iso_bytes = iso_bytes;
    b->ram_bytes = hv_ram_size(vm);
    b->ram = (uint8_t*)hv_gpa(vm, 0, b->ram_bytes);
    b->s_row = -1;
    b->int16_call = (uint32_t)(phantom_bios_int16_call - phantom_bios_start);
    // Карта памяти: обычная память; расширенная область данных BIOS; ПЗУ;
    // всё выше 1 МБ. Промежуток A0000–EFFFF (видеопамять) в карту не входит.
    b->map[0] = (e820_t){ 0, EBDA_GPA, 1 };
    b->map[1] = (e820_t){ EBDA_GPA, 0xA0000 - EBDA_GPA, 2 };
    b->map[2] = (e820_t){ ROM_GPA, 0x100000 - ROM_GPA, 2 };
    b->map[3] = (e820_t){ 0x100000, b->ram_bytes - 0x100000, 1 };
    b->map_n = 4;
    g_bios_used = true;
    return b;
}

void bios_destroy(bios_t* b) {
    if (b == &g_bios) g_bios_used = false;
}

const bios_stats_t* bios_stats(bios_t* b) { return &b->st; }

bool bios_boot_cd(bios_t* b, hv_start_t* st, bios_boot_t* info, const char** err) {
    const uint8_t* iso = b->iso;
    *err = "";
    if (!b->ram || b->ram_bytes < (2ull << 20)) { *err = "guest RAM is too small for a BIOS"; return false; }
    if (b->iso_bytes < 20 * CD_SECTOR) { *err = "file is too small to be an ISO image"; return false; }

    // Том ISO 9660: описатели с сектора 16. Сектор 17 — загрузочная запись
    // El Torito с адресом загрузочного каталога.
    const uint8_t* pvd = iso + 16 * CD_SECTOR;
    const uint8_t* brd = iso + 17 * CD_SECTOR;
    static const char cd001[] = "CD001", tor[] = "EL TORITO SPECIFICATION";
    bool is_iso = true, is_tor = brd[0] == 0;
    for (int i = 0; i < 5; i++) { if (pvd[1 + i] != (uint8_t)cd001[i]) is_iso = false; if (brd[1 + i] != (uint8_t)cd001[i]) is_tor = false; }
    for (int i = 0; i < 23; i++) if (brd[7 + i] != (uint8_t)tor[i]) is_tor = false;
    if (!is_iso) { *err = "not an ISO 9660 image (no CD001 signature)"; return false; }
    if (!is_tor) { *err = "the ISO is not bootable (no El Torito boot record)"; return false; }

    uint64_t cat_off = (uint64_t)rd32(brd + 0x47) * CD_SECTOR;
    if (cat_off + CD_SECTOR > b->iso_bytes) { *err = "El Torito boot catalog is outside the image"; return false; }
    const uint8_t* cat = iso + cat_off;
    // Проверочная запись (платформа 0 = x86, подпись 55 AA) и запись по умолчанию.
    if (cat[0] != 1 || cat[0x1E] != 0x55 || cat[0x1F] != 0xAA) { *err = "El Torito boot catalog is damaged"; return false; }
    if (cat[1] != 0) { *err = "the ISO boots only on a non-BIOS platform (UEFI)"; return false; }
    const uint8_t* e = cat + 32;
    if (e[0] != 0x88) { *err = "El Torito default entry is not bootable"; return false; }
    if (e[1] & 0x0F) { *err = "El Torito floppy/hard-disk emulation is not supported"; return false; }
    uint16_t seg = rd16(e + 2) ? rd16(e + 2) : 0x07C0;
    uint16_t sectors = rd16(e + 6) ? rd16(e + 6) : 1;
    uint32_t lba = rd32(e + 8);
    uint64_t bytes = (uint64_t)sectors * 512;
    if ((uint64_t)lba * CD_SECTOR + bytes > b->iso_bytes || (uint32_t)seg * 16 + bytes > EBDA_GPA) {
        *err = "El Torito boot image does not fit";
        return false;
    }
    b->boot_lba = lba;
    b->boot_sectors = sectors;

    // ---- то, что настоящий BIOS делает при включении ----
    uint8_t* ram = b->ram;
    uint32_t rom_len = (uint32_t)(phantom_bios_end - phantom_bios_start);
    for (uint32_t i = 0; i < rom_len; i++) ram[ROM_GPA + i] = phantom_bios_start[i];
    for (int n = 0; n < 256; n++) {                     // таблица векторов: F000:(n*4)
        wr16(ram + n * 4, (uint16_t)(n * 4));
        wr16(ram + n * 4 + 2, 0xF000);
    }
    wr16(ram + 0x08 * 4, (uint16_t)(phantom_bios_int08 - phantom_bios_start));
    wr16(ram + 0x16 * 4, (uint16_t)(phantom_bios_int16 - phantom_bios_start));

    for (int i = 0x400; i < 0x500; i++) ram[i] = 0;     // область данных BIOS
    wr16(ram + 0x40E, EBDA_GPA >> 4);
    wr16(ram + 0x400, 0x3F8);                           // COM1: загрузчик с SERIAL ищет порт здесь
    wr16(ram + 0x410, 0x0222);                          // сопроцессор, цветной экран, один COM-порт
    wr16(ram + 0x413, EBDA_GPA / 1024);
    wr16(ram + 0x41A, 0x1E); wr16(ram + 0x41C, 0x1E);   // очередь клавиатуры пуста
    wr16(ram + 0x480, 0x1E); wr16(ram + 0x482, 0x3E);
    ram[0x449] = 3;                                     // режим 3: текст 80x25
    wr16(ram + 0x44A, COLS);
    wr16(ram + 0x44C, COLS * ROWS * 2);
    wr16(ram + 0x460, 0x0607);
    wr16(ram + 0x463, 0x3D4);
    ram[0x484] = ROWS - 1;
    wr16(ram + 0x485, 16);
    ram[0x496] = 0x10;
    wr16(ram + EBDA_GPA, 1);                            // расширенная область: 1 КБ
    for (int i = 0; i < COLS * ROWS; i++) { ram[TEXT_GPA + i * 2] = ' '; ram[TEXT_GPA + i * 2 + 1] = 0x07; }

    const uint8_t* img = iso + (uint64_t)lba * CD_SECTOR;
    for (uint64_t i = 0; i < bytes; i++) ram[(uint32_t)seg * 16 + i] = img[i];

    pc_bios_defaults(b->pc);
    pc_set_vmcall(b->pc, bios_vmcall, b);

    for (size_t i = 0; i < sizeof(*st); i++) ((uint8_t*)st)[i] = 0;
    st->mode = HV_START_REAL;
    // Обычай BIOS: образ в сегменте 07C0 запускается как 0000:7C00.
    if (seg == 0x07C0) { st->cs = 0; st->ip = 0x7C00; }
    else               { st->cs = seg; st->ip = 0; }
    st->sp = 0x7C00;
    st->edx = CD_DRIVE;                                 // DL — с какого диска загрузились
    st->intr_on = true;

    for (int i = 0; i < 32; i++) info->volume[i] = (char)pvd[40 + i];
    info->volume[32] = 0;
    for (int i = 31; i >= 0 && info->volume[i] == ' '; i--) info->volume[i] = 0;
    info->boot_lba = lba;
    info->boot_sectors = sectors;
    info->load_seg = seg;
    return true;
}

int bios_screen_row(hv_vm_t* vm, uint32_t origin, int row, char* out) {
    // Видеопамять текста — окно 32 КБ; начало экрана может стоять где угодно
    // внутри него, а строки — заворачиваться через край.
    const uint8_t* t = (const uint8_t*)hv_gpa(vm, TEXT_GPA, 0x8000);
    int n = 0;
    out[0] = 0;
    if (!t || row < 0 || row >= ROWS) return 0;
    for (int c = 0; c < COLS; c++) {
        uint8_t ch = t[((origin + (uint32_t)(row * COLS + c)) * 2) & 0x7FFF];
        out[c] = (ch >= 0x20 && ch <= 0x7E) ? (char)ch : (ch ? '+' : ' ');
        if (out[c] != ' ') n = c + 1;
    }
    out[n] = 0;
    return n;
}
